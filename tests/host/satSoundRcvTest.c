// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Host tests for receiving element sets by sound (core/sat/sat_sound_apply.c and sat_sound_rcv.c, docs/spec/sound-transfer.md)
 * on the simulated flash: the rules of an update (new, updated with the profile kept, current, older, full, refused by the
 * orbit model, no store), and whole transfers from audio to the stored records, 32 satellites twice, including a store job
 * that is slow to take the queue, a second transfer of the same sets and frames that are not satellites.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage/storage.h"
#include "sat/sat_store.h"
#include "sat/sat_sound_rcv.h"
#include "aprs/hdlc.h"
#include "sim/sim_flash.h"
#include "sat_ref.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

#define FLASH_SIZE	0x40000u
#define SAT_ADDR	0x1B000u

static hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = FLASH_SIZE,
	.sat_addr = SAT_ADDR, .sat_size = 0x2000u,
};

const board_t board = { .name = "test", .flash = &sim_flash };

static void boot(void)
{
	sim_flash_power_on();
	CHECK(storage_init());
}

static sat_sound_rcv_t rcv;	// about 3 KB: not on the stack

static sat_sound_item_t item_of(int tle)
{
	sat_sound_item_t it;
	memset(&it, 0, sizeof it);
	CHECK(tle_parse(ref_tles[tle].l1, ref_tles[tle].l2, &it.el) == TLE_OK);
	snprintf(it.name, sizeof it.name, "%s", ref_tles[tle].name);
	return it;
}

static void test_apply(void)
{
	sat_track_t *scratch = &rcv.scratch;
	sat_record_t r;
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	CHECK(sat_store_state() == SAT_STORE_OURS);

	// a satellite the radio doesn't hold is stored, with its built-in profile when the radio knows it
	sat_sound_item_t iss = item_of(0), so50 = item_of(1), unk = item_of(3);
	CHECK(sat_sound_apply(&iss, scratch) == SAT_SOUND_APPLY_NEW);
	CHECK(sat_store_read((uint16_t)sat_store_find(25544), &r) && r.has_elements && r.profile.norad == 25544 && r.profile.up_hz != 0 &&
	      memcmp(&r.elements.epoch, &iss.el.epoch, 8) == 0);
	// one it doesn't know takes its name and no frequencies
	strcpy(unk.name, "A-LONG-NAME-OVER-ELEVEN");
	unk.el.no = 15.5;
	CHECK(sat_sound_apply(&unk, scratch) == SAT_SOUND_APPLY_NEW);
	CHECK(sat_store_read((uint16_t)sat_store_find(99001), &r) && strcmp(r.profile.name, "A-LONG-NAME") == 0 && r.profile.up_hz == 0 &&
	      r.profile.down_hz == 0);

	// the same set again: left as it is; within a second is the same
	CHECK(sat_sound_apply(&iss, scratch) == SAT_SOUND_APPLY_CURRENT);
	sat_sound_item_t near = iss;
	near.el.epoch += 0.9;
	CHECK(sat_sound_apply(&near, scratch) == SAT_SOUND_APPLY_CURRENT);
	// a later set replaces it and the profile the user set stays
	CHECK(sat_store_read((uint16_t)sat_store_find(25544), &r));
	r.profile.up_hz = 145999000u;
	strcpy(r.profile.name, "MY-ISS");
	CHECK(sat_store_put(&r));
	sat_sound_item_t newer = iss;
	newer.el.epoch += 86400.0;
	newer.el.mo = 12.5;
	CHECK(sat_sound_apply(&newer, scratch) == SAT_SOUND_APPLY_UPDATED);
	CHECK(sat_store_read((uint16_t)sat_store_find(25544), &r) && r.profile.up_hz == 145999000u && strcmp(r.profile.name, "MY-ISS") == 0 &&
	      r.elements.mo == 12.5 && r.elements.epoch == newer.el.epoch);
	// an older set than the stored one is kept out
	CHECK(sat_sound_apply(&iss, scratch) == SAT_SOUND_APPLY_OLDER);
	CHECK(sat_store_read((uint16_t)sat_store_find(25544), &r) && r.elements.mo == 12.5);
	// a profile held without elements takes them
	sat_record_t bare = { .profile = { .name = "SO-50", .norad = 27607, .down_hz = 436795000u } };
	CHECK(sat_store_put(&bare));
	CHECK(sat_sound_apply(&so50, scratch) == SAT_SOUND_APPLY_UPDATED);
	CHECK(sat_store_read((uint16_t)sat_store_find(27607), &r) && r.has_elements && r.profile.down_hz == 436795000u);

	// the orbit model must take it: a period of 12 hours is deep space
	sat_sound_item_t deep = item_of(2);
	deep.el.no = 2.0;
	CHECK(sat_sound_apply(&deep, scratch) == SAT_SOUND_APPLY_MODEL && sat_store_find(43017) == -1);

	// full: a new satellite has nowhere to go, a replacement still does
	for (uint32_t n = 200000; sat_store_count() < sat_store_slots(); n++) {
		sat_record_t f = { .profile = { .name = "FILL", .norad = n } };
		CHECK(sat_store_put(&f));
	}
	sat_sound_item_t ao91 = item_of(2);
	CHECK(sat_sound_apply(&ao91, scratch) == SAT_SOUND_APPLY_FULL);
	CHECK(sat_sound_apply(&newer, scratch) == SAT_SOUND_APPLY_CURRENT);

	// someone else's bytes in the area: read only, nothing written
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	sat_record_t keep = { .profile = { .name = "KEEP", .norad = 55555 } };
	CHECK(sat_store_put(&keep));
	sim_flash_data[SAT_ADDR + 5u * SAT_STORE_RECORD + 7u] = 0x12;
	CHECK(sat_store_state() == SAT_STORE_FOREIGN);
	CHECK(sat_sound_apply(&iss, scratch) == SAT_SOUND_APPLY_STORE);
	// a board without a satellite area
	hal_flash_map_t none = map;
	none.sat_size = 0;
	sim_flash_create(FLASH_SIZE, &none);
	boot();
	CHECK(sat_sound_apply(&iss, scratch) == SAT_SOUND_APPLY_STORE);
	CHECK(sim_flash_violations() == 0);
}

