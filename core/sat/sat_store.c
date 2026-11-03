// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Stored satellites (sat_store.h)
#include <string.h>
#include "sat/sat_store.h"
#include "storage/storage.h"

#define SLOTS_PER_SECTOR	(STORAGE_SECTOR / SAT_STORE_RECORD)
#define CRC_AT			(SAT_STORE_RECORD - 4u)

// Record offsets (docs/spec/satellite.md 6.4)
enum {
	OFF_MAGIC = 0, OFF_LAYOUT = 4, OFF_FLAGS = 5, OFF_NAME = 8, OFF_NORAD = 20, OFF_UP = 24, OFF_DOWN = 28,
	OFF_UP_TONE = 32, OFF_ARM_TONE = 34, OFF_ARM_MS = 36, OFF_ARMED_S = 38, OFF_ELEMENTS = 40,
};
#define FLAG_ELEMENTS		0x01u

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
	return get16(p) | (uint32_t)get16(p + 2) << 16;
}

static void putd(uint8_t *p, double d)
{
	uint64_t v;
	memcpy(&v, &d, sizeof v);
	put32(p, (uint32_t)v);
	put32(p + 4, (uint32_t)(v >> 32));
}

static double getd(const uint8_t *p)
{
	uint64_t v = get32(p) | (uint64_t)get32(p + 4) << 32;
	double d;
	memcpy(&d, &v, sizeof d);
	return d;
}

void sat_store_encode(const sat_record_t *r, uint8_t out[SAT_STORE_RECORD])
{
	memset(out, 0, SAT_STORE_RECORD);
	put32(out + OFF_MAGIC, SAT_STORE_MAGIC);
	out[OFF_LAYOUT] = SAT_STORE_LAYOUT;
	out[OFF_FLAGS] = r->has_elements ? FLAG_ELEMENTS : 0u;
	memcpy(out + OFF_NAME, r->profile.name, SAT_NAME_MAX - 1u);
	out[OFF_NAME + SAT_NAME_MAX - 1u] = '\0';
	put32(out + OFF_NORAD, r->profile.norad);
	put32(out + OFF_UP, r->profile.up_hz);
	put32(out + OFF_DOWN, r->profile.down_hz);
	put16(out + OFF_UP_TONE, r->profile.up_tone);
	put16(out + OFF_ARM_TONE, r->profile.arm_tone);
	put16(out + OFF_ARM_MS, r->profile.arm_ms);
	put16(out + OFF_ARMED_S, r->profile.armed_s);
	if (r->has_elements) {
		const sat_elements_t *e = &r->elements;
		const double d[8] = { e->epoch, e->bstar, e->inclo, e->nodeo, e->argpo, e->mo, e->ecco, e->no };
		for (unsigned i = 0; i < 8; i++)
			putd(out + OFF_ELEMENTS + 8u * i, d[i]);
	}
	put32(out + CRC_AT, storage_crc32(0, out, CRC_AT));
}

bool sat_store_decode(const uint8_t in[SAT_STORE_RECORD], sat_record_t *out)
{
	if (get32(in + OFF_MAGIC) != SAT_STORE_MAGIC || in[OFF_LAYOUT] != SAT_STORE_LAYOUT ||
	    get32(in + CRC_AT) != storage_crc32(0, in, CRC_AT))
		return false;
	memset(out, 0, sizeof *out);
	memcpy(out->profile.name, in + OFF_NAME, SAT_NAME_MAX - 1u);
	out->profile.norad = get32(in + OFF_NORAD);
	out->profile.up_hz = get32(in + OFF_UP);
	out->profile.down_hz = get32(in + OFF_DOWN);
	out->profile.up_tone = get16(in + OFF_UP_TONE);
	out->profile.arm_tone = get16(in + OFF_ARM_TONE);
	out->profile.arm_ms = get16(in + OFF_ARM_MS);
	out->profile.armed_s = get16(in + OFF_ARMED_S);
	out->has_elements = (in[OFF_FLAGS] & FLAG_ELEMENTS) != 0;
	if (out->has_elements) {
		sat_elements_t *e = &out->elements;
		double *d[8] = { &e->epoch, &e->bstar, &e->inclo, &e->nodeo, &e->argpo, &e->mo, &e->ecco, &e->no };
		for (unsigned i = 0; i < 8; i++)
			*d[i] = getd(in + OFF_ELEMENTS + 8u * i);
		e->norad = out->profile.norad;
	}
	return true;
}

static bool overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen)
{
	return blen != 0 && a < b + blen && b < a + alen;
}

// The area when the board has a usable one: aligned whole sectors on the flash, clear of every other area, and the
// storage region ours
static bool area(uint32_t *addr, uint32_t *size)
{
	const hal_flash_map_t *m = storage_map();
	if (m == NULL || !storage_ready() || m->sat_size == 0 || m->sat_addr % STORAGE_SECTOR != 0 ||
	    m->sat_size % STORAGE_SECTOR != 0 || m->sat_addr > board.flash->size ||
	    m->sat_size > board.flash->size - m->sat_addr)
		return false;
	uint32_t a = m->sat_addr, n = m->sat_size;
	if (overlaps(a, n, m->settings_addr, m->settings_size) || overlaps(a, n, m->journal_addr, 2u * STORAGE_SECTOR) ||
	    overlaps(a, n, m->openfw_eeprom, 0x10000u) || overlaps(a, n, m->openfw_flash, m->openfw_end - m->openfw_flash) ||
	    overlaps(a, n, m->native_addr, m->native_size) || overlaps(a, n, m->calibration_addr, m->calibration_size) ||
	    overlaps(a, n, m->staging_addr, m->staging_size))
		return false;
	*addr = a;
	*size = n;
	return true;
}

