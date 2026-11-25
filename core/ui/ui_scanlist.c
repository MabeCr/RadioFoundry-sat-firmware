// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Scan lists (Tools > Scan with CONFIG_SCAN_LIST; docs/spec/ui.md 2.7, docs/spec/scan-list.md): the lists, one list's
// channels, adding channels from any zone, and a list's options (rename, delete). The lists live in the scan list area;
// every write goes through ui_svc_scanlist_put / _delete (a job for the housekeeping task), so what is open here is a
// copy that the screens change first and hand over whole
#include "config.h"
#if CONFIG_SCAN_LIST
#include <string.h>
#include "ui_nav.h"
#include "fmt.h"
#include "i18n.h"
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_modes.h"

// ======================= The lists (Tools > Scan) =======================
// Row 0 scans the zone (the band in the VFO) as Tools > Scan did before; then the stored lists; then "New scan list"

typedef struct {
	uint8_t slot;
	uint8_t count;
	bool vote;		// a vote list (docs/spec/vote-scan.md 2)
	char name[CODEPLUG_NAME_LEN + 1];
} row_t;

static row_t rows[SCAN_LIST_MAX];
static uint8_t nrows;
static uint32_t seen_gen = 0xFFFFFFFFu;
static char focus_name[CODEPLUG_NAME_LEN + 1];	// the list to focus once its write has landed ("" = none)

static scan_list_t cur;				// the list the editor and the options work on
#if CONFIG_VOTE_SCAN
// One scratch channel, one scratch list or zone for the vote scan code: the UI is one task and each use ends before the next
static channel_t vch;
static zone_t vzone;
#endif
static uint8_t cur_slot;
static char new_name[CODEPLUG_NAME_LEN + 1];	// a name being typed
static char msg[CODEPLUG_NAME_LEN + 24];		// the toasts
static uint16_t pick_zone;			// the zone the channel picker lists
#if CONFIG_VOTE_SCAN
static scan_list_vote_t cur_vote;		// the open list's vote options

// A channel's mode as a vote list sees it: a stored P25 or NXDN channel by its kind; -1 for a trunked channel (never a
// member) or one the codeplug no longer has
static int vote_mode(uint16_t num)
{
	if (!codeplug_channel(num, &vch) || codeplug_kind_trunked(vch.kind))
		return -1;
	return vch.kind == CODEPLUG_KIND_P25 ? RADIO_MODE_P25 : vch.kind == CODEPLUG_KIND_NXDN ? RADIO_MODE_NXDN : vch.mode;
}

// Why l can't vote (docs/spec/vote-scan.md 2), TR_NONE when it can: two channels or more the codeplug has, all of one
// mode, none trunked. *mode: theirs
static tr_id_t vote_refusal(const scan_list_t *l, int *mode)
{
	uint8_t found = 0;
	*mode = -1;
	for (uint8_t k = 0; k < l->count; k++) {
		if (!codeplug_channel(l->channels[k], &vch))
			continue;
		int m = vote_mode(l->channels[k]);
		if (m < 0)
			return TRN(SCANLIST_WHY_VOTE_TRUNKED, "Vote: no trunked channels");
		if (*mode >= 0 && m != *mode)
			return TRN(SCANLIST_WHY_VOTE_ONE_MODE, "Vote: one mode only");
		*mode = m;
		found++;
	}
	return found < 2 ? TRN(SCANLIST_WHY_NEEDS_TWO_CHANNELS, "Add two channels first") : TR_NONE;
}
#endif

// The stored lists, read again whenever the area changed (ui_svc_scanlist_generation)
static void refresh(void)
{
	static scan_list_t l;
	uint32_t g = ui_svc_scanlist_generation();
	if (g == seen_gen)
		return;
	seen_gen = g;
	nrows = 0;
	uint16_t slots = ui_svc_scanlist_slots();
	for (uint16_t s = 0; s < slots && nrows < SCAN_LIST_MAX; s++) {
		if (!ui_svc_scanlist_get(s, &l))
			continue;
		row_t *r = &rows[nrows++];
		r->slot = (uint8_t)s;
		r->count = l.count;
		str_copy(r->name, sizeof r->name, l.name);
#if CONFIG_VOTE_SCAN
		scan_list_vote_t v;
		r->vote = ui_svc_scanlist_get_vote(s, &v) && (v.flags & SCAN_VOTE_ON) != 0;
#else
		r->vote = false;
#endif
	}
	if (focus_name[0] && ui_frame()->scr == SCR_SLISTS) {
		for (uint8_t i = 0; i < nrows; i++)
			if (strcmp(rows[i].name, focus_name) == 0)
				ui_frame()->focus = (uint16_t)(1u + i);
		focus_name[0] = 0;
	}
}

