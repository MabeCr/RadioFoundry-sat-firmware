// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The signal level over a pass (core/sat/sat_sig.c, docs/spec/ui.md 2.6): readings go into bins by time, the highest of a
// bin is kept, a stretch nobody read stays empty, and the chart's range follows what was read
#include <stdio.h>
#include <string.h>
#include "sat/sat_sig.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static sat_sig_t sig;

static void window(void)
{
	sat_sig_reset(&sig, 10000, 10600);	// a 10 minute pass
	CHECK(sig.t0 == 10000 - SAT_SIG_PAD_S);
	CHECK(sig.n == (600 + 2 * SAT_SIG_PAD_S) / SAT_SIG_STEP_S + 1);
	CHECK(sat_sig_is(&sig, 10000, 10600) && !sat_sig_is(&sig, 10000, 10601) && !sat_sig_is(&sig, 10001, 10600));
	// outside the window, or with the clock not set, nothing is taken
	CHECK(!sat_sig_add(&sig, 0, -100));
	CHECK(!sat_sig_add(&sig, sig.t0 - 1, -100));
	CHECK(!sat_sig_add(&sig, 10600 + SAT_SIG_PAD_S + 3, -100));
	sat_sig_stats_t st;
	sat_sig_stats(&sig, &st);
	CHECK(!st.any && st.samples == 0);
	// the first and the last bin
	CHECK(sat_sig_add(&sig, sig.t0, -120));
	CHECK(sat_sig_add(&sig, 10600 + SAT_SIG_PAD_S, -90));
	CHECK(sat_sig_has(&sig, 0) && sat_sig_dbm(&sig, 0) == -120);
	CHECK(sat_sig_has(&sig, sig.n - 1) && sat_sig_dbm(&sig, sig.n - 1) == -90);
}

static void keeps_the_highest(void)
{
	sat_sig_reset(&sig, 10000, 10600);
	uint32_t t = 10100;
	CHECK(sat_sig_add(&sig, t, -110));
	CHECK(sat_sig_add(&sig, t + 1, -95));	// the same 2 s bin: the highest stays
	CHECK(sat_sig_add(&sig, t, -120));
	uint16_t i = (uint16_t)((t - sig.t0) / SAT_SIG_STEP_S);
	CHECK(sat_sig_dbm(&sig, i) == -95);
	CHECK(sat_sig_add(&sig, t + 2, -101));	// the next bin is its own
	CHECK(sat_sig_dbm(&sig, i + 1) == -101 && sat_sig_bin_utc(&sig, i) == t);
	// the extremes of what a byte holds
	CHECK(sat_sig_add(&sig, t + 4, -300) && sat_sig_dbm(&sig, i + 2) == -140);
	CHECK(sat_sig_add(&sig, t + 6, 200) && sat_sig_dbm(&sig, i + 3) == 114);
}

static void gaps_stay_empty(void)
{
	sat_sig_reset(&sig, 10000, 10600);
	// readings for a minute, then the user is on another screen for three, then back
	for (uint32_t t = 10000; t < 10060; t += 2)
		CHECK(sat_sig_add(&sig, t, -100));
	for (uint32_t t = 10240; t < 10300; t += 2)
		CHECK(sat_sig_add(&sig, t, -90));
	uint16_t a = (uint16_t)((10059 - sig.t0) / SAT_SIG_STEP_S), b = (uint16_t)((10240 - sig.t0) / SAT_SIG_STEP_S);
	CHECK(sat_sig_has(&sig, a) && sat_sig_has(&sig, b));
	for (uint16_t i = a + 1; i < b; i++)
		CHECK(!sat_sig_has(&sig, i));
	CHECK(b - a - 1 == (10240 - 10060) / SAT_SIG_STEP_S);	// 90 empty bins, not squeezed away
	sat_sig_stats_t st;
	sat_sig_stats(&sig, &st);
	CHECK(st.any && st.samples == 60 && st.lo == -100 && st.hi == -90);
	CHECK(st.hi_utc >= 10240 && st.hi_utc < 10300);
}

static void a_long_pass_is_cut_not_overrun(void)
{
	sat_sig_reset(&sig, 20000, 20000 + 3600);	// an hour: more than the bins hold
	CHECK(sig.n == SAT_SIG_MAX);
	CHECK(sat_sig_add(&sig, sig.t0 + (SAT_SIG_MAX - 1) * SAT_SIG_STEP_S, -100));
	CHECK(!sat_sig_add(&sig, sig.t0 + SAT_SIG_MAX * SAT_SIG_STEP_S, -100));
	sat_sig_reset(&sig, 10, 100);	// a pass near the start of the clock: no negative time
	CHECK(sig.t0 == 0 && sig.n > 0);
}

static void range(void)
{
	int16_t lo, hi;
	sat_sig_stats_t st = { 0 };
	sat_sig_range(&st, &lo, &hi);
	CHECK(lo == -130 && hi == -80);		// nothing read: a default
	st = (sat_sig_stats_t){ .any = true, .samples = 5, .lo = -117, .hi = -96 };
	sat_sig_range(&st, &lo, &hi);
	CHECK(lo == -125 && hi == -90);		// a step below and above, on 5 dB
	st = (sat_sig_stats_t){ .any = true, .samples = 5, .lo = -100, .hi = -99 };
	sat_sig_range(&st, &lo, &hi);
	CHECK(hi - lo == 20 && lo < -100 && hi > -99);	// never narrower than 20 dB
	st = (sat_sig_stats_t){ .any = true, .samples = 5, .lo = -140, .hi = -139 };
	sat_sig_range(&st, &lo, &hi);
	CHECK(lo >= -140 && hi - lo >= 20);
	st = (sat_sig_stats_t){ .any = true, .samples = 5, .lo = -50, .hi = -20 };
	sat_sig_range(&st, &lo, &hi);
	CHECK(hi <= -30 && hi - lo >= 20);
}

int main(void)
{
	window();
	keeps_the_highest();
	gaps_stay_empty();
	a_long_pass_is_cut_not_overrun();
	range();
	printf("satSigTest: %s\n", failures ? "FAILED" : "passed");
	return failures != 0;
}
