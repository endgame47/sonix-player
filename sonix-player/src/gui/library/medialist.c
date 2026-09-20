#include "medialist.h"

#include <limits.h>

#include "src/system/library/playlists.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/system/core/config.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"
#include "src/system/playback/playlist.h"
#include "src/system/device/power.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"

lv_obj_t *medialist_screen;
lv_obj_t *medialist_tracks_screen;
lv_obj_t *medialist_albums_screen;

// The exact geometry of the file browser's rows, so the two lists are twins.
#define ROW_HEIGHT 100
#define ROW_GAP 8
#define ROW_PITCH (ROW_HEIGHT + ROW_GAP)
#define ROW_RADIUS 12
#define ROW_PAD 14
#define THUMB_SIZE 72
#define QUALITY_GAP 4 // between a title and the badge under it
#define ROW_POOL 12

// How many rows are held in RAM for a query-backed list: the twelve on screen
// plus enough either side that a flick does not fetch on every frame. At about
// 770 bytes a row this is 36 KB, whatever the size of the library.
#define WINDOW_ROWS 48

// The now-playing mark: a short rounded bar down the left edge of the row.
// It lives INSIDE the row's left padding, so nothing on the row moves to make
// space for it -- the thumbnail still starts at ROW_PAD whether the mark is
// there or not.
#define PLAYMARK_WIDTH 6
#define PLAYMARK_HEIGHT 52
#define PLAYMARK_INSET 4 // from the row's left edge

// Where in the coverloader's slot table each pool lives (the browser owns
// 0..9; see coverloader.h).
#define SLOT_BASE_NAMES 12
#define SLOT_BASE_TRACKS 24
#define SLOT_BASE_ARTIST_ALBUMS 72

// A hard ceiling on how many rows are copied into RAM, for the one kind of list
// that still copies them: a playlist file handed in as an array of paths. A
// query-backed list has no ceiling -- it holds row ids and reads the rows a
// windowful at a time -- so a library is as large as the card allows.
#define MAX_ENTRIES 20000

// The corner buttons on the title row. Two is the most that ever show at once:
// circle-play beside the sort direction on All tracks and on Albums, the album
// grouping beside circle-play on an artist's tracks, shuffle beside the reverse
// on Favourites. The strip is sized for that many whatever a particular list
// shows -- see build_panel() for what happens when it is not.
#define CORNER_BUTTON_SIZE 56
#define CORNER_GAP 4
#define CORNER_MAX_BUTTONS 2

// How often finished artwork is collected while any is pending.
#define THUMB_POLL_MS 150

// ---------------------------------------------------------------------------
// The A-Z strip
//
// A column of letters down the right edge of the long alphabetical lists, the
// way a phone's contacts do it: it appears while the list is moving, a press
// anywhere on it jumps to that letter, and sliding a finger down it walks the
// alphabet. The letter under the finger is drawn large in the middle of the
// screen, because a finger on the strip covers the strip.
//
// '#' comes first because that is where the list itself starts: digits and
// punctuation sort above A (see library_index_letter).
// ---------------------------------------------------------------------------
// '#', A to Z, and one at the end for everything the collation files BELOW Z:
// Cyrillic, Greek, kana, Hangul, CJK. Without it those rows sit under the strip
// with no letter pointing at them, which on a card with Russian tags reads as
// "the artist is not in the list at all".
//
// The names are library.c's: the handles count their rows into these same
// buckets, and two spellings of the same number is how they would drift apart.
#define INDEX_BUCKETS LIBRARY_INDEX_BUCKETS
#define INDEX_PAST_Z_SLOT LIBRARY_INDEX_PAST_Z_SLOT
#define INDEX_BAR_WIDTH 28
#define INDEX_HIDE_MS 2500	// no scrolling and no touch for this long: it goes
#define INDEX_HINT_MS 450	// the big letter outstays the finger by a moment
#define INDEX_MIN_ROWS 30	// shorter lists are quicker to just scroll
#define INDEX_ENGAGE_PX 6	// upward or downward movement that claims the press

typedef struct {
	char *name;
	char *path; // the track's file; on album rows a representative track
				// (cover source); NULL on the other name lists
} entry_t;

// One row of a query-backed list, as read back from the database. Fixed
// buffers rather than pointers: the whole point is that there are only ever
// WINDOW_ROWS of these, so a few kilobytes bought once beats an allocation per
// row bound.
typedef struct {
	char name[256];
	char path[512];
	bool has_path;
} winrow_t;

typedef struct {
	lv_obj_t *button;
	lv_obj_t *icon;
	lv_obj_t *text;	 // title and badge, stacked, so one does not sit on the other
	lv_obj_t *label;
	lv_obj_t *quality;	// the badge under the title on track rows; NULL elsewhere
	lv_obj_t *menu_btn; // the ellipsis on track rows; NULL on name lists
	lv_obj_t *playmark; // the accent bar shown on the playing row
	int index;

	bool has_thumb;
	bool thumb_requested;
	bool thumb_settled;
	cover_image_t thumb;
} row_t;

// Everything one of the two screens owns.
typedef struct {
	lv_obj_t *screen;
	lv_obj_t *title_label;
	lv_obj_t *list;	 // the scrollable viewport
	lv_obj_t *body;	 // fixed-height canvas the rows are placed on
	lv_obj_t *empty; // "nothing here" message
	lv_obj_t *corner;	   // the right-hand button strip on the title row
	lv_obj_t *reverse_btn; // Favourites: the list reversed (newest first)
	lv_obj_t *play_btn;	   // the circle-play corner button
	lv_obj_t *play_menu;   // and the card it opens, one per screen
	lv_obj_t *sort_btn;	   // A-Z / Z-A
	lv_obj_t *sort_icon;
	lv_obj_t *album_btn;   // tracks screen only: group an artist's tracks by album
	lv_obj_t *reorder_btn; // playlists only: put the list in hand-order mode
	bool reordering;	   // and whether it is in it now
	bool reorder_moved;	   // whether anything actually moved while it was
	gui_config_t *cfg;	 // kept for the heading, which is re-measured per list
	row_t rows[ROW_POOL];

	// A list is either a query -- which is held as a handle over its row ids,
	// four bytes a row, with the rows themselves read back a windowful at a
	// time -- or an arbitrary list of paths handed in by the caller, which has
	// no query behind it and is still kept whole. `from_paths` says which.
	library_index_t *ix;
	library_order_t list_order; // what the handle was opened with, to open it again
	bool list_desc;
	bool index_wanted; // whether this KIND of list has a strip at all
	winrow_t *window; // WINDOW_ROWS of them, allocated once with the panel
	int window_first; // index of window[0]; -1 when nothing is cached
	int window_count;

	// Path-backed lists only, with a count of its own: p->count below is the
	// length of the LIST, which for a query-backed one comes from the handle
	// and says nothing at all about this array.
	entry_t *entries;
	int entries_count;

	int count;

	bool is_tracks;		 // which flavour of row this screen shows
	int slot_base;		 // this panel's coverloader slots
	library_list_t kind; // what the name list is listing (albums, artists...)

	// Enough to reload the very same list under a different order, which is
	// what the corner buttons do.
	library_filter_t filter;
	char filter_value[256];
	char title[160];
	bool from_paths; // filled by medialist_open_paths: not a query, cannot reorder
	// The playlist this list came from, empty when it came from anywhere else.
	// It is what lets the ellipsis offer to take a track out of the list it is
	// looking at, rather than to put it into another one.
	char playlist[NAME_MAX + 1];

	// Which list is loaded (kind|filter|value) and where it was scrolled to,
	// so reopening the same list puts the user back where they were.
	char signature[600];
	int saved_scroll;

	// The A-Z strip down the right edge and the letter it shows in the middle
	// of the screen while a finger is on it. index_row[] is the strip read from
	// top to bottom -- already turned round on a Z-A list -- and holds the row
	// each letter jumps to.
	lv_obj_t *index_bar;
	lv_obj_t *index_letters[INDEX_BUCKETS];
	lv_obj_t *index_hint;
	lv_obj_t *index_hint_label;
	int index_row[INDEX_BUCKETS];
	bool index_enabled; // this list is one the strip makes sense on
	bool index_desc;	// the list runs Z-A, so the strip does too
	uint32_t index_wanted_at;
	uint32_t index_hint_at;
	// Where a press on the strip started, and whether it has turned out to be
	// the strip's rather than the page's. Until it has, nothing moves.
	lv_point_t index_press;
	bool index_engaged;
} panel_t;

static panel_t panel_names;
static panel_t panel_tracks;
// One artist's records: a name list like panel_names, on a screen of its own so
// it can sit between the artist list and an album's tracks.
static panel_t panel_artist_albums;

static lv_timer_t *thumb_timer;

// ---------------------------------------------------------------------------
// Ordering, remembered
//
// Per list kind, because "Z-A" is a decision about a particular list and not a
// mood: setting Albums to Z-A and finding Artists reversed too would be a
// surprise. Kept as one bitmask so it is one line of config rather than five.
// ---------------------------------------------------------------------------

static unsigned sort_desc_mask;
static bool artist_album_order; // an artist's tracks grouped by record

// Whether an artist opens as a list of their records. On by default: an artist
// with a dozen albums is a dozen rows this way and four hundred the other.
static bool album_view = true;

// Whether a track row wears its quality badge. Off by default: it is one more
// thing on every row, and a library of mp3s would wear the same badge all the
// way down.
static bool quality_badges;

// Both are read on first use rather than in medialist_init(): the music
// settings page is built before it (see gui_init), so a switch built from these
// at init time would show the default rather than what the user chose, and then
// write that default back the first time it was touched.
static bool view_settings_loaded;

static void load_view_settings(void) {
	if (view_settings_loaded) {
		return;
	}
	view_settings_loaded = true;
	album_view = config_get_int("library", "album_view", 1) != 0;
	quality_badges = config_get_int("library", "quality_badges", 0) != 0;
}

bool medialist_album_view(void) {
	load_view_settings();
	return album_view;
}

void medialist_set_album_view(bool on) {
	load_view_settings();
	album_view = on;
	config_set_int("library", "album_view", on ? 1 : 0);
	config_save();
}

bool medialist_quality_badges(void) {
	load_view_settings();
	return quality_badges;
}

static bool sort_is_desc(library_list_t kind) { return (sort_desc_mask & (1u << (unsigned)kind)) != 0; }

static void sort_set_desc(library_list_t kind, bool desc) {
	if (desc) {
		sort_desc_mask |= 1u << (unsigned)kind;
	} else {
		sort_desc_mask &= ~(1u << (unsigned)kind);
	}
	config_set_int("library", "sort_desc", (long)sort_desc_mask);
	config_save();
}

// ---------------------------------------------------------------------------
// model
// ---------------------------------------------------------------------------

static void model_clear(panel_t *p) {
	// Its own count, not p->count. They agree on a path-backed list and have
	// nothing to do with each other on a query-backed one, where entries is
	// NULL while p->count is a whole library -- which is a walk off the front
	// of a null pointer on the second list opened in the same panel.
	for (int i = 0; i < p->entries_count; i++) {
		free(p->entries[i].name);
		free(p->entries[i].path);
	}
	free(p->entries);
	p->entries = NULL;
	p->entries_count = 0;
	p->count = 0;

	library_index_close(p->ix);
	p->ix = NULL;
	p->window_first = -1;
	p->window_count = 0;
}

// ---------------------------------------------------------------------------
// Reading a row
//
// Every part of the page that draws or acts on a row goes through here, so
// neither backend is visible anywhere else. The strings belong to the panel and
// stay put only until the window next moves, which is why the callers that hold
// on to one copy it first.
// ---------------------------------------------------------------------------

static void index_rebuild(panel_t *p, bool enabled, bool descending);
static void row_drop_thumb(panel_t *p, row_t *row);

typedef struct {
	panel_t *panel;
	int filled;
} window_fill_t;

static bool window_fill_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)artist;
	window_fill_t *fill = user;
	if (fill->filled >= WINDOW_ROWS) {
		return false;
	}
	winrow_t *row = &fill->panel->window[fill->filled++];
	snprintf(row->name, sizeof(row->name), "%s", name && name[0] ? name : tr("medialist_no_name"));
	row->has_path = path != NULL;
	snprintf(row->path, sizeof(row->path), "%s", path ? path : "");
	return true;
}