static bool room(void)
{
	return nrows < ui_svc_scanlist_slots();
}

static uint16_t lists_count(void)
{
	refresh();
	return (uint16_t)(1u + nrows + (room() ? 1u : 0u));
}

static void toast2(const char *text, const char *name)
{
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	sb_str(&b, text);
	sb_str(&b, name);
	ui_toast(msg);
}

static void lists_item(uint16_t i, ui_item_t *it)
{
	refresh();
	sbuf_t b;
	if (i == 0) {
		str_copy(it->label, sizeof it->label, ui_view()->vfo ? TR(SCANLIST_ROW_SCAN_BAND, "Scan band") : TR(SCANLIST_ROW_SCAN_ZONE, "Scan zone"));
		sb_init(&b, it->value, sizeof it->value);
		sb_str(&b, ui_st.scanning ? TR(VALUE_ON, "On") : TR(VALUE_OFF, "Off"));
		return;
	}
	if (i > nrows) {
		str_copy(it->label, sizeof it->label, TR(SCANLIST_ROW_NEW, "New scan list"));
		return;
	}
	const row_t *r = &rows[i - 1u];
	str_copy(it->label, sizeof it->label, r->name);
	sb_init(&b, it->value, sizeof it->value);
	sb_u32(&b, r->count, 1);
	sb_chr(&b, ' ');
	sb_str(&b, TR(SCANLIST_UNIT_CH, "ch"));
	if (r->vote) {
		sb_str(&b, "  ");
		sb_str(&b, TR(SCANLIST_VALUE_VOTE, "Vote"));
	}
}

static void new_named(void)
{
	if (new_name[0] == 0)
		return;
	memset(&cur, 0, sizeof cur);
	str_copy(cur.name, sizeof cur.name, new_name);
	if (!ui_svc_scanlist_put(-1, &cur)) {
		ui_toast(TR(TOAST_BUSY_SAVING, "Busy saving: try again"));
		return;
	}
	str_copy(focus_name, sizeof focus_name, new_name);
	toast2(TR(ZONE_TOAST_ADDED, "Added "), new_name);
}

static void list_options(uint16_t i)
{
	if (i == 0 || i > nrows)
		return;
	ui_push(SCR_SLOPT, rows[i - 1u].slot);
}

static void lists_select(uint16_t i, bool hold)
{
	refresh();
	if (i == 0) {
		ui_go_home();
		ui_scan_toggle();	// says what scans; dual watch ends
		return;
	}
	if (i > nrows) {
		new_name[0] = 0;
		ui_text_open_done(new_name, CODEPLUG_NAME_LEN, new_named);
		return;
	}
	if (hold && !ui_has_action(ACT_CONTEXT)) {
		list_options(i);	// few-key boards: long OK opens what CONTEXT would
		return;
	}
	const row_t *r = &rows[i - 1u];
	if (ui_view()->vfo) {
		ui_toast(TR(SCANLIST_WHY_LEAVE_VFO, "Leave the VFO first"));
		return;
	}
	if (r->count < 2) {
		ui_toast(TR(SCANLIST_WHY_NEEDS_TWO_CHANNELS, "Add two channels first"));
		return;
	}
#if CONFIG_VOTE_SCAN
	if (r->vote) {
		int mode;
		tr_id_t why = ui_svc_scanlist_get(r->slot, &vzone) ? vote_refusal(&vzone, &mode) : TR_NONE;
		if (why != TR_NONE) {
			ui_toast(tr(why));	// the radio core would refuse it too (the channels changed since Vote was turned on)
			return;
		}
	}
#endif
	ui_scan_list(r->slot, r->name, r->vote);
}

static void lists_enter(void)
{
	seen_gen = 0xFFFFFFFFu;
	refresh();
	ui_frame()->focus = 0;
}

const ui_scr_def_t ui_scanlists_screen = {
	.name = "scan-lists", .flags = SF_LIST | SF_FIELD, .prefix = TRN(WORD_SCAN, "Scan"),
	.hint = TRN(SCANLIST_HINT_LISTS, "OK scan  # options  Back"),
	.count = lists_count, .item = lists_item, .select = lists_select, .context = list_options, .enter = lists_enter,
};

// ======================= A list's options (arg: its slot) =======================

enum { LO_EDIT, LO_RENAME, LO_DELETE, LO_VOTE, LO_THRESHOLD, LO_MARGIN, LO_PEEK, LO_TX, LO_LINK, LO_ALL };

