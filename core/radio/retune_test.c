// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The retune test (retune_test.h)
#include "config.h"
#if CONFIG_SAT
#include <string.h>
#include "radio/retune_test.h"
#include "board.h"
#include "hal_time.h"

#define LOCK_MS		300u		// after the first tune of a frequency: its level is steady
#define STEADY_READS	8		// reads averaged for a steady level, SAMPLE_MS apart
#define SAMPLE_MS	5u
#define POLL_US		200u		// between the reads that follow a retune
#define POLL_TIMEOUT_US	100000u		// give up on a retune that never settles
#define WITHIN_DB	3		// settled: this close to the steady level
#define STREAK		3		// ... this many reads in a row

static volatile uint8_t state = RETUNE_IDLE;
static retune_test_args_t args;
static retune_result_t result;

bool retune_test_queue(const retune_test_args_t *a)
{
	if (state == RETUNE_QUEUED || state == RETUNE_RUNNING)
		return false;
	if (a->count == 0 || a->count > RETUNE_TEST_MAX || a->gap_ms < 20 || a->gap_ms > 1000 || a->wide > 1 ||
	    (uint32_t)a->count * a->gap_ms > RETUNE_TEST_MAX_MS || a->step_hz == 0 || a->step_hz > 100000 ||
	    a->step_hz < -100000 || a->base_hz == 0)
		return false;
	args = *a;
	memset(&result, 0, sizeof result);
	state = RETUNE_QUEUED;
	return true;
}

uint8_t retune_test_state(void)
{
	return state;
}

const retune_result_t *retune_test_result(void)
{
	return &result;
}

static const subtone_t no_tone = { TONE_NONE, 0 };

// The steady level on f: tuned, left to lock, then the average of a few reads. *noise gets the highest noise read
static bool steady(uint32_t f, radio_bw_t bw, int16_t *level, uint8_t *noise)
{
	if (!board.radio->rx(f, bw, &no_tone))
		return false;
	hal_delay_ms(LOCK_MS);
	int32_t sum = 0;
	uint8_t nz = 0;
	for (int i = 0; i < STEADY_READS; i++) {
		radio_status_t st;
		board.radio->status(&st);
		sum += st.rssi_dbm;
		if (st.noise > nz)
			nz = st.noise;
		hal_delay_ms(SAMPLE_MS);
	}
	*level = (int16_t)(sum / STEADY_READS);
	*noise = nz;
	return true;
}

static int32_t absdiff(int32_t a, int32_t b)
{
	return a > b ? a - b : b - a;
}

void retune_test_run(bool allowed, uint8_t why)
{
	if (state != RETUNE_QUEUED)
		return;
	if (!allowed) {
		result.reason = why;
		state = RETUNE_REFUSED;
		return;
	}
	state = RETUNE_RUNNING;
	radio_bw_t bw = args.wide ? RADIO_BW_25K : RADIO_BW_12K5;
	int64_t hi64 = (int64_t)args.base_hz + args.step_hz;
	uint32_t hi = hi64 > 0 && hi64 < 0xFFFFFFFFll ? (uint32_t)hi64 : 0;
	uint8_t noise_hi = 0;
	// Both frequencies' steady levels first, ending on the base: the level at a frequency is what a settled read is
	// compared with (a carrier off the middle of the channel reads a little lower)
	if (hi == 0 || !steady(hi, bw, &result.ref_step, &noise_hi) ||
	    !steady(args.base_hz, bw, &result.ref_base, &result.noise_base)) {
		result.reason = RETUNE_WHY_BAND;
		state = RETUNE_REFUSED;
		return;
	}
	for (uint8_t i = 0; i < args.count; i++) {
		uint32_t f = (i & 1u) == 0 ? hi : args.base_hz;
		int16_t ref = (i & 1u) == 0 ? result.ref_step : result.ref_base;
		retune_hop_t *h = &result.hop[i];
		uint32_t t0 = hal_micros();
		board.radio->rx(f, bw, &no_tone);
		uint32_t called = hal_micros() - t0;
		h->call_us = called > 0xFFFFu ? 0xFFFFu : (uint16_t)called;
		h->settle_us = RETUNE_TEST_NEVER;
		h->min_rssi = INT16_MAX;
		uint32_t streak_from = 0;
		int streak = 0;
		for (;;) {
			radio_status_t st;
			board.radio->status(&st);
			uint32_t at = hal_micros() - t0;
			if (st.rssi_dbm < h->min_rssi)
				h->min_rssi = st.rssi_dbm;
			if (st.noise > h->max_noise)
				h->max_noise = st.noise;
			if (!st.carrier && h->lost < 255)
				h->lost++;
			if (absdiff(st.rssi_dbm, ref) <= WITHIN_DB) {
				if (streak++ == 0)
					streak_from = at;
				if (streak >= STREAK) {
					h->settle_us = streak_from;
					break;
				}
			} else {
				streak = 0;
			}
			if (at > POLL_TIMEOUT_US)
				break;
			hal_delay_us(POLL_US);
		}
		result.count = (uint8_t)(i + 1u);
		hal_delay_ms(args.gap_ms);
	}
	state = RETUNE_DONE;
}
#endif
