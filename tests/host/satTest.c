// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Satellite tracking (core/sat, docs/spec/satellite.md): TLE parsing, SGP4 against python-sgp4, look angles and
// passes against Skyfield (tests/host/sat_ref.h, from tools/sat_reference.py), Doppler, locators and the repeater
// arming state machine
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sat/sat_clock.h"
#include "sat/sat_math.h"
#include "sat/sat_tle.h"
#include "sat/sgp4.h"
#include "sat/sat_look.h"
#include "sat/sat_pass.h"
#include "sat/sat_profile.h"
#include "sat_ref.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const char *ISS1 = "1 25544U 98067A   26300.51782528  .00016717  00000-0  30144-3 0  9996";
static const char *ISS2 = "2 25544  51.6416 247.4627 0006703 130.5360 325.0288 15.49815361474557";

// The model's own sine and cosine against the C library's, over the angles it meets (and well past them)
static void test_math(void)
{
	double worst = 0;
	for (double x = -5000.0; x <= 5000.0; x += 0.0123) {
		double s, c;
		sat_sincos(x, &s, &c);
		double ds = fabs(s - sin(x)), dc = fabs(c - cos(x));
		worst = ds > worst ? ds : worst;
		worst = dc > worst ? dc : worst;
	}
	printf("sincos: worst %.2g from the C library over +-5000 rad\n", worst);
	CHECK(worst < 5e-16);
	CHECK(sat_sin(0.0) == 0.0 && sat_cos(0.0) == 1.0);
	CHECK(fabs(sat_sin(3.14159265358979323846 / 2) - 1.0) < 1e-16 && fabs(sat_cos(3.14159265358979323846) + 1.0) < 1e-16);
	CHECK(fabs(sat_sin(-1e6) - sin(-1e6)) < 1e-12);
}

static void test_tle(void)
{
	sat_elements_t e;
	CHECK(tle_checksum(ISS1) == 6 && tle_checksum(ISS2) == 7);
	CHECK(tle_parse(ISS1, ISS2, &e) == TLE_OK);
	CHECK(e.norad == 25544);
	CHECK(fabs(e.inclo - 51.6416) < 1e-12 && fabs(e.nodeo - 247.4627) < 1e-12 && fabs(e.ecco - 0.0006703) < 1e-15);
	CHECK(fabs(e.argpo - 130.5360) < 1e-12 && fabs(e.mo - 325.0288) < 1e-12 && fabs(e.no - 15.49815361) < 1e-12);
	CHECK(fabs(e.bstar - 0.30144e-3) < 1e-15);
	// 2026 day 300.51782528: 27 October 12:25:40.104 UTC
	CHECK(fabs(e.epoch - (1793059200.0 + 0.51782528 * 86400.0)) < 1e-4);

	// Line endings and spaces after column 69 are allowed
	char l1[80], l2[80];
	snprintf(l1, sizeof l1, "%s\r\n", ISS1);
	snprintf(l2, sizeof l2, "%s\n", ISS2);
	CHECK(tle_parse(l1, l2, &e) == TLE_OK);

	// A wrong digit
	strcpy(l1, ISS1);
	l1[20] = '7';
	CHECK(tle_parse(l1, ISS2, &e) == TLE_ERR_CHECKSUM);
	// Short, or the lines swapped
	CHECK(tle_parse("1 25544U", ISS2, &e) == TLE_ERR_LENGTH);
	CHECK(tle_parse(ISS2, ISS1, &e) == TLE_ERR_LENGTH);
	// Two satellites
	CHECK(tle_parse(ref_tles[1].l1, ISS2, &e) == TLE_ERR_MISMATCH);
	// A letter in a number field, its check digit right (the letter counts 0)
	strcpy(l2, ISS2);
	l2[10] = 'x';
	l2[68] = (char)('0' + tle_checksum(l2));
	CHECK(tle_parse(ISS1, l2, &e) == TLE_ERR_FIELD);

	// Alpha-5: A0001 is 100001; J (past I) is 18
	CHECK(tle_parse(ref_tles[4].l1, ref_tles[4].l2, &e) == TLE_OK && e.norad == 100001);

	// The epoch's year: 57-99 the 1900s, 00-56 the 2000s
	CHECK(sat_days_from_civil(1970, 1, 1) == 0 && sat_days_from_civil(2000, 3, 1) == 11017);
	CHECK(tle_epoch_unix(26, 1.0) == 1767225600.0 && tle_epoch_unix(57, 1.0) == -410227200.0);
	CHECK(tle_epoch_unix(0, 60.5) == 951825600.0);	// 29 February 2000, noon
}

