// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Satellite element sets by sound (docs/spec/sound-transfer.md): the frame one satellite travels in, and the bookkeeping
// of a transfer. The frame rides inside an HDLC frame (aprs/hdlc.h) of 1200 baud AFSK (aprs/afsk.h), so the frame's own
// FCS is already checked when a frame gets here; this layer checks what the frame says. No hardware or RTOS here.
#ifndef SAT_SOUND_H
#define SAT_SOUND_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "sat/sat_tle.h"

#define SAT_SOUND_MAGIC		0x54u	// 'T'
#define SAT_SOUND_VERSION	1u	// name and elements
#define SAT_SOUND_VERSION_PROF	2u	// the same, then a profile: what picking the satellite sets (frequencies, tones, arming)
#define SAT_SOUND_NAME_MAX	24u
#define SAT_SOUND_HEAD		6u	// magic, version, transfer id, index, count, name length
#define SAT_SOUND_PROF_LEN	16u	// the profile block of a version 2 frame, all little endian
#define SAT_SOUND_FRAME_MAX	(SAT_SOUND_HEAD + SAT_SOUND_NAME_MAX + 2u * TLE_LINE_LEN + SAT_SOUND_PROF_LEN)
#define SAT_SOUND_SETS_MAX	32u	// the satellite store's slots

// A satellite's profile as a version 2 frame carries it (the fields of sat_profile_t, docs/spec/sound-transfer.md). Checked as
// it is parsed: a frequency is 0 (none) or 100 MHz to 1.3 GHz, a CTCSS tone 0 or 60.0 to 260.0 Hz, an arming carrier at
// most 10 s and the repeater's awake time at most an hour
typedef struct {
	uint32_t up_hz, down_hz;
	uint16_t up_tone, arm_tone;	// CTCSS, Hz x 10
	uint16_t arm_ms, armed_s;
} sat_sound_prof_t;

// One satellite as a frame carries it
typedef struct {
	uint8_t id;		// the transfer it belongs to
	uint8_t index;		// 0 .. count - 1
	uint8_t count;		// satellites in the transfer, 1 .. SAT_SOUND_SETS_MAX
	char name[SAT_SOUND_NAME_MAX + 1];
	char line1[TLE_LINE_LEN + 1];
	char line2[TLE_LINE_LEN + 1];
	sat_elements_t el;	// the two lines parsed
	bool has_prof;		// a version 2 frame with a profile in it (not all zero)
	sat_sound_prof_t prof;
} sat_sound_rec_t;

typedef enum {
	SAT_SOUND_OK = 0,
	SAT_SOUND_ERR_SHORT,	// fewer bytes than a header
	SAT_SOUND_ERR_MAGIC,	// not a satellite frame
	SAT_SOUND_ERR_VERSION,	// a version this radio doesn't read
	SAT_SOUND_ERR_INDEX,	// a count of 0 or over SAT_SOUND_SETS_MAX, or an index at or past it
	SAT_SOUND_ERR_NAME,	// a name over SAT_SOUND_NAME_MAX, or a character outside printable ASCII
	SAT_SOUND_ERR_LENGTH,	// the bytes aren't the header, the name and two lines
	SAT_SOUND_ERR_TLE,	// the lines fail the TLE rules (length, check digits, the same satellite)
	SAT_SOUND_ERR_PROFILE,	// a profile with a value out of range
} sat_sound_err_t;

// The frame for one satellite into out. Returns its length, 0 when an argument is out of range, a line isn't a valid
// TLE, or max is too small
size_t sat_sound_build(uint8_t *out, size_t max, uint8_t id, uint8_t index, uint8_t count, const char *name,
		       const char *line1, const char *line2);
// The same with a profile, as a version 2 frame (p NULL, or all zero: a version 1 frame). 0 also when the profile is out of range
size_t sat_sound_build_profile(uint8_t *out, size_t max, uint8_t id, uint8_t index, uint8_t count, const char *name,
			       const char *line1, const char *line2, const sat_sound_prof_t *p);
// A received frame (without its HDLC FCS) into r, which means something only on SAT_SOUND_OK
sat_sound_err_t sat_sound_parse(const uint8_t *frame, size_t n, sat_sound_rec_t *r);

// One transfer as the radio hears it. Frames can arrive in any order and more than once (the sender repeats the set)
typedef struct {
	bool active;		// a frame of a transfer has been taken
	uint8_t id;
	uint8_t count;
	uint8_t heard;		// distinct satellites taken
	uint32_t seen;		// bit i: satellite i of the transfer was taken
	uint16_t bad;		// frames refused
	uint16_t dups;		// copies of one already taken
} sat_sound_rx_t;

typedef enum {
	SAT_SOUND_RX_BAD = 0,	// refused: r holds nothing useful
	SAT_SOUND_RX_NEW,	// a satellite not heard yet in this transfer: r holds it
	SAT_SOUND_RX_DUP,	// a copy of one already taken: r holds it, nothing to do
} sat_sound_rx_res_t;

void sat_sound_rx_init(sat_sound_rx_t *rx);
// Takes one frame. A frame with a different transfer id, or a different count, starts a new transfer: what was heard
// before is forgotten (its satellites are stored already: each frame stands alone)
sat_sound_rx_res_t sat_sound_rx_frame(sat_sound_rx_t *rx, const uint8_t *frame, size_t n, sat_sound_rec_t *r);
// Take a satellite back out of the tally (its record could not be kept), so a later copy counts as new
void sat_sound_rx_forget(sat_sound_rx_t *rx, uint8_t index);
// Every satellite of the transfer has been taken
bool sat_sound_rx_done(const sat_sound_rx_t *rx);

#endif
