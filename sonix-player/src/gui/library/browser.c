#include "browser.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "lvgl/lvgl.h"

#include <pthread.h>

#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/library/cue.h"
#include "src/system/playback/device_state.h"
#include "src/gui/shell/toast.h"
#include "src/system/playback/playlist.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"

// ---------------------------------------------------------------------------
// A windowed list.
//
// A widget per file does not fit: three widgets a row over a thousand tracks is
// more than a megabyte, allocated on the way into a folder and freed on the way
// out, on a device with ten megabytes free.
//
// So the list is windowed, the way the stock player's listview is: a band around
// the scroll position, quantised to the row height, is the only part that
// exists. ROW_POOL row widgets are built once at startup and re-pointed at
// different entries as the list scrolls, and a folder of N files costs
// N * (8 bytes + its name) -- nothing is allocated or freed on a folder
// change.
// ---------------------------------------------------------------------------

// Side of the album art thumbnail shown in front of each row.
#define THUMB_SIZE 72

// Row geometry. The pitch is what the windowing maths is built on, so it has
// to be exact: the row height plus the gap underneath it.
#define ROW_HEIGHT 100
#define LIST_ROW_GAP 8
#define ROW_PITCH (ROW_HEIGHT + LIST_ROW_GAP)
#define LIST_ROW_RADIUS 12 // Adwaita boxed-list corner radius
#define LIST_ROW_PAD 14

// The now-playing mark, the same one the library lists carry: a rounded
// accent bar in the row's own left padding, so nothing on the row moves.
#define PLAYMARK_WIDTH 6
#define PLAYMARK_HEIGHT 52
#define PLAYMARK_INSET 4

// How many row widgets exist. A 720 px panel shows six or seven; the spares
// cover the partly visible rows at each end and give the recycling somewhere
// to go.
#define ROW_POOL 10

// How often the list checks the artwork worker for finished pictures. Nothing
// is decoded here, so this is just a polling rate.
#define THUMB_TICK_PERIOD_MS 40

// How many big audio files a folder's cue sheets may claim between them. One
// sheet per album is the normal case; a handful covers a folder holding several
// discs of one set.
#define CUE_CLAIM_MAX 8

// Upper bound on entries read from one directory. Each costs eight bytes plus
// its name, so this can be generous; it exists only so that a pathological
// directory cannot eat the heap.
#define MAX_ENTRIES 20000

// The longest folder path list_directory() is given.
#define PATH_BUF 1024

lv_obj_t *browser_screen;

static lv_obj_t *file_list; // the scrolling viewport
static lv_obj_t *list_body; // as tall as the whole list; the rows live in here
static lv_timer_t *thumb_timer;

static lv_style_t style_list_btn;
static lv_style_t style_list_error;

// Root of the browsable area (the SD card mount point). Navigation is not
// allowed to go above this directory.
static char root_path[512];

// The button that jumps back to the card's root, top right. Hidden while the
// browser is already there: a control that would do nothing is one the eye has
// to read and discard every time.
static lv_obj_t *root_button;
static lv_obj_t *shuffle_button;
static char current_path[512];

static lv_obj_t *title_label;
static gui_config_t *config;

// ---------------------------------------------------------------------------
// the entries
//
// Names live end to end in one buffer and each entry keeps an offset into it,
// so a directory listing is two allocations however many files it holds.
//
// There are two sets: one the interface draws from, one the reader thread
// fills. They are swapped when a listing finishes, so the interface is never
// looking at a half-read directory and neither side waits for the other.
// ---------------------------------------------------------------------------

typedef struct {
	uint32_t name_offset; // what the row says
	uint32_t file_offset; // what gets appended to the folder path
	uint8_t is_dir;
} entry_t;

typedef struct {
	entry_t *entries;
	size_t count;
	size_t capacity;

	char *pool;
	size_t pool_used;
	size_t pool_capacity;
} listing_t;

// A parsed cue sheet is some thirty-five kilobytes, far too much for the
// reader thread's stack, and a folder can hold several. One buffer is allocated
// when the browser starts and reused for every sheet of every folder.
static cue_sheet_t *cue_sheets;
static char cue_claims[CUE_CLAIM_MAX][512];

static listing_t shown;	  // what is on screen
static listing_t staging; // what the reader thread is building

// What the row says, and what the row points at. The two are the same string
// for a folder or a file on the card, and different for a track inside a cue
// sheet: the row says "Blue Train" while the thing to open is
// "album.cue?track=1".
static const char *entry_name(size_t index) { return shown.pool + shown.entries[index].name_offset; }
static const char *entry_file(size_t index) { return shown.pool + shown.entries[index].file_offset; }

// Both buffers are kept between folders: the next directory almost certainly
// needs a similar amount, and reusing them is what keeps the heap from churning
// every time the user walks into an album.
static bool listing_reserve(listing_t *list, size_t needed_entries, size_t needed_name_bytes) {
	if (needed_entries > list->capacity) {
		size_t capacity = list->capacity ? list->capacity * 2 : 128;
		while (capacity < needed_entries) {
			capacity *= 2;
		}

		entry_t *grown = realloc(list->entries, capacity * sizeof(*grown));
		if (!grown) {
			return false;
		}
		list->entries = grown;
		list->capacity = capacity;
	}

	if (needed_name_bytes > list->pool_capacity) {
		size_t capacity = list->pool_capacity ? list->pool_capacity * 2 : 4096;
		while (capacity < needed_name_bytes) {
			capacity *= 2;
		}

		char *grown = realloc(list->pool, capacity);
		if (!grown) {
			return false;
		}
		list->pool = grown;
		list->pool_capacity = capacity;
	}

	return true;
}

// `file` may be NULL, which means "the same as the name" -- every entry that
// is a real thing on the card.
static bool listing_add(listing_t *list, const char *name, const char *file, bool is_dir) {
	size_t name_len = strlen(name) + 1;
	size_t file_len = file ? strlen(file) + 1 : 0;

	if (!listing_reserve(list, list->count + 1, list->pool_used + name_len + file_len)) {
		return false;
	}

	memcpy(list->pool + list->pool_used, name, name_len);
	list->entries[list->count].name_offset = (uint32_t)list->pool_used;
	list->entries[list->count].file_offset = (uint32_t)list->pool_used;
	list->pool_used += name_len;

	if (file) {
		memcpy(list->pool + list->pool_used, file, file_len);
		list->entries[list->count].file_offset = (uint32_t)list->pool_used;
		list->pool_used += file_len;
	}

	list->entries[list->count].is_dir = is_dir ? 1 : 0;
	list->count++;

	return true;
}

// qsort has no way to pass the pool through. Per thread: the reader and the
// selection's gatherer both sort, each its own listing.
static __thread const char *sort_pool;

