#include "setupwifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/settings/timeset.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/net/wifi.h"

#define ROW_H 76
#define HEADER_BTN 56
#define POLL_MS 500
// A list left alone is scanned again this often, so a network switched on
// while the page is up appears without a button to ask for it.
#define RESCAN_MS 10000
#define PASSWORD_KEYBOARD_H 316

static lv_obj_t *panel;
static lv_obj_t *status_label;
static lv_obj_t *list;
static lv_timer_t *poll_timer;

static lv_obj_t *pw_panel;
static lv_obj_t *pw_title;
static lv_obj_t *pw_field;
static keyboard_t *pw_keyboard;

static bool radio_was_on;	  // the switch before the page turned it on
static bool joining;		  // a connect asked for from here is under way
static uint32_t drawn_serial; // the network list the rows show
static uint32_t last_scan;	  // lv_tick of the last scan asked for
static char pending_ssid[WIFI_SSID_MAX];

static const lv_image_dsc_t *glyph_for_bars(int bars) {
	switch (bars) {
	case 3:
		return &icon_wifi_max;
	case 2:
		return &icon_wifi_high;
	case 1:
		return &icon_wifi_low;
	default:
		return &icon_wifi_zero;
	}
}

static void status_set(const char *text) { lv_label_set_text(status_label, text ? text : ""); }

// On to the date and time, the page put away. A radio that was off before the
// page is turned off again when no network was joined.
static void finish(bool joined) {
	if (poll_timer) {
		lv_timer_delete(poll_timer);
		poll_timer = NULL;
	}
	keyboard_set_visible(pw_keyboard, false);
	lv_obj_set_hidden(pw_panel, true);
	lv_obj_set_hidden(panel, true);
	if (!joined && !radio_was_on) {
		wifi_set_enabled(false);
	}
	// The next page holds the screen on itself: nothing is released here, or
	// the panel could blank between the two.
	timeset_show_first_boot();
}

static void skip_cb(lv_event_t *e) {
	(void)e;
	finish(false);
}

// ---------------------------------------------------------------------------
// The passphrase
// ---------------------------------------------------------------------------

static void pw_close(void) {
	keyboard_set_visible(pw_keyboard, false);
	lv_obj_set_hidden(pw_panel, true);
}

static void pw_back_cb(lv_event_t *e) {
	(void)e;
	pw_close();
}

static void join(const char *ssid, const char *psk) {
	wifi_connect(ssid, psk);
	joining = true;
	char text[160];
	snprintf(text, sizeof(text), tr("setupwifi_connecting"), ssid);
	status_set(text);
}

static void pw_accept_cb(lv_event_t *e) {
	(void)e;
	const char *psk = lv_textarea_get_text(pw_field);
	if (!psk || !psk[0]) {
		return;
	}
	pw_close();
	join(pending_ssid, psk);
}

static void ask_password(const char *ssid) {
	if (ssid != pending_ssid) {
		snprintf(pending_ssid, sizeof(pending_ssid), "%s", ssid);
	}
	lv_label_set_text(pw_title, pending_ssid);
	lv_textarea_set_text(pw_field, "");
	keyboard_reset(pw_keyboard);
	keyboard_set_visible(pw_keyboard, true);
	lv_obj_set_hidden(pw_panel, false);
	lv_obj_move_foreground(pw_panel);
}

// ---------------------------------------------------------------------------
// The networks
// ---------------------------------------------------------------------------

static void row_free_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

static void row_clicked_cb(lv_event_t *e) {
	const char *ssid = lv_event_get_user_data(e);
	lv_obj_t *row = lv_event_get_current_target(e);
	if (!ssid || !ssid[0] || joining) {
		return;
	}
	bool ask = (uintptr_t)lv_obj_get_user_data(row) != 0;
	if (ask) {
		ask_password(ssid);
	} else {
		join(ssid, NULL);
	}
}

