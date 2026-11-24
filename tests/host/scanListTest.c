// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Stored scan lists (core/codeplug/scan_list.c, docs/spec/scan-list.md) on a simulated NOR flash: records in and out
// unchanged, refused lists, replace, delete and the free slot it leaves, full, someone else's bytes in the area, bad
// maps, the shared buffer taken, and a power cut at every unit of a write: after the next boot the old list or the
// new one, never a mix
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage/storage.h"
#include "codeplug/scan_list.h"
#include "sim/sim_flash.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}
void storage_quick_lock(void) {}
void storage_quick_unlock(void) {}

#define FLASH_SIZE	0x40000u
#define SCAN_ADDR	0x1C000u

static hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = FLASH_SIZE,
	.satellite_addr = 0x1B000u, .satellite_size = 0x1000u,
	.scan_addr = SCAN_ADDR, .scan_size = 0x2000u,
};

const board_t board = { .name = "test", .flash = &sim_flash };

// A list named name with the channels first, first + 1, ... (n of them)
static scan_list_t make(const char *name, uint16_t first, uint16_t n)
{
	scan_list_t l;
	memset(&l, 0, sizeof l);
	snprintf(l.name, sizeof l.name, "%s", name);
	l.count = n;
	for (uint16_t i = 0; i < n; i++)
		l.channels[i] = (uint16_t)(first + i);
	return l;
}

// Field by field: the structs' padding holds whatever was on the stack
static bool same(const scan_list_t *a, const scan_list_t *b)
{
	return strcmp(a->name, b->name) == 0 && a->count == b->count &&
	       memcmp(a->channels, b->channels, a->count * sizeof a->channels[0]) == 0;
}

static void boot(void)
{
	sim_flash_power_on();
	CHECK(storage_init());
}

static void test_codec(void)
{
	uint8_t b[SCAN_LIST_RECORD];
	scan_list_t a = make("SATS", 10, 5), d;
	CHECK(scan_list_encode(&a, b));
	CHECK(b[0] == 0x4F && b[1] == 0x53 && b[2] == 0x43 && b[3] == 0x4C && b[4] == SCAN_LIST_LAYOUT);
	CHECK(b[6] == 5 && b[7] == 0 && b[24] == 10 && b[25] == 0 && b[26] == 11);
	CHECK(scan_list_decode(b, &d) && same(&a, &d));
	for (unsigned i = 184; i < SCAN_LIST_RECORD - 4u; i++)
		CHECK(b[i] == 0);			// the reserved bytes are zero
	// Any changed byte fails the CRC; another layout is refused
	for (unsigned i = 0; i < SCAN_LIST_RECORD; i += 11) {
		b[i] ^= 0x10;
		CHECK(!scan_list_decode(b, &d));
		b[i] ^= 0x10;
	}
	b[4] = SCAN_LIST_LAYOUT + 1;
	CHECK(!scan_list_decode(b, &d));

	// The most a list holds, and a name of every byte of its field (no terminator in the record; non-ASCII bytes kept)
	scan_list_t full = make("0123456789ABCDEF", 1, SCAN_LIST_MAX);
	full.name[3] = (char)0xE9;
	CHECK(scan_list_encode(&full, b) && scan_list_decode(b, &d) && same(&full, &d) && strlen(d.name) == 16);
	// An empty list is a list
	scan_list_t none = make("EMPTY", 0, 0);
	CHECK(scan_list_encode(&none, b) && scan_list_decode(b, &d) && d.count == 0);

	// Refused: no name, too many channels, channel 0, a channel twice
	scan_list_t bad = make("", 1, 3);
	CHECK(!scan_list_encode(&bad, b));
	bad = make("X", 1, SCAN_LIST_MAX);
	bad.count = SCAN_LIST_MAX + 1;
	CHECK(!scan_list_encode(&bad, b));
	bad = make("X", 1, 3);
	bad.channels[1] = 0;
	CHECK(!scan_list_encode(&bad, b));
	bad = make("X", 1, 3);
	bad.channels[2] = 1;
	CHECK(!scan_list_encode(&bad, b));
	// A stored record that is valid only by its CRC (a count past the limit) is not a list
	CHECK(scan_list_encode(&a, b));
	b[6] = SCAN_LIST_MAX + 1;
	uint32_t crc = storage_crc32(0, b, SCAN_LIST_RECORD - 4u);
	for (unsigned k = 0; k < 4; k++)
		b[SCAN_LIST_RECORD - 4u + k] = (uint8_t)(crc >> (8 * k));
	CHECK(!scan_list_decode(b, &d));
}