// Directories sort before files, otherwise alphabetical (case-insensitive).
//
// By the file and not by the label, which are the same string for everything
// except a cue track -- and there the label starts with a track number, where
// "10." sorts between "1." and "2.". The two tracks of one sheet are ordered by
// their number instead, which is the order the record is in.
static int entry_cmp(const void *a, const void *b) {
	const entry_t *ea = a;
	const entry_t *eb = b;

	if (ea->is_dir != eb->is_dir) {
		return ea->is_dir ? -1 : 1;
	}

	const char *fa = sort_pool + ea->file_offset;
	const char *fb = sort_pool + eb->file_offset;
	int by_track = 0;
	if (cue_order_tracks(fa, fb, &by_track)) {
		return by_track;
	}
	return strcasecmp(fa, fb);
}

// ---------------------------------------------------------------------------
// the row widgets
// ---------------------------------------------------------------------------

typedef struct {
	lv_obj_t *button;
	lv_obj_t *icon;
	lv_obj_t *label;
	lv_obj_t *check;	// the tick on a chosen row in selection mode
	lv_obj_t *playmark; // the accent bar on the row of the playing file

	// Which entry this row is currently showing, or -1 when parked.
	int index;

	// And which listing that index belongs to. An index on its own says
	// nothing across a folder change: entry 5 of one folder and entry 5 of the
	// next are different things, and row_bind() skips its work when the index
	// has not moved. Every listing gets a number, a row carries the number it
	// was filled from, and the pair is what "this row is up to date" means.
	uint32_t generation;

	bool has_thumb;
	bool thumb_requested; // the worker has been asked; the answer has not come back
	bool thumb_settled;	  // the answer came back, whatever it was
	cover_image_t thumb;
} row_t;

static row_t rows[ROW_POOL];

// Bumped every time a finished listing is taken over. Starts at 1 so a row
// that has never been filled (generation 0) is never mistaken for a current
// one.
static uint32_t listing_generation = 1;

static void adopt_listing(void);
static bool reader_has_result(void);
static bool reader_busy(void);
static void row_update_selection(row_t *row);
static bool list_directory(const char *path, listing_t *out, cue_sheet_t *sheets, char (*claims)[512]);

static void row_show_glyph(row_t *row, bool is_dir) {
	// Audio files get the note, not a generic sheet of paper -- everything a
	// browser row can show besides a folder is playable anyway.
	lv_image_set_src(row->icon, is_dir ? &icon_folder : &icon_music2);
	lv_obj_add_style(row->icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);
}

// Album art must not be tinted, so the recolour the glyphs rely on is switched
// off for as long as a picture is on the row.
static void row_show_cover(row_t *row, const lv_image_dsc_t *dsc) {
	lv_image_set_src(row->icon, dsc);
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_TRANSP, 0);
}

static int row_slot(const row_t *row) { return (int)(row - rows); }

static void row_drop_thumb(row_t *row) {
	// Whatever the worker was doing for this row is no longer wanted.
	coverloader_release(row_slot(row));

	if (row->has_thumb) {
		// The widget has to stop pointing at the pixels before they go.
		bool is_dir = row->index >= 0 && (size_t)row->index < shown.count && shown.entries[row->index].is_dir;
		row_show_glyph(row, is_dir);
		cover_free(&row->thumb);
		row->has_thumb = false;
	}

	row->thumb_requested = false;
	row->thumb_settled = false;
}

// The file playing right now, cached: row_bind runs on every scroll step and
// this saves it a device_state_get() per row.
static char np_path[512];

static void row_update_playmark(row_t *row) {
	if (!row->playmark) {
		return;
	}

	bool playing = false;
	if (row->index >= 0 && (size_t)row->index < shown.count && np_path[0]) {
		char full[sizeof(current_path) + 256 + 1];
		snprintf(full, sizeof(full), "%s/%s", current_path, entry_file((size_t)row->index));
		if (shown.entries[row->index].is_dir) {
			// A folder carries the mark when the track is somewhere under it,
			// at any depth: walking down to what is playing should not pass
			// through rows that say nothing. The separator has to be there as
			// well, or "Rock" would claim the track inside "Rockabilly".
			size_t len = strlen(full);
			playing = strncmp(np_path, full, len) == 0 && np_path[len] == '/';
		} else {
			playing = strcmp(full, np_path) == 0;
		}
	}

	if (playing) {
		lv_obj_set_hidden(row->playmark, false);
	} else {
		lv_obj_set_hidden(row->playmark, true);
	}
}

void browser_notify_now_playing(void) {
	if (!rows[0].button) {
		return; // called during the boot restore, before the page exists
	}

	device_state_t state;
	device_state_get(&state);
	snprintf(np_path, sizeof(np_path), "%s", state.current_file);

	for (int i = 0; i < ROW_POOL; i++) {
		row_update_playmark(&rows[i]);
	}
}

// Points a row at an entry. Cheap by design: it moves a widget and rewrites a
// label, it does not create anything.
static void row_bind(row_t *row, int index) {
	if (row->index == index && row->generation == listing_generation) {
		return;
	}

	row_drop_thumb(row);
	row->index = index;
	row->generation = listing_generation;

	if (index < 0 || (size_t)index >= shown.count) {
		lv_obj_set_hidden(row->button, true);
		lv_obj_set_hidden(row->check, true);
		// The mark goes with the entry, or it outlives it: the row is hidden
		// here, but set_message() shows the first one again for "reading" and
		// "no audio files" without rebinding it, and a row that comes back for
		// a folder with fewer entries than there are widgets is never rebound
		// at all.
		lv_obj_set_hidden(row->playmark, true);
		return;
	}

	lv_obj_set_hidden(row->button, false);
	lv_obj_set_y(row->button, index * ROW_PITCH);
	lv_label_set_text(row->label, entry_name((size_t)index));
	row_show_glyph(row, shown.entries[index].is_dir != 0);
	row_update_playmark(row);
	row_update_selection(row);
}

// ---------------------------------------------------------------------------
// windowing
// ---------------------------------------------------------------------------

// Assigns the row widgets to the band of entries around the scroll position,
// the way the stock player's listview does: quantise the scroll offset to the
// row pitch, start one row early, and cover the viewport from there.
//
// Entry `index` always lives in widget `index % ROW_POOL`. That stability is
// the point: when the band slides down by one, nine of the ten entries are
// still on screen, and with this mapping their rows are simply left alone --
// only the one that scrolled out gets rebound to the one that scrolled in.
// An assignment of rows[i] = first + i would rename every row on every step of
// the band, and a rename drops the row's artwork: the covers would be stripped
// off rows that never left the screen and refetched at every notch.
static void window_update(void) {
	if (shown.count == 0) {
		for (int i = 0; i < ROW_POOL; i++) {
			row_bind(&rows[i], -1);
		}
		return;
	}

	int scroll = lv_obj_get_scroll_y(file_list);
	if (scroll < 0) {
		scroll = 0;
	}

	int first = (scroll / ROW_PITCH) - 1;
	if (first + ROW_POOL > (int)shown.count) {
		first = (int)shown.count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}

	// ROW_POOL consecutive indices touch each widget exactly once.
	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		row_bind(&rows[index % ROW_POOL], index < (int)shown.count ? index : -1);
	}
}

