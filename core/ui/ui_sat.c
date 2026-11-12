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
#include "sat/sat_follow.h"
#include "sat/sat_doppler.h"
#include "fmt.h"
#include "i18n.h"
#include "radio/radio.h"

static ui_sat_status_t st;		// read once a draw (status())
static uint32_t st_ms = 1;		// ... the UI's time then
static char open_name[12];
static uint32_t open_norad;		// that satellite's catalog number
static uint32_t trk_aos, trk_los, trk_tca;	// the pass being tracked (UTC); tca is its highest point
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

// With CONFIG_SAT_SOUND the list ends with one more row, "Receive sound"
static uint16_t list_count(void)
{
	status();
	return (uint16_t)(st.count + (CONFIG_SAT_SOUND ? 1u : 0u));
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
#if CONFIG_SAT_SOUND
	} else if (st.count == 0) {
		// the list is never empty with the Receive sound row: say what the .empty text says elsewhere
		sb_str(&b, TR("No satellites stored: load them over the cable, or by sound"));
#endif
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
#if CONFIG_SAT_SOUND
	if (i == st.count) {
		str_copy(it->label, sizeof it->label, TR("Receive sound"));
		if (i < 9)
			item_num_set(it, (uint16_t)(i + 1u));
		return;
	}
#endif
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
#if CONFIG_SAT_SOUND
	if (i == st.count) {
		ui_push(SCR_SAT_SOUND, 0);
		return;
	}
#endif
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
	open_norad = s.norad;
	ui_svc_sat_open(s.norad);
	if (hold && (s.state == UI_SAT_UP || s.state == UI_SAT_NEXT)) {
		// Long OK: straight to the next pass (the one under way), skipping the list
		trk_aos = s.aos;
		trk_los = s.los;
		trk_tca = s.tca;
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
	trk_tca = p.tca;
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

// "label  value" like trow, but the value is the first of the candidates that fits the row (longest first), so a wide panel
// shows the time and the countdown and a narrow one only the time
static bool trow_fit(int x, int *y, const char *label, const char *const *cand, int n, color_t vc)
{
	const font_t *f = ui_layout.small;
	int i = 0;
	while (i < n - 1 && x + gfx_text_width(f, label) + f->w + gfx_text_width(f, cand[i]) > gfx_width())
		i++;
	return trow(x, y, label, cand[i], vc);
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

// --- keys on Tracking: tune the receiver to the downlink, the squelch, the dial's nudge ---
// docs/dev/satellite-doppler-plan.md. Opening Tracking changes nothing: the first # puts the radio in the VFO on the
// satellite's profile, later ones follow the Doppler; Back puts the channel and the squelch back

#define TRK_NUDGE_STEP_HZ	1000		// one detent of the dial
#define TRK_NUDGE_MAX_HZ	10000
#define TRK_BEHIND_HZ		1500		// the receiver this far from where it should be: shown as behind

static bool trk_tuned;			// # was pressed: the receiver is on this pass's downlink
static bool trk_entered_vfo;		// Tracking put the radio in the VFO: Back leaves it
static bool trk_mon_ours;		// Tracking latched the squelch open: Back closes it
static int32_t trk_nudge_hz;		// the dial's correction, added to every downlink
static sat_follow_t trk_follow;		// * : the receiver follows the Doppler by itself (core/sat/sat_follow.c)
static uint32_t trk_tune_ms;		// when # last asked the radio to tune: the view takes a moment to follow
static bool trk_rx_only;		// Tracking made the VFO receive only (a typed frequency, a satellite with no uplink)
static uint32_t alt_hz;			// the downlink saved for this satellite (the Frequency screen, settings), 0 none
static bool trk_alt_on;			// OK swapped to it: the Doppler works on it, and the VFO only listens

// The downlink the Doppler works on: the one saved for this satellite when OK swapped to it, else the profile's
static uint32_t track_base(const ui_sat_profile_t *p)
{
	return trk_alt_on && alt_hz != 0 ? alt_hz : p->down_hz;
}

// A typed frequency has nothing to transmit on, and neither has a satellite with no uplink: the VFO only listens
static bool track_listen_only(const ui_sat_profile_t *p)
{
	return p->up_hz == 0 || (trk_alt_on && alt_hz != 0);
}

// The frequency saved for the satellite that is open: the settings hold one, with its catalog number
static uint32_t alt_load(void)
{
	return (uint32_t)ui_svc_setting_get(UI_SET_SAT_ALT_NORAD) == open_norad && open_norad != 0 ?
		(uint32_t)ui_svc_setting_get(UI_SET_SAT_ALT_HZ) : 0u;
}

// Where the receiver should be: the Doppler-corrected downlink while the satellite is up, else the profile's own, and
// the dial's nudge
static uint32_t track_want(const ui_sat_profile_t *p)
{
	ui_sat_live_t l;
	uint32_t f0 = track_base(p);
	// Worked out from two minutes before the rise, so the receiver is on the rise's shift when the satellite comes up.
	// From the range rate, so a frequency typed instead of the profile's gets its own shift
	uint32_t base = ui_svc_sat_live(&l) ? sat_doppler_rx_hz(f0, (double)l.rate_kms) : f0;
	return (uint32_t)((int32_t)base + trk_nudge_hz);
}

static void track_enter(void)
{
	trk_tuned = trk_entered_vfo = trk_mon_ours = trk_rx_only = trk_alt_on = false;
	alt_hz = alt_load();
	trk_nudge_hz = 0;
	trk_follow.on = false;
	trk_follow.last_ms = 0;
}

static void track_leave(void)
{
	const radio_view_t *v = ui_view();
	if (trk_mon_ours && v->monitor)
		ui_svc_monitor(false);
	if (trk_rx_only)
		radio_req_vfo_rx_only(false);	// the VFO is the user's again
	if (trk_entered_vfo && v->vfo)
		radio_req_vfo(false);	// the channel that was on view
	track_enter();
}

// clear: hold #: the dial's nudge goes and the receiver is retuned to the computed frequency alone
static void track_tune(bool clear)
{
	const radio_view_t *v = ui_view();
	ui_sat_profile_t p;
	if (!ui_svc_sat_profile(&p) || track_base(&p) == 0) {
		ui_toast(TR("No downlink set for this satellite"));
		return;
	}
	if (v->state == RS_TX) {
		ui_toast(TR("Not while transmitting"));
		return;
	}
	if (clear)
		trk_nudge_hz = 0;
	uint32_t hz = track_want(&p);
	trk_tune_ms = ui_st.now_ms;
	sat_follow_retuned(&trk_follow, ui_st.now_ms);	// a retune of the user's: the automatic one waits its interval
	if (trk_tuned && v->vfo) {
		radio_req_rx_hz(hz);	// a light retune: one rx() call, a reception in progress goes on
		ui_toast(clear ? TR("Dial cleared") : TR("Retuned"));
		return;
	}
	// The first time: the temporary VFO on the satellite's profile, FM 25 kHz, no receive tone; the uplink as a split
	// transmit with its tone. The channel on view comes back on Back
	if (!v->vfo)
		trk_entered_vfo = true;
	radio_req_vfo(true);
	ui_svc_mode(RADIO_MODE_FM);
	radio_req_bandwidth(RADIO_BW_25K);
	radio_req_tone(RADIO_TONE_RX, (subtone_t){ TONE_NONE, 0 });
	radio_req_vfo_set(hz);
	trk_rx_only = track_listen_only(&p);
	radio_req_vfo_rx_only(trk_rx_only);
	if (p.up_hz != 0) {
		radio_req_vfo_tx(p.up_hz);
		radio_req_tone(RADIO_TONE_TX, p.up_tone != 0 ? (subtone_t){ TONE_CTCSS, p.up_tone } : (subtone_t){ TONE_NONE, 0 });
	}
	trk_tuned = true;
	ui_toast(TR("Tuned to the downlink"));
}

// * : on, when the receiver is tuned to the satellite (tuning it first when it is not), or off
static void track_auto(void)
{
	if (trk_follow.on) {
		trk_follow.on = false;
		ui_toast(TR("Auto off"));
		return;
	}
	if (!(trk_tuned && ui_view()->vfo)) {
		track_tune(false);
		if (!trk_tuned)
			return;		// refused (no downlink, transmitting): its toast says why
	}
	trk_follow.on = true;
	sat_follow_retuned(&trk_follow, ui_st.now_ms);
	ui_toast(TR("Auto on: following the Doppler"));
}

// Once a draw while following: retune when the receiver is the threshold off, at the interval. The draw is the
// screen's tick (it comes about once a second); the decision is core/sat/sat_follow.c's
static void track_follow(void)
{
	if (!trk_follow.on)
		return;
	const radio_view_t *v = ui_view();
	if (!v->vfo) {
		// The radio takes a moment to enter the VFO; after that it is the user's doing (another channel): stop
		if (ui_st.now_ms - trk_tune_ms > 1500u) {
			trk_follow.on = trk_tuned = false;
			ui_toast(TR("Auto off"));
		}
		return;
	}
	ui_sat_profile_t p;
	if (!ui_svc_sat_profile(&p) || track_base(&p) == 0)
		return;
	uint32_t want = track_want(&p);
	if (sat_follow_step(&trk_follow, ui_st.now_ms, v->ch.rx_hz, want, v->state == RS_TX))
		radio_req_rx_hz(want);
}

static void track_squelch(void)
{
	const radio_view_t *v = ui_view();
	if (v->ch.mode == RADIO_MODE_DMR) {
		ui_toast(TR("Squelch: FM only"));
		return;
	}
	bool on = !v->monitor;
	if (!ui_svc_monitor(on))
		return;
	trk_mon_ours = on;
	ui_toast(on ? TR("Squelch open") : TR("Squelch normal"));
}

static void track_nudge(int dir)
{
	int32_t n = trk_nudge_hz + dir * TRK_NUDGE_STEP_HZ;
	trk_nudge_hz = n > TRK_NUDGE_MAX_HZ ? TRK_NUDGE_MAX_HZ : n < -TRK_NUDGE_MAX_HZ ? -TRK_NUDGE_MAX_HZ : n;
	static char buf[24];	// a toast keeps the pointer it is given
	sbuf_t b;
	sb_init(&b, buf, sizeof buf);
	sb_str(&b, TR("Dial"));
	sb_chr(&b, ' ');
	sb_chr(&b, trk_nudge_hz < 0 ? '-' : '+');
	uint32_t a = (uint32_t)(trk_nudge_hz < 0 ? -trk_nudge_hz : trk_nudge_hz);
	sb_u32(&b, a / 1000u, 1);
	sb_chr(&b, '.');
	sb_u32(&b, a % 1000u / 100u, 1);
	sb_chr(&b, 'k');
	ui_toast(buf);
	ui_sat_profile_t p;
	if (trk_tuned && ui_view()->vfo && ui_svc_sat_profile(&p) && track_base(&p) != 0)
		radio_req_rx_hz(track_want(&p));
}

static void track_alt_swap(void);	// below, with the Frequency screen

static bool track_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS, repeat = ev->gesture == GESTURE_REPEAT;
	if (ev->action == ACT_ENC_STEP && ev->value != 0) {
		track_nudge(ev->value > 0 ? 1 : -1);
		return true;
	}
	if ((press || repeat) && (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN)) {	// a radio with no dial
		track_nudge(ev->action == ACT_NAV_UP ? 1 : -1);
		return true;
	}
	if (press && ev->action == ACT_CONTEXT) {	// #
		track_tune(false);
		return true;
	}
	if (press && ev->action == ACT_SELECT) {	// OK: swap to your frequency and back (type one first when there is none)
		track_alt_swap();
		return true;
	}
	if (press && ev->action == ACT_DIGIT_1) {	// 1: type a frequency, or change the saved one
		ui_push(SCR_SAT_FREQ, 0);
		return true;
	}
	if (ev->gesture == GESTURE_LONG && ev->action == ACT_CONTEXT) {	// hold #
		track_tune(true);
		return true;
	}
	if (press && ev->action == ACT_QUICK) {	// *
		track_auto();
		return true;
	}
	if (press && ev->action == ACT_DIGIT_0) {
		track_squelch();
		return true;
	}
	return false;
}

static void track_draw(void)
{
	char buf[40];
	sbuf_t b;
	ui_sat_live_t live;
	ui_svc_sat_track(trk_aos, trk_los);	// asked again at every draw: the service stops when the screen is gone
	bool have = ui_svc_sat_live(&live);
	track_follow();
	status();
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, open_name, TR("Tracking")) + 2;
	int w = gfx_width(), h = ui_layout.hint_y - y - 1;
	bool below = gfx_height() > w;		// a tall panel: the numbers under the plot
	const font_t *fs = ui_layout.small;
	int rh = fs->h + (ui_layout.row_h > fs->h ? 1 : 0);	// a row of the numbers
	const font_t *big = below || w >= 200 ? ui_layout.large : ui_layout.medium;
	// The satellite is drawn only while it is above the horizon: below it, or outside the pass's margin, the plot shows
	// the path alone and the service works nothing out
	bool seen = have && live.el >= 0;
	// What the numbers will be, worked out first: a tall panel sizes the plot by the rows they need
	// What the radio is on, always: an operator should know the receive and transmit frequencies. Tuned (#), the receive
	// one shows its shift from the satellite's own, in the warning color while it is behind where it should be (the
	// Doppler moved on, or the dial); not tuned, the corrected one to tune to comes after
	ui_sat_profile_t p;
	bool prof = ui_svc_sat_profile(&p);
	bool have_p = prof && track_base(&p) != 0;
	const radio_view_t *rv = ui_view();
	bool on = trk_tuned && rv->vfo;
	char rxs[24], txs[24];
	color_t rc = ui_col.fg, tc = ui_col.fg;
	rxs[0] = txs[0] = '\0';
	if (rv->ch.rx_hz != 0) {
		sbuf_t rb;
		sb_init(&rb, rxs, sizeof rxs);
		if (on && have_p) {
			sb_shifted(&rb, rv->ch.rx_hz, track_base(&p));
			int32_t behind = (int32_t)(track_want(&p) - rv->ch.rx_hz);
			rc = (behind >= (int32_t)SAT_FOLLOW_THRESHOLD_HZ || behind <= -(int32_t)SAT_FOLLOW_THRESHOLD_HZ) ? ui_col.warn : ui_col.rx;
		} else {
			sb_freq(&rb, rv->ch.rx_hz);
		}
	}
	if (rv->ch.tx_hz != 0) {
		sbuf_t tb;
		sb_init(&tb, txs, sizeof txs);
		bool none = on && trk_rx_only;		// listening only: nothing is transmitted on it
		if (none)
			sb_str(&tb, TR("RX only"));
		else
			sb_freq(&tb, rv->ch.tx_hz);
		tc = none ? ui_col.dim : ui_col.fg;
	}
	// A tall panel is wide enough for RX and TX on one row, if the text is short enough
	int rxw = gfx_text_width(fs, "RX") + fs->w + gfx_text_width(fs, rxs);
	int txw = gfx_text_width(fs, "TX") + fs->w + gfx_text_width(fs, txs);
	bool one_row = below && rxs[0] != '\0' && txs[0] != '\0' && rxw + 2 * fs->w + txw <= w;
	bool alt_row = alt_hz != 0 && prof && p.down_hz != 0;
	bool tune_row = !on && have_p;
	// Where you are, for the grid locator a contact asks for
	char loc[8];
	loc[0] = '\0';
	if (st.pos_ok) {
		sbuf_t lb;
		sb_init(&lb, loc, sizeof loc);
		sb_locator(&lb, st.lat_e7, st.lon_e7);
	}
	// The pass's three times in the chosen zone (Pass times: local or UTC), the highest point from the service
	uint32_t t_max = trk_tca != 0 ? trk_tca : trk_aos / 2u + trk_los / 2u;
	char t_aos[12], t_max_s[12], t_los[12];
	sbuf_t tbf;
	// To the second on a tall panel (a pass of a few minutes: seconds matter); beside the plot there is no width for them
	void (*sbt)(sbuf_t *, uint32_t) = below ? sb_hhmmss : sb_hhmm;
	sb_init(&tbf, t_aos, sizeof t_aos);
	sbt(&tbf, local(trk_aos));
	sb_init(&tbf, t_max_s, sizeof t_max_s);
	sbt(&tbf, local(t_max));
	sb_init(&tbf, t_los, sizeof t_los);
	sbt(&tbf, local(trk_los));
	bool times = trk_aos != 0 && trk_los != 0;
	// On a tall panel: the three in one row of columns if they fit, else AOS and MAX on a row and LOS under them (with the
	// locator after it): always in the order they happen
	int tw[3];
	tw[0] = gfx_text_width(fs, "AOS") + fs->w + gfx_text_width(fs, t_aos);
	tw[1] = gfx_text_width(fs, "MAX") + fs->w + gfx_text_width(fs, t_max_s);
	tw[2] = gfx_text_width(fs, "LOS") + fs->w + gfx_text_width(fs, t_los);
	bool t_cols = below && times && tw[0] + tw[1] + tw[2] + 2 * fs->w <= w;
	// Too wide in the usual text: the narrowest text, with a thin gap between a label and its time, to keep all three on a row
	const font_t *tf = fs;
	int tgap = fs->w, twm[3];
	if (below && times && !t_cols) {
		const font_t *mf = ui_layout.micro;
		const char *tl[3] = { "AOS", "MAX", "LOS" }, *tv[3] = { t_aos, t_max_s, t_los };
		int tot = 0;
		for (int k = 0; k < 3; k++) {
			twm[k] = gfx_text_width(mf, tl[k]) + mf->w / 2 + gfx_text_width(mf, tv[k]);
			tot += twm[k];
		}
		if (tot + 2 * mf->w <= w) {
			t_cols = true;
			tf = mf;
			tgap = mf->w / 2;
			for (int k = 0; k < 3; k++)
				tw[k] = twm[k];
		}
	}
	bool t_pair = !t_cols && tw[0] + tw[1] + 2 * fs->w <= w;
	int t_rows = !below || !times ? 0 : t_cols ? 1 : t_pair ? 2 : 3;
	// The azimuth: on a tall panel at the right of the countdown's row when the worst case countdown leaves room for it
	char az[24];
	az[0] = '\0';
	if (seen) {
		sbuf_t ab;
		sb_init(&ab, az, sizeof az);
		sb_u32(&ab, live.az, 1);
		sb_str(&ab, GLYPH_DEGREE " ");
		sb_compass(&ab, live.az);
	}
	int az_w = gfx_text_width(fs, "Az") + fs->w + gfx_text_width(fs, az);
	// The countdown: to the set while it is up, else to the rise
	bool up = st.utc >= trk_aos && st.utc < trk_los;
	char cd[16];
	cd[0] = '\0';
	if (st.utc != 0) {
		sbuf_t cb;
		sb_init(&cb, cd, sizeof cd);
		if (st.utc >= trk_los)
			sb_str(&cb, TR("Pass over"));
		else
			sb_clock(&cb, up ? trk_los - st.utc : trk_aos - st.utc);
	}
	const char *cd_word = st.utc >= trk_los ? "" : up ? TR("Sets in") : TR("Rises in");
	bool az_inline = below && seen && st.utc != 0 &&
			 gfx_text_width(fs, cd_word) + fs->w + gfx_text_width(fs, cd) + 2 * fs->w + az_w <= w;
	int side = below ? w : (h < w / 2 ? h : w / 2);
	if (below) {
		int need = (seen ? (az_inline ? 0 : rh) : have ? fs->h + 2 : 0) + (st.utc != 0 ? rh : 0) + t_rows * rh +
			   (one_row ? rh : (rxs[0] != '\0' ? rh : 0) + (txs[0] != '\0' ? rh : 0)) + (alt_row ? rh : 0) +
			   (tune_row ? rh : 0) + (trk_follow.on && !one_row ? rh : 0) + (seen ? rh : 0);
		int fit = h - need - 3;
		if (side > h / 2)
			side = h / 2;
		if (side > fit)
			side = fit < w / 3 ? w / 3 : fit;	// not so small the sky plot is no use: the rows past it are dropped from the bottom
	}
	int r = side / 2 - 1;
	int cx = below ? w / 2 : r + 1, cy = y + r + 1;
	plot(cx, cy, r, seen ? &live : NULL);
	if (below) {
		// The corners of the plot's square are empty: the elevation large at the left (nothing while below the horizon), the
		// locator at the right, bare ("FN42li": a contact that asks for it knows what it looks like)
		if (seen) {
			sbuf_t eb;
			char el[8];
			sb_init(&eb, el, sizeof el);
			sb_i32(&eb, live.el);
			sb_str(&eb, GLYPH_DEGREE);
			gfx_text(big, 0, y + 1, el, ui_col.rx);
		}
		if (loc[0] != '\0')
			gfx_text(fs, w - gfx_text_width(fs, loc) - 2, y + 1, loc, ui_col.dim);
	}
	int x = below ? 0 : cx + r + 5;
	int ty = below ? cy + r + 3 : y;
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
	} else if (below) {
		if (!az_inline)
			trow(x, &ty, "Az", az, ui_col.fg);
	} else {
		// "62°" large, then the way across the sky: on the same row when it fits (a line saved for the times), else under it
		sb_init(&b, buf, sizeof buf);
		sb_i32(&b, live.el);
		sb_str(&b, GLYPH_DEGREE);
		int ex = gfx_text(big, x, ty, buf, ui_col.rx);
		const font_t *sm = ui_layout.small;
		int aw = gfx_text_width(sm, "Az") + sm->w + gfx_text_width(sm, az);
		if (ex + sm->w + aw <= gfx_width()) {
			int ax = gfx_width() - aw - 2;
			if (ax < ex + sm->w)
				ax = ex + sm->w;
			int lx = gfx_text(sm, ax, ty + big->h - sm->h, "Az", ui_col.dim);
			gfx_text(sm, lx + sm->w, ty + big->h - sm->h, az, ui_col.fg);
			ty += big->h + 1;
		} else if (ex + sm->w + gfx_text_width(sm, az) <= gfx_width()) {	// no room for the label: the compass says it
			gfx_text(sm, gfx_width() - gfx_text_width(sm, az) - 2, ty + big->h - sm->h, az, ui_col.fg);
			ty += big->h + 1;
		} else {
			ty += big->h + 1;
			trow(x, &ty, "Az", az, ui_col.fg);
		}
	}
	// The countdown: to the set while it is up, else to the rise (it comes before the rest: a small panel has no room).
	// Beside the plot the row also carries the time of that event, the locator after them when it fits
	if (st.utc != 0) {
		char c1[40], c2[40], c3[40];
		const char *ev = up ? t_los : t_aos;
		const char *cands[3];
		int nc = 0;
		if (below || !times || st.utc >= trk_los) {
			cands[nc++] = cd;
		} else {
			sb_init(&b, c1, sizeof c1);
			sb_str(&b, ev), sb_str(&b, " "), sb_str(&b, cd);
			if (loc[0] != '\0') {
				sb_init(&b, c3, sizeof c3);
				sb_str(&b, c1), sb_str(&b, "  "), sb_str(&b, loc);
				cands[nc++] = c3;
			}
			cands[nc++] = c1;
			sb_init(&b, c2, sizeof c2);
			sb_str(&b, ev);
			cands[nc++] = c2;
		}
		// Beside the plot the label is the event (its time is in the row); on a tall panel the times row below has AOS and LOS,
		// so this one says what the countdown is to, in words
		const char *lab = st.utc >= trk_los ? "" : below ? cd_word : up ? "LOS" : "AOS";
		int cd_y = ty;
		trow_fit(x, &ty, lab, cands, nc, st.utc >= trk_los ? ui_col.warn : ui_col.fg);
		if (az_inline && cd_y + fs->h <= ui_layout.hint_y) {
			int lx = gfx_text(fs, w - az_w - 2, cd_y, "Az", ui_col.dim);
			gfx_text(fs, lx + fs->w, cd_y, az, ui_col.fg);
		}
	}
	// The pass's times (a tall panel; beside the plot they come after the frequencies, below)
	if (below && times) {
		int gap = t_cols ? (w - tw[0] - tw[1] - tw[2]) / 2 : 0;
		if (t_cols && ty + tf->h <= ui_layout.hint_y) {
			const char *lab[3] = { "AOS", "MAX", "LOS" }, *val[3] = { t_aos, t_max_s, t_los };
			int px = 0;
			for (int k = 0; k < 3; k++) {
				int lx = gfx_text(tf, px, ty, lab[k], ui_col.dim);
				gfx_text(tf, lx + tgap, ty, val[k], ui_col.fg);
				px += tw[k] + gap;
			}
			ty += tf->h + 3;
		} else if (t_pair) {
			if (ty + fs->h <= ui_layout.hint_y) {
				int lx = gfx_text(fs, 0, ty, "AOS", ui_col.dim);
				lx = gfx_text(fs, lx + fs->w, ty, t_aos, ui_col.fg);
				lx = gfx_text(fs, lx + 2 * fs->w, ty, "MAX", ui_col.dim);
				gfx_text(fs, lx + fs->w, ty, t_max_s, ui_col.fg);
				ty += rh;
			}
			if (ty + fs->h <= ui_layout.hint_y) {
				int lx = gfx_text(fs, 0, ty, "LOS", ui_col.dim);
				gfx_text(fs, lx + fs->w, ty, t_los, ui_col.fg);
				ty += rh;
			}
		} else {
			trow(x, &ty, "AOS", t_aos, ui_col.fg);
			trow(x, &ty, "MAX", t_max_s, ui_col.fg);
			trow(x, &ty, "LOS", t_los, ui_col.fg);
		}
	}
	if (one_row && ty + fs->h <= ui_layout.hint_y) {
		int lx = gfx_text(fs, 0, ty, "RX", ui_col.dim);
		gfx_text(fs, lx + fs->w, ty, rxs, rc);
		lx = gfx_text(fs, w - txw - 2, ty, "TX", ui_col.dim);
		gfx_text(fs, lx + fs->w, ty, txs, tc);
		ty += rh;
	} else {
		if (rxs[0] != '\0')
			trow(x, &ty, "RX", rxs, rc);
		if (txs[0] != '\0')
			trow(x, &ty, "TX", txs, tc);
	}
	if (alt_row) {
		// What OK swaps to: the frequency not listened on now (two letters like RX: a longer label pushes it off a narrow panel)
		sb_init(&b, buf, sizeof buf);
		sb_freq(&b, trk_alt_on ? p.down_hz : alt_hz);
		trow(x, &ty, "OK", buf, ui_col.dim);
	}
	if (tune_row) {
		sb_init(&b, buf, sizeof buf);
		sb_shifted(&b, track_want(&p), track_base(&p));
		// What # tunes to. A narrow panel has no room for the word beside the whole frequency: a ">" stands in for it there, and
		// the shift is the first to go
		char fq[16];
		str_copy(fq, sizeof fq, buf);
		char *sp = strchr(fq, ' ');
		if (sp != NULL)
			*sp = '\0';
		const char *word = TR("Tune");
		bool room = x + gfx_text_width(ui_layout.small, word) + ui_layout.small->w + gfx_text_width(ui_layout.small, fq) <= gfx_width();
		trow(x, &ty, room ? word : ">", buf, ui_col.dim);
	}
	if (trk_follow.on) {
		// On a tall panel "AUTO" goes in the plot's empty lower right corner; else an "Auto" row
		const font_t *sm = ui_layout.small;
		int ax = w - gfx_text_width(sm, "AUTO") - 2;
		if (below)	// the plot's lower right corner is empty
			gfx_text(sm, ax, y + side - sm->h, "AUTO", ui_col.rx);
		else
			trow(x, &ty, "Auto", TR("on"), ui_col.rx);
	}
	// Beside the plot the pass's times come after the frequencies, the highest point first, the other end of the pass last:
	// each goes only where a row is left
	bool up2 = st.utc >= trk_aos && st.utc < trk_los;
	if (!below && times)
		trow(x, &ty, "MAX", t_max_s, ui_col.dim);
	if (seen) {
		sb_init(&b, buf, sizeof buf);
		sb_u32(&b, (uint32_t)(live.range_km + 0.5f), 1);
		sb_str(&b, " km");
		trow(x, &ty, TR("Rng"), buf, ui_col.fg);
	}
	if (!below && times && st.utc != 0 && st.utc < trk_los)
		trow(x, &ty, up2 ? "AOS" : "LOS", up2 ? t_aos : t_los, ui_col.dim);
	ui_draw_bottom(&ui_st, trk_follow.on ? TR(TEXT_ALT_OPEN "0 Squelch  # Retune  * Stop  OK Freq  Back" TEXT_ALT_NEXT "0 Sql  # Tune  * Stop  OK Freq  Back" TEXT_ALT_NEXT "0 Sql  # Tune  * Stop  Back" TEXT_ALT_NEXT "# Tune  * Stop  Back" TEXT_ALT_CLOSE) :
		       trk_tuned ? TR(TEXT_ALT_OPEN "0 Squelch  # Retune  * Auto  OK Freq  Back" TEXT_ALT_NEXT "0 Sql  # Tune  * Auto  OK Freq  Back" TEXT_ALT_NEXT "0 Sql  # Tune  * Auto  Back" TEXT_ALT_NEXT "# Tune  * Auto  Back" TEXT_ALT_CLOSE) :
		       TR(TEXT_ALT_OPEN "0 Squelch  # Tune  * Auto  OK Freq  Back" TEXT_ALT_NEXT "0 Sql  # Tune  * Auto  OK Freq  Back" TEXT_ALT_NEXT "0 Sql  # Tune  * Auto  Back" TEXT_ALT_NEXT "# Tune  * Auto  Back" TEXT_ALT_CLOSE));
	ui_draw_banner(&ui_st);
}

