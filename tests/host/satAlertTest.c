// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Pass alerts (core/sat/sat_alert.c, docs/spec/satellite.md 5.4): when an alert is due for a marked satellite or pass, once
// per pass, never after the rise or before the lead time, and the single-pass marks
#include <stdio.h>
#include "sat/sat_alert.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
	sat_alert_t a;
	sat_alert_init(&a);
	const uint32_t iss = 25544, so50 = 27607, aos = 100000;
	// Nothing marked: no alert, however near the pass
	CHECK(!sat_alert_due(&a, aos - 60.0, 300, iss, aos, false));
	// A satellite marked for every pass: due inside the lead time, not before, not once it has risen, off with 0
	CHECK(!sat_alert_due(&a, aos - 301.0, 300, iss, aos, true));
	CHECK(!sat_alert_due(&a, aos - 60.0, 0, iss, aos, true));
	CHECK(!sat_alert_due(&a, aos + 1.0, 300, iss, aos, true));
	CHECK(!sat_alert_due(&a, aos, 300, iss, aos, true));
	CHECK(sat_alert_due(&a, aos - 300.0, 300, iss, aos, true));	// exactly the lead time
	// Once per pass, though polled every second, and still once if the element set moved the pass by seconds
	CHECK(!sat_alert_due(&a, aos - 299.0, 300, iss, aos, true));
	CHECK(!sat_alert_due(&a, aos - 250.0, 300, iss, aos + 20, true));
	// The next pass, 90 minutes on, alerts again
	CHECK(sat_alert_due(&a, aos + 5400.0 - 200.0, 300, iss, aos + 5400, true));
	// A single pass marked: that pass alerts (once), another of the same satellite does not
	CHECK(!sat_alert_marked(&a, so50, aos));
	CHECK(sat_alert_toggle(&a, so50, aos));
	CHECK(sat_alert_marked(&a, so50, aos) && sat_alert_marked(&a, so50, aos + 30));
	CHECK(!sat_alert_marked(&a, so50, aos + 5400));
	CHECK(!sat_alert_due(&a, aos - 100.0, 300, so50, aos + 5400, false));
	CHECK(sat_alert_due(&a, aos - 100.0, 300, so50, aos, false));
	CHECK(!sat_alert_marked(&a, so50, aos));	// its mark is spent
	CHECK(!sat_alert_due(&a, aos - 50.0, 300, so50, aos, false));
	// Toggled off again
	CHECK(sat_alert_toggle(&a, so50, aos + 7200));
	CHECK(!sat_alert_toggle(&a, so50, aos + 7200));
	CHECK(!sat_alert_marked(&a, so50, aos + 7200));
	// More marks than slots: the earliest pass goes
	for (unsigned i = 0; i < SAT_ALERT_PASS_MARKS; i++)
		CHECK(sat_alert_toggle(&a, 1000u + i, 200000u + i * 10000u));
	CHECK(sat_alert_toggle(&a, 2000, 500000));
	CHECK(!sat_alert_marked(&a, 1000, 200000) && sat_alert_marked(&a, 1001, 210000) && sat_alert_marked(&a, 2000, 500000));
	// A deleted satellite's marks are forgotten
	sat_alert_forget(&a, 2000);
	CHECK(!sat_alert_marked(&a, 2000, 500000));
	// The record of alerts is bounded: the oldest is forgotten after SAT_ALERT_DONE more
	sat_alert_init(&a);
	for (unsigned i = 0; i < SAT_ALERT_DONE + 1u; i++)
		CHECK(sat_alert_due(&a, 1000.0 * i, 300, 7, 1000u * i + 100u, true));
	CHECK(sat_alert_due(&a, 0.0, 300, 7, 100, true));	// the first one's record is gone
	printf("satAlertTest: %s\n", failures ? "FAILED" : "passed");
	return failures != 0;
}
