// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UTC for satellite tracking (docs/spec/satellite.md 4.1): the firmware reads no real-time clock yet, so the date and
// time of the last NMEA RMC sentence (a board GPS, or a phone or PC feeding one over the host link) stamped with the
// millisecond clock when it arrived. RMC gives whole seconds, sent a fraction of a second after the second they name,
// so the time is up to about a second late. Lock-free: one writer (the NMEA path), readers in any task. No RTOS here.
#ifndef SAT_CLOCK_H
#define SAT_CLOCK_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	volatile uint32_t seq;		// odd while the writer is between its two halves
	volatile uint32_t base_s;	// Unix seconds of the last sentence, 0 none yet
	volatile uint32_t base_ms;	// the millisecond clock when it arrived
} sat_clock_t;

// RMC's date (ddmmyy, two-digit year = 2000 + yy) and time (hhmmss) to Unix seconds; 0 when either is malformed
uint32_t sat_clock_unix_from_nmea(uint32_t ddmmyy, uint32_t hhmmss);
// A sentence's date and time, received at now_ms. False (nothing changes) when either is unknown or malformed
bool sat_clock_set_nmea(sat_clock_t *c, uint32_t ddmmyy, uint32_t hhmmss, uint32_t now_ms);
// Set from a Unix time received at now_ms (the debug link, for a bench without a fix); the next sentence overrides it.
// False for 0
bool sat_clock_set_unix(sat_clock_t *c, uint32_t unix_s, uint32_t now_ms);
// UTC now in Unix seconds; false before the first sentence. The millisecond clock wraps after 49 days: a sentence at
// least that often keeps it right, and a GPS sends one a second
bool sat_clock_now(const sat_clock_t *c, uint32_t now_ms, double *unix_s);

// The radio's clock, which the NMEA path sets (core/aprs/aprs.c)
extern sat_clock_t sat_utc;

#endif