static void test_sgp4(void)
{
	double worst_r = 0, worst_v = 0;
	unsigned n = 0;
	for (unsigned i = 0; i < COUNT(ref_states); i++) {
		const ref_state_t *r = &ref_states[i];
		sat_elements_t e;
		sgp4_t s;
		sgp4_state_t st;
		CHECK(tle_parse(ref_tles[r->tle].l1, ref_tles[r->tle].l2, &e) == TLE_OK);
		CHECK(sgp4_init(&s, &e) == SGP4_OK);
		CHECK(sgp4_propagate(&s, r->tsince, &st) == SGP4_OK);
		for (int k = 0; k < 3; k++) {
			double dr = fabs(st.r[k] - r->r[k]), dv = fabs(st.v[k] - r->v[k]);
			worst_r = dr > worst_r ? dr : worst_r;
			worst_v = dv > worst_v ? dv : worst_v;
		}
		n++;
	}
	printf("sgp4: %u states, worst %.2g km, %.2g km/s from python-sgp4\n", n, worst_r, worst_v);
	CHECK(n >= 40);
	CHECK(worst_r < 1e-6 && worst_v < 1e-9);

	// The perigee under 220 km takes the simplified model
	sat_elements_t e;
	sgp4_t s;
	CHECK(tle_parse(ref_tles[3].l1, ref_tles[3].l2, &e) == TLE_OK && sgp4_init(&s, &e) == SGP4_OK && s.isimp);

	// A 12-hour orbit needs the deep-space model, which isn't here
	e.no = 2.0;
	e.ecco = 0.7;
	CHECK(sgp4_init(&s, &e) == SGP4_ERR_DEEP_SPACE);
	// Out-of-range elements
	e.no = 15.0;
	e.ecco = 1.2;
	CHECK(sgp4_init(&s, &e) == SGP4_ERR_ECC);
	// Decayed: the low orbit's drag brings it down within weeks
	CHECK(tle_parse(ref_tles[3].l1, ref_tles[3].l2, &e) == TLE_OK && sgp4_init(&s, &e) == SGP4_OK);
	sgp4_state_t st;
	CHECK(sgp4_propagate(&s, 60.0 * 24 * 60, &st) != SGP4_OK);
}

static void test_look(void)
{
	sat_observer_t o;
	sat_observer_set(&o, ref_obs[0], ref_obs[1], ref_obs[2]);
	double worst_az = 0, worst_el = 0, worst_rng = 0, worst_rr = 0;
	for (unsigned i = 0; i < COUNT(ref_looks); i++) {
		const ref_look_t *r = &ref_looks[i];
		sat_elements_t e;
		sgp4_t s;
		sat_look_t l;
		CHECK(tle_parse(ref_tles[r->tle].l1, ref_tles[r->tle].l2, &e) == TLE_OK && sgp4_init(&s, &e) == SGP4_OK);
		CHECK(sat_look_at(&s, &o, r->unix_s, &l));
		double daz = fabs(l.az - r->az);
		if (daz > 180.0)
			daz = 360.0 - daz;
		// Azimuth only where it means something: above the horizon, not overhead
		if (r->el > 0.0 && r->el < 85.0)
			worst_az = daz > worst_az ? daz : worst_az;
		double del = fabs(l.el - r->el), drng = fabs(l.range - r->range), drr = fabs(l.range_rate - r->range_rate);
		worst_el = del > worst_el ? del : worst_el;
		worst_rng = drng > worst_rng ? drng : worst_rng;
		worst_rr = drr > worst_rr ? drr : worst_rr;
	}
	printf("look: %zu, worst az %.4f deg, el %.4f deg, range %.3f km, range rate %.2f m/s from Skyfield\n",
		COUNT(ref_looks), worst_az, worst_el, worst_rng, worst_rr * 1000.0);
	// No polar motion or UT1 here: hundredths of a degree, under a hertz of Doppler at 70 cm
	CHECK(worst_az < 0.01 && worst_el < 0.005 && worst_rng < 0.1 && worst_rr < 0.001);

	// Sidereal time: 2000-01-01 12:00 UT1 is 280.46062 degrees
	CHECK(fabs(sat_gmst(946728000.0) * 180.0 / 3.14159265358979323846 - 280.46061837) < 1e-6);
}

