// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Pass alerts (docs/spec/satellite.md 5.4): which upcoming pass is worth telling the user about, and when. A satellite
// can be marked to alert on every pass (the set the settings keep, given in by the caller) and single passes can be
// marked (kept here, in RAM: forgotten at power off). An alert is due once per pass, a lead time before the rise. No
// hardware, no clock of its own and no RTOS: the caller gives the time.
#ifndef SAT_ALERT_H
#define SAT_ALERT_H

#include <stdint.h>
#include <stdbool.h>

#define SAT_ALERT_ALWAYS	6u	// satellites marked for every pass (the settings' list)
#define SAT_ALERT_PASS_MARKS	4u	// single passes marked at once
#define SAT_ALERT_DONE		4u	// passes alerted on lately (not alerted on again)
#define SAT_ALERT_MATCH_S	600u	// a pass is "the same" as a marked or alerted one when its rise is this close: a new
					// element set moves a pass by seconds

typedef struct {
	uint32_t norad, aos;
} sat_alert_pass_t;

typedef struct {
	sat_alert_pass_t marks[SAT_ALERT_PASS_MARKS];	// norad 0: free
	sat_alert_pass_t done[SAT_ALERT_DONE];
	uint8_t done_next;
} sat_alert_t;

void sat_alert_init(sat_alert_t *a);
// Is this pass marked (a single pass)
bool sat_alert_marked(const sat_alert_t *a, uint32_t norad, uint32_t aos);
// Mark the pass, or take its mark off; true when it is marked now. With every slot taken the oldest mark goes
bool sat_alert_toggle(sat_alert_t *a, uint32_t norad, uint32_t aos);
// Forget a satellite's marks (it was deleted)
void sat_alert_forget(sat_alert_t *a, uint32_t norad);
// Is an alert due for this satellite's next pass (rising at aos, UTC seconds) at time now: lead_s seconds or less before the
// rise, not yet risen, marked (always: the satellite is marked for every pass; else the pass must be) and not alerted on
// already. True once per pass: it records the alert and takes a single mark off. lead_s 0: alerts are off
bool sat_alert_due(sat_alert_t *a, double now, uint32_t lead_s, uint32_t norad, uint32_t aos, bool always);

#endif
