// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The vote scan engine (vote.h, docs/spec/vote-scan.md)
#include <string.h>
#include "config.h"
#include "radio/vote.h"

#if CONFIG_VOTE_SCAN

static bool strong(const vote_t *v, uint8_t m)
{
	return v->last_dbm[m] != VOTE_NONE && v->last_dbm[m] >= v->cfg.threshold_dbm;
}

static void phase(vote_t *v, uint8_t p, uint32_t now_ms)
{
	v->phase = p;
	v->phase_ms = now_ms;
}

static uint8_t after(const vote_t *v, uint8_t m)
{
	return (uint8_t)((m + 1u) % v->cfg.n);
}

// The readings of the member tuned now, from its settle on
static void take(vote_t *v, uint32_t now_ms, int16_t rssi_dbm, uint32_t settle_ms)
{
	if (now_ms - v->tuned_ms >= settle_ms) {
		v->acc += rssi_dbm;
		v->nacc++;
	}
}

static int16_t taken(const vote_t *v)
{
	return v->nacc != 0 ? (int16_t)(v->acc / (int32_t)v->nacc) : VOTE_NONE;
}

void vote_begin(vote_t *v, const vote_cfg_t *cfg, uint8_t start, uint32_t now_ms)
{
	memset(v, 0, sizeof *v);
	v->cfg = *cfg;
	if (v->cfg.n > VOTE_MAX)
		v->cfg.n = VOTE_MAX;
	if (v->cfg.n < 2) {
		v->phase = VOTE_PH_OFF;
		return;
	}
	for (uint8_t i = 0; i < VOTE_MAX; i++)
		v->last_dbm[i] = VOTE_NONE;
	v->voted = v->at = start < v->cfg.n ? start : 0;
	v->tuned_ms = v->heard_ms = v->peek_ms = now_ms;
	phase(v, VOTE_PH_HOLD, now_ms);
}

void vote_end(vote_t *v)
{
	v->phase = VOTE_PH_OFF;
}

void vote_tuned(vote_t *v, uint8_t m, uint32_t now_ms)
{
	// Back on the voted site from a peek: how long the call was away from it
	if (v->phase == VOTE_PH_CALL && m == v->voted && v->at != m) {
		uint32_t away = now_ms - v->peek_ms;
		if (away > v->peek_max_ms)
			v->peek_max_ms = (uint16_t)(away > 0xFFFFu ? 0xFFFFu : away);
	}
	v->at = m;
	v->tuned_ms = now_ms;
	v->acc = 0;
	v->nacc = 0;
}

// A call heard on member `at` (HOLD or SWEEP): it carries on there if that site is known to be strong, else a round
// compares every member first, starting with this one (already tuned: no settle to wait for)
static int call_heard(vote_t *v, uint32_t now_ms, int16_t rssi_dbm)
{
	v->heard_ms = now_ms;
	if (v->at != v->voted)
		v->switches++;		// the call turned up on another member during a sweep
	v->voted = v->at;
	if (strong(v, v->at)) {
		v->avg_dbm = rssi_dbm;
		v->peek_ms = now_ms;
		v->next = after(v, v->voted);
		phase(v, VOTE_PH_CALL, now_ms);
		return -1;
	}
	v->rounds++;
	v->next = after(v, v->at);
	v->left = (uint8_t)(v->cfg.n - 1u);
	v->acc = 0;
	v->nacc = 0;
	v->tuned_ms = now_ms - VOTE_SETTLE_MS;	// settled long ago: this member's readings count at once
	phase(v, VOTE_PH_ROUND, now_ms);
	take(v, now_ms, rssi_dbm, VOTE_SETTLE_MS);
	return -1;
}

static int sweep_start(vote_t *v, uint32_t now_ms)
{
	v->sweeps++;
	v->next = after(v, v->voted);
	v->left = (uint8_t)(v->cfg.n - 1u);
	phase(v, VOTE_PH_SWEEP, now_ms);
	uint8_t m = v->next;
	v->next = after(v, m);
	v->left--;
	return m;
}

static int hold(vote_t *v, uint32_t now_ms, int16_t rssi_dbm, bool call)
{
	if (call)
		return call_heard(v, now_ms, rssi_dbm);
	// A weak or unknown site: look for the next call on every member; a strong one: now and then, in case a call is
	// on the air there that this site doesn't carry to us
	if (!strong(v, v->voted) || now_ms - v->heard_ms >= VOTE_RECHECK_MS)
		return sweep_start(v, now_ms);
	return -1;
}

