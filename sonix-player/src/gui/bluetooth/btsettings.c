#include "btsettings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/bluetooth/btaudio.h"
#include "src/gui/bluetooth/btreceiverpage.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/spinner.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/gui/shell/topbar.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/core/lang.h"

lv_obj_t *btsettings_screen;

#define BT_PAGE_POLL_MS 500
#define BT_ROW_H 76
#define CORNER_BTN_SIZE 56

static lv_obj_t *bt_switch;
static lv_obj_t *status_label;
static lv_obj_t *paired_label;
static lv_obj_t *paired_list;
static lv_obj_t *found_label;
static lv_obj_t *found_list;
static lv_obj_t *scan_btn;
static lv_obj_t *scan_icon;
static lv_obj_t *receiver_btn;
static lv_obj_t *receiver_icon;
static lv_timer_t *poll_timer;

static uint32_t drawn_serial = 0xFFFFFFFFu;
static bool drawn_enabled;

// The device a popover is about. Copied, so the list can be rebuilt under it.
static char menu_mac[BT_MAC_MAX];

// The last action requested. The radio reports the same "it worked" for
// connect, disconnect and forget alike, so this is what picks the wording of
// the confirmation. The confirmation names the action, not the device, which
// the list already shows two lines below.
typedef enum {
	ACTION_NONE = 0,
	ACTION_CONNECT,
	ACTION_DISCONNECT,
	ACTION_FORGET,
} last_action_t;

static last_action_t last_action;

static void rebuild_lists(void);

// ---------------------------------------------------------------------------
// Pairing watch
//
// Pairing is the slowest action on this page: bluez has to rediscover the
// device, pair it, and only then bring the A2DP link up -- half a minute on a
// good day, two discovery sweeps on a bad one. A timed toast would expire long
// before the answer, leaving no way to tell a slow success from a silent
// failure.
//
// The notice therefore stays up until the device is actually connected -- not
// paired, not "the command returned" -- watched by this timer. It is separate
// from the page poll timer because it must survive leaving the page: the modal
// lives on the top layer and would otherwise sit over whatever screen follows.
// ---------------------------------------------------------------------------

#define PAIRING_WATCH_MS 300
#define PAIRING_TIMEOUT_MS 90000

static lv_timer_t *pairing_timer;
static char pairing_mac[BT_MAC_MAX];
static char pairing_name[BT_NAME_MAX];
static uint32_t pairing_started_ms;

static bool pairing_watch_active(void) { return pairing_timer != NULL; }

static void pairing_watch_stop(void) {
	if (pairing_timer) {
		lv_timer_delete(pairing_timer);
		pairing_timer = NULL;
	}
	gui_modal_hide();
	pairing_mac[0] = '\0';
}

// True only once the link is really up: bluetooth_get_paired() reports the
// Connected flag bluez publishes on D-Bus, the same source the list is drawn
// from.
static bool pairing_device_connected(void) {
	bt_device_t devices[BT_MAX_DEVICES];
	int count = bluetooth_get_paired(devices, BT_MAX_DEVICES);
	for (int i = 0; i < count; i++) {
		if (devices[i].connected && strcmp(devices[i].mac, pairing_mac) == 0) {
			return true;
		}
	}
	return false;
}

static void pairing_watch_cb(lv_timer_t *timer) {
	(void)timer;

	if (!pairing_mac[0]) {
		pairing_watch_stop();
		return;
	}

	if (pairing_device_connected()) {
		bluetooth_take_op_result(NULL, 0); // consume it here so the page poll does not toast it too
		pairing_watch_stop();
		toast_success("bt_device_connected");
		topbar_refresh_radios();
		return;
	}

	// The worker gave up before the link came up.
	char reported[BT_NAME_MAX];
	if (bluetooth_take_op_result(reported, sizeof(reported)) == BT_OP_FAILED) {
		pairing_watch_stop();
		gui_notify_popup("bt_pairing_failed");
		return;
	}

	// The radio was switched off, or nothing ever came of the request.
	if (!bluetooth_get_enabled() || (lv_tick_get() - pairing_started_ms) > PAIRING_TIMEOUT_MS) {
		pairing_watch_stop();
		gui_notify_popup("bt_pairing_failed");
	}
}