static void add_row(const wifi_network_t *net) {
	char *ssid = strdup(net->ssid);
	if (!ssid) {
		return;
	}

	lv_obj_t *row = lv_btn_create(list);
	lv_obj_set_size(row, lv_pct(100), ROW_H);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 18, 0);
	lv_obj_set_style_pad_ver(row, 0, 0);
	lv_obj_set_style_pad_column(row, 14, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// A passphrase to ask for: secured and not already known.
	lv_obj_set_user_data(row, (void *)(uintptr_t)(net->secured && !net->saved));
	lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, ssid);
	lv_obj_add_event_cb(row, row_free_cb, LV_EVENT_DELETE, ssid);

	lv_obj_t *icon = lv_image_create(row);
	lv_image_set_src(icon, glyph_for_bars(net->bars));
	lv_obj_add_style(icon, &theme_style_icon, 0);

	lv_obj_t *name = lv_label_create(row);
	lv_label_set_text(name, net->ssid);
	lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(name, 1);
	lv_obj_set_height(name, lv_font_get_line_height(&font_ui_24));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	const char *state = net->saved ? "wifi_saved" : (net->secured ? "wifi_secured" : "wifi_open");
	lv_obj_t *sub = lv_label_create(row);
	lv_label_set_text(sub, tr(state));
	lv_obj_add_style(sub, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(sub, &font_ui_20, 0);
}

static void rebuild_list(void) {
	lv_obj_clean(list);
	wifi_network_t networks[WIFI_MAX_NETWORKS];
	int count = wifi_get_networks(networks, WIFI_MAX_NETWORKS);
	int shown = 0;
	for (int i = 0; i < count; i++) {
		// Only what is on the air: a saved network out of range cannot be
		// joined from here.
		if (networks[i].ssid[0] && networks[i].signal) {
			add_row(&networks[i]);
			shown++;
		}
	}
	if (!joining) {
		status_set(shown ? NULL : tr(wifi_scan_running() ? "wifi_searching_again" : "wifi_no_networks_found"));
	}
}

static void poll_cb(lv_timer_t *t) {
	(void)t;

	wifi_status_t status;
	wifi_get_status(&status);

	char ssid[WIFI_SSID_MAX] = "";
	wifi_op_t op = wifi_take_op_result(ssid, sizeof(ssid));
	if (op == WIFI_OP_OK || (status.state == WIFI_STATE_CONNECTED && !joining)) {
		// Joined, from here or by the radio itself to a network it knew. The
		// toast comes after the next page, so it is drawn over it.
		finish(true);
		toast_success(ssid[0] ? ssid : status.ssid);
		return;
	}
	if (op == WIFI_OP_FAILED) {
		joining = false;
		bool wrong_key = wifi_last_failure() == WIFI_FAIL_WRONG_KEY;
		const char *why = wrong_key ? "wifi_incorrect_password" : "wifi_connection_failed";
		status_set(tr(why));
		if (wrong_key && pending_ssid[0]) {
			ask_password(pending_ssid);
		}
		// After the passphrase panel, so the toast is drawn over it.
		toast_error(why);
		return;
	}

	if (!wifi_get_enabled() || wifi_busy()) {
		status_set(tr("turning_on"));
		return;
	}

	uint32_t now = lv_tick_get();
	if (!joining && !wifi_scan_running() && (last_scan == 0 || now - last_scan >= RESCAN_MS)) {
		last_scan = now ? now : 1;
		wifi_scan_start();
	}

	uint32_t serial = wifi_networks_serial();
	if (serial != drawn_serial) {
		drawn_serial = serial;
		rebuild_list();
	}
}

void setupwifi_show(void) {
	if (!panel || !wifi_available()) {
		timeset_show_first_boot();
		return;
	}

	radio_was_on = wifi_get_enabled();
	if (!radio_was_on) {
		wifi_set_enabled(true);
	}
	joining = false;
	pending_ssid[0] = '\0';
	last_scan = 0;
	drawn_serial = wifi_networks_serial() - 1; // the first poll draws the list
	lv_obj_clean(list);
	status_set(tr("wifi_searching_again"));

	lv_obj_set_hidden(panel, false);
	lv_obj_move_foreground(panel);
	power_hold_screen_on(true);

	if (!poll_timer) {
		poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
	}
	poll_cb(NULL);
}

