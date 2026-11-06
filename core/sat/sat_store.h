// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Stored satellites (docs/spec/satellite.md 6.3): each one's profile and orbital elements in a 128-byte record with its
// own CRC, 32 to a 4 KB sector, in the board's satellite area (hal_flash_map_t.sat_addr / sat_size). A change rewrites
// the record's sector through the journal (core/storage), so a power cut leaves the old sector or the new one. The
// records are read from flash when asked for: no RAM copy. Writes block on the flash; a task that must not wait (the
// UI) hands them to the housekeeping task (storage_job_post).
#ifndef SAT_STORE_H
#define SAT_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include "sat/sat_tle.h"
#include "sat/sat_profile.h"

#define SAT_STORE_RECORD	128u
#define SAT_STORE_MAGIC		0x5441534Fu	// "OSAT": bytes 4F 53 41 54
#define SAT_STORE_LAYOUT	1u

typedef struct {
	sat_profile_t profile;		// name, catalog number, frequencies, tones, arming
	bool has_elements;
	sat_elements_t elements;	// valid with has_elements
} sat_record_t;

typedef enum {
	SAT_STORE_NONE,		// no area on this board (or not usable: the flash, a bad map, the storage region foreign)
	SAT_STORE_OURS,		// every slot free or one of our records
	SAT_STORE_FOREIGN,	// a slot holds something else: read only, never written
} sat_store_state_t;

// The area's state, from a scan of its slots
sat_store_state_t sat_store_state(void);
// Slots in the area (32 a sector), 0 without one
uint16_t sat_store_slots(void);
// Slot i's record; false when it is free or not a valid record
bool sat_store_read(uint16_t slot, sat_record_t *out);
// The slot holding catalog number norad, -1 for none
int sat_store_find(uint32_t norad);
// Records in the area
uint16_t sat_store_count(void);
// Store r: replaces the record with its catalog number, else takes the first free slot. False when full, without an
// area, foreign, or the flash fails
bool sat_store_put(const sat_record_t *r);
// Remove the record with catalog number norad; false when there is none or the write fails
bool sat_store_delete(uint32_t norad);

// Erase the whole area, whatever it holds (the debug link's CLEAR, behind a token: the way out of FOREIGN once the
// flash is backed up). False without an area or when the flash fails
bool sat_store_clear(void);

// Counts every change written (put, delete, clear): a reader that keeps what it read knows to read again
uint32_t sat_store_generation(void);

// A record's 128 bytes and back (little endian, doubles as IEEE 754 binary64), for the store and its tests
void sat_store_encode(const sat_record_t *r, uint8_t out[SAT_STORE_RECORD]);
bool sat_store_decode(const uint8_t in[SAT_STORE_RECORD], sat_record_t *out);

#endif