// --- a downlink of your own (OK on Tracking) ---
// The satellite's profile gives the frequency to track on; a pass can use another (an SSTV event on 145.800 MHz, a
// beacon). Typed digits as on the VFO, OK sets it: it is saved for the satellite (settings), listened on now, and OK on
// Tracking swaps between it and the satellite's own from then on. Doppler works on it as on the profile's, and it is
// receive only: the typed frequency is a downlink, so the VFO is made to refuse PTT.

static char fq_digits[UI_ENTRY_MAX + 1];
static uint8_t fq_len;

static void freq_enter(void)
{
	fq_len = 0;
	fq_digits[0] = '\0';
}

// The base the Doppler works on changed (OK swapped, or a frequency was typed): the receiver follows when it is tuned
// already, and the VFO's receive-only flag with it
static void track_alt_apply(void)
{
	const radio_view_t *v = ui_view();
	ui_sat_profile_t p;
	if (trk_tuned && v->vfo && ui_svc_sat_profile(&p) && track_base(&p) != 0) {
		trk_rx_only = track_listen_only(&p);
		radio_req_vfo_rx_only(trk_rx_only);
		radio_req_rx_hz(track_want(&p));
		sat_follow_retuned(&trk_follow, ui_st.now_ms);
	}
}

// What a swap says: the frequency now listened on
static void alt_toast(const ui_sat_profile_t *p)
{
	static char msg[40];
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	if (trk_alt_on) {
		sb_str(&b, TR("Listening on"));
		sb_chr(&b, ' ');
		sb_freq(&b, alt_hz);
		sb_str(&b, TR(": RX only"));
	} else {
		sb_str(&b, TR("The satellite's own frequency"));
		(void)p;
	}
	ui_toast(msg);
}

