// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Satellite tracking on the debug link (CONFIG_SAT, docs/spec/debuglink.md 3.24, docs/spec/satellite.md): DL_CMD_SAT
// loads an element set (in pieces: a request holds 64 bytes, a TLE 138), sets the observer and the clock, works out a
// look or a pass at a given time and times the model; the sat view section shows the satellite now. The bench for the
// model's speed on a radio without an FPU, and what the HIL test compares with the reference values. Every request
// runs the model in the host link's task, so each is bounded: a pass search to an hour's span, a bench to 250 ms.
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"

#if CONFIG_SAT
#include <string.h>
#include "hal_time.h"
#include "sat/sat_clock.h"
#include "sat/sat_pass.h"
#include "sat/sat_track.h"
#if CONFIG_APRS
#include "aprs/aprs.h"
#include "aprs/aprs_fmt.h"
#endif

#define BENCH_MAX_RUNS	1000u
#define BENCH_MAX_US	250000u		// the host link's task stays this long at most
#define PASS_MAX_SPAN_S	3600u		// about 100 runs of the model

static sat_track_t trk;
static char lines[2][TLE_LINE_LEN];	// the element set as it arrives (DL_SAT_OP_TLE)
static uint32_t last_run_us;		// the model's last run in the view, or in LOOK
static uint16_t bench_runs;
static uint32_t bench_us;

static int32_t rnd(double x)
{
	return (int32_t)(x >= 0.0 ? x + 0.5 : x - 0.5);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint8_t *put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
	return p + 4;
}

static uint8_t *put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	return p + 2;
}

static size_t status(uint8_t *out, uint8_t st)
{
	out[0] = st;
	return 1;
}

// A request's time: unix seconds (+ ms), 0 = the radio's clock now. False when it has none
static bool when(uint32_t unix_s, uint16_t ms, double *t)
{
	if (unix_s != 0) {
		*t = (double)unix_s + (double)ms / 1000.0;
		return true;
	}
	return sat_clock_now(&sat_utc, hal_millis(), t);
}

// noinline: inlined into the view's dispatch (LTO), its frame and the model's would sit under every other section
__attribute__((noinline)) void debuglink_view_sat(dl_text_t *t)
{
	double now = 0.0;
	bool utc = sat_clock_now(&sat_utc, hal_millis(), &now);
	dl_text(t, "sat.built=1\nsat.loaded=%u\nsat.utc_ok=%u\n", trk.loaded, utc);
	if (utc) {
		dl_text(t, "sat.utc=%u\n", (unsigned)now);
	}
	if (trk.loaded) {
		dl_text(t, "sat.norad=%u\nsat.epoch=%u\n", (unsigned)trk.norad, (unsigned)trk.model.epoch);
		dl_text_kv_str(t, "sat.profile", trk.profile ? trk.profile->name : "-");
		if (utc) {
			dl_text(t, "sat.age_h=%d\n", (int)rnd((now - trk.model.epoch) / 3600.0));
		}
	}
	dl_text(t, "sat.observer=%u\n", trk.observer_set);
	if (trk.observer_set) {
		const double r2d = 180.0 / 3.14159265358979323846;
		dl_text(t, "sat.lat=%d\nsat.lon=%d\nsat.alt_m=%d\n", (int)rnd(trk.observer.lat * r2d * 1e7),
			(int)rnd(trk.observer.lon * r2d * 1e7), (int)rnd(trk.observer.alt * 1000.0));
	}
	sat_track_now_t n;
	uint32_t t0 = hal_micros();
	if (utc && sat_track_at(&trk, now, &n)) {
		last_run_us = hal_micros() - t0;
		dl_text(t, "sat.az_cdeg=%d\nsat.el_cdeg=%d\nsat.range_m=%d\nsat.rr_mm_s=%d\n", (int)rnd(n.look.az * 100.0),
			(int)rnd(n.look.el * 100.0), (int)rnd(n.look.range * 1000.0), (int)rnd(n.look.range_rate * 1e6));
		if (trk.profile) {
			dl_text(t, "sat.rx_hz=%u\nsat.tx_hz=%u\n", (unsigned)n.rx_hz, (unsigned)n.tx_hz);
		}
	}
	dl_text(t, "sat.run_us=%u\nsat.bench_runs=%u\nsat.bench_us=%u\n", (unsigned)last_run_us, bench_runs,
		(unsigned)bench_us);
}

