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
static uint32_t trk_aos, trk_los;	// the pass being tracked (UTC)
static uint16_t trk_dir;		// its way across the sky

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
	} else if (st.state == UI_SAT_NO_MEMORY) {
		sb_str(&b, TR("Not enough memory to work out passes"));
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
	if (hold && (s.state == UI_SAT_UP || s.state == UI_SAT_NEXT)) {
		// Long OK: straight to the next pass (the one under way), skipping the list
		trk_aos = s.aos;
		trk_los = s.los;
		trk_dir = s.dir;
		ui_push(SCR_SAT_TRACK, 0);
		return;
	}
	ui_push(SCR_SAT_PASSES, i);
}

static void list_enter(void)
{
	ui_svc_sat_refresh();	// from the store again: what was loaded since, ages, the setting
}

const ui_scr_def_t ui_sat_screen = {
	.name = "satellites", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = TRN("Satellites"),
	.hint = TRN("OK passes  Hold OK track"),
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

// OK on a pass: track it
static void passes_select(uint16_t row, bool hold)
{
	(void)hold;
	if (divider >= 0 && row == (uint16_t)divider)
		return;
	uint16_t k = (uint16_t)(row - (divider >= 0 && row > (uint16_t)divider));
	ui_sat_pass_t p;
	if (k >= npass || !ui_svc_sat_pass((uint8_t)k, &p))
		return;
	trk_aos = p.aos;
	trk_los = p.los;
	trk_dir = p.dir;
	ui_push(SCR_SAT_TRACK, 0);
}

const ui_scr_def_t ui_sat_passes_screen = {
	// Not a field-use screen (P4): looking ahead is planning, and four levels from Home (OK 6 0 OK)
	.name = "passes", .flags = SF_LIST, .prefix = NULL, .hint = TRN("OK track  Back"),
	.empty = TRN("No pass above the lowest in 24 h (Settings > Location)"),
	.count = passes_count, .item = passes_item, .select = passes_select, .title = passes_title,
	.body = passes_body,
};
// --- tracking one pass ---

// sin x 256 of a whole degree, from a 6 degree table
static int isin(int deg)
{
	static const uint16_t q[16] = { 0, 27, 53, 79, 104, 128, 150, 171, 190, 207, 222, 234, 243, 250, 255, 256 };
	deg %= 360;
	if (deg < 0)
		deg += 360;
	int sign = deg >= 180 ? -1 : 1;
	deg %= 180;
	if (deg > 90)
		deg = 180 - deg;
	int i = deg / 6, r = deg % 6;
	return sign * (q[i] + (i < 15 ? (q[i + 1] - q[i]) * r / 6 : 0));
}

static void ring(int cx, int cy, int r, color_t c)
{
	int x = r, y = 0, e = 1 - r;
	while (x >= y) {
		gfx_pixel(cx + x, cy + y, c); gfx_pixel(cx - x, cy + y, c);
		gfx_pixel(cx + x, cy - y, c); gfx_pixel(cx - x, cy - y, c);
		gfx_pixel(cx + y, cy + x, c); gfx_pixel(cx - y, cy + x, c);
		gfx_pixel(cx + y, cy - x, c); gfx_pixel(cx - y, cy - x, c);
		y++;
		if (e < 0) {
			e += 2 * y + 1;
		} else {
			x--;
			e += 2 * (y - x) + 1;
		}
	}
}

static void line(int x0, int y0, int x1, int y1, color_t c)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
	int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
	for (;;) {
		gfx_pixel(x0, y0, c);
		if (x0 == x1 && y0 == y1)
			return;
		int e2 = 2 * e;
		if (e2 >= dy) {
			e += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			e += dx;
			y0 += sy;
		}
	}
}

// Where az, el sits on a sky plot of radius r (north up, east right, the zenith at the center)
static void sky_xy(int cx, int cy, int r, int az, int el, int *x, int *y)
{
	int d = r * (90 - (el < 0 ? 0 : el)) / 90;
	*x = cx + d * isin(az) / 256;
	*y = cy - d * isin(az + 90) / 256;
}

static void plot(int cx, int cy, int r, const ui_sat_live_t *live)
{
	ring(cx, cy, r, ui_col.line);
	ring(cx, cy, r / 2, ui_col.line);	// 45 degrees
	gfx_hline(cx - r, cy, 2 * r + 1, ui_col.line);
	gfx_vline(cx, cy - r, 2 * r + 1, ui_col.line);
	gfx_text(ui_layout.tiny, cx + 2, cy - r, "N", ui_col.dim);
	// The path: a segment between points, the part already flown dimmer
	uint8_t n = ui_svc_sat_path_count();
	uint16_t az;
	int8_t el;
	int px = 0, py = 0;
	for (uint8_t i = 0; i < n && ui_svc_sat_path(i, &az, &el); i++) {
		int x, y;
		sky_xy(cx, cy, r, az, el, &x, &y);
		if (i > 0)
			line(px, py, x, y, ui_col.fg);
		px = x;
		py = y;
	}
	if (live != NULL) {
		int x, y, d = r >= 40 ? 3 : 2;
		sky_xy(cx, cy, r, live->az, live->el, &x, &y);
		gfx_fill(x - d, y - d, 2 * d + 1, 2 * d + 1, live->el >= 0 ? ui_col.rx : ui_col.warn);
	}
}