static void list_scroll_cb(lv_event_t *e) {
	(void)e;

	window_update();

	if (thumb_timer) {
		lv_timer_resume(thumb_timer);
	}
}

// ---------------------------------------------------------------------------
// thumbnails
//
// Only the rows that exist can hold artwork, and there are ten of them, so the
// memory artwork occupies is fixed no matter how long the folder is.
// ---------------------------------------------------------------------------

// Runs on the UI thread every tick. It only posts requests and picks up
// finished pictures -- the decoding happens on the worker thread, so however
// slow a cover is, the list keeps scrolling.
static void thumbs_update(void) {
	// A folder the reader thread has finished with is taken over here, on the
	// UI thread, where touching widgets is allowed.
	if (reader_has_result()) {
		adopt_listing();
	}

	bool busy = false;

	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &rows[i];

		if (row->index < 0 || row->thumb_settled) {
			continue;
		}

		// Folders keep their folder icon: reading artwork for every album just
		// to scroll past it is memory spent on a row that already says
		// "folder".
		if (shown.entries[row->index].is_dir) {
			row->thumb_settled = true;
			continue;
		}

		if (!row->thumb_requested) {
			char entry_path[sizeof(current_path) + 256 + 1];
			snprintf(entry_path, sizeof(entry_path), "%s/%s", current_path, entry_file((size_t)row->index));

			coverloader_request(row_slot(row), entry_path, THUMB_SIZE);
			row->thumb_requested = true;
			busy = true;
			continue;
		}

		bool finished = false;
		cover_image_t image;
		bool found = coverloader_take(row_slot(row), &image, &finished);

		if (!finished) {
			busy = true; // still decoding
			continue;
		}

		row->thumb_settled = true;
		if (found) {
			row->thumb = image;
			row->has_thumb = true;
			row_show_cover(row, &row->thumb.dsc);
		}
	}

	if (!busy && !reader_busy()) {
		lv_timer_pause(thumb_timer);
	}
}

static void thumb_timer_cb(lv_timer_t *timer) {
	(void)timer;
	thumbs_update();
}

// ---------------------------------------------------------------------------

static bool is_playable_file(const char *name) {
	// .m4b and .m4a are here because an audiobook is a file like any other in
	// the browser, and a folder of them has to queue.
	//
	// .mp4 is deliberately absent: the decoder can read it, but a .mp4 is
	// almost always a film, and the decision here is made from the name alone
	// -- opening every container to find out would be one extra read per row
	// of every folder. An audio-only .mp4 is still found by the library scan,
	// which opens the container anyway (see is_video_file in library.c).
	static const char *const playable_exts[] = {".wav",  ".mp3",  ".flac", ".ogg", ".m4b", ".m4a",
												 ".alac", ".aac",  ".dsf",  ".dff", ".aif", ".aiff",
												 ".aifc", ".caf",  ".opus", ".wv",   ".ape"};

	if (playlist_is_junk_name(name)) {
		return false; // a macOS resource fork with a .flac on the end is not a track
	}

	for (size_t i = 0; i < sizeof(playable_exts) / sizeof(playable_exts[0]); i++) {
		if (has_extension(name, playable_exts[i]))
			return true;
	}
	return false;
}

static void browser_open_dir(const char *path);

// Scroll positions remembered while walking DOWN the tree, restored on the
// way back up -- so going into an album and back does not land at the top of
// a long folder. One slot per depth level.
#define SCROLL_STACK_DEPTH 12
static int scroll_stack[SCROLL_STACK_DEPTH];
static int scroll_stack_len;
static int pending_scroll = -1; // applied by adopt_listing() when >= 0

// Which row the pointer is over, or -1. The pool of row objects is reused as
// the list scrolls, so a row's index has to be looked up rather than carried.
static row_t *row_of_button(lv_obj_t *button) {
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].button == button) {
			return &rows[i];
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// selection
//
// A long press on a row starts it, as on the library lists: from then on a tap
// chooses a row or lets it go, and letting go of the last one ends it. The
// corner trades its buttons for the ones that act on what is chosen -- the
// queue, the favourites, a playlist -- and close. A chosen folder stands for
// everything playable under it, at any depth, in the order the browser lists
// it. Opening another folder or leaving the page ends the mode.
// ---------------------------------------------------------------------------

#define SELECTION_MAX_TRACKS 20000
#define GATHER_MAX_DEPTH 16

static bool selecting;
static uint8_t *chosen; // one per entry of `shown`, while selecting
static size_t chosen_count;
static bool gathering; // the chosen rows are being turned into tracks

static lv_obj_t *sel_queue_btn;
static lv_obj_t *sel_fav_btn;
static lv_obj_t *sel_add_btn;
static lv_obj_t *sel_close_btn;

static void update_corner_buttons(void);

static void row_update_selection(row_t *row) {
	bool on = selecting && chosen && row->index >= 0 && (size_t)row->index < shown.count && chosen[row->index];
	if (on) {
		lv_obj_set_style_image_recolor(row->check, theme()->accent, 0);
		lv_obj_set_hidden(row->check, false);
	} else {
		lv_obj_set_hidden(row->check, true);
	}
}

static void rows_update_selection(void) {
	for (int i = 0; i < ROW_POOL; i++) {
		row_update_selection(&rows[i]);
	}
}

static void selection_stop(void) {
	if (!selecting) {
		return;
	}
	selecting = false;
	free(chosen);
	chosen = NULL;
	chosen_count = 0;
	update_corner_buttons();
	rows_update_selection();
}

static bool selection_start(void) {
	if (selecting) {
		return true;
	}
	if (shown.count == 0) {
		return false;
	}
	chosen = calloc(shown.count, 1);
	if (!chosen) {
		return false;
	}
	selecting = true;
	chosen_count = 0;
	update_corner_buttons();
	rows_update_selection();
	return true;
}

// Chooses the row, or lets it go. Letting go of the last one leaves the mode.
static void selection_toggle(row_t *row) {
	int index = row->index;
	if (!selecting || index < 0 || (size_t)index >= shown.count) {
		return;
	}
	chosen[index] = !chosen[index];
	if (chosen[index]) {
		chosen_count++;
	} else if (--chosen_count == 0) {
		selection_stop();
		return;
	}
	row_update_selection(row);
}

static void entry_long_pressed_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active() || gathering) {
		return;
	}
	row_t *row = row_of_button(lv_event_get_current_target(e));
	if (!row || row->index < 0 || (size_t)row->index >= shown.count) {
		return;
	}

	// The lift that ends this press would otherwise arrive as a click and
	// take the row straight back out of the selection.
	lv_indev_t *indev = lv_indev_active();
	if (indev) {
		lv_indev_wait_release(indev);
	}

	if (!selection_start()) {
		return;
	}
	if (!chosen[row->index]) {
		selection_toggle(row);
	}
}