static void test_store(void)
{
	scan_list_t r;
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	CHECK(scan_list_slots() == 32 && scan_list_state() == SCAN_LIST_OURS && scan_list_count() == 0);
	CHECK(!scan_list_read(0, &r) && !scan_list_delete(0) && !scan_list_read(32, &r));

	scan_list_t a = make("SATS", 1, 4), b = make("REPEATERS", 20, 12), c = make("SIMPLEX", 100, 3);
	CHECK(scan_list_add(&a) == 0 && scan_list_add(&b) == 1 && scan_list_add(&c) == 2);
	CHECK(scan_list_count() == 3 && scan_list_read(1, &r) && same(&r, &b));
	// A write replaces what is in the slot, in place
	b.channels[b.count++] = 500;
	CHECK(scan_list_write(1, &b) && scan_list_count() == 3 && scan_list_read(1, &r) && same(&r, &b) && r.count == 13);
	// A write to a free slot fills it; delete frees a slot and the next add takes it
	scan_list_t d = make("LATER", 7, 2);
	CHECK(scan_list_write(10, &d) && scan_list_count() == 4 && scan_list_read(10, &r) && same(&r, &d));
	CHECK(scan_list_delete(1) && scan_list_count() == 3 && !scan_list_read(1, &r) && !scan_list_delete(1));
	CHECK(scan_list_read(0, &r) && same(&r, &a) && scan_list_read(2, &r) && same(&r, &c));	// the others keep their slots
	scan_list_t e = make("NEW", 50, 1);
	CHECK(scan_list_add(&e) == 1);
	// Refused lists and slots change nothing
	scan_list_t bad = make("", 1, 1);
	uint32_t gen = scan_list_generation();
	CHECK(!scan_list_write(3, &bad) && scan_list_add(&bad) == -1 && !scan_list_write(32, &a));
	CHECK(scan_list_generation() == gen && scan_list_count() == 4);

	// Fill both sectors; one more doesn't fit, a replacement still does
	for (unsigned i = 0; scan_list_count() < 32; i++) {
		scan_list_t f = make("FILL", (uint16_t)(200 + i), 1);
		CHECK(scan_list_add(&f) >= 0);
	}
	CHECK(scan_list_count() == 32 && scan_list_add(&a) == -1);
	CHECK(scan_list_write(31, &a) && scan_list_read(31, &r) && same(&r, &a));
	CHECK(sim_flash_violations() == 0);

	// Survives a reboot
	boot();
	CHECK(scan_list_count() == 32 && scan_list_read(2, &r) && same(&r, &c) && scan_list_read(10, &r) && same(&r, &d));

	// The shared sector buffer taken (a codeplug write): refused, nothing changed
	uint8_t *buf = storage_buffer_acquire();
	CHECK(buf != NULL && !scan_list_delete(2) && !scan_list_write(2, &a) && scan_list_read(2, &r) && same(&r, &c));
	storage_buffer_release();
	sim_flash_free();
}

