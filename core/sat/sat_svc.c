// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The satellite service (sat_svc.h). One piece of work at a time: the opened satellite's pass list first (someone is
// looking at it), then the list's rows in order. A piece searches an hour of sky per step and keeps its place, so a
// poll's budget bounds the work and the next poll goes on where it stopped.
#include <math.h>
#include <string.h>
#include "sat/sat_svc.h"
#include "sat/sat_math.h"
#include "sat/sat_pass.h"
#include "sat/sat_store.h"

#define STEP_S		3600.0	// one pass search: an hour of sky, about 100 runs of the model at most
#define TRACK_LEAVE_S	5.0	// the tracking stops this long after the screen last asked
#define LIVE_S		1.0	// how often the tracked satellite is looked at
#define EL_NOW_S	2.0	// how often an up satellite's elevation is worked out again
#define PATH_PER_STEP	6	// points of a path worked out per step
#define MOVED_DEG	0.01	// the observer moved this far (about a kilometer): everything again

// Results, read from any task (the sequence count's odd values: being written)
static struct {
	volatile uint32_t seq;
	uint8_t state;
	uint8_t count;
	sat_row_t rows[SAT_SVC_MAX];
	uint32_t open_norad;		// 0 none
	bool open_found;		// in the store
	bool passes_done;
	uint8_t npasses;
	sat_profile_t open_profile;
	sat_pass_row_t passes[SAT_SVC_PASSES];
	sat_live_t live;
	uint8_t npath;
	sat_path_pt_t path[SAT_SVC_PATH];
} r;

// Requests from the screens
static volatile bool refresh_asked = true;
static volatile uint32_t open_asked;
static volatile bool open_changed;
static volatile uint32_t track_aos, track_los;	// the tracked pass, 0 none
static volatile bool track_changed;
static volatile bool track_alive;	// asked again since the poll last looked: the screen is still there

// The work, the service task's own
static struct {
	uint32_t generation;		// the store's when the list was read
	double lat, lon;		// the observer the results are for
	uint8_t min_el;
	bool have_obs;
	sat_observer_t obs;
	int8_t kind;			// what the model holds: -1 nothing, 0 a row, 1 the pass list
	uint8_t row;			// the row being worked (kind 0)
	uint32_t norad;			// whose model it is
	sgp4_t model;
	double from, end;		// the search's place and its end
	uint32_t alive_at;		// when the tracking was last asked for
	uint32_t live_at;		// when the tracked satellite was last looked at
	uint32_t el_at[SAT_SVC_MAX];	// when an up row's elevation was last worked out (UTC seconds)
} w = { .kind = -1 };

// A reader that finds a write under way (the count odd or changed) tries again a few times, then gives up for now: the
// service's task runs below the UI's, so a UI task that waited for a write it had preempted would wait forever
#define SEQ_TRIES	50
#define SEQ_BUSY(s, tries)	((((s) & 1u) != 0 || (s) != r.seq) && ++(tries) < SEQ_TRIES)

static void begin_write(void) { r.seq++; }
static void end_write(void) { r.seq++; }

void sat_svc_init(void)
{
	memset(&r, 0, sizeof r);
	memset(&w, 0, sizeof w);
	w.kind = -1;
	refresh_asked = true;
	open_asked = 0;
	open_changed = false;
	track_aos = track_los = 0;
	track_changed = false;
	track_alive = false;
}

void sat_svc_no_memory(void)
{
	begin_write();
	r.state = SAT_SVC_NO_MEMORY;
	end_write();
}

void sat_svc_refresh(void)
{
	refresh_asked = true;
}

void sat_svc_open(uint32_t norad)
{
	open_asked = norad;
	open_changed = true;
}

void sat_svc_track(uint32_t aos, uint32_t los)
{
	if (aos != track_aos || los != track_los) {
		track_aos = aos;
		track_los = los;
		track_changed = true;
	}
	track_alive = aos != 0;
}

// --- reading the results ---

uint8_t sat_svc_state(void)
{
	return r.state;
}

uint8_t sat_svc_count(void)
{
	return r.count;
}

