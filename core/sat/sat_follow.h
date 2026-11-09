// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// When the receiver follows a satellite's Doppler shift (docs/dev/satellite-doppler-plan.md, docs/spec/satellite.md 4.5):
// given where the receiver is and where it should be, say whether to retune now. No hardware, no RTOS and no clock
// of its own: the caller gives the time, so the host tests run it against a whole pass.
#ifndef SAT_FOLLOW_H
#define SAT_FOLLOW_H

#include <stdint.h>
#include <stdbool.h>

// A 25 kHz FM receiver loses nothing within about 2 kHz and 21 dB at 10 kHz (the retune test, 2026-11-08): retune before
// the error gets there, and not so often that the click of a retune (about 10 ms) is more than a click
#define SAT_FOLLOW_THRESHOLD_HZ	1500u
#define SAT_FOLLOW_MIN_MS	5000u

typedef struct {
	bool on;
	uint32_t last_ms;	// the last retune; 0 = none yet
} sat_follow_t;

// Retune to want_hz now? True when following, the radio is not transmitting (busy), the receiver is at least the
// threshold from want_hz, and the last retune was at least SAT_FOLLOW_MIN_MS ago. A true answer counts as the retune
// (the next is not due until the interval has passed). A retune done another way (the user's #) is noted with
// sat_follow_retuned
bool sat_follow_step(sat_follow_t *f, uint32_t now_ms, uint32_t tuned_hz, uint32_t want_hz, bool busy);
void sat_follow_retuned(sat_follow_t *f, uint32_t now_ms);

#endif