// --- a profile in the frame ---

static void test_profile(void)
{
	sat_track_t *scratch = &rcv.scratch;
	sat_record_t r;
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	CHECK(sat_store_state() == SAT_STORE_OURS);

	// a new satellite takes the frame's profile, and it beats the radio's built-in one
	sat_sound_item_t iss = item_of(0);
	iss.has_prof = true;
	iss.prof = (sat_sound_prof_t){ .up_hz = 145990000u, .down_hz = 437800000u, .up_tone = 1000, .arm_tone = 744, .arm_ms = 2000, .armed_s = 600 };
	CHECK(sat_sound_apply(&iss, scratch) == SAT_SOUND_APPLY_NEW);
	CHECK(sat_store_read((uint16_t)sat_store_find(25544), &r) && r.profile.up_hz == 145990000u && r.profile.down_hz == 437800000u &&
	      r.profile.up_tone == 1000 && r.profile.arm_tone == 744 && r.profile.arm_ms == 2000 && r.profile.armed_s == 600 &&
	      r.profile.norad == 25544);
	// one the radio doesn't know gets frequencies too, under the frame's name
	sat_sound_item_t unk = item_of(3);
	strcpy(unk.name, "TEVEL2-1");
	unk.has_prof = true;
	unk.prof = (sat_sound_prof_t){ .up_hz = 145970000u, .down_hz = 436400000u };
	CHECK(sat_sound_apply(&unk, scratch) == SAT_SOUND_APPLY_NEW);
	CHECK(sat_store_read((uint16_t)sat_store_find(99001), &r) && strcmp(r.profile.name, "TEVEL2-1") == 0 && r.profile.up_hz == 145970000u &&
	      r.profile.down_hz == 436400000u && r.profile.up_tone == 0);

	// a profile the user has set stays: the same set, and a later one, leave it
	CHECK(sat_store_read((uint16_t)sat_store_find(25544), &r));
	r.profile.up_hz = 145999000u;
	CHECK(sat_store_put(&r));
	sat_sound_item_t newer = iss;
	newer.el.epoch += 86400.0;
	newer.prof.up_hz = 145123000u;
	CHECK(sat_sound_apply(&newer, scratch) == SAT_SOUND_APPLY_UPDATED);
	CHECK(sat_store_read((uint16_t)sat_store_find(25544), &r) && r.profile.up_hz == 145999000u && r.elements.epoch == newer.el.epoch);
	CHECK(sat_sound_apply(&iss, scratch) == SAT_SOUND_APPLY_OLDER);
	CHECK(sat_store_read((uint16_t)sat_store_find(25544), &r) && r.profile.up_hz == 145999000u);

	// a satellite held with no frequencies (loaded by name over the cable) has them filled in, the elements as they are
	sat_sound_item_t so50 = item_of(1);
	so50.has_prof = false;
	CHECK(sat_sound_apply(&so50, scratch) == SAT_SOUND_APPLY_NEW);
	CHECK(sat_store_read((uint16_t)sat_store_find(27607), &r) && r.profile.down_hz == 436795000u);	// the built-in one
	sat_record_t blank = { .has_elements = true, .elements = so50.el, .profile = { .name = "SO-50", .norad = 27607 } };
	CHECK(sat_store_put(&blank));
	so50.has_prof = true;
	so50.prof = (sat_sound_prof_t){ .up_hz = 145850000u, .down_hz = 436795000u, .up_tone = 670, .arm_tone = 744, .arm_ms = 2000, .armed_s = 600 };
	CHECK(sat_sound_apply(&so50, scratch) == SAT_SOUND_APPLY_UPDATED);
	CHECK(sat_store_read((uint16_t)sat_store_find(27607), &r) && r.profile.up_hz == 145850000u && r.profile.arm_tone == 744 &&
	      r.profile.armed_s == 600 && strcmp(r.profile.name, "SO-50") == 0 && memcmp(&r.elements.epoch, &so50.el.epoch, 8) == 0);
	// and once it has them, the same frame again changes nothing
	CHECK(sat_sound_apply(&so50, scratch) == SAT_SOUND_APPLY_CURRENT);
	// a frame without a profile fills nothing in
	sat_record_t blank2 = { .has_elements = true, .elements = item_of(2).el, .profile = { .name = "FOX", .norad = 43017 } };
	CHECK(sat_store_put(&blank2));
	sat_sound_item_t ao91 = item_of(2);
	CHECK(sat_sound_apply(&ao91, scratch) == SAT_SOUND_APPLY_CURRENT);
	CHECK(sat_store_read((uint16_t)sat_store_find(43017), &r) && r.profile.up_hz == 0 && r.profile.down_hz == 0);
	CHECK(sim_flash_violations() == 0);
}

