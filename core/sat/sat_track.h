// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// One tracked satellite (docs/spec/satellite.md 7): its model, the observer and its profile, and what they give at a
// time: the look angles and the Doppler-corrected frequencies. The pieces a pass list or a live screen needs, and what
// the debug link's SAT command drives (docs/spec/debuglink.md 3.24). No hardware or RTOS here; about 0.5 KB
#ifndef SAT_TRACK_H
#define SAT_TRACK_H

#include <stdint.h>
#include <stdbool.h>
#include "sat/sat_look.h"
#include "sat/sat_profile.h"

typedef struct {
	sgp4_t model;
	uint32_t norad;
	bool loaded;
	bool observer_set;
	sat_observer_t observer;
	const sat_profile_t *profile;	// the built-in profile for norad, NULL for none
} sat_track_t;

typedef struct {
	sat_look_t look;
	uint32_t rx_hz, tx_hz;		// the profile's downlink and uplink with Doppler, 0 without a profile
} sat_track_now_t;

// Two TLE lines (each read to column 69). On failure nothing changes, and tle / sgp4 say why (TLE_OK / SGP4_OK for
// the stage that passed)
bool sat_track_load(sat_track_t *t, const char *line1, const char *line2, uint8_t *tle, uint8_t *sgp4);
void sat_track_observer(sat_track_t *t, double lat_deg, double lon_deg, double alt_m);
// Where the satellite is at unix_s; false when nothing is loaded, there is no observer, or the model fails
bool sat_track_at(const sat_track_t *t, double unix_s, sat_track_now_t *out);

#endif