static void test_foreign(void)
{
	scan_list_t r, a = make("SATS", 1, 4);
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	CHECK(scan_list_add(&a) == 0);
	// Someone else's bytes in a slot: read only from then on
	sim_flash_data[SCAN_ADDR + 5u * SCAN_LIST_RECORD + 7u] = 0x12;
	CHECK(scan_list_state() == SCAN_LIST_FOREIGN);
	CHECK(scan_list_read(0, &r) && same(&r, &a));
	CHECK(!scan_list_write(0, &a) && scan_list_add(&a) == -1 && !scan_list_delete(0) && scan_list_count() == 1);
	// Cleared: ours again, empty, and writable
	CHECK(scan_list_clear() && scan_list_state() == SCAN_LIST_OURS && scan_list_count() == 0 && scan_list_add(&a) == 0);
	CHECK(sim_flash_violations() == 0);
	sim_flash_free();

	// Maps that give no area: none, unaligned, past the flash, over the settings ring, over the satellite area
	const struct { uint32_t addr, size; } bad[] = { { 0, 0 }, { SCAN_ADDR + 0x100u, 0x1000u }, { SCAN_ADDR, 0x1800u },
		{ FLASH_SIZE - 0x1000u, 0x2000u }, { 0x12000u, 0x1000u }, { 0x1B000u, 0x1000u }, { 0x1A000u, 0x2000u } };
	for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		map.scan_addr = bad[i].addr;
		map.scan_size = bad[i].size;
		sim_flash_create(FLASH_SIZE, &map);
		boot();
		CHECK(scan_list_slots() == 0 && scan_list_state() == SCAN_LIST_NONE && scan_list_add(&a) == -1);
		sim_flash_free();
	}
	map.scan_addr = SCAN_ADDR;
	map.scan_size = 0x2000u;
}

// A power cut at every unit of replacing a list: after the boot (the journal's replay) the store holds the old list
// or the new one, whole, and the others unchanged
static void test_power_cut(void)
{
	scan_list_t r, a = make("SATS", 1, 4), b = make("REPEATERS", 20, 12), a2 = make("SATS", 1, 9);
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	CHECK(scan_list_add(&a) == 0 && scan_list_add(&b) == 1);
	uint32_t before = sim_flash_units();
	CHECK(scan_list_write(0, &a2));
	uint32_t units = sim_flash_units() - before;
	CHECK(scan_list_write(0, &a));
	unsigned olds = 0, news = 0;
	for (uint32_t cut = 1; cut <= units; cut++) {
		sim_flash_cut_after(cut);
		scan_list_write(0, &a2);
		sim_flash_cut_after(0);
		boot();
		CHECK(scan_list_state() == SCAN_LIST_OURS && scan_list_count() == 2);
		CHECK(scan_list_read(1, &r) && same(&r, &b));
		CHECK(scan_list_read(0, &r));
		if (same(&r, &a))
			olds++;
		else if (same(&r, &a2))
			news++;
		else
			CHECK(!"a mix");
		CHECK(scan_list_write(0, &a));
	}
	printf("scanListTest: a write is %u units; cut at each: %u old, %u new\n", (unsigned)units, olds, news);
	CHECK(olds > 0 && news > 0);
	CHECK(sim_flash_violations() == 0);
	sim_flash_free();
}