static lv_obj_t *top_panel(gui_config_t *cfg) {
	lv_obj_t *p = lv_obj_create(lv_layer_top());
	lv_obj_set_size(p, cfg->screen_width, cfg->screen_height);
	lv_obj_align(p, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(p, &theme_style_screen, 0);
	lv_obj_set_style_border_width(p, 0, 0);
	lv_obj_set_style_radius(p, 0, 0);
	lv_obj_set_scrollable(p, false);
	lv_obj_set_hidden(p, true);
	return p;
}

static void build_password_panel(gui_config_t *cfg) {
	pw_panel = top_panel(cfg);
	lv_obj_set_style_pad_all(pw_panel, 0, 0);

	lv_obj_t *back = lv_btn_create(pw_panel);
	lv_obj_set_size(back, HEADER_BTN, HEADER_BTN);
	lv_obj_align(back, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding);
	lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(back, 0, 0);
	lv_obj_set_style_shadow_width(back, 0, 0);
	lv_obj_set_style_pad_all(back, 0, 0);
	lv_obj_add_event_cb(back, pw_back_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *back_icon = lv_image_create(back);
	lv_image_set_src(back_icon, &icon_chevron_left);
	lv_obj_add_style(back_icon, &theme_style_icon, 0);
	lv_obj_center(back_icon);

	pw_title = lv_label_create(pw_panel);
	lv_label_set_long_mode(pw_title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(pw_title, cfg->screen_width - 2 * cfg->padding - HEADER_BTN - 14);
	lv_obj_add_style(pw_title, &theme_style_text, 0);
	lv_obj_set_style_text_font(pw_title, &font_ui_32, 0);
	lv_obj_align(pw_title, LV_ALIGN_TOP_LEFT, cfg->padding + HEADER_BTN + 14, cfg->padding + (HEADER_BTN - lv_font_get_line_height(&font_ui_32)) / 2);

	int top = cfg->padding + HEADER_BTN + cfg->padding;
	lv_obj_t *hint = lv_label_create(pw_panel);
	lv_label_set_text(hint, tr("wifi_network_password"));
	lv_obj_add_style(hint, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(hint, &font_ui_22, 0);
	lv_obj_align(hint, LV_ALIGN_TOP_LEFT, cfg->padding, top);

	pw_field = lv_textarea_create(pw_panel);
	lv_textarea_set_one_line(pw_field, true);
	lv_textarea_set_placeholder_text(pw_field, tr("password"));
	lv_obj_set_size(pw_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(pw_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(pw_field, LV_ALIGN_TOP_LEFT, cfg->padding, top + 36);
	lv_obj_add_style(pw_field, &theme_style_card, 0);
	lv_obj_set_style_radius(pw_field, 12, 0);
	lv_obj_set_style_border_width(pw_field, 0, 0);
	lv_obj_set_style_shadow_width(pw_field, 0, 0);
	lv_obj_set_style_pad_all(pw_field, 14, 0);
	lv_obj_set_style_text_font(pw_field, &font_ui_24, 0);
	keyboard_style_caret(pw_field);
	keyboard_style_password(pw_field, 0);

	pw_keyboard = keyboard_create(pw_panel, cfg->screen_width, PASSWORD_KEYBOARD_H, pw_field, NULL, "ok", pw_accept_cb, NULL);
}

void setupwifi_init(gui_config_t *cfg) {
	panel = top_panel(cfg);
	lv_obj_set_style_pad_all(panel, cfg->padding, 0);
	lv_obj_set_style_pad_top(panel, cfg->padding * 2, 0);
	lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(panel, 14, 0);

	lv_obj_t *title = lv_label_create(panel);
	lv_label_set_text(title, tr("setupwifi_title"));
	lv_obj_add_style(title, &theme_style_text, 0);
	lv_obj_set_style_text_font(title, &font_ui_32, 0);

	lv_obj_t *note = lv_label_create(panel);
	lv_label_set_text(note, tr("setupwifi_note"));
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_20, 0);

	status_label = lv_label_create(panel);
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_20, 0);
	lv_label_set_text(status_label, "");

	list = lv_obj_create(panel);
	lv_obj_set_width(list, lv_pct(100));
	lv_obj_set_flex_grow(list, 1);
	lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_pad_all(list, 0, 0);
	lv_obj_set_style_pad_gap(list, 8, 0);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

	lv_obj_t *skip = lv_btn_create(panel);
	lv_obj_set_size(skip, 228, 64);
	lv_obj_add_style(skip, &theme_style_card, 0);
	lv_obj_add_style(skip, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(skip, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(skip, 0, 0);
	lv_obj_set_style_shadow_width(skip, 0, 0);
	lv_obj_add_event_cb(skip, skip_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *skip_label = lv_label_create(skip);
	lv_label_set_text(skip_label, tr("setupwifi_skip"));
	lv_obj_add_style(skip_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(skip_label, &font_ui_24, 0);
	lv_obj_center(skip_label);

	build_password_panel(cfg);
}
