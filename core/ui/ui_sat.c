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
#include "sat/sat_clock.h"
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
	int mx = cx, my = cy;
	for (uint8_t i = 0; i < n && ui_svc_sat_path(i, &az, &el); i++) {
		int x, y;
		sky_xy(cx, cy, r, az, el, &x, &y);
		if (i > 0)
			line(px, py, x, y, ui_col.fg);
		if (i == UI_SAT_PATH / 2) {
			mx = x;
			my = y;
		}
		px = x;
		py = y;
	}
	if (n > UI_SAT_PATH / 2) {
		// The pass's way across the sky, beside the middle of its path on the side away from the center
		char ar[4];
		sbuf_t ab;
		sb_init(&ab, ar, sizeof ar);
		sb_dir_arrow(&ab, trk_dir);
		const font_t *f = ui_layout.small;
		int ax = mx + (mx >= cx ? 3 : -3 - f->w), ay = my + (my >= cy ? 1 : -1 - f->h);
		gfx_text(f, ax, ay, ar, ui_col.fg);
	}
	if (live != NULL) {
		int x, y, d = r >= 40 ? 3 : 2;
		sky_xy(cx, cy, r, live->az, live->el, &x, &y);
		gfx_fill(x - d, y - d, 2 * d + 1, 2 * d + 1, ui_col.rx);
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
	// The satellite is drawn only while it is above the horizon: below it, or outside the pass's margin, the plot shows
	// the path alone and the service works nothing out
	bool seen = have && live.el >= 0;
	plot(cx, cy, r, seen ? &live : NULL);
	int x = below ? 0 : cx + r + 5;
	int ty = below ? cy + r + 3 : y;
	const font_t *big = below || w >= 200 ? ui_layout.large : ui_layout.medium;
	int num_y = ty;		// the first row of the numbers
	bool near = st.utc == 0 || (st.utc + UI_SAT_TRACK_MARGIN_S >= trk_aos && st.utc < trk_los + UI_SAT_TRACK_MARGIN_S);
	if (!seen) {
		sb_init(&b, buf, sizeof buf);
		if (!have && near)
			sb_str(&b, TR("Working")), sb_str(&b, GLYPH_ELLIPSIS);
		else if (have)
			sb_str(&b, TR("Below horizon"));
		if (buf[0] != '\0') {
			gfx_text(ui_layout.small, x, ty, buf, ui_col.dim);
			ty += ui_layout.small->h + 2;
		}
	} else {
		// "62°" large, then the way across the sky
		sb_init(&b, buf, sizeof buf);
		sb_i32(&b, live.el);
		sb_str(&b, GLYPH_DEGREE);
		gfx_text(big, x, ty, buf, ui_col.rx);
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
	if (seen && ui_svc_sat_profile(&p)) {
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
	// Where you are, for the grid locator a contact asks for. On a tall panel it sits right of the elevation (the
	// numbers fill the rows below it); beside the plot, where the rows are short, it comes before the range, which a
	// small panel drops
	char loc[8];
	bool grid = false;
	if (st.pos_ok) {
		sb_init(&b, loc, sizeof loc);
		sb_locator(&b, st.lat_e7, st.lon_e7);
		grid = true;
		if (below) {
			const font_t *sm = ui_layout.small;
			sb_init(&b, buf, sizeof buf);
			sb_str(&b, TR("Grid"));
			sb_chr(&b, ' ');
			sb_str(&b, loc);
			gfx_text(sm, w - gfx_text_width(sm, buf) - 2, num_y + (big->h - sm->h) / 2, buf, ui_col.dim);
			grid = false;
		}
	}
	if (grid)
		trow(x, &ty, TR("Grid"), loc, ui_col.fg);
	if (seen) {
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
// --- setting the clock (Settings > Location > Set time) ---

enum { CF_YEAR, CF_MONTH, CF_DAY, CF_HOUR, CF_MIN, CF_SEC, CF_COUNT };
static uint16_t clk[CF_COUNT];		// the fields as shown (UTC)
static uint8_t clk_sel;			// the field the keys change
static uint8_t clk_typed;		// digits typed into it so far (0, 1)

static const uint16_t clk_lo[CF_COUNT] = { 2026, 1, 1, 0, 0, 0 };
static const uint16_t clk_hi[CF_COUNT] = { 2099, 12, 31, 23, 59, 59 };

static void clock_enter(void)
{
	status();
	uint32_t u = st.utc != 0 ? st.utc : sat_clock_from_civil(2026, 1, 1, 0, 0, 0);
	uint16_t y;
	uint8_t v[5];
	sat_clock_civil(u, &y, &v[0], &v[1], &v[2], &v[3], &v[4]);
	clk[CF_YEAR] = y < clk_lo[CF_YEAR] ? clk_lo[CF_YEAR] : y;
	for (uint8_t i = 0; i < 5; i++)
		clk[CF_MONTH + i] = v[i];
	clk_sel = CF_HOUR;
	clk_typed = 0;
}

// "2026-11-06", then "14:07:30": each field apart, the selected one underlined
static int clock_field(const font_t *f, int x, int y, uint8_t i, const char *sep)
{
	char t[8];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	sb_u32(&b, clk[i], i == CF_YEAR ? 4 : 2);
	int w = gfx_text_width(f, t);
	gfx_text(f, x, y, t, i == clk_sel ? ui_col.fg : ui_col.dim);
	if (i == clk_sel)
		gfx_fill(x, y + f->h, w, 2, ui_col.fg);
	x += w;
	if (sep[0] != '\0') {
		gfx_text(f, x, y, sep, ui_col.dim);
		x += gfx_text_width(f, sep);
	}
	return x;
}

static void clock_draw(void)
{
	char buf[40];
	sbuf_t b;
	status();
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("Set time"), "UTC") + 4;
	const font_t *f = gfx_width() >= 200 ? ui_layout.large : ui_layout.medium;
	int w = gfx_width();
	// the two lines, centered
	int dw = gfx_text_width(f, "2026-11-06"), tw = gfx_text_width(f, "14:07:30");
	int x = (w - dw) / 2;
	for (uint8_t i = CF_YEAR; i <= CF_DAY; i++)
		x = clock_field(f, x, y, i, i == CF_DAY ? "" : "-");
	y += f->h + 6;
	x = (w - tw) / 2;
	for (uint8_t i = CF_HOUR; i <= CF_SEC; i++)
		x = clock_field(f, x, y, i, i == CF_SEC ? "" : ":");
	y += f->h + 8;
	// the same moment in local time, to check it against a watch
	uint32_t u = sat_clock_from_civil(clk[CF_YEAR], clk[CF_MONTH], clk[CF_DAY], clk[CF_HOUR], clk[CF_MIN], clk[CF_SEC]);
	if (u != 0 && y + ui_layout.small->h < ui_layout.hint_y) {
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, TR("Local"));
		sb_chr(&b, ' ');
		sb_hhmm(&b, (uint32_t)((int32_t)u + st.utc_offset_min * 60));
		sb_str(&b, "  ");
		sb_utc_offset(&b, st.utc_offset_min);
		gfx_text(ui_layout.small, (w - gfx_text_width(ui_layout.small, buf)) / 2, y, buf, ui_col.dim);
	}
	ui_draw_bottom(&ui_st, TR("# next field  OK set"));
	ui_draw_banner(&ui_st);
}

static void clock_step(int d)
{
	int v = clk[clk_sel] + d;
	if (v > clk_hi[clk_sel])
		v = clk_lo[clk_sel];
	else if (v < clk_lo[clk_sel])
		v = clk_hi[clk_sel];
	clk[clk_sel] = (uint16_t)v;
	clk_typed = 0;
}

static bool clock_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS;
	bool repeat = ev->gesture == GESTURE_REPEAT;
	if (ev->action == ACT_ENC_STEP) {
		clock_step(ev->value > 0 ? 1 : -1);
	} else if ((press || repeat) && (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN)) {
		clock_step(ev->action == ACT_NAV_UP ? 1 : -1);
	} else if (press && (ev->action == ACT_NAV_RIGHT || ev->action == ACT_CONTEXT)) {	// # for a keypad with no Right
		clk_sel = (uint8_t)((clk_sel + 1u) % CF_COUNT);
		clk_typed = 0;
	} else if (press && ev->action == ACT_NAV_LEFT) {
		clk_sel = (uint8_t)((clk_sel + CF_COUNT - 1u) % CF_COUNT);
		clk_typed = 0;
	} else if (press && ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9) {
		// Two digits fill a field (the year's last two) and move to the next
		uint16_t d = (uint16_t)(ev->action - ACT_DIGIT_0);
		if (clk_typed == 0) {
			clk[clk_sel] = clk_sel == CF_YEAR ? (uint16_t)(2000u + d) : d;
			clk_typed = 1;
		} else {
			uint16_t v = clk_sel == CF_YEAR ? (uint16_t)(2000u + clk[clk_sel] % 10u * 10u + d) :
				(uint16_t)(clk[clk_sel] * 10u + d);
			clk[clk_sel] = v < clk_lo[clk_sel] ? clk_lo[clk_sel] : v > clk_hi[clk_sel] ? clk_hi[clk_sel] : v;
			clk_typed = 0;
			if (clk_sel + 1u < CF_COUNT)
				clk_sel++;
		}
	} else if (press && ev->action == ACT_SELECT) {
		uint32_t u = sat_clock_from_civil(clk[CF_YEAR], clk[CF_MONTH], clk[CF_DAY], clk[CF_HOUR], clk[CF_MIN],
						  clk[CF_SEC]);
		if (u == 0) {
			ui_toast(TR("Not a date"));
		} else if (ui_svc_sat_clock_set(u)) {
			ui_toast(TR("Clock set"));
			ui_pop();
		} else {
			ui_toast(TR("Couldn't set the clock"));
		}
	} else {
		return false;
	}
	ui_dirty();
	return true;
}

const ui_scr_def_t ui_sat_clock_screen = {
	.name = "set-time", .flags = 0, .key = clock_key, .draw = clock_draw, .enter = clock_enter,
};
#endif