static size_t op_tle(const uint8_t *req, uint16_t len, uint8_t *out)
{
	// line u8 (1, 2), offset u8, the text
	if (len < 4 || (req[1] != 1 && req[1] != 2) || req[2] + (len - 3u) > TLE_LINE_LEN)
		return status(out, DL_ST_BAD_ARG);
	memcpy(&lines[req[1] - 1][req[2]], req + 3, len - 3u);
	return status(out, DL_ST_OK);
}

static size_t op_load(uint8_t *out)
{
	uint8_t tle, sgp4;
	bool ok = sat_track_load(&trk, lines[0], lines[1], &tle, &sgp4);
	out[0] = DL_ST_OK;
	out[1] = tle;
	out[2] = sgp4;
	put32(out + 3, ok ? trk.norad : 0u);
	return 7;
}

static size_t op_observer(const uint8_t *req, uint16_t len, uint8_t *out)
{
	// lat i32, lon i32 (1e-7 degree), alt i16 (m)
	if (len != 11)
		return status(out, DL_ST_BAD_ARG);
	int32_t lat = (int32_t)get32(req + 1), lon = (int32_t)get32(req + 5);
	int16_t alt = (int16_t)get16(req + 9);
	if (lat < -900000000 || lat > 900000000 || lon < -1800000000 || lon > 1800000000)
		return status(out, DL_ST_BAD_ARG);
	sat_track_observer(&trk, lat / 1e7, lon / 1e7, alt);
	return status(out, DL_ST_OK);
}

static size_t op_observer_gps(uint8_t *out)
{
#if CONFIG_APRS
	aprs_gps_t g;
	if (!aprs_gps(&g, hal_millis()) || !g.fix)
		return status(out, DL_ST_UNAVAILABLE);
	sat_track_observer(&trk, g.lat / 1e7, g.lon / 1e7, g.alt_m == APRS_ALT_NONE ? 0.0 : (double)g.alt_m);
	return status(out, DL_ST_OK);
#else
	return status(out, DL_ST_UNAVAILABLE);
#endif
}

static size_t op_look(const uint8_t *req, uint16_t len, uint8_t *out)
{
	// unix u32 (0: now), ms u16
	double t;
	sat_track_now_t n;
	if (len != 7)
		return status(out, DL_ST_BAD_ARG);
	if (!when(get32(req + 1), get16(req + 5), &t))
		return status(out, DL_ST_UNAVAILABLE);
	uint32_t t0 = hal_micros();
	if (!sat_track_at(&trk, t, &n))
		return status(out, DL_ST_UNAVAILABLE);
	last_run_us = hal_micros() - t0;
	uint8_t *p = out;
	*p++ = DL_ST_OK;
	p = put16(p, (uint16_t)rnd(n.look.az * 100.0));
	p = put16(p, (uint16_t)(int16_t)rnd(n.look.el * 100.0));
	p = put32(p, (uint32_t)rnd(n.look.range * 1000.0));
	p = put32(p, (uint32_t)rnd(n.look.range_rate * 1e6));
	p = put32(p, n.rx_hz);
	p = put32(p, n.tx_hz);
	p = put32(p, last_run_us);
	return (size_t)(p - out);
}