// What the chosen rows turn into, and where it goes.
typedef enum {
	SEND_QUEUE,
	SEND_FAVOURITES,
	SEND_PLAYLIST,
} send_t;

typedef struct {
	send_t to;

	char **roots; // the chosen rows, in list order
	uint8_t *root_is_dir;
	int root_count;

	char **paths; // the tracks behind them
	int count;
	int cap;

	cue_sheet_t *sheets;
	char claims[CUE_CLAIM_MAX][512];
} gather_t;

static void gather_free(gather_t *g) {
	for (int i = 0; i < g->root_count; i++) {
		free(g->roots[i]);
	}
	for (int i = 0; i < g->count; i++) {
		free(g->paths[i]);
	}
	free(g->roots);
	free(g->root_is_dir);
	free(g->paths);
	free(g->sheets);
	free(g);
}

// False once no more can be taken.
static bool gather_add(gather_t *g, const char *path) {
	if (g->count >= SELECTION_MAX_TRACKS) {
		return false;
	}
	if (g->count == g->cap) {
		int cap = g->cap ? g->cap * 2 : 64;
		char **grown = realloc(g->paths, (size_t)cap * sizeof(*grown));
		if (!grown) {
			return false;
		}
		g->paths = grown;
		g->cap = cap;
	}
	char *copy = strdup(path);
	if (!copy) {
		return false;
	}
	g->paths[g->count++] = copy;
	return true;
}

// Everything playable under `dir`, in the order the browser lists it: its
// folders first, each walked in turn, then its own tracks.
static bool gather_walk(gather_t *g, const char *dir, int depth) {
	if (depth > GATHER_MAX_DEPTH) {
		return true;
	}
	listing_t l = {0};
	bool more = true;
	if (list_directory(dir, &l, g->sheets, g->claims)) {
		for (size_t i = 0; i < l.count && more; i++) {
			char path[PATH_BUF];
			if (snprintf(path, sizeof(path), "%s/%s", dir, l.pool + l.entries[i].file_offset) >= (int)sizeof(path)) {
				continue;
			}
			more = l.entries[i].is_dir ? gather_walk(g, path, depth + 1) : gather_add(g, path);
		}
	}
	free(l.entries);
	free(l.pool);
	return more;
}

static void gather_done_cb(void *arg) {
	gather_t *g = arg;
	gathering = false;
	toast_busy_end();

	if (g->count == 0) {
		toast_error("browser_no_audio_files");
		gather_free(g);
		return;
	}

	switch (g->to) {
	case SEND_QUEUE: {
		int queued = 0;
		for (int i = 0; i < g->count; i++) {
			queued += playlist_insert_next(g->paths[i]) ? 1 : 0;
		}
		if (queued > 0) {
			// Written down now: the mirror on disk is otherwise refreshed only
			// when a track is loaded, and a power-off before that loses the
			// addition.
			device_state_queue_changed();
			toast_success(queued == 1 ? "added_to_the_queue" : "queue_tracks_added");
		} else {
			toast_error("playlist_cannot_add_the_tracks");
		}
		break;
	}
	case SEND_FAVOURITES:
		playlistpage_add_favourites((const char *const *)g->paths, g->count);
		break;
	case SEND_PLAYLIST:
		playlistpage_add_tracks((const char *const *)g->paths, g->count);
		break;
	}
	gather_free(g);
}

static void *gather_worker(void *arg) {
	gather_t *g = arg;
	thread_be_low_priority("browser gather");
	bool more = true;
	for (int i = 0; i < g->root_count && more; i++) {
		more = g->root_is_dir[i] ? gather_walk(g, g->roots[i], 0) : gather_add(g, g->roots[i]);
	}
	gui_post(gather_done_cb, g);
	return NULL;
}

// Hands the chosen rows to a thread that turns them into tracks -- a folder
// can hold a whole collection, and walking it is card reads -- and leaves the
// mode.
static void selection_send(send_t to) {
	if (!selecting || chosen_count == 0 || gathering) {
		return;
	}
	gather_t *g = calloc(1, sizeof(*g));
	if (!g) {
		return;
	}
	g->to = to;
	g->roots = calloc(chosen_count, sizeof(*g->roots));
	g->root_is_dir = calloc(chosen_count, 1);
	g->sheets = malloc(sizeof(*g->sheets)); // NULL only leaves cue sheets unsplit
	if (!g->roots || !g->root_is_dir) {
		gather_free(g);
		return;
	}
	for (size_t i = 0; i < shown.count && (size_t)g->root_count < chosen_count; i++) {
		if (!chosen[i]) {
			continue;
		}
		char path[PATH_BUF];
		snprintf(path, sizeof(path), "%s/%s", current_path, entry_file(i));
		g->roots[g->root_count] = strdup(path);
		if (!g->roots[g->root_count]) {
			break;
		}
		g->root_is_dir[g->root_count] = shown.entries[i].is_dir;
		g->root_count++;
	}
	selection_stop();

	gathering = true;
	toast_busy("browser_gathering_tracks");
	pthread_t thread;
	if (pthread_create(&thread, NULL, gather_worker, g) != 0) {
		gather_done_cb(g);
		return;
	}
	pthread_detach(thread);
}

static void sel_queue_cb(lv_event_t *e) {
	(void)e;
	selection_send(SEND_QUEUE);
}

static void sel_fav_cb(lv_event_t *e) {
	(void)e;
	selection_send(SEND_FAVOURITES);
}

static void sel_add_cb(lv_event_t *e) {
	(void)e;
	selection_send(SEND_PLAYLIST);
}

static void sel_close_cb(lv_event_t *e) {
	(void)e;
	selection_stop();
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	selection_stop();
}

static void entry_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return; // a swipe across the row, not a tap on it
	}

	row_t *row = row_of_button(lv_event_get_target(e));
	if (!row || row->index < 0 || (size_t)row->index >= shown.count) {
		return;
	}
	int index = row->index;

	if (selecting) {
		selection_toggle(row);
		return;
	}

	char full_path[sizeof(current_path) + 256 + 1];
	snprintf(full_path, sizeof(full_path), "%s/%s", current_path, entry_file((size_t)index));

	if (shown.entries[index].is_dir) {
		// Remember where this level was scrolled to for the way back up.
		if (scroll_stack_len < SCROLL_STACK_DEPTH) {
			scroll_stack[scroll_stack_len++] = lv_obj_get_scroll_y(file_list);
		}
		pending_scroll = -1;
		browser_open_dir(full_path);
	} else {
		player_play_file(full_path);
		switch_screen(player_screen);
	}
}

// True when the folder on screen has at least one track in it. The listing is
// already filtered to folders and playable files (read_directory), so anything
// that is not a folder is a track.
static bool listing_has_tracks(void) {
	for (size_t i = 0; i < shown.count; i++) {
		if (!shown.entries[i].is_dir) {
			return true;
		}
	}
	return false;
}

