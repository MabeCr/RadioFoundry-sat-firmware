// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The satellite service (core/sat/sat_svc.c, docs/spec/satellite.md 5.1) on a simulated flash: each stored
// satellite's next pass and one satellite's pass list against Skyfield's (sat_ref.h), worked out in slices within the
// budget; a pass in progress, the lowest pass, satellites without or with old elements, no time or position, the
// observer moving, the store changing, passes setting as time goes on
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage/storage.h"
#include "sat/sat_store.h"
#include "sat/sat_svc.h"
#include "sat/sat_look.h"
#include "sim/sim_flash.h"
#include "sat_ref.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = 0x40000u,
	.sat_addr = 0x1B000u, .sat_size = 0x1000u,
};

const board_t board = { .name = "test", .flash = &sim_flash };

static void put(int tle, const char *name)
{
	sat_record_t rec = { 0 };
	CHECK(tle_parse(ref_tles[tle].l1, ref_tles[tle].l2, &rec.elements) == TLE_OK);
	rec.has_elements = true;
	const sat_profile_t *p = sat_profile_find(rec.elements.norad);
	if (p != NULL)
		rec.profile = *p;
	snprintf(rec.profile.name, sizeof rec.profile.name, "%s", name);
	rec.profile.norad = rec.elements.norad;
	CHECK(sat_store_put(&rec));
}

static sat_svc_in_t in;

// Polls until the service has nothing left to do; the number of polls it took
static unsigned settle(unsigned budget)
{
	unsigned polls = 0;
	while (sat_svc_poll(&in, budget) && polls < 10000)
		polls++;
	return polls + 1;
}

static int row_of(uint32_t norad, sat_row_t *out)
{
	for (uint8_t i = 0; i < sat_svc_count(); i++)
		if (sat_svc_row(i, out) && out->norad == norad)
			return i;
	return -1;
}

// The first reference pass of satellite tle not over by t with a peak of at least min_el
static const ref_pass_t *ref_next(int tle, double t, double min_el)
{
	for (unsigned i = 0; i < COUNT(ref_passes); i++)
		if (ref_passes[i].tle == tle && ref_passes[i].los > t && ref_passes[i].max_el >= min_el)
			return &ref_passes[i];
	return NULL;
}