static void opt_enter(void)
{
	cur_slot = (uint8_t)ui_frame()->arg;
	if (!ui_svc_scanlist_get(cur_slot, &cur))
		ui_pop();	// gone (deleted elsewhere)
#if CONFIG_VOTE_SCAN
	ui_svc_scanlist_get_vote(cur_slot, &cur_vote);
#endif
}

// Edit, rename, delete; with vote scan, Vote, and once it is on the vote's own rows (docs/spec/vote-scan.md 3)
static uint16_t opt_count(void)
{
#if CONFIG_VOTE_SCAN
	return (cur_vote.flags & SCAN_VOTE_ON) ? LO_ALL : LO_VOTE + 1u;
#else
	return LO_VOTE;
#endif
}

#if CONFIG_VOTE_SCAN
// The transmit choice: the voted site, or one member by its channel name
static void sb_vote_tx(sbuf_t *b)
{
	if (!(cur_vote.flags & SCAN_VOTE_FIXED_TX) || cur_vote.tx_member >= cur.count ||
	    !codeplug_channel(cur.channels[cur_vote.tx_member], &vch)) {
		sb_str(b, TR(SCANLIST_VALUE_VOTED_SITE, "Voted site"));
		return;
	}
	sb_str(b, vch.name);
}
#endif

static void opt_item(uint16_t i, ui_item_t *it)
{
	static const tr_id_t names[LO_ALL] = {
		TRN(SCANLIST_ROW_EDIT_CHANNELS, "Edit channels"), TRN(ZONE_ROW_RENAME, "Rename"), TRN(MSG_ROW_DELETE, "Delete"),
		TRN(SCANLIST_ROW_VOTE, "Vote"), TRN(SCANLIST_ROW_THRESHOLD, "Threshold"), TRN(SCANLIST_ROW_MARGIN, "Margin"),
		TRN(SCANLIST_ROW_PEEK, "Peek during calls"), TRN(SCANLIST_ROW_TRANSMIT, "Transmit"),
		TRN(SCANLIST_ROW_LINK, "Scan key"),
	};
	str_copy(it->label, sizeof it->label, tr(names[i]));
	item_num_set(it, (uint16_t)(i + 1u));
	if (i == LO_DELETE)
		it->flags |= UI_ITEM_WARN;
#if CONFIG_VOTE_SCAN
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	switch (i) {
	case LO_VOTE:
		sb_str(&b, (cur_vote.flags & SCAN_VOTE_ON) ? TR(VALUE_ON, "On") : TR(VALUE_OFF, "Off"));
		break;
	case LO_THRESHOLD:
		sb_i32(&b, scan_vote_threshold_of(&cur_vote));
		sb_str(&b, " dBm");
		break;
	case LO_MARGIN:
		sb_u32(&b, (uint32_t)scan_vote_margin_of(&cur_vote), 1);
		sb_str(&b, " dB");
		break;
	case LO_PEEK:
		sb_str(&b, (cur_vote.flags & SCAN_VOTE_NO_PEEK) ? TR(VALUE_OFF, "Off") : TR(VALUE_ON, "On"));
		break;
	case LO_TX:
		sb_vote_tx(&b);
		break;
	case LO_LINK: {
		if (cur_vote.link != 0 && codeplug_channel(cur_vote.link, &vch))
			sb_str(&b, vch.name);
		else
			sb_str(&b, TR(VALUE_NONE, "None"));
		break;
	}
	default:
		break;
	}
#endif
}

#if CONFIG_VOTE_SCAN
static bool vote_save(const scan_list_vote_t *v)
{
	if (!ui_svc_scanlist_put_vote(cur_slot, v)) {
		ui_toast(TR(TOAST_BUSY_SAVING, "Busy saving: try again"));
		return false;
	}
	cur_vote = *v;
	return true;
}

// Vote on (after the mode rule) or off. An FM list starts with peeks off: a peek is a hole in FM audio
static void vote_toggle(void)
{
	scan_list_vote_t v = cur_vote;
	if (v.flags & SCAN_VOTE_ON) {
		v.flags = (uint8_t)(v.flags & ~SCAN_VOTE_ON);
		if (vote_save(&v))
			ui_toast(TR(SCANLIST_TOAST_VOTE_OFF, "Vote off"));
		return;
	}
	int mode;
	tr_id_t why = vote_refusal(&cur, &mode);
	if (why != TR_NONE) {
		ui_toast(tr(why));
		return;
	}
	if (v.threshold_dbm == 0 && v.margin_db == 0 && mode == RADIO_MODE_FM)
		v.flags |= SCAN_VOTE_NO_PEEK;	// a list made a vote list for the first time
	v.flags |= SCAN_VOTE_ON;
	if (vote_save(&v))
		toast2(TR(SCANLIST_TOAST_VOTE_ON, "Vote on: "), cur.name);
}
#endif

