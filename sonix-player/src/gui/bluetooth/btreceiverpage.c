#include "btreceiverpage.h"

#include <stdio.h>
#include <string.h>

#include "src/gui/shell/confirm.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/bluetooth/btreceiver.h"
#include "src/system/core/lang.h"

lv_obj_t *btreceiverpage_screen;

#define POLL_MS 500
#define CODEC_BTN_SIZE 56

static lv_obj_t *big_icon;
static lv_obj_t *format_label;
static lv_obj_t *device_label;
static lv_obj_t *status_label;
static lv_obj_t *title_label;
static lv_timer_t *poll_timer;
static unsigned last_serial = (unsigned)-1;
static lv_color_t status_normal_color;

// ---------------------------------------------------------------------------

// "44100" -> "44,1 kHz". The rates that divide evenly lose the decimal: 48 kHz
// written as 48,0 reads like a measurement rather than a number everyone knows.
static void format_rate(unsigned rate, char *out, size_t size) {
	if (rate == 0) {
		snprintf(out, size, "--");
	} else if (rate % 1000 == 0) {
		snprintf(out, size, "%u kHz", rate / 1000);
	} else {
		snprintf(out, size, "%.1f kHz", rate / 1000.0);
	}
}

static void refresh(void) {
	btreceiver_state_t st;
	btreceiver_get_state(&st);

	lv_obj_set_style_text_color(status_label, status_normal_color, 0);

	lv_obj_add_flag(title_label, LV_OBJ_FLAG_HIDDEN);

	if (st.error[0]) {
		lv_label_set_text(device_label, st.device);
		lv_label_set_text(format_label, "");
		lv_label_set_text(status_label, st.error);
		lv_obj_set_style_text_color(status_label, lv_color_make(224, 27, 36), 0);
		return;
	}

	if (!st.active) {
		// Asked now, not remembered. The name the mode last ran with belongs to
		// the mode: with nothing streaming here it is the name of a phone that
		// has gone, and showing it would read as one that is arriving.
		char name[BT_NAME_MAX];
		bool source = bluetooth_receiver_device(NULL, 0, name, sizeof(name));
		lv_label_set_text(device_label, source ? name : "");
		lv_label_set_text(format_label, "");
		lv_label_set_text(status_label,
						  source ? tr("btreceiver_connecting") : tr("btreceiver_no_source_note"));
		return;
	}

	lv_label_set_text(device_label, st.device);

	// The codec and the rate, which is the whole of what this page is for: the
	// sender chose both and this is the only place the choice is visible.
	char rate[24];
	format_rate(st.sample_rate, rate, sizeof(rate));
	char text[64];
	if (st.codec[0]) {
		snprintf(text, sizeof(text), "%s  %s", st.codec, rate);
	} else {
		snprintf(text, sizeof(text), "%s", rate);
	}
	lv_label_set_text(format_label, text);

	// What the sender says it is playing, when it says anything. A phone
	// streaming from a music app names the track over AVRCP; a laptop sending
	// system audio usually names nothing, and then the line goes back to saying
	// what the stream is doing.
	bt_track_t track;
	if (st.streaming && bluetooth_receiver_track(&track) && track.title[0]) {
		lv_label_set_text(title_label, track.title);
		lv_obj_remove_flag(title_label, LV_OBJ_FLAG_HIDDEN);

		// Artist and album on one line, with whichever of them there is.
		char line[sizeof(track.artist) + sizeof(track.album) + 8];
		if (track.artist[0] && track.album[0]) {
			snprintf(line, sizeof(line), "%s  --  %s", track.artist, track.album);
		} else {
			snprintf(line, sizeof(line), "%s", track.artist[0] ? track.artist : track.album);
		}
		lv_label_set_text(status_label, line);
		return;
	}

	lv_obj_add_flag(title_label, LV_OBJ_FLAG_HIDDEN);
	lv_label_set_text(status_label, st.streaming ? tr("btreceiver_playing") : tr("btreceiver_waiting"));
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	if (lv_screen_active() != btreceiverpage_screen) {
		return;
	}
	// The codec can be renegotiated mid-stream and the Bluetooth serial moves
	// when it is, so both serials count. The stream falling silent counts too
	// and has no serial of its own: nothing runs when frames stop arriving, so
	// there is nobody to bump one. It is read here instead.
	btreceiver_state_t st;
	btreceiver_get_state(&st);
	// The metadata has its own counter, bumped by the sender's announcement
	// rather than by anything here, and it is carried in the receiver's stream
	// serial: without it a track change on the phone would not redraw the page.
	unsigned now = btreceiver_serial() + bluetooth_devices_serial() + bluetooth_receiver_stream_serial() +
				   (st.streaming ? 1u : 0u);
	if (now == last_serial) {
		return;
	}
	last_serial = now;
	refresh();
}

// ---------------------------------------------------------------------------
// Choosing the codec
//
// The sending device picks the codec when the link comes up, and on a phone
// that choice is buried in the developer options or not offered at all. What
// bluealsa exposes on the sink PCM is the same SelectCodec the headphone side
// uses, so the player can move the link itself.
//
// The list is whatever the two ends have in common on the link that is up.
// Nothing is remembered: the next connection negotiates again.
// ---------------------------------------------------------------------------

#define CODEC_MENU_MAX 6

static popover_item_t codec_items[CODEC_MENU_MAX];
static char codec_labels[CODEC_MENU_MAX][BT_CODEC_MAX];

static void codec_chosen(void *user) {
	const char *codec = user;
	if (codec && codec[0]) {
		bluetooth_receiver_set_codec(codec);
	}
}

