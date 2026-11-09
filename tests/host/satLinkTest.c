// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link's SAT command and sat view section (core/debuglink/debuglink_sat.c, docs/spec/debuglink.md 3.24) on a
// fake clock and a simulated flash: an element set in pieces, the observer, look angles and passes against the
// reference values (sat_ref.h), the bench, the store (save, list, recall, delete), and the refusals
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "radio/retune_test.h"
#include "sat/sat_clock.h"
#include "sat/sat_tle.h"
#include "sim/sim_flash.h"
#include "storage/storage.h"
#include "sat_ref.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t fake_ms, fake_us;

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = 0x40000u,
	.sat_addr = 0x1B000u, .sat_size = 0x1000u,
};

// A receiver for the retune test: after each rx() the level is far from the steady one for settle_time_us (a retune
// dropping the signal), then steady: -70 dBm on the base frequency, -73 on the other. The two first tunes (the test's
// steady levels) are never disturbed. never_in_band makes rx() refuse the frequency
#define BASE_HZ 162400000u
static uint32_t rx_hz, rx_at_us, settle_time_us;
static unsigned rx_calls;
static bool never_in_band;
static uint32_t rx_call_us = 40;	// what an rx() costs on the fake clock

static bool fake_rx(uint32_t hz, radio_bw_t bw, const subtone_t *tone)
{
	(void)bw;
	(void)tone;
	if (never_in_band)
		return false;
	rx_hz = hz;
	rx_at_us = fake_us;
	rx_calls++;
	fake_us += rx_call_us;
	return true;
}

static void fake_status(radio_status_t *st)
{
	bool settled = rx_calls <= 2 || fake_us - rx_at_us >= settle_time_us;
	st->rssi_dbm = settled ? (rx_hz == BASE_HZ ? -70 : -73) : -100;
	st->noise = settled ? 20 : 90;
	st->carrier = settled;
	st->tone_ok = true;
	fake_us += 30;		// a read costs a little
}

static const hal_radio_t fake_radio = { .rx = fake_rx, .status = fake_status };

const board_t board = { .name = "test", .flash = &sim_flash, .radio = &fake_radio };

void hal_delay_ms(uint32_t ms)
{
	fake_us += ms * 1000u;
}

void hal_delay_us(uint32_t us)
{
	fake_us += us;
}

// The radio task, run on the spot: what the request does there (radio_sm.c RREQ_RETUNE_TEST)
static bool task_allows = true;
static uint8_t task_why;
void radio_req_retune_test(void)
{
	retune_test_run(task_allows, task_why);
}

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

