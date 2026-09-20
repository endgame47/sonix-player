#include "btaudio.h"

#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/bluetooth/airpodspage.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/device/power.h"
#include "src/system/bluetooth/airpods.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/audio/audio.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

lv_obj_t *btaudio_screen;

#define BTAUDIO_POLL_MS 700

static lv_obj_t *volume_switch;
static lv_obj_t *airpods_row;
static lv_obj_t *airpods_icon;
static lv_obj_t *codec_row;
static lv_obj_t *codec_value;
static lv_timer_t *poll_timer;

// The codec list is its own page: how many entries there are depends on what
// the headphones answered with, so the rows are built every time it opens.
static lv_obj_t *codec_screen;
static lv_obj_t *codec_list;

// ---------------------------------------------------------------------------
// switching the output over
// ---------------------------------------------------------------------------

// The ALSA device is only read when a stream is opened, so a change made
// while something is playing would not be heard until the next track. Rather
// than reach into the playback thread, the track is reloaded where it stands:
// the same path a normal track change takes, and one it is known to survive.
static void restart_playback_if_running(void) {
	char file[512];
	audio_get_current_file(file, sizeof(file));
	if (!file[0]) {
		return;
	}

	audio_status_t status = audio_get_status();
	if (status == AUDIO_STATUS_STOPPED) {
		return;
	}

	double position = 0;
	double total = 0;
	audio_get_progress(&position, &total);

	audio_play(file);
	audio_seek(position);
	if (status == AUDIO_STATUS_PAUSED) {
		audio_pause();
	}
	player_refresh_now_playing();
}

// ---------------------------------------------------------------------------
// The codec list
//
// What the two ends have in common on the link that is up, and nothing when
// nothing is connected. There is no preference to set in advance: the player
// reads what the headphones offer the moment they connect and takes the best of
// it by itself, so a list shown with nothing on the other end would be a list
// of things to choose that nothing would come of.
//
// Touching a row still moves the link onto that codec, for as long as that link
// lasts. It is a way to hear the difference, not a setting.
//
// LDAC appears three times because its bit rate is a separate setting from the
// codec itself -- 990 and 660 fixed, and adaptive, which lets the rate follow
// the link. The stock player lays them out the same way.
// ---------------------------------------------------------------------------

#define CODEC_ROWS (BT_MAX_CODECS + 3)

typedef struct {
	char label[BT_CODEC_MAX + 8]; // the row's name: "LDAC 990"
	char codec[BT_CODEC_MAX];	  // what to ask bluealsa for
	const char *ldac;			  // the quality that goes with it, NULL for the rest
} codec_row_t;

static codec_row_t codec_rows[CODEC_ROWS];
static int codec_row_count;

static void codec_chosen_cb(lv_event_t *e) {
	const codec_row_t *row = lv_event_get_user_data(e);
	if (!row) {
		return;
	}
	if (row->ldac) {
		bluetooth_set_ldac_quality(row->ldac);
	}
	bluetooth_set_codec(row->codec);
	switch_screen(btaudio_screen);
}

// Best first, which is the order the automatic choice walks and the order
// anybody reading the list expects.
static int codec_order(const char *name) {
	if (strcasecmp(name, "LDAC") == 0) {
		return 50;
	}
	if (strcasecmp(name, "aptX-HD") == 0) {
		return 40;
	}
	if (strcasecmp(name, "aptX") == 0) {
		return 30;
	}
	if (strcasecmp(name, "AAC") == 0) {
		return 20;
	}
	if (strcasecmp(name, "SBC") == 0) {
		return 10;
	}
	return 5; // something this build has not heard of: after the known ones
}

