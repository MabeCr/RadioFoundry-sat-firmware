// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Where a satellite is seen from the ground (docs/spec/satellite.md 4): the SGP4 state turned from TEME to earth-fixed
// by sidereal time, then to azimuth, elevation, range and range rate from an observer on the WGS-84 ellipsoid. Polar
// motion and UT1 - UTC are left out: together under 0.01 degree, far inside a handheld antenna's beam. The Doppler
// shift follows from the range rate. No hardware or RTOS here.
#ifndef SAT_LOOK_H
#define SAT_LOOK_H

#include <stdint.h>
#include <stdbool.h>
#include "sat/sat_doppler.h"
#include "sat/sgp4.h"

typedef struct {
	double lat, lon;	// geodetic, radians (east positive)
	double alt;		// km above the ellipsoid
	double r[3];		// earth-fixed position, km (sat_observer_set)
	double sin_lat, cos_lat, sin_lon, cos_lon;
} sat_observer_t;

typedef struct {
	double az;		// degrees from north, clockwise, 0..360
	double el;		// degrees above the horizon
	double range;		// km
	double range_rate;	// km/s, positive while the satellite moves away
} sat_look_t;

// Greenwich mean sidereal time (IAU 1982), radians, at a Unix time taken as UT1
double sat_gmst(double unix_s);
void sat_observer_set(sat_observer_t *o, double lat_deg, double lon_deg, double alt_m);
// The look angles of a TEME state at a Unix time
void sat_look(const sat_observer_t *o, const sgp4_state_t *st, double unix_s, sat_look_t *out);
// Propagate and look in one step: false when SGP4 can't give a state (decayed, elements out of range)
bool sat_look_at(const sgp4_t *s, const sat_observer_t *o, double unix_s, sat_look_t *out);

// The center of a 4- or 6-character Maidenhead locator ("FN31", "FN31pr"), case-insensitive, in degrees. False for a
// malformed locator
bool sat_locator_to_deg(const char *loc, double *lat_deg, double *lon_deg);

#endif
