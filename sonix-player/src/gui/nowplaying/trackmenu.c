#include "trackmenu.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/library/medialist.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/streaming/qobuzpage.h"
#include "src/gui/streaming/tidalpage.h"
#include "src/gui/shell/popover.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/nowplaying/cover.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/replaygain.h"
#include "src/system/decode/decode.h"
#include "src/system/library/metadata.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"
#include "src/gui/streaming/podcastpage.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/library/library.h"
#include "src/system/playback/playlist.h"
#include "src/system/core/utils.h"

lv_obj_t *queue_screen;
lv_obj_t *details_screen;

// The queue is windowed like every other long list here: a fixed pool of row
// widgets rides the scroll position, and each row looks up its title (one
// indexed database lookup) at the moment it binds. Opening the page costs a
// dozen binds, not hundreds, which is what makes it instant.
#define QUEUE_THUMB_SIZE 56
#define QUEUE_ROW_HEIGHT 72
#define QUEUE_ROW_GAP 8
#define QUEUE_ROW_PITCH (QUEUE_ROW_HEIGHT + QUEUE_ROW_GAP)
#define QUEUE_ROW_POOL 12

typedef struct {
	lv_obj_t *button;
	lv_obj_t *icon;
	lv_obj_t *label;
	int index; // position in the queue, or -1
	bool has_thumb;
	cover_image_t thumb;
} queue_row_t;

static lv_obj_t *queue_list; // the scrollable viewport
static lv_obj_t *queue_body; // fixed-height canvas the pool rows sit on
static lv_obj_t *queue_empty;
static queue_row_t queue_rows[QUEUE_ROW_POOL];
static int queue_total; // how many tracks the queue holds, from its first

static lv_obj_t *details_card;
static lv_obj_t *details_scroll;
static gui_config_t *g_cfg;

// ---------------------------------------------------------------------------
// Queue: the current track and the ones after it, tappable to jump.
// ---------------------------------------------------------------------------

static void queue_row_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}

	lv_obj_t *button = lv_event_get_current_target(e);
	for (int i = 0; i < QUEUE_ROW_POOL; i++) {
		if (queue_rows[i].button == button && queue_rows[i].index >= 0) {
			if (device_state_play_queue_index(queue_rows[i].index)) {
				player_refresh_now_playing();
				switch_screen(player_screen);
			}
			return;
		}
	}
}

static void queue_row_drop_thumb(queue_row_t *row) {
	if (row->has_thumb) {
		// The widget must stop pointing at the pixels before they go.
		lv_image_set_src(row->icon, &icon_music2);
		lv_obj_add_style(row->icon, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);
		cover_free(&row->thumb);
		row->has_thumb = false;
	}
}

// No artwork on these rows: a queue is a list of what is coming, and a column
// of covers is slower to read and slower to build. The playing track gets the
// now-playing mark in the accent colour instead; everything else gets the plain
// note.
//
// Its own function because the mark changes without the row changing: painting
// it only where a row is bound would leave the mark on a finished track until
// the list scrolled far enough to rebind that row.
static void queue_row_paint_mark(queue_row_t *row) {
	if (!row->icon || row->index < 0) {
		return;
	}
	bool is_playing_row = row->index == playlist_current_index();
	lv_image_set_src(row->icon, is_playing_row ? &icon_now_playing : &icon_music2);
	lv_obj_add_style(row->icon, &theme_style_icon, 0);
	if (is_playing_row) {
		lv_obj_set_style_image_recolor(row->icon, theme()->accent, 0);
	} else {
		lv_obj_remove_local_style_prop(row->icon, LV_STYLE_IMAGE_RECOLOR, 0);
	}
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);
}

// The track changed under the page: repaint the marks and nothing else. Called
// from the one place in the player that sees every change of track, the same
// way the browser and the library lists are told.
void trackmenu_notify_now_playing(void) {
	for (int i = 0; i < QUEUE_ROW_POOL; i++) {
		queue_row_paint_mark(&queue_rows[i]);
	}
}