static void renamed(void)
{
	if (new_name[0] == 0 || strcmp(new_name, cur.name) == 0)
		return;
	str_copy(cur.name, sizeof cur.name, new_name);
	if (!ui_svc_scanlist_put(cur_slot, &cur)) {
		ui_toast(TR(TOAST_BUSY_SAVING, "Busy saving: try again"));
		return;
	}
	toast2(TR(SCANLIST_TOAST_RENAMED, "Renamed "), cur.name);
}

static void opt_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i == LO_EDIT) {
		ui_push(SCR_SLEDIT, cur_slot);
	} else if (i == LO_RENAME) {
		str_copy(new_name, sizeof new_name, cur.name);
		ui_text_open_done(new_name, CODEPLUG_NAME_LEN, renamed);
	} else if (i == LO_DELETE) {
		ui_push(SCR_SLDEL, cur_slot);
#if CONFIG_VOTE_SCAN
	} else if (i == LO_VOTE) {
		vote_toggle();
	} else if (i == LO_PEEK) {
		scan_list_vote_t v = cur_vote;
		v.flags ^= SCAN_VOTE_NO_PEEK;
		vote_save(&v);
	} else if (i == LO_LINK) {
		pick_zone = ui_view()->zone_index;
		ui_push(SCR_SLLINK, cur_slot);
	} else {
		ui_push(SCR_SLVOTE, i);	// Threshold, Margin or Transmit: the values to pick from
#endif
	}
}

static const char *opt_title(void) { return cur.name; }

const ui_scr_def_t ui_scanlist_options_screen = {
	.name = "scan-list", .flags = SF_LIST | SF_NUMBERED, .prefix = TRN(SCANLIST_TITLE, "Scan list"),
#if CONFIG_VOTE_SCAN
	.hint = TRN(HINT_OK_PICK_BACK, "OK pick  Back"),
#else
	.hint = TRN(HINT_1_3_PICK_BACK, "1-3 pick  Back"),
#endif
	.count = opt_count, .item = opt_item, .select = opt_select, .title = opt_title, .enter = opt_enter,
};

#if CONFIG_VOTE_SCAN
// ======================= A vote option's values (arg: LO_THRESHOLD, LO_MARGIN or LO_TX) =======================
// Threshold: -50 to -120 dBm in 5 dB steps; Margin: 2 to 20 dB; Transmit: the voted site, then each member

static const uint8_t margins[] = { 2, 3, 4, 5, 6, 8, 10, 12, 15, 20 };
#define N_MARGINS	(sizeof margins / sizeof margins[0])
#define N_THRESHOLDS	15u	// -50 .. -120

static uint16_t vpick_count(void)
{
	uint16_t f = ui_frame()->arg;
	return f == LO_THRESHOLD ? N_THRESHOLDS : f == LO_MARGIN ? (uint16_t)N_MARGINS : (uint16_t)(1u + cur.count);
}

static int vpick_threshold(uint16_t i)
{
	return -50 - 5 * (int)i;
}

static void vpick_item(uint16_t i, ui_item_t *it)
{
	uint16_t f = ui_frame()->arg;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	bool cur_one;
	if (f == LO_THRESHOLD) {
		sb_i32(&b, vpick_threshold(i));
		sb_str(&b, " dBm");
		cur_one = vpick_threshold(i) == scan_vote_threshold_of(&cur_vote);
	} else if (f == LO_MARGIN) {
		sb_u32(&b, margins[i], 1);
		sb_str(&b, " dB");
		cur_one = margins[i] == scan_vote_margin_of(&cur_vote);
	} else if (i == 0) {
		sb_str(&b, TR(SCANLIST_VALUE_VOTED_SITE, "Voted site"));
		cur_one = !(cur_vote.flags & SCAN_VOTE_FIXED_TX);
	} else {
		sb_u32(&b, i, 1);
		sb_chr(&b, ' ');
		sb_str(&b, codeplug_channel(cur.channels[i - 1u], &vch) ? vch.name : "?");
		cur_one = (cur_vote.flags & SCAN_VOTE_FIXED_TX) && cur_vote.tx_member == i - 1u;
	}
	if (cur_one)
		it->flags |= UI_ITEM_TEMP;
}

