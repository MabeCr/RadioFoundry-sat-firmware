// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link's SAT command and sat view section (core/debuglink/debuglink_sat.c, docs/spec/debuglink.md 3.24) on a
// fake clock: an element set in pieces, the observer, look angles and passes against the reference values
// (sat_ref.h), the bench, and the refusals
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "sat/sat_clock.h"
#include "sat/sat_tle.h"
#include "sat_ref.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t fake_ms, fake_us;

uint32_t hal_millis(void)
{
	return fake_ms;
}

// Each read moves 7 us on: the timed sections take a few reads
uint32_t hal_micros(void)
{
	return fake_us += 7;
}

static uint8_t out[DL_REPLY_MAX];

static size_t cmd(const uint8_t *req, uint16_t len)
{
	memset(out, 0xEE, sizeof out);
	return debuglink_cmd_sat(req, len, out, sizeof out);
}

static uint32_t u32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

// A line in two requests, as the host tool sends it
static void send_line(int line, const char *text)
{
	uint8_t req[DL_REQ_MAX];
	for (int off = 0; off < 69; off += 35) {
		int n = off == 0 ? 35 : 34;
		req[0] = DL_SAT_OP_TLE;
		req[1] = (uint8_t)line;
		req[2] = (uint8_t)off;
		memcpy(req + 3, text + off, (size_t)n);
		CHECK(cmd(req, (uint16_t)(3 + n)) == 1 && out[0] == DL_ST_OK);
	}
}

static void load(int i)
{
	send_line(1, ref_tles[i].l1);
	send_line(2, ref_tles[i].l2);
	const uint8_t req[] = { DL_SAT_OP_LOAD };
	CHECK(cmd(req, 1) == 7 && out[0] == DL_ST_OK && out[1] == 0 && out[2] == 0);
}

static void observer(double lat, double lon, int16_t alt)
{
	int32_t la = (int32_t)lround(lat * 1e7), lo = (int32_t)lround(lon * 1e7);
	uint8_t req[11] = { DL_SAT_OP_OBSERVER };
	memcpy(req + 1, &la, 4);
	memcpy(req + 5, &lo, 4);
	memcpy(req + 9, &alt, 2);
	CHECK(cmd(req, sizeof req) == 1 && out[0] == DL_ST_OK);
}

static bool view_has(const char *kv)
{
	char buf[1024];
	dl_text_t t;
	dl_text_init(&t, buf, sizeof buf);
	debuglink_view_sat(&t);
	return strstr(buf, kv) != NULL;
}

