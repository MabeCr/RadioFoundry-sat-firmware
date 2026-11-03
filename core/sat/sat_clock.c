// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UTC from NMEA (sat_clock.h)
#include "sat/sat_clock.h"
#include "sat/sat_tle.h"

sat_clock_t sat_utc;

uint32_t sat_clock_unix_from_nmea(uint32_t ddmmyy, uint32_t hhmmss)
{
	unsigned d = ddmmyy / 10000u, mo = ddmmyy / 100u % 100u, y = ddmmyy % 100u;
	unsigned h = hhmmss / 10000u, mi = hhmmss / 100u % 100u, s = hhmmss % 100u;
	static const uint8_t mdays[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	if (mo < 1 || mo > 12 || d < 1 || d > mdays[mo - 1] || ddmmyy > 311299u)
		return 0;
	// A leap second (60) is taken as the next second's start
	if (h > 23 || mi > 59 || s > 60 || hhmmss > 235960u)
		return 0;
	if (mo == 2 && d == 29 && y % 4u != 0)
		return 0;
	int32_t days = sat_days_from_civil(2000 + (int)y, mo, d);
	return (uint32_t)days * 86400u + h * 3600u + mi * 60u + s;
}

bool sat_clock_set_unix(sat_clock_t *c, uint32_t unix_s, uint32_t now_ms)
{
	if (unix_s == 0)
		return false;
	c->seq++;
	c->base_s = unix_s;
	c->base_ms = now_ms;
	c->seq++;
	return true;
}

bool sat_clock_set_nmea(sat_clock_t *c, uint32_t ddmmyy, uint32_t hhmmss, uint32_t now_ms)
{
	if (ddmmyy == 0 || hhmmss == 0xFFFFFFFFu)
		return false;
	return sat_clock_set_unix(c, sat_clock_unix_from_nmea(ddmmyy, hhmmss), now_ms);
}

bool sat_clock_now(const sat_clock_t *c, uint32_t now_ms, double *unix_s)
{
	uint32_t seq, s, ms;
	do {
		seq = c->seq;
		s = c->base_s;
		ms = c->base_ms;
	} while ((seq & 1u) != 0 || seq != c->seq);
	if (s == 0)
		return false;
	*unix_s = (double)s + (double)(now_ms - ms) / 1000.0;
	return true;
}