bool sat_svc_row(uint8_t i, sat_row_t *out)
{
	uint32_t s;
	unsigned tries = 0;
	bool ok;
	do {
		s = r.seq;
		ok = i < r.count;
		if (ok)
			*out = r.rows[i];
	} while (SEQ_BUSY(s, tries));
	return ok && tries < SEQ_TRIES;
}

bool sat_svc_profile(sat_profile_t *out)
{
	uint32_t s;
	unsigned tries = 0;
	bool ok;
	do {
		s = r.seq;
		ok = r.open_norad != 0 && r.open_found;
		if (ok)
			*out = r.open_profile;
	} while (SEQ_BUSY(s, tries));
	return ok && tries < SEQ_TRIES;
}

uint8_t sat_svc_passes(sat_pass_row_t *out, uint8_t max, bool *done)
{
	uint32_t s;
	unsigned tries = 0;
	uint8_t n;
	do {
		s = r.seq;
		n = r.npasses < max ? r.npasses : max;
		memcpy(out, r.passes, n * sizeof *out);
		*done = r.passes_done;
	} while (SEQ_BUSY(s, tries));
	if (tries >= SEQ_TRIES) {
		*done = false;	// busy: nothing yet, the next draw asks again
		return 0;
	}
	return n;
}

uint8_t sat_svc_pass_count(bool *done)
{
	uint32_t s;
	unsigned tries = 0;
	uint8_t n;
	do {
		s = r.seq;
		n = r.npasses;
		*done = r.passes_done;
	} while (SEQ_BUSY(s, tries));
	if (tries >= SEQ_TRIES) {
		*done = false;	// busy: nothing yet, the next draw asks again
		return 0;
	}
	return n;
}

bool sat_svc_pass(uint8_t i, sat_pass_row_t *out)
{
	uint32_t s;
	unsigned tries = 0;
	bool ok;
	do {
		s = r.seq;
		ok = i < r.npasses;
		if (ok)
			*out = r.passes[i];
	} while (SEQ_BUSY(s, tries));
	return ok && tries < SEQ_TRIES;
}

bool sat_svc_live(sat_live_t *out)
{
	uint32_t s;
	unsigned tries = 0;
	bool ok;
	do {
		s = r.seq;
		*out = r.live;
		ok = r.live.ok;
	} while (SEQ_BUSY(s, tries));
	return ok && tries < SEQ_TRIES;
}

uint8_t sat_svc_path_count(void)
{
	return r.npath;
}

bool sat_svc_path_pt(uint8_t i, sat_path_pt_t *out)
{
	uint32_t s;
	unsigned tries = 0;
	bool ok;
	do {
		s = r.seq;
		ok = i < r.npath;
		if (ok)
			*out = r.path[i];
	} while (SEQ_BUSY(s, tries));
	return ok && tries < SEQ_TRIES;
}

// --- the work ---

static int16_t age_days(const sat_elements_t *e, double now)
{
	double d = (now - e->epoch) / 86400.0;
	return (int16_t)(d > 32000.0 ? 32000 : d < -32000.0 ? -32000 : d);
}

// The list again from the store: names, catalog numbers, what can be predicted. Row by row into the results (a copy
// of the whole list would take 1 KB of the task's stack), each write short, so a reader never waits on the flash
static void read_list(double now)
{
	uint16_t slots = sat_store_slots();
	uint8_t n = 0;
	sat_record_t rec;
	for (uint16_t i = 0; i < slots && n < SAT_SVC_MAX; i++) {
		if (!sat_store_read(i, &rec))
			continue;
		sat_row_t row;
		memset(&row, 0, sizeof row);
		memcpy(row.name, rec.profile.name, SAT_NAME_MAX);
		row.name[SAT_NAME_MAX - 1] = '\0';
		row.norad = rec.profile.norad;
		if (!rec.has_elements) {
			row.state = SAT_ROW_NO_ORBIT;
		} else {
			row.age_d = age_days(&rec.elements, now);
			row.state = row.age_d > SAT_SVC_OLD_DAYS ? SAT_ROW_OLD : SAT_ROW_WORKING;
		}
		begin_write();
		r.rows[n++] = row;
		if (n > r.count)
			r.count = n;
		end_write();
	}
	begin_write();
	r.count = n;
	end_write();
	w.kind = -1;
}