static void build_codec_rows(char mine[][BT_CODEC_MAX], int count) {
	// Sorted in place: five entries, so the simplest sort there is.
	for (int i = 0; i < count; i++) {
		for (int j = i + 1; j < count; j++) {
			if (codec_order(mine[j]) > codec_order(mine[i])) {
				char swap[BT_CODEC_MAX];
				snprintf(swap, sizeof(swap), "%s", mine[i]);
				snprintf(mine[i], BT_CODEC_MAX, "%s", mine[j]);
				snprintf(mine[j], BT_CODEC_MAX, "%s", swap);
			}
		}
	}

	codec_row_count = 0;
	for (int i = 0; i < count && codec_row_count < CODEC_ROWS; i++) {
		if (strcasecmp(mine[i], "LDAC") == 0) {
			static const struct {
				const char *suffix;
				const char *quality;
			} LDAC_MODES[] = {{" 990", "high"}, {" 660", "standard"}, {"", "abr"}};
			for (size_t m = 0; m < sizeof(LDAC_MODES) / sizeof(LDAC_MODES[0]) && codec_row_count < CODEC_ROWS; m++) {
				codec_row_t *row = &codec_rows[codec_row_count++];
				snprintf(row->label, sizeof(row->label), "LDAC%s", LDAC_MODES[m].suffix);
				snprintf(row->codec, sizeof(row->codec), "LDAC");
				row->ldac = LDAC_MODES[m].quality;
			}
			continue;
		}
		codec_row_t *row = &codec_rows[codec_row_count++];
		// By length rather than with %s: the compiler cannot see that each row
		// of the table is terminated, and warns that a name could run on into
		// the next one.
		size_t length = strnlen(mine[i], BT_CODEC_MAX - 1);
		memcpy(row->label, mine[i], length);
		row->label[length] = '\0';
		memcpy(row->codec, mine[i], length);
		row->codec[length] = '\0';
		row->ldac = NULL;
	}
}

