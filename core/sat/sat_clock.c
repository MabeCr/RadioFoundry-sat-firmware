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

#define UNIX_2026	1767225600u	// 2026-01-01 00:00:00 UTC: no firmware of ours ran before it

bool sat_clock_from_rtc(uint16_t day, uint8_t h, uint8_t m, uint8_t s, uint32_t *unix_s)
{
	if (h > 23 || m > 59 || s > 59 || day < SAT_RTC_DAY_BASE)
		return false;
	uint32_t d = (uint32_t)(day - SAT_RTC_DAY_BASE);
	uint32_t t = d * 86400u + h * 3600u + m * 60u + s;
	if (t < UNIX_2026 || d >= 32768u - 1u)
		return false;
	*unix_s = t;
	return true;
}

void sat_clock_to_rtc(uint32_t unix_s, uint16_t *day, uint8_t *h, uint8_t *m, uint8_t *s)
{
	uint32_t r = unix_s % 86400u;
	*day = (uint16_t)(unix_s / 86400u + SAT_RTC_DAY_BASE);
	*h = (uint8_t)(r / 3600u);
	*m = (uint8_t)(r / 60u % 60u);
	*s = (uint8_t)(r % 60u);
}

// Howard Hinnant's civil_from_days
void sat_clock_civil(uint32_t unix_s, uint16_t *year, uint8_t *mon, uint8_t *day, uint8_t *h, uint8_t *m, uint8_t *s)
{
	uint32_t r = unix_s % 86400u;
	int32_t z = (int32_t)(unix_s / 86400u) + 719468;
	int32_t era = z / 146097;
	uint32_t doe = (uint32_t)(z - era * 146097);
	uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
	int32_t y = (int32_t)yoe + era * 400;
	uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
	uint32_t mp = (5u * doy + 2u) / 153u;
	uint32_t d = doy - (153u * mp + 2u) / 5u + 1u;
	uint32_t mo = mp < 10u ? mp + 3u : mp - 9u;
	*year = (uint16_t)(y + (mo <= 2u));
	*mon = (uint8_t)mo;
	*day = (uint8_t)d;
	*h = (uint8_t)(r / 3600u);
	*m = (uint8_t)(r / 60u % 60u);
	*s = (uint8_t)(r % 60u);
}

uint32_t sat_clock_from_civil(unsigned year, unsigned mon, unsigned day, unsigned h, unsigned m, unsigned s)
{
	static const uint8_t mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	if (year < 2026 || year > 2099 || mon < 1 || mon > 12 || day < 1 || h > 23 || m > 59 || s > 59)
		return 0;
	if (day > (unsigned)mdays[mon - 1] + (mon == 2u && year % 4u == 0u ? 1u : 0u))
		return 0;
	return (uint32_t)sat_days_from_civil((int)year, mon, day) * 86400u + h * 3600u + m * 60u + s;
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
	unsigned tries = 0;
	do {
		seq = c->seq;
		s = c->base_s;
		ms = c->base_ms;
	} while (((seq & 1u) != 0 || seq != c->seq) && ++tries < 50u);
	// A write under way for this long is one a task above this one preempted: no time for now, rather than waiting on
	// a write that can't finish while this reader spins
	if (s == 0 || tries >= 50u)
		return false;
	*unix_s = (double)s + (double)(now_ms - ms) / 1000.0;
	return true;
}
