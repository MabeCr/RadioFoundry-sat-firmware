// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Receiving element sets by sound (sat_sound_rcv.h)
#include <string.h>
#include "config.h"
#include "sat/sat_sound_rcv.h"

#if CONFIG_SAT_SOUND

// The job a satellite waits for: runs in the task the poster picked, which may block on the flash
static void store_job(void *ctx)
{
	sat_sound_slot_t *s = ctx;
	sat_sound_rcv_t *r = s->rcv;
	sat_sound_apply_t res = sat_sound_apply(&s->item, &r->scratch);
	if (res < SAT_SOUND_APPLY_COUNT && r->st.applied[res] != 0xFFFFu)
		r->st.applied[res]++;
	s->state = SAT_SOUND_SLOT_FREE;
}

static void on_frame(const uint8_t *frame, size_t n, void *ctx)
{
	sat_sound_rcv_t *r = ctx;
	sat_sound_rec_t *rec = &r->rec;

	r->st.frames++;
	switch (sat_sound_rx_frame(&r->tally, frame, n, rec)) {
	case SAT_SOUND_RX_BAD:
		r->st.refused++;
		return;
	case SAT_SOUND_RX_DUP:
		return;
	case SAT_SOUND_RX_NEW:
		break;
	}
	for (uint8_t i = 0; i < SAT_SOUND_QUEUE; i++) {
		sat_sound_slot_t *s = &r->slot[i];
		if (s->state != SAT_SOUND_SLOT_FREE)
			continue;
		memcpy(s->item.name, rec->name, sizeof(s->item.name));
		s->item.el = rec->el;
		s->rcv = r;
		s->state = SAT_SOUND_SLOT_QUEUED;
		return;
	}
	// no room: forget it was heard, so the next copy of it (the sender repeats the list) is taken
	r->st.lost++;
	sat_sound_rx_forget(&r->tally, rec->index);
}

void sat_sound_rcv_init(sat_sound_rcv_t *r)
{
	memset(r, 0, sizeof(*r));
}

bool sat_sound_rcv_start(sat_sound_rcv_t *r)
{
	if (!sat_sound_rcv_idle(r))
		return false;	// the last transfer's satellites are still being stored
	memset(&r->st, 0, sizeof(r->st));
	sat_sound_rx_init(&r->tally);
	r->running = afsk_demod_init(&r->demod, 8000u, on_frame, r);
	return r->running;
}

void sat_sound_rcv_stop(sat_sound_rcv_t *r)
{
	r->running = false;
}

void sat_sound_rcv_audio(sat_sound_rcv_t *r, const int16_t *pcm, size_t n)
{
	if (!r->running)
		return;
	int32_t peak = 0;
	for (size_t i = 0; i < n; i++) {
		int32_t v = pcm[i] < 0 ? -(int32_t)pcm[i] : pcm[i];
		if (v > peak)
			peak = v;
	}
	r->st.level = (uint16_t)(peak > 32767 ? 32767 : peak);
	afsk_demod(&r->demod, pcm, n);
}

void sat_sound_rcv_post(sat_sound_rcv_t *r, sat_sound_post_fn post)
{
	for (uint8_t i = 0; i < SAT_SOUND_QUEUE; i++) {
		sat_sound_slot_t *s = &r->slot[i];
		if (s->state != SAT_SOUND_SLOT_QUEUED)
			continue;
		s->state = SAT_SOUND_SLOT_POSTED;
		if (!post(store_job, s))
			s->state = SAT_SOUND_SLOT_QUEUED;	// try again on the next call
	}
}

bool sat_sound_rcv_idle(const sat_sound_rcv_t *r)
{
	for (uint8_t i = 0; i < SAT_SOUND_QUEUE; i++) {
		if (r->slot[i].state != SAT_SOUND_SLOT_FREE)
			return false;
	}
	return true;
}

void sat_sound_rcv_stats(const sat_sound_rcv_t *r, sat_sound_stats_t *out)
{
	*out = r->st;
	out->count = r->tally.active ? r->tally.count : 0;
	out->heard = r->tally.heard;
	out->fcs_errors = (uint16_t)afsk_demod_fcs_errors(&r->demod);
	out->listening = r->running;
	uint8_t q = 0;
	for (uint8_t i = 0; i < SAT_SOUND_QUEUE; i++) {
		if (r->slot[i].state != SAT_SOUND_SLOT_FREE)
			q++;
	}
	out->queued = q;
}
#endif