// Row ids are only a promise until something rewrites the table, and a rescan
// or a card coming back rewrites all of them. The handle notices; this is what
// acts on it -- the same list, asked for again. Without it the page keeps a
// handle that hands out nothing and never recovers.
static void panel_refresh_stale(panel_t *p) {
	if (p->from_paths || !p->ix || !library_index_stale(p->ix)) {
		return;
	}

	library_index_t *fresh =
		library_index_open(p->kind, p->filter, p->filter_value, p->list_order, p->list_desc);
	library_index_close(p->ix);
	p->ix = fresh;
	p->count = library_index_count(fresh);
	p->window_first = -1;
	p->window_count = 0;

	// The rows underneath have moved, so every pooled row has to be bound
	// again, and the letters have to be counted again.
	for (int i = 0; i < ROW_POOL; i++) {
		row_drop_thumb(p, &p->rows[i]);
		p->rows[i].index = -2;
	}
	lv_obj_set_height(p->body, p->count ? p->count * ROW_PITCH : ROW_PITCH);
	if (p->count == 0) {
		lv_obj_remove_flag(p->empty, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(p->empty, LV_OBJ_FLAG_HIDDEN);
	}
	index_rebuild(p, p->index_wanted, p->index_desc);
}

// Brings `index` into the cached band, reading a windowful around it. The band
// is placed a few rows before the wanted one so that scrolling back up does not
// refetch on every row.
static bool window_cover(panel_t *p, int index) {
	if (!p->ix || index < 0 || index >= p->count) {
		return false;
	}
	if (!p->window) {
		// Bought once per panel and kept: two of these for the life of the
		// process is seventy kilobytes, against an allocation per row bound.
		p->window = calloc(WINDOW_ROWS, sizeof(*p->window));
		if (!p->window) {
			return false;
		}
	}
	if (p->window_first >= 0 && index >= p->window_first && index < p->window_first + p->window_count) {
		return true;
	}

	int first = index - ROW_POOL;
	if (first < 0) {
		first = 0;
	}
	if (first + WINDOW_ROWS > p->count) {
		first = p->count - WINDOW_ROWS;
	}
	if (first < 0) {
		first = 0;
	}

	window_fill_t fill = {.panel = p, .filled = 0};
	library_index_window(p->ix, first, WINDOW_ROWS, window_fill_cb, &fill);
	p->window_first = first;
	p->window_count = fill.filled;
	return index >= first && index < first + fill.filled;
}

// The name of a row, and its path when it has one. False when the row is not
// there -- a stale handle, or an index past the end.
static bool row_at(panel_t *p, int index, const char **name_out, const char **path_out) {
	if (index < 0 || index >= p->count) {
		return false;
	}
	if (p->from_paths) {
		if (name_out) {
			*name_out = p->entries[index].name;
		}
		if (path_out) {
			*path_out = p->entries[index].path;
		}
		return true;
	}
	if (!window_cover(p, index)) {
		return false;
	}
	const winrow_t *row = &p->window[index - p->window_first];
	if (name_out) {
		*name_out = row->name;
	}
	if (path_out) {
		*path_out = row->has_path ? row->path : NULL;
	}
	return true;
}

// The same, copied out, for a caller that has to keep it across another read.
static bool row_path_copy(panel_t *p, int index, char *out, size_t size) {
	const char *path = NULL;
	if (!row_at(p, index, NULL, &path) || !path) {
		if (size > 0) {
			out[0] = '\0';
		}
		return false;
	}
	snprintf(out, size, "%s", path);
	return true;
}

typedef struct {
	panel_t *panel;
	int capacity;
} load_ctx_t;

static bool load_row_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)artist;
	load_ctx_t *ctx = user;
	panel_t *p = ctx->panel;

	if (p->count >= MAX_ENTRIES) {
		return false;
	}

	if (p->count == ctx->capacity) {
		int grown = ctx->capacity ? ctx->capacity * 2 : 256;
		entry_t *bigger = realloc(p->entries, (size_t)grown * sizeof(entry_t));
		if (!bigger) {
			return false;
		}
		p->entries = bigger;
		ctx->capacity = grown;
	}

	p->entries[p->count].name = strdup(name && name[0] ? name : tr("medialist_no_name"));
	p->entries[p->count].path = path ? strdup(path) : NULL;
	if (!p->entries[p->count].name) {
		return false;
	}
	p->count++;
	p->entries_count = p->count;
	return true;
}

// ---------------------------------------------------------------------------
// rows, windowing and artwork (the browser's technique)
// ---------------------------------------------------------------------------

static int row_slot(panel_t *p, const row_t *row) { return p->slot_base + (int)(row - p->rows); }

// What a row shows before its artwork arrives, or instead of it. One glyph per
// kind of name, rather than the same icon on every row: these lists are about
// what kind of name they hold, and the glyph says that without reading the
// title.
static const lv_image_dsc_t *glyph_for(const panel_t *p) {
	if (p->is_tracks) {
		return &icon_music2;
	}
	switch (p->kind) {
	case LIBRARY_LIST_ARTISTS:
		return &icon_artist;
	case LIBRARY_LIST_ALBUM_ARTISTS:
		return &icon_artist_album;
	case LIBRARY_LIST_GENRES:
		return &icon_genre;
	case LIBRARY_LIST_ALBUMS:
		return &icon_album;
	default:
		return &icon_folder;
	}
}

static void row_show_glyph(panel_t *p, row_t *row) {
	lv_image_set_src(row->icon, glyph_for(p));
	lv_obj_add_style(row->icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);
}

// The plain name lists -- Artists, Album artists, Genres -- carry no icon: the
// same glyph on every row distinguishes nothing, and without it the name gets
// the full width. Albums keeps its icon, since there it becomes the cover art,
// and so do tracks.
static bool panel_shows_icons(const panel_t *p) {
	if (p->is_tracks) {
		return true;
	}
	switch (p->kind) {
	case LIBRARY_LIST_ARTISTS:
	case LIBRARY_LIST_ALBUM_ARTISTS:
	case LIBRARY_LIST_GENRES:
		return false;
	default:
		return true;
	}
}

// Album art must not be tinted, so the recolour the glyphs rely on is
// switched off for as long as a picture is on the row.
static void row_show_cover(row_t *row, const lv_image_dsc_t *dsc) {
	lv_image_set_src(row->icon, dsc);
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_TRANSP, 0);
}

static void row_drop_thumb(panel_t *p, row_t *row) {
	coverloader_release(row_slot(p, row));

	if (row->has_thumb) {
		row_show_glyph(p, row); // stop pointing at the pixels before they go
		cover_free(&row->thumb);
		row->has_thumb = false;
	}

	row->thumb_requested = false;
	row->thumb_settled = false;
}

// ---------------------------------------------------------------------------
// The now-playing mark
//
// A rounded accent bar down the left edge of whichever row the playing track
// is on -- and, on the name lists, of the album/artist/genre that track
// belongs to, so walking into Album shows which record is on.
//
// What is playing is cached here rather than asked per row: binding a row
// happens on every scroll step, and this turns that into four string compares
// against memory instead of a database query per row.
// ---------------------------------------------------------------------------

static char np_path[512];
static char np_album[256];
static char np_artist[256];
static char np_album_artist[256];
static char np_genre[128];

static void np_cache_refresh(void) {
	device_state_t state;
	device_state_get(&state);

	snprintf(np_path, sizeof(np_path), "%s", state.current_file);
	snprintf(np_album, sizeof(np_album), "%s", state.metadata.album);
	snprintf(np_artist, sizeof(np_artist), "%s", state.metadata.artist);
	snprintf(np_album_artist, sizeof(np_album_artist), "%s", state.metadata.album_artist);
	snprintf(np_genre, sizeof(np_genre), "%s", state.metadata.genre);

	// An album with no album-artist tag still belongs to its artist, which is
	// what the mark in Album artists is looked up by.
	if (!np_album_artist[0]) {
		snprintf(np_album_artist, sizeof(np_album_artist), "%s", np_artist);
	}
}

static bool entry_is_now_playing(panel_t *p, int index) {
	if (index < 0 || index >= p->count || !np_path[0]) {
		return false;
	}

	const char *name = NULL;
	const char *path = NULL;
	if (!row_at(p, index, &name, &path)) {
		return false;
	}

	// A track list: the file itself.
	if (p->is_tracks) {
		return path && strcmp(path, np_path) == 0;
	}

	// A name list: whichever of the playing track's tags this list is made of.
	if (!name || !name[0]) {
		return false;
	}
	switch (p->kind) {
	case LIBRARY_LIST_ALBUMS:
		return np_album[0] && strcmp(name, np_album) == 0;
	case LIBRARY_LIST_ARTISTS:
		return np_artist[0] && strcmp(name, np_artist) == 0;
	case LIBRARY_LIST_ALBUM_ARTISTS:
		return np_album_artist[0] && strcmp(name, np_album_artist) == 0;
	case LIBRARY_LIST_GENRES:
		return np_genre[0] && strcmp(name, np_genre) == 0;
	default:
		return false;
	}
}