// Points a pool row at queued track `index` and resolves its title on the spot;
// an indexed lookup is cheap enough to do during a scroll.
static void queue_row_bind(queue_row_t *row, int index) {
	if (row->index == index) {
		return;
	}

	queue_row_drop_thumb(row);
	row->index = index;

	if (index < 0 || index >= queue_total) {
		lv_obj_set_hidden(row->button, true);
		return;
	}

	char path[512];
	if (!playlist_path_at(index, path, sizeof(path))) {
		lv_obj_set_hidden(row->button, true);
		return;
	}

	char title[256];
	if (!library_track_title(path, title, sizeof(title))) {
		// Not in the library: it may be a Qobuz track, which is kept out of the
		// library on purpose. The title is still there, in the sidecar file next
		// to the track. Without this the queue showed a column of numbers, which
		// were the file names.
		song_metadata_t meta;
		metadata_read(path, &meta);
		if (meta.title[0]) {
			snprintf(title, sizeof(title), "%.250s", meta.title);
		} else {
			const char *slash = strrchr(path, '/');
			snprintf(title, sizeof(title), "%.250s", slash ? slash + 1 : path);
			char *dot = strrchr(title, '.');
			if (dot) {
				*dot = '\0';
			}
		}
	}

	lv_obj_set_hidden(row->button, false);
	lv_obj_set_y(row->button, index * QUEUE_ROW_PITCH);
	lv_label_set_text(row->label, title);

	queue_row_paint_mark(row);
}

static void queue_window_update(void) {
	if (queue_total <= 0) {
		for (int i = 0; i < QUEUE_ROW_POOL; i++) {
			queue_row_bind(&queue_rows[i], -1);
		}
		return;
	}

	int scroll = lv_obj_get_scroll_y(queue_list);
	if (scroll < 0) {
		scroll = 0;
	}

	int first = (scroll / QUEUE_ROW_PITCH) - 1;
	if (first + QUEUE_ROW_POOL > queue_total) {
		first = queue_total - QUEUE_ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}

	for (int i = 0; i < QUEUE_ROW_POOL; i++) {
		int index = first + i;
		queue_row_bind(&queue_rows[index % QUEUE_ROW_POOL], index < queue_total ? index : -1);
	}
}

static void queue_scroll_cb(lv_event_t *e) {
	(void)e;
	queue_window_update();
}

static void queue_rebuild(void) {
	// The whole queue, from its first track: the ones already played are part
	// of it and are how the album being listened to can be seen at all.
	queue_total = playlist_count();
	if (queue_total < 0) {
		queue_total = 0;
	}

	lv_obj_set_height(queue_body, queue_total ? queue_total * QUEUE_ROW_PITCH : QUEUE_ROW_PITCH);
	// The scroll below is clamped to the scrollable range, and that range comes
	// from the height just set. Without the layout being brought up to date
	// first the clamp uses the old height and the page opens at the top.
	lv_obj_update_layout(queue_list);

	// Opened on the playing track rather than at the top: with the whole queue
	// listed, the top is wherever the queue began, which may be an hour ago.
	// One row of what came before is left showing, so it reads as a list
	// scrolled to a place rather than as a list that starts there.
	int current = playlist_current_index();
	int32_t at = current > 0 ? (int32_t)(current - 1) * QUEUE_ROW_PITCH : 0;
	lv_obj_scroll_to_y(queue_list, at, LV_ANIM_OFF);

	for (int i = 0; i < QUEUE_ROW_POOL; i++) {
		queue_rows[i].index = -2; // force every row to rebind
	}
	queue_window_update();

	if (queue_total == 0) {
		lv_obj_set_hidden(queue_empty, false);
	} else {
		lv_obj_set_hidden(queue_empty, true);
	}
}

static void queue_loaded_cb(lv_event_t *e) {
	(void)e;
	queue_rebuild();
}

