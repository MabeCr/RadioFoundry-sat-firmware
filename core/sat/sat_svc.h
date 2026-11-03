// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The satellite service (docs/spec/satellite.md 5.1): the stored satellites' next passes, and one satellite's passes
// over the next 24 hours, worked out a slice at a time for the screens to read. The orbit model needs more stack than
// the UI's task has, so a task of its own calls sat_svc_poll (core/app/app.c) and the UI only reads the results here,
// from any task: a sequence count around each update, no lock. No RTOS and no board here; the time and the observer
// come in with each poll.
#ifndef SAT_SVC_H
#define SAT_SVC_H

#include <stdint.h>
#include <stdbool.h>
#include "sat/sat_profile.h"

#define SAT_SVC_MAX		32	// satellites listed: a store sector's worth
#define SAT_SVC_PASSES		8	// passes listed for one satellite
#define SAT_SVC_SPAN_S		(24u * 3600u)	// how far ahead passes are looked for
#define SAT_SVC_OLD_DAYS	30	// element sets older than this give no passes on the list (sat_row_t.state OLD)
#define SAT_SVC_MIN_EL_DEFAULT	10	// degrees: the lowest pass listed until the user sets another

typedef enum {
	SAT_ROW_NO_ORBIT,	// a profile without an element set
	SAT_ROW_WORKING,	// its next pass is being worked out
	SAT_ROW_NEXT,		// aos..los is its next pass
	SAT_ROW_UP,		// above the horizon now: aos..los is this pass, el_now where it is
	SAT_ROW_NONE,		// no pass high enough in the next 24 hours
	SAT_ROW_OLD,		// the element set is more than SAT_SVC_OLD_DAYS old: not predicted
	SAT_ROW_BAD,		// the model refuses the element set (deep space, decayed)
} sat_row_state_t;

typedef struct {
	char name[SAT_NAME_MAX];
	uint32_t norad;
	uint8_t state;		// sat_row_state_t
	int8_t max_el;		// degrees, NEXT and UP
	int8_t el_now;		// degrees, UP
	int16_t age_d;		// days since the element set's epoch (NO_ORBIT: 0)
	uint16_t aos_az, los_az;	// degrees
	uint16_t dir;		// its way across the sky, north up and east right: the bearing from the rise to the set
	uint32_t aos, los;	// UTC, Unix seconds
} sat_row_t;

typedef struct {
	uint32_t aos, los;
	int8_t max_el;
	uint16_t aos_az, los_az;
	uint16_t dir;		// as sat_row_t.dir
} sat_pass_row_t;

// What a poll works with
typedef struct {
	bool time_ok;		// the clock is set (UTC)
	double now;		// UTC, Unix seconds
	bool pos_ok;		// the observer is known
	double lat, lon;	// degrees
	double alt_m;
	uint8_t min_el;		// the lowest pass listed, degrees
} sat_svc_in_t;

typedef enum {
	SAT_SVC_NO_TIME,	// no clock yet: nothing is worked out
	SAT_SVC_NO_POSITION,	// no observer: nothing is worked out
	SAT_SVC_READY,
	SAT_SVC_NO_MEMORY,	// its task couldn't start (sat_svc_no_memory): nothing is ever worked out
} sat_svc_state_t;

void sat_svc_init(void);
// The service's task couldn't be started: the screens say so (nothing polls the service after this)
void sat_svc_no_memory(void);
// Work out the list again from the store (a screen opened); the next poll starts over
void sat_svc_refresh(void);
// Satellite norad's passes for the passes screen; 0 closes them
void sat_svc_open(uint32_t norad);
// One slice of work: about budget runs of the model (a pass search may finish past it). True while work remains, so
// the caller polls again soon; false when everything is up to date (poll again in a second or so)
bool sat_svc_poll(const sat_svc_in_t *in, unsigned budget);

// --- The results, from any task ---
uint8_t sat_svc_state(void);	// sat_svc_state_t
uint8_t sat_svc_count(void);	// rows in the list
bool sat_svc_row(uint8_t i, sat_row_t *out);
// The opened satellite's profile (false when none is open or it has gone from the store)
bool sat_svc_profile(sat_profile_t *out);
// The opened satellite's passes so far, earliest first, at most max; *done once all 24 hours are searched
uint8_t sat_svc_passes(sat_pass_row_t *out, uint8_t max, bool *done);
// The same one at a time, for a reader with little stack: how many so far, and pass i
uint8_t sat_svc_pass_count(bool *done);
bool sat_svc_pass(uint8_t i, sat_pass_row_t *out);

#endif