static void codec_list_message(const char *tag) {
	lv_obj_t *none = lv_label_create(codec_list);
	lv_label_set_text(none, tr(tag));
	lv_obj_add_style(none, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(none, &font_ui_22, 0);
	lv_obj_set_width(none, lv_pct(100));
	lv_label_set_long_mode(none, LV_LABEL_LONG_WRAP);
}

static void rebuild_codec_list(void) {
	lv_obj_clean(codec_list);
	codec_row_count = 0;

	char offered[BT_MAX_CODECS][BT_CODEC_MAX];
	char selected[BT_CODEC_MAX] = "";
	int offered_count = bluetooth_get_codecs(offered, BT_MAX_CODECS, selected, sizeof(selected));

	if (!bluetooth_audio_active()) {
		codec_list_message("btaudio_no_device");
		return;
	}
	if (offered_count <= 0) {
		// Connected, and bluealsa has not answered yet or the sink named nothing
		// this build understands. Either way there is nothing to offer.
		codec_list_message("btaudio_no_codecs");
		return;
	}

	build_codec_rows(offered, offered_count);

	// One mark, for the one thing there is to say: which codec the link is
	// actually on. There is no second state to show -- nothing is remembered,
	// so nothing can be set and not yet in force.
	const char *quality = bluetooth_ldac_quality();

	for (int i = 0; i < codec_row_count; i++) {
		codec_row_t *row = &codec_rows[i];
		bool ldac_matches = !row->ldac || strcmp(row->ldac, quality) == 0;
		bool in_force = selected[0] && strcasecmp(selected, row->codec) == 0 && ldac_matches;

		lv_obj_t *value = NULL;
		settingsrow_add(codec_list, row->label, &value, codec_chosen_cb, row);

		lv_label_set_text(value, in_force ? tr("in_use") : "");
		lv_obj_set_style_text_color(value, theme()->accent, 0);
	}
}

static void codec_screen_loaded_cb(lv_event_t *e) {
	(void)e;
	bluetooth_refresh_codecs();
	rebuild_codec_list();
}

static void codec_row_cb(lv_event_t *e) {
	(void)e;
	switch_screen(codec_screen);
}

// ---------------------------------------------------------------------------
// the page itself
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The AirPods row
//
// It exists only while a pair is actually connected, so the page is one row
// shorter with anything else on the other end. The picture at its right is the
// model that is in the room, worked out from what the headphones themselves
// say (see airpods.h); it sits to the left of the chevron, in the space a
// value would use on any other row.
// ---------------------------------------------------------------------------

#define AIRPODS_ICON_INSET 48 // clears the chevron at the row's right edge

static void airpods_row_cb(lv_event_t *e) {
	(void)e;
	switch_screen(airpodspage_screen);
}

static void refresh_airpods_row(void) {
	airpods_state_t state;
	bool present = airpods_get(&state);

	if (present) {
		lv_obj_remove_flag(airpods_row, LV_OBJ_FLAG_HIDDEN);
		lv_image_set_src(airpods_icon, airpodspage_model_icon(state.model));
		// The product name, not the tag: "AirPods Pro 2" rather than "AirPods",
		// so the row says which pair is on the other end.
		lv_label_set_text(settingsrow_name_label(airpods_row), state.name[0] ? state.name : tr("airpods"));
	} else {
		lv_obj_add_flag(airpods_row, LV_OBJ_FLAG_HIDDEN);
	}
}

static void volume_toggle_cb(lv_event_t *e) {
	(void)e;
	bluetooth_set_volume_sync(lv_obj_has_state(volume_switch, LV_STATE_CHECKED));
}

static void refresh_name_row(void);

static void refresh_page(void) {
	// The name is read from the firmware's file at startup and can be changed
	// from this page, so it is re-read here rather than written once.
	refresh_name_row();

	bt_device_t device;
	bool connected = bluetooth_connected_device(&device);

	char selected[BT_CODEC_MAX];
	char codecs[BT_MAX_CODECS][BT_CODEC_MAX];
	int count = bluetooth_get_codecs(codecs, BT_MAX_CODECS, selected, sizeof(selected));

	lv_label_set_text(codec_value, connected && selected[0] ? selected : "--");

	// Only while something is connected: the list is what the other end offers,
	// so with nothing on the other end there is nothing behind the row.
	settingsrow_chevron_active(codec_row, connected && count > 0);

	refresh_airpods_row();

	bool sync = bluetooth_volume_sync();
	if (lv_obj_has_state(volume_switch, LV_STATE_CHECKED) != sync) {
		if (sync) {
			lv_obj_add_state(volume_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_remove_state(volume_switch, LV_STATE_CHECKED);
		}
	}
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	refresh_page();
}

// Watches where the sound is going and reloads the track when that changes --
// headphones connecting mid-song, or walking out of range. Runs whether or not
// this page is open, because that is when it matters.
//
// Once a second while the device is in use, and once every five with the panel
// dark and nothing playing: there is no stream for a routing change to spoil
// then, and a slower tick leaves the main loop asleep in standby.
#define ROUTING_WATCH_MS 1000
#define ROUTING_WATCH_IDLE_MS 5000

static void routing_watch_cb(lv_timer_t *timer) {
	bool matters = power_screen_is_on() || audio_get_status() == AUDIO_STATUS_PLAYING;
	lv_timer_set_period(timer, matters ? ROUTING_WATCH_MS : ROUTING_WATCH_IDLE_MS);

	char now[160];
	audio_get_output_device(now, sizeof(now));
	bool now_bt = strncmp(now, "bluealsa", 8) == 0;

	// The volume profile first, and driven from where the sound is actually
	// going rather than from the change detection below, so a player started
	// with the headphones already connected also gets the right profile. Safe
	// on every tick: alsa-controls holds every level and knows which one is
	// current, and a call that changes nothing returns at once.
	//
	// Ordering matters on connection: btvolume waits for this swap and then
	// reads the level the headphones are actually at, so the player adopts
	// theirs rather than pushing its own.
	//
	// The PCM name answers for Bluetooth and USB-C; for the two holes in the
	// side of the player only the jack detect can, because they are the same
	// ALSA device and differ in the route.
	volume_output_t out;
	if (now_bt) {
		out = VOLUME_OUTPUT_BLUETOOTH;
	} else if (strncmp(now, "plughw:", 7) == 0) {
		out = VOLUME_OUTPUT_USB;
	} else {
		out = headphone_jack_state() == JACK_BALANCED ? VOLUME_OUTPUT_BALANCED : VOLUME_OUTPUT_PHONES;
	}
	volume_profile_set_output(out);

	static char known[160];
	if (!known[0]) {
		snprintf(known, sizeof(known), "%s", now); // first look: nothing to reload
		return;
	}
	if (strcmp(known, now) == 0) {
		return;
	}

	bool was_bt = strncmp(known, "bluealsa", 8) == 0;
	snprintf(known, sizeof(known), "%s", now);
	printf("btaudio: output moved to '%s'; reloading the track\n", now);

	// Leaving the headphones is a disconnection, whichever end caused it: they
	// walked out of range, their battery went, or the radio was switched off.
	// Either way nobody is listening any more, and the reload below is what
	// would otherwise carry the album on out of the jack. Pause first: the
	// reload keeps whatever state it finds.
	if (was_bt && !now_bt) {
		player_output_unplugged("the Bluetooth headphones");
	}

	restart_playback_if_running();
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	bluetooth_refresh_codecs();
	refresh_page();
	lv_timer_resume(poll_timer);
}

static void name_layer_hide(void);

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	// Leaving the page puts the rename sheet away: coming back to a keyboard
	// nobody asked for is a page that looks stuck.
	name_layer_hide();
	lv_timer_pause(poll_timer);
}

// The player's own Bluetooth name, and the sheet that changes it: a field and a
// keyboard over the page, the same shape the preset and playlist names use.
#define NAME_CANCEL_SIZE 56

static lv_obj_t *name_row_value;
static lv_obj_t *name_layer;
static lv_obj_t *name_field;
static keyboard_t *name_keyboard;

// ---------------------------------------------------------------------------
// the name
// ---------------------------------------------------------------------------

static void refresh_name_row(void) {
	if (name_row_value) {
		lv_label_set_text(name_row_value, bluetooth_local_name());
	}
}

static void name_layer_hide(void) {
	if (name_layer) {
		lv_obj_add_flag(name_layer, LV_OBJ_FLAG_HIDDEN);
	}
	back_btn_force_hidden(false);
}

static void name_cancel_cb(lv_event_t *e) {
	(void)e;
	name_layer_hide();
}

static void name_accept_cb(lv_event_t *e) {
	(void)e;

	const char *typed = lv_textarea_get_text(name_field);
	if (!bluetooth_set_local_name(typed ? typed : "")) {
		gui_notify_popup("name_required");
		return;
	}

	name_layer_hide();
	refresh_name_row();
	toast_success("bt_name_changed");
}

static void name_row_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active() || !name_layer) {
		return;
	}
	lv_textarea_set_text(name_field, bluetooth_local_name());
	keyboard_reset(name_keyboard);
	lv_obj_remove_flag(name_layer, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(name_layer);
	// The sheet has its own close in the corner, and the chevron under it would
	// leave the page without putting the keyboard away.
	back_btn_force_hidden(true);
}

