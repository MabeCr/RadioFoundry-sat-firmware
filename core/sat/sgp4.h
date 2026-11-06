// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SGP4, the near-earth orbit model the two-line elements are fitted to (docs/spec/satellite.md 3), written from the
// published equations: Hoots and Roehrich, Spacetrack Report No. 3 (1980), with the corrections of Vallado, Crawford,
// Hujsak and Kelso, "Revisiting Spacetrack Report #3" (AIAA 2006-6753). WGS-72 constants, as the elements are fitted
// with. Near-earth only: an orbit of 225 minutes or longer needs the deep-space terms (SDP4), which this leaves out,
// and is refused. Every amateur FM satellite is in low earth orbit. Double precision, no state beyond sgp4_t.
#ifndef SAT_SGP4_H
#define SAT_SGP4_H

#include <stdbool.h>
#include "sat/sat_tle.h"

typedef enum {
	SGP4_OK = 0,
	SGP4_ERR_ECC,		// eccentricity out of 0..1 (an element set or a propagation past its useful life)
	SGP4_ERR_MEAN_MOTION,	// mean motion not positive
	SGP4_ERR_SEMI_LATUS,	// semi-latus rectum negative
	SGP4_ERR_DECAYED,	// below the earth's surface
	SGP4_ERR_DEEP_SPACE,	// period of 225 minutes or more: SDP4's orbit, not handled
} sgp4_err_t;

// The model's constants for one element set, worked out once (sgp4_init). 46 doubles
typedef struct {
	double epoch;		// Unix seconds
	double bstar, inclo, nodeo, argpo, mo, ecco, no;	// radians, radians a minute (no: un-Kozai'd)
	double aycof, con41, cc1, cc4, cc5, d2, d3, d4, delmo, eta, argpdot, omgcof, sinmao, t2cof, t3cof, t4cof,
	       t5cof, x1mth2, x7thm1, mdot, nodedot, xlcof, xmcof, nodecf, cosio, sinio;
	bool isimp;		// perigee below 220 km: the simplified drag terms
} sgp4_t;

// Position and velocity in the TEME frame (true equator, mean equinox of date), km and km/s
typedef struct {
	double r[3];
	double v[3];
} sgp4_state_t;

sgp4_err_t sgp4_init(sgp4_t *s, const sat_elements_t *e);
// The state tsince minutes after the elements' epoch (negative: before it)
sgp4_err_t sgp4_propagate(const sgp4_t *s, double tsince, sgp4_state_t *out);

#endif