// OK: swap between the satellite's own downlink and the one saved for it; with none saved, type one
static void track_alt_swap(void)
{
	if (alt_hz == 0) {
		ui_push(SCR_SAT_FREQ, 0);
		return;
	}
	trk_alt_on = !trk_alt_on;
	track_alt_apply();
	ui_sat_profile_t p;
	if (!ui_svc_sat_profile(&p))
		memset(&p, 0, sizeof p);
	alt_toast(&p);
}

static bool freq_key(const input_event_t *ev)
{
	if (ev->gesture != GESTURE_PRESS)
		return false;
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9) {
		(void)ui_freq_entry_digit(fq_digits, &fq_len, (char)('0' + (ev->action - ACT_DIGIT_0)));
		return true;
	}
	if (ev->action == ACT_BACK && fq_len > 0) {	// takes a digit back; with none typed it leaves
		fq_digits[--fq_len] = '\0';
		return true;
	}
	if (ev->action == ACT_SELECT || ev->action == ACT_CONTEXT) {
		uint32_t hz = 0;
		if (fq_len != 0) {
			hz = ui_freq_entry_hz(fq_digits, fq_len);
			if (hz == 0)
				return true;	// its toast says why (too few digits, not a band)
		}
		ui_pop();
		if (hz == 0)
			return true;	// nothing typed: nothing changes (OK on Tracking swaps)
		// Saved for this satellite (one frequency in the settings: the last typed), and listened on now
		alt_hz = hz;
		(void)ui_svc_setting_set(UI_SET_SAT_ALT_NORAD, (int32_t)open_norad);
		(void)ui_svc_setting_set(UI_SET_SAT_ALT_HZ, (int32_t)hz);
		trk_alt_on = true;
		track_alt_apply();
		ui_sat_profile_t p;
		if (!ui_svc_sat_profile(&p))
			memset(&p, 0, sizeof p);
		alt_toast(&p);
		return true;
	}
	return false;
}