// Every row worked out again (the observer moved, the lowest pass changed)
static void rows_again(void)
{
	begin_write();
	for (uint8_t i = 0; i < r.count; i++) {
		uint8_t st = r.rows[i].state;
		if (st == SAT_ROW_NEXT || st == SAT_ROW_UP || st == SAT_ROW_NONE)
			r.rows[i].state = SAT_ROW_WORKING;
	}
	r.npasses = 0;
	r.passes_done = false;
	r.npath = 0;
	r.live.ok = false;
	end_write();
	w.kind = -1;
}

// The model for norad, from the store; false when it isn't there or the model refuses it
static bool load_model(uint32_t norad, sat_record_t *rec)
{
	int at = sat_store_find(norad);
	if (at < 0 || !sat_store_read((uint16_t)at, rec) || !rec->has_elements)
		return false;
	if (w.norad == norad && w.kind >= 0)
		return true;
	if (sgp4_init(&w.model, &rec->elements) != SGP4_OK)
		return false;
	w.norad = norad;
	return true;
}

// A pass's way across a sky plot (north up, east right): the bearing from where it rises to where it sets
static uint16_t heading(float aos_az, float los_az)
{
	const double d2r = 3.14159265358979323846 / 180.0;
	double sa, ca, sl, cl;
	sat_sincos((double)aos_az * d2r, &sa, &ca);
	sat_sincos((double)los_az * d2r, &sl, &cl);
	double h = atan2(sl - sa, cl - ca) / d2r;
	return (uint16_t)((int)(h < 0.0 ? h + 360.5 : h + 0.5) % 360);
}

static void set_row(uint8_t i, uint8_t state, const sat_pass_t *p)
{
	uint16_t dir = p != NULL ? heading(p->aos_az, p->los_az) : 0;	// the slow arithmetic before the write
	begin_write();
	sat_row_t *row = &r.rows[i];
	row->state = state;
	if (p != NULL) {
		row->aos = (uint32_t)p->aos;
		row->los = (uint32_t)p->los;
		row->max_el = (int8_t)(p->max_el + 0.5f);
		row->aos_az = (uint16_t)(p->aos_az + 0.5f) % 360u;
		row->los_az = (uint16_t)(p->los_az + 0.5f) % 360u;
		row->dir = dir;
	}
	end_write();
}

// The open satellite's pass list: start it over
static void open_start(void)
{
	uint32_t norad = open_asked;
	open_changed = false;
	sat_record_t rec;
	int at = norad != 0 ? sat_store_find(norad) : -1;
	bool found = at >= 0 && sat_store_read((uint16_t)at, &rec);
	begin_write();
	r.open_norad = norad;
	r.open_found = found;
	if (found)
		r.open_profile = rec.profile;
	r.npasses = 0;
	r.passes_done = !found || !rec.has_elements;
	end_write();
	if (w.kind == 1)
		w.kind = -1;
}

// The tracked pass changed: its path and position again
static void track_start(void)
{
	track_changed = false;
	begin_write();
	r.npath = 0;
	r.live.ok = false;
	end_write();
	w.live_at = 0;
	if (w.kind == 2)
		w.kind = -1;
}

static void put_point(uint8_t i, const sat_look_t *l)
{
	begin_write();
	r.path[i].az = (uint16_t)((int)(l->az + 0.5) % 360);
	r.path[i].el = (int8_t)(l->el < 0.0 ? 0 : l->el + 0.5);
	r.npath = (uint8_t)(i + 1u);
	end_write();
}

// The path's next points (a few runs of the model); false when it is complete
static bool path_step(unsigned *used)
{
	double aos = (double)track_aos, los = (double)track_los;
	for (unsigned k = 0; k < PATH_PER_STEP && r.npath < SAT_SVC_PATH; k++) {
		sat_look_t l;
		double t = aos + (los - aos) * (double)r.npath / (double)(SAT_SVC_PATH - 1);
		(*used)++;
		if (!sat_look_at(&w.model, &w.obs, t, &l)) {
			begin_write();
			r.npath = SAT_SVC_PATH;	// the model failed: no path, the plot shows only the dot
			end_write();
			return false;
		}
		put_point(r.npath, &l);
	}
	return r.npath < SAT_SVC_PATH;
}

