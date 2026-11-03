// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The UI's satellite services over core/sat (ui_svc.h, CONFIG_SAT): what the satellite service's task has worked out
// so far, read without waiting (core/sat/sat_svc.h), and the clock. Nothing here runs the orbit model: the UI's task
// has no stack for it
#include "config.h"
#if CONFIG_SAT
#include <string.h>
#include "ui_svc.h"
#include "hal_time.h"
#include "sat/sat_clock.h"
#include "sat/sat_store.h"
#include "sat/sat_svc.h"

bool ui_svc_sat_status(ui_sat_status_t *out)
{
	double now;
	memset(out, 0, sizeof *out);
	out->state = sat_svc_state();
	out->count = sat_svc_count();
	out->store = sat_store_slots() != 0;
	out->utc = sat_clock_now(&sat_utc, hal_millis(), &now) ? (uint32_t)now : 0u;
	out->utc_offset_min = (int16_t)ui_svc_setting_get(UI_SET_UTC_OFFSET);
	out->min_el = (uint8_t)ui_svc_setting_get(UI_SET_SAT_MIN_EL);
	return true;
}

bool ui_svc_sat_get(uint8_t i, ui_sat_t *out)
{
	sat_row_t r;
	if (!sat_svc_row(i, &r))
		return false;
	memcpy(out->name, r.name, sizeof out->name);
	out->name[sizeof out->name - 1] = '\0';
	out->norad = r.norad;
	out->state = r.state;	// the same values: UI_SAT_* follows sat_row_state_t
	out->max_el = r.max_el;
	out->el_now = r.el_now;
	out->age_d = r.age_d;
	out->dir = r.dir;
	out->aos = r.aos;
	out->los = r.los;
	return true;
}

void ui_svc_sat_refresh(void)
{
	sat_svc_refresh();
}

void ui_svc_sat_open(uint32_t norad)
{
	sat_svc_open(norad);
}

bool ui_svc_sat_profile(ui_sat_profile_t *out)
{
	sat_profile_t p;
	if (!sat_svc_profile(&p))
		return false;
	out->up_hz = p.up_hz;
	out->down_hz = p.down_hz;
	out->up_tone = p.up_tone;
	out->arm_tone = p.arm_tone;
	out->age_d = INT16_MIN;
	sat_row_t r;
	for (uint8_t i = 0; sat_svc_row(i, &r); i++) {
		if (r.norad == p.norad && r.state != SAT_ROW_NO_ORBIT) {
			out->age_d = r.age_d;
			break;
		}
	}
	return true;
}

uint8_t ui_svc_sat_pass_count(bool *done)
{
	return sat_svc_pass_count(done);
}

bool ui_svc_sat_pass(uint8_t i, ui_sat_pass_t *out)
{
	sat_pass_row_t p;
	if (!sat_svc_pass(i, &p))
		return false;
	out->aos = p.aos;
	out->los = p.los;
	out->max_el = p.max_el;
	out->aos_az = p.aos_az;
	out->los_az = p.los_az;
	out->dir = p.dir;
	return true;
}

// UI_SAT_* and sat_row_state_t are the same values (two enums: compared as numbers)
#define SAME(a, b)	((int)(a) == (int)(b))
_Static_assert(SAME(UI_SAT_NO_ORBIT, SAT_ROW_NO_ORBIT) && SAME(UI_SAT_WORKING, SAT_ROW_WORKING) &&
	       SAME(UI_SAT_NEXT, SAT_ROW_NEXT) && SAME(UI_SAT_UP, SAT_ROW_UP) && SAME(UI_SAT_NONE, SAT_ROW_NONE) &&
	       SAME(UI_SAT_OLD, SAT_ROW_OLD) && SAME(UI_SAT_BAD, SAT_ROW_BAD), "ui_svc.h's satellite states follow sat_svc.h's");
_Static_assert(SAME(UI_SAT_NO_TIME, SAT_SVC_NO_TIME) && SAME(UI_SAT_NO_POSITION, SAT_SVC_NO_POSITION) &&
	       SAME(UI_SAT_READY, SAT_SVC_READY), "ui_svc.h's satellite service states follow sat_svc.h's");
#endif