static void row_update_playmark(panel_t *p, row_t *row) {
	if (!row->playmark) {
		return;
	}
	if (entry_is_now_playing(p, row->index)) {
		lv_obj_remove_flag(row->playmark, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(row->playmark, LV_OBJ_FLAG_HIDDEN);
	}
}

static void panel_refresh_playmarks(panel_t *p) {
	for (int i = 0; i < ROW_POOL; i++) {
		row_update_playmark(p, &p->rows[i]);
	}
}

void medialist_notify_now_playing(void) {
	// Before the pages exist (the boot-time restore refreshes the player
	// before the GUI is finished) there is nothing to mark.
	if (!panel_names.rows[0].button) {
		return;
	}
	np_cache_refresh();
	panel_refresh_playmarks(&panel_names);
	panel_refresh_playmarks(&panel_tracks);
}

// The badge a track wears, or nothing at all when the setting is off, the row
// is not a track, or the index never learned what the file holds.
static void row_update_quality(panel_t *p, row_t *row, const char *path) {
	if (!row->quality) {
		return;
	}
	if (!quality_badges || !path || !path[0]) {
		lv_obj_add_flag(row->quality, LV_OBJ_FLAG_HIDDEN);
		return;
	}

	const lv_image_dsc_t *icon = NULL;
	switch (library_track_quality(path, NULL, NULL)) {
	case LIBRARY_QUALITY_LOSSY:
		icon = &icon_quality_lossy;
		break;
	case LIBRARY_QUALITY_CD:
		icon = &icon_quality_cd;
		break;
	case LIBRARY_QUALITY_HIFI:
		icon = &icon_quality_hifi;
		break;
	case LIBRARY_QUALITY_DSD:
		icon = &icon_quality_dsd;
		break;
	default:
		break;
	}
	(void)p;

	if (!icon) {
		lv_obj_add_flag(row->quality, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	lv_image_set_src(row->quality, icon);
	lv_obj_remove_flag(row->quality, LV_OBJ_FLAG_HIDDEN);
}

static void row_bind(panel_t *p, row_t *row, int index) {
	if (row->index == index) {
		return;
	}

	row_drop_thumb(p, row);
	row->index = index;

	if (index < 0 || index >= p->count) {
		lv_obj_add_flag(row->button, LV_OBJ_FLAG_HIDDEN);
		return;
	}

	const char *name = NULL;
	const char *path = NULL;
	if (!row_at(p, index, &name, &path)) {
		// The handle went stale under the list, or the row is gone. Hiding the
		// row is what a reload will fix; drawing a neighbour's name would not
		// look like a fault at all.
		lv_obj_add_flag(row->button, LV_OBJ_FLAG_HIDDEN);
		return;
	}

	lv_obj_remove_flag(row->button, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_y(row->button, index * ROW_PITCH);
	lv_label_set_text(row->label, name);
	// Decided here and not at construction, because the same panel serves
	// Artists, Albums and Genres in turn: only the bind knows which list is
	// loaded right now.
	if (panel_shows_icons(p)) {
		lv_obj_remove_flag(row->icon, LV_OBJ_FLAG_HIDDEN);
		row_show_glyph(p, row);
	} else {
		lv_obj_add_flag(row->icon, LV_OBJ_FLAG_HIDDEN);
	}
	row_update_playmark(p, row);
	row_update_quality(p, row, path);
}

// Requests artwork for the visible rows and collects what the worker has
// finished. Same loop as the browser's thumbs_update.
static void thumbs_update(panel_t *p) {
	// No icons, no covers: loading thumbnails that would land in a hidden
	// image is work for the worker and nothing else.
	if (!panel_shows_icons(p)) {
		return;
	}

	bool anything_pending = false;

	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &p->rows[i];
		if (row->index < 0 || row->index >= p->count || row->thumb_settled) {
			continue;
		}

		const char *source = NULL;
		if (!row_at(p, row->index, NULL, &source) || !source) {
			row->thumb_settled = true; // nothing to load art from
			continue;
		}

		if (!row->thumb_requested) {
			coverloader_request(row_slot(p, row), source, THUMB_SIZE);
			row->thumb_requested = true;
		}

		bool finished = false;
		cover_image_t image;
		if (coverloader_take(row_slot(p, row), &image, &finished)) {
			row->thumb = image;
			row->has_thumb = true;
			row->thumb_settled = true;
			row_show_cover(row, &row->thumb.dsc);
		} else if (finished) {
			row->thumb_settled = true; // no artwork in this file
		} else {
			anything_pending = true;
		}
	}

	if (anything_pending) {
		lv_timer_resume(thumb_timer);
	}
}

static void thumb_timer_cb(lv_timer_t *timer) {
	lv_timer_pause(timer); // thumbs_update resumes it while work is pending

	// Every panel that draws covers, the artist-records screen included: a panel
	// left out of this list asks the worker for its jackets and has nobody to
	// collect them, so its covers only appear when something else calls
	// window_update() by hand.
	panel_t *const panels[] = {&panel_names, &panel_tracks, &panel_artist_albums};
	lv_obj_t *active = lv_screen_active();
	for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
		if (active == panels[i]->screen) {
			thumbs_update(panels[i]);
			return;
		}
	}
}

static void window_update(panel_t *p) {
	panel_refresh_stale(p);

	if (p->count == 0) {
		for (int i = 0; i < ROW_POOL; i++) {
			row_bind(p, &p->rows[i], -1);
		}
		return;
	}

	int scroll = lv_obj_get_scroll_y(p->list);
	if (scroll < 0) {
		scroll = 0;
	}

	int first = (scroll / ROW_PITCH) - 1;
	if (first + ROW_POOL > p->count) {
		first = p->count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}

	// index -> widget index % ROW_POOL, so a one-step slide of the band
	// rebinds exactly one row instead of renaming all of them.
	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		row_bind(p, &p->rows[index % ROW_POOL], index < p->count ? index : -1);
	}

	// row_bind() is a no-op when a row's index has not moved, so a row whose
	// *content* changed underneath it (the list reloaded to the same length,
	// a reorder) would keep the mark it was given last time. Re-deciding it
	// for the whole pool here is twelve string compares and makes the mark
	// self-correcting whatever route the list took to get here.
	panel_refresh_playmarks(p);

	thumbs_update(p);
}

void medialist_set_quality_badges(bool on) {
	load_view_settings();
	if (quality_badges == on) {
		return;
	}
	quality_badges = on;
	config_set_int("library", "quality_badges", on ? 1 : 0);
	config_save();

	// The rows already bound were bound with the old answer, and row_bind()
	// leaves a row whose index has not moved alone. Forgetting their indices is
	// what makes the switch show on the list behind the settings page instead
	// of at the next list opened.
	if (!panel_tracks.rows[0].button) {
		return;
	}
	for (int i = 0; i < ROW_POOL; i++) {
		panel_tracks.rows[i].index = -1;
	}
	window_update(&panel_tracks);
}

// Takes one entry out of the model and lays the list out again. Used when the
// user removes the thing a row stands for -- a favourite, a playlist entry --
// so the row goes at once instead of at the next visit to the page. Reloading
// the whole list would work too, but it loses the scroll position and throws
// away every thumbnail already decoded.
static void model_remove(panel_t *p, int index) {
	if (index < 0 || index >= p->count) {
		return;
	}

	// Only a list of paths gets here. A query-backed one is rebuilt instead:
	// everything that can take a row out of one (unstarring, for now) moves row
	// ids as it does so, which the handle notices and panel_refresh_stale acts
	// on -- and the caller runs that first.
	if (!p->from_paths) {
		return;
	}

	free(p->entries[index].name);
	free(p->entries[index].path);
	memmove(&p->entries[index], &p->entries[index + 1], (size_t)(p->count - index - 1) * sizeof(entry_t));
	p->count--;
	p->entries_count = p->count;

	// Every pooled row now stands for a different entry, so none of them may
	// keep the artwork or the index it had.
	for (int i = 0; i < ROW_POOL; i++) {
		row_drop_thumb(p, &p->rows[i]);
		p->rows[i].index = -2;
	}

	lv_obj_set_height(p->body, p->count ? p->count * ROW_PITCH : ROW_PITCH);
	if (p->count == 0) {
		lv_obj_remove_flag(p->empty, LV_OBJ_FLAG_HIDDEN);
	}
	window_update(p);
}

// ---------------------------------------------------------------------------
// the A-Z strip
// ---------------------------------------------------------------------------

static const char *const INDEX_TEXT[INDEX_BUCKETS] = {"#", "A", "B", "C", "D", "E", "F", "G", "H", "I",
													  "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S",
													  "T", "U", "V", "W", "X", "Y", "Z", "\xE2\x80\xA6"};


static void index_show_bar(panel_t *p, bool shown) {
	if (!p->index_bar) {
		return;
	}
	if (shown) {
		lv_obj_remove_flag(p->index_bar, LV_OBJ_FLAG_HIDDEN);
		lv_obj_move_foreground(p->index_bar);
	} else {
		lv_obj_add_flag(p->index_bar, LV_OBJ_FLAG_HIDDEN);
	}
}

// The strip is wanted: it is scrolling, or a finger is on it. Called often, so
// it only notes the moment; the timer takes it away again.
static void index_flash(panel_t *p) {
	if (!p->index_enabled) {
		return;
	}
	p->index_wanted_at = lv_tick_get();
	if (p->index_bar && lv_obj_has_flag(p->index_bar, LV_OBJ_FLAG_HIDDEN)) {
		index_show_bar(p, true);
	}
}

static void index_hint_show(panel_t *p, int slot) {
	if (!p->index_hint) {
		return;
	}
	int bucket = p->index_desc ? INDEX_BUCKETS - 1 - slot : slot;
	lv_label_set_text(p->index_hint_label, INDEX_TEXT[bucket]);
	lv_obj_remove_flag(p->index_hint, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(p->index_hint);
	p->index_hint_at = lv_tick_get();
}

// Rebuilds the letter -> row map from the model, and decides whether this list
// gets a strip at all.
static void index_rebuild(panel_t *p, bool enabled, bool descending) {
	p->index_enabled = enabled && p->count >= INDEX_MIN_ROWS;
	p->index_desc = descending;

	if (!p->index_bar) {
		return;
	}

	index_show_bar(p, false);
	if (p->index_hint) {
		lv_obj_add_flag(p->index_hint, LV_OBJ_FLAG_HIDDEN);
	}

	if (!p->index_enabled) {
		return;
	}

	// Where each letter's rows begin.
	//
	// Taken from the counts the handle keeps as it goes past (see
	// library_index_open): the first row of a bucket is the sum of the ones
	// before it. That holds only because bucket order and collation order are
	// the same thing: '#' covers the groups the collation files above A, A-Z is
	// Latin folded to its first letter, and the last bucket is everything filed
	// below Z.
	//
	// A list of paths has no handle and no counts, so it walks its own names --
	// those lists are a playlist long, not a library long.
	int first[INDEX_BUCKETS];
	for (int i = 0; i < INDEX_BUCKETS; i++) {
		first[i] = -1;
	}

	int counts[LIBRARY_INDEX_BUCKETS];
	if (!p->from_paths && library_index_buckets(p->ix, counts)) {
		int seen = 0;
		for (int bucket = 0; bucket < INDEX_BUCKETS; bucket++) {
			if (counts[bucket] > 0) {
				// Ascending, the bucket starts after everything before it.
				// Descending, the list is the same rows read from the other
				// end, so it starts that far from the bottom.
				first[bucket] = descending ? p->count - seen - counts[bucket] : seen;
				seen += counts[bucket];
			}
		}
	} else if (p->from_paths) {
		for (int i = 0; i < p->entries_count; i++) {
			int bucket = library_index_slot(library_index_letter(p->entries[i].name));
			if (first[bucket] < 0) {
				first[bucket] = i;
			}
		}
	} else {
		// No trustworthy counts: better no strip than one that lands on the
		// wrong rows.
		p->index_enabled = false;
		return;
	}

	// Top to bottom on screen, which is the alphabet turned round on a Z-A
	// list. The label texts follow the same order, so the strip always reads
	// the way the list under it runs.
	for (int slot = 0; slot < INDEX_BUCKETS; slot++) {
		int bucket = descending ? INDEX_BUCKETS - 1 - slot : slot;
		p->index_row[slot] = first[bucket];
		lv_label_set_text(p->index_letters[slot], INDEX_TEXT[bucket]);
	}


	// A letter nothing starts with still has to answer: it lands on the first
	// row of the next letter that does, which is where those rows would be.
	// The ones past the last letter in use fall back to the previous.
	int next = -1;
	for (int slot = INDEX_BUCKETS - 1; slot >= 0; slot--) {
		if (p->index_row[slot] >= 0) {
			next = p->index_row[slot];
		} else {
			p->index_row[slot] = next;
		}
	}
	int previous = -1;
	for (int slot = 0; slot < INDEX_BUCKETS; slot++) {
		if (p->index_row[slot] >= 0) {
			previous = p->index_row[slot];
		} else {
			p->index_row[slot] = previous;
		}
	}
}

static void index_go(panel_t *p, int slot) {
	if (slot < 0) {
		slot = 0;
	}
	if (slot >= INDEX_BUCKETS) {
		slot = INDEX_BUCKETS - 1;
	}

	index_hint_show(p, slot); // the letter shows even where there are no rows

	int row = p->index_row[slot];
	if (row < 0) {
		return;
	}

	lv_obj_scroll_to_y(p->list, row * ROW_PITCH, LV_ANIM_OFF);
	p->saved_scroll = lv_obj_get_scroll_y(p->list);
	window_update(p);
}

// The slot the finger is over. Asked of the letters themselves rather than
// worked out from the strip's height: the row of labels is laid out with even
// gaps around it, so it does not quite fill the box, and arithmetic on the box
// answers a press near either end with the neighbouring letter.
static void index_go_at(panel_t *p, lv_point_t point) {
	for (int slot = 0; slot < INDEX_BUCKETS; slot++) {
		lv_area_t area;
		lv_obj_get_coords(p->index_letters[slot], &area);
		if (point.y <= area.y2) {
			index_go(p, slot);
			return;
		}
	}
	index_go(p, INDEX_BUCKETS - 1); // past the last letter: the last letter
}

// The strip carries the page's own gestures as well -- the swipe back and the
// pull that brings the player in -- so a press on it does not commit to the
// alphabet until it is clear that is what it is. Sideways belongs to the page;
// up and down, or a tap that does not move at all, belongs to the strip. Which
// is why nothing jumps on the press itself: a swipe that begins over the strip
// would have moved the list first and slid the page away over the result.
static void index_bar_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);

	// The page took the gesture: the strip is out of it until the next press.
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		p->index_engaged = false;
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		lv_indev_t *indev = lv_indev_active();
		if (code == LV_EVENT_RELEASED && !p->index_engaged && indev) {
			// It never moved: a tap on a letter, which is the other way the
			// strip is meant to be used. Acted on at the lift.
			lv_point_t point;
			lv_indev_get_point(indev, &point);
			index_go_at(p, point);
		}
		p->index_hint_at = lv_tick_get(); // from here the big letter fades out
		p->index_engaged = false;
		return;
	}

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_point_t point;
	lv_indev_get_point(indev, &point);

	index_flash(p); // the strip stays up for as long as a finger is on it

	if (code == LV_EVENT_PRESSED) {
		p->index_press = point;
		p->index_engaged = false;
		return;
	}
	if (code != LV_EVENT_PRESSING) {
		return;
	}

	if (!p->index_engaged) {
		int dx = LV_ABS(point.x - p->index_press.x);
		int dy = LV_ABS(point.y - p->index_press.y);
		if (dx > dy) {
			return; // going sideways: this is the page's, not the strip's
		}
		if (dy < INDEX_ENGAGE_PX) {
			return; // not enough to tell yet
		}
		p->index_engaged = true;
	}

	index_go_at(p, point);
}

// ---------------------------------------------------------------------------
// The sentinel
//
// The strip's two objects are built once and never deleted, so a bad pointer in
// either can only mean something wrote over the panel -- and in panel_t they sit
// immediately after a 600-byte char buffer. They are therefore written down when
// they are built and checked before they are used: if the two disagree the log
// says so and the strip switches itself off.
//
// The copy is kept outside panel_t on purpose. One stored next to what it guards
// is overwritten by the same run of bytes that overwrote the original, and would
// agree with it all the way into the fault.
// ---------------------------------------------------------------------------