// One search step of the current piece: false when the piece is done
static bool step(const sat_svc_in_t *in, unsigned *used)
{
	if (w.kind == 2)
		return path_step(used);
	sat_pass_t p;
	unsigned evals = 0;
	double span = w.end - w.from < STEP_S ? w.end - w.from : STEP_S;
	bool found = span > 0.0 && sat_pass_next(&w.model, &w.obs, w.from, span, (float)in->min_el, &p, &evals);
	*used += evals > 0 ? evals : 1u;
	if (w.kind == 0) {
		if (found) {
			set_row(w.row, p.aos <= in->now ? SAT_ROW_UP : SAT_ROW_NEXT, &p);
			w.el_at[w.row] = 0;
			return false;
		}
		w.from += span;
		if (w.from >= w.end) {
			set_row(w.row, SAT_ROW_NONE, NULL);
			return false;
		}
		return true;
	}
	// The pass list
	if (found) {
		uint16_t dir = heading(p.aos_az, p.los_az);
		begin_write();
		sat_pass_row_t *q = &r.passes[r.npasses++];
		q->aos = (uint32_t)p.aos;
		q->los = (uint32_t)p.los;
		q->max_el = (int8_t)(p.max_el + 0.5f);
		q->aos_az = (uint16_t)(p.aos_az + 0.5f) % 360u;
		q->los_az = (uint16_t)(p.los_az + 0.5f) % 360u;
		q->dir = dir;
		if (r.npasses >= SAT_SVC_PASSES)
			r.passes_done = true;
		end_write();
		w.from = p.los + 60.0;
	} else {
		w.from += span;
	}
	if (w.from >= w.end || r.passes_done) {
		begin_write();
		r.passes_done = true;
		end_write();
		return false;
	}
	return true;
}

// The next piece of work: the pass list, else the first row still to work out. False when there is none
static bool next_piece(const sat_svc_in_t *in)
{
	sat_record_t rec;
	if (track_aos != 0 && r.open_found && r.npath < SAT_SVC_PATH && load_model(r.open_norad, &rec)) {
		w.kind = 2;	// the tracked pass's path first: someone is looking at it
		return true;
	}
	if (r.open_norad != 0 && !r.passes_done) {
		if (!load_model(r.open_norad, &rec)) {
			begin_write();
			r.passes_done = true;
			end_write();
			return false;
		}
		w.kind = 1;
		w.from = r.npasses ? r.passes[r.npasses - 1].los + 60.0 : in->now;
		w.end = in->now + SAT_SVC_SPAN_S;
		return true;
	}
	for (uint8_t i = 0; i < r.count; i++) {
		if (r.rows[i].state != SAT_ROW_WORKING)
			continue;
		if (!load_model(r.rows[i].norad, &rec)) {
			set_row(i, SAT_ROW_BAD, NULL);
			continue;
		}
		w.kind = 0;
		w.row = i;
		w.from = in->now;
		w.end = in->now + SAT_SVC_SPAN_S;
		return true;
	}
	return false;
}

// Rows whose pass has begun or ended, and the elevation of those up now (one run each, every EL_NOW_S)
static void keep_rows(const sat_svc_in_t *in, unsigned *used)
{
	for (uint8_t i = 0; i < r.count; i++) {
		sat_row_t *row = &r.rows[i];
		if ((row->state == SAT_ROW_NEXT || row->state == SAT_ROW_UP) && in->now >= row->los) {
			set_row(i, SAT_ROW_WORKING, NULL);	// over: its next one
			if (w.kind == 0 && w.row == i)
				w.kind = -1;
			continue;
		}
		if (row->state == SAT_ROW_NEXT && in->now >= row->aos)
			set_row(i, SAT_ROW_UP, NULL);
		if (row->state != SAT_ROW_UP || in->now - (double)w.el_at[i] < EL_NOW_S)
			continue;
		sat_record_t rec;
		sat_look_t l;
		int8_t kind = w.kind;
		if (kind >= 0 && w.norad != row->norad)
			continue;	// the model is busy with another piece: next poll
		if (kind < 0 && !load_model(row->norad, &rec))
			continue;
		w.el_at[i] = (uint32_t)in->now;
		(*used)++;
		if (sat_look_at(&w.model, &w.obs, in->now, &l)) {
			begin_write();
			row->el_now = (int8_t)(l.el < 0.0 ? 0 : l.el + 0.5);
			end_write();
		}
	}
	// The pass list's passes that have set are dropped from its head
	begin_write();
	while (r.npasses > 0 && in->now >= r.passes[0].los) {
		memmove(&r.passes[0], &r.passes[1], (size_t)(r.npasses - 1) * sizeof r.passes[0]);
		r.npasses--;
		if (r.passes_done && r.npasses < SAT_SVC_PASSES)
			r.passes_done = false;	// room for one more at the end of the 24 hours
	}
	end_write();
}

