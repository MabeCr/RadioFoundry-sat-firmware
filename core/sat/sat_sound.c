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

size_t sat_sound_build(uint8_t *out, size_t max, uint8_t id, uint8_t index, uint8_t count, const char *name,
		       const char *line1, const char *line2)
{
	size_t nlen = name != NULL ? strlen(name) : 0;
	sat_elements_t e;

	if (count == 0 || count > SAT_SOUND_SETS_MAX || index >= count || !name_ok((const uint8_t *)name, nlen)) {
		return 0;
	}
	if (line1 == NULL || line2 == NULL || strlen(line1) != TLE_LINE_LEN || strlen(line2) != TLE_LINE_LEN ||
	    tle_parse(line1, line2, &e) != TLE_OK) {
		return 0;
	}
	size_t total = SAT_SOUND_HEAD + nlen + 2u * TLE_LINE_LEN;
	if (out == NULL || max < total) {
		return 0;
	}
	out[0] = SAT_SOUND_MAGIC;
	out[1] = SAT_SOUND_VERSION;
	out[2] = id;
	out[3] = index;
	out[4] = count;
	out[5] = (uint8_t)nlen;
	memcpy(out + SAT_SOUND_HEAD, name, nlen);
	memcpy(out + SAT_SOUND_HEAD + nlen, line1, TLE_LINE_LEN);
	memcpy(out + SAT_SOUND_HEAD + nlen + TLE_LINE_LEN, line2, TLE_LINE_LEN);
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
	if (f[1] != SAT_SOUND_VERSION) {
		return SAT_SOUND_ERR_VERSION;
	}
	uint8_t count = f[4], index = f[3];
	if (count == 0 || count > SAT_SOUND_SETS_MAX || index >= count) {
		return SAT_SOUND_ERR_INDEX;
	}
	size_t nlen = f[5];
	if (!name_ok(f + SAT_SOUND_HEAD, nlen <= n - SAT_SOUND_HEAD ? nlen : SAT_SOUND_NAME_MAX + 1u)) {
		return SAT_SOUND_ERR_NAME;
	}
	if (n != SAT_SOUND_HEAD + nlen + 2u * TLE_LINE_LEN) {
		return SAT_SOUND_ERR_LENGTH;
	}
	sat_sound_rec_t t;
	memset(&t, 0, sizeof(t));
	t.id = f[2];
	t.index = index;
	t.count = count;
	memcpy(t.name, f + SAT_SOUND_HEAD, nlen);
	memcpy(t.line1, f + SAT_SOUND_HEAD + nlen, TLE_LINE_LEN);
	memcpy(t.line2, f + SAT_SOUND_HEAD + nlen + TLE_LINE_LEN, TLE_LINE_LEN);
	if (tle_parse(t.line1, t.line2, &t.el) != TLE_OK) {
		return SAT_SOUND_ERR_TLE;
	}
	*r = t;
	return SAT_SOUND_OK;
}

void sat_sound_rx_init(sat_sound_rx_t *rx)
{
	memset(rx, 0, sizeof(*rx));
}

sat_sound_rx_res_t sat_sound_rx_frame(sat_sound_rx_t *rx, const uint8_t *frame, size_t n, sat_sound_rec_t *r)
{
	sat_sound_rec_t t;

	if (sat_sound_parse(frame, n, &t) != SAT_SOUND_OK) {
		rx->bad++;
		return SAT_SOUND_RX_BAD;
	}
	if (!rx->active || t.id != rx->id || t.count != rx->count) {
		uint16_t bad = rx->bad;
		memset(rx, 0, sizeof(*rx));
		rx->bad = bad;
		rx->active = true;
		rx->id = t.id;
		rx->count = t.count;
	}
	*r = t;
	uint32_t bit = 1u << t.index;
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
