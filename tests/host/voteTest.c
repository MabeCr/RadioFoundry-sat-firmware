// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The vote scan engine (core/radio/vote.c, docs/spec/vote-scan.md) against a simulated system: sites with their own
// signal strength, all carrying the same calls, silent between them. The radio retunes at once to whatever the engine
// names, and polls every 25 ms as the radio core does
#include <stdio.h>
#include <string.h>
#include "radio/vote.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define POLL_MS		25u
#define NOISE_DBM	(-125)

static int16_t site_dbm[VOTE_MAX];	// what each site reads while it transmits (NOISE_DBM: out of range)
static bool on_air;			// a call on the system: every site transmits it
static uint32_t now;
static unsigned tunes, away_polls;	// retunes the engine asked for; polls spent off the voted site during calls
static uint32_t sync_delay_ms;		// a digital site: its sync (the call) comes this long after the retune; its carrier at once

static vote_t v;

static void run(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += POLL_MS) {
		now += POLL_MS;
		int16_t dbm = on_air ? site_dbm[v.at] : NOISE_DBM;
		bool carrier = on_air && site_dbm[v.at] > -118 && now - v.tuned_ms >= VOTE_SETTLE_MS;
		bool call = on_air && site_dbm[v.at] > -118 && now - v.tuned_ms >= sync_delay_ms;
		if (on_air && v.phase == VOTE_PH_PEEK)
			away_polls++;
		int m = vote_poll(&v, now, dbm, call, carrier && sync_delay_ms != 0);
		if (m >= 0) {
			tunes++;
			vote_tuned(&v, (uint8_t)m, now);
		}
	}
}

static void start(uint8_t n, int16_t threshold, uint8_t margin, bool peeks, const int16_t *dbm)
{
	vote_cfg_t cfg = { .n = n, .threshold_dbm = threshold, .margin_db = margin, .peeks = peeks };
	memset(site_dbm, 0, sizeof site_dbm);
	for (uint8_t i = 0; i < n; i++)
		site_dbm[i] = dbm[i];
	on_air = false;
	now = 1000;
	sync_delay_ms = 0;
	tunes = away_polls = 0;
	vote_begin(&v, &cfg, 0, now);
}

// The first call: nothing is known, so the radio sweeps between calls and votes the strongest when one starts
static void test_first_call(void)
{
	static const int16_t dbm[] = { -100, -80, -95, -110 };
	start(4, -70, 6, false, dbm);
	CHECK(vote_on(&v) && v.phase == VOTE_PH_HOLD && v.voted == 0);
	run(1000);
	CHECK(v.phase == VOTE_PH_SWEEP || v.phase == VOTE_PH_HOLD);	// the start site is unknown: looking around
	CHECK(v.sweeps >= 1);
	on_air = true;
	run(1500);
	CHECK(v.rounds == 1 && v.phase == VOTE_PH_CALL && v.voted == 1 && v.at == 1);
	CHECK(v.last_dbm[0] == -100 && v.last_dbm[1] == -80 && v.last_dbm[3] == -110);
	// The call ends: the site keeps what the call read
	on_air = false;
	run(1500);
	CHECK(v.phase != VOTE_PH_CALL && v.last_dbm[1] >= -81 && v.last_dbm[1] <= -79);
}

// A digital site's sync takes longer than a sweep's dwell (a P25 NAC comes once a frame): a sweep that waits on a carrier
// finds the call, one that doesn't never does
static void test_sweep_waits_for_sync(void)
{
	static const int16_t dbm[] = { -85, -83, -88, -90 };	// all weak: below the -70 threshold, so the radio sweeps
	start(4, -70, 6, true, dbm);
	sync_delay_ms = 400;
	run(1500);				// between calls: sweeping
	on_air = true;
	run(6000);
	CHECK(v.rounds >= 1 && (v.phase == VOTE_PH_CALL || v.phase == VOTE_PH_ROUND));
	// A carrier that never gives sync (another system): waited on once, then left alone for a while
	start(3, -70, 6, true, dbm);
	sync_delay_ms = 100000;
	on_air = true;
	run(VOTE_PROBE_MS * 3 + 1000);
	CHECK(v.rounds == 0 && v.phase == VOTE_PH_SWEEP);
	CHECK(v.sweeps >= 1);
}