// --- audio to records ---

static void make_tle(unsigned norad, char *l1, char *l2)
{
	strcpy(l1, "1 25544U 98067A   26300.51782528  .00016717  00000-0  30144-3 0  9990");
	strcpy(l2, "2 25544  51.6416 247.4627 0006703 130.5360 325.0288 15.49815361474550");
	char num[8];
	snprintf(num, sizeof(num), "%05u", norad);
	memcpy(l1 + 2, num, 5);
	memcpy(l2 + 2, num, 5);
	l1[68] = (char)('0' + tle_checksum(l1));
	l2[68] = (char)('0' + tle_checksum(l2));
}

static bool post_ok(void (*job)(void *), void *ctx)
{
	job(ctx);	// the housekeeping task, at once
	return true;
}

static bool refuse_all;
static bool post_slow(void (*job)(void *), void *ctx)
{
	if (refuse_all)
		return false;
	job(ctx);
	return true;
}

// A whole transfer as 8 kHz audio: every frame, the list `passes` times, fed to the receiver in 20 ms blocks with the
// store jobs posted after each block. frame_hook, if set, may replace a satellite's frame
static void send(unsigned id, unsigned sats, unsigned passes, sat_sound_post_fn post, unsigned garbage_every)
{
	static uint8_t tones[1024], frame[SAT_SOUND_FRAME_MAX + 4];
	static int16_t pcm[24000];
	char l1[80], l2[80], name[32];
	for (unsigned p = 0; p < passes; p++) {
		for (unsigned i = 0; i < sats; i++) {
			size_t n;
			if (garbage_every != 0 && i % garbage_every == 0) {
				// a valid HDLC frame that is not a satellite frame
				for (size_t k = 0; k < 90; k++)
					frame[k] = (uint8_t)(k * 7 + 3);
				n = 90;
			} else {
				make_tle(40000u + i, l1, l2);
				snprintf(name, sizeof(name), "SAT-%02u", i);
				n = sat_sound_build(frame, sizeof(frame), (uint8_t)id, (uint8_t)i, (uint8_t)sats, name, l1, l2);
				CHECK(n != 0);
			}
			hdlc_enc_t e;
			hdlc_enc_init(&e, tones, sizeof(tones));
			hdlc_enc_flags(&e, 30);
			hdlc_enc_frame(&e, frame, n);
			hdlc_enc_flags(&e, 3);
			afsk_mod_t m;
			afsk_mod_init(&m, 8000);
			size_t k = afsk_mod(&m, tones, e.nbits, 16000, pcm, sizeof(pcm) / sizeof(pcm[0]));
			size_t at = 0;
			while (at < k + 800) {
				static int16_t blk[160];
				for (size_t j = 0; j < 160; j++)
					blk[j] = at + j < k ? pcm[at + j] : 0;
				sat_sound_rcv_audio(&rcv, blk, 160);
				sat_sound_rcv_post(&rcv, post);
				at += 160;
			}
		}
	}
}

