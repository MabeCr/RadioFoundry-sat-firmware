// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// When the receiver follows a satellite's Doppler shift (docs/dev/satellite-doppler-plan.md, docs/spec/satellite.md 4.5):
// given where the receiver is and where it should be, say whether to retune now. No hardware, no RTOS and no clock
// of its own: the caller gives the time, so the host tests run it against a whole pass.
#ifndef SAT_FOLLOW_H
#define SAT_FOLLOW_H

#include <stdint.h>
#include <stdbool.h>

// A 25 kHz FM receiver loses nothing within about 2 kHz and 21 dB at 10 kHz (the retune test, 2026-11-08), but the
// error runs from 0 up to the threshold between retunes, and on a real SO-50 pass (2026-11-15) a signal 1.5 kHz off was
// too far for comfort: retune at 500 Hz, three times as often (about 36 retunes a pass at 70 cm), each a click of about
// 10 ms that was not heard on air. The minimum gap is under the interval at the pass's fastest Doppler (about 100 Hz a second)
#define SAT_FOLLOW_THRESHOLD_HZ	500u
#define SAT_FOLLOW_MIN_MS	3000u
// How far behind the receive line must be before the Tracking screen colors it as a warning (not following, or a retune
// pending): the distance at which a signal starts to suffer, not the following's own threshold
#define SAT_FOLLOW_WARN_HZ	1500u

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
