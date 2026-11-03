// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The satellite service's task (sat_task.h). The orbit model needs about 0.75 KB of stack, more than the UI's task has
// to spare, so the service runs here, at the idle task's priority: it works only when nothing else wants the CPU, and
// the idle task (the watchdog's feed) still runs between its slices. The service itself (core/sat/sat_svc.c) has no
// RTOS calls; this file only gathers its inputs, polls it and sleeps
#include "config.h"
#if CONFIG_SAT
#include "app/sat_task.h"
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
	for (;;) {
		inputs(&in);
		bool more = sat_svc_poll(&in, SAT_BUDGET);
		vTaskDelay(pdMS_TO_TICKS(more ? SAT_BUSY_MS : SAT_IDLE_MS));
	}
}

void sat_task_start(void)
{
	sat_svc_init();
	xTaskCreate(sat_task, "sat", SAT_TASK_STACK, NULL, tskIDLE_PRIORITY, NULL);
}
#endif
