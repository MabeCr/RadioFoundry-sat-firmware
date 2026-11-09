// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The automatic Doppler following (core/sat/sat_follow.c, docs/spec/satellite.md 4.5) against real passes: the Doppler
// curve of the reference passes (sat_ref.h, Skyfield's) is fed to it a second at a time, and a receiver that follows it
// keeps within the threshold, retunes no more often than the interval, keeps the user's nudge, and stays still while
// transmitting
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sat/sat_follow.h"
#include "sat/sat_track.h"
#include "sat_ref.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// The rules, one at a time
static void rules(void)
{
	sat_follow_t f = { .on = true };
	CHECK(!sat_follow_step(&f, 1000, 437000000u, 437001400u, false));	// 1.4 kHz: inside the threshold
	CHECK(sat_follow_step(&f, 1000, 437000000u, 437001500u, false));	// 1.5 kHz: retune
	CHECK(!sat_follow_step(&f, 2000, 437001500u, 437004000u, false));	// 2.5 kHz behind, but 1 s since the last
	CHECK(!sat_follow_step(&f, 5999, 437001500u, 437004000u, false));
	CHECK(sat_follow_step(&f, 6000, 437001500u, 437004000u, false));	// 5 s
	CHECK(sat_follow_step(&f, 20000, 437004000u, 436999000u, false));	// below the wanted one too
	CHECK(!sat_follow_step(&f, 40000, 437004000u, 437100000u, true));	// transmitting: never
	f.on = false;
	CHECK(!sat_follow_step(&f, 60000, 437004000u, 437100000u, false));	// off
	// A retune of the user's counts as the last one; time 0 is not "none yet" after it
	f.on = true;
	f.last_ms = 0;
	sat_follow_retuned(&f, 0);
	CHECK(f.last_ms != 0);
	CHECK(!sat_follow_step(&f, 1000, 437000000u, 437100000u, false));
	// The first step may retune at once, whatever the time
	sat_follow_t g = { .on = true };
	CHECK(sat_follow_step(&g, 1, 437000000u, 437100000u, false));
}

// A pass: the receiver follows the corrected downlink plus a nudge, the way the screen does it
static void pass(int tle, uint32_t norad, int32_t nudge_hz, unsigned *retunes_out, uint32_t *worst_out)
{
	sat_track_t t;
	memset(&t, 0, sizeof t);
	uint8_t e1, e2;
	CHECK(sat_track_load(&t, ref_tles[tle].l1, ref_tles[tle].l2, &e1, &e2));
	sat_profile_t prof = { .norad = norad, .up_hz = 145850000u, .down_hz = 437800000u };
	t.profile = prof;
	t.has_profile = true;
	sat_track_observer(&t, ref_obs[0], ref_obs[1], ref_obs[2]);
	const ref_pass_t *p = NULL;
	for (unsigned i = 0; i < sizeof ref_passes / sizeof ref_passes[0]; i++)
		if (ref_passes[i].tle == tle && ref_passes[i].max_el >= 30.0) {
			p = &ref_passes[i];
			break;
		}
	CHECK(p != NULL);
	if (p == NULL)
		return;
	sat_follow_t f = { .on = true };
	// From a minute before the rise to the set, a second at a time; the receiver starts on the first wanted frequency,
	// as the # that began the following leaves it
	uint32_t tuned = 0, worst = 0;
	unsigned retunes = 0;
	uint32_t last_retune_ms = 0, min_gap = UINT32_MAX;
	for (double u = p->aos - 60.0; u <= p->los; u += 1.0) {
		sat_track_now_t now;
		CHECK(sat_track_at(&t, u, &now));
		uint32_t want = (uint32_t)((int32_t)now.rx_hz + nudge_hz);
		uint32_t ms = (uint32_t)((u - (p->aos - 60.0)) * 1000.0) + 1000u;
		if (tuned == 0) {
			tuned = want;
			sat_follow_retuned(&f, ms);
		} else if (sat_follow_step(&f, ms, tuned, want, false)) {
			tuned = want;
			retunes++;
			if (last_retune_ms != 0 && ms - last_retune_ms < min_gap)
				min_gap = ms - last_retune_ms;
			last_retune_ms = ms;
		}
		uint32_t err = tuned > want ? tuned - want : want - tuned;
		if (u >= p->aos && err > worst)	// from the rise: the minute before is the shift being applied
			worst = err;
	}
	*retunes_out = retunes;
	*worst_out = worst;
	CHECK(min_gap == UINT32_MAX || min_gap >= SAT_FOLLOW_MIN_MS);
	printf("satFollowTest: %s pass of %.0f s with %+d Hz nudge: %u retunes, worst error %u Hz, shortest gap %u ms\n",
	       tle == 0 ? "ISS" : tle == 1 ? "SO-50" : "AO-91", p->los - p->aos, (int)nudge_hz, retunes, (unsigned)worst,
	       min_gap == UINT32_MAX ? 0u : (unsigned)min_gap);
}

int main(void)
{
	rules();
	for (int tle = 0; tle < 3; tle++) {
		unsigned r0, r1;
		uint32_t w0, w1;
		pass(tle, tle == 0 ? 25544 : tle == 1 ? 27607 : 43017, 0, &r0, &w0);
		// From the rise the receiver is never further than the threshold plus what a second of Doppler adds (under 100 Hz)
		CHECK(w0 < SAT_FOLLOW_THRESHOLD_HZ + 100u);
		CHECK(r0 >= 2 && r0 <= 40);		// a few retunes a pass, not a flood
		// The user's nudge shifts the wanted frequency, the following keeps to it just the same
		pass(tle, tle == 0 ? 25544 : tle == 1 ? 27607 : 43017, 5000, &r1, &w1);
		CHECK(w1 < SAT_FOLLOW_THRESHOLD_HZ + 100u);
		CHECK(r1 >= 2 && r1 <= 40);
	}
	printf("satFollowTest: %s\n", failures ? "FAILED" : "passed");
	return failures != 0;
}
