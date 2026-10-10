#include "playlistpage.h"

#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "src/gui/shell/gui.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/library/medialist.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/library/playlists.h"
#include "src/system/core/utils.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/qobuzsync.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/streaming/tidalsync.h"

lv_obj_t *playlistpage_screen;

// Rows are rebuilt on every visit: a card holds a handful of playlists, not
// thousands of tracks, so the browser's windowing machinery would cost more
// than it saves here.

#define ROW_HEIGHT 88
#define ROW_GAP 8
#define ROW_RADIUS 12
#define PLAYLIST_NAME_MAX 200

static gui_config_t *config;
static lv_obj_t *list;
static lv_obj_t *empty_label;
static lv_obj_t *title_label;

// Picker mode: the track waiting for a playlist to be chosen.
static bool picking;
static char pending_track[512];

// Service track id of the pending track, zero for a local file. It decides
// everything that follows: which playlists are listed (the account's instead of
// the card's) and where the track is written when one is tapped.
//
// A local playlist is not an option for a streamed track: the file being played
// lives in the cache, which empties itself, so a local playlist holding it
// would be a list of dead paths within an afternoon.
static long pending_qobuz_id;
static long pending_tidal_id;

// Picker mode for a set of tracks at once (the library lists' selection): the
// paths waiting for a playlist, owned by this page. Always local files.
static char **pending_many;
static int pending_many_count;

static void pending_many_free(void) {
	for (int i = 0; i < pending_many_count; i++) {
		free(pending_many[i]);
	}
	free(pending_many);
	pending_many = NULL;
	pending_many_count = 0;
}

// ---------------------------------------------------------------------------
// Adding a set of tracks
//
// Every track costs a read of its tags and of its length, which for a few
// albums at once is seconds of card access. It runs on a thread of its own, one
// set at a time, under a card that says so and stays until it is done.
// ---------------------------------------------------------------------------

typedef struct {
	char name[PLAYLIST_NAME_MAX + 1];
	char **paths;
	int count;
	int added;
} batch_t;

static pthread_mutex_t batch_lock = PTHREAD_MUTEX_INITIALIZER;

static void batch_done_cb(void *arg) {
	batch_t *b = arg;
	toast_busy_end();
	if (b->added == b->count) {
		toast_success("playlist_tracks_added");
	} else if (b->added > 0) {
		toast_error("playlist_some_tracks_not_added");
	} else {
		gui_notify_popup("playlist_cannot_add_the_tracks");
	}
	for (int i = 0; i < b->count; i++) {
		free(b->paths[i]);
	}
	free(b->paths);
	free(b);
}

static void *batch_worker(void *arg) {
	batch_t *b = arg;
	thread_be_low_priority("playlistadd");
	pthread_mutex_lock(&batch_lock);
	for (int i = 0; i < b->count; i++) {
		if (playlists_add_track(b->name, b->paths[i])) {
			b->added++;
		}
	}
	pthread_mutex_unlock(&batch_lock);
	gui_post(batch_done_cb, b);
	return NULL;
}

bool playlistpage_add_tracks_to(const char *name, const char *const *paths, int count) {
	if (!name || !name[0] || !paths || count <= 0) {
		return false;
	}
	if (!playlists_exists(name) && !playlists_create(name)) {
		return false;
	}
	batch_t *b = calloc(1, sizeof(*b));
	if (!b) {
		return false;
	}
	b->paths = malloc((size_t)count * sizeof(*b->paths));
	if (!b->paths) {
		free(b);
		return false;
	}
	snprintf(b->name, sizeof(b->name), "%s", name);
	for (int i = 0; i < count; i++) {
		b->paths[b->count] = strdup(paths[i] ? paths[i] : "");
		if (!b->paths[b->count]) {
			break;
		}
		b->count++;
	}
	toast_busy("playlist_adding_tracks");
	pthread_t thread;
	if (pthread_create(&thread, NULL, batch_worker, b) != 0) {
		batch_done_cb(b);
		return false;
	}
	pthread_detach(thread);
	return true;
}

// Hands the pending set to a worker that adds it to `name`. False when nothing
// could be started; the set is gone either way.
static bool batch_start(const char *name) {
	batch_t *b = calloc(1, sizeof(*b));
	if (!b) {
		pending_many_free();
		return false;
	}
	snprintf(b->name, sizeof(b->name), "%s", name);
	b->paths = pending_many;
	b->count = pending_many_count;
	pending_many = NULL;
	pending_many_count = 0;

	toast_busy("playlist_adding_tracks");
	pthread_t thread;
	if (pthread_create(&thread, NULL, batch_worker, b) != 0) {
		batch_done_cb(b);
		return false;
	}
	pthread_detach(thread);
	return true;
}

// ---------------------------------------------------------------------------
// Starring a set of tracks
//
// The same kind of work as adding them to a playlist -- a name and an artist
// read for each track -- on the same terms: a thread of its own, one set at a
// time, under a card that stays until it is done. Written in chunks, one
// transaction each.
// ---------------------------------------------------------------------------

#define FAV_CHUNK 64

typedef struct {
	char **paths;
	int count;
	int starred;
} fav_batch_t;

static void fav_done_cb(void *arg) {
	fav_batch_t *b = arg;
	toast_busy_end();
	if (b->starred == b->count) {
		toast_success("favourites_tracks_added");
	} else if (b->starred > 0) {
		toast_error("favourites_some_tracks_not_added");
	} else {
		gui_notify_popup("favourites_cannot_add_the_tracks");
	}
	for (int i = 0; i < b->count; i++) {
		free(b->paths[i]);
	}
	free(b->paths);
	free(b);
}

static void *fav_worker(void *arg) {
	fav_batch_t *b = arg;
	thread_be_low_priority("favouritesadd");

	library_fav_row_t *rows = malloc(FAV_CHUNK * sizeof(*rows));
	char(*titles)[512] = malloc(FAV_CHUNK * sizeof(*titles));
	char(*artists)[256] = malloc(FAV_CHUNK * sizeof(*artists));

	pthread_mutex_lock(&batch_lock);
	for (int first = 0; rows && titles && artists && first < b->count; first += FAV_CHUNK) {
		int n = b->count - first < FAV_CHUNK ? b->count - first : FAV_CHUNK;
		for (int i = 0; i < n; i++) {
			const char *path = b->paths[first + i];
			playlists_track_names(path, titles[i], sizeof(titles[i]), artists[i], sizeof(artists[i]));
			rows[i] = (library_fav_row_t){path, titles[i], artists[i]};
		}
		b->starred += library_fav_add_many(rows, n);
	}
	pthread_mutex_unlock(&batch_lock);

	free(rows);
	free(titles);
	free(artists);
	gui_post(fav_done_cb, b);
	return NULL;
}