static void show_if(lv_obj_t *obj, bool show) {
	if (!obj) {
		return;
	}
	if (show) {
		lv_obj_set_hidden(obj, false);
	} else {
		lv_obj_set_hidden(obj, true);
	}
}

static void update_corner_buttons(void) {
	show_if(root_button, !selecting && browser_can_go_up());
	show_if(shuffle_button, !selecting && listing_has_tracks());
	show_if(sel_queue_btn, selecting);
	show_if(sel_fav_btn, selecting);
	show_if(sel_add_btn, selecting);
	show_if(sel_close_btn, selecting);
	if (title_label) {
		settingsrow_title_corner_slots(title_label, config, selecting ? 4 : 2);
	}
}

static void root_button_cb(lv_event_t *e) {
	(void)e;
	browser_open_dir(root_path);
}

// This folder, in a random order.
//
// Deliberately not device_state_play_list() with the folder's files, which is
// how the library lists do it: that builds a custom queue, and a custom queue
// is exactly what turns "Play by folder" off -- chain_beside() in device_state
// refuses to walk on to the next folder when the queue was handed to it. So
// this goes the same way a tapped file goes, through the folder queue, and only
// changes two things about it: the mode is set to shuffle first, so the queue
// is dealt shuffled as it loads, and the track it starts from is drawn at
// random rather than being the one under the finger.
static void shuffle_button_cb(lv_event_t *e) {
	(void)e;
	if (!current_path[0]) {
		return;
	}

	size_t tracks = 0;
	for (size_t i = 0; i < shown.count; i++) {
		if (!shown.entries[i].is_dir) {
			tracks++;
		}
	}
	if (tracks == 0) {
		return;
	}

	size_t wanted = tracks > 1 ? (size_t)lv_rand(0, (uint32_t)(tracks - 1)) : 0;
	size_t seen = 0;
	const char *name = NULL;
	for (size_t i = 0; i < shown.count; i++) {
		if (shown.entries[i].is_dir) {
			continue;
		}
		if (seen++ == wanted) {
			name = entry_file(i);
			break;
		}
	}
	if (!name) {
		return;
	}

	playlist_set_mode(musicsettings_endless_shuffle() ? PLAYBACK_MODE_SHUFFLE_REPEAT : PLAYBACK_MODE_SHUFFLE);

	char full_path[1024];
	snprintf(full_path, sizeof(full_path), "%s/%s", current_path, name);
	player_play_file(full_path);
	switch_screen(player_screen);
}

void browser_open_dir_public(const char *path) { browser_open_dir(path); }

void browser_refresh(void) {
	if (current_path[0]) {
		browser_open_dir(current_path);
	}
}

// Whether the chevron/swipe would walk up a folder rather than leave the
// page. The swipe-back drag asks, because "up a folder" has no ready-made
// previous page to show underneath.
bool browser_can_go_up(void) { return current_path[0] && strcmp(current_path, root_path) != 0; }

bool browser_go_up(void) {
	if (strcmp(current_path, root_path) == 0) {
		return false; // already at root: the caller leaves the browser instead
	}

	char parent[512];
	strncpy(parent, current_path, sizeof(parent) - 1);
	parent[sizeof(parent) - 1] = '\0';

	char *slash = strrchr(parent, '/');
	if (slash && slash != parent) {
		*slash = '\0';
	}

	// Don't allow navigating above the SD card root.
	if (strncmp(parent, root_path, strlen(root_path)) != 0) {
		strncpy(parent, root_path, sizeof(parent) - 1);
		parent[sizeof(parent) - 1] = '\0';
	}

	// Going up: restore where this level was left, if it was remembered.
	pending_scroll = scroll_stack_len > 0 ? scroll_stack[--scroll_stack_len] : -1;

	browser_open_dir(parent);
	return true;
}

// The first row doubles as the place messages are shown: there is no reason to
// build a widget just to say a folder is empty.
static void set_message(const char *text) {
	lv_obj_set_hidden(rows[0].button, false);
	lv_obj_set_y(rows[0].button, 0);
	lv_obj_set_hidden(rows[0].icon, true);
	// A sentence is not a track: whatever this row was showing before, it is
	// not playing now.
	lv_obj_set_hidden(rows[0].playmark, true);
	lv_label_set_text(rows[0].label, tr(text));
	lv_obj_add_style(rows[0].label, &style_list_error, 0);
}

static void clear_message(void) {
	lv_obj_set_hidden(rows[0].icon, false);
	lv_obj_remove_style(rows[0].label, &style_list_error, 0);
}

// ---------------------------------------------------------------------------
// the reader thread
//
// opendir/readdir on a microSD card is slow enough to be felt: hundreds of
// entries, each one a read from the card, and on exFAT the kernel does not
// even fill in d_type. None of that can happen on the thread that draws the
// interface.
// ---------------------------------------------------------------------------

static pthread_mutex_t reader_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reader_wake = PTHREAD_COND_INITIALIZER;

static char reader_request[512]; // the folder to read; empty when idle
static bool reader_pending;
static bool reader_working; // the reader picked the request up and is on it
static bool reader_ready;	// a finished listing is waiting to be adopted
static bool reader_failed;
static bool reader_started;

// Which navigation a listing answers. Every submit bumps `request_seq`; the
// reader stamps `ready_seq` with the sequence it served when it finishes. The
// two matching is the only proof that a ready listing is the one the user is
// currently waiting for.
//
// Asking "is something ready?" under one hold of the lock and swapping the
// buffers under another opens a race: between the two the reader can pick up the
// next request and start rewriting `staging`, and the swap then hands the
// interface a listing that is being realloc'd under it. Both questions are
// therefore answered inside one hold (see adopt_listing).
static uint32_t request_seq;
static uint32_t ready_seq;

static bool is_playable_file(const char *name);