static void vpick_enter(void)
{
	uint16_t n = vpick_count();
	ui_frame()->focus = 0;
	for (uint16_t i = 0; i < n; i++) {
		ui_item_t it;
		memset(&it, 0, sizeof it);
		vpick_item(i, &it);
		if (it.flags & UI_ITEM_TEMP)
			ui_frame()->focus = i;
	}
}

static void vpick_select(uint16_t i, bool hold)
{
	(void)hold;
	uint16_t f = ui_frame()->arg;
	scan_list_vote_t v = cur_vote;
	if (f == LO_THRESHOLD) {
		v.threshold_dbm = (int8_t)vpick_threshold(i);
	} else if (f == LO_MARGIN) {
		v.margin_db = margins[i];
	} else if (i == 0) {
		v.flags = (uint8_t)(v.flags & ~SCAN_VOTE_FIXED_TX);
		v.tx_member = 0;
	} else {
		v.flags |= SCAN_VOTE_FIXED_TX;
		v.tx_member = (uint8_t)(i - 1u);
	}
	if (vote_save(&v))
		ui_pop();
}

static const char *vpick_prefix(void) { return cur.name; }

const ui_scr_def_t ui_scanlist_vote_screen = {
	.name = "scan-list-vote", .flags = SF_LIST, .prefix = TRN(SCANLIST_ROW_VOTE, "Vote"), .hint = TRN(HINT_OK_PICK_BACK, "OK pick  Back"),
	.count = vpick_count, .item = vpick_item, .select = vpick_select, .title = vpick_prefix, .enter = vpick_enter,
};
#endif

// ======================= Delete a list (arg: its slot) =======================

static uint16_t del_count(void) { return 2; }

static void del_item(uint16_t i, ui_item_t *it)
{
	str_copy(it->label, sizeof it->label, i == 0 ? TR(ZONE_ROW_KEEP, "No, keep it") : TR(ZONE_ROW_CONFIRM_DELETE, "Yes, delete"));
	if (i == 1)
		it->flags |= UI_ITEM_WARN;
}

static void del_select(uint16_t i, bool hold)
{
	(void)hold;
	ui_pop();
	if (i == 0)
		return;
	if (!ui_svc_scanlist_delete(cur_slot)) {
		ui_toast(TR(TOAST_BUSY_SAVING, "Busy saving: try again"));
		return;
	}
	ui_pop();	// the options are of nothing now
	toast2(TR(ZONE_TOAST_DELETED, "Deleted "), cur.name);
}

static const char *del_body(void)
{
	return TR(SCANLIST_BODY_DELETE_ASK, "Delete this scan list? Its channels stay in their zones.");
}

const ui_scr_def_t ui_scanlist_delete_screen = {
	.name = "scan-list-delete", .flags = SF_LIST, .prefix = TRN(MSG_ROW_DELETE, "Delete"), .hint = TRN(HINT_OK_CHOOSE_BACK_CANCELS, "OK choose  Back cancel"),
	.count = del_count, .item = del_item, .select = del_select, .title = opt_title, .body = del_body,
};

// ======================= A list's channels (arg: its slot) =======================
// Raw row 0 adds a channel; then the list's channels, OK removes one

static void edit_enter(void)
{
	cur_slot = (uint8_t)ui_frame()->arg;
	if (!ui_svc_scanlist_get(cur_slot, &cur))
		ui_pop();
	ui_frame()->focus = 0;
}

static uint16_t edit_count(void) { return (uint16_t)(1u + cur.count); }

static void edit_item(uint16_t i, ui_item_t *it)
{
	static channel_t ch;
	sbuf_t b;
	if (i == 0) {
		str_copy(it->label, sizeof it->label, TR(ZONE_ROW_ADD_CHANNEL, "Add channel"));
		return;
	}
	bool known = codeplug_channel(cur.channels[i - 1u], &ch);	// a channel deleted from the codeplug since
	sb_init(&b, it->label, sizeof it->label);
	sb_u32(&b, i, 2);
	sb_chr(&b, ' ');
	sb_str(&b, known ? ch.name : "?");
	sb_init(&b, it->value, sizeof it->value);
	if (known) {
		if (!sb_channel_row(&b, cur.channels[i - 1u], &ch))
			it->flags |= UI_ITEM_DIM;	// a trunked channel whose system isn't set up
	} else {
		sb_str(&b, TR(SCANLIST_VALUE_MISSING, "Missing"));
		it->flags |= UI_ITEM_DIM;
	}
}