int main(void)
{
	sim_flash_create(0x40000u, &map);
	CHECK(storage_init());
	put(0, "ISS");
	put(1, "SO-50");
	put(2, "AO-91");
	// A profile without elements, and the ISS's elements as if they were 60 days old
	sat_record_t none = { .profile = { .name = "RS-44", .norad = 44909, .up_hz = 145965000u, .down_hz = 435640000u } };
	CHECK(sat_store_put(&none));
	sat_record_t old = { 0 };
	CHECK(tle_parse(ref_tles[4].l1, ref_tles[4].l2, &old.elements) == TLE_OK);
	old.has_elements = true;
	old.profile.norad = old.elements.norad;
	snprintf(old.profile.name, sizeof old.profile.name, "OLD-1");

	sat_svc_init();
	// No time yet: the list's names, nothing predicted
	in.min_el = 10;
	CHECK(!sat_svc_poll(&in, 100));
	CHECK(sat_svc_state() == SAT_SVC_NO_TIME && sat_svc_count() == 4);
	sat_row_t row;
	CHECK(sat_svc_row(3, &row) && strcmp(row.name, "RS-44") == 0 && row.state == SAT_ROW_NO_ORBIT);
	CHECK(sat_svc_row(0, &row) && row.state == SAT_ROW_WORKING);
	// The time, no position
	// From the latest epoch of the three: the reference passes start at each satellite's own epoch and run two days
	sat_elements_t e;
	double t0 = 0.0;
	for (int k = 0; k < 3; k++)
		if (tle_parse(ref_tles[k].l1, ref_tles[k].l2, &e) == TLE_OK && e.epoch > t0)
			t0 = e.epoch;
	in.time_ok = true;
	in.now = t0;
	CHECK(!sat_svc_poll(&in, 100) && sat_svc_state() == SAT_SVC_NO_POSITION);

	// The reference observer: every satellite's next pass of 10 degrees and more
	in.pos_ok = true;
	in.lat = ref_obs[0];
	in.lon = ref_obs[1];
	in.alt_m = ref_obs[2];
	unsigned polls = settle(60);
	printf("satSvcTest: the list in %u polls of 60 runs\n", polls);
	CHECK(polls > 3);
	CHECK(sat_svc_state() == SAT_SVC_READY);
	for (int tle = 0; tle < 3; tle++) {
		uint32_t norad = tle == 0 ? 25544 : tle == 1 ? 27607 : 43017;
		const ref_pass_t *p = ref_next(tle, t0, 10.0);
		CHECK(p != NULL && row_of(norad, &row) >= 0);
		if (p == NULL)
			continue;
		CHECK(row.state == (p->aos <= t0 ? SAT_ROW_UP : SAT_ROW_NEXT));
		CHECK(fabs(row.aos - p->aos) < 2.0 && fabs(row.los - p->los) < 2.0 && abs(row.max_el - (int)lround(p->max_el)) <= 1);
		CHECK(fabs(row.tca - p->tca) < 2.0 && row.tca >= row.aos && row.tca <= row.los);	// the highest point, for the Tracking screen
		CHECK(row.age_d >= 0 && row.age_d <= 3 && row.aos_az < 360 && row.los_az < 360 && row.dir < 360);
	}

	// The lowest pass at 40 degrees: later passes, the ones in between skipped
	in.min_el = 40;
	settle(60);
	const ref_pass_t *hi = ref_next(0, t0, 40.0);
	CHECK(hi != NULL && row_of(25544, &row) >= 0);
	if (hi != NULL)
		CHECK(fabs(row.aos - hi->aos) < 2.0 && row.max_el >= 40);
	in.min_el = 10;
	settle(60);

	// The ISS's pass list: every reference pass of 10 degrees and more in the next 24 hours
	sat_svc_open(25544);
	settle(60);
	sat_profile_t prof;
	CHECK(sat_svc_profile(&prof) && prof.norad == 25544 && prof.down_hz == 437800000u);
	sat_pass_row_t passes[SAT_SVC_PASSES];
	bool done = false;
	uint8_t n = sat_svc_passes(passes, SAT_SVC_PASSES, &done);
	unsigned want = 0;
	for (unsigned i = 0; i < COUNT(ref_passes); i++) {
		const ref_pass_t *p = &ref_passes[i];
		if (p->tle != 0 || p->max_el < 10.0 || p->los <= t0 || p->aos >= t0 + SAT_SVC_SPAN_S)
			continue;
		if (want < n) {
			const sat_pass_row_t *q = &passes[want];
			CHECK(fabs(q->aos - p->aos) < 2.0 && fabs(q->los - p->los) < 2.0);
			CHECK(fabs(q->tca - p->tca) < 2.0 && q->tca >= q->aos && q->tca <= q->los);
			// Its way across the sky: from the rise toward the set, north up and east right
			double r0 = q->aos_az * M_PI / 180.0, r1 = q->los_az * M_PI / 180.0;
			double h = atan2(sin(r1) - sin(r0), cos(r1) - cos(r0)) * 180.0 / M_PI;
			double d = fabs(fmod(h + 360.0, 360.0) - q->dir);
			CHECK(q->dir < 360 && (d < 1.0 || d > 359.0));
		}
		want++;
	}
	printf("satSvcTest: the ISS's passes: %u of %u\n", n, want);
	CHECK(done && n == (want < SAT_SVC_PASSES ? want : SAT_SVC_PASSES) && n >= 3);
	bool done1;
	sat_pass_row_t one;
	CHECK(sat_svc_pass_count(&done1) == n && done1 && sat_svc_pass(n - 1, &one) && one.aos == passes[n - 1].aos);
	CHECK(!sat_svc_pass(n, &one));

	// Into the ISS's next pass: up, with its elevation now; the pass list drops nothing yet
	const ref_pass_t *p0 = ref_next(0, t0, 10.0);
	in.now = p0->tca;
	settle(60);
	CHECK(row_of(25544, &row) >= 0 && row.state == SAT_ROW_UP && abs(row.el_now - (int)lround(p0->max_el)) <= 1);
	// Tracking that pass at its highest point: the position there, the path across the sky, the Doppler
	sat_svc_track((uint32_t)p0->aos, (uint32_t)p0->los);
	settle(60);
	sat_live_t live;
	CHECK(sat_svc_live(&live) && live.at == (uint32_t)in.now && fabs((double)live.el - p0->max_el) < 1.0);
	CHECK(live.range_km > 300.0f && live.range_km < 3000.0f && fabsf(live.rate_kms) < 1.0f);
	CHECK(live.rx_hz == sat_doppler_rx_hz(437800000u, (double)live.rate_kms) &&
	      live.tx_hz == sat_doppler_tx_hz(145990000u, (double)live.rate_kms));
	CHECK(sat_svc_path_count() == SAT_SVC_PATH);
	sat_path_pt_t pt;
	int top = 0;
	for (uint8_t i = 0; i < SAT_SVC_PATH && sat_svc_path_pt(i, &pt); i++)
		top = pt.el > top ? pt.el : top;
	CHECK(row_of(25544, &row) >= 0);
	CHECK(sat_svc_path_pt(0, &pt) && pt.el <= 1 && abs((int)pt.az - (int)row.aos_az) <= 1);
	CHECK(sat_svc_path_pt(SAT_SVC_PATH - 1, &pt) && pt.el <= 1 && abs((int)pt.az - (int)row.los_az) <= 1);
	CHECK(top >= (int)p0->max_el - 6 && top <= (int)lround(p0->max_el));
	CHECK(!sat_svc_path_pt(SAT_SVC_PATH, &pt));
	// Nobody asks again: it stops, and the position stays the last one
	in.now += 30.0;
	settle(60);
	CHECK(sat_svc_live(&live) && live.at == (uint32_t)(in.now - 30.0));
	in.now -= 30.0;
	sat_svc_track(0, 0);
	settle(60);
	CHECK(sat_svc_path_count() == 0 && !sat_svc_live(&live));
	// Before the pass, past the margin: nothing is looked at, so there is no position
	sat_svc_track((uint32_t)p0->aos, (uint32_t)p0->los);
	in.now = p0->aos - (double)SAT_TRACK_MARGIN_S - 60.0;
	sat_svc_track((uint32_t)p0->aos, (uint32_t)p0->los);	// asked again: the screen is still there
	settle(60);
	CHECK(!sat_svc_live(&live));
	// Inside the margin but still below the horizon: the position and the Doppler are worked out (the receiver can
	// be tuned for the rise)
	in.now = p0->aos - 110.0;
	sat_svc_track((uint32_t)p0->aos, (uint32_t)p0->los);
	settle(60);
	CHECK(sat_svc_live(&live) && live.el < 0.0f && live.rx_hz != 0 && live.tx_hz != 0);
	// Up: the Doppler is back
	in.now = p0->tca;
	sat_svc_track((uint32_t)p0->aos, (uint32_t)p0->los);
	settle(60);
	CHECK(sat_svc_live(&live) && live.el > 0.0f && live.rx_hz != 0);
	// Past the set and the margin: nothing again
	in.now = p0->los + (double)SAT_TRACK_MARGIN_S + 60.0;
	sat_svc_track((uint32_t)p0->aos, (uint32_t)p0->los);
	settle(60);
	CHECK(!sat_svc_live(&live));
	sat_svc_track(0, 0);
	settle(60);
	// After it: the next one, and the pass list starts at it
	in.now = p0->los + 1.0;
	settle(60);
	const ref_pass_t *p1 = ref_next(0, in.now, 10.0);
	CHECK(row_of(25544, &row) >= 0 && row.state == SAT_ROW_NEXT && p1 != NULL && fabs(row.aos - p1->aos) < 2.0);
	n = sat_svc_passes(passes, SAT_SVC_PASSES, &done);
	CHECK(n > 0 && fabs(passes[0].aos - p1->aos) < 2.0 && done);

	// The store changes: a satellite added shows up, one deleted goes
	CHECK(sat_store_put(&old) && sat_store_delete(43017));
	in.now = t0 + 60.0 * 86400.0;	// 60 days on: every element set is old
	settle(60);
	CHECK(sat_svc_count() == 4 && row_of(43017, &row) < 0 && row_of(100001, &row) >= 0 && row.state == SAT_ROW_OLD);
	CHECK(row_of(25544, &row) >= 0 && row.state == SAT_ROW_OLD && row.age_d >= 60);
	// The opened satellite stays (it's still stored)
	CHECK(sat_svc_profile(&prof) && prof.norad == 25544);
	sat_svc_open(43017);	// gone: nothing to show
	settle(60);
	CHECK(!sat_svc_profile(&prof) && sat_svc_passes(passes, SAT_SVC_PASSES, &done) == 0 && done);

	// A delete asked for through the service: done by the next poll, the list follows, the open satellite's passes close
	sat_svc_open(25544);
	settle(60);
	CHECK(sat_svc_profile(&prof) && prof.norad == 25544);
	uint8_t had = sat_svc_count();
	sat_svc_delete(25544);
	settle(60);
	CHECK(sat_svc_count() == had - 1 && row_of(25544, &row) < 0 && row_of(27607, &row) >= 0);
	CHECK(!sat_svc_profile(&prof) && sat_svc_passes(passes, SAT_SVC_PASSES, &done) == 0 && done);
	sat_svc_delete(999999);		// not stored: nothing changes
	settle(60);
	CHECK(sat_svc_count() == had - 1);
	put(0, "ISS");			// back for the rest
	sat_svc_refresh();
	settle(60);

	// The observer moves: worked out again
	in.now = t0;
	sat_svc_refresh();
	settle(60);
	CHECK(row_of(25544, &row) >= 0 && row.state != SAT_ROW_WORKING);
	uint32_t before = row.aos;
	in.lat = -33.9;		// Cape Town
	in.lon = 18.4;
	sat_svc_poll(&in, 1);
	CHECK(row_of(25544, &row) >= 0 && (row.state == SAT_ROW_WORKING || row.aos != before));
	settle(60);
	CHECK(row_of(25544, &row) >= 0 && (row.state == SAT_ROW_NEXT || row.state == SAT_ROW_UP) && row.aos != before);
	// The position is published, 1e-7 degree
	int32_t lat_e7, lon_e7;
	CHECK(sat_svc_position(&lat_e7, &lon_e7) && lat_e7 == -339000000 && lon_e7 == 184000000);
	// The fix is lost: the last position stays, so the passes go on and nothing is worked out again
	uint32_t aos_kept = row.aos;
	in.pos_ok = false;
	in.lat = in.lon = 0.0;		// whatever the inputs hold is not used
	unsigned again = settle(60);
	CHECK(again <= 1 && sat_svc_state() == SAT_SVC_READY);
	CHECK(row_of(25544, &row) >= 0 && row.aos == aos_kept && row.state != SAT_ROW_WORKING);
	CHECK(sat_svc_position(&lat_e7, &lon_e7) && lat_e7 == -339000000 && lon_e7 == 184000000);
	// Pass alerts: a satellite marked for every pass alerts once, a lead time before its rise; a marked single pass alerts
	// once and its mark is spent; an unmarked one never does
	in.now = t0;
	settle(60);
	sat_svc_alert_t al;
	CHECK(row_of(25544, &row) >= 0 && row.state == SAT_ROW_NEXT);
	uint32_t iss_aos = row.aos;
	int8_t iss_el = row.max_el;
	in.alert_lead_s = 300;
	in.now = (double)iss_aos - 200.0;
	settle(60);
	CHECK(!sat_svc_alert_take(&al));		// nothing marked
	in.alert_norad[0] = 25544;
	in.now = (double)iss_aos - 600.0;
	settle(60);
	CHECK(!sat_svc_alert_take(&al));		// marked, not yet within the lead time
	in.now = (double)iss_aos - 200.0;
	settle(60);
	CHECK(sat_svc_alert_take(&al) && al.norad == 25544 && al.aos == iss_aos && al.max_el == iss_el);
	CHECK(!sat_svc_alert_take(&al));
	settle(60);
	CHECK(!sat_svc_alert_take(&al));		// once
	in.alert_norad[0] = 0;
	CHECK(row_of(27607, &row) >= 0 && row.state == SAT_ROW_NEXT);
	uint32_t so_aos = row.aos;
	CHECK(!sat_svc_pass_alert_marked(27607, so_aos));
	sat_svc_pass_alert_toggle(27607, so_aos);
	CHECK(sat_svc_pass_alert_marked(27607, so_aos));	// asked for counts at once
	in.now = (double)so_aos - 100.0;
	settle(60);
	CHECK(sat_svc_alert_take(&al) && al.norad == 27607 && al.aos == so_aos);
	CHECK(!sat_svc_pass_alert_marked(27607, so_aos));	// spent
	in.alert_lead_s = 0;
	in.now = t0;
	settle(60);

	// The pass lists of the satellites opened lately are kept: opening one pl_again is at once, with no search; a list that lost
	// its pl_first pass to the clock is searched on, not from the start; a changed store or observer drops them all
	in.now = t0;
	in.lat = -33.9;
	in.lon = 18.4;
	in.pos_ok = true;
	settle(60);
	sat_pass_row_t pl_first[SAT_SVC_PASSES], pl_again[SAT_SVC_PASSES];
	bool d1, d2;
	sat_svc_open(25544);
	settle(60);
	uint8_t n1 = sat_svc_passes(pl_first, SAT_SVC_PASSES, &d1);
	CHECK(n1 >= 2 && d1);
	sat_svc_open(27607);
	settle(60);
	CHECK(sat_svc_passes(pl_again, SAT_SVC_PASSES, &d2) > 0 && d2);
	sat_svc_open(25544);
	bool dn = true;
	CHECK(sat_svc_pass_count(&dn) == 0 && !dn);	// asked for, not switched to yet: not the last one's passes
	CHECK(sat_svc_passes(pl_again, SAT_SVC_PASSES, &d2) == 0 && !d2 && !sat_svc_pass(0, &pl_again[0]));
	CHECK(!sat_svc_poll(&in, 1));			// nothing left to work out: the list came from the cache
	uint8_t n2 = sat_svc_passes(pl_again, SAT_SVC_PASSES, &d2);
	CHECK(n2 == n1 && d2 && memcmp(pl_first, pl_again, n1 * sizeof pl_first[0]) == 0);
	sat_svc_open(25544);				// the one already open, asked for again: its list stays
	CHECK(sat_svc_pass_count(&dn) == n1);
	// The satellite list opened again (a refresh) keeps them as well
	sat_svc_refresh();
	sat_svc_open(25544);
	sat_svc_poll(&in, 1);				// rows are worked again, the open list is not
	CHECK(sat_svc_passes(pl_again, SAT_SVC_PASSES, &d2) == n1 && memcmp(pl_first, pl_again, n1 * sizeof pl_first[0]) == 0);
	// Time passes: the pl_first pass is over, the rest stay and the search goes on after them
	in.now = (double)pl_first[0].los + 120.0;
	sat_svc_open(27607);
	settle(60);
	sat_svc_open(25544);
	settle(60);
	n2 = sat_svc_passes(pl_again, SAT_SVC_PASSES, &d2);
	CHECK(n2 >= n1 - 1 && d2 && pl_again[0].aos == pl_first[1].aos && pl_again[0].los > (uint32_t)in.now);
	for (uint8_t i = 1; i < n2; i++)
		CHECK(pl_again[i].aos > pl_again[i - 1].aos);
	CHECK(pl_again[n2 - 1].aos >= pl_first[n1 - 1].aos);
	// The store changes: every kept list is out of date
	sat_pass_row_t pl_before[SAT_SVC_PASSES];
	uint8_t nb = sat_svc_passes(pl_before, SAT_SVC_PASSES, &d1);
	sat_svc_open(27607);
	settle(60);
	CHECK(sat_store_put(&none));
	unsigned worked = settle(60);
	sat_svc_open(25544);
	worked += settle(60);
	CHECK(worked > 3);				// searched pl_again
	n2 = sat_svc_passes(pl_again, SAT_SVC_PASSES, &d2);
	CHECK(n2 == nb);
	for (uint8_t i = 0; i < nb && i < n2; i++)	// worked out from another start: the same passes to a second or two
		CHECK(pl_again[i].aos - pl_before[i].aos + 2u <= 4u && pl_again[i].los - pl_before[i].los + 2u <= 4u);
	// The observer moves: the same
	sat_svc_open(27607);
	settle(60);
	in.lat = 51.5;
	in.lon = -0.1;
	settle(60);
	sat_svc_open(25544);
	settle(60);
	n2 = sat_svc_passes(pl_again, SAT_SVC_PASSES, &d2);
	CHECK(n2 > 0 && pl_again[0].aos != pl_before[0].aos);
	in.now = t0;
	in.lat = -33.9;
	in.lon = 18.4;
	settle(60);

	// Without time it still stops
	in.time_ok = false;
	sat_svc_poll(&in, 1);
	CHECK(sat_svc_state() == SAT_SVC_NO_TIME);
	in.time_ok = true;

	CHECK(sim_flash_violations() == 0);
	sim_flash_free();
	printf("satSvcTest: %s\n", failures ? "FAILED" : "passed");
	return failures != 0;
}