// A strong site stays voted between calls and takes the next call with no round; after the recheck time one sweep
static void test_strong_site_holds(void)
{
	static const int16_t dbm[] = { -60, -90, -95 };
	start(3, -70, 6, true, dbm);
	on_air = true;
	run(1000);				// the first call: a round, the start site wins
	CHECK(v.voted == 0 && v.rounds == 1 && v.phase == VOTE_PH_CALL);
	on_air = false;
	run(2000);
	CHECK(v.phase == VOTE_PH_HOLD && v.at == 0);
	unsigned t = tunes;
	run(10000);
	CHECK(tunes == t && v.phase == VOTE_PH_HOLD);	// strong: no sweeping between calls
	on_air = true;
	run(1000);
	CHECK(v.rounds == 1 && v.phase == VOTE_PH_CALL && v.voted == 0 && tunes == t);	// no round, no retune
	CHECK(v.peeks == 0);				// strong: no peeks either
	on_air = false;
	run(VOTE_HANG_MS + VOTE_RECHECK_MS + 3 * VOTE_SWEEP_DWELL_MS + 500);
	CHECK(v.sweeps == 1 && v.phase == VOTE_PH_HOLD && v.at == 0);	// one pass after the recheck time, then home
}

// Within the margin, the site the call was heard on stays voted
static void test_margin_holds_a_near_tie(void)
{
	static const int16_t dbm[] = { -85, -82, -100 };
	start(3, -70, 6, false, dbm);
	on_air = true;
	run(1000);
	CHECK(v.voted == 0 && v.switches == 0);
	// A clear winner moves it
	site_dbm[2] = -75;
	on_air = false;
	run(1500);
	on_air = true;
	run(1500);
	CHECK(v.voted == 2 && v.switches == 1);
	// A near tie the call starts on is kept, and so is the margin's edge: 6 dB better moves it, 5 dB doesn't
	CHECK(v.last_dbm[0] == -85 && v.last_dbm[1] == -82);
}

// During a call on a weak site: peeks at most every gap, each about VOTE_PEEK_MS, and a better site is voted at once
static void test_peeks(void)
{
	static const int16_t dbm[] = { -90, -100, -95, -80 };
	start(4, -70, 6, true, dbm);
	on_air = true;
	run(1000);
	CHECK(v.voted == 3 && v.phase == VOTE_PH_CALL);	// the round picked the best: still weak (-80 < -70)
	uint32_t p0 = v.peeks;
	run(10000);
	CHECK(v.peeks > p0 && v.peeks <= p0 + 10000 / VOTE_PEEK_GAP_MS + 1);
	CHECK(v.voted == 3);				// nothing better
	CHECK(v.peek_max_ms >= VOTE_PEEK_MS && v.peek_max_ms <= VOTE_PEEK_MS + 2 * POLL_MS);
	// The radio moves: site 1 becomes much stronger during the call
	site_dbm[1] = -60;
	run(8000);
	CHECK(v.voted == 1 && v.at == 1 && v.phase == VOTE_PH_CALL);
	// Strong now: no more peeks
	uint32_t p1 = v.peeks;
	run(10000);
	CHECK(v.peeks == p1);
}

// Peeks off: a weak site keeps the whole call; the next call starts with a round
static void test_peeks_off(void)
{
	static const int16_t dbm[] = { -90, -100, -95 };
	start(3, -70, 6, false, dbm);
	on_air = true;
	run(1000);
	CHECK(v.voted == 0);
	site_dbm[2] = -60;
	unsigned t = tunes;
	run(10000);
	CHECK(v.peeks == 0 && tunes == t && v.voted == 0 && away_polls == 0);
	on_air = false;
	run(1500);
	on_air = true;
	run(1500);
	CHECK(v.rounds == 2 && v.voted == 2);
}

// A site out of range carries nothing: a call heard on another one during the sweep is voted there
static void test_out_of_range(void)
{
	static const int16_t dbm[] = { NOISE_DBM, -95, NOISE_DBM };
	start(3, -70, 6, true, dbm);
	run(2000);
	on_air = true;
	run(2000);
	CHECK(v.voted == 1 && v.phase == VOTE_PH_CALL);
}

static void test_tx_member_and_bounds(void)
{
	static const int16_t dbm[] = { -60, -90 };
	start(2, -70, 6, true, dbm);
	CHECK(vote_tx_member(&v, false, 1) == v.voted && vote_tx_member(&v, true, 1) == 1 && vote_tx_member(&v, true, 9) == v.voted);
	vote_cfg_t one = { .n = 1, .threshold_dbm = -70, .margin_db = 6 };
	vote_t w;
	vote_begin(&w, &one, 0, 0);
	CHECK(!vote_on(&w) && vote_poll(&w, 25, -60, true, false) == -1);
	vote_end(&v);
	CHECK(!vote_on(&v));
}

int main(void)
{
	test_first_call();
	test_sweep_waits_for_sync();
	test_strong_site_holds();
	test_margin_holds_a_near_tie();
	test_peeks();
	test_peeks_off();
	test_out_of_range();
	test_tx_member_and_bounds();
	printf("voteTest: %s\n", failures ? "FAILED" : "passed");
	return failures != 0;
}
