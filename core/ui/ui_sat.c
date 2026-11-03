// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Tools > Satellites (CONFIG_SAT, docs/spec/ui.md 2.6): the stored satellites with each one's next pass, and one
// satellite's passes over the next 24 hours. Times are local (Settings > Location > Time zone); each pass has an arrow
// for its way across the sky. Everything shown is read from the satellite service (ui_svc_sat_*), which another task
// works out: these screens never run the orbit model, and fill in as its results arrive (the list redraws every
// second).
#include "config.h"
#if CONFIG_SAT
#include <string.h>
#include "ui_nav.h"
#include "fmt.h"
#include "i18n.h"

static ui_sat_status_t st;		// read once a draw (status())
static uint32_t st_ms = 1;		// ... the UI's time then
static char open_name[12];

// The service's status, read again once per draw: whichever of the list's callbacks runs first
static void status(void)
{
	if (st_ms == ui_st.now_ms)
		return;
	st_ms = ui_st.now_ms;
	if (!ui_svc_sat_status(&st))
		memset(&st, 0, sizeof st);
}

static uint32_t local(uint32_t utc)
{
	return (uint32_t)((int32_t)utc + st.utc_offset_min * 60);
}

// --- the list ---

static uint16_t list_count(void)
{
	status();
	return st.count;
}

static const char *list_title(void)
{
	// The local time beside the title, once the clock is set
	static char t[8];
	status();
	if (st.utc == 0)
		return NULL;
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	sb_hhmm(&b, local(st.utc));
	return t;
}

static const char *list_body(void)
{
	static char t[96];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	status();
	if (st.state == UI_SAT_NO_TIME) {
		sb_str(&b, TR("Waiting for the time: passes need UTC from a GPS fix"));
	} else if (st.state == UI_SAT_NO_POSITION) {
		sb_str(&b, TR("No position: a GPS fix, or a fixed one in Tools > APRS"));
	} else {
		// "Above 10°, 24 h, UTC-6": what the list holds and the time zone its times are in
		sb_str(&b, TR("Above"));
		sb_chr(&b, ' ');
		sb_u32(&b, st.min_el, 1);
		sb_str(&b, GLYPH_DEGREE ", 24 h, ");
		sb_utc_offset(&b, st.utc_offset_min);
	}
	return t;
}

static void list_item(uint16_t i, ui_item_t *it)
{
	ui_sat_t s;
	if (!ui_svc_sat_get((uint8_t)i, &s))
		return;
	str_copy(it->label, sizeof it->label, s.name[0] ? s.name : "?");
	if (i < 9)
		item_num_set(it, (uint16_t)(i + 1u));
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	if (s.state == UI_SAT_NO_ORBIT) {
		sb_str(&b, TR("no orbit"));
		it->flags |= UI_ITEM_DIM;
		return;
	}
	if (s.state == UI_SAT_OLD || s.state == UI_SAT_BAD) {
		if (s.state == UI_SAT_OLD) {
			sb_str(&b, "TLE ");
			sb_i32(&b, s.age_d);
			sb_chr(&b, 'd');
		} else {
			sb_str(&b, TR("bad orbit"));
		}
		it->flags |= UI_ITEM_WARN;
		return;
	}
	if (st.state != UI_SAT_READY || s.state == UI_SAT_WORKING) {
		sb_str(&b, GLYPH_ELLIPSIS);
		return;
	}
	if (s.state == UI_SAT_NONE) {
		sb_str(&b, TR("no pass"));
		return;
	}
	// Its way across the sky, then the time (or "up now") and how high: with a shorter form for narrow screens
	char when[24];
	sbuf_t w;
	sb_init(&w, when, sizeof when);
	if (s.state == UI_SAT_UP) {
		sb_str(&w, TR("up now"));
		sb_chr(&w, ' ');
		sb_i32(&w, s.el_now);
		it->flags |= UI_ITEM_LIVE;
	} else {
		sb_hhmm(&w, local(s.aos));
		sb_str(&w, "  ");
		sb_i32(&w, s.max_el);
	}
	sb_str(&w, GLYPH_DEGREE);
	sb_str(&b, TEXT_ALT_OPEN);
	sb_dir_arrow(&b, s.dir);
	sb_chr(&b, ' ');
	sb_str(&b, when);
	sb_str(&b, TEXT_ALT_NEXT);
	sb_str(&b, when);
	sb_str(&b, TEXT_ALT_CLOSE);
}

static void list_select(uint16_t i, bool hold)
{
	(void)hold;
	ui_sat_t s;
	if (!ui_svc_sat_get((uint8_t)i, &s))
		return;
	if (s.state == UI_SAT_NO_ORBIT) {
		ui_toast(TR("No orbit data: load its TLE over the cable"));
		return;
	}
	if (s.state == UI_SAT_BAD) {
		ui_toast(TR("This TLE can't be predicted"));
		return;
	}
	str_copy(open_name, sizeof open_name, s.name);
	ui_svc_sat_open(s.norad);
	ui_push(SCR_SAT_PASSES, i);
}

static void list_enter(void)
{
	ui_svc_sat_refresh();	// from the store again: what was loaded since, ages, the setting
}

