#include "fwupdate.h"

#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/device/firmware.h"
#include "src/system/device/ota.h"
#include "src/system/device/power.h"
#include "src/system/device/sysinfo.h"

#define CARD_W 420
#define BUTTON_H 64
#define BUTTONS 3
#define POLL_MS 200

// The title while a release is shown, downloaded and installed; the version
// is in the line under it.
#define PRODUCT_NAME "Sonix Player"

// The notes scroll inside the card past this share of the screen height.
#define NOTES_MAX_PERCENT 40

// Between "Installing..." being drawn and the screen fading out.
#define INSTALL_DELAY_MS 600

typedef void (*action_fn)(void);

static lv_obj_t *veil;
static lv_obj_t *card;
static lv_obj_t *title_label;
static lv_obj_t *message_label;
static lv_obj_t *notes_box;
static lv_obj_t *notes_content;
static int32_t notes_max_h;
static lv_obj_t *bar;
static lv_obj_t *buttons[BUTTONS];
static lv_obj_t *button_labels[BUTTONS];
static action_fn button_actions[BUTTONS];

static lv_timer_t *poll_timer;
static bool holding_screen;

static enum { PHASE_NONE, PHASE_CHECK, PHASE_DOWNLOAD } phase;

static char notes_line[4096];

// ---------------------------------------------------------------------------
// the card
// ---------------------------------------------------------------------------

static void button_cb(lv_event_t *e) {
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i >= 0 && i < BUTTONS && button_actions[i]) {
		button_actions[i]();
	}
}

// Taps on the dimmed page are swallowed: every state has its own way out.
static void veil_cb(lv_event_t *e) { (void)e; }

