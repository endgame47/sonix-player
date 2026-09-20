#include "appearance.h"

#include <stdint.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

lv_obj_t *appearance_screen;

static lv_obj_t *btn_dark;
static lv_obj_t *btn_light;
static lv_obj_t *btn_clock[4]; // left / centre / right / hidden
static lv_obj_t *btn_accent[THEME_ACCENT_COUNT]; // the coloured circles
static lv_obj_t *tint_toggle;
static lv_obj_t *battery_percent_toggle;
// The status bar draws the battery either way; this setting is only about the
// number beside it, and is on by default.
static void battery_percent_cb(lv_event_t *e) {
	(void)e;
	bool shown = lv_obj_has_state(battery_percent_toggle, LV_STATE_CHECKED);
	topbar_set_battery_percent(shown);
	config_set_int("screen", "battery_percent", shown ? 1 : 0);
	config_save();
}

// The accent's hue spread over the backgrounds and cards. theme_set_dynamic_tint
// repaints the interface itself, including this page.
static void dynamic_tint_cb(lv_event_t *e) {
	(void)e;
	theme_set_dynamic_tint(lv_obj_has_state(tint_toggle, LV_STATE_CHECKED));
}

// Paints the pair: the active choice is the filled accent button, the other a
// quiet neutral, like libadwaita's suggested-action beside a regular button.
static void refresh_buttons(void) {
	bool dark = theme_is_dark();

	lv_obj_t *on = dark ? btn_dark : btn_light;
	lv_obj_t *off = dark ? btn_light : btn_dark;

	lv_obj_set_style_bg_color(on, theme()->accent, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(on, 0), lv_color_white(), 0);

	lv_obj_set_style_bg_color(off, theme()->surface_pressed, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(off, 0), theme()->text_primary, 0);
}

// The active circle wears a ring in the text colour; the rest sit flat.
static void refresh_accent_buttons(void) {
	int active_accent = theme_get_accent();
	for (int i = 0; i < THEME_ACCENT_COUNT; i++) {
		if (!btn_accent[i]) {
			continue;
		}
		lv_obj_set_style_bg_color(btn_accent[i], theme_accent_preset(i), 0);
		if (i == active_accent) {
			lv_obj_set_style_border_width(btn_accent[i], 3, 0);
			lv_obj_set_style_border_color(btn_accent[i], theme()->text_primary, 0);
			lv_obj_set_style_border_opa(btn_accent[i], LV_OPA_COVER, 0);
		} else {
			lv_obj_set_style_border_width(btn_accent[i], 0, 0);
		}
	}
}

static void accent_pick_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	theme_set_accent(index);
	refresh_accent_buttons();
}

static lv_obj_t *make_accent_circle(lv_obj_t *parent, int index) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, 48, 48);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_bg_color(btn, theme_accent_preset(index), 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
	lv_obj_add_event_cb(btn, accent_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
	return btn;
}

// Paints the clock-position pills the same way: accent on the active one.
static void refresh_clock_buttons(void) {
	int active = (int)config_get_int("screen", "clock_pos", TOPBAR_CLOCK_CENTER);
	for (int i = 0; i < 4; i++) {
		if (!btn_clock[i]) {
			continue;
		}
		if (i == active) {
			lv_obj_set_style_bg_color(btn_clock[i], theme()->accent, 0);
			lv_obj_set_style_text_color(lv_obj_get_child(btn_clock[i], 0), lv_color_white(), 0);
		} else {
			lv_obj_set_style_bg_color(btn_clock[i], theme()->surface_pressed, 0);
			lv_obj_set_style_text_color(lv_obj_get_child(btn_clock[i], 0), theme()->text_primary, 0);
		}
	}
}

static void clock_pos_cb(lv_event_t *e) {
	int pos = (int)(intptr_t)lv_event_get_user_data(e);
	config_set_int("screen", "clock_pos", pos);
	config_save();
	topbar_set_clock_position(pos);
	refresh_clock_buttons();
}

static lv_obj_t *make_clock_choice(lv_obj_t *parent, const char *text, int pos) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 64);
	lv_obj_set_style_pad_hor(btn, 14, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // Adwaita pill button
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, clock_pos_cb, LV_EVENT_CLICKED, (void *)(intptr_t)pos);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_center(label);

	return btn;
}

static void pick_cb(lv_event_t *e) {
	bool dark = (bool)(uintptr_t)lv_event_get_user_data(e);
	theme_set_dark(dark);
	refresh_buttons();
}

static lv_obj_t *make_choice(lv_obj_t *parent, const char *text, bool dark) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 64);
	lv_obj_set_style_pad_hor(btn, 34, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // Adwaita pill button
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, pick_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)dark);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_center(label);

	return btn;
}