void playlistpage_add_favourites(const char *const *paths, int count) {
	if (!paths || count <= 0) {
		return;
	}
	fav_batch_t *b = calloc(1, sizeof(*b));
	if (!b) {
		return;
	}
	b->paths = malloc((size_t)count * sizeof(*b->paths));
	if (!b->paths) {
		free(b);
		return;
	}
	for (int i = 0; i < count; i++) {
		b->paths[b->count] = strdup(paths[i] ? paths[i] : "");
		if (!b->paths[b->count]) {
			break;
		}
		b->count++;
	}

	toast_busy("favourites_adding_tracks");
	pthread_t thread;
	if (pthread_create(&thread, NULL, fav_worker, b) != 0) {
		fav_done_cb(b);
		return;
	}
	pthread_detach(thread);
}

// The naming dialog.
static lv_obj_t *name_layer; // full-screen cover holding the field + keyboard
static lv_obj_t *name_field;
static keyboard_t *name_keyboard;

static void rebuild_rows(void);
static void qobuz_row_clicked_cb(lv_event_t *e);
static void tidal_row_clicked_cb(lv_event_t *e);
static void name_layer_show(const char *initial);

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Opening one
//
// The same windowed list the rest of the library uses: a handle over the
// playlist's own table, four bytes a row, a bandful of rows read back as the
// viewport moves. Nothing is read off the card: which entries are shown, and
// under which names, is the library's to say (library.h).
// ---------------------------------------------------------------------------

static void open_playlist(const char *name) {
	if (playlists_count_tracks(name) <= 0) {
		gui_notify_popup("playlist_empty_playlist");
		return;
	}

	medialist_open(name, LIBRARY_LIST_PLAYLIST, LIBRARY_FILTER_NONE, name);
}

static void row_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return; // a swipe across the list, not a tap
	}

	const char *name = lv_event_get_user_data(e);
	if (!name) {
		return;
	}

	if (!picking) {
		open_playlist(name);
		return;
	}

	if (pending_many_count > 0) {
		batch_start(name);
		back_btn_cb(NULL);
		return;
	}

	if (playlists_add_track(name, pending_track)) {
		toast_success("playlist_track_added");
		back_btn_cb(NULL); // straight back to where the track was tapped
	} else {
		gui_notify_popup("playlist_cannot_add_the_track");
	}
}

// The row's own name string, freed with the row.
static void row_delete_cb(lv_event_t *e) { free(lv_event_get_user_data(e)); }

// Playlist the ellipsis menu is acting on. Deleting confirms first: a playlist
// is something the user built by hand.
static char menu_name[PLAYLIST_NAME_MAX + 1];

// The playlist the naming dialog is renaming, empty when it is creating one.
// The dialog is the same either way -- same field, same keyboard -- and this is
// what tells its accept button which of the two it is finishing.
static char renaming[PLAYLIST_NAME_MAX + 1];

static void do_delete(void *user) {
	(void)user;
	if (!menu_name[0]) {
		return;
	}
	if (playlists_delete(menu_name)) {
		rebuild_rows();
	} else {
		gui_notify_popup("playlist_cannot_delete_the_playlist");
	}
	menu_name[0] = '\0';
}

static void menu_delete_action(void *user) {
	(void)user;
	if (!menu_name[0]) {
		return;
	}
	char message[PLAYLIST_NAME_MAX + 64];
	snprintf(message, sizeof(message), tr("playlist_will_be_removed_its_backup_stays"), menu_name);
	confirm_show("delete_the_playlist", message, "delete", do_delete, NULL);
}

// Renaming opens the same dialog that names a new playlist, with the current
// name already in the field: the new name is a file name either way, and the
// checks it has to pass are the same.
static void menu_rename_action(void *user) {
	(void)user;
	if (!menu_name[0]) {
		return;
	}
	snprintf(renaming, sizeof(renaming), "%s", menu_name);
	name_layer_show(menu_name);
}

// Backup: the playlist written out as a .m3u in the Playlist folder, which is
// what that folder is for now. It is also the way back -- Import reads the
// folder, so a playlist deleted by mistake returns from its backup.
static void menu_backup_action(void *user) {
	(void)user;
	if (!menu_name[0]) {
		return;
	}
	if (playlists_backup(menu_name, NULL, 0)) {
		toast_success("playlist_backed_up");
	} else {
		gui_notify_popup("playlist_cannot_write_the_backup");
	}
	menu_name[0] = '\0';
}

static void menu_btn_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e);
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}

	const char *name = lv_event_get_user_data(e);
	if (!name) {
		return;
	}
	snprintf(menu_name, sizeof(menu_name), "%s", name);

	popover_item_t items[3] = {
		{"rename", menu_rename_action, NULL},
		{"delete", menu_delete_action, NULL},
		{"playlist_backup", menu_backup_action, NULL},
	};
	popover_show(lv_event_get_current_target(e), items, 3);
}

// ---------------------------------------------------------------------------
// The track count under each name
//
// A COUNT over the playlist's own table: no card, no file, nothing to correct
// afterwards. The number is a column of the index, so it costs no lookups and
// needs no worker behind it.
// ---------------------------------------------------------------------------