// Vote options (docs/spec/vote-scan.md 3): 0 on a new list, set on their own, kept when the list is edited and across a
// reboot, gone with the list; the codec puts and takes them; unknown flag bits dropped; the defaults and their clamps
static void test_vote(void)
{
	scan_list_t r;
	scan_list_vote_t v;
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	scan_list_t a = make("MVP25", 1, 8), b = make("OTHER", 40, 2);
	CHECK(scan_list_add(&a) == 0 && scan_list_add(&b) == 1);
	CHECK(scan_list_read_vote(0, &v) && v.flags == 0 && v.threshold_dbm == 0 && v.margin_db == 0 && v.tx_member == 0);
	CHECK(scan_vote_threshold_of(&v) == -90 && scan_vote_margin_of(&v) == 6);
	CHECK(!scan_list_read_vote(5, &v) && v.flags == 0);	// a free slot: a plain list's zeros

	scan_list_vote_t w = { .flags = SCAN_VOTE_ON | SCAN_VOTE_FIXED_TX | 0x80u, .threshold_dbm = -85, .margin_db = 4,
			       .tx_member = 1 };
	CHECK(scan_list_write_vote(0, &w));
	CHECK(scan_list_read_vote(0, &v) && v.flags == (SCAN_VOTE_ON | SCAN_VOTE_FIXED_TX) && v.threshold_dbm == -85 &&
	      v.margin_db == 4 && v.tx_member == 1);
	CHECK(scan_list_read(0, &r) && same(&r, &a));				// the list itself unchanged
	CHECK(scan_list_read_vote(1, &v) && v.flags == 0);			// the other list untouched
	// An edit of the list (a channel added, a rename) keeps them
	a.channels[a.count++] = 99;
	snprintf(a.name, sizeof a.name, "MV LINK");
	CHECK(scan_list_write(0, &a) && scan_list_read(0, &r) && same(&r, &a));
	CHECK(scan_list_read_vote(0, &v) && v.flags == (SCAN_VOTE_ON | SCAN_VOTE_FIXED_TX) && v.threshold_dbm == -85);
	boot();
	CHECK(scan_list_read_vote(0, &v) && v.flags == (SCAN_VOTE_ON | SCAN_VOTE_FIXED_TX) && v.margin_db == 4);
	// Deleted, the slot's next list starts plain
	CHECK(scan_list_delete(0) && !scan_list_write_vote(0, &w) && scan_list_add(&b) == 0);
	CHECK(scan_list_read_vote(0, &v) && v.flags == 0);
	CHECK(sim_flash_violations() == 0);
	sim_flash_free();

	// The linked channel: one list to a channel, kept with the list, found by channel, and not by a list's own slot
	sim_flash_create(FLASH_SIZE, &map);
	boot();
	scan_list_t c = make("SITES", 5, 3), d = make("OTHER", 40, 2);
	CHECK(scan_list_add(&c) == 0 && scan_list_add(&d) == 1);
	CHECK(scan_list_read_vote(0, &v) && v.link == 0 && scan_list_find_link(3, -1) == -1 && scan_list_find_link(0, -1) == -1);
	scan_list_vote_t l0 = { .flags = SCAN_VOTE_ON, .link = 3 }, l1 = { .flags = SCAN_VOTE_ON, .link = 3 };
	CHECK(scan_list_write_vote(0, &l0) && scan_list_find_link(3, -1) == 0 && scan_list_find_link(3, 0) == -1);
	CHECK(!scan_list_write_vote(1, &l1));					// channel 3 is the first list's
	l1.link = 4;
	CHECK(scan_list_write_vote(1, &l1) && scan_list_find_link(4, -1) == 1);
	CHECK(scan_list_write_vote(0, &l0));					// its own channel again is not a clash
	c.channels[c.count++] = 77;
	CHECK(scan_list_write(0, &c) && scan_list_read_vote(0, &v) && v.link == 3);	// an edit keeps it
	boot();
	CHECK(scan_list_read_vote(0, &v) && v.link == 3 && scan_list_find_link(3, -1) == 0);
	l0.link = 0;
	CHECK(scan_list_write_vote(0, &l0) && scan_list_find_link(3, -1) == -1);	// unlinked: the channel is free
	CHECK(scan_list_delete(1) && scan_list_find_link(4, -1) == -1);
	CHECK(sim_flash_violations() == 0);
	sim_flash_free();

	// The codec and the clamps
	uint8_t rec[SCAN_LIST_RECORD];
	CHECK(scan_list_encode(&b, rec));
	scan_list_put_vote(rec, &w);
	CHECK(scan_list_decode(rec, &r) && same(&r, &b));			// the CRC was made again
	scan_list_get_vote(rec, &v);
	CHECK(v.flags == (SCAN_VOTE_ON | SCAN_VOTE_FIXED_TX) && v.threshold_dbm == -85 && v.tx_member == 1 && v.link == 0);
	w.link = 0x1234;
	scan_list_put_vote(rec, &w);
	scan_list_get_vote(rec, &v);
	CHECK(v.link == 0x1234 && rec[188] == 0x34 && rec[189] == 0x12 && rec[187] == 0);
	scan_list_vote_t x = { .threshold_dbm = -127, .margin_db = 200 };
	CHECK(scan_vote_threshold_of(&x) == SCAN_VOTE_THRESHOLD_MIN && scan_vote_margin_of(&x) == SCAN_VOTE_MARGIN_MAX);
	x.threshold_dbm = -20;
	CHECK(scan_vote_threshold_of(&x) == SCAN_VOTE_THRESHOLD_MAX);
}

int main(void)
{
	test_codec();
	test_store();
	test_foreign();
	test_power_cut();
	test_vote();
	printf("scanListTest: %s\n", failures ? "FAILED" : "passed");
	return failures != 0;
}
