// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Receiving element sets by sound (docs/spec/sound-transfer.md): 8 kHz audio in, satellites stored. The AFSK demodulator
// (aprs/afsk.h) finds the HDLC frames, sat_sound.h checks them and tallies the transfer, and each new satellite waits in a
// small queue for a task that may block on the flash to store it (sat_sound_apply.h): the audio task never waits. No hardware
// or RTOS here: the caller feeds the audio and posts the jobs.
#ifndef SAT_SOUND_RCV_H
#define SAT_SOUND_RCV_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "aprs/afsk.h"
#include "sat/sat_sound.h"
#include "sat/sat_sound_apply.h"

#define SAT_SOUND_QUEUE		4u	// satellites waiting for the store; a frame takes about 1.4 s, a write far less

typedef enum {
	SAT_SOUND_SLOT_FREE = 0,
	SAT_SOUND_SLOT_QUEUED,	// waiting for a job
	SAT_SOUND_SLOT_POSTED,	// a job holds it
} sat_sound_slot_state_t;

struct sat_sound_rcv;

typedef struct {
	volatile uint8_t state;		// sat_sound_slot_state_t: the receiver sets QUEUED and POSTED, the job sets FREE
	struct sat_sound_rcv *rcv;
	sat_sound_item_t item;
} sat_sound_slot_t;

typedef struct {
	uint16_t count;			// satellites in the transfer being heard, 0 before the first frame
	uint16_t heard;			// distinct ones taken so far
	uint16_t frames;		// frames whose FCS was good
	uint16_t refused;		// ... that were not satellite frames (sat_sound_parse said no)
	uint16_t fcs_errors;		// frames the demodulator found with a bad FCS
	uint16_t lost;			// satellites heard but dropped: the queue was full
	uint16_t applied[SAT_SOUND_APPLY_COUNT];	// what the store did with each, by sat_sound_apply_t
	uint16_t level;			// the loudest sample of the last block, 0 to 32767
	uint8_t queued;			// satellites waiting for the store
	bool listening;
} sat_sound_stats_t;

typedef struct sat_sound_rcv {
	bool running;
	afsk_demod_t demod;
	sat_sound_rx_t tally;
	sat_sound_slot_t slot[SAT_SOUND_QUEUE];
	sat_track_t scratch;		// the orbit model's try-out, used by the jobs one at a time
	sat_sound_stats_t st;
} sat_sound_rcv_t;

void sat_sound_rcv_init(sat_sound_rcv_t *r);
// Start listening: a fresh tally and counters. False if the demodulator can't start
bool sat_sound_rcv_start(sat_sound_rcv_t *r);
// Stop listening. Satellites already heard still go to the store
void sat_sound_rcv_stop(sat_sound_rcv_t *r);
// Audio in: n samples at 8 kHz
void sat_sound_rcv_audio(sat_sound_rcv_t *r, const int16_t *pcm, size_t n);

// Hands a job to a task that may block (storage_job_post): false when it can't take it now
typedef bool (*sat_sound_post_fn)(void (*job)(void *ctx), void *ctx);
// Post a job for every satellite waiting; one the poster refuses waits for the next call
void sat_sound_rcv_post(sat_sound_rcv_t *r, sat_sound_post_fn post);
// Nothing waits for the store and no job holds a satellite
bool sat_sound_rcv_idle(const sat_sound_rcv_t *r);
// The counters now
void sat_sound_rcv_stats(const sat_sound_rcv_t *r, sat_sound_stats_t *out);

#endif