// The tracked satellite's position, once a second: one run of the model, and the Doppler for its profile
static void keep_track(const sat_svc_in_t *in, unsigned *used)
{
	if (track_aos == 0)
		return;
	// The screen asks again at every draw; it has gone (the service has no way to hear) once it stops
	if (track_alive) {
		track_alive = false;
		w.alive_at = (uint32_t)in->now;
	} else if (in->now - (double)w.alive_at > TRACK_LEAVE_S) {
		track_aos = track_los = 0;
		track_changed = true;	// its results go
		return;
	}
	if (!r.open_found || in->now - (double)w.live_at < LIVE_S)
		return;
	if (w.kind >= 0 && w.norad != r.open_norad)
		return;	// the model is busy with another piece: next poll
	sat_record_t rec;
	if (w.kind < 0 && !load_model(r.open_norad, &rec))
		return;
	sat_look_t l;
	w.live_at = (uint32_t)in->now;
	(*used)++;
	bool ok = sat_look_at(&w.model, &w.obs, in->now, &l);
	// All the arithmetic first: a write stays a few stores, as short as it can be
	sat_live_t v = { .ok = ok };
	if (ok) {
		v.az = (float)l.az;
		v.el = (float)l.el;
		v.range_km = (float)l.range;
		v.rate_kms = (float)l.range_rate;
		v.rx_hz = r.open_profile.down_hz ? sat_doppler_rx_hz(r.open_profile.down_hz, l.range_rate) : 0;
		v.tx_hz = r.open_profile.up_hz ? sat_doppler_tx_hz(r.open_profile.up_hz, l.range_rate) : 0;
		v.at = (uint32_t)in->now;
	}
	begin_write();
	r.live = v;
	end_write();
}

bool sat_svc_poll(const sat_svc_in_t *in, unsigned budget)
{
	uint8_t state = !in->time_ok ? SAT_SVC_NO_TIME : !in->pos_ok ? SAT_SVC_NO_POSITION : SAT_SVC_READY;
	if (state != r.state) {
		begin_write();
		r.state = state;
		end_write();
		refresh_asked = true;	// the ages were worked out without the time
	}
	bool fresh = refresh_asked || sat_store_generation() != w.generation;
	if (fresh) {
		refresh_asked = false;
		w.generation = sat_store_generation();
		read_list(in->now);
		open_changed = true;	// the open satellite's record may have changed too
	}
	if (open_changed)
		open_start();
	if (track_changed)
		track_start();
	if (state != SAT_SVC_READY)
		return false;

	double dlat = in->lat - w.lat, dlon = in->lon - w.lon;
	if (!w.have_obs || dlat > MOVED_DEG || dlat < -MOVED_DEG || dlon > MOVED_DEG || dlon < -MOVED_DEG ||
	    in->min_el != w.min_el) {
		w.have_obs = true;
		w.lat = in->lat;
		w.lon = in->lon;
		w.min_el = in->min_el;
		sat_observer_set(&w.obs, in->lat, in->lon, in->alt_m);
		rows_again();
		open_start();
		track_start();
	}

	unsigned used = 0;
	keep_rows(in, &used);
	keep_track(in, &used);
	while (used < budget) {
		if (w.kind < 0 && !next_piece(in))
			return false;
		if (!step(in, &used))
			w.kind = -1;
	}
	return true;
}