// `tidal_uuid`, when present, is duplicated and freed with the row. Passing an
// index into the playlist mirror and re-reading it on tap would be shorter, but
// the mirror refreshes in the background: between drawing a row and tapping it
// the index can shift, and the track would land in a different playlist from
// the one on screen. A uuid attached to the row cannot slide.
// Returns the subtitle label, for a caller that has to correct it later.
static lv_obj_t *add_row(const char *name, const char *subtitle, const lv_image_dsc_t *glyph, long qobuz_id,
						 const char *tidal_uuid) {
	char *owned = strdup(name);
	if (!owned) {
		return NULL;
	}

	lv_obj_t *row = lv_btn_create(list);
	lv_obj_set_size(row, lv_pct(100), ROW_HEIGHT);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, ROW_RADIUS, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 16, 0);
	lv_obj_set_style_pad_ver(row, 10, 0);
	lv_obj_set_style_pad_column(row, 14, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(row, true); // the player sheet drags from here too
	char *uuid = NULL;
	if (qobuz_id > 0) {
		// An account playlist has no file to delete, so it carries no menu:
		// tapping it is the only action.
		lv_obj_add_event_cb(row, qobuz_row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)qobuz_id);
	} else if (tidal_uuid && tidal_uuid[0]) {
		uuid = strdup(tidal_uuid);
		if (!uuid) {
			lv_obj_delete(row);
			free(owned);
			return NULL;
		}
		lv_obj_add_event_cb(row, tidal_row_clicked_cb, LV_EVENT_CLICKED, uuid);
		lv_obj_add_event_cb(row, row_delete_cb, LV_EVENT_DELETE, uuid);
	} else {
		lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, owned);
	}
	lv_obj_add_event_cb(row, row_delete_cb, LV_EVENT_DELETE, owned);

	lv_obj_t *icon = lv_image_create(row);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);

	lv_obj_t *texts = lv_obj_create(row);
	lv_obj_set_flex_grow(texts, 1);
	lv_obj_set_height(texts, lv_pct(100));
	lv_obj_set_style_bg_opa(texts, 0, 0);
	lv_obj_set_style_border_width(texts, 0, 0);
	lv_obj_set_style_pad_all(texts, 0, 0);
	lv_obj_set_scrollable(texts, false);
	lv_obj_set_event_bubble(texts, true);
	lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(texts, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *name_label = lv_label_create(texts);
	lv_label_set_text(name_label, name);
	lv_label_set_long_mode(name_label, LV_LABEL_LONG_DOT);
	lv_obj_set_width(name_label, lv_pct(100));
	// One line, cut with dots past it.
	lv_obj_set_height(name_label, lv_font_get_line_height(&font_ui_24));
	lv_obj_add_style(name_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(name_label, &font_ui_24, 0);

	lv_obj_t *sub = NULL;
	if (subtitle && subtitle[0]) {
		sub = lv_label_create(texts);
		lv_label_set_text(sub, subtitle);
		lv_obj_add_style(sub, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(sub, &font_ui_16, 0);
	}

	if (qobuz_id > 0 || uuid) {
		return sub; // no ellipsis: account playlists cannot be deleted from here
	}

	// The ellipsis on the right edge, as on every track row.
	lv_obj_t *menu_btn = lv_btn_create(row);
	lv_obj_set_size(menu_btn, 44, 44);
	lv_obj_set_style_bg_opa(menu_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(menu_btn, 0, 0);
	lv_obj_set_style_shadow_width(menu_btn, 0, 0);
	lv_obj_set_style_pad_all(menu_btn, 0, 0);
	lv_obj_add_event_cb(menu_btn, menu_btn_cb, LV_EVENT_CLICKED, owned);

	lv_obj_t *dots = lv_image_create(menu_btn);
	lv_image_set_src(dots, &icon_ellipsis_vertical);
	lv_obj_add_style(dots, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(dots, LV_OPA_COVER, 0);
	lv_obj_center(dots);

	return sub;
}

// --- account playlists -----------------------------------------------------

// Result of a service write, handed back to the GUI thread.
static char sync_error[192];

static void sync_done_async(void *user) {
	(void)user;
	if (sync_error[0]) {
		gui_notify_popup(sync_error);
		return;
	}
	toast_success("playlist_track_added");
	back_btn_cb(NULL);
}

static void sync_done_cb(bool ok, const char *error, void *user) {
	(void)user;
	// The fallback message names the service actually being written to: this
	// callback serves both, and saying "Qobuz" while adding a track to a Tidal
	// playlist sends the user looking in the wrong place.
	snprintf(sync_error, sizeof(sync_error), "%s",
			 ok ? ""
				: (error && error[0]        ? error
				   : pending_tidal_id > 0 ? tr("tidal_change_refused")
										   : tr("qobuz_change_refused")));
	gui_post(sync_done_async, NULL);
}

// The playlist mirror arrived; redraw if it is still what the page is showing.
static void qobuz_playlists_ready_async(void *user) {
	(void)user;
	if (picking && pending_qobuz_id > 0) {
		rebuild_rows();
	}
}

static void qobuz_playlists_changed(void) { gui_post(qobuz_playlists_ready_async, NULL); }

static void qobuz_row_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	long playlist_id = (long)(intptr_t)lv_event_get_user_data(e);
	if (playlist_id <= 0 || pending_qobuz_id <= 0) {
		return;
	}
	qobuzsync_playlist_add(playlist_id, pending_qobuz_id, sync_done_cb, NULL);
}

static void tidal_row_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	const char *playlist_id = lv_event_get_user_data(e);
	if (!playlist_id || !playlist_id[0] || pending_tidal_id <= 0) {
		return;
	}
	tidalsync_playlist_add(playlist_id, pending_tidal_id, sync_done_cb, NULL);
}

// The Tidal playlist mirror arrived; redraw if it is still what is shown.
static void tidal_playlists_ready_async(void *user) {
	(void)user;
	if (picking && pending_tidal_id > 0) {
		rebuild_rows();
	}
}

static void tidal_playlists_changed(void) { gui_post(tidal_playlists_ready_async, NULL); }

static bool collect_row_cb(const char *name, void *user) {
	int *count = user;

	char subtitle[48];
	int tracks = playlists_count_tracks(name);
	snprintf(subtitle, sizeof(subtitle), tr(tracks == 1 ? "playlist_track_2" : "playlist_tracks"), tracks);
	add_row(name, subtitle, &icon_list_music, 0, NULL);

	(*count)++;
	return true;
}

// ---------------------------------------------------------------------------
// Import
//
// A playlist the player did not write costs a card lookup per entry to make
// sense of -- the stock player's carry several hundred deep Windows paths --
// and one of those on the page is seconds of frozen interface every time it is
// drawn, so such files are not listed. This is the way back in: the file is
// read once, every entry resolved (by its path, or by its file name in the
// index when the path does not answer), and the result written into the
// Playlist folder in the player's own form, which opens without touching the
// card at all.
//
// It runs on a worker: the resolving is exactly the card walk the page refuses
// to do while the user waits.
// ---------------------------------------------------------------------------

#define IMPORT_MAX_CANDIDATES 24

// The warning mark on a row that would replace a playlist: amber, readable on
// the light card and the dark one alike.
#define IMPORT_ALERT_COLOUR 0xE0A030

static lv_obj_t *import_layer;
static lv_obj_t *import_heading;
static lv_obj_t *import_body;	// the scrolling middle: rows, or the report
static lv_obj_t *import_action; // the button along the bottom
static lv_obj_t *import_action_label;
static lv_obj_t *import_btn; // the corner button that opens all this

static playlists_candidate_t import_candidates[IMPORT_MAX_CANDIDATES];
static bool import_selected[IMPORT_MAX_CANDIDATES];
static lv_obj_t *import_ticks[IMPORT_MAX_CANDIDATES];
static int import_count;
// Set on the interface thread, cleared there too -- except by a worker that has
// given up handing its result back, which is why it is volatile.
static volatile bool import_running;

// Bumped whenever the dialog is put away. A worker's result carrying an old
// generation belongs to a dialog that is no longer up: the playlists it wrote
// are on the card either way, but its report has nowhere to go.
static uint32_t import_generation;

typedef struct {
	playlists_candidate_t items[IMPORT_MAX_CANDIDATES];
	int count;
	uint32_t generation;
} import_job_t;

// One playlist's outcome. The worker only counts; the words are put together
// on the interface thread, where the report is drawn.
typedef struct {
	bool imported; // false: nothing in the file could be used
	playlists_import_result_t got; // got.name is the file's name when !imported
} import_entry_t;

typedef struct {
	uint32_t generation;
	int imported;
	int count;
	import_entry_t entries[IMPORT_MAX_CANDIDATES];
} import_result_t;

static void import_layer_hide(void);

// --- the heading -------------------------------------------------------------

// The heading takes the page title's place, and its size: the chevron is put
// away while the dialog is up (the close button is the way out), so the text
// starts at the left margin and runs up to the close button. A translation too
// long for 32 px steps down rather than being cut.
static void import_heading_set(const char *key) {
	static lv_font_t *const STEPS[] = {&font_ui_32, &font_ui_28, &font_ui_26, &font_ui_24};
	const char *text = tr(key);
	int width = lv_obj_get_style_width(import_heading, LV_PART_MAIN);

	lv_label_set_text(import_heading, text);
	for (size_t i = 0; i < sizeof(STEPS) / sizeof(STEPS[0]); i++) {
		lv_point_t size;
		lv_text_get_size(&size, text, STEPS[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		if (size.x <= width || i + 1 == sizeof(STEPS) / sizeof(STEPS[0])) {
			lv_obj_set_style_text_font(import_heading, STEPS[i], 0);
			lv_obj_set_height(import_heading, lv_font_get_line_height(STEPS[i]));
			break;
		}
	}
}

// --- the report, on the interface thread -----------------------------------

static lv_obj_t *report_line(lv_obj_t *parent, const char *text, lv_font_t *font, bool dim) {
	lv_obj_t *label = lv_label_create(parent);
	lv_label_set_text(label, text);
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_add_style(label, dim ? &theme_style_text_dim : &theme_style_text, 0);
	lv_obj_set_style_text_font(label, font, 0);
	return label;
}

// One card per playlist: its name at the size of a list row, then what became
// of its entries, then the names of the ones that were not found.
static void report_add_entry(const import_entry_t *entry) {
	lv_obj_t *card = lv_obj_create(import_body);
	lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, ROW_RADIUS, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_hor(card, 16, 0);
	lv_obj_set_style_pad_ver(card, 14, 0);
	lv_obj_set_style_pad_row(card, 6, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_clickable(card, false);

	const playlists_import_result_t *got = &entry->got;
	report_line(card, got->name, &font_ui_28, false);

	if (!entry->imported) {
		report_line(card, tr("playlist_import_nothing_usable"), &font_ui_22, true);
		return;
	}

	char line[192];
	snprintf(line, sizeof(line), tr("playlist_found_d_missing_d"), got->found, got->missing);
	report_line(card, line, &font_ui_24, false);
	if (got->by_name > 0) {
		snprintf(line, sizeof(line), tr("playlist_d_found_again_by_file_name"), got->by_name);
		report_line(card, line, &font_ui_22, true);
	}
	if (got->missing > 0) {
		report_line(card, tr("playlist_not_found_2"), &font_ui_22, true);
		for (int m = 0; m < got->missing_listed; m++) {
			lv_obj_t *name = report_line(card, got->missing_names[m], &font_ui_20, true);
			lv_obj_set_style_pad_left(name, 14, 0);
		}
		if (got->missing > got->missing_listed) {
			snprintf(line, sizeof(line), tr("playlist_not_listed_d"), got->missing - got->missing_listed);
			lv_obj_t *more = report_line(card, line, &font_ui_20, true);
			lv_obj_set_style_pad_left(more, 14, 0);
		}
	}
}

static void import_show_report(const import_result_t *result) {
	lv_obj_clean(import_body);
	for (int i = 0; i < IMPORT_MAX_CANDIDATES; i++) {
		import_ticks[i] = NULL;
	}

	for (int i = 0; i < result->count; i++) {
		report_add_entry(&result->entries[i]);
	}
	lv_obj_scroll_to_y(import_body, 0, LV_ANIM_OFF);

	import_heading_set("playlist_import_finished");
	lv_label_set_text(import_action_label, tr("ok"));
	lv_obj_set_hidden(import_action, false);
}

static void import_finished(void *user) {
	import_result_t *result = user;
	import_running = false;

	if (result->generation == import_generation && import_layer && !lv_obj_is_hidden(import_layer)) {
		import_show_report(result);
	}
	if (result->imported > 0) {
		rebuild_rows(); // the page has playlists it did not have a moment ago
	}
	free(result);
}

static void *import_worker(void *arg) {
	import_job_t *job = arg;

	// One core, and the user is looking at a dialog that says so.
	thread_be_background("plimport");

	import_result_t *result = calloc(1, sizeof(*result));
	if (!result) {
		free(job);
		return NULL;
	}
	result->generation = job->generation;

	for (int i = 0; i < job->count; i++) {
		import_entry_t *entry = &result->entries[result->count++];
		// Always replacing: a file imported under a name that is taken is the
		// new version of that playlist, every track it no longer lists gone
		// with the old one -- also when an earlier file in this same import
		// has just taken the name.
		entry->imported = playlists_import(job->items[i].path, true, &entry->got);
		if (!entry->imported) {
			memset(&entry->got, 0, sizeof(entry->got));
			snprintf(entry->got.name, sizeof(entry->got.name), "%s", job->items[i].name);
			continue;
		}
		result->imported++;
	}

	free(job);

	// gui_post refuses when its queue is full, and dropping the result would
	// leave the dialog saying "importing" over an import that has finished.
	for (int tries = 0; tries < 50; tries++) {
		if (gui_post(import_finished, result)) {
			return NULL;
		}
		usleep(100000);
	}
	free(result);
	import_running = false;
	return NULL;
}

// --- the dialog ------------------------------------------------------------

static void import_tick_paint(int index) {
	if (!gui_obj_alive(import_ticks[index])) {
		return;
	}
	bool on = import_selected[index];
	lv_obj_set_style_image_recolor(import_ticks[index], on ? theme()->accent : theme()->text_secondary, 0);
	lv_obj_set_style_image_opa(import_ticks[index], on ? LV_OPA_COVER : LV_OPA_30, 0);
}

static void import_row_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= import_count) {
		return;
	}
	import_selected[index] = !import_selected[index];
	import_tick_paint(index);
}

// False when nothing was started -- no playlist ticked, or no thread -- and the
// rows are still there to choose from.
static bool import_start(void) {
	import_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return false;
	}
	for (int i = 0; i < import_count; i++) {
		if (import_selected[i]) {
			job->items[job->count++] = import_candidates[i];
		}
	}
	if (job->count == 0) {
		free(job);
		gui_notify_popup("playlist_choose_at_least_one_playlist");
		return false;
	}
	job->generation = import_generation;

	pthread_t thread;
	if (pthread_create(&thread, NULL, import_worker, job) != 0) {
		free(job);
		gui_notify_popup("playlist_cannot_import_the_playlists");
		return false;
	}
	pthread_detach(thread);
	import_running = true;

	// The waiting state: the rows go, because tapping them now would change a
	// selection the worker has already taken a copy of.
	lv_obj_clean(import_body);
	for (int i = 0; i < IMPORT_MAX_CANDIDATES; i++) {
		import_ticks[i] = NULL;
	}
	lv_obj_t *label = lv_label_create(import_body);
	lv_label_set_text(label, tr("playlist_importing_please_wait"));
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_set_hidden(import_action, true);
	return true;
}

// The import goes ahead from the confirmation, if the dialog it was asked from
// is still up with its rows.
static void import_overwrite_confirmed(void *user) {
	(void)user;
	if (import_running || import_count == 0 || !import_layer || lv_obj_is_hidden(import_layer)) {
		return;
	}
	if (import_start()) {
		import_count = 0;
	}
}

// Ticked playlists whose names are taken are said out loud before anything is
// replaced: the playlists they would replace, by name, and a button that says
// what it does. True when the question was asked and the import now waits for
// the answer.
#define IMPORT_OVERWRITE_NAMED 6

static bool import_confirm_overwrite(void) {
	int taken = 0;
	char names[IMPORT_OVERWRITE_NAMED * 64 + 16] = "";
	size_t used = 0;
	for (int i = 0; i < import_count; i++) {
		if (!import_selected[i] || !import_candidates[i].exists) {
			continue;
		}
		if (taken < IMPORT_OVERWRITE_NAMED) {
			int wrote = snprintf(names + used, sizeof(names) - used, "\n%.60s", import_candidates[i].name);
			if (wrote > 0 && (size_t)wrote < sizeof(names) - used) {
				used += (size_t)wrote;
			}
		} else if (taken == IMPORT_OVERWRITE_NAMED) {
			int wrote = snprintf(names + used, sizeof(names) - used, "\n...");
			if (wrote > 0 && (size_t)wrote < sizeof(names) - used) {
				used += (size_t)wrote;
			}
		}
		taken++;
	}
	if (taken == 0) {
		return false;
	}

	static char message[sizeof(names) + 256];
	snprintf(message, sizeof(message), "%s%s", tr("playlist_overwrite_note"), names);
	confirm_show("playlist_overwrite_title", message, "playlist_overwrite", import_overwrite_confirmed, NULL);
	return true;
}

static void import_action_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	if (import_running) {
		return;
	}
	// The same button finishes both states: it starts the import while there
	// are rows to choose from, and closes the report afterwards.
	if (import_count > 0) {
		if (!import_confirm_overwrite() && import_start()) {
			import_count = 0; // the rows are gone; the next press closes
		}
		return;
	}
	import_layer_hide();
}

static void import_layer_hide(void) {
	if (import_layer) {
		lv_obj_set_hidden(import_layer, true);
	}
	back_btn_hide_on_screen(playlistpage_screen, false);
	import_generation++;
	import_count = 0;
	for (int i = 0; i < IMPORT_MAX_CANDIDATES; i++) {
		import_ticks[i] = NULL;
	}
}

static void import_close_cb(lv_event_t *e) {
	(void)e;
	if (import_running) {
		return; // a worker is holding the card; leaving mid-write helps nobody
	}
	import_layer_hide();
}

char* get_playlist_location_tr(playlists_candidate_t candidate) {
	switch (candidate.playlist_location) {
	case PLAYLIST_LOCATION_SD_ROOT:
		return "playlist_in_the_root_of_the_card";
	case PLAYLIST_LOCATION_PLAYLIST:
		return "playlist_in_the_playlist_folder";
	case PLAYLIST_LOCATION_PLAYLIST_DATA:
		return "playlist_in_the_playlist_data_folder";
	}
	return "playlist_in_the_root_of_the_card";
}

static void import_add_candidate_row(int index) {
	lv_obj_t *row = lv_btn_create(import_body);
	lv_obj_set_size(row, lv_pct(100), ROW_HEIGHT);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, ROW_RADIUS, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 16, 0);
	lv_obj_set_style_pad_ver(row, 10, 0);
	lv_obj_set_style_pad_column(row, 14, 0);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(row, import_row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	lv_obj_t *tick = lv_image_create(row);
	lv_image_set_src(tick, &icon_check);
	lv_obj_add_style(tick, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(tick, LV_OPA_COVER, 0);
	import_ticks[index] = tick;
	import_tick_paint(index);

	lv_obj_t *texts = lv_obj_create(row);
	lv_obj_set_flex_grow(texts, 1);
	lv_obj_set_height(texts, lv_pct(100));
	lv_obj_set_style_bg_opa(texts, 0, 0);
	lv_obj_set_style_border_width(texts, 0, 0);
	lv_obj_set_style_pad_all(texts, 0, 0);
	lv_obj_set_scrollable(texts, false);
	lv_obj_set_clickable(texts, false);
	lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(texts, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *name = lv_label_create(texts);
	lv_label_set_text(name, import_candidates[index].name);
	lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
	lv_obj_set_width(name, lv_pct(100));
	lv_obj_set_height(name, lv_font_get_line_height(&font_ui_24));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	lv_obj_t *where = lv_label_create(texts);
	lv_label_set_text(where,tr(get_playlist_location_tr(import_candidates[index])));
	lv_obj_add_style(where, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(where, &font_ui_16, 0);

	// A playlist by this name is already there, and importing replaces it.
	if (import_candidates[index].exists) {
		lv_obj_t *alert = lv_image_create(row);
		lv_image_set_src(alert, &icon_playlist_overwrite_alert);
		lv_obj_set_style_image_recolor(alert, lv_color_hex(IMPORT_ALERT_COLOUR), 0);
		lv_obj_set_style_image_recolor_opa(alert, LV_OPA_COVER, 0);
	}
}

static void import_btn_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	if (import_running) {
		return;
	}

	import_count = playlists_importable(import_candidates, IMPORT_MAX_CANDIDATES);
	if (import_count == 0) {
		gui_notify_popup("playlist_no_playlists_to_import");
		return;
	}

	lv_obj_clean(import_body);
	for (int i = 0; i < IMPORT_MAX_CANDIDATES; i++) {
		import_ticks[i] = NULL;
		// Everything found, unless unticked -- except what would replace a
		// playlist that is already there, which waits to be asked for.
		import_selected[i] = i < import_count && !import_candidates[i].exists;
	}
	for (int i = 0; i < import_count; i++) {
		import_add_candidate_row(i);
	}

	import_heading_set("playlist_import_playlists");
	lv_label_set_text(import_action_label, tr("playlist_import"));
	lv_obj_set_hidden(import_action, false);
	lv_obj_set_hidden(import_layer, false);
	lv_obj_move_foreground(import_layer);
	lv_obj_scroll_to_y(import_body, 0, LV_ANIM_OFF);
	back_btn_hide_on_screen(playlistpage_screen, true);
}

// ---------------------------------------------------------------------------
// The naming dialog
// ---------------------------------------------------------------------------

static void name_layer_hide(void) {
	if (name_layer) {
		lv_obj_set_hidden(name_layer, true);
	}
	// A rename that was cancelled, or a dialog put away on the way into the
	// page: either way there is no longer a playlist waiting to be renamed, and
	// leaving one here would turn the next "New playlist" into a rename.
	renaming[0] = '\0';
}

static void name_cancel_cb(lv_event_t *e) {
	(void)e;
	name_layer_hide();
}

static void name_accept_cb(lv_event_t *e) {
	(void)e;

	const char *typed = lv_textarea_get_text(name_field);
	char name[PLAYLIST_NAME_MAX + 1];
	snprintf(name, sizeof(name), "%s", typed ? typed : "");

	// Trailing spaces produce a file name that looks identical but is not.
	size_t len = strlen(name);
	while (len > 0 && name[len - 1] == ' ') {
		name[--len] = '\0';
	}
	if (len == 0) {
		gui_notify_popup("name_required");
		return;
	}

	// Renaming comes first: it is the one job of this dialog that acts on a
	// playlist that already exists, so none of what follows applies to it.
	if (renaming[0]) {
		// strcasecmp, not strcmp: on the card "prova" and "Prova" are the same
		// file, so this test has to let a change of capitalisation through --
		// playlists_rename() is the one that knows whether the name really
		// belongs to another playlist, and it refuses if it does.
		if (strcasecmp(renaming, name) != 0 && playlists_exists(name)) {
			gui_notify_popup("playlist_name_taken");
			return;
		}
		if (!playlists_rename(renaming, name)) {
			gui_notify_popup("playlist_cannot_rename_the_playlist");
			return;
		}
		// A track list left open on this playlist still names it the old way,
		// and "Remove from playlist" there would look for a file that is gone.
		medialist_playlist_renamed(renaming, name);
		name_layer_hide(); // which is also what forgets the playlist just renamed
		rebuild_rows();
		return;
	}

	// A Qobuz playlist is created on the account with the track in it, in one
	// call. Nothing is written to the card.
	if (picking && pending_qobuz_id > 0) {
		name_layer_hide();
		qobuzsync_playlist_create_with(name, pending_qobuz_id, sync_done_cb, NULL);
		return;
	}

	if (picking && pending_tidal_id > 0) {
		name_layer_hide();
		tidalsync_playlist_create_with(name, pending_tidal_id, sync_done_cb, NULL);
		return;
	}

	if (playlists_exists(name)) {
		gui_notify_popup("playlist_name_taken");
		return;
	}

	if (!playlists_create(name)) {
		gui_notify_popup("playlist_cannot_create_the_playlist");
		return;
	}

	name_layer_hide();

	if (picking && pending_many_count > 0) {
		batch_start(name);
		back_btn_cb(NULL);
		return;
	}

	if (picking && pending_track[0]) {
		if (playlists_add_track(name, pending_track)) {
			toast_success("playlist_track_added");
			back_btn_cb(NULL); // straight back to where the track was tapped
			return;
		}
		gui_notify_popup("playlist_cannot_add_the_track");
	}

	rebuild_rows();
}

static void name_layer_show(const char *initial) {
	lv_textarea_set_text(name_field, initial ? initial : "");
	keyboard_reset(name_keyboard);

	// LVGL restarts the caret blink on focus, and nothing ever focuses this
	// field because the keyboard types into it without it being tapped. Sending
	// the event by hand avoids a frozen caret here while the search page blinks.
	lv_obj_add_state(name_field, LV_STATE_FOCUSED);
	lv_obj_send_event(name_field, LV_EVENT_FOCUSED, NULL);
	lv_obj_set_hidden(name_layer, false);
	lv_obj_move_foreground(name_layer);
}

static void new_playlist_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	(void)e;
	renaming[0] = '\0';
	name_layer_show(NULL);
}

// ---------------------------------------------------------------------------
// Building
// ---------------------------------------------------------------------------

// The revision the rows on screen were counted at. A track taken out of a
// playlist from inside the playlist changes a number on this page, and coming
// back to it with the chevron does not go through playlistpage_open() -- so the
// page checks, on the way in, whether anything it drew has moved since.
static unsigned rows_revision;

static void rebuild_rows(void) {
	rows_revision = library_revision(LIBRARY_LIST_PLAYLIST);
	lv_obj_clean(list);

	// The "new playlist" row comes first in both modes: it is the only way the
	// first playlist can be made.
	lv_obj_t *new_row = lv_btn_create(list);
	lv_obj_set_size(new_row, lv_pct(100), ROW_HEIGHT);
	lv_obj_add_style(new_row, &theme_style_card, 0);
	lv_obj_add_style(new_row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(new_row, ROW_RADIUS, 0);
	lv_obj_set_style_border_width(new_row, 0, 0);
	lv_obj_set_style_shadow_width(new_row, 0, 0);
	lv_obj_set_style_pad_hor(new_row, 16, 0);
	lv_obj_set_style_pad_column(new_row, 14, 0);
	lv_obj_set_flex_flow(new_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(new_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_event_bubble(new_row, true);
	lv_obj_add_event_cb(new_row, new_playlist_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *new_icon = lv_image_create(new_row);
	lv_image_set_src(new_icon, &icon_plus);
	lv_obj_add_style(new_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor(new_icon, theme()->accent, 0);
	lv_obj_set_style_image_recolor_opa(new_icon, LV_OPA_COVER, 0);

	lv_obj_t *new_label = lv_label_create(new_row);
	lv_label_set_text(new_label, tr("playlist_new_playlist"));
	lv_obj_add_style(new_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(new_label, &font_ui_24, 0);
	lv_obj_set_style_text_color(new_label, theme()->accent, 0);

	int count = 0;
	if (picking && pending_qobuz_id > 0) {
		qobuz_playlist_t got[QOBUZSYNC_PLAYLISTS_MAX];
		int total = qobuzsync_playlists(got, QOBUZSYNC_PLAYLISTS_MAX);
		for (int i = 0; i < total; i++) {
			char subtitle[48];
			snprintf(subtitle, sizeof(subtitle), tr(got[i].track_count == 1 ? "playlist_track_2" : "playlist_tracks"),
					 got[i].track_count);
			add_row(got[i].name, subtitle, &icon_list_music, got[i].id, NULL);
			count++;
		}
		// Nothing has arrived yet: ask for it, and the callback calls this
		// function again once it does.
		if (!qobuzsync_playlists_known()) {
			qobuzsync_playlists_refresh(qobuz_playlists_changed);
		}
	} else if (picking && pending_tidal_id > 0) {
		tidal_playlist_t got[TIDALSYNC_PLAYLISTS_MAX];
		int total = tidalsync_playlists(got, TIDALSYNC_PLAYLISTS_MAX);
		for (int i = 0; i < total; i++) {
			char subtitle[48];
			snprintf(subtitle, sizeof(subtitle), tr(got[i].track_count == 1 ? "playlist_track_2" : "playlist_tracks"),
					 got[i].track_count);
			add_row(got[i].name, subtitle, &icon_list_music, 0, got[i].id);
			count++;
		}
		if (!tidalsync_playlists_known()) {
			tidalsync_playlists_refresh(tidal_playlists_changed);
		}
	} else {
		playlists_for_each(collect_row_cb, &count);
	}

	if (count == 0) {
		lv_obj_set_hidden(empty_label, false);
	} else {
		lv_obj_set_hidden(empty_label, true);
	}
}

void playlistpage_open(void) {
	picking = false;
	pending_track[0] = '\0';
	pending_many_free();
	pending_qobuz_id = 0;
	pending_tidal_id = 0;
	lv_label_set_text(title_label, tr("playlists"));
	name_layer_hide();
	import_layer_hide();
	lv_obj_set_hidden(import_btn, false);
	rebuild_rows();
	switch_screen(playlistpage_screen);
}

void playlistpage_add_track(const char *track_path) {
	if (!track_path || !track_path[0]) {
		return;
	}
	picking = true;
	pending_many_free();
	snprintf(pending_track, sizeof(pending_track), "%s", track_path);
	pending_qobuz_id = qobuzcache_track_id(track_path);
	pending_tidal_id = tidalcache_track_id(track_path);
	// The title states where the track is about to be written: an account or
	// the card. The two caches live in different directories, so at most one of
	// the two ids can be non-zero.
	lv_label_set_text(title_label, pending_qobuz_id > 0   ? tr("playlist_qobuz_playlist")
						   : pending_tidal_id > 0 ? tr("playlist_tidal_playlist")
												  : tr("playlist_add_to_playlist_2"));
	name_layer_hide();
	import_layer_hide();
	// While the page is choosing where a track goes it is not somewhere to
	// import from: the corner button would open a dialog over a half-finished
	// action.
	lv_obj_set_hidden(import_btn, true);
	rebuild_rows();
	switch_screen(playlistpage_screen);
}

void playlistpage_add_tracks(const char *const *paths, int count) {
	pending_many_free();
	if (!paths || count <= 0) {
		return;
	}
	pending_many = malloc((size_t)count * sizeof(*pending_many));
	if (!pending_many) {
		return;
	}
	for (int i = 0; i < count; i++) {
		pending_many[pending_many_count] = strdup(paths[i] ? paths[i] : "");
		if (!pending_many[pending_many_count]) {
			pending_many_free();
			return;
		}
		pending_many_count++;
	}

	picking = true;
	pending_track[0] = '\0';
	pending_qobuz_id = 0;
	pending_tidal_id = 0;
	lv_label_set_text(title_label, tr("playlist_add_to_playlist_2"));
	name_layer_hide();
	import_layer_hide();
	lv_obj_set_hidden(import_btn, true);
	rebuild_rows();
	switch_screen(playlistpage_screen);
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	if (rows_revision != library_revision(LIBRARY_LIST_PLAYLIST)) {
		rebuild_rows();
	}
}

void playlistpage_init(gui_config_t *cfg) {
	config = cfg;

	playlistpage_screen = lv_obj_create(NULL);
	lv_obj_add_style(playlistpage_screen, &theme_style_screen, 0);
	title_label = settingsrow_title(playlistpage_screen, cfg, "playlists");

	int content_top = settingsrow_content_top(cfg);

	list = lv_obj_create(playlistpage_screen);
	lv_obj_set_size(list, cfg->screen_width, cfg->screen_height - content_top);
	lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(list, 0, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_radius(list, 0, 0);
	lv_obj_set_style_pad_hor(list, cfg->padding, 0);
	lv_obj_set_style_pad_bottom(list, 12, 0);
	lv_obj_set_style_pad_gap(list, ROW_GAP, 0);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_event_bubble(list, true);

	empty_label = lv_label_create(playlistpage_screen);
	lv_label_set_text(empty_label, tr("playlist_empty_note"));
	lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(empty_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(empty_label, &font_ui_24, 0);
	lv_obj_align(empty_label, LV_ALIGN_TOP_MID, 0, content_top + 140);
	lv_obj_set_hidden(empty_label, true);

	// --- the naming dialog: a full-screen layer with the field at the top and
	// the shared keyboard at the bottom, matching the search page.
	name_layer = lv_obj_create(playlistpage_screen);
	lv_obj_set_size(name_layer, cfg->screen_width, cfg->screen_height);
	lv_obj_align(name_layer, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(name_layer, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(name_layer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(name_layer, 0, 0);
	lv_obj_set_style_radius(name_layer, 0, 0);
	lv_obj_set_style_pad_all(name_layer, 0, 0);
	lv_obj_set_scrollable(name_layer, false);
	lv_obj_set_hidden(name_layer, true);

	lv_obj_t *heading = lv_label_create(name_layer);
	lv_label_set_text(heading, tr("playlist_name"));
	lv_obj_add_style(heading, &theme_style_text, 0);
	lv_obj_set_style_text_font(heading, &font_ui_24, 0);
	// Offset clear of the floating back chevron, which sits on top of this
	// layer; the same offset every page title uses.
	lv_obj_align(heading, LV_ALIGN_TOP_LEFT, cfg->padding + 56 + 14, cfg->padding + cfg->top_bar_height + 10);

	lv_obj_t *cancel = lv_btn_create(name_layer);
	lv_obj_set_size(cancel, 56, 56);
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
	lv_textarea_set_max_length(name_field, PLAYLIST_NAME_MAX);
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

	// --- the import button, in the title row's corner ------------------------
	import_btn = lv_btn_create(playlistpage_screen);
	lv_obj_set_size(import_btn, 56, 56);
	lv_obj_align(import_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_set_style_bg_opa(import_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(import_btn, 0, 0);
	lv_obj_set_style_shadow_width(import_btn, 0, 0);
	lv_obj_set_style_pad_all(import_btn, 0, 0);
	lv_obj_add_event_cb(import_btn, import_btn_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *import_glyph = lv_image_create(import_btn);
	lv_image_set_src(import_glyph, &icon_import);
	lv_obj_add_style(import_glyph, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(import_glyph, LV_OPA_COVER, 0);
	lv_obj_center(import_glyph);

	// --- the import dialog: the same full-screen layer shape as the naming one
	import_layer = lv_obj_create(playlistpage_screen);
	lv_obj_set_size(import_layer, cfg->screen_width, cfg->screen_height);
	lv_obj_align(import_layer, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(import_layer, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(import_layer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(import_layer, 0, 0);
	lv_obj_set_style_radius(import_layer, 0, 0);
	lv_obj_set_style_pad_all(import_layer, 0, 0);
	lv_obj_set_scrollable(import_layer, false);
	lv_obj_set_hidden(import_layer, true);

	// No chevron while this layer is up, so the heading starts at the margin
	// and stops short of the close button.
	import_heading = lv_label_create(import_layer);
	lv_obj_add_style(import_heading, &theme_style_text, 0);
	lv_label_set_long_mode(import_heading, LV_LABEL_LONG_DOT);
	lv_obj_set_width(import_heading, cfg->screen_width - 2 * cfg->padding - 56 - 14);
	lv_obj_align(import_heading, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding + cfg->top_bar_height + 10);
	import_heading_set("playlist_import_playlists");

	lv_obj_t *import_close = lv_btn_create(import_layer);
	lv_obj_set_size(import_close, 56, 56);
	lv_obj_align(import_close, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_set_style_bg_opa(import_close, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(import_close, 0, 0);
	lv_obj_set_style_shadow_width(import_close, 0, 0);
	lv_obj_set_style_pad_all(import_close, 0, 0);
	lv_obj_add_event_cb(import_close, import_close_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *import_close_icon = lv_image_create(import_close);
	lv_image_set_src(import_close_icon, &icon_close);
	lv_obj_add_style(import_close_icon, &theme_style_icon, 0);
	lv_obj_center(import_close_icon);

	// The button sits on the bottom edge; the list has whatever is left.
	import_action = lv_btn_create(import_layer);
	lv_obj_set_size(import_action, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_align(import_action, LV_ALIGN_BOTTOM_MID, 0, -cfg->padding);
	lv_obj_set_style_bg_color(import_action, theme()->accent, 0);
	lv_obj_set_style_border_width(import_action, 0, 0);
	lv_obj_set_style_shadow_width(import_action, 0, 0);
	lv_obj_set_style_radius(import_action, 12, 0);
	lv_obj_add_event_cb(import_action, import_action_cb, LV_EVENT_CLICKED, NULL);

	import_action_label = lv_label_create(import_action);
	lv_label_set_text(import_action_label, tr("playlist_import"));
	lv_obj_set_style_text_font(import_action_label, &font_ui_24, 0);
	lv_obj_set_style_text_color(import_action_label, lv_color_white(), 0);
	lv_obj_center(import_action_label);

	int import_top = content_top;
	import_body = lv_obj_create(import_layer);
	lv_obj_set_size(import_body, cfg->screen_width, cfg->screen_height - import_top - 62 - 2 * cfg->padding);
	lv_obj_align(import_body, LV_ALIGN_TOP_LEFT, 0, import_top);
	lv_obj_set_style_bg_opa(import_body, 0, 0);
	lv_obj_set_style_border_width(import_body, 0, 0);
	lv_obj_set_style_radius(import_body, 0, 0);
	lv_obj_set_style_pad_hor(import_body, cfg->padding, 0);
	lv_obj_set_style_pad_ver(import_body, 4, 0);
	lv_obj_set_style_pad_gap(import_body, ROW_GAP, 0);
	lv_obj_set_scroll_dir(import_body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(import_body, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_flex_flow(import_body, LV_FLEX_FLOW_COLUMN);

	lv_obj_add_event_cb(playlistpage_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	switcher_attach_back_gesture(playlistpage_screen);
	player_sheet_attach_drag(playlistpage_screen, true);
}