static void freq_draw(void)
{
	char buf[32];
	sbuf_t b;
	status();
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, open_name, TR("Frequency")) + 6;
	int w = gfx_width();
	const font_t *big = w >= 200 ? ui_layout.large : ui_layout.medium, *sm = ui_layout.small;
	// the digits so far, blanks for the rest, as the VFO's own entry
	sb_init(&b, buf, sizeof buf);
	int dec = fq_len > UI_ENTRY_MHZ ? fq_len - UI_ENTRY_MHZ : 0;
	int show = dec < 3 ? 3 : dec;
	for (int i = 0; i < UI_ENTRY_MHZ + show; i++) {
		if (i == UI_ENTRY_MHZ)
			sb_chr(&b, '.');
		sb_chr(&b, i < fq_len ? fq_digits[i] : '_');
	}
	int tw = gfx_text_width(big, buf);
	gfx_text(big, (w - tw) / 2, y, buf, fq_len != 0 ? ui_col.warn : ui_col.dim);
	y += big->h + 6;
	// The two frequencies first, then what the screen is for, as far as the panel has rows (the 128x64 one has the digits
	// and a line or two)
	const int hy = ui_layout.hint_y;
	ui_sat_profile_t p;
	if (ui_svc_sat_profile(&p) && p.down_hz != 0 && y + sm->h <= hy) {
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, TR("Its own"));
		sb_chr(&b, ' ');
		sb_freq(&b, p.down_hz);
		gfx_text(sm, 2, y, buf, trk_alt_on || alt_hz == 0 ? ui_col.dim : ui_col.rx);
		y += sm->h + 1;
	}
	if (alt_hz != 0 && y + sm->h <= hy) {
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, TR("Yours"));
		sb_chr(&b, ' ');
		sb_freq(&b, alt_hz);
		gfx_text(sm, 2, y, buf, trk_alt_on ? ui_col.rx : ui_col.dim);
		y += sm->h + 1;
	}
	y += 3;
	if (y + sm->h <= hy)
		gfx_text(sm, 2, y, TR("Downlink to listen on,"), ui_col.fg);
	y += sm->h + 1;
	if (y + sm->h <= hy)
		gfx_text(sm, 2, y, TR("before the Doppler shift"), ui_col.fg);
	y += sm->h + 4;
	if (y + sm->h <= hy)
		gfx_text(sm, 2, y, TR("Receive only"), ui_col.dim);
	ui_draw_bottom(&ui_st, fq_len == 0 ? TR(TEXT_ALT_OPEN "0-9 type  OK close  Back" TEXT_ALT_NEXT "0-9 type  Back" TEXT_ALT_CLOSE) :
		       TR(TEXT_ALT_OPEN "0-9 type  OK set  Back: delete" TEXT_ALT_NEXT "0-9  OK set  Back del" TEXT_ALT_CLOSE));
	ui_draw_banner(&ui_st);
}

