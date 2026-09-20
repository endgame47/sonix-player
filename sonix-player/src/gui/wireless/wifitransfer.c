#include "wifitransfer.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/shell/confirm.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/wireless/wifisettings.h"
#include "src/system/audio/audio.h"
#include "src/system/core/lang.h"
#include "src/system/net/wifi.h"
#include "src/system/device/led.h"
#include "src/system/net/wifitransfer.h"

lv_obj_t *wifitransfer_screen;

// Half a second: the page has a state that lasts only as long as the server
// takes to come up, and a "turning on" message that lingers after thttpd is
// already listening reads as a slow player.
#define WT_POLL_MS 500

// The stock player puts its server back every fifteen seconds if it has died
// (it runs cgic_deamon on that cadence). Same interval, counted in ticks.
#define WT_WATCHDOG_TICKS 30

// How long the link has to be gone before this page believes it.
//
// One sample is not enough: `wpa_cli status` reports SCANNING while a perfectly
// good link does a background scan, and reports nothing at all when the read
// loses a race with a busy supplicant, and either would tear the server down
// with an upload in flight. The question asked is whether wlan0 has an address
// -- which is what a server needs -- and it has to answer no for three seconds
// running before anything is switched off.
#define WT_LINK_GRACE_TICKS 6

static lv_obj_t *toggle;
static lv_obj_t *status_label;
static lv_obj_t *url_label;
static lv_obj_t *addr_label;
static lv_obj_t *warn1_label;
static lv_obj_t *warn2_label;
static lv_obj_t *wifi_row;
static lv_timer_t *poll_timer;

static int watchdog_ticks;

// True from the moment the confirmation is accepted until the page is gone, so
// the back guard does not ask a second time on the way out.
static bool leaving;

// Keys taken verbatim from the stock firmware's own strings for this page
// (/usr/resource/str/italy/wifi_song.ini), which are a better source than a
// fresh translation.
#define WT_HELP "wifitransfer_address_note"
#define WT_WARN1 "wifitransfer_same_network_note"
#define WT_WARN2 "wifitransfer_keep_open_note"
#define WT_STOP_PLAY "wifitransfer_stops_playback"

// ---------------------------------------------------------------------------

static int link_missing_ticks;

// The kernel's answer: an ioctl, no fork, no flap. What it does NOT say is
// whether the radio is on -- an interface switched off keeps the address it was
// given until something takes it away, and on this device nothing does, so this
// alone would show a live server with the Wi-Fi off.
static bool wifi_has_address(void) { return wifi_interface_address(NULL, 0); }

// What every other wireless page asks, and the reason they all get this right:
// AirPlay, DLNA and SonixLink decide by this and nothing else.
static bool wifi_is_connected(void) {
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_CONNECTED && status.ip[0] != '\0';
}

// The radio is off. Unlike everything else in here this is not a poll's
// opinion: the player writes it itself the moment the user switches Wi-Fi off
// (see set_enabled() in system/wifi.c), so there is nothing to wait and see
// about.
static bool wifi_is_off(void) {
	wifi_status_t status;
	wifi_get_status(&status);
	return status.state == WIFI_STATE_OFF;
}

// One poll's worth of evidence, and the only thing that writes the counter.
//
// Two questions are being answered at once here, which is why it reads the way
// it does. Is there a network? -- the supplicant, same as the other three
// pages, so one device never shows two answers. Is it safe to tear down a
// running server? -- that one needs the three seconds, because `wpa_cli status`
// says SCANNING during a background scan of a link that is carrying an upload
// just fine.
static void link_note(void) {
	if (wifi_is_off()) {
		link_missing_ticks = WT_LINK_GRACE_TICKS; // believed at once
		return;
	}
	if (wifi_is_connected() || wifi_has_address()) {
		link_missing_ticks = 0;
		return;
	}
	if (link_missing_ticks < WT_LINK_GRACE_TICKS) {
		link_missing_ticks++;
	}
}

