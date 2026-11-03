// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Look angles, Doppler and locators (sat_look.h)
#include <math.h>
#include "sat/sat_look.h"
#include "sat/sat_math.h"

#define PI		3.14159265358979323846
#define TWOPI		(2.0 * PI)
#define DEG2RAD		(PI / 180.0)
#define RAD2DEG		(180.0 / PI)

// WGS-84 ellipsoid, for the observer
#define WGS84_A		6378.137
#define WGS84_F		(1.0 / 298.257223563)
// The earth's rotation, rad/s
#define OMEGA_E		7.292115e-5
// 2000-01-01 12:00 UTC (J2000.0)
#define UNIX_J2000	946728000.0

double sat_gmst(double unix_s)
{
	double tut1 = (unix_s - UNIX_J2000) / (86400.0 * 36525.0);
	double sec = -6.2e-6 * tut1 * tut1 * tut1 + 0.093104 * tut1 * tut1 +
		(876600.0 * 3600.0 + 8640184.812866) * tut1 + 67310.54841;
	double g = sec * DEG2RAD / 240.0;
	g -= TWOPI * (double)(int32_t)(g / TWOPI);
	return g < 0.0 ? g + TWOPI : g;
}

void sat_observer_set(sat_observer_t *o, double lat_deg, double lon_deg, double alt_m)
{
	const double e2 = WGS84_F * (2.0 - WGS84_F);
	o->lat = lat_deg * DEG2RAD;
	o->lon = lon_deg * DEG2RAD;
	o->alt = alt_m / 1000.0;
	double sl, cl;
	sat_sincos(o->lat, &sl, &cl);
	o->sin_lat = sl;
	o->cos_lat = cl;
	sat_sincos(o->lon, &o->sin_lon, &o->cos_lon);
	double n = WGS84_A / sqrt(1.0 - e2 * sl * sl);
	o->r[0] = (n + o->alt) * cl * o->cos_lon;
	o->r[1] = (n + o->alt) * cl * o->sin_lon;
	o->r[2] = (n * (1.0 - e2) + o->alt) * sl;
}

void sat_look(const sat_observer_t *o, const sgp4_state_t *st, double unix_s, sat_look_t *out)
{
	// TEME to earth-fixed: a turn by sidereal time about the pole; the velocity loses the earth's rotation
	double g = sat_gmst(unix_s);
	double sg, cg;
	sat_sincos(g, &sg, &cg);
	double x = cg * st->r[0] + sg * st->r[1];
	double y = -sg * st->r[0] + cg * st->r[1];
	double z = st->r[2];
	double vx = cg * st->v[0] + sg * st->v[1] + OMEGA_E * y;
	double vy = -sg * st->v[0] + cg * st->v[1] - OMEGA_E * x;
	double vz = st->v[2];

	double rx = x - o->r[0], ry = y - o->r[1], rz = z - o->r[2];
	double range = sqrt(rx * rx + ry * ry + rz * rz);
	// South, east and up at the observer
	double sl = o->sin_lat, cl = o->cos_lat, so = o->sin_lon, co = o->cos_lon;
	double s = sl * co * rx + sl * so * ry - cl * rz;
	double e = -so * rx + co * ry;
	double u = cl * co * rx + cl * so * ry + sl * rz;
	double az = atan2(e, -s) * RAD2DEG;
	out->az = az < 0.0 ? az + 360.0 : az;
	// atan2 rather than asin(u / range): the same angle, and asin would bring 1 KB of the C library
	out->el = atan2(u, sqrt(s * s + e * e)) * RAD2DEG;
	out->range = range;
	out->range_rate = (rx * vx + ry * vy + rz * vz) / range;
}

bool sat_look_at(const sgp4_t *s, const sat_observer_t *o, double unix_s, sat_look_t *out)
{
	sgp4_state_t st;
	if (sgp4_propagate(s, (unix_s - s->epoch) / 60.0, &st) != SGP4_OK)
		return false;
	sat_look(o, &st, unix_s, out);
	return true;
}

uint32_t sat_doppler_rx_hz(uint32_t f_hz, double range_rate)
{
	return (uint32_t)((double)f_hz * (1.0 - range_rate / SAT_C_KMS) + 0.5);
}

uint32_t sat_doppler_tx_hz(uint32_t f_hz, double range_rate)
{
	return (uint32_t)((double)f_hz / (1.0 - range_rate / SAT_C_KMS) + 0.5);
}

static int up(char c)
{
	return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

bool sat_locator_to_deg(const char *loc, double *lat_deg, double *lon_deg)
{
	int f0 = up(loc[0]) - 'A', f1 = up(loc[1]) - 'A';
	if (f0 < 0 || f0 > 17 || f1 < 0 || f1 > 17 || loc[2] < '0' || loc[2] > '9' || loc[3] < '0' || loc[3] > '9')
		return false;
	double lon = -180.0 + f0 * 20.0 + (loc[2] - '0') * 2.0;
	double lat = -90.0 + f1 * 10.0 + (loc[3] - '0') * 1.0;
	if (loc[4] == '\0') {
		lon += 1.0;
		lat += 0.5;
	} else {
		int s0 = up(loc[4]) - 'A', s1 = up(loc[5]) - 'A';
		if (s0 < 0 || s0 > 23 || s1 < 0 || s1 > 23 || loc[6] != '\0')
			return false;
		lon += s0 * (2.0 / 24.0) + 1.0 / 24.0;
		lat += s1 * (1.0 / 24.0) + 1.0 / 48.0;
	}
	*lat_deg = lat;
	*lon_deg = lon;
	return true;
}