// Reads one directory into `out`, sorted: its folders and what can be played
// in it, as the browser lists them. `sheets` is a buffer for parsing its cue
// sheets (NULL leaves them unsplit) and `claims` room for CUE_CLAIM_MAX names.
// False when the folder cannot be opened. Off the UI thread.
static bool list_directory(const char *path, listing_t *out, cue_sheet_t *sheets, char (*claims)[512]) {
	out->count = 0;
	out->pool_used = 0;

	DIR *dir = opendir(path);
	if (!dir) {
		return false;
	}

	size_t skipped = 0;
	struct dirent *de;
	errno = 0;

	// The cue sheets first, because what they contain decides what the second
	// pass is allowed to list.
	//
	// A sheet describes one long file cut into tracks, and the browser lists it
	// the way the library indexes it -- one row per track -- so the same album
	// is the same thing whichever page it is reached through.
	//
	// The big file is claimed and not listed: it is the same audio as the rows
	// above it, and a listing that offers both offers the same album twice.
	size_t claim_count = 0;
	if (sheets) {
		while ((de = readdir(dir)) != NULL) {
			if (de->d_name[0] == '.' || !cue_is_sheet(de->d_name)) {
				continue;
			}
			char sheet_path[PATH_BUF + sizeof(de->d_name) + 1];
			snprintf(sheet_path, sizeof(sheet_path), "%s/%s", path, de->d_name);
			if (!cue_parse(sheet_path, sheets) || sheets->split) {
				continue; // not a sheet this player can follow: leave the folder as it is
			}

			for (int t = 0; t < sheets->track_count; t++) {
				char file[300];
				cue_virtual_path(de->d_name, sheets->tracks[t].number, file, sizeof(file));

				// The row says what the sheet says. A sheet with no title for a
				// track is rare and still has to read as something.
				char label[220];
				const char *title = sheets->tracks[t].title;
				if (title[0]) {
					snprintf(label, sizeof(label), "%d. %s", sheets->tracks[t].number, title);
				} else {
					snprintf(label, sizeof(label), "%d", sheets->tracks[t].number);
				}

				if (out->count >= MAX_ENTRIES || !listing_add(out, label, file, false)) {
					break;
				}
			}

			// Only the name: the second pass compares names, and a sheet whose
			// audio lives in another folder claims nothing here.
			const char *slash = strrchr(sheets->audio_path, '/');
			if (claim_count < CUE_CLAIM_MAX) {
				snprintf(claims[claim_count], sizeof(claims[0]), "%s", slash ? slash + 1 : sheets->audio_path);
				claim_count++;
			}
		}
		rewinddir(dir);
		errno = 0;
	}

	while ((de = readdir(dir)) != NULL) {
		// Hidden entries stay hidden -- including the player's own .local
		// folder, which has no business showing up as something to browse -- and
		// so do the folders a desktop leaves behind, which do not begin with a
		// dot: __MACOSX, the recycle bin, System Volume Information.
		if (de->d_name[0] == '.' || playlist_is_junk_name(de->d_name)) {
			continue;
		}

		// Trust d_type when the filesystem fills it in; it saves a stat() per
		// entry. exFAT does not fill it in -- every entry comes back
		// DT_UNKNOWN -- so a folder of eight hundred tracks would be eight
		// hundred stat() calls against a slow card. A name ending in .flac is
		// a track and there is nothing to ask the filesystem about; only names
		// that could go either way are worth a stat.
		bool is_dir;
		if (de->d_type == DT_DIR) {
			is_dir = true;
		} else if (de->d_type == DT_REG || de->d_type == DT_LNK) {
			is_dir = false;
		} else if (is_playable_file(de->d_name)) {
			is_dir = false;
		} else {
			char entry_path[PATH_BUF + sizeof(de->d_name) + 1];
			snprintf(entry_path, sizeof(entry_path), "%s/%s", path, de->d_name);

			struct stat st;
			if (stat(entry_path, &st) != 0) {
				errno = 0;
				continue;
			}
			is_dir = S_ISDIR(st.st_mode);
		}

		if (!is_dir && !is_playable_file(de->d_name)) {
			continue; // unplayable files are not listed
		}

		if (!is_dir) {
			bool claimed = false;
			for (size_t i = 0; i < claim_count && !claimed; i++) {
				claimed = strcmp(claims[i], de->d_name) == 0;
			}
			if (claimed) {
				continue; // a sheet above already listed this file, track by track
			}
		}

		if (out->count >= MAX_ENTRIES) {
			skipped++;
			continue;
		}

		if (!listing_add(out, de->d_name, NULL, is_dir)) {
			fprintf(stderr, "browser: out of memory at %zu entries\n", out->count);
			break;
		}
	}

	// readdir() returns NULL both at the end of the directory and on failure.
	// Telling them apart matters: a truncated listing otherwise just looks like
	// a folder with fewer files in it.
	if (errno != 0) {
		fprintf(stderr, "browser: readdir stopped after %zu entries in '%s': %s\n", out->count, path,
				strerror(errno));
	}

	closedir(dir);

	if (skipped > 0) {
		fprintf(stderr, "browser: '%s' holds more than %d entries, %zu not shown\n", path, MAX_ENTRIES, skipped);
	}

	sort_pool = out->pool;
	qsort(out->entries, out->count, sizeof(*out->entries), entry_cmp);
	return true;
}

// Reads one directory into `staging`. Runs on the reader thread with the lock
// released, so nothing on the UI side waits for it. `seq` identifies which
// request this answers.
static void read_directory(const char *path, uint32_t seq) {
	bool ok = list_directory(path, &staging, cue_sheets, cue_claims);

	// Whatever the previous folder's artwork left behind goes back to the
	// kernel here rather than sitting in the arena as a high-water mark -- and
	// off the UI thread, where walking the heap would be one more pause.
	if (ok) {
		malloc_trim(0);
	}

	pthread_mutex_lock(&reader_lock);
	reader_failed = !ok;
	reader_ready = true;
	ready_seq = seq;
	pthread_mutex_unlock(&reader_lock);
}

static void *reader_thread(void *arg) {
	(void)arg;

	// One core: a stat() storm at normal priority would fight the interface
	// for it.
	thread_be_background("browser reader");

	for (;;) {
		pthread_mutex_lock(&reader_lock);
		while (!reader_pending) {
			pthread_cond_wait(&reader_wake, &reader_lock);
		}

		char path[sizeof(reader_request)];
		memcpy(path, reader_request, sizeof(path));
		uint32_t seq = request_seq;
		reader_pending = false;
		reader_ready = false;

		// `working` is what keeps reader_busy() true for the whole read.
		// Without it there is a gap -- request picked up (pending false),
		// listing not finished (ready false) -- in which the browser's tick
		// timer sees nothing happening and pauses itself, leaving the finished
		// listing uncollected and the folder on the "reading" notice forever.
		// The gap is seconds wide at SCHED_IDLE on this single core.
		reader_working = true;
		pthread_mutex_unlock(&reader_lock);

		read_directory(path, seq);

		pthread_mutex_lock(&reader_lock);
		reader_working = false; // ready is already true, so no busy-gap opens here
		pthread_mutex_unlock(&reader_lock);
	}

	return NULL;
}

// True when a listing is sitting there waiting to be adopted.
static bool reader_has_result(void) {
	pthread_mutex_lock(&reader_lock);
	bool ready = reader_ready;
	pthread_mutex_unlock(&reader_lock);
	return ready;
}

// True while a folder is queued, being read, or read and not yet adopted.
static bool reader_busy(void) {
	pthread_mutex_lock(&reader_lock);
	bool busy = reader_pending || reader_working || reader_ready;
	pthread_mutex_unlock(&reader_lock);
	return busy;
}

// Asks for a folder. Returns at once; the listing turns up on a later tick.
static void reader_submit(const char *path) {
	pthread_mutex_lock(&reader_lock);

	snprintf(reader_request, sizeof(reader_request), "%s", path);
	request_seq++;
	reader_pending = true;
	reader_ready = false;

	pthread_cond_signal(&reader_wake);
	pthread_mutex_unlock(&reader_lock);
}