// The debounced version, and the only one anything acts on.
static bool wifi_link_is_up(void) { return link_missing_ticks < WT_LINK_GRACE_TICKS; }

static bool switch_is_on(void) { return lv_obj_has_state(toggle, LV_STATE_CHECKED); }

static void hide(lv_obj_t *obj) { lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN); }
static void show(lv_obj_t *obj) { lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN); }

// lv_label_set_text reallocates and invalidates whether or not the text
// changed, so setting the same five labels twice a second would redraw the
// whole page twice a second -- the address at font_ui_32 included -- for
// nothing.
static void set_text(lv_obj_t *label, const char *text) {
	const char *shown = lv_label_get_text(label);
	if (shown && strcmp(shown, text) == 0) {
		return;
	}
	lv_label_set_text(label, text);
}

// Everything below the switch, from the state the page is actually in. Cheap
// enough to run on every tick: it is four labels and a row.
static void refresh(void) {
	if (!wifitransfer_available()) {
		set_text(status_label, tr("wifitransfer_unavailable"));
		lv_obj_add_state(toggle, LV_STATE_DISABLED);
		hide(url_label);
		hide(addr_label);
		hide(warn1_label);
		hide(warn2_label);
		hide(wifi_row);
		return;
	}

	if (!wifi_link_is_up()) {
		// The stock player says this in a modal with a settings button; saying
		// what is missing and offering the way there does the same job without a
		// dialog to dismiss. Same wording as every other network-backed page.
		set_text(status_label, tr("enable_wi_fi_first"));
		lv_obj_add_state(toggle, LV_STATE_DISABLED);
		hide(url_label);
		hide(addr_label);
		hide(warn1_label);
		hide(warn2_label);
		show(wifi_row);
		return;
	}

	lv_obj_remove_state(toggle, LV_STATE_DISABLED);
	hide(wifi_row);

	if (!switch_is_on()) {
		set_text(status_label, tr("wifitransfer_off_note"));
		hide(url_label);
		hide(addr_label);
		hide(warn1_label);
		hide(warn2_label);
		return;
	}

	// On, but the server takes a moment: the first time it is switched on it
	// also copies the web page into the writable partition. Until thttpd is
	// actually listening there is no point printing an address for it.
	if (!wifitransfer_running()) {
		set_text(status_label, tr("turning_on"));
		hide(url_label);
		hide(addr_label);
		hide(warn1_label);
		hide(warn2_label);
		return;
	}

	// The name first, the address under it. Both work; the name is the one worth
	// typing, and the address is the one that works from an Android browser,
	// which cannot resolve a .local name at all.
	char name[80];
	char address[80];
	wifitransfer_url(name, sizeof(name));
	wifitransfer_address(address, sizeof(address));

	set_text(status_label, tr(WT_HELP));

	if (name[0]) {
		set_text(url_label, name);
		if (address[0]) {
			lv_label_set_text_fmt(addr_label, tr("wifitransfer_or"), address);
			show(addr_label);
		} else {
			hide(addr_label);
		}
	} else {
		set_text(url_label, address[0] ? address : "--");
		hide(addr_label);
	}

	set_text(warn1_label, tr(WT_WARN1));
	set_text(warn2_label, tr(WT_WARN2));
	show(url_label);
	show(warn1_label);
	show(warn2_label);
}

// ---------------------------------------------------------------------------
// switching it on and off
// ---------------------------------------------------------------------------

// The stock player stops playback outright, closing the decoder and clearing
// the now-playing state. Pausing is the audible half of that and keeps the
// track loaded, so playback resumes with one tap.
//
// It happens at all because the browser can delete or overwrite the file being
// played: the card is about to be written by a root process outside this one.
static void pause_playback_and_warn(void) {
	if (audio_get_status() != AUDIO_STATUS_PLAYING) {
		return;
	}

	player_key_play_pause(); // through the player, so the sheet and the bar follow
	gui_notify_popup(WT_STOP_PLAY);
}

