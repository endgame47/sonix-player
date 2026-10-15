#include "dacpage.h"

#include <stdio.h>

#include "src/gui/shell/confirm.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/pagetimer.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/audio/usbdac.h"

lv_obj_t *dacpage_screen;

static lv_obj_t *mode_switch;
static lv_obj_t *big_icon;
static lv_obj_t *format_label;
static lv_obj_t *status_label;
static lv_obj_t *charge_btn;
static lv_obj_t *charge_glyph;
static lv_timer_t *poll_timer;
static unsigned last_serial = (unsigned)-1;
static lv_color_t status_normal_color; // the theme's colour, captured at build time

static void refresh_charge_glyph(void) {
	if (!charge_glyph) {
		return;
	}
	// A lightning bolt when the cable is allowed to charge, the struck-through
	// one when it is not.
	lv_image_set_src(charge_glyph, usbdac_charging_enabled() ? &icon_zap : &icon_zap_off);
}

static void refresh(void) {
	usbdac_state_t st;
	usbdac_get_state(&st);

	// The switch follows the mode rather than the finger: a start that failed
	// must not leave the toggle sitting proudly in the on position.
	if (st.active || st.starting) {
		lv_obj_add_state(mode_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(mode_switch, LV_STATE_CHECKED);
	}

	// Dimmed when the mode is off, but never LV_STATE_DISABLED: a disabled LVGL
	// object stops receiving presses altogether, so the button stays clickable
	// and explains itself instead of silently doing nothing.
	lv_obj_set_style_opa(charge_glyph, st.active ? LV_OPA_COVER : LV_OPA_40, 0);
	refresh_charge_glyph();

	// Back to the theme's colour unless the checks below make it red. Set, never
	// removed: lv_obj_remove_style(obj, NULL, ...) drops every local style on
	// that part, width, font and alignment included. The colour to restore is
	// captured at build time, before anything local is set over it.
	lv_obj_set_style_text_color(status_label, status_normal_color, 0);

	if (st.error[0]) {
		lv_label_set_text(format_label, "");
		lv_label_set_text(status_label, st.error);
		lv_obj_set_style_text_color(status_label, lv_color_make(224, 27, 36), 0);
		return;
	}

	if (st.starting) {
		lv_label_set_text(format_label, "");
		lv_label_set_text(status_label, tr("dac_turning_on_2"));
		return;
	}

	if (!st.active) {
		lv_label_set_text(format_label, "");
		lv_label_set_text(status_label, tr("dac_note"));
		return;
	}

	if (!st.streaming) {
		lv_label_set_text(format_label, "");
		lv_label_set_text(status_label, tr("dac_waiting"));
		return;
	}

	// The numbers under the icon. The rate is the host's choice: the gadget
	// announces the whole range and the computer picks one.
	//
	// Sample rate only: the driver always delivers 32-bit words whatever the
	// host negotiated, so a bit depth printed here would always read 32.
	char text[64];
	if (st.dsd) {
		snprintf(text, sizeof(text), "DSD  %d Hz", st.sample_rate);
	} else if (st.sample_rate % 1000 == 0) {
		snprintf(text, sizeof(text), "%d kHz", st.sample_rate / 1000);
	} else {
		snprintf(text, sizeof(text), "%.1f kHz", st.sample_rate / 1000.0);
	}
	lv_label_set_text(format_label, text);
	lv_label_set_text(status_label, tr("dac_playing"));
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	if (lv_screen_active() != dacpage_screen) {
		return;
	}
	unsigned now = usbdac_serial();
	if (now == last_serial) {
		return;
	}
	last_serial = now;
	refresh();
}

// Applies whatever the switch currently says.
static void apply_mode_from_switch(void) {
	if (lv_obj_has_state(mode_switch, LV_STATE_CHECKED)) {
		usbdac_start(); // a failure lands in the state, and refresh() shows it
	} else {
		usbdac_stop();
	}
	last_serial = (unsigned)-1;
	refresh();
}

// The whole row toggles, not just the switch: a 68x36 switch at the far edge of
// the screen is a small target, and the row gives one the width of the page
// whatever else may sit over that corner.
static void mode_row_cb(lv_event_t *e) {
	(void)e;
	if (lv_obj_has_state(mode_switch, LV_STATE_CHECKED)) {
		lv_obj_remove_state(mode_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_add_state(mode_switch, LV_STATE_CHECKED);
	}
	apply_mode_from_switch();
}

static void mode_cb(lv_event_t *e) {
	(void)e;
	// The stored switch rather than the event target, matching every other
	// toggle in the settings pages: the target of a VALUE_CHANGED can be the
	// row rather than the switch depending on how the row was built.
	apply_mode_from_switch();
}

static bool leaving;

// Leaves for real; the guard below lets the chevron through once `leaving` is
// set. Deferred by one turn of the event loop because really_leave() runs from
// inside the confirmation dialog's own handler, and navigating out from under a
// dialog that is still closing leaves the screen change undone.
static void leave_async(void *user) {
	(void)user;
	back_btn_cb(NULL);
	leaving = false;
}

static void really_leave(void *user) {
	(void)user;
	usbdac_stop();
	last_serial = (unsigned)-1;
	refresh();
	leaving = true;
	lv_async_call(leave_async, NULL);
}

// The back chevron and the swipe both come through here. Returning true means
// "handled, stay put": DAC mode is not something to wander out of by accident
// with a computer streaming through the cable. Once it is off the guard steps
// aside and back works as it does anywhere else.
static bool back_guard(void) {
	if (leaving || !usbdac_is_active()) {
		return false;
	}
	confirm_show("dac_leave_dac_mode",
				 "dac_leave_confirm_note", "leave",
				 really_leave, NULL);
	return true;
}

static void charge_cb(lv_event_t *e) {
	(void)e;
	if (!usbdac_is_active()) {
		gui_notify_popup("dac_turn_dac_mode_on_first");
		return;
	}
	// The choice is written to the config, so a player set never to charge in
	// DAC mode stays that way across reboots.
	bool now_on = !usbdac_charging_enabled();
	usbdac_set_charging(now_on);
	refresh_charge_glyph();

	// The two glyphs differ by one thin diagonal stroke, which at 34 px is not
	// enough to tell a state change from a button that did nothing. Say it in
	// words as well.
	gui_notify_popup(now_on ? "dac_usb_charging_on" : "dac_usb_charging_off");
}

bool dacpage_is_holding(void) {
	return usbdac_is_active() && lv_screen_active() == dacpage_screen;
}

// Same shape as the corner buttons on the music page.
static lv_obj_t *corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph, lv_obj_t **glyph_out) {
	lv_obj_t *button = lv_btn_create(dacpage_screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (56 + 6), cfg->padding + cfg->top_bar_height);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	if (glyph_out) {
		*glyph_out = icon;
	}
	return button;
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	last_serial = (unsigned)-1;
	refresh();
}

void dacpage_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(dacpage_screen, cfg, "dac");

	// The mode is a switch, not a side effect of arriving: the page can be
	// looked at, and the charging button read, without the cable being taken
	// over the moment it opens.
	mode_switch = NULL;
	lv_obj_t *mode_row = settingsrow_toggle(container, "dac_mode", &mode_switch, mode_cb);
	lv_obj_set_clickable(mode_row, true);
	lv_obj_add_event_cb(mode_row, mode_row_cb, LV_EVENT_CLICKED, NULL);

	// Straight into the flex flow, with no alignment of its own: the container
	// settingsrow_page() builds already centres its children on the cross axis,
	// and an object positioned by alignment leaves the flow and is measured as
	// taking no space, so it would overlap the row above and steal its presses.
	big_icon = lv_image_create(container);
	lv_image_set_src(big_icon, &icon_dac_page);
	lv_obj_set_style_margin_top(big_icon, 40, 0);

	format_label = lv_label_create(container);
	lv_obj_set_width(format_label, lv_pct(100));
	lv_label_set_long_mode(format_label, LV_LABEL_LONG_WRAP);
	// The theme's own text style, not the inherited default, which is black and
	// so invisible on the dark theme.
	lv_obj_add_style(format_label, &theme_style_text, 0);
	lv_obj_set_style_text_align(format_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(format_label, &font_ui_24_bold, 0);
	lv_obj_set_style_margin_top(format_label, 20, 0);
	lv_label_set_text(format_label, "");

	status_label = lv_label_create(container);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_22, 0);
	lv_obj_set_style_margin_top(status_label, 10, 0);
	lv_label_set_text(status_label, "");
	status_normal_color = lv_obj_get_style_text_color(status_label, LV_PART_MAIN);

	charge_btn = corner_button(cfg, 0, &icon_zap, &charge_glyph);
	lv_obj_add_event_cb(charge_btn, charge_cb, LV_EVENT_CLICKED, NULL);
	// settingsrow_title reserves CORNER_BTN_SIZE on the right, but the settings
	// container spans the whole screen and is built after this button, so the
	// button must come to the front to stay reachable.
	lv_obj_move_foreground(charge_btn);
	lv_obj_set_style_opa(charge_glyph, LV_OPA_40, 0);
	refresh_charge_glyph();

	lv_obj_add_event_cb(dacpage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_set_back_guard(dacpage_screen, back_guard);

	poll_timer = lv_timer_create(poll_cb, 500, NULL);
	pagetimer_bind(poll_timer, dacpage_screen, NULL);
}