static void test_passes(void)
{
	sat_observer_t o;
	sat_observer_set(&o, ref_obs[0], ref_obs[1], ref_obs[2]);
	double worst_aos = 0, worst_los = 0, worst_tca = 0, worst_el = 0;
	unsigned evals_max = 0;
	for (unsigned i = 0; i < COUNT(ref_passes); i++) {
		const ref_pass_t *r = &ref_passes[i];
		sat_elements_t e;
		sgp4_t s;
		sat_pass_t p;
		unsigned evals;
		CHECK(tle_parse(ref_tles[r->tle].l1, ref_tles[r->tle].l2, &e) == TLE_OK && sgp4_init(&s, &e) == SGP4_OK);
		CHECK(sat_pass_next(&s, &o, r->from, 3600.0, 0.0f, &p, &evals));
		double da = fabs(p.aos - r->aos), dl = fabs(p.los - r->los), dt = fabs(p.tca - r->tca);
		double de = fabs((double)p.max_el - r->max_el);
		worst_aos = da > worst_aos ? da : worst_aos;
		worst_los = dl > worst_los ? dl : worst_los;
		worst_tca = dt > worst_tca ? dt : worst_tca;
		worst_el = de > worst_el ? de : worst_el;
		evals_max = evals > evals_max ? evals : evals_max;

		// The same pass from inside it: its rise lies behind
		sat_pass_t q;
		CHECK(sat_pass_next(&s, &o, r->tca + 30.0, 3600.0, 0.0f, &q, NULL));
		CHECK(fabs(q.aos - p.aos) < 2.0 && fabs(q.los - p.los) < 2.0);
	}
	printf("passes: %zu, worst rise %.1f s, set %.1f s, peak %.1f s, %.3f deg; at most %u SGP4 runs each\n",
		COUNT(ref_passes), worst_aos, worst_los, worst_tca, worst_el, evals_max);
	CHECK(COUNT(ref_passes) >= 20);
	CHECK(worst_aos < 2.0 && worst_los < 2.0 && worst_tca < 5.0 && worst_el < 0.05);

	// Only passes at least 30 degrees up: the first such reference pass of the ISS, skipping the lower ones
	const ref_pass_t *first = NULL;
	for (unsigned i = 0; i < COUNT(ref_passes) && first == NULL; i++)
		if (ref_passes[i].tle == 0 && ref_passes[i].max_el >= 30.0)
			first = &ref_passes[i];
	CHECK(first != NULL);
	if (first != NULL) {
		sat_elements_t e;
		sgp4_t s;
		sat_pass_t p;
		unsigned evals;
		CHECK(tle_parse(ref_tles[0].l1, ref_tles[0].l2, &e) == TLE_OK && sgp4_init(&s, &e) == SGP4_OK);
		CHECK(sat_pass_next(&s, &o, ref_passes[0].from, 86400.0, 30.0f, &p, &evals));
		CHECK(fabs(p.aos - first->aos) < 2.0 && p.max_el >= 30.0f);
		// Nothing that high within an hour of the first, low pass
		CHECK(!sat_pass_next(&s, &o, ref_passes[0].from, 1200.0, 30.0f, &p, NULL));

		// A day of the ISS's passes, as the pass list would compute them
		unsigned total = 0, count = 0;
		double t = ref_passes[0].from;
		while (sat_pass_next(&s, &o, t, ref_passes[0].from + 86400.0 - t, 0.0f, &p, &evals)) {
			total += evals;
			count++;
			t = p.los + 60.0;
		}
		total += evals;
		printf("passes: a day of the ISS is %u passes for %u SGP4 runs\n", count, total);
		CHECK(count >= 4 && count <= 8 && total < 2500);
	}
}