const ui_scr_def_t ui_sat_freq_screen = {
	// Not a field-use screen (P4): a step inside Tracking
	.name = "sat-freq", .flags = 0, .draw = freq_draw, .key = freq_key, .enter = freq_enter,
};

const ui_scr_def_t ui_sat_track_screen = {
	// Not a field-use screen (P4): OK on a pass of the list, four levels from Home
	.name = "track", .flags = SF_HOLD_CONTEXT | SF_OWN_QUICK, .draw = track_draw, .key = track_key, .enter = track_enter, .leave = track_leave,
};
#if CONFIG_SAT_SOUND
// --- receive sound (the last row of the list; docs/spec/sound-transfer.md) ---
// Opening it asks the radio to listen at the microphone for satellites a phone plays; the radio stores each one it hears
// (a DMR channel is needed: the microphone is read in the modem's DMR work mode). This screen only asks, and shows what
// the radio says: whether it listens, the level, how many satellites of the transfer were heard and what became of them

static sat_sound_stats_t snd;
static radio_sound_state_t snd_state;
static uint32_t snd_ms = 1;

static void snd_read(void)
{
	if (snd_ms == ui_st.now_ms)
		return;
	snd_ms = ui_st.now_ms;
	snd_state = radio_sound_status(&snd);
}