static int sweep(vote_t *v, uint32_t now_ms, int16_t rssi_dbm, bool call)
{
	if (call && now_ms - v->tuned_ms >= VOTE_SETTLE_MS)
		return call_heard(v, now_ms, rssi_dbm);
	if (now_ms - v->tuned_ms < VOTE_SWEEP_DWELL_MS)
		return -1;
	if (v->left == 0) {
		v->heard_ms = now_ms;		// the recheck counts from here
		phase(v, VOTE_PH_HOLD, now_ms);
		return v->at != v->voted ? v->voted : -1;
	}
	uint8_t m = v->next;
	v->next = after(v, m);
	v->left--;
	return m;
}

static int round_(vote_t *v, uint32_t now_ms, int16_t rssi_dbm)
{
	take(v, now_ms, rssi_dbm, VOTE_SETTLE_MS);
	if (now_ms - v->tuned_ms < VOTE_SAMPLE_MS)
		return -1;
	v->last_dbm[v->at] = taken(v);
	if (v->left != 0) {
		uint8_t m = v->next;
		v->next = after(v, m);
		v->left--;
		return m;
	}
	// Every member read: the strongest, if it beats the one the call was heard on by the margin
	uint8_t best = v->voted;
	for (uint8_t i = 0; i < v->cfg.n; i++)
		if (v->last_dbm[i] != VOTE_NONE && v->last_dbm[i] > v->last_dbm[best])
			best = i;
	if (best != v->voted && (v->last_dbm[v->voted] == VOTE_NONE ||
				 v->last_dbm[best] >= v->last_dbm[v->voted] + v->cfg.margin_db)) {
		v->voted = best;
		v->switches++;
	}
	v->avg_dbm = v->last_dbm[v->voted] != VOTE_NONE ? v->last_dbm[v->voted] : rssi_dbm;
	v->heard_ms = v->peek_ms = now_ms;
	v->next = after(v, v->voted);
	phase(v, VOTE_PH_CALL, now_ms);
	return v->at != v->voted ? v->voted : -1;
}

static int call_(vote_t *v, uint32_t now_ms, int16_t rssi_dbm, bool call)
{
	if (now_ms - v->tuned_ms < VOTE_SETTLE_MS)
		return -1;			// just back from a peek: the reading is still the other site's
	if (call) {
		v->heard_ms = now_ms;
		v->avg_dbm = (int16_t)(v->avg_dbm + (rssi_dbm - v->avg_dbm) / (1 << VOTE_AVG_SHIFT));
	} else if (now_ms - v->heard_ms >= VOTE_HANG_MS) {
		v->last_dbm[v->voted] = v->avg_dbm;	// the next call starts here if this site was strong
		v->heard_ms = now_ms;
		phase(v, VOTE_PH_HOLD, now_ms);
		return -1;
	}
	if (!v->cfg.peeks || v->avg_dbm >= v->cfg.threshold_dbm || now_ms - v->peek_ms < VOTE_PEEK_GAP_MS)
		return -1;
	if (v->next == v->voted)
		v->next = after(v, v->next);
	uint8_t m = v->next;
	v->next = after(v, m);
	v->peek_ms = now_ms;
	v->peeks++;
	phase(v, VOTE_PH_PEEK, now_ms);
	return m;
}

static int peek(vote_t *v, uint32_t now_ms, int16_t rssi_dbm)
{
	take(v, now_ms, rssi_dbm, VOTE_PEEK_SETTLE_MS);
	if (now_ms - v->tuned_ms < VOTE_PEEK_MS)
		return -1;
	int16_t r = taken(v);
	v->last_dbm[v->at] = r;
	phase(v, VOTE_PH_CALL, now_ms);
	if (r != VOTE_NONE && r >= v->avg_dbm + v->cfg.margin_db) {
		// Better than the call's site: the rest of the call comes from here, with no second retune
		v->voted = v->at;
		v->avg_dbm = r;
		v->heard_ms = now_ms;
		v->switches++;
		uint32_t away = now_ms - v->peek_ms;
		if (away > v->peek_max_ms)
			v->peek_max_ms = (uint16_t)(away > 0xFFFFu ? 0xFFFFu : away);
		v->next = after(v, v->voted);
		return -1;
	}
	return v->voted;
}

int vote_poll(vote_t *v, uint32_t now_ms, int16_t rssi_dbm, bool call)
{
	switch (v->phase) {
	case VOTE_PH_HOLD:
		return hold(v, now_ms, rssi_dbm, call);
	case VOTE_PH_SWEEP:
		return sweep(v, now_ms, rssi_dbm, call);
	case VOTE_PH_ROUND:
		return round_(v, now_ms, rssi_dbm);
	case VOTE_PH_CALL:
		return call_(v, now_ms, rssi_dbm, call);
	case VOTE_PH_PEEK:
		return peek(v, now_ms, rssi_dbm);
	default:
		return -1;
	}
}

uint8_t vote_tx_member(const vote_t *v, bool fixed, uint8_t fixed_member)
{
	return fixed && fixed_member < v->cfg.n ? fixed_member : v->voted;
}

#endif // CONFIG_VOTE_SCAN