static void edit_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i == 0) {
		if (cur.count >= SCAN_LIST_MAX) {
			ui_toast(TR(SCANLIST_WHY_FULL, "The list is full"));
			return;
		}
		pick_zone = ui_view()->zone_index;
		ui_push(SCR_SLPICK, cur_slot);
		return;
	}
	uint16_t pos = (uint16_t)(i - 1u);
	scan_list_t next = cur;
	memmove(&next.channels[pos], &next.channels[pos + 1u], (size_t)(next.count - pos - 1u) * sizeof next.channels[0]);
	next.count--;
	if (!ui_svc_scanlist_put(cur_slot, &next)) {
		ui_toast(TR(TOAST_BUSY_SAVING, "Busy saving: try again"));
		return;
	}
	cur = next;
	if (ui_frame()->focus >= edit_count())
		ui_frame()->focus = (uint16_t)(edit_count() - 1u);
	ui_toast(TR(SCANLIST_TOAST_REMOVED, "Removed"));
}

const ui_scr_def_t ui_scanlist_edit_screen = {
	.name = "scan-list-edit", .flags = SF_LIST, .prefix = TRN(TITLE_CHANNELS, "Channels"), .hint = TRN(SCANLIST_HINT_CHANNELS, "OK remove  Back"),
	.count = edit_count, .item = edit_item, .select = edit_select, .title = opt_title, .enter = edit_enter,
};

// ======================= Add channels from any zone =======================
// Raw row 0 is the zone (OK: another zone), then the zone's channels; OK adds one to the list or takes it out again, and
// the screen stays, so a list takes channels from several zones one after the other. Digits filter by name

static int in_list(uint16_t num)
{
	for (uint8_t k = 0; k < cur.count; k++)
		if (cur.channels[k] == num)
			return k;
	return -1;
}

static uint16_t pick_raw_count(void)
{
	static zone_t z;
	return (uint16_t)(1u + (codeplug_zone(pick_zone, &z) ? z.count : 0u));
}

static bool pick_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	static zone_t z;
	static channel_t ch;
	sbuf_t b;
	sb_init(&b, buf, len);
	if (raw == 0) {
		sb_str(&b, TR(WORD_ZONE, "Zone"));
		return true;
	}
	if (!codeplug_zone(pick_zone, &z) || raw - 1u >= z.count || !codeplug_channel(z.channels[raw - 1u], &ch))
		return false;
	sb_u32(&b, z.channels[raw - 1u], 1);
	sb_chr(&b, ' ');
	sb_str(&b, ch.name);
	return true;
}

static void pick_enter(void)
{
	ui_filter_reset();
	ui_frame()->focus = ui_filter_count() > 1 ? 1 : 0;
}

static void pick_item(uint16_t i, ui_item_t *it)
{
	static zone_t z;
	uint16_t raw = ui_filter_raw(i);
	sbuf_t b;
	if (!pick_raw_label(raw, it->label, sizeof it->label))
		return;
	sb_init(&b, it->value, sizeof it->value);
	if (raw == 0) {
		if (codeplug_zone(pick_zone, &z))
			sb_str(&b, z.name);
		return;
	}
	if (codeplug_zone(pick_zone, &z) && in_list(z.channels[raw - 1u]) >= 0) {
		sb_str(&b, TR(SCANLIST_VALUE_IN_LIST, "In list"));
		it->flags |= UI_ITEM_TEMP;
	}
}

static void pick_select(uint16_t i, bool hold)
{
	static zone_t z;
	(void)hold;
	uint16_t raw = ui_filter_raw(i);
	if (raw == 0) {
		ui_push(SCR_SLZONE, 0);
		return;
	}
	if (!codeplug_zone(pick_zone, &z) || raw - 1u >= z.count)
		return;
	uint16_t num = z.channels[raw - 1u];
	scan_list_t next = cur;
	int at = in_list(num);
#if CONFIG_VOTE_SCAN
	if (at < 0 && (cur_vote.flags & SCAN_VOTE_ON)) {
		int mode;
		next.channels[next.count] = num;
		next.count++;
		tr_id_t why = vote_refusal(&next, &mode);
		next = cur;
		if (why != TR_NONE && why != TRN(SCANLIST_WHY_NEEDS_TWO_CHANNELS, "Add two channels first")) {
			ui_toast(tr(why));	// a vote list takes channels of its own mode only
			return;
		}
	}
#endif
	if (at >= 0) {
		memmove(&next.channels[at], &next.channels[at + 1], (size_t)(next.count - (uint8_t)at - 1u) * sizeof next.channels[0]);
		next.count--;
	} else if (next.count >= SCAN_LIST_MAX) {
		ui_toast(TR(SCANLIST_WHY_FULL, "The list is full"));
		return;
	} else {
		next.channels[next.count++] = num;
	}
	if (!ui_svc_scanlist_put(cur_slot, &next)) {
		ui_toast(TR(TOAST_BUSY_SAVING, "Busy saving: try again"));
		return;
	}
	cur = next;
}