static void build_name_layer(gui_config_t *cfg) {
	name_layer = lv_obj_create(btaudio_screen);
	lv_obj_set_size(name_layer, lv_pct(100), lv_pct(100));
	lv_obj_align(name_layer, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(name_layer, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(name_layer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(name_layer, 0, 0);
	lv_obj_set_style_radius(name_layer, 0, 0);
	lv_obj_set_style_pad_all(name_layer, 0, 0);
	lv_obj_remove_flag(name_layer, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(name_layer, LV_OBJ_FLAG_HIDDEN);

	lv_obj_t *heading = lv_label_create(name_layer);
	lv_label_set_text(heading, tr("bt_rename"));
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_24, 0);
	lv_obj_align(heading, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding + cfg->top_bar_height + 10);

	lv_obj_t *cancel = lv_btn_create(name_layer);
	lv_obj_set_size(cancel, NAME_CANCEL_SIZE, NAME_CANCEL_SIZE);
	lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cancel, 0, 0);
	lv_obj_set_style_shadow_width(cancel, 0, 0);
	lv_obj_set_style_pad_all(cancel, 0, 0);
	lv_obj_add_event_cb(cancel, name_cancel_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *cancel_icon = lv_image_create(cancel);
	lv_image_set_src(cancel_icon, &icon_close);
	lv_obj_add_style(cancel_icon, &theme_style_icon, 0);
	lv_obj_center(cancel_icon);

	name_field = lv_textarea_create(name_layer);
	lv_textarea_set_one_line(name_field, true);
	// One under the adapter's own limit, so a name that fits the field is a
	// name the adapter will take whole.
	lv_textarea_set_max_length(name_field, BT_NAME_MAX - 1);
	lv_textarea_set_placeholder_text(name_field, tr("name"));
	lv_obj_set_size(name_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(name_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(name_field, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding + cfg->top_bar_height + 60);
	lv_obj_add_style(name_field, &theme_style_card, 0);
	lv_obj_set_style_radius(name_field, 12, 0);
	lv_obj_set_style_border_width(name_field, 0, 0);
	lv_obj_set_style_shadow_width(name_field, 0, 0);
	lv_obj_set_style_pad_all(name_field, 14, 0);
	lv_obj_set_style_text_font(name_field, &font_ui_24, 0);
	keyboard_style_caret(name_field);

	name_keyboard = keyboard_create(name_layer, cfg->screen_width, 316, name_field, NULL, "ok", name_accept_cb, NULL);
}

void btaudio_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(btaudio_screen, cfg, "btaudio_title");

	// No "audio via Bluetooth" switch, no paragraph explaining the absence, and
	// no "output" row: the stock player has none of the three. Connected
	// headphones are the output, and which ones they are is written on the
	// Bluetooth page itself.

	// The player's own name, first: it is about this device rather than about
	// whatever is connected to it.
	settingsrow_add(container, "bt_rename", &name_row_value, name_row_cb, NULL);

	// The AirPods row, above the codec: it is about the headphones themselves
	// rather than about the link, and it is the one row here that is not
	// always there.
	airpods_row = settingsrow_add(container, "airpods", NULL, airpods_row_cb, NULL);
	airpods_icon = lv_image_create(airpods_row);
	lv_obj_add_style(airpods_icon, &theme_style_icon, 0);
	lv_image_set_src(airpods_icon, airpodspage_model_icon(AIRPODS_MODEL_UNKNOWN));
	lv_obj_align(airpods_icon, LV_ALIGN_RIGHT_MID, -AIRPODS_ICON_INSET, 0);
	lv_obj_add_flag(airpods_row, LV_OBJ_FLAG_HIDDEN);

	// The codec in use, and the list of what the sink offered.
	codec_row = settingsrow_add(container, "btaudio_codecs", &codec_value, codec_row_cb, NULL);

	// One level or two, on by default. On, the keys here move the headphones
	// over AVRCP absolute volume and their own buttons move the number on the
	// screen. Off, bluealsa attenuates the stream on this side and the
	// headphones keep their own level, so the two are independent.
	settingsrow_toggle(container, "btaudio_synchronised_volume", &volume_switch, volume_toggle_cb);
	if (bluetooth_volume_sync()) {
		lv_obj_add_state(volume_switch, LV_STATE_CHECKED);
	}

	// --- the codec list, on a page of its own ---
	codec_screen = lv_obj_create(NULL);
	codec_list = settingsrow_page(codec_screen, cfg, "btaudio_codecs");
	lv_obj_add_event_cb(codec_screen, codec_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(codec_screen);

	poll_timer = lv_timer_create(poll_cb, BTAUDIO_POLL_MS, NULL);
	lv_timer_pause(poll_timer);

	// This one is never paused: the output can change while the user is on the
	// player, which is exactly when they would like to keep hearing the music.
	lv_timer_create(routing_watch_cb, ROUTING_WATCH_MS, NULL);

	lv_obj_add_event_cb(btaudio_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(btaudio_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(btaudio_screen);

	build_name_layer(cfg);

	refresh_page();
}
