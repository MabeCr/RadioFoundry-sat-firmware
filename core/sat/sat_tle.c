// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Two-line element sets (sat_tle.h, docs/spec/satellite.md 2). Fields are read by column, as the format defines them;
// numbers go through a small fixed-point reader instead of strtod, which would bring the C library's float parser
#include "sat/sat_tle.h"

// A field's columns, 1-based and inclusive as the format's tables give them
#define COL(line, first)	((line) + (first) - 1)
#define LEN(first, last)	((last) - (first) + 1)

uint8_t tle_checksum(const char *line)
{
	unsigned sum = 0;
	for (int i = 0; i < TLE_LINE_LEN - 1; i++) {
		char c = line[i];
		if (c >= '0' && c <= '9')
			sum += (unsigned)(c - '0');
		else if (c == '-')
			sum++;
	}
	return (uint8_t)(sum % 10u);
}

static const double pow10_tab[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14,
	1e15, 1e16, 1e17, 1e18 };

// A decimal in a fixed field: blanks around it, an optional sign, digits with at most one point. implied_point: the
// field has no point and its digits are a fraction (the eccentricity's "0001234" is 0.0001234)
static bool read_fixed(const char *s, int n, bool implied_point, double *out)
{
	int i = 0;
	while (i < n && s[i] == ' ')
		i++;
	bool neg = false;
	if (i < n && (s[i] == '-' || s[i] == '+'))
		neg = s[i++] == '-';
	uint64_t v = 0;
	int digits = 0, frac = -1;	// frac: digits after the point, -1 before it
	for (; i < n && s[i] != ' '; i++) {
		if (s[i] == '.' && frac < 0 && !implied_point) {
			frac = 0;
			continue;
		}
		if (s[i] < '0' || s[i] > '9' || digits >= 18)
			return false;
		v = v * 10u + (uint64_t)(s[i] - '0');
		digits++;
		if (frac >= 0)
			frac++;
	}
	for (; i < n; i++)
		if (s[i] != ' ')
			return false;
	if (digits == 0)
		return false;
	if (implied_point)
		frac = digits;
	double d = (double)v;
	if (frac > 0)
		d /= pow10_tab[frac];
	*out = neg ? -d : d;
	return true;
}

// The exponent form of the drag term and the second derivative: " 12345-3" is 0.12345e-3, a sign in the first column
static bool read_exp(const char *s, double *out)
{
	double mant;
	if (!read_fixed(s + 1, 5, true, &mant) || (s[0] != ' ' && s[0] != '+' && s[0] != '-'))
		return false;
	if ((s[6] != '+' && s[6] != '-' && s[6] != ' ') || s[7] < '0' || s[7] > '9')
		return false;
	double e = pow10_tab[s[7] - '0'];
	mant = s[6] == '-' ? mant / e : mant * e;
	*out = s[0] == '-' ? -mant : mant;
	return true;
}

// The catalog number: five digits, or Alpha-5, a letter for the leading two digits (A = 10 .. Z = 33, without I and O)
static bool read_catalog(const char *s, uint32_t *out)
{
	uint32_t lead;
	char c = s[0];
	if (c >= '0' && c <= '9') {
		lead = (uint32_t)(c - '0');
	} else if (c >= 'A' && c <= 'Z' && c != 'I' && c != 'O') {
		lead = 10u + (uint32_t)(c - 'A') - (c > 'I') - (c > 'O');
	} else if (c == ' ') {
		lead = 0;
	} else {
		return false;
	}
	uint32_t v = 0;
	for (int i = 1; i < 5; i++) {
		if (s[i] < '0' || s[i] > '9')
			return false;
		v = v * 10u + (uint32_t)(s[i] - '0');
	}
	*out = lead * 10000u + v;
	return true;
}

int32_t sat_days_from_civil(int year, unsigned month, unsigned day)
{
	// Day count from 1 March of year 0, so the leap day falls at the end of the year
	int y = year - (month <= 2);
	int era = y / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153u * (month > 2 ? month - 3u : month + 9u) + 2u) / 5u + day - 1u;
	unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
	return era * 146097 + (int32_t)doe - 719468;
}

double tle_epoch_unix(unsigned yy, double doy)
{
	int year = yy < 57 ? 2000 + (int)yy : 1900 + (int)yy;
	return ((double)sat_days_from_civil(year, 1, 1) + doy - 1.0) * 86400.0;
}

// A line's length to its last non-blank (CR, LF and spaces after column 69 are allowed)
static bool line_ok(const char *l, char number)
{
	size_t n = 0;
	while (n < TLE_LINE_LEN && l[n] != '\0' && l[n] != '\r' && l[n] != '\n')
		n++;
	return n == TLE_LINE_LEN && l[0] == number && l[1] == ' ';
}

tle_err_t tle_parse(const char *line1, const char *line2, sat_elements_t *e)
{
	if (!line_ok(line1, '1') || !line_ok(line2, '2'))
		return TLE_ERR_LENGTH;
	if (tle_checksum(line1) != (uint8_t)(line1[68] - '0') || tle_checksum(line2) != (uint8_t)(line2[68] - '0'))
		return TLE_ERR_CHECKSUM;

	sat_elements_t t;
	uint32_t n2;
	double yy, doy;
	if (!read_catalog(COL(line1, 3), &t.norad) || !read_catalog(COL(line2, 3), &n2))
		return TLE_ERR_FIELD;
	if (t.norad != n2)
		return TLE_ERR_MISMATCH;
	if (!read_fixed(COL(line1, 19), LEN(19, 20), false, &yy) || !read_fixed(COL(line1, 21), LEN(21, 32), false, &doy) ||
	    !read_exp(COL(line1, 54), &t.bstar) || yy < 0.0 || doy < 1.0 || doy >= 367.0)
		return TLE_ERR_FIELD;
	if (!read_fixed(COL(line2, 9), LEN(9, 16), false, &t.inclo) || !read_fixed(COL(line2, 18), LEN(18, 25), false, &t.nodeo) ||
	    !read_fixed(COL(line2, 27), LEN(27, 33), true, &t.ecco) || !read_fixed(COL(line2, 35), LEN(35, 42), false, &t.argpo) ||
	    !read_fixed(COL(line2, 44), LEN(44, 51), false, &t.mo) || !read_fixed(COL(line2, 53), LEN(53, 63), false, &t.no))
		return TLE_ERR_FIELD;
	if (t.no <= 0.0 || t.inclo < 0.0 || t.inclo > 180.0)
		return TLE_ERR_FIELD;
	t.epoch = tle_epoch_unix((unsigned)yy, doy);
	*e = t;
	return TLE_OK;
}
