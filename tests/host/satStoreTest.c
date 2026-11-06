// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Stored satellites (core/sat/sat_store.c, docs/spec/satellite.md 6.3) on a simulated NOR flash: records in and out
// unchanged, replace by catalog number, full, delete, someone else's bytes in the area, a bad map, the shared buffer
// taken, and a power cut at every unit of a write: after the next boot the old record or the new one, never a mix
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage/storage.h"
#include "sat/sat_store.h"
#include "sat/sgp4.h"
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

static sat_record_t rec(int tle, uint32_t up_hz)
{
	sat_record_t r = { 0 };
	CHECK(tle_parse(ref_tles[tle].l1, ref_tles[tle].l2, &r.elements) == TLE_OK);
	r.has_elements = true;
	const sat_profile_t *p = sat_profile_find(r.elements.norad);
	if (p != NULL)
		r.profile = *p;
	else
		snprintf(r.profile.name, sizeof r.profile.name, "%s", ref_tles[tle].name);
	r.profile.norad = r.elements.norad;
	r.profile.up_hz = up_hz;
	return r;
}

// Field by field: the structs' padding holds whatever was on the stack
static bool same(const sat_record_t *a, const sat_record_t *b)
{
	const sat_profile_t *p = &a->profile, *q = &b->profile;
	const sat_elements_t *e = &a->elements, *f = &b->elements;
	if (strcmp(p->name, q->name) != 0 || p->norad != q->norad || p->up_hz != q->up_hz || p->down_hz != q->down_hz ||
	    p->up_tone != q->up_tone || p->arm_tone != q->arm_tone || p->arm_ms != q->arm_ms || p->armed_s != q->armed_s ||
	    a->has_elements != b->has_elements)
		return false;
	// Bit for bit: a stored double comes back exactly
	return !a->has_elements || (memcmp(&e->epoch, &f->epoch, 8) == 0 && memcmp(&e->bstar, &f->bstar, 8) == 0 &&
		memcmp(&e->inclo, &f->inclo, 8) == 0 && memcmp(&e->nodeo, &f->nodeo, 8) == 0 &&
		memcmp(&e->argpo, &f->argpo, 8) == 0 && memcmp(&e->mo, &f->mo, 8) == 0 &&
		memcmp(&e->ecco, &f->ecco, 8) == 0 && memcmp(&e->no, &f->no, 8) == 0 && e->norad == f->norad);
}

static void boot(void)
{
	sim_flash_power_on();
	CHECK(storage_init());
}

static void test_codec(void)
{
	uint8_t b[SAT_STORE_RECORD];
	sat_record_t a = rec(1, 145850000u), d;
	sat_store_encode(&a, b);
	CHECK(b[0] == 0x4F && b[1] == 0x53 && b[2] == 0x41 && b[3] == 0x54 && b[4] == SAT_STORE_LAYOUT);
	CHECK(sat_store_decode(b, &d) && same(&a, &d) && strcmp(d.profile.name, "SO-50") == 0 && d.profile.arm_tone == 744);
	// The elements drive the model to the same state as the parsed ones
	sgp4_t m1, m2;
	sgp4_state_t s1, s2;
	CHECK(sgp4_init(&m1, &a.elements) == SGP4_OK && sgp4_init(&m2, &d.elements) == SGP4_OK);
	CHECK(sgp4_propagate(&m1, 1234.5, &s1) == SGP4_OK && sgp4_propagate(&m2, 1234.5, &s2) == SGP4_OK);
	CHECK(memcmp(&s1, &s2, sizeof s1) == 0);
	// Any changed byte fails the CRC; another layout is refused
	for (unsigned i = 0; i < SAT_STORE_RECORD; i += 13) {
		b[i] ^= 0x10;
		CHECK(!sat_store_decode(b, &d));
		b[i] ^= 0x10;
	}
	b[4] = SAT_STORE_LAYOUT + 1;
	CHECK(!sat_store_decode(b, &d));
	// A profile without elements (frequencies kept for a satellite whose elements aren't in yet)
	sat_record_t p = { .profile = { .name = "NEW-1", .norad = 99999, .down_hz = 436000000u } };
	sat_store_encode(&p, b);
	CHECK(sat_store_decode(b, &d) && same(&p, &d) && !d.has_elements);
	// A name of 11 characters and more is cut to 11
	memset(p.profile.name, 'A', sizeof p.profile.name);
	sat_store_encode(&p, b);
	CHECK(sat_store_decode(b, &d) && strlen(d.profile.name) == 11);
}