static const char *pick_title(void) { return cur.name; }

const ui_scr_def_t ui_scanlist_pick_screen = {
	.name = "scan-list-add", .flags = SF_LIST | SF_FILTER, .prefix = TRN(SCANLIST_TITLE_ADD_TO, "Add to"),
	.hint = TRN(ZONE_HINT_ADD_CHANNEL, TEXT_ALT_OPEN "OK add  0-9 find  Back" TEXT_ALT_NEXT "OK add  Back" TEXT_ALT_CLOSE),
	.count = ui_filter_count, .item = pick_item, .select = pick_select, .title = pick_title, .enter = pick_enter,
	.filter = &(const ui_filter_def_t){ .raw_count = pick_raw_count, .raw_label = pick_raw_label },
};

// ======================= The zone for the picker =======================

static uint16_t zone_count(void) { return codeplug_zone_count(); }

static void zone_item(uint16_t i, ui_item_t *it)
{
	static zone_t z;
	if (!codeplug_zone(i, &z))
		return;
	str_copy(it->label, sizeof it->label, z.name);
	if (i == pick_zone)
		it->flags |= UI_ITEM_TEMP;
}

static void zone_enter(void)
{
	ui_frame()->focus = pick_zone;
}

static void zone_select(uint16_t i, bool hold)
{
	(void)hold;
	pick_zone = i;
	ui_pop();	// back to the picker, now on that zone
	ui_filter_reset();
	ui_frame()->focus = ui_filter_count() > 1 ? 1 : 0;
	ui_frame()->top = 0;
}

const ui_scr_def_t ui_scanlist_zone_screen = {
	.name = "scan-list-zone", .flags = SF_LIST | SF_FIELD, .prefix = TRN(WORD_ZONE, "Zone"), .hint = TRN(HINT_OK_PICK_BACK, "OK pick  Back"),
	.count = zone_count, .item = zone_item, .select = zone_select, .enter = zone_enter,
};

#if CONFIG_VOTE_SCAN
// ======================= The channel whose Scan key starts the list (arg: its slot) =======================
// The same zone and channel picker as Add to, for any channel: a member or not. OK links it, OK on the linked one takes
// the link off; a channel another list is linked to says which (one list to a channel, docs/spec/vote-scan.md 2)

#define LINK_SLOTS	32	// the most slots a scan list area has (two erase units of 16)
static uint16_t link_of[LINK_SLOTS];	// each slot's linked channel, as the picker opened

static void link_enter(void)
{
	ui_filter_reset();
	ui_frame()->focus = ui_filter_count() > 1 ? 1 : 0;
	for (uint16_t k = 0; k < LINK_SLOTS; k++) {
		scan_list_vote_t v;
		link_of[k] = k != cur_slot && ui_svc_scanlist_get_vote(k, &v) ? v.link : 0;
	}
}

static int link_other(uint16_t num)
{
	for (uint16_t k = 0; num != 0 && k < LINK_SLOTS; k++)
		if (link_of[k] == num)
			return (int)k;
	return -1;
}

static void link_item(uint16_t i, ui_item_t *it)
{
	uint16_t raw = ui_filter_raw(i);
	sbuf_t b;
	if (!pick_raw_label(raw, it->label, sizeof it->label))
		return;
	sb_init(&b, it->value, sizeof it->value);
	if (raw == 0) {
		if (codeplug_zone(pick_zone, &vzone))
			sb_str(&b, vzone.name);
		return;
	}
	if (!codeplug_zone(pick_zone, &vzone))
		return;
	uint16_t num = vzone.channels[raw - 1u];
	if (num == cur_vote.link) {
		sb_str(&b, TR(SCANLIST_VALUE_LINKED, "Linked"));
		it->flags |= UI_ITEM_TEMP;
	} else if (link_other(num) >= 0) {
		sb_str(&b, TR(SCANLIST_VALUE_LINKED_ELSEWHERE, "Other list"));
	}
}