void appearance_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(appearance_screen, cfg, "appearance");

	// One tall card: the option's name on top, the two choices under it.
	lv_obj_t *card = lv_obj_create(container);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_gap(card, 18, 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *name = lv_label_create(card);
	lv_label_set_text(name, tr("appearance_theme"));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	lv_obj_t *row = lv_obj_create(card);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_gap(row, 14, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	btn_dark = make_choice(row, "appearance_dark", true);
	btn_light = make_choice(row, "appearance_light", false);

	// A second card: where the clock sits in the status bar.
	lv_obj_t *clock_card = lv_obj_create(container);
	lv_obj_set_width(clock_card, lv_pct(100));
	lv_obj_set_height(clock_card, LV_SIZE_CONTENT);
	lv_obj_add_style(clock_card, &theme_style_card, 0);
	lv_obj_set_style_radius(clock_card, 12, 0);
	lv_obj_set_style_border_width(clock_card, 0, 0);
	lv_obj_set_style_shadow_width(clock_card, 0, 0);
	lv_obj_set_style_pad_all(clock_card, 20, 0);
	lv_obj_set_style_pad_gap(clock_card, 18, 0);
	lv_obj_remove_flag(clock_card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(clock_card, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(clock_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(clock_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *clock_name = lv_label_create(clock_card);
	lv_label_set_text(clock_name, tr("appearance_clock_position"));
	lv_obj_add_style(clock_name, &theme_style_text, 0);
	lv_obj_set_style_text_font(clock_name, &font_ui_24, 0);

	lv_obj_t *clock_row = lv_obj_create(clock_card);
	lv_obj_set_size(clock_row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(clock_row, 0, 0);
	lv_obj_set_style_border_width(clock_row, 0, 0);
	lv_obj_set_style_pad_all(clock_row, 0, 0);
	lv_obj_set_style_pad_gap(clock_row, 10, 0);
	lv_obj_remove_flag(clock_row, LV_OBJ_FLAG_SCROLLABLE);
	// Wrapped, not a single line: four pills of translated words do not fit
	// across 480 pixels in every language, and one that does not fit is drawn
	// off the card rather than shrunk.
	lv_obj_set_flex_flow(clock_row, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(clock_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	btn_clock[TOPBAR_CLOCK_LEFT] = make_clock_choice(clock_row, "left", TOPBAR_CLOCK_LEFT);
	btn_clock[TOPBAR_CLOCK_CENTER] = make_clock_choice(clock_row, "centre", TOPBAR_CLOCK_CENTER);
	btn_clock[TOPBAR_CLOCK_RIGHT] = make_clock_choice(clock_row, "right", TOPBAR_CLOCK_RIGHT);
	btn_clock[TOPBAR_CLOCK_HIDDEN] = make_clock_choice(clock_row, "hide", TOPBAR_CLOCK_HIDDEN);

	// A third card: the accent colour, as a row of coloured circles.
	lv_obj_t *accent_card = lv_obj_create(container);
	lv_obj_set_width(accent_card, lv_pct(100));
	lv_obj_set_height(accent_card, LV_SIZE_CONTENT);
	lv_obj_add_style(accent_card, &theme_style_card, 0);
	lv_obj_set_style_radius(accent_card, 12, 0);
	lv_obj_set_style_border_width(accent_card, 0, 0);
	lv_obj_set_style_shadow_width(accent_card, 0, 0);
	lv_obj_set_style_pad_all(accent_card, 20, 0);
	lv_obj_set_style_pad_gap(accent_card, 18, 0);
	lv_obj_remove_flag(accent_card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(accent_card, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_set_flex_flow(accent_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(accent_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *accent_name = lv_label_create(accent_card);
	lv_label_set_text(accent_name, tr("appearance_accent_colour"));
	lv_obj_add_style(accent_name, &theme_style_text, 0);
	lv_obj_set_style_text_font(accent_name, &font_ui_24, 0);

	lv_obj_t *accent_row = lv_obj_create(accent_card);
	lv_obj_set_size(accent_row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(accent_row, 0, 0);
	lv_obj_set_style_border_width(accent_row, 0, 0);
	lv_obj_set_style_pad_all(accent_row, 0, 0);
	lv_obj_set_style_pad_gap(accent_row, 14, 0);
	lv_obj_remove_flag(accent_row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(accent_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(accent_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	for (int i = 0; i < THEME_ACCENT_COUNT; i++) {
		btn_accent[i] = make_accent_circle(accent_row, i);
	}

	// Straight under the accent, because it is what the tint is made from: the
	// same hue carried across the backgrounds, the cards and the status bar.
	settingsrow_toggle(container, "appearance_dynamic_tint", &tint_toggle, dynamic_tint_cb);
	if (theme_dynamic_tint()) {
		lv_obj_add_state(tint_toggle, LV_STATE_CHECKED);
	}

	lv_obj_t *tint_note = lv_label_create(container);
	lv_label_set_long_mode(tint_note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(tint_note, lv_pct(100));
	lv_obj_add_style(tint_note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(tint_note, &font_ui_22, 0);
	lv_label_set_text(tint_note, tr("appearance_dynamic_tint_note"));

	// A fifth card, and the last plain switch on the page: whether the status
	// bar prints the charge as a number as well as drawing it.
	settingsrow_toggle(container, "appearance_battery_percentage", &battery_percent_toggle, battery_percent_cb);
	if (config_get_int("screen", "battery_percent", 1) != 0) {
		lv_obj_add_state(battery_percent_toggle, LV_STATE_CHECKED);
	}

	refresh_buttons();
	refresh_clock_buttons();
	refresh_accent_buttons();
	theme_register_refresh(refresh_buttons);
	theme_register_refresh(refresh_clock_buttons);
	theme_register_refresh(refresh_accent_buttons);
}
