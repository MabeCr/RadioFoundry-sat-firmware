// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Two-line element sets (docs/spec/satellite.md 2): the 69-column NORAD format, both lines' modulo-10 check digits
// required. Only what SGP4 reads is kept (sat_elements_t, 64 bytes instead of the 140 of text), so a stored set is a
// struct, not the lines. No hardware or RTOS here.
#ifndef SAT_TLE_H
#define SAT_TLE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define TLE_LINE_LEN	69

// The mean elements at epoch, in the units the TLE gives them
typedef struct {
	double epoch;		// Unix time (UTC seconds) of the elements
	double bstar;		// drag term, 1 / earth radii
	double inclo;		// inclination, degrees
	double nodeo;		// right ascension of the ascending node, degrees
	double argpo;		// argument of perigee, degrees
	double mo;		// mean anomaly, degrees
	double ecco;		// eccentricity
	double no;		// mean motion, revolutions a day (Kozai)
	uint32_t norad;		// catalog number (5 digits; Alpha-5 "A0000" counts on from 100000)
} sat_elements_t;

typedef enum {
	TLE_OK = 0,
	TLE_ERR_LENGTH,		// a line shorter than 69 columns, or the wrong line number in column 1
	TLE_ERR_CHECKSUM,	// column 69 isn't the line's check digit
	TLE_ERR_FIELD,		// a field that doesn't parse
	TLE_ERR_MISMATCH,	// the two lines name different satellites
} tle_err_t;

// A line's check digit: the sum of its digits, plus 1 for each '-', modulo 10, over columns 1-68
uint8_t tle_checksum(const char *line);
// Both lines (trailing CR / LF or spaces allowed, nothing before column 1). Fills e only on TLE_OK
tle_err_t tle_parse(const char *line1, const char *line2, sat_elements_t *e);
// Unix seconds of a TLE epoch: two-digit year (57-99 = 1957-1999, 00-56 = 2000-2056) and day of year (1.0 = 1 Jan 0h)
double tle_epoch_unix(unsigned yy, double doy);
// Days from 1970-01-01 to a civil date (proleptic Gregorian, 1970 <= year)
int32_t sat_days_from_civil(int year, unsigned month, unsigned day);

#endif