static struct {
	const panel_t *panel;
	lv_obj_t *bar;
	lv_obj_t *hint;
	bool reported;
} index_built[3];
static int index_built_count;

static void index_guard_note(panel_t *p) {
	for (int i = 0; i < index_built_count; i++) {
		if (index_built[i].panel == p) {
			index_built[i].bar = p->index_bar;
			index_built[i].hint = p->index_hint;
			return;
		}
	}
	if (index_built_count >= (int)(sizeof(index_built) / sizeof(index_built[0]))) {
		return;
	}
	index_built[index_built_count].panel = p;
	index_built[index_built_count].bar = p->index_bar;
	index_built[index_built_count].hint = p->index_hint;
	index_built_count++;
}

// False when the panel no longer holds the objects it was built with. Says so
// once, then takes the strip out of service: a pointer that has changed by
// itself is not one to hand to LVGL.
static bool index_guard_ok(panel_t *p) {
	for (int i = 0; i < index_built_count; i++) {
		if (index_built[i].panel != p) {
			continue;
		}
		if (p->index_bar == index_built[i].bar && p->index_hint == index_built[i].hint) {
			return true;
		}
		if (!index_built[i].reported) {
			index_built[i].reported = true;
			fprintf(stderr,
					"medialist: the A-Z strip was overwritten (bar %p, was %p; hint %p, was %p) -- "
					"switching it off\n",
					(void *)p->index_bar, (void *)index_built[i].bar, (void *)p->index_hint,
					(void *)index_built[i].hint);
			fflush(stderr);
		}
		p->index_bar = NULL;
		p->index_hint = NULL;
		p->index_enabled = false;
		return false;
	}
	return true; // never recorded: there is nothing to compare against
}

// Takes the strip and the big letter away once nothing has asked for them for
// a moment. One timer for all three panels; only the one on screen can be
// showing anything.
static void index_tick(panel_t *p) {
	if (!index_guard_ok(p)) {
		return;
	}
	if (p->index_bar && !lv_obj_has_flag(p->index_bar, LV_OBJ_FLAG_HIDDEN) &&
		lv_tick_elaps(p->index_wanted_at) > INDEX_HIDE_MS) {
		index_show_bar(p, false);
	}
	if (p->index_hint && !lv_obj_has_flag(p->index_hint, LV_OBJ_FLAG_HIDDEN) &&
		lv_tick_elaps(p->index_hint_at) > INDEX_HINT_MS) {
		lv_obj_add_flag(p->index_hint, LV_OBJ_FLAG_HIDDEN);
	}
}

static void index_timer_cb(lv_timer_t *timer) {
	(void)timer;
	// All three: the artist's albums has a strip of its own, and a panel left
	// out here has nothing to hide its strip again once it has shown.
	index_tick(&panel_names);
	index_tick(&panel_tracks);
	index_tick(&panel_artist_albums);
}

// The reorder button's glyph: the accent while the list is being reordered,
// ordinary ink the rest of the time.
//
// It is worth a function of its own because it is the only place that decides
// this colour, and because a hand-set colour is a snapshot: the glyph is built
// wearing theme_style_icon and follows the palette by itself only until reorder
// mode is first entered or left, at which point a local property is pinned on it
// and the shared style can no longer reach it. The theme refresh therefore calls
// this too.
static void reorder_icon_paint(panel_t *p) {
	if (!p->reorder_btn) {
		return;
	}
	lv_obj_t *glyph = lv_obj_get_child(p->reorder_btn, 0);
	if (glyph) {
		lv_obj_set_style_image_recolor(glyph, p->reordering ? theme()->accent : theme()->text_primary, 0);
	}
}

// The strip's own tint is a hand-set colour, so it needs telling when the
// palette moves; the letters and the card behind the big letter follow their
// shared styles on their own.
static void index_theme_refresh(void) {
	panel_t *panels[] = {&panel_names, &panel_tracks, &panel_artist_albums};
	for (size_t i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
		if (panels[i]->index_bar) {
			lv_obj_set_style_bg_color(panels[i]->index_bar, theme()->surface, 0);
		}
		if (panels[i]->index_hint) {
			lv_obj_set_style_bg_color(panels[i]->index_hint, theme()->accent, 0);
		}
		reorder_icon_paint(panels[i]);
	}
}

