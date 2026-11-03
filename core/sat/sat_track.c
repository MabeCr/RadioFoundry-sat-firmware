// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// One tracked satellite (sat_track.h)
#include "sat/sat_track.h"

bool sat_track_load(sat_track_t *t, const char *line1, const char *line2, uint8_t *tle, uint8_t *sgp4)
{
	sat_elements_t e;
	*tle = (uint8_t)tle_parse(line1, line2, &e);
	*sgp4 = SGP4_OK;
	if (*tle != TLE_OK)
		return false;
	sgp4_t m;
	*sgp4 = (uint8_t)sgp4_init(&m, &e);
	if (*sgp4 != SGP4_OK)
		return false;
	t->model = m;
	t->norad = e.norad;
	t->loaded = true;
	t->profile = sat_profile_find(e.norad);
	return true;
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
	out->rx_hz = t->profile ? sat_doppler_rx_hz(t->profile->down_hz, out->look.range_rate) : 0;
	out->tx_hz = t->profile ? sat_doppler_tx_hz(t->profile->up_hz, out->look.range_rate) : 0;
	return true;
}
