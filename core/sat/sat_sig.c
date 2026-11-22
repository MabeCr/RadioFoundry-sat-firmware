// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The signal level over a pass (sat_sig.h)
#include <string.h>
#include "sat/sat_sig.h"

#define V_OFFSET	141
#define RANGE_MIN_DB	20
#define LEVEL_FLOOR	(-140)
#define LEVEL_CEIL	(-30)

void sat_sig_reset(sat_sig_t *s, uint32_t aos, uint32_t los)
{
	memset(s, 0, sizeof(*s));
	s->aos = aos;
	s->los = los;
	s->t0 = aos > SAT_SIG_PAD_S ? aos - SAT_SIG_PAD_S : 0;
	uint32_t span = los > s->t0 ? los + SAT_SIG_PAD_S - s->t0 : 0;
	uint32_t n = span / SAT_SIG_STEP_S + 1u;
	s->n = (uint16_t)(n > SAT_SIG_MAX ? SAT_SIG_MAX : n);
}

bool sat_sig_is(const sat_sig_t *s, uint32_t aos, uint32_t los)
{
	return s->aos == aos && s->los == los && s->n != 0;
}

bool sat_sig_add(sat_sig_t *s, uint32_t utc, int16_t dbm)
{
	if (utc == 0 || utc < s->t0)
		return false;
	uint32_t i = (utc - s->t0) / SAT_SIG_STEP_S;
	if (i >= s->n)
		return false;
	int v = dbm + V_OFFSET;
	if (v < 1)
		v = 1;
	if (v > 255)
		v = 255;
	if (s->v[i] < v)
		s->v[i] = (uint8_t)v;
	return true;
}

bool sat_sig_has(const sat_sig_t *s, uint16_t i)
{
	return i < s->n && s->v[i] != 0;
}

int16_t sat_sig_dbm(const sat_sig_t *s, uint16_t i)
{
	return (int16_t)((int)s->v[i] - V_OFFSET);
}

uint32_t sat_sig_bin_utc(const sat_sig_t *s, uint16_t i)
{
	return s->t0 + (uint32_t)i * SAT_SIG_STEP_S;
}

void sat_sig_stats(const sat_sig_t *s, sat_sig_stats_t *st)
{
	memset(st, 0, sizeof(*st));
	for (uint16_t i = 0; i < s->n; i++) {
		if (s->v[i] == 0)
			continue;
		int16_t d = sat_sig_dbm(s, i);
		if (!st->any || d < st->lo)
			st->lo = d;
		if (!st->any || d > st->hi) {
			st->hi = d;
			st->hi_utc = sat_sig_bin_utc(s, i);
		}
		st->any = true;
		st->samples++;
	}
}

static int16_t floor5(int16_t v)
{
	return (int16_t)((v >= 0 ? v / 5 : -((-v + 4) / 5)) * 5);
}

void sat_sig_range(const sat_sig_stats_t *st, int16_t *lo, int16_t *hi)
{
	int16_t l, h;
	if (st == NULL || !st->any) {
		*lo = -130;
		*hi = -80;
		return;
	}
	l = (int16_t)(floor5(st->lo) - 5);
	h = (int16_t)(-floor5(-st->hi) + 5);	// rounded up
	if (h - l < RANGE_MIN_DB) {
		int16_t mid = (int16_t)((l + h) / 2);
		l = (int16_t)(mid - RANGE_MIN_DB / 2);
		h = (int16_t)(mid + RANGE_MIN_DB / 2);
	}
	if (l < LEVEL_FLOOR)
		l = LEVEL_FLOOR;
	if (h > LEVEL_CEIL)
		h = LEVEL_CEIL;
	if (h - l < RANGE_MIN_DB) {	// a clamp ate into the range: widen on the side that has room
		if (l <= LEVEL_FLOOR)
			h = (int16_t)(l + RANGE_MIN_DB);
		else
			l = (int16_t)(h - RANGE_MIN_DB);
	}
	*lo = l;
	*hi = h;
}
