// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The satellite service's task (sat_task.h). The orbit model needs about 0.75 KB of stack, more than the UI's task has
// to spare, so the service runs here, at the idle task's priority: it works only when nothing else wants the CPU, and
// the idle task (the watchdog's feed) still runs between its slices. The service itself (core/sat/sat_svc.c) has no
// RTOS calls; this file only gathers its inputs, polls it and sleeps
#include "config.h"
#if CONFIG_SAT
#include "app/sat_task.h"
#include "hal_rtc.h"
#include "hal_time.h"
#include "sat/sat_clock.h"
#include "sat/sat_svc.h"
#include "settings/settings.h"
#if CONFIG_APRS
#include "aprs/aprs.h"
#include "aprs/aprs_fmt.h"
#endif
#include "FreeRTOS.h"
#include "task.h"

// words: sat_svc_poll down through a pass search and the model to the soft-float helpers, with a stored record read
// on the way (make stack-check)
#define SAT_TASK_STACK		384
#define SAT_BUDGET		30	// runs of the model a slice, about 30-100 ms without an FPU
#define SAT_BUSY_MS		20	// between slices while work remains
#define SAT_IDLE_MS		1000	// between polls when all is up to date: passes start and end, up satellites move

// --- the battery-backed clock, where the board has one (hal_rtc.h) ---
// Weak: a board without one leaves them out, and nothing is read or written
__attribute__((weak)) bool hal_rtc_read(uint16_t *day, uint8_t *h, uint8_t *m, uint8_t *s)
{
	(void)day; (void)h; (void)m; (void)s;
	return false;
}

__attribute__((weak)) bool hal_rtc_write(uint16_t day, uint8_t h, uint8_t m, uint8_t s)
{
	(void)day; (void)h; (void)m; (void)s;
	return false;
}

#define RTC_SYNC_MS	600000u	// how often the radio's time is compared with the RTC's
#define RTC_TOLERANCE_S	2	// a difference this large (GPS corrected the radio's clock) is written over

static volatile bool rtc_dirty;	// the user set the clock: write it at once
static uint32_t rtc_sync_ms;

void sat_task_rtc_write(void)
{
	rtc_dirty = true;
}

// The radio's time into the RTC
static bool rtc_store(void)
{
	double now;
	if (!sat_clock_now(&sat_utc, hal_millis(), &now))
		return false;
	uint16_t day;
	uint8_t h, m, s;
	sat_clock_to_rtc((uint32_t)now, &day, &h, &m, &s);
	return hal_rtc_write(day, h, m, s);
}

// The RTC's time, as UTC; false when it has none of ours
static bool rtc_load(uint32_t *unix_s)
{
	uint16_t day;
	uint8_t h, m, s;
	return hal_rtc_read(&day, &h, &m, &s) && sat_clock_from_rtc(day, h, m, s, unix_s);
}

// At start the radio's clock is the RTC's (a GPS fix sets it again within a second of locking). Then: a clock set by
// hand goes into the RTC at once, and now and then the two are compared, so a GPS-corrected time reaches it too
static void rtc_tick(bool first)
{
	uint32_t t;
	double now;
	if (first) {
		if (rtc_load(&t) && !sat_clock_now(&sat_utc, hal_millis(), &now))
			sat_clock_set_unix(&sat_utc, t, hal_millis());
		rtc_sync_ms = hal_millis();
		return;
	}
	if (rtc_dirty) {
		rtc_dirty = false;
		rtc_store();
		rtc_sync_ms = hal_millis();
	} else if (hal_millis() - rtc_sync_ms >= RTC_SYNC_MS) {
		rtc_sync_ms = hal_millis();
		if (sat_clock_now(&sat_utc, hal_millis(), &now) &&
		    (!rtc_load(&t) || (double)t - now > RTC_TOLERANCE_S || now - (double)t > RTC_TOLERANCE_S))
			rtc_store();
	}
}

static void inputs(sat_svc_in_t *in)
{
	in->time_ok = sat_clock_now(&sat_utc, hal_millis(), &in->now);
	in->pos_ok = false;
#if CONFIG_APRS
	int32_t lat, lon;
	uint32_t err_m;
	bool dmr_gps;
	if (aprs_position_now(&lat, &lon, &err_m, &dmr_gps)) {
		in->pos_ok = true;
		in->lat = lat / 1e7;
		in->lon = lon / 1e7;
		aprs_gps_t g;
		in->alt_m = aprs_gps(&g, hal_millis()) && g.fix && g.alt_m != APRS_ALT_NONE ? (double)g.alt_m : 0.0;
	}
#endif
	// Read in place: a copy of the settings would cost this task 0.4 KB of stack
	const settings_t *st = settings_begin();
	in->min_el = st->sat_min_el == 0 ? SAT_SVC_MIN_EL_DEFAULT : (uint8_t)(st->sat_min_el - 1u);
	settings_end(hal_millis());
}

static void sat_task(void *arg)
{
	(void)arg;
	sat_svc_in_t in;
	rtc_tick(true);
	for (;;) {
		rtc_tick(false);
		inputs(&in);
		bool more = sat_svc_poll(&in, SAT_BUDGET);
		vTaskDelay(pdMS_TO_TICKS(more ? SAT_BUSY_MS : SAT_IDLE_MS));
	}
}

// The heap the other tasks may still need: never take the satellite task's stack from it (tests/hil/test_task_stacks.py
// asks for 2 KB spare). Without that room the radio runs on and Tools > Satellites says so, rather than the heap
// running out later, which the crash log turns into a restart
#define SAT_HEAP_RESERVE	2048u

void sat_task_start(void)
{
	sat_svc_init();
	size_t need = SAT_TASK_STACK * sizeof(StackType_t) + sizeof(StaticTask_t) + SAT_HEAP_RESERVE;
	if (xPortGetFreeHeapSize() < need ||
	    xTaskCreate(sat_task, "sat", SAT_TASK_STACK, NULL, tskIDLE_PRIORITY, NULL) != pdPASS)
		sat_svc_no_memory();
}
#endif