static void reader_start(void) {
	if (reader_started) {
		return;
	}
	reader_started = true;

	pthread_t thread;
	if (pthread_create(&thread, NULL, reader_thread, NULL) != 0) {
		fprintf(stderr, "browser: could not start the directory reader\n");
		reader_started = false;
	} else {
		pthread_detach(thread);
	}
}

static void browser_open_dir(const char *path) {
	// The chosen rows are entries of the listing about to go.
	selection_stop();

	// browser_refresh() passes current_path straight back in, so guard against
	// copying the buffer onto itself.
	if (path != current_path) {
		strncpy(current_path, path, sizeof(current_path) - 1);
		current_path[sizeof(current_path) - 1] = '\0';
	}

	if (thumb_timer) {
		lv_timer_pause(thumb_timer);
	}

	// Park every row before the entries they point at go away.
	for (int i = 0; i < ROW_POOL; i++) {
		row_bind(&rows[i], -1);
	}

	shown.count = 0;
	shown.pool_used = 0;
	lv_obj_set_height(list_body, ROW_PITCH);
	lv_obj_scroll_to_y(file_list, 0, LV_ANIM_OFF);

	clear_message();
	set_message("reading");

	// The directory is read on the reader thread. On this device that is not a
	// micro-optimisation: exFAT on a slow card takes seconds to hand over a
	// folder of a few hundred tracks, which is a frozen interface if it happens
	// here.
	reader_submit(current_path);

	lv_timer_reset(thumb_timer);
	lv_timer_resume(thumb_timer);
}

// Takes the finished listing over from the reader thread. Runs on the UI
// thread, from the list tick.
static void adopt_listing(void) {
	// The rows point into `shown`, which is about to change hands. Park them
	// first -- while `shown` is still the listing their indices refer to --
	// so no widget is left holding an index into the new one. This also forces
	// window_update() below to rebind every row: row_bind() is a no-op when the
	// index is unchanged, which is right while scrolling but wrong across a
	// folder change, where index 3 now names a different file.
	for (int i = 0; i < ROW_POOL; i++) {
		row_bind(&rows[i], -1);
	}

	pthread_mutex_lock(&reader_lock);

	// Everything is decided inside this one hold of the lock. `reader_ready`
	// may have been true a tick ago and false now (the reader picked up the
	// next request and is rewriting `staging`); `ready_seq` may be stale (the
	// finished listing answers a folder the user has already left). In either
	// case the swap must not happen -- the listing that is wanted is still on
	// its way, and the timer will call back here when it lands.
	if (!reader_ready || ready_seq != request_seq) {
		pthread_mutex_unlock(&reader_lock);
		// The parking above hid the message row too; put the notice back so
		// the wait for the listing that is still coming stays visible.
		set_message("reading");
		return;
	}

	// Swap rather than copy: the buffers change hands and get reused, so a
	// folder change allocates nothing. Safe precisely because the reader is
	// provably idle: reader_ready went true only after it finished writing,
	// and it cannot start again without taking the lock this thread holds.
	listing_t finished = staging;
	staging = shown;
	shown = finished;

	staging.count = 0;
	staging.pool_used = 0;

	// From here on every row holds a stale index, whatever number it is.
	listing_generation++;

	bool failed = reader_failed;
	reader_ready = false;
	pthread_mutex_unlock(&reader_lock);

	clear_message();

	// The body is as tall as the whole list would be: that is what the scroll
	// position and the scrollbar come from, while only ROW_POOL widgets exist.
	lv_obj_set_height(list_body, shown.count ? (int)(shown.count * ROW_PITCH) : ROW_PITCH);

	// Fresh folders start at the top; a walk back up resumes where the level
	// was left (clamped: the folder may have shrunk meanwhile).
	int target_scroll = pending_scroll >= 0 ? pending_scroll : 0;
	pending_scroll = -1;
	int max_scroll = (int)(shown.count * ROW_PITCH) - lv_obj_get_height(file_list);
	if (target_scroll > max_scroll) {
		target_scroll = max_scroll > 0 ? max_scroll : 0;
	}
	lv_obj_scroll_to_y(file_list, target_scroll, LV_ANIM_OFF);

	if (failed) {
		set_message("browser_open_failed");
	} else if (shown.count == 0) {
		set_message("browser_no_audio_files");
	} else {
		window_update();
	}

	update_corner_buttons();

	fprintf(stderr, "browser: '%s': %zu entries, %d row widgets, %zu KB of names\n", current_path, shown.count,
			ROW_POOL, shown.pool_used / 1024);
}

// ---------------------------------------------------------------------------

static void init_list_styles(void) {
	lv_style_init(&style_list_error);
	lv_style_set_text_color(&style_list_error, lv_color_make(212, 94, 76));

	// Row geometry. The colours live in the shared theme styles, so switching
	// palette repaints the list without touching a single row.
	lv_style_init(&style_list_btn);
	lv_style_set_border_width(&style_list_btn, 0);
	lv_style_set_radius(&style_list_btn, LIST_ROW_RADIUS);
	lv_style_set_shadow_width(&style_list_btn, 0);
	lv_style_set_pad_all(&style_list_btn, LIST_ROW_PAD);
	lv_style_set_pad_column(&style_list_btn, 14);
}