static void sound_enter(void)
{
	snd_ms = 1;
	radio_req_sound(true);
}

static void sound_leave(void)
{
	radio_req_sound(false);
}

// A bar for the level: the number of bits in the loudest sample, out of 15, so a quiet phone and a loud one both show
static int level_bits(uint16_t v)
{
	int n = 0;
	while (v != 0) {
		n++;
		v >>= 1;
	}
	return n;
}

static void snd_line(int x, int *y, const font_t *f, const char *s, color_t c, int limit)
{
	if (*y + f->h <= limit)
		gfx_text(f, x, *y, s, c);
	*y += f->h + 1;
}

static void sound_draw(void)
{
	char buf[40];
	sbuf_t b;
	snd_read();
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, NULL, TR("Receive sound")) + 4;
	int w = gfx_width();
	const font_t *sm = ui_layout.small;
	const int hy = ui_layout.hint_y;
	if (snd_state != SOUND_LISTENING) {
		const char *l1 = TR("Starting"), *l2 = "";
		switch (snd_state) {
		case SOUND_NEED_DMR:
			l1 = TR("Switch to a DMR channel,");
			l2 = TR("then open this again");
			break;
		case SOUND_BUSY:
			l1 = TR("Transmitting, or still");
			l2 = TR("storing the last transfer");
			break;
		case SOUND_NO_STORE:
			l1 = TR("No place to keep");
			l2 = TR("satellites on this radio");
			break;
		case SOUND_NO_AUDIO:
			l1 = TR("This radio can't");
			l2 = TR("listen for sound");
			break;
		default:
			break;
		}
		snd_line(2, &y, sm, l1, snd_state == SOUND_OFF ? ui_col.dim : ui_col.warn, hy);
		snd_line(2, &y, sm, l2, ui_col.warn, hy);
		ui_draw_bottom(&ui_st, TR("Back close"));
		ui_draw_banner(&ui_st);
		return;
	}
	// the level, as a bar the width of the screen
	int bw = w - 4, fill = (level_bits(snd.level) * bw) / 15;
	if (y + sm->h <= hy) {
		gfx_rect(2, y, bw, sm->h, ui_col.line);
		if (fill > 0)
			gfx_fill(2, y, fill, sm->h, snd.level > 12000 ? ui_col.warn : ui_col.rx);
	}
	y += sm->h + 4;
	if (snd.count == 0) {
		// Before the first satellite, what the receiver sees: the loudest sample of the last block, frames decoded and frames
		// that failed their check (a bar that never moves means no sound reaches the microphone; frames with errors, a signal
		// that is too weak or too distorted)
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, TR("Lvl"));
		sb_chr(&b, ' ');
		sb_u32(&b, snd.level, 1);
		sb_chr(&b, ' ');
		sb_str(&b, TR("Fr"));
		sb_chr(&b, ' ');
		sb_u32(&b, snd.frames, 1);
		sb_chr(&b, ' ');
		sb_str(&b, TR("Err"));
		sb_chr(&b, ' ');
		sb_u32(&b, (uint32_t)snd.fcs_errors + snd.refused, 1);
		snd_line(2, &y, sm, buf, ui_col.dim, hy);
		snd_line(2, &y, sm, TR("Waiting for the sound"), ui_col.fg, hy);
		snd_line(2, &y, sm, TR("Hold the phone's speaker"), ui_col.dim, hy);
		snd_line(2, &y, sm, TR("at the microphone"), ui_col.dim, hy);
	} else {
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, TR("Sats heard"));
		sb_chr(&b, ' ');
		sb_u32(&b, snd.heard, 1);
		sb_chr(&b, ' ');
		sb_str(&b, TR("of"));
		sb_chr(&b, ' ');
		sb_u32(&b, snd.count, 1);
		bool done = snd.heard == snd.count && snd.queued == 0;
		snd_line(2, &y, sm, buf, done ? ui_col.rx : ui_col.fg, hy);
		uint16_t kept = (uint16_t)(snd.applied[SAT_SOUND_APPLY_CURRENT] + snd.applied[SAT_SOUND_APPLY_OLDER]);
		uint16_t bad = (uint16_t)(snd.applied[SAT_SOUND_APPLY_FULL] + snd.applied[SAT_SOUND_APPLY_MODEL] +
					  snd.applied[SAT_SOUND_APPLY_STORE]);
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, TR("New"));
		sb_chr(&b, ' ');
		sb_u32(&b, snd.applied[SAT_SOUND_APPLY_NEW], 1);
		sb_str(&b, "  ");
		sb_str(&b, TR("Updated"));
		sb_chr(&b, ' ');
		sb_u32(&b, snd.applied[SAT_SOUND_APPLY_UPDATED], 1);
		snd_line(2, &y, sm, buf, ui_col.fg, hy);
		sb_init(&b, buf, sizeof buf);
		sb_str(&b, TR("Kept"));
		sb_chr(&b, ' ');
		sb_u32(&b, kept, 1);
		sb_str(&b, "  ");
		sb_str(&b, TR("Problems"));
		sb_chr(&b, ' ');
		sb_u32(&b, bad, 1);
		snd_line(2, &y, sm, buf, bad != 0 ? ui_col.warn : ui_col.dim, hy);
		if (done)
			snd_line(2, &y, sm, TR("Done: all heard"), ui_col.rx, hy);
	}
	ui_draw_bottom(&ui_st, TR("Back stop"));
	ui_draw_banner(&ui_st);
}

const ui_scr_def_t ui_sat_sound_screen = {
	// Not a field-use screen (P4): a step beside the list
	.name = "sat-sound", .flags = 0, .draw = sound_draw, .enter = sound_enter, .leave = sound_leave,
};
#endif

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
