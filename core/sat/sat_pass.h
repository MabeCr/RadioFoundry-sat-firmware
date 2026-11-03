// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Pass prediction (docs/spec/satellite.md 5): when a satellite rises above the observer's horizon, when it is highest
// and when it sets. A search with SGP4 alone: steps sized to how far below the horizon the satellite is, then
// bisection to the second at rise and set. No hardware or RTOS here; the caller spreads the work over time.
#ifndef SAT_PASS_H
#define SAT_PASS_H

#include <stdbool.h>
#include "sat/sat_look.h"

typedef struct {
	double aos;		// rise (acquisition of signal), Unix seconds
	double tca;		// highest point (time of closest approach)
	double los;		// set (loss of signal)
	float max_el;		// degrees at tca
	float aos_az, los_az;	// degrees at rise and at set
} sat_pass_t;

// The first pass that hasn't set by `from` (one in progress counts, its aos then lies before from) and rises within
// span seconds, with a highest point of at least min_el degrees; lower passes are skipped. False for none, or when
// SGP4 fails along the way. evals, when not NULL, gets the number of SGP4 runs it took (for the cost on the radio)
bool sat_pass_next(const sgp4_t *s, const sat_observer_t *o, double from, double span, float min_el, sat_pass_t *out,
	unsigned *evals);

#endif
