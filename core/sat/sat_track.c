// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// One tracked satellite (sat_track.h)
#include <stddef.h>
#include "sat/sat_track.h"

bool sat_track_set(sat_track_t *t, const sat_elements_t *e, const sat_profile_t *profile, uint8_t *sgp4)
{
	sgp4_t m;
	*sgp4 = (uint8_t)sgp4_init(&m, e);
	if (*sgp4 != SGP4_OK)
		return false;
	t->model = m;
	t->elements = *e;
	t->norad = e->norad;
	t->loaded = true;
	if (profile == NULL)
		profile = sat_profile_find(e->norad);
	t->has_profile = profile != NULL;
	if (profile != NULL)
		t->profile = *profile;
	return true;
}

bool sat_track_load(sat_track_t *t, const char *line1, const char *line2, uint8_t *tle, uint8_t *sgp4)
{
	sat_elements_t e;
	*tle = (uint8_t)tle_parse(line1, line2, &e);
	*sgp4 = SGP4_OK;
	return *tle == TLE_OK && sat_track_set(t, &e, NULL, sgp4);
}

void sat_track_observer(sat_track_t *t, double lat_deg, double lon_deg, double alt_m)
{
	sat_observer_set(&t->observer, lat_deg, lon_deg, alt_m);
	t->observer_set = true;
}

bool sat_track_at(const sat_track_t *t, double unix_s, sat_track_now_t *out)
{
	if (!t->loaded || !t->observer_set || !sat_look_at(&t->model, &t->observer, unix_s, &out->look))
		return false;
	// A profile without a frequency (a stored satellite not set up yet) gives 0 for it
	out->rx_hz = t->has_profile && t->profile.down_hz ? sat_doppler_rx_hz(t->profile.down_hz, out->look.range_rate) : 0;
	out->tx_hz = t->has_profile && t->profile.up_hz ? sat_doppler_tx_hz(t->profile.up_hz, out->look.range_rate) : 0;
	return true;
}