static void set_transfer(bool on) {
	if (on) {
		// The page is written out when the server starts, so the colour has to
		// be handed over first. Read fresh each time: the accent can have been
		// changed in Aspetto since the last time this page was opened.
		lv_color_t accent = theme()->accent;
		wifitransfer_set_accent(((uint32_t)accent.red << 16) | ((uint32_t)accent.green << 8) | accent.blue);
	}

	wifitransfer_set_enabled(on);
	watchdog_ticks = 0;

	// The light follows the switch straight away. Left to the status bar's own
	// poll it would be up to five seconds behind, which on a toggle reads as the
	// toggle not having worked.
	led_set_wifi_transfer(on);

	// While the server is up the radio is carrying files, and every status poll
	// is a fork of the whole player. The bars can lag.
	wifi_set_status_poll_slow(on);

	if (on) {
		pause_playback_and_warn();
	}
	refresh();
}

static void toggle_changed_cb(lv_event_t *e) {
	(void)e;

	bool on = switch_is_on();

	if (on && !wifitransfer_available()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		gui_notify_popup("wifitransfer_unavailable");
		return;
	}
	if (on && !wifi_is_connected()) {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		gui_notify_popup("enable_wi_fi_first");
		return;
	}

	set_transfer(on);
}

// ---------------------------------------------------------------------------
// leaving
// ---------------------------------------------------------------------------

// Deferred by one turn of the event loop. Calling back_btn_cb() straight from
// here runs it inside the confirmation dialog's own handler, and the screen
// change is then undone as the dialog finishes closing over it, which makes
// confirming a no-op.
static void leave_async(void *user) {
	(void)user;
	back_btn_cb(NULL); // the guard lets it through now
	leaving = false;
}

static void confirm_leave_cb(void *user) {
	(void)user;

	leaving = true;
	lv_obj_remove_state(toggle, LV_STATE_CHECKED);
	set_transfer(false);
	lv_async_call(leave_async, NULL);
}

// The chevron, while the server is up. Leaving the page takes the server with
// it, so an upload in progress dies: ask first.
static bool back_guard(void) {
	if (leaving || !switch_is_on()) {
		return false;
	}

	confirm_show("wifitransfer_leave_transfer", "wifitransfer_off_confirm_note", "wifitransfer_turn_off",
				 confirm_leave_cb, NULL);
	return true;
}

// ---------------------------------------------------------------------------

