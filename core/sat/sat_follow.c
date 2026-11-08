// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// When the receiver follows a satellite's Doppler shift (sat_follow.h)
#include "sat/sat_follow.h"

void sat_follow_retuned(sat_follow_t *f, uint32_t now_ms)
{
	f->last_ms = now_ms != 0 ? now_ms : 1;	// 0 means none yet
}

bool sat_follow_step(sat_follow_t *f, uint32_t now_ms, uint32_t tuned_hz, uint32_t want_hz, bool busy)
{
	if (!f->on || busy)
		return false;
	uint32_t err = tuned_hz > want_hz ? tuned_hz - want_hz : want_hz - tuned_hz;
	if (err < SAT_FOLLOW_THRESHOLD_HZ)
		return false;
	if (f->last_ms != 0 && now_ms - f->last_ms < SAT_FOLLOW_MIN_MS)
		return false;
	sat_follow_retuned(f, now_ms);
	return true;
}