static void build(void) {
	int32_t screen_w = lv_display_get_horizontal_resolution(lv_display_get_default());
	int32_t screen_h = lv_display_get_vertical_resolution(lv_display_get_default());

	veil = lv_obj_create(lv_layer_top());
	lv_obj_set_size(veil, lv_pct(100), lv_pct(100));
	lv_obj_set_pos(veil, 0, 0);
	lv_obj_set_style_bg_color(veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(veil, LV_OPA_60, 0);
	lv_obj_set_style_border_width(veil, 0, 0);
	lv_obj_set_style_radius(veil, 0, 0);
	lv_obj_set_style_pad_all(veil, 0, 0);
	lv_obj_set_scrollable(veil, false);
	lv_obj_set_hidden(veil, true);
	lv_obj_add_event_cb(veil, veil_cb, LV_EVENT_CLICKED, NULL);

	card = lv_obj_create(veil);
	lv_obj_set_width(card, CARD_W < screen_w - 40 ? CARD_W : screen_w - 40);
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_center(card);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 18, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 22, 0);
	lv_obj_set_style_pad_gap(card, 14, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	title_label = lv_label_create(card);
	lv_label_set_long_mode(title_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(title_label, lv_pct(100));
	lv_obj_add_style(title_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(title_label, &font_ui_26, 0);
	lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);

	message_label = lv_label_create(card);
	lv_label_set_long_mode(message_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(message_label, lv_pct(100));
	lv_obj_add_style(message_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(message_label, &font_ui_22, 0);
	lv_obj_set_style_text_align(message_label, LV_TEXT_ALIGN_CENTER, 0);

	// A fixed height, set in fit_notes(): with LV_SIZE_CONTENT the box is
	// measured with its scroll offset, and dragging past the end of the notes
	// shrinks it and the card with it.
	notes_max_h = screen_h * NOTES_MAX_PERCENT / 100;
	notes_box = lv_obj_create(card);
	lv_obj_set_width(notes_box, lv_pct(100));
	lv_obj_set_height(notes_box, notes_max_h);
	lv_obj_set_scroll_elastic(notes_box, false);
	lv_obj_set_style_bg_opa(notes_box, 0, 0);
	lv_obj_set_style_border_width(notes_box, 0, 0);
	lv_obj_set_style_pad_all(notes_box, 0, 0);
	lv_obj_set_scroll_dir(notes_box, LV_DIR_VER);

	notes_content = lv_obj_create(notes_box);
	lv_obj_set_width(notes_content, lv_pct(100));
	lv_obj_set_height(notes_content, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(notes_content, 0, 0);
	lv_obj_set_style_border_width(notes_content, 0, 0);
	lv_obj_set_style_pad_all(notes_content, 0, 0);
	lv_obj_set_style_pad_row(notes_content, 2, 0);
	lv_obj_set_clickable(notes_content, false);
	lv_obj_set_scrollable(notes_content, false);
	lv_obj_set_flex_flow(notes_content, LV_FLEX_FLOW_COLUMN);

	bar = lv_bar_create(card);
	lv_obj_set_width(bar, lv_pct(100));
	lv_obj_set_height(bar, 12);
	lv_bar_set_range(bar, 0, 1000);
	lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(bar, LV_OPA_40, LV_PART_MAIN);
	lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

	lv_obj_t *column = lv_obj_create(card);
	lv_obj_set_width(column, lv_pct(100));
	lv_obj_set_height(column, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(column, 0, 0);
	lv_obj_set_style_border_width(column, 0, 0);
	lv_obj_set_style_pad_all(column, 0, 0);
	lv_obj_set_style_pad_gap(column, 12, 0);
	lv_obj_set_scrollable(column, false);
	lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);

	for (int i = 0; i < BUTTONS; i++) {
		lv_obj_t *btn = lv_btn_create(column);
		lv_obj_set_width(btn, lv_pct(100));
		lv_obj_set_height(btn, BUTTON_H);
		lv_obj_set_style_radius(btn, 12, 0);
		lv_obj_set_style_border_width(btn, 0, 0);
		lv_obj_set_style_shadow_width(btn, 0, 0);
		lv_obj_add_event_cb(btn, button_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		lv_obj_t *label = lv_label_create(btn);
		lv_obj_set_style_text_font(label, &font_ui_24, 0);
		lv_obj_center(label);

		buttons[i] = btn;
		button_labels[i] = label;
	}
}

// Button `i` with the text of `key`, the accent fill or the neutral one.
static void set_button(int i, const char *key, bool accent, action_fn action) {
	lv_obj_t *btn = buttons[i];
	lv_obj_remove_style(btn, &theme_style_switch, 0);
	lv_obj_remove_local_style_prop(btn, LV_STYLE_BG_COLOR, 0);
	lv_obj_remove_local_style_prop(btn, LV_STYLE_BG_OPA, 0);
	if (accent) {
		lv_obj_set_style_bg_color(btn, theme()->accent, 0);
		lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
	} else {
		lv_obj_add_style(btn, &theme_style_switch, 0);
	}

	lv_label_set_text(button_labels[i], tr(key));
	lv_obj_set_style_text_color(button_labels[i], accent ? lv_color_white() : theme()->text_primary, 0);
	button_actions[i] = action;
	lv_obj_set_hidden(btn, false);
}

// Hides button `from` and every one after it.
static void hide_buttons(int from) {
	for (int i = from; i < BUTTONS; i++) {
		lv_obj_set_hidden(buttons[i], true);
		button_actions[i] = NULL;
	}
}

static void set_visible(lv_obj_t *obj, bool visible) {
	if (visible) {
		lv_obj_set_hidden(obj, false);
	} else {
		lv_obj_set_hidden(obj, true);
	}
}

// Title and message are plain text, already translated; a NULL message hides
// the line.
static void layout(const char *title, const char *message, bool notes, bool progress) {
	lv_label_set_text(title_label, title ? title : "");
	lv_label_set_text(message_label, message ? message : "");
	set_visible(message_label, message && message[0]);
	set_visible(notes_box, notes);
	set_visible(bar, progress);
	lv_obj_set_style_bg_color(bar, theme()->text_secondary, LV_PART_MAIN);
	lv_obj_set_style_bg_color(bar, theme()->accent, LV_PART_INDICATOR);
}

static void stop_polling(void) {
	if (poll_timer) {
		lv_timer_pause(poll_timer);
	}
}

static void close_card(void) {
	stop_polling();
	phase = PHASE_NONE;
	if (veil) {
		lv_obj_set_hidden(veil, true);
	}
	if (holding_screen) {
		holding_screen = false;
		power_hold_screen_on(false);
	}
}

// ---------------------------------------------------------------------------
// release notes
// ---------------------------------------------------------------------------

// Marks where bold text starts or stops in the output of inline_text().
#define BOLD_MARK '\x01'

// A line of the Markdown GitHub keeps, with its inline marks resolved: "**"
// and "__" become BOLD_MARK, links keep their text, images go, and code marks
// go. A pair left unclosed is dropped rather than making the rest bold.
static void inline_text(const char *p, const char *end, char *out, size_t size) {
	size_t o = 0;
	size_t last_mark = 0;
	int marks = 0;
	while (p < end && o + 1 < size) {
		if (end - p >= 2 && ((p[0] == '*' && p[1] == '*') || (p[0] == '_' && p[1] == '_'))) {
			last_mark = o;
			marks++;
			out[o++] = BOLD_MARK;
			p += 2;
			continue;
		}
		if (*p == '`') {
			p++;
			continue;
		}
		if (*p == '[' || (*p == '!' && end - p >= 2 && p[1] == '[')) {
			const char *open = *p == '!' ? p + 1 : p;
			const char *close = memchr(open, ']', (size_t)(end - open));
			const char *stop = close && close + 1 < end && close[1] == '(' ? memchr(close, ')', (size_t)(end - close))
																		: NULL;
			if (stop) {
				if (*p == '[') {
					for (const char *c = open + 1; c < close && o + 1 < size; c++) {
						out[o++] = *c;
					}
				}
				p = stop + 1;
				continue;
			}
		}
		out[o++] = *p++;
	}
	if (marks % 2) {
		memmove(out + last_mark, out + last_mark + 1, o - last_mark - 1);
		o--;
	}
	while (o > 0 && out[o - 1] == ' ') {
		o--;
	}
	out[o] = '\0';
}

// Removes the bold marks from `text`, for a heading that is bold throughout.
static void strip_marks(char *text) {
	char *w = text;
	for (const char *r = text; *r; r++) {
		if (*r != BOLD_MARK) {
			*w++ = *r;
		}
	}
	*w = '\0';
}

// Space above a heading, and above whatever follows a blank line.
#define NOTES_GAP 10
#define BULLET_INDENT 18

// A paragraph in font_ui_20, the stretches between bold marks in
// font_ui_20_bold, wrapping at the width it is given.
static lv_obj_t *notes_rich(lv_obj_t *parent, const char *text) {
	lv_obj_t *group = lv_spangroup_create(parent);
	lv_obj_set_clickable(group, false);
	// The width of the notes and the height of the content: the lines wrap.
	// A bullet narrows it to what is left beside the bullet.
	lv_obj_set_size(group, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(group, &theme_style_text, 0);
	lv_obj_set_style_text_font(group, &font_ui_20, 0);

	bool bold = false;
	const char *p = text;
	while (*p) {
		const char *stop = strchr(p, BOLD_MARK);
		size_t len = stop ? (size_t)(stop - p) : strlen(p);
		if (len > 0) {
			char piece[sizeof(notes_line)];
			memcpy(piece, p, len);
			piece[len] = '\0';
			lv_span_t *span = lv_spangroup_add_span(group);
			lv_span_set_text(span, piece);
			if (bold) {
				lv_style_set_text_font(lv_span_get_style(span), &font_ui_20_bold);
			}
		}
		if (!stop) {
			break;
		}
		bold = !bold;
		p = stop + 1;
	}
	lv_spangroup_refresh(group);
	return group;
}

static lv_obj_t *notes_text_label(lv_obj_t *parent, const char *text, const lv_font_t *font) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_label_set_text(label, text);
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
	return label;
}

// "- item": a bullet, and the text beside it wrapping under itself rather
// than under the bullet.
static lv_obj_t *notes_bullet(const char *text, int depth) {
	lv_obj_t *row = lv_obj_create(notes_content);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_left(row, depth * BULLET_INDENT, 0);
	lv_obj_set_style_pad_column(row, 8, 0);
	lv_obj_set_clickable(row, false);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

	notes_text_label(row, "\xE2\x80\xA2", &font_ui_20); // U+2022
	lv_obj_t *body = notes_rich(row, text);
	lv_obj_set_width(body, 1);
	lv_obj_set_flex_grow(body, 1);
	return row;
}

// Fills the notes: "#" lines as bold headings, "**bold**" within a line, "-",
// "*" and "+" items as bullets (indented by their leading spaces, two per
// level), anything else as plain paragraphs. HTML comments are dropped and
// blank lines become a gap.
static void render_notes(const char *md) {
	lv_obj_clean(notes_content);

	bool gap = false;
	bool any = false;
	const char *p = md;
	while (*p) {
		const char *eol = strchr(p, '\n');
		const char *end = eol ? eol : p + strlen(p);
		const char *next = eol ? eol + 1 : end;

		if (strncmp(p, "<!--", 4) == 0) {
			const char *close = strstr(p, "-->");
			p = close ? close + 3 : p + strlen(p);
			continue;
		}

		int spaces = 0;
		while (p < end && (*p == ' ' || *p == '\t')) {
			spaces += *p == '\t' ? 2 : 1;
			p++;
		}
		while (end > p && (end[-1] == '\r' || end[-1] == ' ')) {
			end--;
		}
		if (p == end) {
			gap = any;
			p = next;
			continue;
		}

		lv_obj_t *item;
		if (*p == '#') {
			while (p < end && *p == '#') {
				p++;
			}
			while (p < end && *p == ' ') {
				p++;
			}
			inline_text(p, end, notes_line, sizeof(notes_line));
			strip_marks(notes_line);
			item = notes_text_label(notes_content, notes_line, &font_ui_22_bold);
			lv_obj_set_width(item, lv_pct(100));
			gap = any;
		} else if (end - p >= 2 && (*p == '-' || *p == '*' || *p == '+') && p[1] == ' ') {
			inline_text(p + 2, end, notes_line, sizeof(notes_line));
			int depth = spaces / 2;
			item = notes_bullet(notes_line, depth > 2 ? 2 : depth);
		} else {
			inline_text(p, end, notes_line, sizeof(notes_line));
			if (!notes_line[0]) {
				p = next;
				continue;
			}
			item = notes_rich(notes_content, notes_line);
		}

		if (gap) {
			lv_obj_set_style_margin_top(item, NOTES_GAP, 0);
		}
		gap = false;
		any = true;
		p = next;
	}

	if (!any) {
		lv_obj_t *item = notes_text_label(notes_content, tr("system_update_no_notes"), &font_ui_20);
		lv_obj_set_width(item, lv_pct(100));
	}
}

// ---------------------------------------------------------------------------
// the states
// ---------------------------------------------------------------------------

static void show_choice(void);
static void poll_cb(lv_timer_t *timer);

static void start_polling(void) {
	if (!poll_timer) {
		poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
	} else {
		lv_timer_resume(poll_timer);
	}
}

// A message and one OK that closes the card.
static void show_notice(const char *message) {
	stop_polling();
	phase = PHASE_NONE;
	layout(tr("system_update_firmware"), message, false, false);
	set_button(0, "ok", true, close_card);
	hide_buttons(1);
}

static void cancel_running(void) {
	ota_cancel();
	close_card();
}

static void show_checking(void) {
	layout(tr("system_update_firmware"), tr("system_update_checking"), false, false);
	set_button(0, "cancel", false, cancel_running);
	hide_buttons(1);
}

static void show_progress(void) {
	long done = 0, total = 0;
	ota_progress(&done, &total);
	if (total <= 0) {
		total = ota_release()->size;
	}

	char got_text[32], total_text[32], text[96];
	sysinfo_format_size((uint64_t)(done > 0 ? done : 0), got_text, sizeof(got_text));
	sysinfo_format_size((uint64_t)(total > 0 ? total : 0), total_text, sizeof(total_text));
	int permille = total > 0 ? (int)((long long)done * 1000 / total) : 0;
	snprintf(text, sizeof(text), "%s / %s  (%d%%)", got_text, total_text, permille / 10);

	lv_label_set_text(message_label, text);
	lv_bar_set_value(bar, permille, LV_ANIM_OFF);
}

static void show_downloading(void) {
	layout(PRODUCT_NAME, "", false, true);
	lv_obj_set_hidden(message_label, false);
	lv_bar_set_value(bar, 0, LV_ANIM_OFF);
	show_progress();
	set_button(0, "cancel", false, cancel_running);
	hide_buttons(1);
}

static void download_clicked(void) {
	if (!ota_download_start()) {
		show_notice(tr("system_update_busy"));
		return;
	}
	phase = PHASE_DOWNLOAD;
	show_downloading();
	start_polling();
}

// The box as tall as the notes, up to notes_max_h; longer notes scroll.
static void fit_notes(void) {
	lv_obj_set_height(notes_box, notes_max_h);
	lv_obj_update_layout(notes_box);
	int32_t h = lv_obj_get_height(notes_content);
	lv_obj_set_height(notes_box, h < notes_max_h ? h : notes_max_h);
	lv_obj_scroll_to_y(notes_box, 0, LV_ANIM_OFF);
}

static void show_release(void) {
	const ota_release_t *rel = ota_release();

	char size_text[32], message[128];
	sysinfo_format_size((uint64_t)rel->size, size_text, sizeof(size_text));
	snprintf(message, sizeof(message), tr("system_update_version_size"), rel->tag, size_text);

	layout(PRODUCT_NAME, message, true, false);

	render_notes(rel->notes);
	fit_notes();

	set_button(0, "system_update", true, download_clicked);
	set_button(1, "cancel", false, close_card);
	hide_buttons(2);
}

static void install_cb(lv_timer_t *timer) {
	(void)timer;
	firmware_update_start();
	// Returns only on the host build.
	close_card();
}

static void show_installing(void) {
	layout(PRODUCT_NAME, tr("system_update_installing"), false, false);
	hide_buttons(0);
	lv_timer_t *t = lv_timer_create(install_cb, INSTALL_DELAY_MS, NULL);
	lv_timer_set_repeat_count(t, 1);
}

static void check_finished(ota_result_t r) {
	char text[160];
	switch (r) {
	case OTA_OK:
		show_release();
		break;
	case OTA_UP_TO_DATE:
		snprintf(text, sizeof(text), tr("system_update_up_to_date"),
				 ota_installed_version()[0] ? ota_installed_version() : "?");
		show_notice(text);
		break;
	case OTA_NO_NETWORK:
		show_notice(tr("system_update_no_wifi"));
		break;
	case OTA_CANCELLED:
		close_card();
		break;
	default:
		show_notice(tr("system_update_check_failed"));
		break;
	}
}

static void download_finished(ota_result_t r) {
	switch (r) {
	case OTA_OK:
		show_installing();
		break;
	case OTA_NO_CARD:
		show_notice(tr("system_no_card"));
		break;
	case OTA_NO_SPACE:
		show_notice(tr("system_update_no_space"));
		break;
	case OTA_CORRUPT:
		show_notice(tr("system_update_corrupt"));
		break;
	case OTA_CANCELLED:
		close_card();
		break;
	default:
		show_notice(tr("system_update_download_failed"));
		break;
	}
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	ota_state_t s = ota_state();
	if (s == OTA_DOWNLOADING && phase == PHASE_DOWNLOAD) {
		show_progress();
		return;
	}
	if (s != OTA_FINISHED) {
		return;
	}

	int finished = phase;
	ota_result_t r = ota_result();
	ota_acknowledge();
	stop_polling();
	phase = PHASE_NONE;

	if (finished == PHASE_CHECK) {
		check_finished(r);
	} else if (finished == PHASE_DOWNLOAD) {
		download_finished(r);
	}
}

static void online_clicked(void) {
	if (!ota_check_start()) {
		show_notice(tr("system_update_busy"));
		return;
	}
	phase = PHASE_CHECK;
	show_checking();
	start_polling();
}

// The card goes first, then the file is looked for: on success the screen
// fades out, and a card left over it would be the last thing drawn.
static void sd_run(void *unused) {
	(void)unused;
	char path[512];
	if (!firmware_update_file_find(path, sizeof(path))) {
		gui_notify_popup("system_update_file_missing");
		return;
	}
	printf("firmware: update file %s\n", path);
	firmware_update_start();
}

static void sd_clicked(void) {
	close_card();
	lv_async_call(sd_run, NULL);
}

static void show_choice(void) {
	layout(tr("system_update_firmware"), NULL, false, false);
	set_button(0, "system_update_online", true, online_clicked);
	set_button(1, "system_update_from_sd", false, sd_clicked);
	set_button(2, "cancel", false, close_card);
}

void fwupdate_show(void) {
	if (!veil) {
		build();
	}
	phase = PHASE_NONE;
	show_choice();
	lv_obj_set_hidden(veil, false);
	lv_obj_move_foreground(veil);
	if (!holding_screen) {
		holding_screen = true;
		power_hold_screen_on(true);
	}
}