// Builds the fixed pool of rows. This runs once, at startup: from here on the
// browser never creates or destroys a widget, whatever the user opens.
static void build_rows(int width) {
	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &rows[i];

		row->button = lv_btn_create(list_body);
		lv_obj_set_size(row->button, width, ROW_HEIGHT);
		lv_obj_set_x(row->button, 0);
		lv_obj_add_style(row->button, &theme_style_card, 0);
		lv_obj_add_style(row->button, &style_list_btn, 0);
		lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_hidden(row->button, true);
		lv_obj_set_event_bubble(row->button, true); // so a swipe can start on a row
		lv_obj_add_event_cb(row->button, entry_clicked_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_add_event_cb(row->button, entry_long_pressed_cb, LV_EVENT_LONG_PRESSED, NULL);

		lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		row->icon = lv_image_create(row->button);
		lv_obj_set_size(row->icon, THUMB_SIZE, THUMB_SIZE);
		lv_image_set_inner_align(row->icon, LV_IMAGE_ALIGN_CENTER);

		row->label = lv_label_create(row->button);
		lv_label_set_long_mode(row->label, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(row->label, 1);
		lv_obj_add_style(row->label, &theme_style_text, 0);
		lv_obj_set_style_text_font(row->label, &font_ui_24, 0);
		// Two lines at most: the cap on the height is what makes LV_LABEL_LONG_DOT
		// cut the name there, with the ellipsis, instead of wrapping on past the
		// row. A cap and not a height, so a one-line name stays centred.
		lv_obj_set_style_max_height(row->label,
									2 * lv_font_get_line_height(&font_ui_24) +
										lv_obj_get_style_text_line_space(row->label, LV_PART_MAIN),
									0);

		row->check = lv_image_create(row->button);
		lv_image_set_src(row->check, &icon_check);
		lv_obj_add_style(row->check, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(row->check, LV_OPA_COVER, 0);
		lv_obj_set_clickable(row->check, false);
		lv_obj_set_hidden(row->check, true);

		// Outside the flex layout, in the row's own left padding: it marks the
		// row without moving anything on it.
		row->playmark = lv_obj_create(row->button);
		lv_obj_set_ignore_layout(row->playmark, true);
		lv_obj_set_size(row->playmark, PLAYMARK_WIDTH, PLAYMARK_HEIGHT);
		lv_obj_align(row->playmark, LV_ALIGN_LEFT_MID, PLAYMARK_INSET - LIST_ROW_PAD, 0);
		lv_obj_add_style(row->playmark, &theme_style_accent_bg, 0);
		lv_obj_set_style_radius(row->playmark, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_width(row->playmark, 0, 0);
		lv_obj_set_style_shadow_width(row->playmark, 0, 0);
		lv_obj_set_style_pad_all(row->playmark, 0, 0);
		lv_obj_set_scrollable(row->playmark, false);
		lv_obj_set_clickable(row->playmark, false);
		lv_obj_set_hidden(row->playmark, true);

		row->index = -1;
		row->has_thumb = false;
		row->thumb_requested = false;
		row->thumb_settled = false;
	}
}

// A button in the title row's corner, `slot` places from the right edge.
// Hidden until something shows it.
static lv_obj_t *corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(browser_screen);
	lv_obj_set_size(btn, 56, 56);
	lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_pad_all(btn, 0, 0);
	lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * 62, cfg->padding + cfg->top_bar_height);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_set_hidden(btn, true);

	lv_obj_t *image = lv_image_create(btn);
	lv_image_set_src(image, glyph);
	lv_obj_add_style(image, &theme_style_icon, 0);
	lv_obj_center(image);
	return btn;
}

void browser_init(gui_config_t *cfg) {
	init_list_styles();

	// Once, and kept: a folder with a sheet in it is read on the reader thread,
	// and thirty-five kilobytes is not something to take from a thread stack or
	// to allocate and free on every folder. A failure here is not fatal -- the
	// browser goes back to listing the one long file, which is where it was.
	cue_sheets = malloc(sizeof(*cue_sheets));
	if (!cue_sheets) {
		fprintf(stderr, "browser: no room for a cue sheet; sheets will not be split into tracks\n");
	}

	strncpy(root_path, cfg->sd_root_path ? cfg->sd_root_path : "/", sizeof(root_path) - 1);
	root_path[sizeof(root_path) - 1] = '\0';

	lv_obj_add_style(browser_screen, &theme_style_screen, 0);

	config = cfg;
	title_label = settingsrow_title(browser_screen, cfg, "browser_file_browser");
	settingsrow_title_corner_slots(title_label, cfg, 2); // root, and shuffle beside it

	// Straight back to the top of the card, from however deep the walk went.
	// Same corner, size and spacing as the buttons on the Music page, so the
	// two pages' top right corners are the same place.
	root_button = corner_button(cfg, 0, &icon_folder_root, root_button_cb);

	// And this folder in a random order, in the slot to its left. Only where
	// there is something to play: in a folder of nothing but folders it would
	// be a control with no meaning, and the walk down to an album passes
	// through several of those.
	shuffle_button = corner_button(cfg, 1, &icon_shuffle, shuffle_button_cb);

	// Selection mode's buttons, in the same corner while it lasts, from the
	// right: close, a playlist, the favourites, the queue.
	sel_close_btn = corner_button(cfg, 0, &icon_close, sel_close_cb);
	sel_add_btn = corner_button(cfg, 1, &icon_list_plus, sel_add_cb);
	sel_fav_btn = corner_button(cfg, 2, &icon_star_plus, sel_fav_cb);
	sel_queue_btn = corner_button(cfg, 3, &icon_list_queue, sel_queue_cb);
	lv_obj_add_event_cb(browser_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	int content_top = settingsrow_content_top(cfg);

	lv_obj_t *screen_container = lv_obj_create(browser_screen);
	lv_obj_set_size(screen_container, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(screen_container, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(screen_container, 0, 0);
	lv_obj_set_style_border_width(screen_container, 0, 0);
	lv_obj_set_style_pad_hor(screen_container, 0, 0);
	// The default theme pads containers vertically too, which leaves a dead
	// strip at the bottom of the list.
	lv_obj_set_style_pad_ver(screen_container, 0, 0);
	lv_obj_set_style_radius(screen_container, 0, 0);
	lv_obj_set_flex_flow(screen_container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(screen_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(screen_container, cfg->padding, 0);
	lv_obj_set_scrollable(screen_container, false);

	// No path line: the title names the folder and the rows say what is in it,
	// so the raw filesystem path would be a row of space spent on noise.

	// The viewport. It scrolls; nothing inside it is laid out by LVGL, because
	// the rows are positioned by hand from the scroll offset.
	file_list = lv_obj_create(screen_container);
	lv_obj_set_width(file_list, lv_pct(100));
	lv_obj_set_style_bg_opa(file_list, 0, 0);
	lv_obj_set_style_border_width(file_list, 0, 0);
	lv_obj_set_style_radius(file_list, 0, 0);
	lv_obj_set_style_pad_hor(file_list, cfg->padding, 0);
	lv_obj_set_style_pad_ver(file_list, 0, 0);
	lv_obj_set_flex_grow(file_list, 1);
	lv_obj_set_scroll_dir(file_list, LV_DIR_VER);
	lv_obj_add_event_cb(file_list, list_scroll_cb, LV_EVENT_SCROLL, NULL);

	// As tall as the entire list. This is what gives the viewport something to
	// scroll over; the rows inside it are the only widgets that exist.
	list_body = lv_obj_create(file_list);
	lv_obj_set_width(list_body, lv_pct(100));
	lv_obj_set_height(list_body, ROW_PITCH);
	lv_obj_set_style_bg_opa(list_body, 0, 0);
	lv_obj_set_style_border_width(list_body, 0, 0);
	lv_obj_set_style_pad_all(list_body, 0, 0);
	lv_obj_set_scrollable(list_body, false);
	lv_obj_set_event_bubble(list_body, true);

	// Full usable width. The row's own inner padding must not be subtracted
	// here as well, or every row carries ~30 px of dead space down its right
	// edge.
	build_rows(cfg->screen_width - (2 * cfg->padding));

	// The browser too is a surface the player slides in over, and one the
	// swipe-back drag can start on.
	player_sheet_attach_drag(file_list, true);
	switcher_attach_back_gesture(file_list);

	// Artwork is decoded on one thread, directories are read on another; the
	// interface thread only ever draws.
	coverloader_start();
	reader_start();

	thumb_timer = lv_timer_create(thumb_timer_cb, THUMB_TICK_PERIOD_MS, NULL);
	lv_timer_pause(thumb_timer);

	browser_open_dir(root_path);
}
