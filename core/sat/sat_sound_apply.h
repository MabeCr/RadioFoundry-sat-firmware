// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What the radio does with a satellite it heard by sound (docs/spec/sound-transfer.md): the rules of an update, as the debug
// link's `sat update` and the web page's write apply them. A satellite the radio holds is replaced only by a later set of
// elements, keeping its profile (frequencies, tones, arming, name); one it doesn't hold is stored, with the built-in profile
// if the radio knows the catalog number, else with its name and no frequencies. The orbit model must take the elements.
// Writes block on the flash: call it from a task that may wait (the storage job). No hardware or RTOS here.
#ifndef SAT_SOUND_APPLY_H
#define SAT_SOUND_APPLY_H

#include <stdint.h>
#include "sat/sat_sound.h"
#include "sat/sat_tle.h"
#include "sat/sat_track.h"

#define SAT_SOUND_PENDING_NAME	(SAT_SOUND_NAME_MAX + 1u)

// What a heard satellite needs to be stored: its name and its elements
typedef struct {
	char name[SAT_SOUND_PENDING_NAME];
	sat_elements_t el;
} sat_sound_item_t;

typedef enum {
	SAT_SOUND_APPLY_NEW = 0,	// stored: the radio had no such satellite
	SAT_SOUND_APPLY_UPDATED,	// the stored set was older: replaced, its profile kept
	SAT_SOUND_APPLY_CURRENT,	// the stored set has the same epoch: left as it is
	SAT_SOUND_APPLY_OLDER,		// the stored set is later than the one heard: kept
	SAT_SOUND_APPLY_FULL,		// no free place for a new satellite
	SAT_SOUND_APPLY_MODEL,		// the orbit model refuses the elements (deep space, decayed)
	SAT_SOUND_APPLY_STORE,		// no usable store (none, someone else's bytes) or the flash failed
	SAT_SOUND_APPLY_COUNT,
} sat_sound_apply_t;

// scratch: a tracker to try the elements in (about 0.6 KB), so the caller's stack needn't hold one
sat_sound_apply_t sat_sound_apply(const sat_sound_item_t *item, sat_track_t *scratch);

#endif
