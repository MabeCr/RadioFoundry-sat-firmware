// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Stored scan lists (docs/spec/scan-list.md): a named list of channel numbers from any zone, in a 256-byte record with
// its own CRC, 16 to a 4 KB sector, in the board's scan list area (hal_flash_map_t.scan_addr / scan_size). A change
// rewrites the record's sector through the journal (core/storage), so a power cut leaves the old sector or the new
// one. The records are read from flash when asked for: no RAM copy. Writes block on the flash; a task that must not
// wait (the UI) hands them to the housekeeping task (storage_job_post).
//
// A list is a zone_t (codeplug.h): the zone helpers in zone_edit.h that change one in RAM serve a scan list too. A list
// is named by its slot, the way a zone is by its index: deleting one leaves a free slot, and the lists after it keep
// their numbers. A channel number the codeplug no longer holds stays in the list, and the scan skips it.
#ifndef SCAN_LIST_H
#define SCAN_LIST_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"

#define SCAN_LIST_RECORD	256u
#define SCAN_LIST_MAGIC		0x4C43534Fu	// "OSCL": bytes 4F 53 43 4C
#define SCAN_LIST_LAYOUT	1u
#define SCAN_LIST_MAX		CODEPLUG_ZONE_MAX	// channels in a list

typedef zone_t scan_list_t;

// A list's vote scan options (docs/spec/vote-scan.md 3), kept in the record's flags byte and reserved bytes 184-186. Every
// list has them, 0 for a plain list; a change to the list's channels or name keeps them, in a build without vote scan
// too. A field left 0 reads as its default (the _of helpers)
#define SCAN_VOTE_ON		0x01u	// flags: the list is a vote list
#define SCAN_VOTE_NO_PEEK	0x02u	// flags: no peeks at other sites during a call
#define SCAN_VOTE_FIXED_TX	0x04u	// flags: PTT transmits on member tx_member, else on the voted site
#define SCAN_VOTE_FLAGS		0x07u	// the bits this layout knows
#define SCAN_VOTE_THRESHOLD_DEFAULT	(-90)	// dBm
#define SCAN_VOTE_THRESHOLD_MIN		(-120)
#define SCAN_VOTE_THRESHOLD_MAX		(-50)
#define SCAN_VOTE_MARGIN_DEFAULT	6	// dB
#define SCAN_VOTE_MARGIN_MAX		20
typedef struct {
	uint8_t flags;		// SCAN_VOTE_*
	int8_t threshold_dbm;	// 0 = SCAN_VOTE_THRESHOLD_DEFAULT
	uint8_t margin_db;	// 0 = SCAN_VOTE_MARGIN_DEFAULT
	uint8_t tx_member;	// with SCAN_VOTE_FIXED_TX: the member PTT uses (0-based)
	uint16_t link;		// the channel whose Scan key starts this list (any channel, a member or not); 0: none. Bytes 188-189
} scan_list_vote_t;

static inline int scan_vote_threshold_of(const scan_list_vote_t *v)
{
	int t = v->threshold_dbm;
	return t == 0 ? SCAN_VOTE_THRESHOLD_DEFAULT : t < SCAN_VOTE_THRESHOLD_MIN ? SCAN_VOTE_THRESHOLD_MIN :
	       t > SCAN_VOTE_THRESHOLD_MAX ? SCAN_VOTE_THRESHOLD_MAX : t;
}
static inline int scan_vote_margin_of(const scan_list_vote_t *v)
{
	return v->margin_db == 0 ? SCAN_VOTE_MARGIN_DEFAULT : v->margin_db > SCAN_VOTE_MARGIN_MAX ? SCAN_VOTE_MARGIN_MAX :
	       v->margin_db;
}

typedef enum {
	SCAN_LIST_NONE,		// no area on this board (or not usable: the flash, a bad map, the storage region foreign)
	SCAN_LIST_OURS,		// every slot free or one of our records
	SCAN_LIST_FOREIGN,	// a slot holds something else: read only, never written
} scan_list_state_t;

// The area's state, from a scan of its slots
scan_list_state_t scan_list_state(void);
// Slots in the area (16 a sector), 0 without one
uint16_t scan_list_slots(void);
// Slot i's list; false when it is free or not a valid record
bool scan_list_read(uint16_t slot, scan_list_t *out);
// Lists in the area
uint16_t scan_list_count(void);
// Slot i's vote options; false when it holds no valid list (out then all 0: a plain list)
bool scan_list_read_vote(uint16_t slot, scan_list_vote_t *out);
// The slot of the list linked to channel (scan_list_vote_t.link), skipping skip_slot (-1: none): -1 when no list is
// (the lowest slot when several are: the options screens never make that, a record from elsewhere might)
int scan_list_find_link(uint16_t channel, int skip_slot);
// Set slot i's vote options, the list itself unchanged. False when it holds no valid list, the area is foreign, the
// link is another list's channel (one list to a channel), or the flash fails
bool scan_list_write_vote(uint16_t slot, const scan_list_vote_t *v);
// Store l in slot, replacing what is there; the slot's vote options stay. False for a slot outside the area, a list that isn't valid (no name, more
// than SCAN_LIST_MAX channels, channel number 0 or one twice), a foreign area, or when the flash fails
bool scan_list_write(uint16_t slot, const scan_list_t *l);
// Store l in the first free slot: the slot, or -1 when full or when scan_list_write would refuse it
int scan_list_add(const scan_list_t *l);
// Free a slot; false when it holds no list or the write fails
bool scan_list_delete(uint16_t slot);

// Erase the whole area, whatever it holds (the way out of FOREIGN once the flash is backed up). False without an area
// or when the flash fails
bool scan_list_clear(void);

// Counts every change written (write, add, delete, clear): a reader that keeps what it read knows to read again
uint32_t scan_list_generation(void);

// A record's 256 bytes and back (little endian), for the store, the storage claim and the tests. Encode returns false
// for a list that isn't valid (see scan_list_write)
bool scan_list_encode(const scan_list_t *l, uint8_t out[SCAN_LIST_RECORD]);
bool scan_list_decode(const uint8_t in[SCAN_LIST_RECORD], scan_list_t *out);
// A record's vote options: put them in an encoded record (its CRC made again), take them out of a decoded one. Flag bits
// this layout doesn't know are dropped on the way in and ignored on the way out
void scan_list_put_vote(uint8_t rec[SCAN_LIST_RECORD], const scan_list_vote_t *v);
void scan_list_get_vote(const uint8_t rec[SCAN_LIST_RECORD], scan_list_vote_t *out);

#endif
