#include "lastfmsettings.h"

#include <stdio.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/audio/audio.h"
#include "src/system/core/lang.h"
#include "src/system/lastfm/lastfm.h"
#include "src/system/library/playcount.h"
#include "src/system/net/wifi.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/radio.h"

#define TICK_MS 1000

static lv_obj_t *page_screen;
static lv_obj_t *enabled_switch;
static lv_obj_t *account_value;
static lv_obj_t *status_value;
static lv_obj_t *queued_value;
static lv_obj_t *note_label;

static lv_obj_t *login_screen;
static lv_obj_t *user_field;
static lv_obj_t *password_field;
static lv_obj_t *password_eye_icon;
static keyboard_t *login_keyboard;

static unsigned seen_login_serial;
static bool waiting_for_login;

lv_obj_t *lastfmsettings_screen(void) { return page_screen; }

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

static void refresh(void) {
	lastfm_status_t st;
	lastfm_get_status(&st);

	if (st.enabled) {
		lv_obj_add_state(enabled_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(enabled_switch, LV_STATE_CHECKED);
	}
	if (st.state == LASTFM_STATE_UNAVAILABLE) {
		lv_obj_add_state(enabled_switch, LV_STATE_DISABLED);
	}

	lv_label_set_text(account_value, st.user[0] ? st.user : tr("lastfm_not_signed_in"));

	const char *status = "";
	switch (st.state) {
	case LASTFM_STATE_UNAVAILABLE:
		status = tr("lastfm_unavailable");
		break;
	case LASTFM_STATE_OFF:
		status = tr("lastfm_state_off");
		break;
	case LASTFM_STATE_SIGNED_OUT:
		status = tr("lastfm_not_signed_in");
		break;
	case LASTFM_STATE_SIGNING_IN:
		status = tr("lastfm_state_signing_in");
		break;
	case LASTFM_STATE_CONNECTED:
		status = tr("connected");
		break;
	case LASTFM_STATE_SESSION_EXPIRED:
		status = tr("lastfm_state_session_expired");
		break;
	case LASTFM_STATE_REFUSED:
		status = st.detail;
		break;
	}
	lv_label_set_text(status_value, status);
	lv_label_set_text(note_label, tr(st.state == LASTFM_STATE_UNAVAILABLE ? "api_keys_unavailable" : "lastfm_note"));
	lv_label_set_text_fmt(queued_value, "%d", st.queued);
}

static void open_login(void) {
	lv_textarea_set_text(user_field, "");
	lv_textarea_set_text(password_field, "");
	lv_textarea_set_password_mode(password_field, true);
	lv_image_set_src(password_eye_icon, &icon_eye);
	keyboard_refresh_password(password_field);
	keyboard_reset(login_keyboard);
	lv_obj_t *other = password_field;
	keyboard_set_field(login_keyboard, user_field);
	lv_obj_add_state(user_field, LV_STATE_FOCUSED);
	lv_obj_remove_state(other, LV_STATE_FOCUSED);
	keyboard_show_caret(user_field, true);
	keyboard_show_caret(other, false);
	switch_screen(login_screen);
}

static void enabled_cb(lv_event_t *e) {
	(void)e;
	bool on = lv_obj_has_state(enabled_switch, LV_STATE_CHECKED);
	lastfm_set_enabled(on);
	lastfm_status_t st;
	lastfm_get_status(&st);
	// Switched on with no account behind it: the sign-in is the next step.
	if (on && !st.user[0] && st.state != LASTFM_STATE_SIGNING_IN) {
		open_login();
	}
	refresh();
}

static void do_logout(void *user) {
	(void)user;
	lastfm_logout();
	toast_success("signed_out");
	refresh();
}

static void account_cb(lv_event_t *e) {
	(void)e;
	lastfm_status_t st;
	lastfm_get_status(&st);
	if (st.state == LASTFM_STATE_UNAVAILABLE) {
		toast_error("api_keys_unavailable");
		return;
	}
	if (st.state == LASTFM_STATE_SIGNING_IN) {
		return;
	}
	if (st.user[0]) {
		char message[160];
		snprintf(message, sizeof(message), tr("sign_out_confirm_note"), "Last.fm");
		confirm_show("lastfm_sign_out", message, "leave", do_logout, NULL);
		return;
	}
	open_login();
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	refresh();
}

static void build_page(gui_config_t *cfg) {
	page_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(page_screen, cfg, "lastfm");

	settingsrow_toggle(container, "lastfm_scrobble", &enabled_switch, enabled_cb);
	settingsrow_add(container, "lastfm_account", &account_value, account_cb, NULL);
	settingsrow_add(container, "lastfm_status", &status_value, NULL, NULL);
	settingsrow_add(container, "lastfm_queued", &queued_value, NULL, NULL);

	// A user name or a message from Last.fm can be longer than the room beside
	// the row's name: one line, cut with an ellipsis.
	lv_obj_t *values[] = {account_value, status_value};
	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		lv_label_set_long_mode(values[i], LV_LABEL_LONG_DOT);
		lv_obj_set_style_max_width(values[i], lv_pct(60), 0);
		lv_obj_set_height(values[i], lv_font_get_line_height(&font_ui_24));
	}

	note_label = lv_label_create(container);
	lv_label_set_long_mode(note_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note_label, lv_pct(100));
	lv_obj_add_style(note_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note_label, &font_ui_22, 0);
	lv_label_set_text(note_label, tr("lastfm_note"));

	lv_obj_add_event_cb(page_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(page_screen);
}

// ---------------------------------------------------------------------------
// signing in
// ---------------------------------------------------------------------------

static void password_eye_cb(lv_event_t *e) {
	(void)e;
	bool hidden = !lv_textarea_get_password_mode(password_field);
	lv_textarea_set_password_mode(password_field, hidden);
	lv_image_set_src(password_eye_icon, hidden ? &icon_eye : &icon_eye_off);
	keyboard_refresh_password(password_field);
}

// The keyboard writes into the last field touched.
static void field_focus_cb(lv_event_t *e) {
	lv_obj_t *field = lv_event_get_target(e);
	lv_obj_t *other = field == user_field ? password_field : user_field;
	keyboard_set_field(login_keyboard, field);
	lv_obj_add_state(field, LV_STATE_FOCUSED);
	lv_obj_remove_state(other, LV_STATE_FOCUSED);
	keyboard_show_caret(field, true);
	keyboard_show_caret(other, false);
}

static void login_accept_cb(lv_event_t *e) {
	(void)e;
	const char *name = lv_textarea_get_text(user_field);
	const char *password = lv_textarea_get_text(password_field);
	if (!name || !name[0] || !password || !password[0]) {
		toast_error("credentials_required");
		return;
	}
	wifi_status_t wifi;
	wifi_get_status(&wifi);
	if (wifi.state != WIFI_STATE_CONNECTED || !wifi.ip[0]) {
		toast_error("enable_wi_fi_first");
		return;
	}

	lastfm_login(name, password);
	lv_textarea_set_text(password_field, "");
	waiting_for_login = true;
	toast_busy("lastfm_state_signing_in");
}

static lv_obj_t *make_field(lv_obj_t *parent, gui_config_t *cfg, const char *placeholder, int y) {
	lv_obj_t *field = lv_textarea_create(parent);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_placeholder_text(field, tr(placeholder));
	lv_obj_set_size(field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, cfg->padding, y);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_radius(field, 12, 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_all(field, 14, 0);
	lv_obj_set_style_text_font(field, &font_ui_24, 0);
	keyboard_style_caret(field);
	lv_obj_add_event_cb(field, field_focus_cb, LV_EVENT_CLICKED, NULL);
	return field;
}

static void build_login(gui_config_t *cfg) {
	login_screen = lv_obj_create(NULL);
	lv_obj_add_style(login_screen, &theme_style_screen, 0);

	settingsrow_title(login_screen, cfg, "lastfm_sign_in");
	int top = settingsrow_content_top(cfg);

	user_field = make_field(login_screen, cfg, "lastfm_username", top);
	password_field = make_field(login_screen, cfg, "password", top + 78);
	keyboard_style_password(password_field, 0);
	lv_obj_set_style_pad_right(password_field, 60, 0);

	lv_obj_t *eye = lv_btn_create(login_screen);
	lv_obj_set_size(eye, 56, 56);
	lv_obj_align(eye, LV_ALIGN_TOP_RIGHT, -cfg->padding - 4, top + 78 + 3);
	lv_obj_set_style_bg_opa(eye, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(eye, 0, 0);
	lv_obj_set_style_border_width(eye, 0, 0);
	lv_obj_add_event_cb(eye, password_eye_cb, LV_EVENT_CLICKED, NULL);

	password_eye_icon = lv_image_create(eye);
	lv_obj_add_style(password_eye_icon, &theme_style_icon, 0);
	lv_image_set_src(password_eye_icon, &icon_eye);
	lv_obj_center(password_eye_icon);

	lv_obj_t *note = lv_label_create(login_screen);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, cfg->screen_width - 2 * cfg->padding);
	lv_obj_align(note, LV_ALIGN_TOP_LEFT, cfg->padding, top + 156);
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_label_set_text(note, tr("lastfm_login_note"));

	login_keyboard = keyboard_create(login_screen, cfg->screen_width, 316, user_field, NULL, "ok", login_accept_cb, NULL);
	switcher_attach_back_gesture(login_screen);
}

static void login_finished(const lastfm_status_t *st) {
	if (!waiting_for_login) {
		return;
	}
	waiting_for_login = false;
	toast_busy_end();

	switch (st->login_result) {
	case LASTFM_LOGIN_OK:
		if (lv_screen_active() == login_screen) {
			back_btn_cb(NULL);
		}
		toast_success("signed_in");
		break;
	case LASTFM_LOGIN_WRONG_CREDENTIALS:
		toast_error("lastfm_wrong_credentials");
		break;
	case LASTFM_LOGIN_NO_REPLY:
		toast_error("lastfm_no_reply");
		break;
	case LASTFM_LOGIN_REFUSED:
		toast_error(st->detail);
		break;
	}
}

// ---------------------------------------------------------------------------
// the timer: what is playing, and the page while it is on screen
// ---------------------------------------------------------------------------

static void tick_cb(lv_timer_t *t) {
	(void)t;
	char file[512];
	audio_get_current_file(file, sizeof(file));
	bool playing = audio_get_status() == AUDIO_STATUS_PLAYING;
	double position = 0.0, duration = 0.0;
	audio_get_progress(&position, &duration);
	bool skip = radio_is_active() || audiobook_is_playing() || podcastcache_is_episode(file);

	// The player's own count of plays, by the same rule, whether Last.fm is in
	// use or not.
	playcount_playback_t counted = {
		.playing = playing, .file = file, .position = position, .duration = duration, .skip = skip};
	playcount_note_playback(&counted);

	if (lastfm_active()) {
		lastfm_playback_t now = {0};
		now.file = file;
		now.playing = playing;
		now.position = position;
		now.duration = duration;

		// Tags only once device_state has read them for this very file.
		const char *tags_file = NULL;
		const song_metadata_t *m = device_state_loaded_metadata(&tags_file);
		bool tags = file[0] && tags_file && strcmp(tags_file, file) == 0;
		now.artist = tags ? (m->artist[0] ? m->artist : m->album_artist) : "";
		now.title = tags ? m->title : "";
		now.album = tags ? m->album : "";

		now.skip = skip;
		lastfm_note_playback(&now);
	}

	lastfm_status_t st;
	lastfm_get_status(&st);
	if (st.login_serial != seen_login_serial) {
		seen_login_serial = st.login_serial;
		login_finished(&st);
	}
	if (lv_screen_active() == page_screen) {
		refresh();
	}
}

void lastfmsettings_init(gui_config_t *cfg) {
	build_page(cfg);
	build_login(cfg);

	lastfm_status_t st;
	lastfm_get_status(&st);
	seen_login_serial = st.login_serial;
	refresh();

	lv_timer_create(tick_cb, TICK_MS, NULL);
}