// The strip and the big letter, built once with the panel and hidden.
static void index_build(panel_t *p, gui_config_t *cfg) {
	int content_top = settingsrow_content_top(cfg);

	p->index_bar = lv_obj_create(p->screen);
	lv_obj_remove_style_all(p->index_bar);
	lv_obj_set_size(p->index_bar, INDEX_BAR_WIDTH, cfg->screen_height - content_top - 8);
	lv_obj_align(p->index_bar, LV_ALIGN_TOP_RIGHT, -2, content_top + 4);
	lv_obj_set_style_bg_color(p->index_bar, theme()->surface, 0);
	lv_obj_set_style_bg_opa(p->index_bar, LV_OPA_60, 0);
	lv_obj_set_style_radius(p->index_bar, INDEX_BAR_WIDTH / 2, 0);
	lv_obj_remove_flag(p->index_bar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(p->index_bar, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_flex_flow(p->index_bar, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(p->index_bar, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// The strip takes its own presses -- it must not scroll the list underneath
	// -- but it is still part of the page, so the swipe back and the pull that
	// brings the player in start on it as well. Which of the three a press is
	// gets decided in index_bar_cb, from the direction it moves.
	lv_obj_add_flag(p->index_bar, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(p->index_bar, index_bar_cb, LV_EVENT_PRESSED, p);
	lv_obj_add_event_cb(p->index_bar, index_bar_cb, LV_EVENT_PRESSING, p);
	lv_obj_add_event_cb(p->index_bar, index_bar_cb, LV_EVENT_RELEASED, p);
	lv_obj_add_event_cb(p->index_bar, index_bar_cb, LV_EVENT_PRESS_LOST, p);
	switcher_attach_back_gesture(p->index_bar);
	player_sheet_attach_drag(p->index_bar, true);

	for (int i = 0; i < INDEX_BUCKETS; i++) {
		p->index_letters[i] = lv_label_create(p->index_bar);
		lv_label_set_text(p->index_letters[i], INDEX_TEXT[i]);
		lv_obj_add_style(p->index_letters[i], &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(p->index_letters[i], &font_ui_14, 0);
		// The presses belong to the strip as a whole: the letter under the
		// finger comes from where it is, not from which label it landed on.
		lv_obj_remove_flag(p->index_letters[i], LV_OBJ_FLAG_CLICKABLE);
	}

	// The letter under the finger, in the middle of the screen where the hand
	// is not. In the accent colour rather than the card one: it lands on top of
	// a list of cards, and a card on cards is a card nobody sees.
	p->index_hint = lv_obj_create(p->screen);
	lv_obj_remove_style_all(p->index_hint);
	lv_obj_set_size(p->index_hint, 132, 124);
	lv_obj_align(p->index_hint, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_bg_color(p->index_hint, theme()->accent, 0);
	lv_obj_set_style_bg_opa(p->index_hint, LV_OPA_90, 0);
	lv_obj_set_style_radius(p->index_hint, 26, 0);
	lv_obj_remove_flag(p->index_hint, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(p->index_hint, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(p->index_hint, LV_OBJ_FLAG_HIDDEN);

	p->index_hint_label = lv_label_create(p->index_hint);
	lv_label_set_text(p->index_hint_label, "");
	lv_obj_set_style_text_font(p->index_hint_label, &font_ui_64_bold, 0);
	lv_obj_set_style_text_color(p->index_hint_label, lv_color_white(), 0);
	lv_obj_center(p->index_hint_label);

	index_guard_note(p);
}

static void scroll_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	p->saved_scroll = lv_obj_get_scroll_y(p->list);
	index_flash(p);
	window_update(p);
}

// ---------------------------------------------------------------------------
// clicks
// ---------------------------------------------------------------------------

static library_filter_t filter_for(library_list_t kind) {
	switch (kind) {
	case LIBRARY_LIST_ALBUMS:
		return LIBRARY_FILTER_ALBUM;
	case LIBRARY_LIST_ARTISTS:
		return LIBRARY_FILTER_ARTIST;
	case LIBRARY_LIST_ALBUM_ARTISTS:
		return LIBRARY_FILTER_ALBUM_ARTIST;
	case LIBRARY_LIST_GENRES:
		return LIBRARY_FILTER_GENRE;
	default:
		return LIBRARY_FILTER_NONE;
	}
}

static void row_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return; // a swipe across the list, not a tap
	}

	panel_t *p = lv_event_get_user_data(e);
	if (p->reordering) {
		// The list is being arranged, not listened to. A tap that started a
		// track here would replace what is playing with the entry the user was
		// only trying to take hold of.
		return;
	}
	lv_obj_t *button = lv_event_get_current_target(e);

	// Which pool row was hit.
	int index = -1;
	for (int i = 0; i < ROW_POOL; i++) {
		if (p->rows[i].button == button) {
			index = p->rows[i].index;
			break;
		}
	}
	if (index < 0 || index >= p->count) {
		return;
	}

	char path[512];
	char name[256];
	{
		const char *n = NULL;
		const char *pth = NULL;
		if (!row_at(p, index, &n, &pth)) {
			return;
		}
		snprintf(name, sizeof(name), "%s", n ? n : "");
		snprintf(path, sizeof(path), "%s", pth ? pth : "");
		if (p->is_tracks && !pth) {
			return;
		}
	}

	if (p->is_tracks) {
		// The queue is this list, so next/prev walk it -- not the folder the
		// file happens to live in.
		//
		// A query-backed list hands the queue a copy of its row ids, not its
		// rows: the queue is then the same four bytes a track the list is,
		// instead of a second full set of paths beside it.
		bool queued = false;
		if (!p->from_paths) {
			// The same refresh menu_tracks_open() does, and for the same reason:
			// a clone of a stale handle is stale too, the queue cannot read the
			// path of the row it was told to start on, and it falls back to the
			// first entry of the list. On a playlist the handle is stale almost
			// always -- opening one starts a background pass over its files that
			// bumps the generation.
			//
			// It also keeps the reopen off the audio thread's back: done here,
			// the index is current before playback is asked for, instead of
			// being rebuilt inside playlist_current_path() while the playback
			// thread is trying to take the Bluetooth transport again.
			panel_refresh_stale(p);
			if (index >= p->count) {
				return; // the list got shorter under the finger
			}
			queued = device_state_play_index(library_index_clone(p->ix), index);
		} else {
			const char **paths = malloc((size_t)p->entries_count * sizeof(char *));
			if (paths) {
				for (int i = 0; i < p->entries_count; i++) {
					paths[i] = p->entries[i].path;
				}
				device_state_play_list(paths, p->entries_count, index);
				free(paths);
				queued = true;
			}
		}

		if (queued) {
			// If this list is an album, say so: "play consecutive albums"
			// needs to know which record is ending to pick the next one.
			if (p->filter == LIBRARY_FILTER_ALBUM) {
				playlist_set_album(p->filter_value);
			}
			player_refresh_now_playing();
		} else {
			player_play_file(path); // out of memory: at least play it
		}
		switch_screen(player_screen);
		return;
	}

	// An artist: their records, or their tracks when the album view is off.
	// An album, a genre: the tracks either way.
	bool by_artist = p->kind == LIBRARY_LIST_ARTISTS || p->kind == LIBRARY_LIST_ALBUM_ARTISTS;
	if (by_artist && medialist_album_view()) {
		medialist_open(name, LIBRARY_LIST_ALBUMS, filter_for(p->kind), name);
		return;
	}
	medialist_open(name, LIBRARY_LIST_TRACKS, filter_for(p->kind), name);
}

// One of the small transparent glyph buttons that sit on the title row.
static lv_obj_t *corner_button(lv_obj_t *parent, const lv_image_dsc_t *glyph, lv_event_cb_t cb, panel_t *p) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, CORNER_BUTTON_SIZE, CORNER_BUTTON_SIZE);
	lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_pad_all(btn, 0, 0);
	lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, p);

	lv_obj_t *image = lv_image_create(btn);
	lv_image_set_src(image, glyph);
	lv_obj_add_style(image, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
	lv_obj_center(image);
	return btn;
}

// ---------------------------------------------------------------------------
// Reordering a playlist by hand
//
// A playlist is the one list whose order is the user's rather than the
// database's: it comes back ORDER BY idx, and idx is what a drag rewrites. So
// the corner button is a mode, not an action -- lit in the accent while it is
// on, like the album grouping and the Favourites reverse.
//
// While it is on, the ellipsis at the right of each row becomes the grip, and
// that grip is the handle: dragging it moves the entry, dragging anywhere else
// still scrolls the list. Handles are why the grip glyph exists -- without one,
// a list that reorders on a drag is a list that cannot be scrolled.
//
// What moves is written down as it happens, one entry per drop, inside a
// transaction, so leaving the mode has nothing to commit and nothing is ever
// held half-reordered in memory.
// ---------------------------------------------------------------------------

// How close to the top or bottom edge a finger has to be before the list starts
// moving under it, and how far it moves each time the event comes round.
#define REORDER_EDGE_PX 70
#define REORDER_EDGE_STEP 18

// Only one drag can be in flight, so this is one set of variables rather than a
// set per panel.
static panel_t *drag_panel;
static int drag_from = -1; // the entry being carried, by position in the list
static int drag_to = -1;   // where it would land if the finger lifted now
static lv_obj_t *drop_line;

static void row_bind(panel_t *p, row_t *row, int index);
static void window_update(panel_t *p);

// The ellipsis or the grip, and whether a drag on it scrolls the list.
//
// The scroll chain is the reason this is done per row rather than once: a press
// that lands on a child walks up looking for something scrollable, and while
// the grip is a handle it must not find the list. Outside the mode the flag
// goes back, because there the ellipsis is an ordinary button and a swipe that
// happens to start on it should still scroll the page.
static void row_apply_reorder_look(const panel_t *p, const row_t *row) {
	if (!row->menu_btn) {
		return;
	}
	lv_obj_t *glyph = lv_obj_get_child(row->menu_btn, 0);
	if (glyph) {
		lv_image_set_src(glyph, p->reordering ? &icon_grip : &icon_ellipsis_vertical);
	}
	if (p->reordering) {
		lv_obj_remove_flag(row->menu_btn, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
	} else {
		lv_obj_add_flag(row->menu_btn, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
	}
}

// The accent rule that says where the entry would land. Drawn on the body, in
// the list's own coordinates, so it scrolls with the rows.
static void drop_line_show(panel_t *p, int before_index) {
	if (!drop_line) {
		drop_line = lv_obj_create(p->body);
		lv_obj_add_flag(drop_line, LV_OBJ_FLAG_IGNORE_LAYOUT);
		lv_obj_remove_flag(drop_line, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_remove_flag(drop_line, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_border_width(drop_line, 0, 0);
		lv_obj_set_style_shadow_width(drop_line, 0, 0);
		lv_obj_set_style_pad_all(drop_line, 0, 0);
		lv_obj_add_style(drop_line, &theme_style_accent_bg, 0);
		lv_obj_set_style_radius(drop_line, 2, 0);
	} else if (lv_obj_get_parent(drop_line) != p->body) {
		lv_obj_set_parent(drop_line, p->body);
	}
	lv_obj_set_size(drop_line, lv_obj_get_width(p->body) - 2 * ROW_PAD, 4);
	lv_obj_set_pos(drop_line, ROW_PAD, before_index * ROW_PITCH - ROW_GAP / 2 - 2);
	lv_obj_remove_flag(drop_line, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(drop_line);
}

static void drop_line_hide(void) {
	if (drop_line) {
		lv_obj_add_flag(drop_line, LV_OBJ_FLAG_HIDDEN);
	}
}

// The entry the row being carried would take, from where the finger is.
static int index_under(const panel_t *p, lv_coord_t y) {
	lv_area_t body;
	lv_obj_get_coords(p->body, &body);
	int inside = (int)(y - body.y1);
	int index = inside / ROW_PITCH;
	if (index < 0) {
		index = 0;
	}
	if (index > p->count - 1) {
		index = p->count - 1;
	}
	return index;
}

// Marks the row being carried, so the user can see which one they have. Runs
// over the pool because the row for an index moves as the list scrolls.
static void drag_mark_rows(panel_t *p) {
	for (int i = 0; i < ROW_POOL; i++) {
		bool carried = drag_panel == p && p->rows[i].index >= 0 && p->rows[i].index == drag_from;
		lv_obj_set_style_border_width(p->rows[i].button, carried ? 2 : 0, 0);
		if (carried) {
			lv_obj_set_style_border_color(p->rows[i].button, theme()->accent, 0);
			lv_obj_set_style_border_opa(p->rows[i].button, LV_OPA_COVER, 0);
		}
	}
}

static void drag_end(void) {
	panel_t *p = drag_panel;
	drop_line_hide();
	drag_panel = NULL;
	int from = drag_from;
	int to = drag_to;
	drag_from = -1;
	drag_to = -1;
	if (p) {
		drag_mark_rows(p);
	}

	if (!p || from < 0 || to < 0 || from == to || !p->playlist[0]) {
		return;
	}

	if (!library_playlist_move(p->playlist, from, to)) {
		toast_error("medialist_reorder_failed");
		return;
	}
	p->reorder_moved = true;

	// The move bumped the playlist generation, so the handle this page holds is
	// stale by definition. Forgetting the pool's indices as well is what makes
	// every row rebind: the ids under them have moved, and a row whose index
	// has not changed would otherwise keep the name it had.
	for (int i = 0; i < ROW_POOL; i++) {
		p->rows[i].index = -1;
	}
	window_update(p);
	drag_mark_rows(p);
}

static void grip_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);

	if (!p->reordering) {
		return; // out of the mode this button is the ellipsis, handled elsewhere
	}
	lv_event_stop_bubbling(e);

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		drag_end();
		return;
	}

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_point_t point;
	lv_indev_get_point(indev, &point);

	if (code == LV_EVENT_PRESSED) {
		lv_obj_t *button = lv_event_get_current_target(e);
		drag_panel = p;
		drag_from = -1;
		for (int i = 0; i < ROW_POOL; i++) {
			if (p->rows[i].menu_btn == button) {
				drag_from = p->rows[i].index;
				break;
			}
		}
		if (drag_from < 0 || drag_from >= p->count) {
			drag_panel = NULL;
			drag_from = -1;
			return;
		}
		drag_to = drag_from;
		drag_mark_rows(p);
		return;
	}
	if (code != LV_EVENT_PRESSING || drag_panel != p || drag_from < 0) {
		return;
	}

	// The list follows the finger when it reaches either end, so an entry can
	// be carried past the screenful it started on.
	lv_area_t view;
	lv_obj_get_coords(p->list, &view);
	int step = 0;
	if (point.y < view.y1 + REORDER_EDGE_PX) {
		step = -REORDER_EDGE_STEP;
	} else if (point.y > view.y2 - REORDER_EDGE_PX) {
		step = REORDER_EDGE_STEP;
	}
	if (step != 0) {
		int scroll = lv_obj_get_scroll_y(p->list);
		int limit = p->count * ROW_PITCH - lv_obj_get_height(p->list);
		if (limit < 0) {
			limit = 0;
		}
		int wanted = scroll + step;
		wanted = wanted < 0 ? 0 : (wanted > limit ? limit : wanted);
		if (wanted != scroll) {
			lv_obj_scroll_to_y(p->list, wanted, LV_ANIM_OFF);
			window_update(p);
			drag_mark_rows(p);
		}
	}

	drag_to = index_under(p, point.y);
	drop_line_show(p, drag_to > drag_from ? drag_to + 1 : drag_to);
}

// The corner button: on turns the mode on, on again turns it off. There is
// nothing to save at the second press -- every drop was written when it
// happened -- so this only says what changed.
static void reorder_clicked_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);

	drag_end();
	p->reordering = !p->reordering;

	if (p->reordering) {
		p->reorder_moved = false;
	} else if (p->reorder_moved) {
		p->reorder_moved = false;
		toast_success("medialist_playlist_order_saved");
	}

	reorder_icon_paint(p);
	for (int i = 0; i < ROW_POOL; i++) {
		row_apply_reorder_look(p, &p->rows[i]);
	}
}

// Leaves the mode without asking, for the routes that are not the button:
// walking off the page, the list being reloaded under it, a card going away.
static void reorder_stop(panel_t *p) {
	drag_end();
	if (!p->reordering) {
		return;
	}
	p->reordering = false;
	p->reorder_moved = false;
	reorder_icon_paint(p);
	for (int i = 0; i < ROW_POOL; i++) {
		row_apply_reorder_look(p, &p->rows[i]);
	}
}

static void screen_unloaded_cb(lv_event_t *e) { reorder_stop(lv_event_get_user_data(e)); }

// Reopens whatever this panel is showing, under whatever the ordering now is.
// The signature is cleared first so the list comes back at the top: after a
// re-sort the old scroll position points at nothing in particular.
static void reload_current(panel_t *p) {
	if (p->from_paths) {
		return;
	}
	char title[sizeof(p->title)];
	char value[sizeof(p->filter_value)];
	snprintf(title, sizeof(title), "%s", p->title);
	snprintf(value, sizeof(value), "%s", p->filter_value);

	p->signature[0] = '\0';
	medialist_open(title, p->kind, p->filter, value[0] ? value : NULL);
}

static void sort_clicked_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	if (!p) {
		return;
	}
	sort_set_desc(p->kind, !sort_is_desc(p->kind));
	reload_current(p);
}

// Favourites reversed: the most recently starred track on top. The database
// returns the list in its own order, oldest first; the button flips it, and
// while on it carries the accent colour, because it is a state, not an action.
static bool fav_reversed;

static void reverse_clicked_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	if (!p) {
		return;
	}
	fav_reversed = !fav_reversed;
	config_set_int("library", "fav_reversed", fav_reversed ? 1 : 0);
	config_save();
	reload_current(p);
}

static void album_order_clicked_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	if (!p) {
		return;
	}
	artist_album_order = !artist_album_order;
	config_set_int("library", "artist_album_order", artist_album_order ? 1 : 0);
	config_save();
	reload_current(p);
}

// Copies the path of the one row a window was asked for.
static bool take_one_path_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)name;
	(void)artist;
	char *out = user;
	snprintf(out, 512, "%s", path ? path : "");
	return false; // one row is all that was wanted
}

// ---------------------------------------------------------------------------
// Starting a whole artist
//
// The corner button on an artist's records opens a small card in the middle of
// the screen -- the shape the Gearboy pause menu uses -- rather than doing one
// thing. There are three sensible ways to start an artist and no reason to
// choose one of them for the user.
//
// All three build the same queue: that artist's tracks, gathered by record and
// each record in its running order. What differs is where playback starts and
// in what order it goes on.
//
// The album chaining option keeps working through this. What it needs is a
// record to carry on from when the queue runs out, so the queue is stamped with
// the artist's last one: play the artist through and the next record in the
// library follows, which is what the option promises.
// ---------------------------------------------------------------------------

static panel_t *play_menu_panel;

static void play_menu_hide(void) {
	if (play_menu_panel && play_menu_panel->play_menu) {
		lv_obj_add_flag(play_menu_panel->play_menu, LV_OBJ_FLAG_HIDDEN);
	}
	play_menu_panel = NULL;
}

// The tracks the menu plays, as a handle. What that is depends on what the list
// underneath is showing:
//
//   a list of tracks  -- those tracks, in the order they are on screen. Whether
//                        that is all of them, one artist's, one artist's
//                        gathered by record, the favourites or a playlist, the
//                        list is the answer;
//   a list of records -- the tracks of those records, gathered by record and
//                        each record in its running order, so "in sequence"
//                        starts at the top of the list and works down.
//
// NULL when there is nothing to play, which includes a list handed in as an
// array of paths: there is no query behind one, and menu_paths_collect() below
// is what that case uses instead.
//
// The caller owns it: device_state_play_index() takes it, and the callers that
// only read it close it themselves.
static library_index_t *menu_tracks_open(panel_t *p) {
	if (!p || p->from_paths) {
		return NULL;
	}

	// The handle the queue is about to be built from has to be the current one.
	// Row ids are only a promise, and a stale handle hands out a clone that is
	// stale too: library_index_window() refuses it, the queue cannot read the
	// path of the track it was told to start on, and it falls back to entry 0.
	//
	// This is not a theoretical window: opening a playlist starts a background
	// pass that checks whether its files are still on the card, and that pass
	// bumps the playlist generation a fraction of a second after the page
	// appears, so the page is stale almost immediately, every time.
	panel_refresh_stale(p);

	library_index_t *ix = NULL;
	if (p->kind == LIBRARY_LIST_TRACKS || p->kind == LIBRARY_LIST_FAVOURITES || p->kind == LIBRARY_LIST_PLAYLIST) {
		ix = library_index_clone(p->ix);
	} else if (p->kind == LIBRARY_LIST_ALBUMS) {
		ix = library_index_open(LIBRARY_LIST_TRACKS, p->filter, p->filter_value[0] ? p->filter_value : NULL,
								LIBRARY_ORDER_ALBUM, false);
	}

	if (ix && library_index_count(ix) > 0) {
		return ix;
	}
	library_index_close(ix);
	return NULL;
}

