// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What the radio does with a satellite heard by sound (sat_sound_apply.h)
#include <string.h>
#include "config.h"
#include "sat/sat_sound_apply.h"

#if CONFIG_SAT_SOUND
#include "sat/sat_store.h"

// The frame's profile into a stored one, under the name it has (or, with none, the name the frame gave)
static void fill_profile(sat_profile_t *p, const sat_sound_item_t *item)
{
	p->up_hz = item->prof.up_hz;
	p->down_hz = item->prof.down_hz;
	p->up_tone = item->prof.up_tone;
	p->arm_tone = item->prof.arm_tone;
	p->arm_ms = item->prof.arm_ms;
	p->armed_s = item->prof.armed_s;
	if (p->name[0] == '\0') {
		size_t n = strnlen(item->name, SAT_SOUND_PENDING_NAME);
		if (n > SAT_NAME_MAX - 1u)
			n = SAT_NAME_MAX - 1u;
		memcpy(p->name, item->name, n);
	}
}

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
		bool newer = !r.has_elements || item->el.epoch > r.elements.epoch + 1.0;
		// A profile with no frequencies at all is one nobody set: the frame's fills it. One with any is the user's
		bool fill = item->has_prof && r.profile.up_hz == 0 && r.profile.down_hz == 0;
		if (!newer && !fill)
			return item->el.epoch >= r.elements.epoch - 1.0 ? SAT_SOUND_APPLY_CURRENT : SAT_SOUND_APPLY_OLDER;
		if (newer) {
			r.has_elements = true;
			r.elements = item->el;
		}
		if (fill)
			fill_profile(&r.profile, item);
		return sat_store_put(&r) ? SAT_SOUND_APPLY_UPDATED : SAT_SOUND_APPLY_STORE;
	}

	if (sat_store_count() >= sat_store_slots())
		return SAT_SOUND_APPLY_FULL;
	memset(&r, 0, sizeof(r));
	r.has_elements = true;
	r.elements = item->el;
	const sat_profile_t *p = sat_profile_find(item->el.norad);
	if (item->has_prof) {
		fill_profile(&r.profile, item);
	} else if (p != NULL) {
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