static unsigned stored_sats(void)
{
	unsigned found = 0;
	char l1[80], l2[80];
	for (unsigned i = 0; i < 32; i++) {
		sat_elements_t want;
		sat_record_t r;
		make_tle(40000u + i, l1, l2);
		CHECK(tle_parse(l1, l2, &want) == TLE_OK);
		int slot = sat_store_find(40000u + i);
		if (slot >= 0 && sat_store_read((uint16_t)slot, &r) && r.has_elements && memcmp(&r.elements.epoch, &want.epoch, 8) == 0 &&
		    memcmp(&r.elements.no, &want.no, 8) == 0 && memcmp(&r.elements.ecco, &want.ecco, 8) == 0)
			found++;
	}
	return found;
}

static void test_transfer(void)
{
	sat_sound_stats_t st;
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	sat_sound_rcv_init(&rcv);

	// nothing is taken before start
	{
		int16_t quiet[160] = { 0 };
		sat_sound_rcv_audio(&rcv, quiet, 160);
		sat_sound_rcv_stats(&rcv, &st);
		CHECK(!st.listening && st.frames == 0);
	}

	// a whole transfer of 32 satellites, twice
	CHECK(sat_sound_rcv_start(&rcv));
	send(21, 32, 2, post_ok, 0);
	sat_sound_rcv_stats(&rcv, &st);
	CHECK(st.listening && st.count == 32 && st.heard == 32 && st.frames == 64 && st.refused == 0 && st.lost == 0 && st.queued == 0);
	CHECK(st.applied[SAT_SOUND_APPLY_NEW] == 32);
	{
		// the level is the last block's loudest sample
		int16_t loud[160];
		for (int i = 0; i < 160; i++)
			loud[i] = (int16_t)(i == 77 ? -12345 : 100);
		sat_sound_rcv_audio(&rcv, loud, 160);
		sat_sound_rcv_stats(&rcv, &st);
		CHECK(st.level == 12345);
	}
	CHECK(stored_sats() == 32 && sat_store_count() == 32);
	sat_sound_rcv_stop(&rcv);
	sat_sound_rcv_stats(&rcv, &st);
	CHECK(!st.listening);

	// the same sets in a new transfer: heard again, nothing to change
	CHECK(sat_sound_rcv_start(&rcv));
	send(22, 32, 1, post_ok, 0);
	sat_sound_rcv_stats(&rcv, &st);
	CHECK(st.heard == 32 && st.applied[SAT_SOUND_APPLY_CURRENT] == 32 && st.applied[SAT_SOUND_APPLY_NEW] == 0);
	sat_sound_rcv_stop(&rcv);

	// a store job that takes its time: the queue fills, satellites are dropped, and the second pass brings them back
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	sat_sound_rcv_init(&rcv);
	CHECK(sat_sound_rcv_start(&rcv));
	refuse_all = true;	// the poster takes nothing during the first pass
	send(23, 12, 1, post_slow, 0);
	sat_sound_rcv_stats(&rcv, &st);
	CHECK(st.lost == 8 && st.heard == 4 && st.queued == 4 && st.applied[SAT_SOUND_APPLY_NEW] == 0);
	CHECK(!sat_sound_rcv_idle(&rcv) && !sat_sound_rcv_start(&rcv));	// no restart while satellites wait
	refuse_all = false;
	sat_sound_rcv_post(&rcv, post_slow);
	sat_sound_rcv_stats(&rcv, &st);
	CHECK(st.queued == 0 && st.applied[SAT_SOUND_APPLY_NEW] == 4 && sat_sound_rcv_idle(&rcv));
	send(23, 12, 1, post_ok, 0);	// the sender's next pass
	sat_sound_rcv_stats(&rcv, &st);
	CHECK(st.heard == 12 && st.applied[SAT_SOUND_APPLY_NEW] == 12);
	sat_sound_rcv_stop(&rcv);
	CHECK(sat_store_count() == 12);

	// frames that are not satellites are counted and change nothing
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	sat_sound_rcv_init(&rcv);
	CHECK(sat_sound_rcv_start(&rcv));
	send(24, 8, 1, post_ok, 2);
	sat_sound_rcv_stats(&rcv, &st);
	CHECK(st.refused == 4 && st.heard == 4 && sat_store_count() == 4);
	sat_sound_rcv_stop(&rcv);
	CHECK(sim_flash_violations() == 0);
}

int main(void)
{
	test_apply();
	test_profile();
	test_transfer();
	if (failures != 0) {
		printf("# FAILED %d\n", failures);
		return 1;
	}
	printf("# PASSED\n");
	return 0;
}