// The record the queue ends on, for album chaining to carry on from: the last
// row of the album list the user is looking at, which is the same list the
// queue was built from.
//
// Cleared when the list is not one of records. A queue of tracks has no record
// to be the end of, and a leftover name from an earlier list would send the
// chaining off after the wrong one.
static void stamp_last_album(const panel_t *p) {
	const char *name = NULL;
	if (p->kind == LIBRARY_LIST_ALBUMS && p->count > 0 && row_at((panel_t *)p, p->count - 1, &name, NULL) && name &&
		name[0]) {
		playlist_set_album(name);
		return;
	}
	playlist_set_album("");
}

// The playable paths of a list handed in as an array, for the lists that have
// no query behind them. The caller frees the array; the strings belong to the
// panel and outlive the call.
static int menu_paths_collect(const panel_t *p, const char ***out) {
	*out = NULL;
	if (!p || !p->from_paths || p->entries_count <= 0) {
		return 0;
	}
	const char **paths = malloc((size_t)p->entries_count * sizeof(*paths));
	if (!paths) {
		return 0;
	}
	int usable = 0;
	for (int i = 0; i < p->entries_count; i++) {
		if (p->entries[i].path) {
			paths[usable++] = p->entries[i].path;
		}
	}
	if (usable == 0) {
		free(paths);
		return 0;
	}
	*out = paths;
	return usable;
}

static void play_artist(panel_t *p, bool shuffled) {
	if (!p) {
		return;
	}

	// Mode first: with shuffle already on, the queue deals its random order as
	// it is loaded, so it comes out shuffled in one step. Which of the two
	// shuffles is the "Continuous shuffle" option -- the plain one stops at the
	// end of the deal, the other comes round again in the same order.
	playback_mode_t shuffle_mode =
		musicsettings_endless_shuffle() ? PLAYBACK_MODE_SHUFFLE_REPEAT : PLAYBACK_MODE_SHUFFLE;
	playlist_set_mode(shuffled ? shuffle_mode : PLAYBACK_MODE_NORMAL);

	if (p->from_paths) {
		const char **paths = NULL;
		int count = menu_paths_collect(p, &paths);
		if (count == 0) {
			return;
		}
		int start = shuffled && count > 1 ? (int)lv_rand(0, (uint32_t)(count - 1)) : 0;
		device_state_play_list(paths, count, start);
		free(paths);
	} else {
		library_index_t *ix = menu_tracks_open(p);
		if (!ix) {
			return;
		}
		int count = library_index_count(ix);
		int start = shuffled && count > 1 ? (int)lv_rand(0, (uint32_t)(count - 1)) : 0;
		if (!device_state_play_index(ix, start)) {
			return; // the handle was taken either way
		}
	}
	stamp_last_album(p);

	player_refresh_now_playing();
	switch_screen(player_screen);
}

static void play_menu_shuffle_cb(lv_event_t *e) {
	(void)e;
	panel_t *p = play_menu_panel;
	play_menu_hide();
	play_artist(p, true);
}

static void play_menu_sequence_cb(lv_event_t *e) {
	(void)e;
	panel_t *p = play_menu_panel;
	play_menu_hide();
	play_artist(p, false);
}

// One of the artist's tracks, chosen at random, put after what is playing.
// Reads the one row it needs out of the handle rather than the whole list.
static void play_menu_random_track_cb(lv_event_t *e) {
	(void)e;
	panel_t *p = play_menu_panel;
	play_menu_hide();
	if (!p) {
		return;
	}

	char path[512] = "";
	if (p->from_paths) {
		const char **paths = NULL;
		int count = menu_paths_collect(p, &paths);
		if (count == 0) {
			return;
		}
		int pick = count > 1 ? (int)lv_rand(0, (uint32_t)(count - 1)) : 0;
		snprintf(path, sizeof(path), "%s", paths[pick]);
		free(paths);
	} else {
		library_index_t *ix = menu_tracks_open(p);
		if (!ix) {
			return;
		}
		int count = library_index_count(ix);
		int pick = count > 1 ? (int)lv_rand(0, (uint32_t)(count - 1)) : 0;
		library_index_window(ix, pick, 1, take_one_path_cb, path);
		library_index_close(ix);
	}

	if (path[0] && playlist_insert_next(path)) {
		device_state_queue_changed();
		toast_success("added_to_the_queue");
	}
}

static void play_menu_dismiss_cb(lv_event_t *e) {
	(void)e;
	play_menu_hide();
}