static size_t op_pass(const uint8_t *req, uint16_t len, uint8_t *out)
{
	// from u32 (0: now), span u16 (s), min_el i8 (degrees)
	double from;
	if (len != 8 || get16(req + 5) == 0 || get16(req + 5) > PASS_MAX_SPAN_S)
		return status(out, DL_ST_BAD_ARG);
	if (!trk.loaded || !trk.observer_set || !when(get32(req + 1), 0, &from))
		return status(out, DL_ST_UNAVAILABLE);
	sat_pass_t p = { 0 };
	unsigned evals = 0;
	uint32_t t0 = hal_micros();
	bool found = sat_pass_next(&trk.model, &trk.observer, from, get16(req + 5), (float)(int8_t)req[7], &p, &evals);
	uint32_t us = hal_micros() - t0;
	uint8_t *q = out;
	*q++ = DL_ST_OK;
	*q++ = found;
	q = put32(q, found ? (uint32_t)rnd(p.aos) : 0u);
	q = put32(q, found ? (uint32_t)rnd(p.tca) : 0u);
	q = put32(q, found ? (uint32_t)rnd(p.los) : 0u);
	q = put16(q, (uint16_t)(int16_t)rnd((double)p.max_el * 100.0));
	q = put16(q, (uint16_t)rnd((double)p.aos_az * 100.0));
	q = put16(q, (uint16_t)rnd((double)p.los_az * 100.0));
	q = put16(q, (uint16_t)(evals > 0xFFFFu ? 0xFFFFu : evals));
	q = put32(q, us);
	return (size_t)(q - out);
}

static size_t op_bench(const uint8_t *req, uint16_t len, uint8_t *out)
{
	// runs u16: the model run that many times a minute apart from the epoch, or for 250 ms, whichever ends first
	if (len != 3 || get16(req + 1) == 0 || get16(req + 1) > BENCH_MAX_RUNS)
		return status(out, DL_ST_BAD_ARG);
	if (!trk.loaded)
		return status(out, DL_ST_UNAVAILABLE);
	uint16_t want = get16(req + 1), n = 0;
	sgp4_state_t st;
	uint32_t t0 = hal_micros(), us = 0;
	while (n < want && us < BENCH_MAX_US) {
		(void)sgp4_propagate(&trk.model, (double)n, &st);
		n++;
		us = hal_micros() - t0;
	}
	bench_runs = n;
	bench_us = us;
	uint8_t *p = out;
	*p++ = DL_ST_OK;
	p = put16(p, n);
	p = put32(p, us);
	return (size_t)(p - out);
}

// noinline for the same reason, under the debug link's dispatch
__attribute__((noinline)) size_t debuglink_cmd_sat(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)cap;
	if (len < 1)
		return status(out, DL_ST_BAD_ARG);
	switch (req[0]) {
	case DL_SAT_OP_TLE:
		return op_tle(req, len, out);
	case DL_SAT_OP_LOAD:
		return len == 1 ? op_load(out) : status(out, DL_ST_BAD_ARG);
	case DL_SAT_OP_OBSERVER:
		return op_observer(req, len, out);
	case DL_SAT_OP_OBSERVER_GPS:
		return len == 1 ? op_observer_gps(out) : status(out, DL_ST_BAD_ARG);
	case DL_SAT_OP_TIME:
		if (len != 5 || get32(req + 1) == 0)
			return status(out, DL_ST_BAD_ARG);
		sat_clock_set_unix(&sat_utc, get32(req + 1), hal_millis());
		return status(out, DL_ST_OK);
	case DL_SAT_OP_LOOK:
		return op_look(req, len, out);
	case DL_SAT_OP_PASS:
		return op_pass(req, len, out);
	case DL_SAT_OP_BENCH:
		return op_bench(req, len, out);
	default:
		return status(out, DL_ST_BAD_ARG);
	}
}

#else

void debuglink_view_sat(dl_text_t *t)
{
	dl_text(t, "sat.built=0\n");
}

size_t debuglink_cmd_sat(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)req;
	(void)len;
	(void)cap;
	out[0] = DL_ST_UNAVAILABLE;
	return 1;
}

#endif
