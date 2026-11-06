// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Satellite profiles and repeater arming (docs/spec/satellite.md 6). A profile holds what picking a satellite sets:
// uplink and downlink (before Doppler), the uplink's access tone, and an optional arming step for a repeater that
// sleeps until a carrier with a different tone wakes it for a while. The arming is a state machine on the caller's
// millisecond clock: it says when to key and with which tone, and how long the repeater stays awake. No hardware or
// RTOS here: the radio core keys the transmitter.
#ifndef SAT_PROFILE_H
#define SAT_PROFILE_H

#include <stdint.h>
#include <stdbool.h>

#define SAT_NAME_MAX	12	// with the terminator

typedef struct {
	char name[SAT_NAME_MAX];
	uint32_t norad;		// catalog number: the element set the profile tracks with
	uint32_t up_hz;		// uplink, as the satellite hears it
	uint32_t down_hz;	// downlink, as the satellite sends it
	uint16_t up_tone;	// CTCSS on the uplink, Hz x 10 (670 = 67.0 Hz), 0 none
	uint16_t arm_tone;	// CTCSS of the arming carrier, Hz x 10, 0: the repeater needs no arming
	uint16_t arm_ms;	// how long the arming carrier is keyed
	uint16_t armed_s;	// how long the repeater stays awake after it
} sat_profile_t;

// The built-in profiles, a starting set to check against AMSAT's current list before use (satellite.md 6.1)
extern const sat_profile_t sat_profiles_builtin[];
extern const uint8_t sat_profiles_builtin_count;
// A built-in profile by catalog number, NULL for none
const sat_profile_t *sat_profile_find(uint32_t norad);

typedef enum {
	SAT_ARM_IDLE,		// not armed, or the awake time ran out
	SAT_ARM_SENDING,	// the arming carrier is on the air
	SAT_ARM_ARMED,		// the repeater should be awake: contacts use the uplink tone
} sat_arm_state_t;

typedef struct {
	uint8_t state;		// sat_arm_state_t
	bool expired;		// the last arming ran its awake time out (cleared by the next start)
	uint16_t arm_tone, up_tone;
	uint16_t arm_ms, armed_s;
	uint32_t until;		// SENDING: the carrier's end; ARMED: the end of the awake time (ms)
} sat_arm_t;

// What the radio does now
typedef struct {
	bool key;		// transmit the arming carrier
	uint16_t tone;		// the CTCSS to send with any transmission now, Hz x 10, 0 none
	uint16_t armed_left_s;	// ARMED: seconds of awake time left, rounded up
} sat_arm_out_t;

void sat_arm_reset(sat_arm_t *a, const sat_profile_t *p);
// Start the arming carrier at now_ms. False (nothing changes) for a profile without arming, or while it is on the air
bool sat_arm_start(sat_arm_t *a, uint32_t now_ms);
// The carrier stopped before its time (PTT released, the user cancelled, the transmitter refused): not armed
void sat_arm_abort(sat_arm_t *a);
// Run with the clock: the state moves on as time passes
sat_arm_out_t sat_arm_poll(sat_arm_t *a, uint32_t now_ms);

#endif
