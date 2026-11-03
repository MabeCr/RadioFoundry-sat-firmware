// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Satellite profiles and repeater arming (sat_profile.h)
#include <stddef.h>
#include "sat/sat_profile.h"

// FM satellites as AMSAT lists them (amsat.org, FM satellite frequencies), November 2026. Satellites fail and new ones
// launch: these are a start, and the user's own list replaces them
const sat_profile_t sat_profiles_builtin[] = {
	// The ISS's cross-band repeater
	{ .name = "ISS", .norad = 25544, .up_hz = 145990000u, .down_hz = 437800000u, .up_tone = 670 },
	// SO-50 sleeps until a 2 s carrier with 74.4 Hz wakes it for 10 minutes; contacts then use 67.0 Hz
	{ .name = "SO-50", .norad = 27607, .up_hz = 145850000u, .down_hz = 436795000u, .up_tone = 670,
	  .arm_tone = 744, .arm_ms = 2000, .armed_s = 600 },
	// AO-91 runs on its solar panels: in sunlight only
	{ .name = "AO-91", .norad = 43017, .up_hz = 435250000u, .down_hz = 145960000u, .up_tone = 670 },
};
const uint8_t sat_profiles_builtin_count = sizeof sat_profiles_builtin / sizeof sat_profiles_builtin[0];

const sat_profile_t *sat_profile_find(uint32_t norad)
{
	for (uint8_t i = 0; i < sat_profiles_builtin_count; i++)
		if (sat_profiles_builtin[i].norad == norad)
			return &sat_profiles_builtin[i];
	return NULL;
}

// The millisecond clock wraps every 49 days: compare by difference
static bool reached(uint32_t now, uint32_t until)
{
	return (int32_t)(now - until) >= 0;
}

void sat_arm_reset(sat_arm_t *a, const sat_profile_t *p)
{
	a->state = SAT_ARM_IDLE;
	a->expired = false;
	a->arm_tone = p->arm_tone;
	a->up_tone = p->up_tone;
	a->armed_s = p->armed_s;
	a->arm_ms = p->arm_ms;
	a->until = 0;
}

bool sat_arm_start(sat_arm_t *a, uint32_t now_ms)
{
	if (a->arm_tone == 0 || a->state == SAT_ARM_SENDING)
		return false;
	a->state = SAT_ARM_SENDING;
	a->expired = false;
	a->until = now_ms + a->arm_ms;
	return true;
}

void sat_arm_abort(sat_arm_t *a)
{
	if (a->state == SAT_ARM_SENDING)
		a->state = SAT_ARM_IDLE;
}

sat_arm_out_t sat_arm_poll(sat_arm_t *a, uint32_t now_ms)
{
	sat_arm_out_t out = { .tone = a->up_tone };
	if (a->state == SAT_ARM_SENDING) {
		if (!reached(now_ms, a->until)) {
			out.key = true;
			out.tone = a->arm_tone;
			return out;
		}
		// The awake time counts from the carrier's end, when the repeater has heard all of it
		a->state = SAT_ARM_ARMED;
		a->until += (uint32_t)a->armed_s * 1000u;
	}
	if (a->state == SAT_ARM_ARMED) {
		if (reached(now_ms, a->until)) {
			a->state = SAT_ARM_IDLE;
			a->expired = true;
		} else {
			out.armed_left_s = (uint16_t)((a->until - now_ms + 999u) / 1000u);
		}
	}
	return out;
}