static void codec_btn_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}

	char offered[BT_MAX_CODECS][BT_CODEC_MAX];
	char selected[BT_CODEC_MAX] = "";
	int count = bluetooth_receiver_codecs(offered, BT_MAX_CODECS, selected, sizeof(selected));
	if (count <= 0) {
		gui_notify_popup("btreceiver_no_codecs");
		return;
	}

	if (count > CODEC_MENU_MAX) {
		count = CODEC_MENU_MAX;
	}
	for (int i = 0; i < count; i++) {
		// Copied: the popover calls back after it has closed, so the strings it
		// was given have to still be there.
		memcpy(codec_labels[i], offered[i], sizeof(codec_labels[i]));
		codec_labels[i][sizeof(codec_labels[i]) - 1] = '\0';
		codec_items[i].label = codec_labels[i];
		codec_items[i].action = codec_chosen;
		codec_items[i].user = codec_labels[i];
		codec_items[i].checked = selected[0] && strcasecmp(selected, codec_labels[i]) == 0;
	}

	popover_show(lv_event_get_current_target(e), codec_items, count);
}

// ---------------------------------------------------------------------------

static bool leaving;

static void leave_async(void *user) {
	(void)user;
	back_btn_cb(NULL);
	leaving = false;
}

// Deferred by one turn of the event loop, as on the DAC page: this runs from
// inside the confirmation's own handler, and navigating out from under a dialog
// that is still closing leaves the screen change undone.
static void really_leave(void *user) {
	(void)user;
	btreceiver_stop();
	last_serial = (unsigned)-1;
	refresh();
	leaving = true;
	lv_async_call(leave_async, NULL);
}

// The back chevron and the swipe both come through here. True means "handled,
// stay put": leaving stops the audio, and that is worth one question.
static bool back_guard(void) {
	if (leaving || !btreceiver_is_active()) {
		return false;
	}
	confirm_show("btreceiver_leave_confirm", "btreceiver_leave_confirm_note", "leave", really_leave, NULL);
	return true;
}

// ---------------------------------------------------------------------------

static void loaded_cb(lv_event_t *e) {
	(void)e;
	last_serial = (unsigned)-1;
	// Arriving is the switch: there is nothing else this page does, so a toggle
	// on it would only repeat what opening it already said.
	btreceiver_start();
	refresh();
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	// Every other way out -- the power menu, a notification, a screen change
	// this page did not ask for -- ends the mode too. The guard above catches
	// the deliberate ones; this catches the rest, so the device is never left
	// holding a stream nobody can see.
	btreceiver_stop();
}

void btreceiverpage_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(btreceiverpage_screen, cfg, "btreceiver_title");

	// The codec button, in the corner every other page puts its options in.
	lv_obj_t *codec_btn = lv_btn_create(btreceiverpage_screen);
	lv_obj_set_size(codec_btn, CODEC_BTN_SIZE, CODEC_BTN_SIZE);
	lv_obj_set_style_bg_opa(codec_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(codec_btn, 0, 0);
	lv_obj_set_style_shadow_width(codec_btn, 0, 0);
	lv_obj_set_style_pad_all(codec_btn, 0, 0);
	lv_obj_align(codec_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_add_event_cb(codec_btn, codec_btn_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *codec_glyph = lv_image_create(codec_btn);
	lv_image_set_src(codec_glyph, &icon_change_codec);
	lv_obj_add_style(codec_glyph, &theme_style_icon, 0);
	lv_obj_center(codec_glyph);

	// Straight into the flex flow with no alignment of its own: the container
	// settingsrow_page() builds already centres its children, and an object
	// positioned by alignment leaves the flow and is measured as taking no
	// space.
	big_icon = lv_image_create(container);
	lv_image_set_src(big_icon, &icon_bluetooth_receiver_page);
	lv_obj_set_style_margin_top(big_icon, 40, 0);

	format_label = lv_label_create(container);
	lv_obj_set_width(format_label, lv_pct(100));
	lv_label_set_long_mode(format_label, LV_LABEL_LONG_WRAP);
	lv_obj_add_style(format_label, &theme_style_text, 0);
	lv_obj_set_style_text_align(format_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(format_label, &font_ui_24_bold, 0);
	lv_obj_set_style_margin_top(format_label, 20, 0);
	lv_label_set_text(format_label, "");

	device_label = lv_label_create(container);
	lv_obj_set_width(device_label, lv_pct(100));
	lv_label_set_long_mode(device_label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_align(device_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(device_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(device_label, &font_ui_22, 0);
	lv_obj_set_style_margin_top(device_label, 10, 0);
	lv_label_set_text(device_label, "");

	// The track the sender names, under the device it comes from. Hidden when
	// there is nothing to name, so a laptop's system audio does not leave an
	// empty row on the page.
	title_label = lv_label_create(container);
	lv_obj_set_width(title_label, lv_pct(100));
	lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(title_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(title_label, &font_ui_24, 0);
	lv_obj_set_style_margin_top(title_label, 18, 0);
	lv_label_set_text(title_label, "");
	lv_obj_add_flag(title_label, LV_OBJ_FLAG_HIDDEN);

	status_label = lv_label_create(container);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_22, 0);
	lv_obj_set_style_margin_top(status_label, 10, 0);
	lv_label_set_text(status_label, "");
	status_normal_color = lv_obj_get_style_text_color(status_label, LV_PART_MAIN);

	lv_obj_add_event_cb(btreceiverpage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(btreceiverpage_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_set_back_guard(btreceiverpage_screen, back_guard);

	poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
	(void)poll_timer;
}
