// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The receiver's signal level over a satellite pass, for the Tracking screen's Chart view (docs/spec/ui.md 2.6): a reading
// every few seconds, kept in bins by time, so a stretch with no readings (the user was on another screen) stays empty
// instead of the line closing over it. No hardware, no clock of its own and no RTOS: the caller gives the time.
#ifndef SAT_SIG_H
#define SAT_SIG_H

#include <stdint.h>
#include <stdbool.h>

#define SAT_SIG_STEP_S	2u	// seconds a bin covers
#define SAT_SIG_PAD_S	30u	// kept before the rise and after the set
#define SAT_SIG_MAX	512u	// bins: a pass of up to about 16 minutes with its padding (512 bytes)

typedef struct {
	uint32_t aos, los;	// the pass the readings belong to (UTC)
	uint32_t t0;		// UTC of the first bin
	uint16_t n;		// bins in use
	uint8_t v[SAT_SIG_MAX];	// 0: nothing read; else dBm + 141, 1 to 255 (-140 dBm or less to +114)
} sat_sig_t;

typedef struct {
	bool any;
	uint16_t samples;	// bins with a reading
	int16_t lo, hi;		// the lowest and the highest, dBm
	uint32_t hi_utc;	// when the highest was read (the start of its bin)
} sat_sig_stats_t;

// Start over for a pass (clears everything)
void sat_sig_reset(sat_sig_t *s, uint32_t aos, uint32_t los);
// Is this the pass the readings are of
bool sat_sig_is(const sat_sig_t *s, uint32_t aos, uint32_t los);
// A reading at utc: the bin keeps the highest it has been given (a short fade then doesn't punch a hole). False when
// utc is outside the window or the clock is not set (utc 0)
bool sat_sig_add(sat_sig_t *s, uint32_t utc, int16_t dbm);
bool sat_sig_has(const sat_sig_t *s, uint16_t i);
int16_t sat_sig_dbm(const sat_sig_t *s, uint16_t i);
uint32_t sat_sig_bin_utc(const sat_sig_t *s, uint16_t i);
void sat_sig_stats(const sat_sig_t *s, sat_sig_stats_t *st);
// The levels the chart's vertical axis spans for those readings: a 5 dB step above and below, at least 20 dB between,
// kept within -140 to -30 dBm. With none read, a default range
void sat_sig_range(const sat_sig_stats_t *st, int16_t *lo, int16_t *hi);

#endif
