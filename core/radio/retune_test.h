// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The retune test (CONFIG_SAT; docs/dev/satellite-doppler-plan.md, docs/spec/debuglink.md 3.24): how long the receiver
// takes to settle after a retune, measured on the radio's own clock. A Doppler correction retunes the receiver every
// few seconds; whether that drops the audio, and for how long, is a property of the radio's synthesizer and chip that
// has to be measured. The receiver is stepped between a base frequency and an offset of it, and after each retune the
// signal level is read until it is back where it settles. Receive only: it never transmits.
// retune_test_queue() takes the arguments from any task (the debug link); the radio task runs retune_test_run() when
// radio_req_retune_test() reaches it, which blocks that task for the length of the test (at most RETUNE_TEST_MAX_MS).
#ifndef RETUNE_TEST_H
#define RETUNE_TEST_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

#define RETUNE_TEST_MAX		32		// retunes in a test
#define RETUNE_TEST_MAX_MS	10000u		// count * gap at most: the radio task is busy this long
#define RETUNE_TEST_NEVER	0xFFFFFFFFu	// retune_hop_t.settle_us: the level never came back

typedef struct {
	uint32_t base_hz;
	int32_t step_hz;	// the second frequency is base + step, either sign
	uint8_t count;		// 1..RETUNE_TEST_MAX, alternating base + step and base
	uint16_t gap_ms;	// 20..1000 between retunes
	uint8_t wide;		// 1: 25 kHz, 0: 12.5 kHz
} retune_test_args_t;

typedef struct {
	uint16_t call_us;	// the rx() call itself
	uint32_t settle_us;	// from the call until the level is within 3 dB of its steady value for three reads
	int16_t min_rssi;	// the lowest level read meanwhile (dBm)
	uint8_t max_noise;	// the highest noise reading meanwhile
	uint8_t lost;		// reads in which the chip said no carrier
} retune_hop_t;

typedef enum { RETUNE_IDLE, RETUNE_QUEUED, RETUNE_RUNNING, RETUNE_DONE, RETUNE_REFUSED } retune_state_t;
// Why a test was refused (retune_result_t.reason)
enum { RETUNE_WHY_NONE, RETUNE_WHY_BUSY, RETUNE_WHY_SCANNING, RETUNE_WHY_NOT_FM, RETUNE_WHY_OTHER_RX, RETUNE_WHY_BAND };

typedef struct {
	uint8_t reason;		// RETUNE_WHY_* when refused
	uint8_t count;		// retunes made
	int16_t ref_base, ref_step;	// the steady levels on the two frequencies (dBm)
	uint8_t noise_base;	// the steady noise reading on the base frequency
	retune_hop_t hop[RETUNE_TEST_MAX];
} retune_result_t;

// Check and keep the arguments (any task). False for out-of-range arguments, or while a test is queued or running
bool retune_test_queue(const retune_test_args_t *a);
// The radio task: run the queued test, or refuse it (allowed false, with the reason). Receive only, FM only
void retune_test_run(bool allowed, uint8_t why);
// The state, and when it is RETUNE_DONE or RETUNE_REFUSED the result: a pointer to the one copy, valid until the next test
// is queued (read the state again after copying, as a new test would overwrite it)
uint8_t retune_test_state(void);
const retune_result_t *retune_test_result(void);

#endif