static void test_doppler(void)
{
	// Coming in at 7 km/s: the 70 cm downlink is heard 10.2 kHz high, the 2 m uplink sent 3.4 kHz low
	CHECK(sat_doppler_rx_hz(437800000u, -7.0) == 437810222u);
	CHECK(sat_doppler_tx_hz(145990000u, -7.0) == 145986591u);
	// Going away: the other way
	CHECK(sat_doppler_rx_hz(437800000u, 7.0) == 437789778u);
	CHECK(sat_doppler_tx_hz(145990000u, 7.0) == 145993409u);
	CHECK(sat_doppler_rx_hz(145960000u, 0.0) == 145960000u && sat_doppler_tx_hz(435250000u, 0.0) == 435250000u);
}

static void test_locator(void)
{
	double lat, lon;
	CHECK(sat_locator_to_deg("FN31pr", &lat, &lon));
	CHECK(fabs(lat - (41.0 + 17.0 / 24.0 + 1.0 / 48.0)) < 1e-9 && fabs(lon - (-74.0 + 15.0 / 12.0 + 1.0 / 24.0)) < 1e-9);
	CHECK(sat_locator_to_deg("dm79", &lat, &lon) && lat == 39.5 && lon == -105.0);
	CHECK(sat_locator_to_deg("JJ00aa", &lat, &lon) && fabs(lat - 1.0 / 48.0) < 1e-12 && fabs(lon - 1.0 / 24.0) < 1e-12);
	CHECK(!sat_locator_to_deg("SS00", &lat, &lon));		// fields end at R
	CHECK(!sat_locator_to_deg("FN3", &lat, &lon));
	CHECK(!sat_locator_to_deg("FN31p", &lat, &lon));
	CHECK(!sat_locator_to_deg("FN31yy", &lat, &lon));	// subsquares end at X
	CHECK(!sat_locator_to_deg("FN31pr7", &lat, &lon));
}