static void pairing_watch_start(const char *mac, const char *name) {
	if (pairing_timer) {
		lv_timer_delete(pairing_timer);
		pairing_timer = NULL;
	}

	snprintf(pairing_mac, sizeof(pairing_mac), "%s", mac);
	snprintf(pairing_name, sizeof(pairing_name), "%s", name ? name : "");
	pairing_started_ms = lv_tick_get();

	// The Bluetooth glyph in the default accent blue, regardless of the accent
	// the user picked elsewhere: here the colour identifies the radio rather
	// than decorating the screen.
	gui_modal_show(&icon_bluetooth_connecting, theme_accent_preset(0), "pairing",
				   pairing_name[0] ? pairing_name : NULL);

	pairing_timer = lv_timer_create(pairing_watch_cb, PAIRING_WATCH_MS, NULL);
}

// ---------------------------------------------------------------------------
// The single status line
// ---------------------------------------------------------------------------

static void refresh_status_label(void) {
	if (!status_label) {
		return;
	}

	if (!bluetooth_available()) {
		lv_label_set_text(status_label, tr("bt_no_hardware"));
		return;
	}

	if (bluetooth_busy()) {
		lv_label_set_text(status_label, bluetooth_get_enabled() ? tr("turning_on") : tr("bt_turning_off"));
		return;
	}

	// Once the radio is up the lists already say which devices exist and which
	// one is connected, so this line only reports the local visible name.
	if (bluetooth_get_enabled()) {
		lv_label_set_text_fmt(status_label, tr("bt_visible_as"), bluetooth_local_name());
	} else {
		lv_label_set_text(status_label, tr("bt_bluetooth_off"));
	}
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

static void connect_action(void *user) {
	(void)user;
	last_action = ACTION_CONNECT;
	bluetooth_connect(menu_mac);
}

static void disconnect_action(void *user) {
	(void)user;
	last_action = ACTION_DISCONNECT;
	bluetooth_disconnect(menu_mac);
}

static void forget_action(void *user) {
	(void)user;
	last_action = ACTION_FORGET;
	bluetooth_forget(menu_mac);
}

// ---------------------------------------------------------------------------
// Renaming a device
//
// The name a paired device is shown under on this player, kept by bluez with
// the pairing. A sheet over the page with the field and the keyboard, as for
// the player's own name. Emptied, the field gives the device back the name it
// announces itself.
// ---------------------------------------------------------------------------

#define RENAME_CANCEL_SIZE 56

static lv_obj_t *rename_layer;
static lv_obj_t *rename_field;
static keyboard_t *rename_keyboard;
// The device being renamed. Its own copy: the popover's can change under the
// sheet if another row's menu is opened from behind it.
static char rename_mac[BT_MAC_MAX];

static void rename_layer_hide(void) {
	if (rename_layer) {
		lv_obj_set_hidden(rename_layer, true);
	}
	back_btn_force_hidden(false);
}

static void rename_cancel_cb(lv_event_t *e) {
	(void)e;
	rename_layer_hide();
}

static void rename_accept_cb(lv_event_t *e) {
	(void)e;
	const char *typed = lv_textarea_get_text(rename_field);
	rename_layer_hide();
	if (bluetooth_rename(rename_mac, typed ? typed : "")) {
		toast_success("bt_name_changed");
	} else {
		toast_error("bt_bluetooth_off");
	}
}

static void rename_action(void *user) {
	(void)user;
	if (!rename_layer) {
		return;
	}
	snprintf(rename_mac, sizeof(rename_mac), "%s", menu_mac);

	// The field starts with the name the list shows.
	const char *current = "";
	bt_device_t devices[BT_MAX_DEVICES];
	int count = bluetooth_get_paired(devices, BT_MAX_DEVICES);
	for (int i = 0; i < count; i++) {
		if (strcmp(devices[i].mac, rename_mac) == 0) {
			current = devices[i].name;
			break;
		}
	}
	lv_textarea_set_text(rename_field, current);
	keyboard_reset(rename_keyboard);
	lv_obj_set_hidden(rename_layer, false);
	lv_obj_move_foreground(rename_layer);
	// The sheet has its own close in the corner, and the chevron under it would
	// leave the page without putting the keyboard away.
	back_btn_force_hidden(true);
}

static void build_rename_layer(gui_config_t *cfg) {
	rename_layer = lv_obj_create(btsettings_screen);
	lv_obj_set_size(rename_layer, lv_pct(100), lv_pct(100));
	lv_obj_align(rename_layer, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(rename_layer, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(rename_layer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(rename_layer, 0, 0);
	lv_obj_set_style_radius(rename_layer, 0, 0);
	lv_obj_set_style_pad_all(rename_layer, 0, 0);
	lv_obj_set_scrollable(rename_layer, false);
	lv_obj_set_hidden(rename_layer, true);

	lv_obj_t *heading = lv_label_create(rename_layer);
	lv_label_set_text(heading, tr("rename"));
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_24, 0);
	lv_obj_align(heading, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding + cfg->top_bar_height + 10);

	lv_obj_t *cancel = lv_btn_create(rename_layer);
	lv_obj_set_size(cancel, RENAME_CANCEL_SIZE, RENAME_CANCEL_SIZE);
	lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_pad_all(cancel, 0, 0);
	lv_obj_add_event_cb(cancel, rename_cancel_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *cancel_icon = lv_image_create(cancel);
	lv_image_set_src(cancel_icon, &icon_close);
	lv_obj_add_style(cancel_icon, &theme_style_icon, 0);
	lv_obj_center(cancel_icon);

	rename_field = lv_textarea_create(rename_layer);
	lv_textarea_set_one_line(rename_field, true);
	// One under bluez's own limit, so a name that fits the field is a name the
	// device keeps whole.
	lv_textarea_set_max_length(rename_field, BT_NAME_MAX - 1);
	lv_textarea_set_placeholder_text(rename_field, tr("name"));
	lv_obj_set_size(rename_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(rename_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(rename_field, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding + cfg->top_bar_height + 60);
	lv_obj_add_style(rename_field, &theme_style_card, 0);
	lv_obj_set_style_radius(rename_field, 12, 0);
	lv_obj_set_style_border_width(rename_field, 0, 0);
	lv_obj_set_style_shadow_width(rename_field, 0, 0);
	lv_obj_set_style_pad_all(rename_field, 14, 0);
	lv_obj_set_style_text_font(rename_field, &font_ui_24, 0);
	keyboard_style_caret(rename_field);

	rename_keyboard =
		keyboard_create(rename_layer, cfg->screen_width, 316, rename_field, NULL, "ok", rename_accept_cb, NULL);
}

static void row_free_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

static void remember_mac(const char *mac) { snprintf(menu_mac, sizeof(menu_mac), "%s", mac); }

static void show_device_menu(lv_obj_t *anchor, bool connected) {
	if (connected) {
		static const popover_item_t items[] = {
			{"disconnect", disconnect_action, NULL},
			{"rename", rename_action, NULL},
			{"bt_forget", forget_action, NULL},
		};
		popover_show(anchor, items, 3);
	} else {
		static const popover_item_t items[] = {
			{"connect", connect_action, NULL},
			{"rename", rename_action, NULL},
			{"bt_forget", forget_action, NULL},
		};
		popover_show(anchor, items, 3);
	}
}

// A tap on a paired device connects it. On the connected one a tap opens the
// menu instead, since connecting again would be a no-op; the same menu is
// reachable everywhere through the three dots or a long press.
static void paired_row_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}

	lv_obj_t *row = lv_event_get_current_target(e);
	const char *mac = lv_event_get_user_data(e);
	if (!mac || !mac[0]) {
		return;
	}
	remember_mac(mac);

	if (lv_obj_get_user_data(row) != NULL) {
		show_device_menu(row, true);
	} else {
		last_action = ACTION_CONNECT;
		bluetooth_connect(mac);
	}
}

static void paired_row_long_pressed_cb(lv_event_t *e) {
	lv_obj_t *row = lv_event_get_current_target(e);
	const char *mac = lv_event_get_user_data(e);
	if (!mac || !mac[0]) {
		return;
	}
	remember_mac(mac);
	show_device_menu(row, lv_obj_get_user_data(row) != NULL);
}

static void row_menu_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	const char *mac = lv_event_get_user_data(e);
	lv_obj_t *button = lv_event_get_current_target(e);
	if (!mac || !mac[0]) {
		return;
	}
	remember_mac(mac);
	// The connected flag lives in the user data of the row the button sits on,
	// and decides between a Disconnect and a Connect entry.
	lv_obj_t *row = lv_obj_get_parent(button);
	show_device_menu(button, row && lv_obj_get_user_data(row) != NULL);
}

static void found_row_clicked_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	const char *mac = lv_event_get_user_data(e);
	if (!mac || !mac[0] || pairing_watch_active()) {
		return;
	}

	// Look the name up before pairing starts: the pairing notice should name
	// the headphones, not their address.
	char name[BT_NAME_MAX] = "";
	bt_device_t devices[BT_MAX_DEVICES];
	int count = bluetooth_get_found(devices, BT_MAX_DEVICES);
	for (int i = 0; i < count; i++) {
		if (strcmp(devices[i].mac, mac) == 0) {
			snprintf(name, sizeof(name), "%s", devices[i].name);
			break;
		}
	}

	bluetooth_pair(mac);
	pairing_watch_start(mac, name);
}

// ---------------------------------------------------------------------------
// The lists
// ---------------------------------------------------------------------------

static void add_row(lv_obj_t *parent, const bt_device_t *device, bool paired_section) {
	char *mac_copy = strdup(device->mac);
	if (!mac_copy) {
		return;
	}

	lv_obj_t *row = lv_btn_create(parent);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, BT_ROW_H);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 18, 0);
	lv_obj_set_style_pad_ver(row, 0, 0);
	lv_obj_set_event_bubble(row, true);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 14, 0);

	lv_obj_set_user_data(row, device->connected ? (void *)1 : NULL);
	if (paired_section) {
		lv_obj_add_event_cb(row, paired_row_clicked_cb, LV_EVENT_CLICKED, mac_copy);
		lv_obj_add_event_cb(row, paired_row_long_pressed_cb, LV_EVENT_LONG_PRESSED, mac_copy);
	} else {
		lv_obj_add_event_cb(row, found_row_clicked_cb, LV_EVENT_CLICKED, mac_copy);
	}
	lv_obj_add_event_cb(row, row_free_cb, LV_EVENT_DELETE, mac_copy);

	lv_obj_t *icon = lv_image_create(row);
	lv_image_set_src(icon, &icon_bluetooth_status);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
	if (device->connected) {
		lv_obj_set_style_image_recolor(icon, theme()->accent, 0);
	} else if (!paired_section) {
		lv_obj_set_style_image_opa(icon, LV_OPA_60, 0);
	}

	// Name on a single line, without the MAC address under it: what matters is
	// the name and whether the device is connected.
	lv_obj_t *name = lv_label_create(row);
	lv_label_set_text(name, device->name);
	lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(name, 1);
	lv_obj_set_height(name, lv_font_get_line_height(&font_ui_24));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	if (device->connected) {
		lv_obj_t *state = lv_label_create(row);
		lv_label_set_text(state, tr("connected"));
		lv_obj_add_style(state, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(state, &font_ui_20, 0);
		lv_obj_set_style_text_color(state, theme()->accent, 0);
	}

	// Three dots on every known device, connected or merely paired: a tap on
	// the row connects, so disconnect and forget need somewhere else to live.
	if (paired_section) {
		lv_obj_t *menu_btn = lv_btn_create(row);
		lv_obj_set_size(menu_btn, 44, 56);
		lv_obj_set_style_bg_opa(menu_btn, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(menu_btn, 0, 0);
		lv_obj_set_style_shadow_width(menu_btn, 0, 0);
		lv_obj_set_style_pad_all(menu_btn, 0, 0);
		lv_obj_add_event_cb(menu_btn, row_menu_clicked_cb, LV_EVENT_CLICKED, mac_copy);

		lv_obj_t *dots = lv_image_create(menu_btn);
		lv_image_set_src(dots, &icon_ellipsis_vertical);
		lv_obj_add_style(dots, &theme_style_icon, 0);
		lv_obj_set_style_image_opa(dots, LV_OPA_70, 0);
		lv_obj_center(dots);
	}
}

// `spin` puts a spinner left of the text: without something moving, "nothing
// here" and "still searching" read the same.
static void add_placeholder(lv_obj_t *parent, const char *text, bool spin) {
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_remove_style_all(row);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(row, 4, 0);
	lv_obj_set_style_pad_column(row, 10, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	if (spin) {
		spinner_create(row, &icon_loader_small);
	}

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, tr(text));
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
}

static void rebuild_lists(void) {
	lv_obj_clean(paired_list);
	lv_obj_clean(found_list);

	bool on = bluetooth_get_enabled() && bluetooth_available();
	if (!on) {
		lv_obj_set_hidden(paired_label, true);
		lv_obj_set_hidden(found_label, true);
		return;
	}
	lv_obj_set_hidden(paired_label, false);

	bt_device_t devices[BT_MAX_DEVICES];

	int count = bluetooth_get_paired(devices, BT_MAX_DEVICES);
	if (count == 0) {
		add_placeholder(paired_list, "bt_none", false);
	}
	for (int i = 0; i < count; i++) {
		add_row(paired_list, &devices[i], true);
	}

	// The "other devices" heading appears only once a scan is running or has
	// found something: a heading over an empty box with an instruction under it
	// is three lines of furniture repeating what the corner button says.
	count = bluetooth_get_found(devices, BT_MAX_DEVICES);
	if (count == 0) {
		if (bluetooth_scan_running()) {
			lv_obj_set_hidden(found_label, false);
			add_placeholder(found_list, "searching", true);
		} else {
			lv_obj_set_hidden(found_label, true);
		}
		return;
	}

	lv_obj_set_hidden(found_label, false);
	for (int i = 0; i < count; i++) {
		add_row(found_list, &devices[i], false);
	}
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

static void switch_changed_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(bt_switch, LV_STATE_CHECKED);

	if (on && !bluetooth_available()) {
		lv_obj_remove_state(bt_switch, LV_STATE_CHECKED);
		gui_notify_popup("bt_bluetooth_not_available");
		return;
	}

	bluetooth_set_enabled(on);
	refresh_status_label();
	rebuild_lists();
	topbar_refresh_radios();
}

// The radio is up and answering, which is not the same as the switch being on:
// that is true from the moment it is tapped and stays true for the ten seconds
// the stack needs to load. A scan started in that window finds nothing and
// looks broken, so the scan button stays disabled until this holds.
static bool radio_ready(void) {
	return bluetooth_available() && bluetooth_get_enabled() && !bluetooth_busy();
}

// The page is a trap once it is receiving, so it is not somewhere to arrive by
// accident with the radio off: the two things it needs are said here rather
// than left for an empty page to imply.
static void receiver_clicked_cb(lv_event_t *e) {
	(void)e;
	if (!radio_ready()) {
		return;
	}
	// With headphones on the other end this device is the source, and receiver
	// mode is the opposite arrangement: the output is already taken and there is
	// nothing for a phone to stream into, so the page is not opened at all.
	if (bluetooth_audio_active()) {
		gui_notify_popup("bt_disconnect_headphones_first");
		return;
	}
	switch_screen(btreceiverpage_screen);
}

static void scan_clicked_cb(lv_event_t *e) {
	(void)e;
	if (!radio_ready()) {
		return;
	}
	if (bluetooth_scan_running()) {
		bluetooth_scan_stop();
	} else {
		bluetooth_scan_start();
	}
	refresh_status_label();
	rebuild_lists();
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;

	bool on = bluetooth_get_enabled();
	if (lv_obj_has_state(bt_switch, LV_STATE_CHECKED) != on) {
		if (on) {
			lv_obj_add_state(bt_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(bt_switch, LV_STATE_CHECKED);
		}
	}

	refresh_status_label();

	uint32_t serial = bluetooth_devices_serial();
	if (serial != drawn_serial || on != drawn_enabled) {
		drawn_serial = serial;
		drawn_enabled = on;
		rebuild_lists();
	}

	// Scan button: disabled while the stack is still loading, faded while a
	// sweep is running (tapping it then stops the sweep), solid otherwise.
	bool ready = radio_ready();
	if (scan_btn && receiver_btn) {
		if (ready) {
			lv_obj_remove_state(scan_btn, LV_STATE_DISABLED);
			lv_obj_remove_state(receiver_btn, LV_STATE_DISABLED);
		} else {
			lv_obj_add_state(scan_btn, LV_STATE_DISABLED);
			lv_obj_add_state(receiver_btn, LV_STATE_DISABLED);
		}
	}
	if (scan_icon) {
		lv_obj_set_style_image_opa(
			scan_icon, !ready ? LV_OPA_20 : (bluetooth_scan_running() ? LV_OPA_50 : LV_OPA_COVER), 0);
		lv_obj_set_style_image_opa(
			receiver_icon, !ready ? LV_OPA_20 : (bluetooth_scan_running() ? LV_OPA_50 : LV_OPA_COVER), 0);
	}

	// While a pairing is in flight its own watcher owns the result: consuming
	// it here too would mean whichever timer ran first swallowed it.
	if (pairing_watch_active()) {
		return;
	}

	char name[BT_NAME_MAX];
	switch (bluetooth_take_op_result(name, sizeof(name))) {
	case BT_OP_OK:
		switch (last_action) {
		case ACTION_CONNECT:
			toast_success("bt_device_connected");
			break;
		case ACTION_DISCONNECT:
			toast_success("bt_device_disconnected");
			break;
		case ACTION_FORGET:
			toast_success("bt_device_removed");
			break;
		default:
			toast_success("done");
			break;
		}
		last_action = ACTION_NONE;
		topbar_refresh_radios();
		break;
	case BT_OP_FAILED:
		last_action = ACTION_NONE;
		gui_notify_popup("bt_connect_failed");
		break;
	default:
		break;
	}
}

// The control centre switched the radio with the page open under it. The poll
// already follows the radio; this only saves waiting for its next round.
static void switched_elsewhere_cb(lv_event_t *e) {
	(void)e;
	if (poll_timer) {
		lv_timer_ready(poll_timer);
	}
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	drawn_serial = 0xFFFFFFFFu;
	lv_timer_resume(poll_timer);
	lv_timer_ready(poll_timer);
	bluetooth_refresh();
	// Visible to other devices only from here. Searching and being searched for
	// are the two halves of the same page: without this a phone looking for the
	// player finds nothing, whatever it does.
	bluetooth_set_discoverable(true);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	// A sheet left open would greet the next visit.
	if (rename_layer && !lv_obj_is_hidden(rename_layer)) {
		rename_layer_hide();
	}
	lv_timer_pause(poll_timer);
	bluetooth_set_discoverable(false);
	if (bluetooth_scan_running()) {
		bluetooth_scan_stop(); // a scan nobody is watching only costs battery
	}
}

// Corner buttons, laid out from the right edge as on the Music page.
static lv_obj_t *corner_button(lv_obj_t *screen, gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph,
							   lv_obj_t **icon_out) {
	lv_obj_t *button = lv_btn_create(screen);
	lv_obj_set_size(button, CORNER_BTN_SIZE, CORNER_BTN_SIZE);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (CORNER_BTN_SIZE + 6),
				 cfg->padding + cfg->top_bar_height);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	if (icon_out) {
		*icon_out = icon;
	}

	return button;
}

static lv_obj_t *make_section(lv_obj_t *container, const char *title, lv_obj_t **label_out) {
	lv_obj_t *label = lv_label_create(container);
	lv_label_set_text(label, tr(title));
	// Full width, otherwise the page's centring flex pulls the heading inward.
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_set_style_pad_top(label, 6, 0);
	lv_obj_set_style_pad_hor(label, 4, 0);
	if (label_out) {
		*label_out = label;
	}

	lv_obj_t *box = lv_obj_create(container);
	lv_obj_set_width(box, lv_pct(100));
	lv_obj_set_height(box, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(box, 0, 0);
	lv_obj_set_style_border_width(box, 0, 0);
	lv_obj_set_style_pad_all(box, 0, 0);
	lv_obj_set_style_pad_gap(box, 8, 0);
	lv_obj_set_scrollable(box, false);
	lv_obj_set_event_bubble(box, true);
	lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
	return box;
}

void btsettings_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(btsettings_screen, cfg, "bluetooth");
	settingsrow_title_corner_slots(settingsrow_page_title(btsettings_screen), cfg, 3);

	settingsrow_toggle(container, "bluetooth", &bt_switch, switch_changed_cb);
	if (bluetooth_get_enabled()) {
		lv_obj_add_state(bt_switch, LV_STATE_CHECKED);
	}

	status_label = lv_label_create(container);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_20, 0);
	lv_obj_set_style_pad_hor(status_label, 4, 0);
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);

	paired_list = make_section(container, "bt_paired_devices", &paired_label);
	found_list = make_section(container, "bt_other_devices", &found_label);
	lv_obj_set_style_pad_bottom(found_list, 12, 0);

	// The gear takes slot 0, the same corner the Music page uses for its own.
	// Receiver mode sits immediately to its left, and the scan button moves out
	// one place: the three read right to left in order of how often they are
	// wanted.
	lv_obj_t *settings_btn = corner_button(btsettings_screen, cfg, 0, &icon_music_settings, NULL);
	lv_obj_add_event_cb(settings_btn, switch_screen_cb, LV_EVENT_CLICKED, btaudio_screen);

	receiver_btn = corner_button(btsettings_screen, cfg, 1, &icon_bluetooth_receiver, &receiver_icon);
	lv_obj_add_event_cb(receiver_btn, receiver_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_state(receiver_btn, LV_STATE_DISABLED); // enabled by the poll once the radio answers
	lv_obj_set_style_image_opa(receiver_icon, LV_OPA_20, 0);

	scan_btn = corner_button(btsettings_screen, cfg, 2, &icon_bluetooth_search, &scan_icon);
	lv_obj_add_event_cb(scan_btn, scan_clicked_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_state(scan_btn, LV_STATE_DISABLED); // enabled by the poll once the radio answers
	lv_obj_set_style_image_opa(scan_icon, LV_OPA_20, 0);

	poll_timer = lv_timer_create(poll_cb, BT_PAGE_POLL_MS, NULL);
	lv_timer_pause(poll_timer);

	lv_obj_add_event_cb(btsettings_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(btsettings_screen, switched_elsewhere_cb, LV_EVENT_REFRESH, NULL);
	lv_obj_add_event_cb(btsettings_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(btsettings_screen);

	build_rename_layer(cfg);

	refresh_status_label();
	rebuild_lists();
}
