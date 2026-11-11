// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Doppler shift of a frequency at a range rate (docs/spec/satellite.md 4.3). Inline so the UI can use it without the
// orbit model: the satellite screen works it out for a downlink the user typed.
#ifndef SAT_DOPPLER_H
#define SAT_DOPPLER_H

#include <stdint.h>

#define SAT_C_KMS	299792.458	// speed of light, km/s

// The frequency to receive a downlink sent on f_hz, and to send on so the satellite hears the uplink on f_hz, at a
// range rate (km/s, positive while it moves away): f (1 - v / c) and f / (1 - v / c), rounded to the hertz
static inline uint32_t sat_doppler_rx_hz(uint32_t f_hz, double range_rate)
{
	return (uint32_t)((double)f_hz * (1.0 - range_rate / SAT_C_KMS) + 0.5);
}

static inline uint32_t sat_doppler_tx_hz(uint32_t f_hz, double range_rate)
{
	return (uint32_t)((double)f_hz / (1.0 - range_rate / SAT_C_KMS) + 0.5);
}

#endif
