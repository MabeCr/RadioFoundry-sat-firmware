// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The vote scan engine (docs/spec/vote-scan.md): which member of a vote list to listen to, and when to look at the others.
// No hardware and no channel reading here: the radio core (radio_sm.c) tunes the member the engine names, and tells it
// every poll what the receiver hears there. Host tests drive it with readings of their own (tests/host/voteTest.c).
//
// A conventional repeater transmits only while there is a call, and the sites of a linked system carry each call at the
// same time: between calls there is nothing to measure. So the engine votes when a call is on the air.
//
// Phases:
//   HOLD    on the voted site between calls, waiting for one. Kept while the site was strong in the last call (at or
//           above the threshold); after VOTE_RECHECK_MS with nothing heard, one pass over the others (SWEEP) in case a
//           call is on the air that the voted site doesn't hear (the radio moved)
//   SWEEP   between calls, the voted site weak or unknown: each member in turn for VOTE_SWEEP_DWELL_MS, listening for a call
//   ROUND   a call heard, the site it was heard on not known to be strong: each other member for VOTE_SAMPLE_MS (its
//           settle included), then the strongest is voted (the one the call was heard on unless another beats it by the
//           margin)
//   CALL    on the voted site during a call: its reading averaged; with peeks on and the average below the threshold, a
//           PEEK at most every VOTE_PEEK_GAP_MS
//   PEEK    one other member for VOTE_PEEK_MS, then back; it is voted at once when it beats the call's average by the margin
#ifndef RADIO_VOTE_H
#define RADIO_VOTE_H

#include <stdint.h>
#include <stdbool.h>

#define VOTE_MAX		16	// members (docs/spec/vote-scan.md 1)
#define VOTE_SETTLE_MS		50	// readings right after a retune belong to the last member (scan.h SCAN_SETTLE_MS)
#define VOTE_SAMPLE_MS		100	// ROUND: per member, the settle included
#define VOTE_SWEEP_DWELL_MS	150	// SWEEP: per member, the settle included (radio.h RADIO_SCAN_DWELL_MS)
#define VOTE_RECHECK_MS		30000u	// HOLD: a sweep after this long with no call heard
#define VOTE_PEEK_GAP_MS	2000u	// CALL: at most one peek in this long
#define VOTE_PEEK_MS		60	// PEEK: away from the call this long, the retune included
#define VOTE_PEEK_SETTLE_MS	40	// PEEK: of which the settle
#define VOTE_HANG_MS		1000u	// CALL: the call is over after this long without one
#define VOTE_AVG_SHIFT		3	// CALL: the average follows each reading by 1/8 (about 200 ms at a 25 ms poll)
#define VOTE_NONE		INT16_MIN	// a member not read (or read with nothing on the air)

typedef enum { VOTE_PH_OFF, VOTE_PH_HOLD, VOTE_PH_SWEEP, VOTE_PH_ROUND, VOTE_PH_CALL, VOTE_PH_PEEK } vote_phase_t;

typedef struct {
	uint8_t n;		// members, 2..VOTE_MAX
	int16_t threshold_dbm;	// a site at or above it is strong: no peeks, no round
	uint8_t margin_db;	// another member must beat the voted one by this much to be voted
	bool peeks;		// PEEK allowed during a call
} vote_cfg_t;

typedef struct {
	vote_cfg_t cfg;
	uint8_t phase;		// vote_phase_t
	uint8_t voted;		// the member voted (listened to, transmitted on with steering)
	uint8_t at;		// the member tuned now
	uint32_t phase_ms;	// when the phase began
	uint32_t tuned_ms;	// when `at` was tuned
	uint32_t heard_ms;	// the last poll a call was heard (CALL, HOLD)
	uint32_t peek_ms;	// the last peek's start
	int32_t acc;		// ROUND, PEEK: the sum of this member's readings since its settle
	uint16_t nacc;		// ... and their count
	int16_t avg_dbm;	// CALL: the voted site's average
	uint8_t next;		// ROUND, SWEEP, PEEK: the member the pass is at / the next to peek
	uint8_t left;		// ROUND, SWEEP: members still to visit
	int16_t last_dbm[VOTE_MAX];	// each member's last reading (VOTE_NONE: not read, or silent)
	// Counters for the debug link (docs/spec/vote-scan.md 8)
	uint32_t rounds, sweeps, switches, peeks;
	uint16_t peek_max_ms;	// the longest peek, the retune back included
} vote_t;

// Start voting on member start (the list's first: docs/spec/vote-scan.md 2): the engine is in HOLD there, the caller
// tunes it
void vote_begin(vote_t *v, const vote_cfg_t *cfg, uint8_t start, uint32_t now_ms);
void vote_end(vote_t *v);
static inline bool vote_on(const vote_t *v)
{
	return v->phase != VOTE_PH_OFF;
}

// Every poll. rssi_dbm: the receiver's strength reading on member `at`; call: a call of the member's own kind is heard
// there (its sync: NAC, color code, RAN or tone). Returns the member to tune next (the caller tunes it and calls
// vote_tuned), or -1 to stay
int vote_poll(vote_t *v, uint32_t now_ms, int16_t rssi_dbm, bool call);
// The caller tuned member m at now_ms
void vote_tuned(vote_t *v, uint8_t m, uint32_t now_ms);

// The member PTT transmits on: the voted one (steering), or fixed (0-based) when it names a member
uint8_t vote_tx_member(const vote_t *v, bool fixed, uint8_t fixed_member);

#endif