// "label  value" rows from y; false once the next would run into the hint row. A value too wide for what is left of
// the row is cut at its first space (a frequency's shift goes first)
static bool trow(int x, int *y, const char *label, const char *value, color_t vc)
{
	const font_t *f = ui_layout.small;
	if (*y + f->h > ui_layout.hint_y)
		return false;
	char cut[32];
	if (x + gfx_text_width(f, label) + f->w + gfx_text_width(f, value) > gfx_width()) {
		str_copy(cut, sizeof cut, value);
		char *sp = strchr(cut, ' ');
		if (sp != NULL)
			*sp = '\0';
		value = cut;
	}
	int lx = gfx_text(f, x, *y, label, ui_col.dim);
	gfx_text(f, lx + f->w, *y, value, vc);
	*y += f->h + (ui_layout.row_h > f->h ? 1 : 0);
	return true;
}

// 12:34 or 1:02:03: a countdown in seconds
static void sb_clock(sbuf_t *b, uint32_t s)
{
	if (s >= 3600u) {
		sb_u32(b, s / 3600u, 1);
		sb_chr(b, ':');
	}
	sb_u32(b, s / 60u % 60u, s >= 3600u ? 2 : 1);
	sb_chr(b, ':');
	sb_u32(b, s % 60u, 2);
}

// A frequency with Doppler: 436.7954, and how far it is from the nominal ("+10.8k")
static void sb_shifted(sbuf_t *b, uint32_t hz, uint32_t nominal)
{
	sb_freq(b, hz);
	int32_t d = (int32_t)(hz - nominal);
	sb_chr(b, ' ');
	sb_chr(b, d < 0 ? '-' : '+');
	uint32_t a = (uint32_t)(d < 0 ? -d : d);
	sb_u32(b, a / 1000u, 1);
	sb_chr(b, '.');
	sb_u32(b, a % 1000u / 100u, 1);
	sb_chr(b, 'k');
}

static void track_draw(void)
{
	char buf[40];
	sbuf_t b;
	ui_sat_live_t live;
	ui_svc_sat_track(trk_aos, trk_los);	// asked again at every draw: the service stops when the screen is gone
	bool have = ui_svc_sat_live(&live);
	status();
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, open_name, TR("Tracking")) + 2;
	int w = gfx_width(), h = ui_layout.hint_y - y - 1;
	bool below = gfx_height() > w;		// a tall panel: the numbers under the plot
	int side = below ? w : (h < w / 2 ? h : w / 2);
	if (below && side > h / 2)
		side = h / 2;
	int r = side / 2 - 1;
	int cx = below ? w / 2 : r + 1, cy = y + r + 1;
	plot(cx, cy, r, have ? &live : NULL);
	int x = below ? 0 : cx + r + 5;
	int ty = below ? cy + r + 3 : y;
	const font_t *big = below || w >= 200 ? ui_layout.large : ui_layout.medium;
	if (!have) {
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, TR("Working"));
		sb_str(&b, GLYPH_ELLIPSIS);
		gfx_text(ui_layout.small, x, ty, buf, ui_col.dim);
		ty += ui_layout.small->h + 2;
	} else {
		// "62°" large, then the way across the sky
		sb_init(&b, buf, sizeof buf);
		sb_i32(&b, live.el);
		sb_str(&b, GLYPH_DEGREE);
		gfx_text(big, x, ty, buf, live.el >= 0 ? ui_col.rx : ui_col.warn);
		sb_init(&b, buf, sizeof buf);
		sb_dir_arrow(&b, trk_dir);
		gfx_text(ui_layout.medium, w - gfx_text_width(ui_layout.medium, buf) - 2, ty, buf, ui_col.fg);
		ty += big->h + 1;
		sb_init(&b, buf, sizeof buf);
		sb_u32(&b, live.az, 1);
		sb_str(&b, GLYPH_DEGREE " ");
		sb_compass(&b, live.az);
		trow(x, &ty, "Az", buf, ui_col.fg);
	}
	// The countdown: to the set while it is up, else to the rise (it comes before the rest: a small panel has no room)
	if (st.utc != 0) {
		sb_init(&b, buf, sizeof buf);
		bool up = st.utc >= trk_aos && st.utc < trk_los;
		if (st.utc >= trk_los)
			sb_str(&b, TR("Pass over"));
		else
			sb_clock(&b, up ? trk_los - st.utc : trk_aos - st.utc);
		trow(x, &ty, st.utc >= trk_los ? "" : up ? "LOS" : "AOS", buf, st.utc >= trk_los ? ui_col.warn : ui_col.fg);
	}
	ui_sat_profile_t p;
	if (have && ui_svc_sat_profile(&p)) {
		if (live.rx_hz && p.down_hz) {
			sb_init(&b, buf, sizeof buf);
			sb_shifted(&b, live.rx_hz, p.down_hz);
			trow(x, &ty, "RX", buf, ui_col.fg);
		}
		if (live.tx_hz && p.up_hz) {
			sb_init(&b, buf, sizeof buf);
			sb_shifted(&b, live.tx_hz, p.up_hz);
			trow(x, &ty, "TX", buf, ui_col.fg);
		}
	}
	if (have) {
		sb_init(&b, buf, sizeof buf);
		sb_u32(&b, (uint32_t)(live.range_km + 0.5f), 1);
		sb_str(&b, " km");
		trow(x, &ty, TR("Rng"), buf, ui_col.fg);
	}
	ui_draw_bottom(&ui_st, TR("Back"));
	ui_draw_banner(&ui_st);
}

const ui_scr_def_t ui_sat_track_screen = {
	// Not a field-use screen (P4): OK on a pass of the list, four levels from Home
	.name = "track", .flags = 0, .draw = track_draw,
};
#endif
