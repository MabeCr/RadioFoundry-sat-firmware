// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What the radio does with a satellite heard by sound (sat_sound_apply.h)
#include <string.h>
#include "config.h"
#include "sat/sat_sound_apply.h"

#if CONFIG_SAT_SOUND
#include "sat/sat_store.h"

sat_sound_apply_t sat_sound_apply(const sat_sound_item_t *item, sat_track_t *scratch)
{
	uint8_t sgp4;
	sat_record_t r;

	if (sat_store_state() != SAT_STORE_OURS || sat_store_slots() == 0)
		return SAT_SOUND_APPLY_STORE;
	if (!sat_track_set(scratch, &item->el, NULL, &sgp4))
		return SAT_SOUND_APPLY_MODEL;

	int slot = sat_store_find(item->el.norad);
	if (slot >= 0 && sat_store_read((uint16_t)slot, &r)) {
		// The radio reports the epoch rounded to the second: a set within a second of the stored one is the same
		if (r.has_elements) {
			if (item->el.epoch <= r.elements.epoch + 1.0) {
				return item->el.epoch >= r.elements.epoch - 1.0 ? SAT_SOUND_APPLY_CURRENT : SAT_SOUND_APPLY_OLDER;
			}
		}
		r.has_elements = true;
		r.elements = item->el;	// the profile stays as it is
		return sat_store_put(&r) ? SAT_SOUND_APPLY_UPDATED : SAT_SOUND_APPLY_STORE;
	}

	if (sat_store_count() >= sat_store_slots())
		return SAT_SOUND_APPLY_FULL;
	memset(&r, 0, sizeof(r));
	r.has_elements = true;
	r.elements = item->el;
	const sat_profile_t *p = sat_profile_find(item->el.norad);
	if (p != NULL) {
		r.profile = *p;
	} else {
		size_t n = strnlen(item->name, SAT_SOUND_PENDING_NAME);
		if (n > SAT_NAME_MAX - 1u)
			n = SAT_NAME_MAX - 1u;
		memcpy(r.profile.name, item->name, n);
	}
	r.profile.norad = item->el.norad;
	return sat_store_put(&r) ? SAT_SOUND_APPLY_NEW : SAT_SOUND_APPLY_STORE;
}
#endif