uint16_t sat_store_slots(void)
{
	uint32_t a, n;
	return area(&a, &n) ? (uint16_t)(n / SAT_STORE_RECORD) : 0u;
}

typedef enum { SLOT_FREE, SLOT_VALID, SLOT_OTHER, SLOT_UNREADABLE } slot_t;

#define PIECE	32u

// A slot's kind, and its catalog number when it is a record. Read in pieces with the CRC run along, so the scans
// (state, find, count) need no record on the stack: the debug link's view runs them in a task with little to spare
static slot_t slot_kind(uint16_t i, uint32_t *norad)
{
	uint32_t a, n;
	uint8_t b[PIECE];
	if (!area(&a, &n) || i >= n / SAT_STORE_RECORD)
		return SLOT_UNREADABLE;
	uint32_t at = a + (uint32_t)i * SAT_STORE_RECORD, crc = 0;
	bool blank = true, head = false;
	storage_lock();
	for (uint32_t off = 0; off < SAT_STORE_RECORD; off += PIECE) {
		if (!board.flash->read(at + off, b, PIECE)) {
			storage_unlock();
			return SLOT_UNREADABLE;
		}
		if (off == 0) {
			head = get32(b + OFF_MAGIC) == SAT_STORE_MAGIC && b[OFF_LAYOUT] == SAT_STORE_LAYOUT;
			*norad = get32(b + OFF_NORAD);
		}
		for (unsigned k = 0; k < PIECE; k++)
			blank = blank && b[k] == 0xFFu;
		bool last = off + PIECE == SAT_STORE_RECORD;
		crc = storage_crc32(crc, b, last ? PIECE - 4u : PIECE);
		if (last && head && get32(b + PIECE - 4u) == crc) {
			storage_unlock();
			return SLOT_VALID;
		}
	}
	storage_unlock();
	return blank ? SLOT_FREE : SLOT_OTHER;
}

static slot_t slot_read(uint16_t i, sat_record_t *r)
{
	uint32_t a, n;
	uint8_t b[SAT_STORE_RECORD];
	if (!area(&a, &n) || i >= n / SAT_STORE_RECORD)
		return SLOT_UNREADABLE;
	storage_lock();
	bool ok = board.flash->read(a + (uint32_t)i * SAT_STORE_RECORD, b, sizeof b);
	storage_unlock();
	if (!ok)
		return SLOT_UNREADABLE;
	return sat_store_decode(b, r) ? SLOT_VALID : SLOT_OTHER;
}

bool sat_store_read(uint16_t slot, sat_record_t *out)
{
	return slot_read(slot, out) == SLOT_VALID;
}

sat_store_state_t sat_store_state(void)
{
	uint16_t n = sat_store_slots();
	uint32_t norad;
	if (n == 0)
		return SAT_STORE_NONE;
	for (uint16_t i = 0; i < n; i++) {
		slot_t s = slot_kind(i, &norad);
		if (s == SLOT_UNREADABLE)
			return SAT_STORE_NONE;
		if (s == SLOT_OTHER)
			return SAT_STORE_FOREIGN;
	}
	return SAT_STORE_OURS;
}

int sat_store_find(uint32_t norad)
{
	uint16_t n = sat_store_slots();
	uint32_t id;
	for (uint16_t i = 0; i < n; i++)
		if (slot_kind(i, &id) == SLOT_VALID && id == norad)
			return i;
	return -1;
}

uint16_t sat_store_count(void)
{
	uint16_t n = sat_store_slots(), c = 0;
	uint32_t id;
	for (uint16_t i = 0; i < n; i++)
		c += slot_kind(i, &id) == SLOT_VALID;
	return c;
}

// Rewrite slot i with rec (NULL: erase it to free) through the journal
static bool slot_write(uint16_t i, const uint8_t *rec)
{
	uint32_t a, n;
	if (!area(&a, &n))
		return false;
	uint32_t sector = a + (uint32_t)(i / SLOTS_PER_SECTOR) * STORAGE_SECTOR;
	uint32_t off = (uint32_t)(i % SLOTS_PER_SECTOR) * SAT_STORE_RECORD;
	storage_lock();
	uint8_t *buf = storage_buffer_acquire();
	bool ok = buf != NULL && board.flash->read(sector, buf, STORAGE_SECTOR);
	if (ok) {
		if (rec != NULL)
			memcpy(buf + off, rec, SAT_STORE_RECORD);
		else
			memset(buf + off, 0xFF, SAT_STORE_RECORD);
		ok = storage_sector_write(sector, buf);
	}
	if (buf != NULL)
		storage_buffer_release();
	storage_unlock();
	return ok;
}

bool sat_store_put(const sat_record_t *r)
{
	if (sat_store_state() != SAT_STORE_OURS)
		return false;
	int at = sat_store_find(r->profile.norad);
	uint16_t n = sat_store_slots();
	uint32_t id;
	for (uint16_t i = 0; at < 0 && i < n; i++)
		if (slot_kind(i, &id) == SLOT_FREE)
			at = i;
	if (at < 0)
		return false;
	uint8_t rec[SAT_STORE_RECORD];
	sat_store_encode(r, rec);
	return slot_write((uint16_t)at, rec);
}

bool sat_store_clear(void)
{
	uint32_t a, n;
	if (!area(&a, &n))
		return false;
	storage_lock();
	bool ok = storage_write(a, NULL, n);
	storage_unlock();
	return ok;
}

bool sat_store_delete(uint32_t norad)
{
	int at = sat_store_find(norad);
	return at >= 0 && sat_store_state() == SAT_STORE_OURS && slot_write((uint16_t)at, NULL);
}