const ui_scr_def_t ui_sat_screen = {
	.name = "satellites", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = TRN("Satellites"),
	.hint = TRN("OK passes  Back"),
	.empty = TRN("No satellites stored. Load them over the cable: rfoundry_debug.py sat"),
	.count = list_count, .item = list_item, .select = list_select, .title = list_title, .body = list_body,
	.enter = list_enter,
};

// --- one satellite's passes ---

// Rows: the passes, a "Tomorrow" line before the first one of the next day, and "Working" while the 24 hours aren't
// all searched
static uint8_t npass;
static bool pass_done;
static int8_t divider;		// the row of "Tomorrow", -1 none

static uint16_t passes_count(void)
{
	status();
	npass = ui_svc_sat_pass_count(&pass_done);
	divider = -1;
	uint32_t today = local(st.utc) / 86400u;
	ui_sat_pass_t p;
	for (uint8_t k = 0; k < npass && st.utc != 0; k++) {
		if (ui_svc_sat_pass(k, &p) && local(p.aos) / 86400u != today) {
			divider = (int8_t)k;	// before the first pass of the next day
			break;
		}
	}
	return (uint16_t)(npass + (divider >= 0) + !pass_done);
}

// "SO-50  TLE 2d": the name and how old its element set is
static const char *passes_title(void)
{
	static char t[24];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	sb_str(&b, open_name);
	ui_sat_profile_t p;
	if (ui_svc_sat_profile(&p) && p.age_d != INT16_MIN) {
		sb_str(&b, "  TLE ");
		sb_i32(&b, p.age_d);
		sb_chr(&b, 'd');
	}
	return t;
}

static const char *passes_body(void)
{
	static char t[96];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	ui_sat_profile_t p;
	if (!ui_svc_sat_profile(&p))
		return NULL;
	// "TX 145.850 RX 436.795", then the tones and the element set's age
	if (p.up_hz) {
		sb_str(&b, "TX ");
		sb_freq(&b, p.up_hz);
		sb_chr(&b, ' ');
	}
	if (p.down_hz) {
		sb_str(&b, "RX ");
		sb_freq(&b, p.down_hz);
	}
	sb_chr(&b, '\n');
	if (p.up_tone) {
		sb_str(&b, TR("Tone"));
		sb_chr(&b, ' ');
		subtone_t tone = { .type = TONE_CTCSS, .value = p.up_tone };
		sb_tone(&b, &tone);
		sb_chr(&b, ' ');
	}
	if (p.arm_tone) {
		sb_str(&b, TR("arm"));
		sb_chr(&b, ' ');
		subtone_t tone = { .type = TONE_CTCSS, .value = p.arm_tone };
		sb_tone(&b, &tone);
	}
	return t;
}

static void passes_item(uint16_t row, ui_item_t *it)
{
	if (divider >= 0 && row == (uint16_t)divider) {
		str_copy(it->label, sizeof it->label, TR("Tomorrow"));
		it->flags |= UI_ITEM_INFO;
		return;
	}
	uint16_t k = (uint16_t)(row - (divider >= 0 && row > (uint16_t)divider));
	ui_sat_pass_t p;
	if (k >= npass || !ui_svc_sat_pass((uint8_t)k, &p)) {
		sbuf_t w;
		sb_init(&w, it->label, sizeof it->label);
		sb_str(&w, TR("Working"));
		sb_str(&w, GLYPH_ELLIPSIS);
		it->flags |= UI_ITEM_INFO;
		return;
	}
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sb_hhmm(&b, local(p.aos));
	sb_str(&b, "  ");
	sb_i32(&b, p.max_el);
	sb_str(&b, GLYPH_DEGREE);
	// "↘ NW→SE 13m", narrower "NW→SE 13m", then "13m"
	char way[16];
	sbuf_t w;
	sb_init(&w, way, sizeof way);
	sb_compass(&w, p.aos_az);
	sb_str(&w, GLYPH_ARROW);
	sb_compass(&w, p.los_az);
	char len[8];
	sbuf_t l;
	sb_init(&l, len, sizeof len);
	sb_u32(&l, (p.los - p.aos + 30u) / 60u, 1);
	sb_chr(&l, 'm');
	sb_init(&b, it->value, sizeof it->value);
	sb_str(&b, TEXT_ALT_OPEN);
	sb_dir_arrow(&b, p.dir);
	sb_chr(&b, ' ');
	sb_str(&b, way);
	sb_chr(&b, ' ');
	sb_str(&b, len);
	sb_str(&b, TEXT_ALT_NEXT);
	sb_str(&b, way);
	sb_chr(&b, ' ');
	sb_str(&b, len);
	sb_str(&b, TEXT_ALT_NEXT);
	sb_str(&b, len);
	sb_str(&b, TEXT_ALT_CLOSE);
}

static void passes_select(uint16_t row, bool hold)
{
	(void)row;
	(void)hold;
}

const ui_scr_def_t ui_sat_passes_screen = {
	// Not a field-use screen (P4): looking ahead is planning, and four levels from Home (OK 6 0 OK)
	.name = "passes", .flags = SF_LIST, .prefix = NULL, .hint = TRN("Back"),
	.empty = TRN("No pass above the lowest in 24 h (Settings > Location)"),
	.count = passes_count, .item = passes_item, .select = passes_select, .title = passes_title,
	.body = passes_body,
};
#endif