static void test_store(void)
{
	sat_record_t r;
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	CHECK(sat_store_slots() == 64 && sat_store_state() == SAT_STORE_OURS && sat_store_count() == 0);
	CHECK(!sat_store_read(0, &r) && sat_store_find(25544) == -1 && !sat_store_delete(25544));

	sat_record_t iss = rec(0, 145990000u), so50 = rec(1, 145850000u), ao91 = rec(2, 435250000u);
	CHECK(sat_store_put(&iss) && sat_store_put(&so50) && sat_store_put(&ao91));
	CHECK(sat_store_count() == 3 && sat_store_find(25544) == 0 && sat_store_find(27607) == 1 && sat_store_find(43017) == 2);
	CHECK(sat_store_read(1, &r) && same(&r, &so50));
	// The same satellite again replaces its record in place
	iss.profile.up_hz = 145985000u;
	CHECK(sat_store_put(&iss) && sat_store_count() == 3 && sat_store_find(25544) == 0);
	CHECK(sat_store_read(0, &r) && r.profile.up_hz == 145985000u);
	// Delete frees the slot, and the next new one takes it
	CHECK(sat_store_delete(27607) && sat_store_count() == 2 && sat_store_find(27607) == -1 && !sat_store_read(1, &r));
	sat_record_t low = rec(3, 0);
	CHECK(sat_store_put(&low) && sat_store_find(low.profile.norad) == 1);
	// Fill both sectors; one more doesn't fit, a replacement still does
	unsigned added = 0;
	for (uint32_t n = 200000; sat_store_count() < 64; n++) {
		sat_record_t f = { .profile = { .name = "FILL", .norad = n } };
		CHECK(sat_store_put(&f));
		added++;
	}
	CHECK(added == 61 && sat_store_count() == 64 && sat_store_find(200060) == 63);
	sat_record_t more = { .profile = { .name = "MORE", .norad = 300000 } };
	CHECK(!sat_store_put(&more));
	CHECK(sat_store_put(&ao91));
	CHECK(sim_flash_violations() == 0);

	// Survives a reboot
	boot();
	CHECK(sat_store_count() == 64 && sat_store_read(2, &r) && same(&r, &ao91));

	// The shared sector buffer taken (a codeplug write): refused, nothing changed
	uint8_t *b = storage_buffer_acquire();
	CHECK(b != NULL && !sat_store_delete(25544) && sat_store_find(25544) == 0);
	storage_buffer_release();
	sim_flash_free();
}

static void test_foreign(void)
{
	sat_record_t r, iss = rec(0, 145990000u);
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	CHECK(sat_store_put(&iss));
	// Someone else's bytes in a slot: read only from then on
	sim_flash_data[SAT_ADDR + 5u * SAT_STORE_RECORD + 7u] = 0x12;
	CHECK(sat_store_state() == SAT_STORE_FOREIGN);
	CHECK(sat_store_read(0, &r) && same(&r, &iss));
	CHECK(!sat_store_put(&iss) && !sat_store_delete(25544) && sat_store_count() == 1);
	// Cleared: ours again, empty, and writable
	CHECK(sat_store_clear() && sat_store_state() == SAT_STORE_OURS && sat_store_count() == 0 && sat_store_put(&iss));
	CHECK(sim_flash_violations() == 0);
	sim_flash_free();

	// Maps that give no area: none, unaligned, past the flash, over the settings ring
	const struct { uint32_t addr, size; } bad[] = { { 0, 0 }, { SAT_ADDR + 0x100u, 0x1000u }, { SAT_ADDR, 0x1800u },
		{ FLASH_SIZE - 0x1000u, 0x2000u }, { 0x12000u, 0x1000u }, { 0x19000u, 0x1000u } };
	for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		map.sat_addr = bad[i].addr;
		map.sat_size = bad[i].size;
		sim_flash_create(FLASH_SIZE, &map);
		boot();
		CHECK(sat_store_slots() == 0 && sat_store_state() == SAT_STORE_NONE && !sat_store_put(&iss));
		sim_flash_free();
	}
	map.sat_addr = SAT_ADDR;
	map.sat_size = 0x2000u;
}

// A power cut at every unit of replacing a record: after the boot (the journal's replay) the store holds the old
// record or the new one, whole, and the others unchanged
static void test_power_cut(void)
{
	sat_record_t r, iss = rec(0, 145990000u), so50 = rec(1, 145850000u), iss2 = iss;
	iss2.profile.up_hz = 145980000u;
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	CHECK(sat_store_put(&iss) && sat_store_put(&so50));
	uint32_t before = sim_flash_units();
	CHECK(sat_store_put(&iss2));
	uint32_t units = sim_flash_units() - before;
	CHECK(sat_store_put(&iss));
	unsigned olds = 0, news = 0;
	for (uint32_t cut = 1; cut <= units; cut++) {
		sim_flash_cut_after(cut);
		sat_store_put(&iss2);
		sim_flash_cut_after(0);
		boot();
		CHECK(sat_store_state() == SAT_STORE_OURS && sat_store_count() == 2);
		CHECK(sat_store_read(1, &r) && same(&r, &so50));
		CHECK(sat_store_read(0, &r));
		if (same(&r, &iss))
			olds++;
		else if (same(&r, &iss2))
			news++;
		else
			CHECK(!"a mix");
		CHECK(sat_store_put(&iss));
	}
	printf("satStoreTest: a write is %u units; cut at each: %u old, %u new\n", (unsigned)units, olds, news);
	CHECK(olds > 0 && news > 0);
	CHECK(sim_flash_violations() == 0);
	sim_flash_free();
}

int main(void)
{
	test_codec();
	test_store();
	test_foreign();
	test_power_cut();
	printf("satStoreTest: %s\n", failures ? "FAILED" : "passed");
	return failures != 0;
}