static void link_select(uint16_t i, bool hold)
{
	(void)hold;
	uint16_t raw = ui_filter_raw(i);
	if (raw == 0) {
		ui_push(SCR_SLZONE, 0);
		return;
	}
	if (!codeplug_zone(pick_zone, &vzone) || raw - 1u >= vzone.count)
		return;
	uint16_t num = vzone.channels[raw - 1u];
	scan_list_vote_t v = cur_vote;
	int at = link_other(num);
	if (at >= 0) {
		if (ui_svc_scanlist_get((uint16_t)at, &vzone))
			toast2(TR(SCANLIST_TOAST_LINK_USED_BY, "Used by "), vzone.name);
		return;
	}
	v.link = num == cur_vote.link ? 0 : num;
	if (!vote_save(&v))
		return;
	if (v.link != 0 && codeplug_channel(num, &vch))
		toast2(TR(SCANLIST_TOAST_LINKED_TO, "Scan key: "), vch.name);
	else
		ui_toast(TR(SCANLIST_TOAST_LINK_OFF, "Scan key link removed"));
	ui_pop();
}

const ui_scr_def_t ui_scanlist_link_screen = {
	.name = "scan-list-link", .flags = SF_LIST | SF_FILTER, .prefix = TRN(SCANLIST_TITLE_LINK, "Scan key"),
	.hint = TRN(SCANLIST_HINT_LINK, TEXT_ALT_OPEN "OK link  0-9 find  Back" TEXT_ALT_NEXT "OK link  Back" TEXT_ALT_CLOSE),
	.count = ui_filter_count, .item = link_item, .select = link_select, .title = pick_title, .enter = link_enter,
	.filter = &(const ui_filter_def_t){ .raw_count = pick_raw_count, .raw_label = pick_raw_label },
};

static int linked_slot;		// ui_scan_link_here's answer
static uint16_t linked_chan;	// ... for this channel,
static uint32_t linked_gen = UINT32_MAX;	// ... as of this list change
static bool linked_vfo;		// ... in the VFO or not

static const char *linked_refusal(uint16_t slot, scan_list_t *l)
{
	int mode;
	tr_id_t why = ui_svc_scanlist_get(slot, l) ? vote_refusal(l, &mode) : TR_NONE;
	return why != TR_NONE ? tr(why) : NULL;
}

bool ui_scan_link_here(const radio_view_t *v)
{
	uint32_t gen = ui_svc_scanlist_generation();
	if (v->channel_number != linked_chan || gen != linked_gen || v->vfo != linked_vfo) {
		linked_chan = v->channel_number;
		linked_gen = gen;
		linked_vfo = v->vfo;
		scan_list_vote_t vt;
		linked_slot = !v->vfo && ui_svc_scanlist_slots() > 0 ? ui_svc_scanlist_find_link(v->channel_number, -1) : -1;
		if (linked_slot >= 0 && !(ui_svc_scanlist_get_vote((uint16_t)linked_slot, &vt) && (vt.flags & SCAN_VOTE_ON)))
			linked_slot = -1;	// a list with Vote off keeps its link, and does nothing with it
	}
	return linked_slot >= 0;
}

bool ui_scan_linked_start(const radio_view_t *v)
{
	if (!ui_scan_link_here(v))
		return false;
	const char *why = linked_refusal((uint16_t)linked_slot, &vzone);
	if (why != NULL) {
		ui_toast(why);	// the channels changed since Vote was turned on
		return true;
	}
	ui_scan_list((uint8_t)linked_slot, vzone.name, true);
	return true;
}
#endif

#if CONFIG_VOTE_SCAN
static char voting[CODEPLUG_NAME_LEN + 1];	// the vote list started last, for Home

const char *ui_scan_list_voting(void)
{
	return voting;
}
#endif

// Start the list in slot: the zone scan's place on the Home screen, with the name in the toast
void ui_scan_list(uint8_t slot, const char *name, bool vote)
{
#if CONFIG_DUAL_WATCH
	if (ui_st.dual.on)
		ui_dual_stop();
#endif
	if (!ui_svc_scanlist_start(slot)) {
		ui_toast(TR(WHY_SCAN_NOT_IN_BUILD, "Scan: not in this build"));
		return;
	}
	ui_st.scanning = true;
	ui_go_home();
#if CONFIG_VOTE_SCAN
	if (vote) {
		str_copy(voting, sizeof voting, name);
		toast2(TR(SCANLIST_TOAST_VOTING, "Voting "), name);
		return;
	}
#endif
	(void)vote;
	toast2(TR(SCANLIST_TOAST_SCANNING, "Scanning "), name);
}
#endif