static void poll_cb(lv_timer_t *timer) {
	(void)timer;

	// Before the repaint, so the page answers on the same tick that decides.
	bool was_up = wifi_link_is_up();
	link_note();

	refresh();

	if (!switch_is_on()) {
		return;
	}

	// The network went away under a running server: there is nothing left to
	// serve and no address to print, so hand the port back.
	if (!wifi_link_is_up()) {
		if (was_up) {
			printf("wifitransfer: %s; switching the server off\n",
				   wifi_is_off() ? "the radio has been switched off" : "wlan0 has had no network for three seconds");
		}
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
		set_transfer(false);
		return;
	}

	if (++watchdog_ticks >= WT_WATCHDOG_TICKS) {
		watchdog_ticks = 0;
		wifitransfer_watchdog();
	}
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;

	leaving = false;
	watchdog_ticks = 0;

	// Answered straight away on arrival rather than after the grace period. The
	// three seconds exist to keep one bad sample from tearing down a running
	// server; nothing is running when the page opens, so there is nothing to
	// protect and every reason not to show a live-looking page for three
	// seconds to someone with the Wi-Fi off.
	link_missing_ticks = (wifi_is_connected() || wifi_has_address()) ? 0 : WT_LINK_GRACE_TICKS;

	// The switch says what the server is doing, not what it was doing last time
	// this page was open: something else may have taken it down in between.
	if (wifitransfer_get_enabled() && wifitransfer_available()) {
		lv_obj_add_state(toggle, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(toggle, LV_STATE_CHECKED);
	}

	refresh();
	lv_timer_resume(poll_timer);
	lv_timer_ready(poll_timer);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(poll_timer);

	// Whatever route was taken off this page -- the confirmed chevron, a jump
	// to the Wi-Fi settings, the player's own navigation -- the server does not
	// outlive the page. An unauthenticated file server running as root is not
	// something to leave behind by accident.
	if (wifitransfer_get_enabled()) {
		wifitransfer_set_enabled(false);
		led_set_wifi_transfer(false);
	}
	wifi_set_status_poll_slow(false);
}

static void wifi_row_cb(lv_event_t *e) {
	(void)e;
	switch_screen(wifisettings_screen);
}

// The address is painted in the accent colour once, when the page is built, so
// like every other accent-coloured widget it hooks the theme refresh pass; an
// accent changed afterwards would otherwise stay stale until reboot.
static void refresh_accent(void) {
	if (url_label) {
		lv_obj_set_style_text_color(url_label, theme()->accent, 0);
	}
}

// ---------------------------------------------------------------------------

void wifitransfer_page_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(wifitransfer_screen, cfg, "transfer");

	settingsrow_toggle(container, "wifitransfer_title", &toggle, toggle_changed_cb);

	status_label = lv_label_create(container);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(status_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_22, 0);
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_hor(status_label, 4, 0);
	lv_obj_set_style_pad_top(status_label, 14, 0);

	// The address, which is the whole point of the page: big, centred, and in
	// the accent colour, the way the stock page prints it in blue.
	url_label = lv_label_create(container);
	lv_obj_set_width(url_label, lv_pct(100));
	lv_label_set_long_mode(url_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(url_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(url_label, &font_ui_32, 0);
	lv_obj_set_style_text_color(url_label, theme()->accent, 0);
	lv_obj_set_style_text_align(url_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_ver(url_label, 16, 0);

	// The address, quieter and under the name: a fallback, not a second offer.
	addr_label = lv_label_create(container);
	lv_obj_set_width(addr_label, lv_pct(100));
	lv_label_set_long_mode(addr_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(addr_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(addr_label, &font_ui_20, 0);
	lv_obj_set_style_text_align(addr_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_bottom(addr_label, 14, 0);
	hide(addr_label);

	warn1_label = lv_label_create(container);
	lv_obj_set_width(warn1_label, lv_pct(100));
	lv_label_set_long_mode(warn1_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(warn1_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(warn1_label, &font_ui_20, 0);
	lv_obj_set_style_text_align(warn1_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_hor(warn1_label, 4, 0);

	warn2_label = lv_label_create(container);
	lv_obj_set_width(warn2_label, lv_pct(100));
	lv_label_set_long_mode(warn2_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(warn2_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(warn2_label, &font_ui_20, 0);
	lv_obj_set_style_text_align(warn2_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_hor(warn2_label, 4, 0);
	lv_obj_set_style_pad_top(warn2_label, 8, 0);

	// The way out of the "no Wi-Fi" state, which is the only actionable thing
	// this page ever offers besides its own switch.
	wifi_row = settingsrow_add(container, "wi_fi_settings", NULL, wifi_row_cb, NULL);
	lv_obj_set_style_margin_top(wifi_row, 16, 0);
	hide(wifi_row);

	poll_timer = lv_timer_create(poll_cb, WT_POLL_MS, NULL);
	lv_timer_pause(poll_timer);

	lv_obj_add_event_cb(wifitransfer_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(wifitransfer_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	// Deliberately no swipe-back on this page: a page that is also the switch
	// for a server should not be left by a gesture that can happen while
	// scrolling. The chevron is the way out, and it asks first.
	switcher_set_back_guard(wifitransfer_screen, back_guard);

	theme_register_refresh(refresh_accent);

	refresh();
}