static lv_obj_t *play_menu_row(lv_obj_t *parent, const char *text, lv_event_cb_t cb) {
	lv_obj_t *row = lv_btn_create(parent);
	lv_obj_set_size(row, lv_pct(100), 76);
	lv_obj_add_style(row, &theme_style_card, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(row, 12, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_shadow_width(row, 0, 0);
	lv_obj_set_style_pad_hor(row, 16, 0);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_text(label, tr(text));
	lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_center(label);

	return row;
}

// Built once, on the artist-records screen, and shown and hidden from there.
static void play_menu_build(panel_t *p) {
	p->play_menu = lv_obj_create(p->screen);
	lv_obj_remove_style_all(p->play_menu);
	lv_obj_set_size(p->play_menu, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(p->play_menu, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(p->play_menu, LV_OPA_60, 0);
	lv_obj_add_flag(p->play_menu, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(p->play_menu, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(p->play_menu, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(p->play_menu, play_menu_dismiss_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *card = lv_obj_create(p->play_menu);
	lv_obj_set_size(card, 380, LV_SIZE_CONTENT);
	lv_obj_center(card);
	lv_obj_add_style(card, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(card, 18, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 16, 0);
	lv_obj_set_style_pad_gap(card, 10, 0);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	play_menu_row(card, "medialist_play_in_random_order", play_menu_shuffle_cb);
	play_menu_row(card, "medialist_play_in_sequence", play_menu_sequence_cb);
	play_menu_row(card, "medialist_queue_random_track", play_menu_random_track_cb);
}

static void play_menu_clicked_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	if (!p || p->count <= 0) {
		return;
	}
	if (!p->play_menu) {
		play_menu_build(p);
	}
	play_menu_panel = p;
	lv_obj_remove_flag(p->play_menu, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(p->play_menu);
}

// The ellipsis menu on a track row.
static char menu_path[512];
static char menu_name[256];
// The row the menu was opened on, so an action that removes the thing the row
// stands for can take the row away with it.
static panel_t *menu_panel;
static int menu_index = -1;

// The row goes now, not at the next visit. Only when the list is one whose
// membership the action just changed: taking a track out of the favourites has
// to empty its row in the favourites list, and has to leave every other list
// alone.
static void menu_drop_row_if_listed(void) {
	// The index was taken when the popover opened, and a list can reload
	// underneath one -- a scan finishing, a sort flipped. Checking that the row
	// still stands for the same track before dropping it costs one compare and
	// stops the wrong row disappearing; the file itself is always operated on by
	// path, so it is only ever the view that could go out of step.
	// The list may have been rebuilt under the open popover -- unstarring is
	// itself one of the things that rebuilds it.
	if (menu_panel) {
		panel_refresh_stale(menu_panel);
	}
	if (menu_panel && menu_index >= 0 && menu_index < menu_panel->count) {
		// The row may well be outside the cached band by now -- the popover
		// survives a scroll -- so this is a read that can go to the database.
		// Once per menu action, which is nothing.
		char path[512];
		if (row_path_copy(menu_panel, menu_index, path, sizeof(path)) && strcmp(path, menu_path) == 0) {
			model_remove(menu_panel, menu_index);
		}
	}
	menu_panel = NULL;
	menu_index = -1;
}

static void menu_playlist_remove_action(void *user) {
	(void)user;
	if (!menu_panel || !menu_panel->playlist[0] || !menu_path[0]) {
		return;
	}
	if (playlists_remove_track(menu_panel->playlist, menu_path)) {
		toast_success("medialist_track_removed");
		menu_drop_row_if_listed();
	} else {
		gui_notify_popup("medialist_remove_failed");
	}
}

static void menu_fav_action(void *user) {
	(void)user;
	if (!menu_path[0]) {
		return;
	}
	// No notification popup -- the change itself is the feedback.
	bool was_favourite = library_fav_contains(menu_path);
	library_fav_toggle(menu_path, menu_name, "");

	// Taken out of the favourites while looking at the favourites: the row is
	// about to stand for something that is no longer in the list, so it goes.
	if (was_favourite && menu_panel && menu_panel->kind == LIBRARY_LIST_FAVOURITES) {
		menu_drop_row_if_listed();
	}
}

static void menu_playlist_action(void *user) {
	(void)user;
	if (menu_path[0]) {
		playlistpage_add_track(menu_path);
	}
}

// "Add to queue": the track goes in right after the one playing, and a second
// one after that -- see playlist_insert_next().
//
// And the queue on disk is refreshed here rather than left to the next track
// change: the mirror only writes it when a track is loaded, so a track queued
// and then a power-off would be a track the next boot has never heard of.
static void menu_queue_action(void *user) {
	(void)user;
	if (!menu_path[0]) {
		return;
	}
	if (playlist_insert_next(menu_path)) {
		device_state_queue_changed();
		toast_success("added_to_the_queue");
	}
}

static void menu_details_action(void *user) {
	(void)user;
	if (menu_path[0]) {
		trackmenu_details_open(menu_path);
	}
}

// "Show album": the record this track came off, with everything else on it.
// The album name is looked up again at the moment it is asked for rather than
// carried on every row -- one query against a path that is already indexed,
// against a string per entry in a list that can run to thousands.
static char menu_album[256];

static void menu_album_action(void *user) {
	(void)user;
	if (menu_album[0]) {
		medialist_open(menu_album, LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, menu_album);
	}
}

static void menu_btn_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e);

	panel_t *p = lv_event_get_user_data(e);
	if (p->reordering) {
		// This button is the drag handle in reorder mode. A press and a lift
		// without any movement is still a click, and without this the track
		// menu would open on top of the list being arranged.
		return;
	}
	lv_obj_t *button = lv_event_get_current_target(e);

	int index = -1;
	for (int i = 0; i < ROW_POOL; i++) {
		if (p->rows[i].menu_btn == button) {
			index = p->rows[i].index;
			break;
		}
	}
	const char *row_name = NULL;
	const char *row_path = NULL;
	if (!row_at(p, index, &row_name, &row_path) || !row_path) {
		return;
	}

	snprintf(menu_path, sizeof(menu_path), "%s", row_path);
	snprintf(menu_name, sizeof(menu_name), "%s", row_name ? row_name : "");
	menu_panel = p;
	menu_index = index;

	menu_album[0] = '\0';
	if (!library_track_album(menu_path, menu_album, sizeof(menu_album))) {
		menu_album[0] = '\0';
	}

	popover_item_t items[5];
	int n = 0;
	// The two "add to" lines stay next to each other: they are the same kind
	// of thing, and reading past one to reach the other is friction with no
	// reason behind it. "Show album" goes with "Details" instead, both being
	// ways of finding out more about the track rather than doing something to
	// it.
	items[n++] = (popover_item_t){
		library_fav_contains(menu_path) ? "remove_from_favourites" : "medialist_add_to_favourites", menu_fav_action, NULL};
	items[n++] = (popover_item_t){"add_to_queue", menu_queue_action, NULL};
	// Inside a playlist the useful action is the opposite one: this list is
	// where the track already is, so offering to file it somewhere else while
	// looking at it would be the wrong way round.
	if (p->playlist[0]) {
		items[n++] = (popover_item_t){"medialist_remove_from_playlist", menu_playlist_remove_action, NULL};
	} else {
		items[n++] = (popover_item_t){"add_to_playlist", menu_playlist_action, NULL};
	}

	// Only when there is an album to show, and not when this list already IS
	// that album: offering to open the page already on screen is noise.
	if (menu_album[0] && !(p->filter == LIBRARY_FILTER_ALBUM && strcmp(p->filter_value, menu_album) == 0)) {
		items[n++] = (popover_item_t){"show_album", menu_album_action, NULL};
	}

	items[n++] = (popover_item_t){"details", menu_details_action, NULL};
	popover_show(button, items, n);
}

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

static lv_style_t style_row;
static bool styles_ready;

static void init_styles(void) {
	if (styles_ready) {
		return;
	}
	styles_ready = true;

	// The browser's row geometry, verbatim.
	lv_style_init(&style_row);
	lv_style_set_border_width(&style_row, 0);
	lv_style_set_radius(&style_row, ROW_RADIUS);
	lv_style_set_shadow_width(&style_row, 0);
	lv_style_set_pad_all(&style_row, ROW_PAD);
	lv_style_set_pad_column(&style_row, ROW_PAD);
}

// Coming back to a list through the back chevron. The library can have been
// rescanned, or the card taken out and put back, while the page sat on the
// stack: without this the rows keep the labels they were bound with and every
// tap is dead until the user scrolls.
static void screen_loaded_cb(lv_event_t *e) {
	panel_t *p = lv_event_get_user_data(e);
	if (p) {
		window_update(p);
	}
}

static void build_panel(panel_t *p, gui_config_t *cfg, bool is_tracks, int slot_base) {
	p->cfg = cfg;
	p->is_tracks = is_tracks;
	p->slot_base = slot_base;
	p->window_first = -1;

	lv_obj_add_style(p->screen, &theme_style_screen, 0);
	lv_obj_add_event_cb(p->screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, p);
	p->title_label = settingsrow_title(p->screen, cfg, "");

	int content_top = settingsrow_content_top(cfg);
	int width = cfg->screen_width - 2 * cfg->padding;

	// Full-width viewport with the rows inset by the padding, exactly like
	// the browser: that puts the scrollbar out in the margin, beside the
	// rows, instead of drawing it on top of them.
	p->list = lv_obj_create(p->screen);
	lv_obj_set_size(p->list, cfg->screen_width, cfg->screen_height - content_top);
	lv_obj_align(p->list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(p->list, 0, 0);
	lv_obj_set_style_border_width(p->list, 0, 0);
	lv_obj_set_style_radius(p->list, 0, 0);
	lv_obj_set_style_pad_hor(p->list, cfg->padding, 0);
	lv_obj_set_style_pad_ver(p->list, 0, 0);
	lv_obj_set_scroll_dir(p->list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(p->list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_add_event_cb(p->list, scroll_cb, LV_EVENT_SCROLL, p);

	p->body = lv_obj_create(p->list);
	lv_obj_set_width(p->body, width);
	lv_obj_set_height(p->body, ROW_PITCH);
	lv_obj_set_pos(p->body, 0, 0);
	lv_obj_set_style_bg_opa(p->body, 0, 0);
	lv_obj_set_style_border_width(p->body, 0, 0);
	lv_obj_set_style_pad_all(p->body, 0, 0);
	lv_obj_remove_flag(p->body, LV_OBJ_FLAG_SCROLLABLE);
	// Row presses bubble row -> body -> list; without this flag they stop here
	// and the swipe gestures never see a press that began on a row.
	lv_obj_add_flag(p->body, LV_OBJ_FLAG_EVENT_BUBBLE);

	// The corner strip, on the title row's right: whichever of the three
	// buttons this particular list has a use for. A row rather than three
	// aligned objects, so two of them showing at once lay themselves out.
	p->corner = lv_obj_create(p->screen);
	lv_obj_remove_style_all(p->corner);

	// A fixed width, not LV_SIZE_CONTENT.
	//
	// Hiding a flex child takes it out of the layout, so a content-sized box
	// is one button wide and the second button of a pair lands outside its own
	// parent, at x = -60, and is never drawn. Sizing for the most buttons that
	// can coexist costs a strip of empty space on the pages that show fewer.
	lv_obj_set_size(p->corner, CORNER_MAX_BUTTONS * CORNER_BUTTON_SIZE + (CORNER_MAX_BUTTONS - 1) * CORNER_GAP, 56);
	lv_obj_remove_flag(p->corner, LV_OBJ_FLAG_CLICKABLE); // it is a shelf, not a control
	lv_obj_align(p->corner, LV_ALIGN_TOP_RIGHT, -cfg->padding, cfg->padding + cfg->top_bar_height);
	lv_obj_remove_flag(p->corner, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(p->corner, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(p->corner, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(p->corner, CORNER_GAP, 0);

	// Tracks of one artist, gathered by record instead of strung out
	// alphabetically. Only ever shown on an artist's own track list.
	p->album_btn = NULL;
	if (is_tracks) {
		p->album_btn = corner_button(p->corner, &icon_album_corner, album_order_clicked_cb, p);
	}

	// Hand-ordering, on the playlists only. Created before the play button
	// because the strip is a plain flex row: the first child made is the
	// leftmost, and this one belongs to the left of circle-play.
	p->reorder_btn = NULL;
	if (is_tracks) {
		p->reorder_btn = corner_button(p->corner, &icon_reorder, reorder_clicked_cb, p);
	}

	// Ways to start the whole of what the list is showing. Built on every
	// panel: which lists actually wear it is medialist_open()'s to decide.
	p->play_btn = corner_button(p->corner, &icon_circle_play, play_menu_clicked_cb, p);

	// The Favourites reverse button, to the left of shuffle: created before
	// it, so the flex row lays the two out in that order.
	p->reverse_btn = NULL;
	if (is_tracks) {
		p->reverse_btn = corner_button(p->corner, &icon_arrow_down_up, reverse_clicked_cb, p);
	}

	// A-Z, and Z-A when it has been pressed.
	p->sort_btn = corner_button(p->corner, &icon_sort_az, sort_clicked_cb, p);
	p->sort_icon = lv_obj_get_child(p->sort_btn, 0);

	p->empty = lv_label_create(p->list);
	lv_label_set_text(p->empty, tr("library_empty_note"));
	lv_obj_set_style_text_align(p->empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(p->empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(p->empty, &font_ui_24, 0);
	lv_obj_align(p->empty, LV_ALIGN_TOP_MID, 0, 120);
	lv_obj_add_flag(p->empty, LV_OBJ_FLAG_HIDDEN);

	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &p->rows[i];

		row->button = lv_btn_create(p->body);
		lv_obj_set_size(row->button, width, ROW_HEIGHT);
		lv_obj_set_x(row->button, 0);
		lv_obj_add_style(row->button, &theme_style_card, 0);
		lv_obj_add_style(row->button, &style_row, 0);
		lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_add_flag(row->button, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(row->button, LV_OBJ_FLAG_EVENT_BUBBLE); // so the player sheet can be dragged in
		lv_obj_add_event_cb(row->button, row_clicked_cb, LV_EVENT_CLICKED, p);

		lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		row->icon = lv_image_create(row->button);
		lv_obj_set_size(row->icon, THUMB_SIZE, THUMB_SIZE);
		lv_image_set_inner_align(row->icon, LV_IMAGE_ALIGN_CENTER);

		// The title, and under it the quality badge. A column rather than the
		// label alone: with the badge in the row itself it would sit beside
		// the title and eat the width the title needs.
		row->text = lv_obj_create(row->button);
		lv_obj_remove_style_all(row->text);
		lv_obj_set_flex_grow(row->text, 1);
		lv_obj_set_height(row->text, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(row->text, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(row->text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
		lv_obj_set_style_pad_row(row->text, QUALITY_GAP, 0);
		lv_obj_remove_flag(row->text, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_remove_flag(row->text, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_flag(row->text, LV_OBJ_FLAG_EVENT_BUBBLE);

		row->label = lv_label_create(row->text);
		lv_label_set_long_mode(row->label, LV_LABEL_LONG_DOT);
		lv_obj_set_width(row->label, LV_PCT(100));
		// One line, always. LV_LABEL_LONG_DOT wraps before it truncates, and a
		// title on two lines pushes the badge under it out of the row.
		lv_obj_set_height(row->label, lv_font_get_line_height(&font_ui_24));
		lv_obj_add_style(row->label, &theme_style_text, 0);
		lv_obj_set_style_text_font(row->label, &font_ui_24, 0);

		// Only track rows have a quality to show: an album or an artist is not
		// one recording.
		row->quality = NULL;
		if (is_tracks) {
			row->quality = lv_image_create(row->text);
			lv_obj_add_flag(row->quality, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(row->quality, LV_OBJ_FLAG_CLICKABLE);
		}

		// Out of the flex layout on purpose: it sits in the row's own left
		// padding, so adding it moves nothing. Hidden until a row it belongs
		// to is bound.
		row->playmark = lv_obj_create(row->button);
		lv_obj_add_flag(row->playmark, LV_OBJ_FLAG_IGNORE_LAYOUT);
		lv_obj_set_size(row->playmark, PLAYMARK_WIDTH, PLAYMARK_HEIGHT);
		lv_obj_align(row->playmark, LV_ALIGN_LEFT_MID, PLAYMARK_INSET - ROW_PAD, 0);
		lv_obj_add_style(row->playmark, &theme_style_accent_bg, 0);
		lv_obj_set_style_radius(row->playmark, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_width(row->playmark, 0, 0);
		lv_obj_set_style_shadow_width(row->playmark, 0, 0);
		lv_obj_set_style_pad_all(row->playmark, 0, 0);
		lv_obj_remove_flag(row->playmark, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_remove_flag(row->playmark, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_flag(row->playmark, LV_OBJ_FLAG_HIDDEN);

		row->menu_btn = NULL;
		if (!is_tracks) {
			// Name rows (album, artist, genre...) open another list: say so
			// with the chevron, like every other page-opening row.
			lv_obj_t *chev = lv_image_create(row->button);
			lv_image_set_src(chev, &icon_chevron_right);
			lv_obj_add_style(chev, &theme_style_icon, 0);
			lv_obj_set_style_image_opa(chev, LV_OPA_60, 0);
		}
		if (is_tracks) {
			// The per-track menu, riding the right edge of the row.
			row->menu_btn = lv_btn_create(row->button);
			lv_obj_set_size(row->menu_btn, 44, 44);
			lv_obj_set_style_bg_opa(row->menu_btn, LV_OPA_TRANSP, 0);
			lv_obj_set_style_border_width(row->menu_btn, 0, 0);
			lv_obj_set_style_shadow_width(row->menu_btn, 0, 0);
			lv_obj_set_style_pad_all(row->menu_btn, 0, 0);
			lv_obj_add_event_cb(row->menu_btn, menu_btn_cb, LV_EVENT_CLICKED, p);

			// The same button is the drag handle while the list is being
			// reordered. grip_cb returns at once outside that mode, so the
			// ellipsis is an ordinary button there.
			lv_obj_add_event_cb(row->menu_btn, grip_cb, LV_EVENT_PRESSED, p);
			lv_obj_add_event_cb(row->menu_btn, grip_cb, LV_EVENT_PRESSING, p);
			lv_obj_add_event_cb(row->menu_btn, grip_cb, LV_EVENT_RELEASED, p);
			lv_obj_add_event_cb(row->menu_btn, grip_cb, LV_EVENT_PRESS_LOST, p);

			lv_obj_t *dots = lv_image_create(row->menu_btn);
			lv_image_set_src(dots, &icon_ellipsis_vertical);
			lv_obj_add_style(dots, &theme_style_icon, 0);
			lv_obj_set_style_image_recolor_opa(dots, LV_OPA_COVER, 0);
			lv_obj_center(dots);
		}

		row->index = -1;
		row->has_thumb = false;
		row->thumb_requested = false;
		row->thumb_settled = false;
	}

	// Walking off the page leaves the mode. Otherwise the panel comes back to
	// another list still in it -- with a playlist name in p->playlist that is no
	// longer the list on screen, which is the one way a drop could be written
	// to the wrong playlist.
	lv_obj_add_event_cb(p->screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, p);

	// The player sheet can be pulled in from the list, like everywhere else,
	// and the swipe-back drag can start on it too.
	player_sheet_attach_drag(p->list, true); // a page surface: swiping left pulls the player in
	switcher_attach_back_gesture(p->list);

	// Last, so both sit above the rows they cover.
	index_build(p, cfg);
}

// ---------------------------------------------------------------------------
// opening
// ---------------------------------------------------------------------------

static void show_corner(lv_obj_t *btn, bool shown) {
	if (!btn) {
		return;
	}
	if (shown) {
		lv_obj_remove_flag(btn, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
	}
}

void medialist_open(const char *title, library_list_t kind, library_filter_t filter, const char *filter_value) {
	// Nothing to open without the card. The index lives on it, so every list
	// here would come up empty -- and an empty list under a title, with "scan
	// the library" written across it, tells the user to do the one thing that
	// cannot work right now.
	if (!library_is_open()) {
		gui_notify_no_card();
		return;
	}

	// Which of the three screens this list belongs on. Tracks are always the
	// last level; an album list narrowed to one artist is the middle one;
	// everything else is where a walk starts.
	bool tracks_like =
		(kind == LIBRARY_LIST_TRACKS || kind == LIBRARY_LIST_FAVOURITES || kind == LIBRARY_LIST_PLAYLIST);
	bool artist_albums = kind == LIBRARY_LIST_ALBUMS &&
						 (filter == LIBRARY_FILTER_ARTIST || filter == LIBRARY_FILTER_ALBUM_ARTIST);
	panel_t *p = tracks_like ? &panel_tracks : (artist_albums ? &panel_artist_albums : &panel_names);
	p->kind = kind;
	// A playlist is a query like any other, and the query's value is its
	// name -- which is also what the ellipsis needs to know to offer taking a
	// track out of it.
	if (kind == LIBRARY_LIST_PLAYLIST) {
		snprintf(p->playlist, sizeof(p->playlist), "%s", filter_value ? filter_value : "");
	} else {
		p->playlist[0] = '\0';
	}
	p->filter = filter;
	p->from_paths = false;
	snprintf(p->filter_value, sizeof(p->filter_value), "%s", filter_value ? filter_value : "");
	snprintf(p->title, sizeof(p->title), "%s", title ? title : "");

	// Coming back to the very list that was open before? Then put the user
	// back where they left it instead of at the top.
	char signature[600];
	snprintf(signature, sizeof(signature), "%d|%d|%s", (int)kind, (int)filter, filter_value ? filter_value : "");
	bool same_list = strcmp(signature, p->signature) == 0;
	snprintf(p->signature, sizeof(p->signature), "%s", signature);

	lv_label_set_text(p->title_label, title ? title : "");

	// Which of the corner buttons this list has a use for.
	//
	// An artist's own track list gets the album grouping; the four flat
	// alphabetical lists get the direction switch; Favourites gets shuffle. A
	// list ordered by album has no alphabetical direction to invert, so the
	// two never appear together.
	bool artist_tracks =
		kind == LIBRARY_LIST_TRACKS && (filter == LIBRARY_FILTER_ARTIST || filter == LIBRARY_FILTER_ALBUM_ARTIST);
	bool sortable = (kind == LIBRARY_LIST_TRACKS && filter == LIBRARY_FILTER_NONE) ||
					(kind == LIBRARY_LIST_ALBUMS && !artist_albums) || kind == LIBRARY_LIST_ARTISTS ||
					kind == LIBRARY_LIST_ALBUM_ARTISTS;

	bool want_album = artist_tracks;
	// The circle-play menu, wherever "play all of this" is a question worth
	// asking: an artist's records, an artist's tracks, all the records, all the
	// tracks, the favourites, a playlist. Not on the name lists, where a row is
	// not a set of tracks, and not inside one album, where a tap on the first
	// row is the whole answer.
	bool want_play = artist_albums || (kind == LIBRARY_LIST_ALBUMS && filter == LIBRARY_FILTER_NONE) ||
					 kind == LIBRARY_LIST_FAVOURITES || kind == LIBRARY_LIST_PLAYLIST ||
					 (kind == LIBRARY_LIST_TRACKS &&
					  (filter == LIBRARY_FILTER_NONE || filter == LIBRARY_FILTER_ARTIST ||
					   filter == LIBRARY_FILTER_ALBUM_ARTIST));
	bool want_sort = sortable && !(artist_tracks && artist_album_order);
	bool want_reverse = kind == LIBRARY_LIST_FAVOURITES;
	// A playlist is the only list whose order belongs to the user. Every other
	// one comes back from the database in an order the database decides, and
	// dragging a row in it would have nowhere to be written down.
	bool want_reorder = kind == LIBRARY_LIST_PLAYLIST;

	// Whatever this panel was showing before, it is not showing it now.
	reorder_stop(p);

	show_corner(p->reorder_btn, want_reorder);
	show_corner(p->album_btn, want_album);
	show_corner(p->play_btn, want_play);
	show_corner(p->sort_btn, want_sort);
	show_corner(p->reverse_btn, want_reverse);

	// The heading stops where the corner shelf starts, and the shelf is a
	// different width on every one of these lists, so the heading is told how
	// many buttons to clear rather than the one settingsrow_title() assumes. A
	// long title -- a translated list name, or an album's own name -- otherwise
	// runs under them.
	int corner_buttons = (want_album ? 1 : 0) + (want_play ? 1 : 0) + (want_sort ? 1 : 0) + (want_reverse ? 1 : 0) +
						 (want_reorder ? 1 : 0);
	settingsrow_title_corner_slots(p->title_label, p->cfg, corner_buttons);

	if (p->album_btn) {
		// Lit in the accent while the grouping is on: the button is a state,
		// not an action that happens once.
		lv_obj_set_style_image_recolor(lv_obj_get_child(p->album_btn, 0),
									   artist_album_order ? theme()->accent : theme()->text_primary, 0);
	}
	if (p->reverse_btn) {
		// Lit in the accent colour for as long as the list is reversed.
		lv_obj_set_style_image_recolor(lv_obj_get_child(p->reverse_btn, 0),
									   fav_reversed ? theme()->accent : theme()->text_primary, 0);
	}
	lv_image_set_src(p->sort_icon, sort_is_desc(kind) ? &icon_sort_za : &icon_sort_az);
	lv_obj_move_foreground(p->corner);

	// Every pool row lets go of its artwork *before* the entries under it are
	// freed and reloaded.
	for (int i = 0; i < ROW_POOL; i++) {
		row_drop_thumb(p, &p->rows[i]);
		p->rows[i].index = -2; // force a rebind (indices may look unchanged)
	}

	model_clear(p);
	library_order_t order = (artist_tracks && artist_album_order) ? LIBRARY_ORDER_ALBUM : LIBRARY_ORDER_DEFAULT;

	// Z-A is the same list read backwards. The database has already done the
	// hard part -- the collation groups by script, folds case and accents and
	// skips leading articles -- and reading that backwards is exactly "the
	// other way round", where a second DESC query would only be the same sort
	// again. Reversed favourites are the same list read backwards as well: the
	// database returns it oldest-starred first, and with the button on, the
	// newest belongs on top.
	bool desc = (sortable && sort_is_desc(kind)) || (kind == LIBRARY_LIST_FAVOURITES && fav_reversed);

	p->from_paths = false;
	p->list_order = order;
	p->list_desc = desc;
	p->ix = library_index_open(kind, filter, filter_value, order, desc);
	p->count = library_index_count(p->ix);

	lv_obj_set_height(p->body, p->count ? p->count * ROW_PITCH : ROW_PITCH);
	if (p->count == 0) {
		lv_obj_remove_flag(p->empty, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(p->empty, LV_OBJ_FLAG_HIDDEN);
	}

	// The A-Z strip, on the two long flat lists: all the tracks and all the
	// records. The others are either short (an artist's albums) or not filed by
	// letter at all (an artist's tracks gathered by record, Favourites in the
	// order they were starred), and a strip of letters on those would point at
	// rows that are not where it says.
	p->index_wanted = (kind == LIBRARY_LIST_TRACKS && filter == LIBRARY_FILTER_NONE) ||
					  (kind == LIBRARY_LIST_ALBUMS && !artist_albums) || kind == LIBRARY_LIST_ARTISTS ||
					  kind == LIBRARY_LIST_ALBUM_ARTISTS;
	index_rebuild(p, p->index_wanted, sortable && sort_is_desc(kind));

	// The same list resumes at its old scroll position; a different one
	// starts at the top.
	int target_scroll = same_list ? p->saved_scroll : 0;
	int max_scroll = p->count * ROW_PITCH - lv_obj_get_height(p->list);
	if (target_scroll > max_scroll) {
		target_scroll = max_scroll > 0 ? max_scroll : 0;
	}
	lv_obj_scroll_to_y(p->list, target_scroll, LV_ANIM_OFF);
	p->saved_scroll = target_scroll;
	window_update(p);

	switch_screen(p->screen);
}

// A track list built from paths handed in rather than queried: a playlist's
// contents. It reuses the tracks panel wholesale -- covers, the ellipsis
// menu, the tap that starts the whole list as the queue -- by filling the
// model through the very callback library_for_each would have used.
void medialist_open_paths(const char *title, const char *const *paths, const char *const *names,
						  const char *const *artists, int count, const char *playlist) {
	panel_t *p = &panel_tracks;
	p->kind = LIBRARY_LIST_TRACKS;
	p->filter = LIBRARY_FILTER_NONE;
	p->from_paths = true;
	snprintf(p->playlist, sizeof(p->playlist), "%s", playlist ? playlist : "");

	// Never "the same list" as a database one: the contents can change under
	// a playlist between visits, so always start at the top.
	snprintf(p->signature, sizeof(p->signature), "paths|%s", title ? title : "");

	lv_label_set_text(p->title_label, title ? title : "");

	// A list handed in as paths has an order of its own -- the one whoever
	// built it put it in -- so the buttons that reorder the list stay away.
	// circle-play is not one of them: it orders the queue, not the list.
	show_corner(p->play_btn, true);
	show_corner(p->sort_btn, false);
	show_corner(p->album_btn, false);
	show_corner(p->reverse_btn, false);
	settingsrow_title_corner_slots(p->title_label, p->cfg, 1);

	for (int i = 0; i < ROW_POOL; i++) {
		row_drop_thumb(p, &p->rows[i]);
		p->rows[i].index = -2;
	}

	model_clear(p);
	load_ctx_t ctx = {.panel = p, .capacity = 0};
	for (int i = 0; i < count; i++) {
		if (!paths || !paths[i] || !paths[i][0]) {
			continue;
		}
		const char *name = (names && names[i] && names[i][0]) ? names[i] : NULL;
		if (!name) {
			const char *slash = strrchr(paths[i], '/');
			name = slash ? slash + 1 : paths[i];
		}
		const char *artist = (artists && artists[i] && artists[i][0]) ? artists[i] : NULL;
		if (!load_row_cb(name, paths[i], artist, &ctx)) {
			break;
		}
	}

	lv_obj_set_height(p->body, p->count ? p->count * ROW_PITCH : ROW_PITCH);
	if (p->count == 0) {
		lv_obj_remove_flag(p->empty, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(p->empty, LV_OBJ_FLAG_HIDDEN);
	}

	// No A-Z strip on a playlist: its order is the one the user put it in.
	p->index_wanted = false;
	index_rebuild(p, false, false);

	lv_obj_scroll_to_y(p->list, 0, LV_ANIM_OFF);
	p->saved_scroll = 0;
	window_update(p);

	switch_screen(p->screen);
}

void medialist_playlist_renamed(const char *old_name, const char *new_name) {
	panel_t *p = &panel_tracks;
	if (!old_name || !new_name || !new_name[0] || strcmp(p->playlist, old_name) != 0) {
		return;
	}
	snprintf(p->playlist, sizeof(p->playlist), "%s", new_name);
	// The name is also the query: the list is a handle over the table called
	// after it, so a rename that stopped at the heading would leave the panel
	// asking for a table that is no longer there the next time it reloads.
	if (p->kind == LIBRARY_LIST_PLAYLIST) {
		snprintf(p->filter_value, sizeof(p->filter_value), "%s", new_name);
		snprintf(p->title, sizeof(p->title), "%s", new_name);
		snprintf(p->signature, sizeof(p->signature), "%d|%d|%s", (int)p->kind, (int)p->filter, new_name);
	}
	// The heading is the playlist's name here, and the list is still on screen
	// behind the page the rename was typed on.
	lv_label_set_text(p->title_label, new_name);
}

void medialist_init(gui_config_t *cfg) {
	init_styles();
	coverloader_start();

	sort_desc_mask = (unsigned)config_get_int("library", "sort_desc", 0);
	artist_album_order = config_get_int("library", "artist_album_order", 0) != 0;
	load_view_settings();
	fav_reversed = config_get_int("library", "fav_reversed", 0) != 0;

	panel_names.screen = medialist_screen;
	panel_tracks.screen = medialist_tracks_screen;
	panel_artist_albums.screen = medialist_albums_screen;

	build_panel(&panel_names, cfg, false, SLOT_BASE_NAMES);
	build_panel(&panel_tracks, cfg, true, SLOT_BASE_TRACKS);
	build_panel(&panel_artist_albums, cfg, false, SLOT_BASE_ARTIST_ALBUMS);

	thumb_timer = lv_timer_create(thumb_timer_cb, THUMB_POLL_MS, NULL);
	lv_timer_pause(thumb_timer);

	// The A-Z strip fades itself out; one timer serves all three panels.
	//
	// Registered for standby: with the screen dark there is no strip to take
	// away and nobody to see it go, and five wake-ups a second all night is
	// exactly the sort of thing power_screen_off() exists to stop.
	lv_timer_t *index_timer = lv_timer_create(index_timer_cb, 200, NULL);
	power_pause_in_standby(index_timer);
	theme_register_refresh(index_theme_refresh);
}
