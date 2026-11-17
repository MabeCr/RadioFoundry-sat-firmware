// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Pass alerts: see sat_alert.h
#include <string.h>
#include "sat/sat_alert.h"

static bool near(uint32_t a, uint32_t b)
{
	return (a > b ? a - b : b - a) <= SAT_ALERT_MATCH_S;
}

void sat_alert_init(sat_alert_t *a)
{
	memset(a, 0, sizeof *a);
}

static int find(const sat_alert_pass_t *list, unsigned n, uint32_t norad, uint32_t aos)
{
	for (unsigned i = 0; i < n; i++)
		if (list[i].norad == norad && norad != 0 && near(list[i].aos, aos))
			return (int)i;
	return -1;
}

bool sat_alert_marked(const sat_alert_t *a, uint32_t norad, uint32_t aos)
{
	return find(a->marks, SAT_ALERT_PASS_MARKS, norad, aos) >= 0;
}

bool sat_alert_toggle(sat_alert_t *a, uint32_t norad, uint32_t aos)
{
	int at = find(a->marks, SAT_ALERT_PASS_MARKS, norad, aos);
	if (at >= 0) {
		a->marks[at].norad = 0;
		return false;
	}
	// A free slot, else the one whose pass is earliest (the oldest by the clock)
	unsigned slot = 0;
	for (unsigned i = 0; i < SAT_ALERT_PASS_MARKS; i++) {
		if (a->marks[i].norad == 0) {
			slot = i;
			break;
		}
		if (a->marks[i].aos < a->marks[slot].aos)
			slot = i;
	}
	a->marks[slot].norad = norad;
	a->marks[slot].aos = aos;
	return true;
}

void sat_alert_forget(sat_alert_t *a, uint32_t norad)
{
	for (unsigned i = 0; i < SAT_ALERT_PASS_MARKS; i++)
		if (a->marks[i].norad == norad)
			a->marks[i].norad = 0;
}

bool sat_alert_due(sat_alert_t *a, double now, uint32_t lead_s, uint32_t norad, uint32_t aos, bool always)
{
	if (lead_s == 0 || norad == 0 || (double)aos <= now || (double)aos - now > (double)lead_s)
		return false;
	int at = find(a->marks, SAT_ALERT_PASS_MARKS, norad, aos);
	if (!always && at < 0)
		return false;
	if (find(a->done, SAT_ALERT_DONE, norad, aos) >= 0)
		return false;
	a->done[a->done_next].norad = norad;
	a->done[a->done_next].aos = aos;
	a->done_next = (uint8_t)((a->done_next + 1u) % SAT_ALERT_DONE);
	if (at >= 0)
		a->marks[at].norad = 0;
	return true;
}