// The store: the loaded satellite saved with its own profile and with one given, listed, recalled and deleted
static void test_store(void)
{
	uint8_t req[DL_REQ_MAX];
	CHECK(view_has("sat.store=ours\nsat.slots=32\nsat.stored=0\n"));
	// SO-50, saved with its built-in profile
	load(1);
	req[0] = DL_SAT_OP_SAVE;
	CHECK(cmd(req, 1) == 1 && out[0] == DL_ST_OK);
	// AO-91 with a profile given: other frequencies and a name
	load(2);
	uint8_t *p = req;
	*p++ = DL_SAT_OP_SAVE;
	const uint32_t up = 435250000u, down = 145960000u;
	const uint16_t tones[4] = { 670, 0, 0, 0 };
	memcpy(p, &up, 4);
	memcpy(p + 4, &down, 4);
	memcpy(p + 8, tones, 8);
	memcpy(p + 16, "FOX-1B", 6);
	CHECK(cmd(req, 23) == 1 && out[0] == DL_ST_OK);
	CHECK(view_has("sat.stored=2\n"));
	// A name too long (12 characters), a profile cut short
	CHECK(cmd(req, 29) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(req, 16) == 1 && out[0] == DL_ST_BAD_ARG);
	// A short name, and none, are fine: the ISS's three characters, and a profile for a satellite with no name yet
	memcpy(p + 16, "ISS", 3);
	CHECK(cmd(req, 20) == 1 && out[0] == DL_ST_OK);
	CHECK(cmd(req, 17) == 1 && out[0] == DL_ST_OK);
	memcpy(p + 16, "ELEVEN-CHARS", 11);
	CHECK(cmd(req, 28) == 1 && out[0] == DL_ST_OK);
	memcpy(p + 16, "FOX-1B", 6);
	CHECK(cmd(req, 23) == 1 && out[0] == DL_ST_OK);

	// The list
	req[0] = DL_SAT_OP_STORED;
	req[1] = 0;
	req[2] = 0;
	size_t n = cmd(req, 3);
	CHECK(n == 1 + 1 + 4 + 1 + 4 + 4 + 4 + 8 + 5 && out[0] == DL_ST_OK && out[1] == 1 && u32(out + 2) == 27607);
	CHECK(out[6] == 1 && u32(out + 11) == 145850000u && u16(out + 21) == 744 && memcmp(out + 27, "SO-50", 5) == 0);
	req[1] = 1;
	n = cmd(req, 3);
	CHECK(n == 33 && u32(out + 2) == 43017 && u32(out + 15) == 145960000u && memcmp(out + 27, "FOX-1B", 6) == 0);
	req[1] = 2;
	CHECK(cmd(req, 3) == 2 && out[0] == DL_ST_OK && out[1] == 0);	// free
	req[1] = 200;
	CHECK(cmd(req, 3) == 2 && out[1] == 0);			// past the end

	// Recall SO-50 over the loaded AO-91: its elements and profile come back
	req[0] = DL_SAT_OP_RECALL;
	req[1] = 0;
	CHECK(cmd(req, 3) == 6 && out[0] == DL_ST_OK && out[1] == 0 && u32(out + 2) == 27607);
	CHECK(view_has("sat.norad=27607\n") && view_has("sat.profile=SO-50\n"));
	req[1] = 2;
	CHECK(cmd(req, 3) == 1 && out[0] == DL_ST_UNAVAILABLE);
	// Saved again after the recall: replaced in place, still two
	req[0] = DL_SAT_OP_SAVE;
	CHECK(cmd(req, 1) == 1 && out[0] == DL_ST_OK && view_has("sat.stored=2\n"));

	// Delete
	uint32_t id = 43017;
	req[0] = DL_SAT_OP_DELETE;
	memcpy(req + 1, &id, 4);
	CHECK(cmd(req, 5) == 1 && out[0] == DL_ST_OK && view_has("sat.stored=1\n"));
	CHECK(cmd(req, 5) == 1 && out[0] == DL_ST_UNAVAILABLE);
	// Clear, only with its token
	uint32_t token = 0x43544154u;
	req[0] = DL_SAT_OP_CLEAR;
	memcpy(req + 1, &token, 4);
	CHECK(cmd(req, 5) == 1 && out[0] == DL_ST_REFUSED && view_has("sat.stored=1\n"));
	token = DL_SAT_CLEAR_TOKEN;
	memcpy(req + 1, &token, 4);
	CHECK(cmd(req, 5) == 1 && out[0] == DL_ST_OK && view_has("sat.stored=0\n"));
	load(0);
}

static size_t retune_args(uint8_t *req, uint32_t base, int32_t step, uint8_t count, uint16_t gap, uint8_t wide)
{
	req[0] = DL_SAT_OP_RETUNE_TEST;
	memcpy(req + 1, &base, 4);
	memcpy(req + 5, &step, 4);
	req[9] = count;
	req[10] = (uint8_t)gap;
	req[11] = (uint8_t)(gap >> 8);
	req[12] = wide;
	return 13;
}

static uint16_t i16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

