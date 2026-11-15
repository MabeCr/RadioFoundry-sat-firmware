// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Satellite element sets by sound: the frame and the transfer bookkeeping (sat_sound.h)
#include <string.h>
#include "sat/sat_sound.h"

static bool name_ok(const uint8_t *s, size_t n)
{
	if (n > SAT_SOUND_NAME_MAX) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (s[i] < 0x20u || s[i] > 0x7Eu) {
			return false;
		}
	}
	return true;
}

static bool prof_zero(const sat_sound_prof_t *p)
{
	return p->up_hz == 0 && p->down_hz == 0 && p->up_tone == 0 && p->arm_tone == 0 && p->arm_ms == 0 && p->armed_s == 0;
}

static bool freq_ok(uint32_t hz)
{
	return hz == 0 || (hz >= 100000000u && hz <= 1300000000u);
}

static bool tone_ok(uint16_t t)
{
	return t == 0 || (t >= 600u && t <= 2600u);
}

static bool prof_ok(const sat_sound_prof_t *p)
{
	return freq_ok(p->up_hz) && freq_ok(p->down_hz) && tone_ok(p->up_tone) && tone_ok(p->arm_tone) && p->arm_ms <= 10000u &&
	       p->armed_s <= 3600u;
}

static void put16(uint8_t *o, uint16_t v)
{
	o[0] = (uint8_t)v;
	o[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *o, uint32_t v)
{
	put16(o, (uint16_t)v);
	put16(o + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *o)
{
	return (uint16_t)(o[0] | (o[1] << 8));
}

static uint32_t get32(const uint8_t *o)
{
	return get16(o) | ((uint32_t)get16(o + 2) << 16);
}

size_t sat_sound_build(uint8_t *out, size_t max, uint8_t id, uint8_t index, uint8_t count, const char *name,
		       const char *line1, const char *line2)
{
	return sat_sound_build_profile(out, max, id, index, count, name, line1, line2, NULL);
}

size_t sat_sound_build_profile(uint8_t *out, size_t max, uint8_t id, uint8_t index, uint8_t count, const char *name,
			       const char *line1, const char *line2, const sat_sound_prof_t *p)
{
	bool with = p != NULL && !prof_zero(p);
	if (with && !prof_ok(p))
		return 0;
	size_t nlen = name != NULL ? strlen(name) : 0;
	sat_elements_t e;

	if (count == 0 || count > SAT_SOUND_SETS_MAX || index >= count || !name_ok((const uint8_t *)name, nlen)) {
		return 0;
	}
	if (line1 == NULL || line2 == NULL || strlen(line1) != TLE_LINE_LEN || strlen(line2) != TLE_LINE_LEN ||
	    tle_parse(line1, line2, &e) != TLE_OK) {
		return 0;
	}
	size_t total = SAT_SOUND_HEAD + nlen + 2u * TLE_LINE_LEN + (with ? SAT_SOUND_PROF_LEN : 0u);
	if (out == NULL || max < total) {
		return 0;
	}
	out[0] = SAT_SOUND_MAGIC;
	out[1] = with ? SAT_SOUND_VERSION_PROF : SAT_SOUND_VERSION;
	out[2] = id;
	out[3] = index;
	out[4] = count;
	out[5] = (uint8_t)nlen;
	memcpy(out + SAT_SOUND_HEAD, name, nlen);
	memcpy(out + SAT_SOUND_HEAD + nlen, line1, TLE_LINE_LEN);
	memcpy(out + SAT_SOUND_HEAD + nlen + TLE_LINE_LEN, line2, TLE_LINE_LEN);
	if (with) {
		uint8_t *o = out + SAT_SOUND_HEAD + nlen + 2u * TLE_LINE_LEN;
		put32(o, p->up_hz);
		put32(o + 4, p->down_hz);
		put16(o + 8, p->up_tone);
		put16(o + 10, p->arm_tone);
		put16(o + 12, p->arm_ms);
		put16(o + 14, p->armed_s);
	}
	return total;
}

sat_sound_err_t sat_sound_parse(const uint8_t *f, size_t n, sat_sound_rec_t *r)
{
	if (n < SAT_SOUND_HEAD) {
		return SAT_SOUND_ERR_SHORT;
	}
	if (f[0] != SAT_SOUND_MAGIC) {
		return SAT_SOUND_ERR_MAGIC;
	}
	if (f[1] != SAT_SOUND_VERSION && f[1] != SAT_SOUND_VERSION_PROF) {
		return SAT_SOUND_ERR_VERSION;
	}
	size_t extra = f[1] == SAT_SOUND_VERSION_PROF ? SAT_SOUND_PROF_LEN : 0u;
	uint8_t count = f[4], index = f[3];
	if (count == 0 || count > SAT_SOUND_SETS_MAX || index >= count) {
		return SAT_SOUND_ERR_INDEX;
	}
	size_t nlen = f[5];
	if (!name_ok(f + SAT_SOUND_HEAD, nlen <= n - SAT_SOUND_HEAD ? nlen : SAT_SOUND_NAME_MAX + 1u)) {
		return SAT_SOUND_ERR_NAME;
	}
	if (n != SAT_SOUND_HEAD + nlen + 2u * TLE_LINE_LEN + extra) {
		return SAT_SOUND_ERR_LENGTH;
	}
	// Straight into r (a temporary of this size on the radio task's stack would double it): r means something only on OK
	memset(r, 0, sizeof(*r));
	r->id = f[2];
	r->index = index;
	r->count = count;
	memcpy(r->name, f + SAT_SOUND_HEAD, nlen);
	memcpy(r->line1, f + SAT_SOUND_HEAD + nlen, TLE_LINE_LEN);
	memcpy(r->line2, f + SAT_SOUND_HEAD + nlen + TLE_LINE_LEN, TLE_LINE_LEN);
	if (tle_parse(r->line1, r->line2, &r->el) != TLE_OK)
		return SAT_SOUND_ERR_TLE;
	if (extra != 0) {
		const uint8_t *o = f + SAT_SOUND_HEAD + nlen + 2u * TLE_LINE_LEN;
		r->prof.up_hz = get32(o);
		r->prof.down_hz = get32(o + 4);
		r->prof.up_tone = get16(o + 8);
		r->prof.arm_tone = get16(o + 10);
		r->prof.arm_ms = get16(o + 12);
		r->prof.armed_s = get16(o + 14);
		if (!prof_ok(&r->prof))
			return SAT_SOUND_ERR_PROFILE;
		r->has_prof = !prof_zero(&r->prof);
	}
	return SAT_SOUND_OK;
}

void sat_sound_rx_init(sat_sound_rx_t *rx)
{
	memset(rx, 0, sizeof(*rx));
}

sat_sound_rx_res_t sat_sound_rx_frame(sat_sound_rx_t *rx, const uint8_t *frame, size_t n, sat_sound_rec_t *r)
{
	if (sat_sound_parse(frame, n, r) != SAT_SOUND_OK) {
		rx->bad++;
		return SAT_SOUND_RX_BAD;
	}
	if (!rx->active || r->id != rx->id || r->count != rx->count) {
		uint16_t bad = rx->bad;
		memset(rx, 0, sizeof(*rx));
		rx->bad = bad;
		rx->active = true;
		rx->id = r->id;
		rx->count = r->count;
	}
	uint32_t bit = 1u << r->index;
	if ((rx->seen & bit) != 0) {
		rx->dups++;
		return SAT_SOUND_RX_DUP;
	}
	rx->seen |= bit;
	rx->heard++;
	return SAT_SOUND_RX_NEW;
}

void sat_sound_rx_forget(sat_sound_rx_t *rx, uint8_t index)
{
	uint32_t bit = index < SAT_SOUND_SETS_MAX ? 1u << index : 0u;
	if ((rx->seen & bit) != 0) {
		rx->seen &= ~bit;
		rx->heard--;
	}
}

bool sat_sound_rx_done(const sat_sound_rx_t *rx)
{
	return rx->active && rx->heard == rx->count;
}