static void test_arm(void)
{
	const sat_profile_t *so50 = sat_profile_find(27607);
	const sat_profile_t *iss = sat_profile_find(25544);
	CHECK(so50 != NULL && iss != NULL && sat_profile_find(1) == NULL);
	CHECK(sat_profiles_builtin_count == 3);
	if (so50 == NULL || iss == NULL)
		return;
	CHECK(strcmp(so50->name, "SO-50") == 0 && so50->arm_tone == 744 && so50->up_tone == 670);

	// No arming on the ISS: contacts use its tone at once
	sat_arm_t a;
	sat_arm_reset(&a, iss);
	CHECK(!sat_arm_start(&a, 1000));
	sat_arm_out_t out = sat_arm_poll(&a, 1000);
	CHECK(!out.key && out.tone == 670 && out.armed_left_s == 0 && a.state == SAT_ARM_IDLE);

	// SO-50: 2 s of carrier with 74.4 Hz, then 10 minutes with 67.0 Hz
	sat_arm_reset(&a, so50);
	out = sat_arm_poll(&a, 500);
	CHECK(!out.key && out.tone == 670 && a.state == SAT_ARM_IDLE);
	CHECK(sat_arm_start(&a, 1000));
	CHECK(!sat_arm_start(&a, 1500));	// already on the air
	out = sat_arm_poll(&a, 1000);
	CHECK(out.key && out.tone == 744);
	out = sat_arm_poll(&a, 2999);
	CHECK(out.key && out.tone == 744 && a.state == SAT_ARM_SENDING);
	out = sat_arm_poll(&a, 3000);
	CHECK(!out.key && out.tone == 670 && a.state == SAT_ARM_ARMED && out.armed_left_s == 600);
	out = sat_arm_poll(&a, 3001);
	CHECK(out.armed_left_s == 600);
	out = sat_arm_poll(&a, 3000 + 599000);
	CHECK(out.armed_left_s == 1 && a.state == SAT_ARM_ARMED);
	out = sat_arm_poll(&a, 3000 + 600000);
	CHECK(out.armed_left_s == 0 && a.state == SAT_ARM_IDLE && a.expired && out.tone == 670);
	// Arming again clears the expiry
	CHECK(sat_arm_start(&a, 700000) && !a.expired);

	// The carrier cut short: not armed
	sat_arm_reset(&a, so50);
	CHECK(sat_arm_start(&a, 1000));
	sat_arm_abort(&a);
	out = sat_arm_poll(&a, 1500);
	CHECK(!out.key && a.state == SAT_ARM_IDLE && !a.expired);
	// Aborting while armed changes nothing: the repeater already heard the carrier
	CHECK(sat_arm_start(&a, 2000));
	sat_arm_poll(&a, 4000);
	sat_arm_abort(&a);
	CHECK(a.state == SAT_ARM_ARMED);
	// Re-arming while armed starts a new carrier
	CHECK(sat_arm_start(&a, 10000) && a.state == SAT_ARM_SENDING);

	// Across the millisecond clock's wrap
	sat_arm_reset(&a, so50);
	CHECK(sat_arm_start(&a, 0xFFFFFC00u));
	out = sat_arm_poll(&a, 0xFFFFFF00u);
	CHECK(out.key);
	out = sat_arm_poll(&a, 0x00000800u);	// 3.072 s on: armed 1.072 s ago
	CHECK(!out.key && a.state == SAT_ARM_ARMED && out.armed_left_s == 599);
}

static void test_clock(void)
{
	// 27 October 2026, 12:25:40
	CHECK(sat_clock_unix_from_nmea(271026, 122540) == 1793103940u);
	CHECK(sat_clock_unix_from_nmea(10100, 0) == 946684800u);		// 1 January 2000, midnight
	CHECK(sat_clock_unix_from_nmea(290224, 235959) == 1709251199u);	// a leap day
	CHECK(sat_clock_unix_from_nmea(290225, 0) == 0 && sat_clock_unix_from_nmea(310426, 0) == 0);
	CHECK(sat_clock_unix_from_nmea(0, 0) == 0 && sat_clock_unix_from_nmea(11326, 0) == 0);	// day 0, month 13
	CHECK(sat_clock_unix_from_nmea(271026, 246000) == 0 && sat_clock_unix_from_nmea(271026, 126000) == 0);

	sat_clock_t c = { 0 };
	double t;
	CHECK(!sat_clock_now(&c, 1000, &t));
	CHECK(!sat_clock_set_nmea(&c, 0, 122540, 1000));		// RMC without a date
	CHECK(!sat_clock_set_nmea(&c, 271026, 0xFFFFFFFFu, 1000));
	CHECK(!sat_clock_now(&c, 1000, &t));
	CHECK(sat_clock_set_nmea(&c, 271026, 122540, 5000));
	CHECK(sat_clock_now(&c, 5000, &t) && t == 1793103940.0);
	CHECK(sat_clock_now(&c, 7250, &t) && t == 1793103942.25);
	CHECK((c.seq & 1u) == 0);
	// Across the millisecond clock's wrap
	CHECK(sat_clock_set_nmea(&c, 271026, 122540, 0xFFFFFC18u));
	CHECK(sat_clock_now(&c, 0x000003E8u, &t) && t == 1793103942.0);
}

int main(void)
{
	test_math();
	test_tle();
	test_sgp4();
	test_look();
	test_passes();
	test_doppler();
	test_locator();
	test_arm();
	test_clock();
	printf("satTest: %s\n", failures ? "FAILED" : "passed");
	return failures != 0;
}
