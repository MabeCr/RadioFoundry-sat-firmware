// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Sine and cosine for the orbit model (docs/spec/satellite.md 3.2): the C library's handle any argument, with an
// argument reduction for huge ones that costs 3.4 KB of flash. The model's angles stay under a few thousand radians,
// so a three-part reduction by pi/2 is exact to the last bit there, and Taylor series on +-pi/4 converge past double
// precision. Good to 1 ulp or so for |x| < 2^20 (about a million radians); beyond that the result loses digits.
#ifndef SAT_MATH_H
#define SAT_MATH_H

void sat_sincos(double x, double *s, double *c);
double sat_sin(double x);
double sat_cos(double x);

#endif
