// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Sine and cosine (sat_math.h)
#include <stdint.h>
#include "sat/sat_math.h"

// pi/2 in three parts of 30 significant bits or fewer (worked out from pi to 80 digits): n x P1 and n x P2 are exact
// for |n| < 2^23, so x - n pi/2 keeps its digits
#define PIO2_1		0x1.921fb54000000p+0
#define PIO2_2		0x1.10b4611800000p-30
#define PIO2_3		0x1.313198a2e0370p-61
#define TWO_OVER_PI	0.63661977236758134308

// On |r| <= pi/4: the series to r^15 / 15! (sine) and r^16 / 16! (cosine), whose next terms are below 5e-17 there
static double sin_r(double r)
{
	double r2 = r * r;
	return r + r * r2 * (-1.0 / 6 + r2 * (1.0 / 120 + r2 * (-1.0 / 5040 + r2 * (1.0 / 362880 + r2 * (-1.0 / 39916800 +
		r2 * (1.0 / 6227020800.0 + r2 * (-1.0 / 1307674368000.0)))))));
}

static double cos_r(double r)
{
	double r2 = r * r;
	return 1.0 + r2 * (-0.5 + r2 * (1.0 / 24 + r2 * (-1.0 / 720 + r2 * (1.0 / 40320 + r2 * (-1.0 / 3628800 +
		r2 * (1.0 / 479001600.0 + r2 * (-1.0 / 87178291200.0 + r2 * (1.0 / 20922789888000.0))))))));
}

void sat_sincos(double x, double *s, double *c)
{
	double fn = x * TWO_OVER_PI;
	int32_t n = (int32_t)(fn < 0.0 ? fn - 0.5 : fn + 0.5);
	double dn = (double)n;
	double r = ((x - dn * PIO2_1) - dn * PIO2_2) - dn * PIO2_3;
	double sr = sin_r(r), cr = cos_r(r);
	switch (n & 3) {
	case 0: *s = sr; *c = cr; break;
	case 1: *s = cr; *c = -sr; break;
	case 2: *s = -sr; *c = -cr; break;
	default: *s = -cr; *c = sr; break;
	}
}

double sat_sin(double x)
{
	double s, c;
	sat_sincos(x, &s, &c);
	return s;
}

double sat_cos(double x)
{
	double s, c;
	sat_sincos(x, &s, &c);
	return c;
}