int main(void)
{
	uint8_t req[DL_REQ_MAX];

	// Nothing loaded: the view says so, and the requests that need a satellite are unavailable
	CHECK(view_has("sat.built=1\nsat.loaded=0\nsat.utc_ok=0\n"));
	req[0] = DL_SAT_OP_BENCH;
	req[1] = 10;
	req[2] = 0;
	CHECK(cmd(req, 3) == 1 && out[0] == DL_ST_UNAVAILABLE);
	memset(req, 0, sizeof req);
	req[0] = DL_SAT_OP_LOOK;
	req[1] = 1;
	CHECK(cmd(req, 7) == 1 && out[0] == DL_ST_UNAVAILABLE);

	// Bad pieces: a third line, past column 69, nothing at all, an unknown operation
	req[0] = DL_SAT_OP_TLE;
	req[1] = 3;
	req[2] = 0;
	CHECK(cmd(req, 10) == 1 && out[0] == DL_ST_BAD_ARG);
	req[1] = 1;
	req[2] = 60;
	CHECK(cmd(req, 13) == 1 && out[0] == DL_ST_BAD_ARG);	// 60 + 10 > 69
	CHECK(cmd(req, 0) == 1 && out[0] == DL_ST_BAD_ARG);
	req[0] = 99;
	CHECK(cmd(req, 1) == 1 && out[0] == DL_ST_BAD_ARG);

	// A typo: the check digit catches it, and what was loaded stays
	char bad[70];
	memcpy(bad, ref_tles[0].l2, 69);
	bad[20] = bad[20] == '1' ? '2' : '1';
	send_line(1, ref_tles[0].l1);
	send_line(2, bad);
	req[0] = DL_SAT_OP_LOAD;
	CHECK(cmd(req, 1) == 7 && out[0] == DL_ST_OK && out[1] == TLE_ERR_CHECKSUM && u32(out + 3) == 0);
	CHECK(view_has("sat.loaded=0\n"));

	// The ISS and the reference observer
	load(0);
	CHECK(view_has("sat.loaded=1\n") && view_has("sat.norad=25544\n") && view_has("sat.profile=ISS\n"));
	observer(ref_obs[0], ref_obs[1], (int16_t)ref_obs[2]);
	CHECK(view_has("sat.observer=1\nsat.lat=400150000\nsat.lon=-1052700000\nsat.alt_m=1650\n"));
	// No clock yet: no look now, and the view has no position
	memset(req, 0, sizeof req);
	req[0] = DL_SAT_OP_LOOK;
	CHECK(cmd(req, 7) == 1 && out[0] == DL_ST_UNAVAILABLE);
	CHECK(!view_has("sat.az_cdeg="));

	// Look angles at the reference times, to the reply's 0.01 degree
	unsigned looks = 0;
	for (unsigned i = 0; i < COUNT(ref_looks); i++) {
		const ref_look_t *r = &ref_looks[i];
		if (r->tle != 0)
			continue;
		uint32_t s = (uint32_t)r->unix_s;
		uint16_t ms = (uint16_t)lround((r->unix_s - s) * 1000.0);
		req[0] = DL_SAT_OP_LOOK;
		memcpy(req + 1, &s, 4);
		memcpy(req + 5, &ms, 2);
		CHECK(cmd(req, 7) == 25 && out[0] == DL_ST_OK);
		double az = u16(out + 1) / 100.0, el = (int16_t)u16(out + 3) / 100.0;
		double daz = fabs(az - r->az);
		daz = daz > 180.0 ? 360.0 - daz : daz;
		if (r->el > 0.0 && r->el < 85.0)
			CHECK(daz < 0.02);
		CHECK(fabs(el - r->el) < 0.02);
		CHECK(fabs(u32(out + 5) / 1000.0 - r->range) < 0.1);
		CHECK(fabs((int32_t)u32(out + 9) / 1e6 - r->range_rate) < 0.001);
		// The ISS's downlink and uplink with Doppler: 437.800 and 145.990 MHz
		double rr = (int32_t)u32(out + 9) / 1e6;
		CHECK(fabs((double)u32(out + 13) - 437800000.0 * (1.0 - rr / 299792.458)) < 2.0);
		CHECK(fabs((double)u32(out + 17) - 145990000.0 / (1.0 - rr / 299792.458)) < 2.0);
		CHECK(u32(out + 21) > 0);
		looks++;
	}
	CHECK(looks > 100);

	// Passes, from 10 minutes before each rise
	unsigned passes = 0;
	for (unsigned i = 0; i < COUNT(ref_passes); i++) {
		const ref_pass_t *r = &ref_passes[i];
		if (r->tle != 0)
			continue;
		uint32_t from = (uint32_t)r->from;
		uint16_t span = 3600;
		req[0] = DL_SAT_OP_PASS;
		memcpy(req + 1, &from, 4);
		memcpy(req + 5, &span, 2);
		req[7] = 0;
		CHECK(cmd(req, 8) == 26 && out[0] == DL_ST_OK && out[1] == 1);
		CHECK(fabs(u32(out + 2) - r->aos) < 2.0 && fabs(u32(out + 10) - r->los) < 2.0);
		CHECK(fabs(u32(out + 6) - r->tca) < 3.0 && fabs((int16_t)u16(out + 14) / 100.0 - r->max_el) < 0.05);
		CHECK(u16(out + 20) > 10 && u16(out + 20) < 200 && u32(out + 22) > 0);
		passes++;
	}
	CHECK(passes > 5);
	// None that high within the hour; the span is held to an hour
	uint32_t from = (uint32_t)ref_passes[0].from;
	uint16_t span = 3600;
	req[0] = DL_SAT_OP_PASS;
	memcpy(req + 1, &from, 4);
	memcpy(req + 5, &span, 2);
	req[7] = 89;
	CHECK(cmd(req, 8) == 26 && out[0] == DL_ST_OK && out[1] == 0 && u32(out + 2) == 0);
	span = 3601;
	memcpy(req + 5, &span, 2);
	CHECK(cmd(req, 8) == 1 && out[0] == DL_ST_BAD_ARG);

	// The clock from the host: the view then shows the satellite now
	uint32_t now = (uint32_t)ref_looks[3].unix_s;
	req[0] = DL_SAT_OP_TIME;
	memcpy(req + 1, &now, 4);
	fake_ms = 5000;
	CHECK(cmd(req, 5) == 1 && out[0] == DL_ST_OK);
	CHECK(view_has("sat.utc_ok=1\n") && view_has("sat.az_cdeg=") && view_has("sat.rx_hz=") && view_has("sat.age_h="));
	memset(req + 1, 0, 4);
	CHECK(cmd(req, 5) == 1 && out[0] == DL_ST_BAD_ARG);	// 0 is no time

	// The bench: the runs asked for, or 250 ms of them
	req[0] = DL_SAT_OP_BENCH;
	req[1] = 100;
	req[2] = 0;
	CHECK(cmd(req, 3) == 7 && out[0] == DL_ST_OK && u16(out + 1) == 100 && u32(out + 3) >= 700);
	CHECK(view_has("sat.bench_runs=100\n"));
	req[1] = 0xE9;
	req[2] = 0x03;		// 1001
	CHECK(cmd(req, 3) == 1 && out[0] == DL_ST_BAD_ARG);

	// An observer out of range
	int32_t la = 910000000, lo = 0;
	int16_t alt = 0;
	req[0] = DL_SAT_OP_OBSERVER;
	memcpy(req + 1, &la, 4);
	memcpy(req + 5, &lo, 4);
	memcpy(req + 9, &alt, 2);
	CHECK(cmd(req, 11) == 1 && out[0] == DL_ST_BAD_ARG);
	// No GPS in this build
	req[0] = DL_SAT_OP_OBSERVER_GPS;
	CHECK(cmd(req, 1) == 1 && out[0] == DL_ST_UNAVAILABLE);

	printf("satLinkTest: %u looks, %u passes, %s\n", looks, passes, failures ? "FAILED" : "passed");
	return failures != 0;
}