// A result: state, reason, count, levels, noise, then ten bytes a retune
static void retune_test_part(uint8_t *req)
{
	// Nothing yet
	req[0] = DL_SAT_OP_RETUNE_RESULT;
	CHECK(cmd(req, 1) == 2 && out[0] == DL_ST_OK && out[1] == RETUNE_IDLE);
	CHECK(cmd(req, 2) == 1 && out[0] == DL_ST_BAD_ARG);
	// Bad arguments: a wrong length, no count, too many, a gap out of range, too long a test, no step, no frequency
	CHECK(cmd(req, retune_args(req, BASE_HZ, 2000, 4, 100, 1) - 1) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(req, retune_args(req, BASE_HZ, 2000, 0, 100, 1)) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(req, retune_args(req, BASE_HZ, 2000, 33, 100, 1)) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(req, retune_args(req, BASE_HZ, 2000, 4, 19, 1)) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(req, retune_args(req, BASE_HZ, 2000, 32, 400, 1)) == 1 && out[0] == DL_ST_BAD_ARG);	// 12.8 s
	CHECK(cmd(req, retune_args(req, BASE_HZ, 0, 4, 100, 1)) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(req, retune_args(req, 0, 2000, 4, 100, 1)) == 1 && out[0] == DL_ST_BAD_ARG);
	CHECK(cmd(req, retune_args(req, BASE_HZ, 2000, 4, 100, 2)) == 1 && out[0] == DL_ST_BAD_ARG);
	req[0] = DL_SAT_OP_RETUNE_RESULT;
	CHECK(cmd(req, 1) == 2 && out[1] == RETUNE_IDLE);	// none of them started anything

	// A quick settle: the level is back at the second read after each retune
	settle_time_us = 500;
	rx_calls = 0;
	CHECK(cmd(req, retune_args(req, BASE_HZ, 2000, 6, 50, 1)) == 1 && out[0] == DL_ST_OK);
	CHECK(rx_calls == 2 + 6);	// the two steady levels first, then each retune
	req[0] = DL_SAT_OP_RETUNE_RESULT;
	size_t n = cmd(req, 1);
	CHECK(n == 9 + 6 * 10 && out[0] == DL_ST_OK && out[1] == RETUNE_DONE && out[2] == RETUNE_WHY_NONE && out[3] == 6);
	CHECK((int16_t)i16(out + 4) == -70 && (int16_t)i16(out + 6) == -73 && out[8] == 20);
	for (int i = 0; i < 6; i++) {
		const uint8_t *h = out + 9 + i * 10;
		uint32_t settle = u32(h + 2);
		CHECK(i16(h) >= rx_call_us && i16(h) < rx_call_us + 100);		// the rx() call itself
		CHECK(settle != RETUNE_TEST_NEVER && settle >= 500 && settle < 1000);	// the first read after the drop starts the streak
		CHECK((int16_t)i16(h + 6) == -100 && h[8] == 90 && h[9] >= 1 && h[9] <= 5);	// the low reads: level, noise, no carrier
	}
	// Started again before it is asked for: a new test is accepted once the last is done
	// An immediate settle: no read is off
	settle_time_us = 0;
	rx_calls = 0;
	CHECK(cmd(req, retune_args(req, BASE_HZ, -3000, 2, 20, 0)) == 1 && out[0] == DL_ST_OK);
	req[0] = DL_SAT_OP_RETUNE_RESULT;
	n = cmd(req, 1);
	CHECK(n == 9 + 2 * 10 && out[1] == RETUNE_DONE && u32(out + 9 + 2) < 200 && (int16_t)i16(out + 9 + 6) >= -73 && out[9 + 9] == 0);
	// A level that never comes back: the hop says so, after the 100 ms timeout
	settle_time_us = 10000000u;
	rx_calls = 0;
	CHECK(cmd(req, retune_args(req, BASE_HZ, 5000, 1, 20, 1)) == 1 && out[0] == DL_ST_OK);
	req[0] = DL_SAT_OP_RETUNE_RESULT;
	n = cmd(req, 1);
	CHECK(n == 9 + 10 && out[1] == RETUNE_DONE && u32(out + 9 + 2) == RETUNE_TEST_NEVER && out[9 + 9] > 100);
	// The radio task refuses: busy transmitting, not FM
	settle_time_us = 0;
	task_allows = false;
	task_why = RETUNE_WHY_NOT_FM;
	CHECK(cmd(req, retune_args(req, BASE_HZ, 2000, 4, 100, 1)) == 1 && out[0] == DL_ST_OK);
	req[0] = DL_SAT_OP_RETUNE_RESULT;
	n = cmd(req, 1);
	CHECK(n == 9 && out[1] == RETUNE_REFUSED && out[2] == RETUNE_WHY_NOT_FM && out[3] == 0);
	task_allows = true;
	// A frequency the receiver won't take: refused for the band
	never_in_band = true;
	CHECK(cmd(req, retune_args(req, BASE_HZ, 2000, 4, 100, 1)) == 1 && out[0] == DL_ST_OK);
	req[0] = DL_SAT_OP_RETUNE_RESULT;
	CHECK(cmd(req, 1) >= 4 && out[1] == RETUNE_REFUSED && out[2] == RETUNE_WHY_BAND);
	never_in_band = false;
	// A test left queued (the radio task has not run it) blocks another
	task_allows = true;
}

int main(void)
{
	uint8_t req[DL_REQ_MAX];

	sim_flash_create(0x40000u, &map);
	CHECK(storage_init());
	// Nothing loaded: nothing to save
	req[0] = DL_SAT_OP_SAVE;
	CHECK(cmd(req, 1) == 1 && out[0] == DL_ST_UNAVAILABLE);

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

	test_store();
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
	CHECK(view_has("sat.utc_ok=1\n") && view_has("sat.age_h="));
	// The view shows the last LOOK (it never runs the model itself)
	req[0] = DL_SAT_OP_LOOK;
	memset(req + 1, 0, 6);
	CHECK(cmd(req, 7) == 25 && out[0] == DL_ST_OK);
	CHECK(view_has("sat.az_cdeg=") && view_has("sat.rx_hz=") && view_has("sat.look_at="));
	req[0] = DL_SAT_OP_TIME;
	memcpy(req + 1, &now, 4);
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

	// The retune test: receive only, timed on the radio's clock
	retune_test_part(req);

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

	sim_flash_free();
	printf("satLinkTest: %u looks, %u passes, %s\n", looks, passes, failures ? "FAILED" : "passed");
	return failures != 0;
}