static void build_queue_page(gui_config_t *cfg) {
	lv_obj_add_style(queue_screen, &theme_style_screen, 0);
	settingsrow_title(queue_screen, cfg, "trackmenu_queue");

	int content_top = settingsrow_content_top(cfg);
	int width = cfg->screen_width - 2 * cfg->padding;

	queue_list = lv_obj_create(queue_screen);
	lv_obj_set_size(queue_list, cfg->screen_width, cfg->screen_height - content_top);
	lv_obj_align(queue_list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(queue_list, 0, 0);
	lv_obj_set_style_border_width(queue_list, 0, 0);
	lv_obj_set_style_radius(queue_list, 0, 0);
	lv_obj_set_style_pad_hor(queue_list, cfg->padding, 0);
	lv_obj_set_style_pad_ver(queue_list, 0, 0);
	lv_obj_set_scroll_dir(queue_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(queue_list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_add_event_cb(queue_list, queue_scroll_cb, LV_EVENT_SCROLL, NULL);

	queue_body = lv_obj_create(queue_list);
	lv_obj_set_width(queue_body, width);
	lv_obj_set_height(queue_body, QUEUE_ROW_PITCH);
	lv_obj_set_pos(queue_body, 0, 0);
	lv_obj_set_style_bg_opa(queue_body, 0, 0);
	lv_obj_set_style_border_width(queue_body, 0, 0);
	lv_obj_set_style_pad_all(queue_body, 0, 0);
	lv_obj_set_scrollable(queue_body, false);
	lv_obj_set_event_bubble(queue_body, true);

	queue_empty = lv_label_create(queue_list);
	lv_label_set_text(queue_empty, tr("trackmenu_queue_empty"));
	lv_obj_set_style_text_align(queue_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(queue_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(queue_empty, &font_ui_24, 0);
	lv_obj_align(queue_empty, LV_ALIGN_TOP_MID, 0, 120);
	lv_obj_set_hidden(queue_empty, true);

	for (int i = 0; i < QUEUE_ROW_POOL; i++) {
		queue_row_t *row = &queue_rows[i];

		row->button = lv_btn_create(queue_body);
		lv_obj_set_size(row->button, width, QUEUE_ROW_HEIGHT);
		lv_obj_set_x(row->button, 0);
		lv_obj_add_style(row->button, &theme_style_card, 0);
		lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row->button, 12, 0);
		lv_obj_set_style_border_width(row->button, 0, 0);
		lv_obj_set_style_shadow_width(row->button, 0, 0);
		lv_obj_set_style_pad_all(row->button, 8, 0);
		lv_obj_set_style_pad_column(row->button, 14, 0);
		lv_obj_set_hidden(row->button, true);
		lv_obj_set_event_bubble(row->button, true);
		lv_obj_add_event_cb(row->button, queue_row_clicked_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		row->icon = lv_image_create(row->button);
		lv_obj_set_size(row->icon, QUEUE_THUMB_SIZE, QUEUE_THUMB_SIZE);
		lv_image_set_inner_align(row->icon, LV_IMAGE_ALIGN_CENTER);

		row->label = lv_label_create(row->button);
		lv_label_set_long_mode(row->label, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(row->label, 1);
		lv_obj_set_height(row->label, 32);
		lv_obj_add_style(row->label, &theme_style_text, 0);
		lv_obj_set_style_text_font(row->label, &font_ui_24, 0);

		row->index = -1;
		row->has_thumb = false;
	}

	// No player-sheet swipe on the queue: it kept sliding the player over the
	// list being read. The swipe-back drag still works from here.
	switcher_attach_back_gesture(queue_list);
	lv_obj_add_event_cb(queue_screen, queue_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// Details: everything known about the playing file.
// ---------------------------------------------------------------------------

static void details_add_row(const char *name, const char *value) {
	if (!value || !value[0]) {
		return; // an empty field is noise, not information
	}

	lv_obj_t *row = lv_obj_create(details_card);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_gap(row, 2, 0);
	lv_obj_set_scrollable(row, false);
	// Presses over the text must climb to the scroll surface, or the swipe
	// gestures can never start on the card's content.
	lv_obj_set_event_bubble(row, true);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);

	lv_obj_t *name_label = lv_label_create(row);
	lv_label_set_text(name_label, tr(name));
	lv_obj_add_style(name_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(name_label, &font_ui_18, 0);

	lv_obj_t *value_label = lv_label_create(row);
	lv_label_set_text(value_label, value);
	lv_label_set_long_mode(value_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(value_label, lv_pct(100));
	lv_obj_add_style(value_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(value_label, &font_ui_24, 0);
}

// Which file the page describes: a specific one (opened from a library
// list's popover) or, when empty, whatever is playing right now (opened from
// the player's own menu).
static char details_path[512];

// The common tail of both variants: tags, format, size, path.
// `codec` is the name the decoder reports ("MP3", "AAC", "ALAC"...), empty when
// it has none; `kbps` is the bitrate the file declares, 0 when it declares
// none; `lossy` says the format discards information. See decode.h: for a lossy
// file the bit/kHz pair describes the PCM leaving the decoder, not what was
// encoded.
// One ReplayGain figure from the tags: the correction, and the peak it was
// measured against when the file gives one.
static void details_add_gain(const char *name, float gain_db, float peak) {
	char buffer[64];
	if (peak > 0) {
		snprintf(buffer, sizeof(buffer), tr("trackmenu_rg_gain_peak"), gain_db, peak);
	} else {
		snprintf(buffer, sizeof(buffer), "%+.2f dB", gain_db);
	}
	details_add_row(name, buffer);
}

// `playing` adds what the player is applying to this file right now, which
// only exists for the track being played.
static void details_fill(const char *file, const song_metadata_t *tags, int bits, double sample_rate,
						 int channels, double duration_secs, const char *codec, int kbps, bool lossy,
						 bool playing) {
	char buffer[64];

	const char *slash = strrchr(file, '/');
	details_add_row("trackmenu_title", tags->title[0] ? tags->title : (slash ? slash + 1 : file));
	details_add_row("trackmenu_artist", tags->artist);
	details_add_row("trackmenu_album_artist", tags->album_artist);
	details_add_row("albums", tags->album);
	details_add_row("genre", tags->genre);

	if (tags->year > 0) {
		snprintf(buffer, sizeof(buffer), "%d", tags->year);
		details_add_row("trackmenu_year", buffer);
	}
	if (tags->track_number > 0) {
		snprintf(buffer, sizeof(buffer), "%d", tags->track_number);
		details_add_row("track", buffer);
	}
	if (tags->disc_number > 0) {
		snprintf(buffer, sizeof(buffer), "%d", tags->disc_number);
		details_add_row("trackmenu_disc_number", buffer);
	}

	// A lossy file is described by its bitrate, not by bits and kHz: those two
	// numbers belong to the PCM leaving the decoder and are identical for a
	// 96 kbps AAC and a 320 kbps one. Lossless files keep the usual pair.
	if (lossy) {
		if (kbps > 0 && codec && codec[0]) {
			snprintf(buffer, sizeof(buffer), "%d kbps %s", kbps, codec);
		} else if (kbps > 0) {
			snprintf(buffer, sizeof(buffer), "%d kbps", kbps);
		} else {
			snprintf(buffer, sizeof(buffer), "%s", codec && codec[0] ? codec : "");
		}
		if (buffer[0]) {
			details_add_row("trackmenu_format", buffer);
		}
	} else if (sample_rate > 0) {
		snprintf(buffer, sizeof(buffer), tr("trackmenu_bit_khz_channels"), bits > 0 ? bits : 16,
				 sample_rate / 1000.0, channels);
		details_add_row("trackmenu_format", buffer);
	}

	// A separate bitrate row, only for local lossless tracks: size over
	// duration, which is how dense the file is. Lossy files already carry it in
	// the row above, and for a streamed track the cache file may be half
	// downloaded, making the number meaningless.
	if (!lossy && !qobuzcache_owns(file) && !tidalcache_owns(file) && duration_secs > 0.5) {
		struct stat st_kbps;
		if (stat(file, &st_kbps) == 0 && st_kbps.st_size > 0) {
			int avg = (int)((double)st_kbps.st_size * 8.0 / duration_secs / 1000.0 + 0.5);
			snprintf(buffer, sizeof(buffer), "%d kbps", avg);
			details_add_row("trackmenu_bitrate", buffer);
		}
	}

	if (tags->has_track_gain) {
		details_add_gain("trackmenu_rg_track", tags->track_gain_db, tags->track_peak);
	}
	if (tags->has_album_gain) {
		details_add_gain("trackmenu_rg_album", tags->album_gain_db, tags->album_peak);
	}
	// The applied figure can differ from both tags: it is the one the mode
	// picks, lowered when the peak would clip, and capped at +12 dB.
	if (playing && (tags->has_track_gain || tags->has_album_gain)) {
		if (replaygain_mode() == REPLAYGAIN_OFF) {
			details_add_row("trackmenu_rg_applied", tr("trackmenu_rg_off"));
		} else {
			snprintf(buffer, sizeof(buffer), "%+.2f dB", replaygain_current_db());
			details_add_row("trackmenu_rg_applied", buffer);
		}
	}

	// No size row for a streamed track: it would be the size of the cache file,
	// which may still be downloading -- a number that grows on screen by itself
	// and describes the download, not the track.
	struct stat st;
	if (!qobuzcache_owns(file) && !tidalcache_owns(file) && stat(file, &st) == 0) {
		snprintf(buffer, sizeof(buffer), "%.1f MB", st.st_size / (1024.0 * 1024.0));
		details_add_row("trackmenu_size", buffer);
	}

	// The path, except for a streamed track: that lives in a cache directory
	// nobody chose, under a numeric name, and showing it implies a file of the
	// user's own that does not exist. The source service is shown instead.
	if (qobuzcache_owns(file)) {
		details_add_row("trackmenu_source", "qobuz");
	} else if (tidalcache_owns(file)) {
		details_add_row("trackmenu_source", "tidal");
	} else {
		details_add_row("trackmenu_file", file);
	}
}

static void details_rebuild(void) {
	lv_obj_clean(details_card);

	if (details_path[0]) {
		// An arbitrary library track: tags read from the file itself, format
		// from the decoder's header (opened and closed without decoding).
		song_metadata_t tags;
		metadata_read(details_path, &tags);

		int bits = 0, channels = 0;
		double sample_rate = 0;
		decode_format_t format = decode_detect_format(details_path);
		double duration_secs = 0;
		char codec[16] = "";
		int kbps = 0;
		bool lossy = false;
		decoder_t *dec = format != DECODE_FORMAT_UNKNOWN ? decoder_open_info(details_path, format) : NULL;
		if (dec) {
			bits = decoder_source_bits(dec);
			channels = decoder_channels(dec);
			sample_rate = decoder_sample_rate(dec);
			snprintf(codec, sizeof(codec), "%s", decoder_codec_name(dec));
			kbps = decoder_bitrate_kbps(dec);
			lossy = decoder_is_lossy(dec);
			uint64_t frames = decoder_total_pcm_frames(dec);
			if (sample_rate > 0 && frames > 0) {
				duration_secs = (double)frames / sample_rate;
			}
			decoder_close(dec);
		}

		// A lossy file that declares no bitrate: use the real average, which can
		// be computed here because the whole file is present (streamed tracks
		// never reach this branch).
		if (lossy && kbps <= 0 && duration_secs > 0.5) {
			struct stat st_lossy;
			if (stat(details_path, &st_lossy) == 0 && st_lossy.st_size > 0) {
				kbps = (int)((double)st_lossy.st_size * 8.0 / duration_secs / 1000.0 + 0.5);
			}
		}

		details_fill(details_path, &tags, bits, sample_rate, channels, duration_secs, codec, kbps, lossy,
					 false);
		return;
	}

	device_state_t state;
	device_state_get(&state);

	if (!state.current_file[0]) {
		details_add_row("trackmenu_state", tr("trackmenu_nothing_is_playing"));
		return;
	}

	// The stream, as it is actually being played.
	char codec[16] = "";
	audio_get_stream_codec(codec, sizeof(codec));
	bool lossy = audio_stream_is_lossy();
	int kbps = audio_get_stream_bitrate_kbps();

	// As in the player: when the file declares no bitrate, fall back to the real
	// average, but only for a local file -- a streamed one may still be half
	// downloaded and the number would climb by itself.
	if (lossy && kbps <= 0 && !qobuzcache_owns(state.current_file) && !tidalcache_owns(state.current_file) &&
		state.progress_total_secs > 0.5) {
		struct stat st_lossy;
		if (stat(state.current_file, &st_lossy) == 0 && st_lossy.st_size > 0) {
			kbps = (int)((double)st_lossy.st_size * 8.0 / state.progress_total_secs / 1000.0 + 0.5);
		}
	}

	details_fill(state.current_file, &state.metadata, audio_get_stream_bits(), state.stream_sample_rate,
				 state.stream_channels, state.progress_total_secs, codec, kbps, lossy, true);
}

static void details_loaded_cb(lv_event_t *e) {
	(void)e;
	details_rebuild();

	// Always start at the top. The page is built once and refilled on every
	// open, so the scroll container otherwise keeps the position from last time
	// and the card for another track opens halfway down. The first row is the
	// title, which is what should be visible on entry.
	//
	// After details_rebuild(), not before: that is what sets the content height,
	// and LVGL clamps a scroll past the bottom of whatever was there before.
	if (details_scroll) {
		lv_obj_scroll_to_y(details_scroll, 0, LV_ANIM_OFF);
	}
}

static void build_details_page(gui_config_t *cfg) {
	lv_obj_add_style(details_screen, &theme_style_screen, 0);
	settingsrow_title(details_screen, cfg, "details");

	int content_top = settingsrow_content_top(cfg);

	lv_obj_t *scroll = lv_obj_create(details_screen);
	lv_obj_set_size(scroll, cfg->screen_width, cfg->screen_height - content_top);
	lv_obj_align(scroll, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(scroll, 0, 0);
	lv_obj_set_style_border_width(scroll, 0, 0);
	lv_obj_set_style_radius(scroll, 0, 0);
	lv_obj_set_style_pad_hor(scroll, cfg->padding, 0);
	lv_obj_set_style_pad_ver(scroll, 0, 0);
	lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
	details_scroll = scroll;

	details_card = lv_obj_create(scroll);
	lv_obj_set_width(details_card, lv_pct(100));
	lv_obj_set_height(details_card, LV_SIZE_CONTENT);
	lv_obj_add_style(details_card, &theme_style_card, 0);
	lv_obj_set_style_radius(details_card, 12, 0);
	lv_obj_set_style_border_width(details_card, 0, 0);
	lv_obj_set_style_shadow_width(details_card, 0, 0);
	lv_obj_set_style_pad_all(details_card, 18, 0);
	lv_obj_set_style_pad_gap(details_card, 14, 0);
	lv_obj_set_scrollable(details_card, false);
	lv_obj_set_event_bubble(details_card, true);
	lv_obj_set_flex_flow(details_card, LV_FLEX_FLOW_COLUMN);

	// Deliberately not player_sheet_attach_drag(): this page is opened from the
	// player, so dragging it sideways back into the player was one gesture
	// meaning two contradictory things. Removing details_screen from gui.c's
	// swipe list was not enough on its own -- the drag was attached here, to the
	// scroll container, and that is what kept working.
	switcher_attach_back_gesture(scroll);
	lv_obj_add_event_cb(details_screen, details_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// the popover
// ---------------------------------------------------------------------------

static void open_queue_cb(void *user) {
	(void)user;
	trackmenu_open_queue();
}

// The same page without going through the menu: the player calls this when a
// podcast is playing, where the overflow button becomes the episodes button --
// and a podcast's episodes are exactly the queue.
void trackmenu_open_queue(void) {
	player_sheet_close(false);
	switch_screen(queue_screen);
	switcher_set_player_return(queue_screen);
}

static void open_details_cb(void *user) {
	(void)user;
	details_path[0] = '\0'; // the playing track
	player_sheet_close(false);
	switch_screen(details_screen);
	switcher_set_player_return(details_screen);
}

// "Add to playlist" for the track the player has loaded: the same picker the
// library lists open, on the playing file.
static void add_to_playlist_cb(void *user) {
	(void)user;
	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0]) {
		return;
	}
	player_sheet_close(false);
	playlistpage_add_track(state.current_file);
	switcher_set_player_return(playlistpage_screen);
}

// "Show album": the record the playing track came off. Backing out of it
// returns to the player, like everything else this menu opens.
static void open_album_cb(void *user) {
	(void)user;

	device_state_t state;
	device_state_get(&state);
	if (!state.current_file[0]) {
		return;
	}

	// Podcasts come first, because there is no album to show: the other episodes
	// are what is wanted. The feed id was kept by the sidecar file when the
	// episode was downloaded; the "album" title alone would not do, because two
	// podcasts can share a name and the catalogue is queried by id.
	long long feed_id = 0;
	if (podcastcache_feed_id(state.current_file, &feed_id)) {
		device_state_t now;
		device_state_get(&now);
		player_sheet_close(false);
		if (!podcastpage_open_feed(feed_id, now.metadata.album)) {
			gui_notify_popup("trackmenu_podcast_failed");
			return;
		}
		switcher_set_player_return(podcast_list_screen);
		return;
	}

	// A Qobuz track is not in the local library, so its album is not looked up
	// in the database: it is reopened on Qobuz, using the id the sidecar file
	// kept when the track was downloaded.
	char qobuz_album[40];
	if (qobuzcache_album_id(state.current_file, qobuz_album, sizeof(qobuz_album))) {
		char title[256];
		if (!library_track_album(state.current_file, title, sizeof(title)) || !title[0]) {
			device_state_t now;
			device_state_get(&now);
			snprintf(title, sizeof(title), "%s", now.metadata.album);
		}
		player_sheet_close(false);
		if (!qobuzpage_open_album(qobuz_album, title)) {
			gui_notify_popup("trackmenu_qobuz_album_failed");
			return;
		}
		switcher_set_player_return(qobuz_list_screen);
		return;
	}

	// Same for Tidal, for the same reason: a streamed track's album is reopened
	// on the service it came from, not in the local database, where it is not.
	char tidal_album[40];
	if (tidalcache_album_id(state.current_file, tidal_album, sizeof(tidal_album))) {
		char title[256];
		if (!library_track_album(state.current_file, title, sizeof(title)) || !title[0]) {
			device_state_t now;
			device_state_get(&now);
			snprintf(title, sizeof(title), "%s", now.metadata.album);
		}
		player_sheet_close(false);
		if (!tidalpage_open_album(tidal_album, title)) {
			gui_notify_popup("trackmenu_tidal_album_failed");
			return;
		}
		switcher_set_player_return(tidal_list_screen);
		return;
	}

	char album[256];
	if (!library_track_album_value(state.current_file, album, sizeof(album)) || !album[0]) {
		gui_notify_popup("trackmenu_no_album");
		return;
	}

	player_sheet_close(false);
	medialist_open(album, LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, album);
	switcher_set_player_return(medialist_tracks_screen);
}

static void open_settings_cb(void *user) {
	(void)user;
	player_sheet_close(false);
	switch_screen(musicsettings_screen);
	switcher_set_player_return(musicsettings_screen);
}

// Public: the details page for a specific file, from a library list's menu.
// NOT from the player, so backing out returns to the list, not the sheet.
void trackmenu_details_open(const char *path) {
	if (!path || !path[0]) {
		return;
	}
	snprintf(details_path, sizeof(details_path), "%s", path);
	switch_screen(details_screen);
	switcher_set_player_return(NULL);
}

void trackmenu_open(lv_obj_t *anchor) {
	// Two tables rather than one with the label patched at runtime, because of
	// the string-extraction tool: it reads popover_item_t initialisers (see
	// LABEL_STRUCTS in tools/extract_strings.py), not assignments. A
	// "trackmenu_show_podcast" written with an equals sign would never reach the
	// language files and would stay untranslated everywhere.
	//
	// The action behind both is the same: open_album_cb inspects where the
	// playing file came from and decides for itself. Only the wording differs,
	// because a podcast episode has no album and calling it one would be a
	// borrowed term to make a menu line up.
	static const popover_item_t ITEMS[] = {
		{"trackmenu_queue", open_queue_cb, NULL},
		{"add_to_playlist", add_to_playlist_cb, NULL},
		{"show_album", open_album_cb, NULL},
		{"details", open_details_cb, NULL},
		{"settings", open_settings_cb, NULL},
	};
	static const popover_item_t ITEMS_PODCAST[] = {
		{"trackmenu_queue", open_queue_cb, NULL},
		{"add_to_playlist", add_to_playlist_cb, NULL},
		{"trackmenu_show_podcast", open_album_cb, NULL},
		{"details", open_details_cb, NULL},
		{"settings", open_settings_cb, NULL},
	};

	device_state_t state;
	device_state_get(&state);
	bool podcast = state.current_file[0] && podcastcache_is_episode(state.current_file);

	popover_show(anchor, podcast ? ITEMS_PODCAST : ITEMS, 5);
}

void trackmenu_init(gui_config_t *cfg) {
	g_cfg = cfg;
	(void)g_cfg;
	build_queue_page(cfg);
	build_details_page(cfg);
}
