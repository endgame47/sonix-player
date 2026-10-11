#include "audiobooks.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/audiobookextras.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/sleeptimer.h"

lv_obj_t *audiobooks_screen;
lv_obj_t *audiobooksettings_screen;
lv_obj_t *audiobookscan_screen;
lv_obj_t *audiobookcontrols_screen;

// The two list pages the tiles lead to: books, and authors or series.
static lv_obj_t *books_screen;
static lv_obj_t *names_screen;

static void start_scan(void *user);

// ---------------------------------------------------------------------------
// Which list a page shows, and the order it is read in
//
// Each list keeps its own order, under [audiobook] in the config, and offers
// only the orders that mean something for it.
// ---------------------------------------------------------------------------

typedef enum {
	LIST_LIBRARY,
	LIST_CONTINUE,
	LIST_FINISHED,
	LIST_AUTHOR_BOOKS,
	LIST_SERIES_BOOKS,
	LIST_AUTHORS,
	LIST_SERIES,
	LIST_COUNT,
} list_id_t;

typedef enum {
	SORT_AZ,
	SORT_ZA,
	SORT_NEW,
	SORT_OLD,
	SORT_PLAYED,
	SORT_SERIES,
} sort_choice_t;

typedef struct {
	const char *config_key;
	sort_choice_t fallback;
	int choice_count;
	sort_choice_t choices[6];
} list_sorts_t;

static const list_sorts_t LIST_SORTS[LIST_COUNT] = {
	[LIST_LIBRARY] = {"sort_library", SORT_AZ, 5, {SORT_AZ, SORT_ZA, SORT_NEW, SORT_OLD, SORT_PLAYED}}, [LIST_CONTINUE] = {"sort_continue", SORT_PLAYED, 3, {SORT_PLAYED, SORT_AZ, SORT_ZA}}, [LIST_FINISHED] = {"sort_finished", SORT_NEW, 4, {SORT_AZ, SORT_ZA, SORT_NEW, SORT_OLD}}, [LIST_AUTHOR_BOOKS] = {"sort_author_books", SORT_AZ, 4, {SORT_AZ, SORT_ZA, SORT_NEW, SORT_OLD}}, [LIST_SERIES_BOOKS] = {"sort_series_books", SORT_SERIES, 5, {SORT_SERIES, SORT_AZ, SORT_ZA, SORT_NEW, SORT_OLD}}, [LIST_AUTHORS] = {"sort_authors", SORT_AZ, 4, {SORT_AZ, SORT_ZA, SORT_NEW, SORT_OLD}}, [LIST_SERIES] = {"sort_series", SORT_AZ, 4, {SORT_AZ, SORT_ZA, SORT_NEW, SORT_OLD}},
};

static const char *const SORT_LABELS[] = {
	[SORT_AZ] = "medialist_sort_name_az", [SORT_ZA] = "medialist_sort_name_za", [SORT_NEW] = "medialist_sort_added_new", [SORT_OLD] = "medialist_sort_added_old", [SORT_PLAYED] = "audiobook_sort_played", [SORT_SERIES] = "audiobook_sort_series",
};

static sort_choice_t list_sort(list_id_t list) {
	const list_sorts_t *sorts = &LIST_SORTS[list];
	long value = config_get_int("audiobook", sorts->config_key, sorts->fallback);
	for (int i = 0; i < sorts->choice_count; i++) {
		if (sorts->choices[i] == value) {
			return (sort_choice_t)value;
		}
	}
	return sorts->fallback;
}

static const lv_image_dsc_t *sort_icon(sort_choice_t choice) {
	switch (choice) {
	case SORT_ZA:
		return &icon_sort_za;
	case SORT_NEW:
		return &icon_sort_date_new;
	case SORT_OLD:
		return &icon_sort_date_old;
	case SORT_PLAYED:
		return &icon_sort_played;
	case SORT_SERIES:
		return &icon_sort_series;
	default:
		return &icon_sort_az;
	}
}

static void sort_to_order(sort_choice_t choice, audiobook_order_t *order, bool *desc) {
	*desc = choice == SORT_ZA || choice == SORT_NEW || choice == SORT_PLAYED;
	*order = choice == SORT_NEW || choice == SORT_OLD ? AUDIOBOOK_ORDER_ADDED : choice == SORT_PLAYED ? AUDIOBOOK_ORDER_PLAYED : choice == SORT_SERIES ? AUDIOBOOK_ORDER_SERIES : AUDIOBOOK_ORDER_NAME;
}

// The sort button's menu. `reload` puts the page back together in the new
// order.
static list_id_t sort_menu_list;
static void (*sort_menu_reload)(void);

static void sort_picked(void *user) {
	config_set_int("audiobook", LIST_SORTS[sort_menu_list].config_key, (long)(intptr_t)user);
	config_save();
	if (sort_menu_reload) {
		sort_menu_reload();
	}
}

static void sort_menu_show(lv_obj_t *anchor, list_id_t list, void (*reload)(void)) {
	const list_sorts_t *sorts = &LIST_SORTS[list];
	sort_choice_t current = list_sort(list);
	popover_item_t items[6];
	for (int i = 0; i < sorts->choice_count; i++) {
		sort_choice_t choice = sorts->choices[i];
		items[i] = (popover_item_t){SORT_LABELS[choice], sort_picked, (void *)(intptr_t)choice, choice == current};
	}
	sort_menu_list = list;
	sort_menu_reload = reload;
	popover_show(anchor, items, sorts->choice_count);
}

// A glyph button on the title row, counted from the right edge.
static lv_obj_t *corner_button(lv_obj_t *screen, gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (56 + 6), cfg->padding + cfg->top_bar_height);
	if (cb) {
		lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
	}

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	return button;
}

// The viewport of a windowed list: a scrollable page with a fixed-height canvas
// inside it, so twelve row widgets can stand in for however many entries.
static lv_obj_t *make_viewport(lv_obj_t *screen, gui_config_t *cfg, lv_obj_t **body_out, lv_obj_t **empty_out, int row_width, lv_event_cb_t scroll_cb) {
	int content_top = settingsrow_content_top(cfg);

	lv_obj_t *list = lv_obj_create(screen);
	lv_obj_set_size(list, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(list, 0, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_radius(list, 0, 0);
	lv_obj_set_style_pad_hor(list, cfg->padding, 0);
	lv_obj_set_style_pad_ver(list, 0, 0);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_add_event_cb(list, scroll_cb, LV_EVENT_SCROLL, NULL);
	// Presses on the rows and on the empty area bubble into the gesture
	// handlers; set here because an empty list binds no row to set it later.
	lv_obj_set_event_bubble(list, true);

	lv_obj_t *body = lv_obj_create(list);
	lv_obj_set_width(body, row_width);
	lv_obj_set_height(body, 1);
	lv_obj_set_pos(body, 0, 0);
	lv_obj_set_style_bg_opa(body, 0, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_pad_all(body, 0, 0);
	lv_obj_set_scrollable(body, false);
	lv_obj_set_event_bubble(body, true);

	lv_obj_t *empty = lv_label_create(list);
	lv_obj_set_width(empty, row_width);
	lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(empty, &font_ui_24, 0);
	lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 90);
	lv_obj_set_hidden(empty, true);

	player_sheet_attach_drag(list, true);
	switcher_attach_back_gesture(list);

	*body_out = body;
	*empty_out = empty;
	return list;
}

// ---------------------------------------------------------------------------
// the books page
//
// The rows are the ones the all-tracks list uses -- same height, same 72 px
// thumbnail, same now-playing mark, same windowed pool of twelve widgets. A
// book carries a cover far more reliably than a loose track does.
//
// The data is windowed with the widgets: the page holds a handle -- four bytes
// a book -- and a band of forty-eight rows read back as the viewport moves.
// ---------------------------------------------------------------------------

#define ROW_HEIGHT 100
#define ROW_GAP 8
#define ROW_PITCH (ROW_HEIGHT + ROW_GAP)
#define ROW_RADIUS 12
#define ROW_PAD 14
#define THUMB_SIZE 72
#define ROW_POOL 12

// How many rows are read from the database at a time. Four times the pool, so
// scrolling a screenful does not go back to the database.
#define WINDOW_ROWS 48

#define PLAYMARK_WIDTH 6
#define PLAYMARK_HEIGHT 52
#define PLAYMARK_INSET 4 // from the row's left edge

#define THUMB_POLL_MS 150

typedef struct {
	char name[256];
	char path[512];
} winrow_t;

typedef struct {
	lv_obj_t *button;
	lv_obj_t *icon;
	lv_obj_t *label;
	lv_obj_t *playmark;
	int index;

	bool has_thumb;
	bool thumb_requested;
	bool thumb_settled;
	cover_image_t thumb;
} row_t;

static lv_obj_t *books_title;
static lv_obj_t *books_sort_icon;
static lv_obj_t *book_list; // the scrollable viewport
static lv_obj_t *book_body; // the fixed-height canvas the rows are placed on
static lv_obj_t *empty_label;
static lv_timer_t *thumb_timer;

static row_t rows[ROW_POOL];

static audiobookdb_index_t *book_index;
static int entry_count;

static winrow_t *window_rows; // WINDOW_ROWS of them, allocated once
static int window_first = -1;
static int window_count;

static int row_width;

// What the books page is showing: the list, and the author or series for the
// two lists that are one of those.
static list_id_t books_list = LIST_LIBRARY;
static char books_value[256];
static char np_book[512]; // the book playing now, for the mark

static int row_slot(const row_t *row) { return COVERLOADER_AUDIOBOOKS_BASE + (int)(row - rows); }

static bool row_at(int index, const char **name_out, const char **path_out);

static audiobook_list_t books_kind(void) {
	switch (books_list) {
	case LIST_FINISHED:
		return AUDIOBOOK_LIST_FINISHED;
	case LIST_CONTINUE:
		return AUDIOBOOK_LIST_CONTINUE;
	case LIST_AUTHOR_BOOKS:
		return AUDIOBOOK_LIST_AUTHOR;
	case LIST_SERIES_BOOKS:
		return AUDIOBOOK_LIST_SERIES;
	default:
		return AUDIOBOOK_LIST_ALL;
	}
}

static audiobookdb_index_t *books_index_open(void) {
	audiobook_order_t order;
	bool desc;
	sort_to_order(list_sort(books_list), &order, &desc);
	return audiobookdb_index_open(books_kind(), books_value, order, desc);
}

static void row_show_glyph(row_t *row) {
	lv_image_set_src(row->icon, &icon_book_headphones_row);
	lv_obj_add_style(row->icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);
}

// A jacket must not be tinted, so the recolour the glyph relies on is switched
// off for as long as a picture is on the row.
static void row_show_cover(row_t *row, const lv_image_dsc_t *dsc) {
	lv_image_set_src(row->icon, dsc);
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_TRANSP, 0);
}

static void row_drop_thumb(row_t *row) {
	coverloader_release(row_slot(row));

	if (row->has_thumb) {
		row_show_glyph(row); // stop pointing at the pixels before they go
		cover_free(&row->thumb);
		row->has_thumb = false;
	}
	row->thumb_requested = false;
	row->thumb_settled = false;
}

static void row_update_playmark(row_t *row) {
	if (!row->playmark) {
		return;
	}
	const char *path = NULL;
	bool playing = np_book[0] && row_at(row->index, NULL, &path) && path[0] && strcmp(path, np_book) == 0;
	if (playing) {
		lv_obj_set_hidden(row->playmark, false);
	} else {
		lv_obj_set_hidden(row->playmark, true);
	}
}

// The mark follows the book, so every file of a folder book lights its row.
static void refresh_playmarks(void) {
	device_state_t state;
	device_state_get(&state);
	np_book[0] = '\0';
	if (!state.live && state.current_file[0]) {
		audiobookdb_book_for_file(state.current_file, np_book, sizeof(np_book));
	}

	for (int i = 0; i < ROW_POOL; i++) {
		row_update_playmark(&rows[i]);
	}
}

static void row_bind(row_t *row, int index) {
	if (row->index == index) {
		return;
	}

	row_drop_thumb(row);
	row->index = index;

	if (index < 0 || index >= entry_count) {
		lv_obj_set_hidden(row->button, true);
		return;
	}

	const char *name = NULL;
	if (!row_at(index, &name, NULL)) {
		// The handle went stale under the list, or the row is gone. Hiding it
		// is what the next window_update() undoes, once the list is rebuilt.
		lv_obj_set_hidden(row->button, true);
		return;
	}

	lv_obj_set_hidden(row->button, false);
	lv_obj_set_y(row->button, index * ROW_PITCH);
	lv_label_set_text(row->label, name);
	row_show_glyph(row);
	row_update_playmark(row);
}

static bool first_part_cb(const char *path, const char *title, void *user) {
	(void)title;
	snprintf(user, 512, "%s", path);
	return false;
}

// Where a book's cover is looked for: its own file, or the first file of a
// folder book -- the picture inside it, else the cover image beside it.
static const char *cover_source(const char *book, char *first, size_t size) {
	if (size < 512 || !audiobookdb_is_folder_book(book)) {
		return book;
	}
	first[0] = '\0';
	audiobookdb_parts_for_each(book, first_part_cb, first);
	return first[0] ? first : book;
}

// Asks for the artwork of the visible rows and collects what the worker has
// finished.
static void thumbs_update(void) {
	bool anything_pending = false;

	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &rows[i];
		if (row->index < 0 || row->index >= entry_count || row->thumb_settled) {
			continue;
		}

		const char *source = NULL;
		if (!row_at(row->index, NULL, &source) || !source[0]) {
			row->thumb_settled = true;
			continue;
		}

		if (!row->thumb_requested) {
			char first[512];
			coverloader_request(row_slot(row), cover_source(source, first, sizeof(first)), THUMB_SIZE);
			row->thumb_requested = true;
		}

		bool finished = false;
		cover_image_t image;
		if (coverloader_take(row_slot(row), &image, &finished)) {
			row->thumb = image;
			row->has_thumb = true;
			row->thumb_settled = true;
			row_show_cover(row, &row->thumb.dsc);
		} else if (finished) {
			row->thumb_settled = true; // no jacket in this one; the book glyph stays
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
	if (lv_screen_active() == books_screen) {
		thumbs_update();
	}
}

static void show_empty(lv_obj_t *label, int count) {
	if (count == 0) {
		lv_obj_set_hidden(label, false);
	} else {
		lv_obj_set_hidden(label, true);
	}
}

// A rescan, or a card change, or a book marked finished: the rows the handle
// names have moved, so the list is rebuilt where it stands.
static void refresh_stale(void) {
	if (!book_index || !audiobookdb_index_stale(book_index)) {
		return;
	}
	audiobookdb_index_t *fresh = books_index_open();
	audiobookdb_index_close(book_index);
	book_index = fresh;
	entry_count = audiobookdb_index_count(fresh);
	window_first = -1;
	window_count = 0;

	for (int i = 0; i < ROW_POOL; i++) {
		row_drop_thumb(&rows[i]);
		rows[i].index = -2; // every row now stands for a different book
	}
	lv_obj_set_height(book_body, entry_count > 0 ? entry_count * ROW_PITCH : 1);
	show_empty(empty_label, entry_count);
}

static void window_update(void) {
	refresh_stale();
	if (entry_count == 0) {
		for (int i = 0; i < ROW_POOL; i++) {
			row_bind(&rows[i], -1);
		}
		return;
	}

	int scroll = lv_obj_get_scroll_y(book_list);
	if (scroll < 0) {
		scroll = 0;
	}

	int first = (scroll / ROW_PITCH) - 1;
	if (first + ROW_POOL > entry_count) {
		first = entry_count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}

	// index -> widget index % ROW_POOL, so a one-step slide of the band
	// rebinds exactly one row instead of renaming all twelve.
	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		row_bind(&rows[index % ROW_POOL], index < entry_count ? index : -1);
	}

	thumbs_update();
}

static void books_scroll_cb(lv_event_t *e) {
	(void)e;
	window_update();
}

static bool window_fill_cb(const char *name, const char *path, void *user) {
	int *filled = user;
	if (*filled >= WINDOW_ROWS) {
		return false;
	}
	winrow_t *row = &window_rows[*filled];
	snprintf(row->name, sizeof(row->name), "%s", name ? name : "");
	snprintf(row->path, sizeof(row->path), "%s", path ? path : "");
	(*filled)++;
	return true;
}

// Brings `index` into the band, reading a windowful around it. The band starts
// a pool's worth before the wanted row, so scrolling back up a little does not
// send it to the database again.
static bool window_cover(int index) {
	if (!book_index || index < 0 || index >= entry_count) {
		return false;
	}
	if (!window_rows) {
		window_rows = calloc(WINDOW_ROWS, sizeof(*window_rows));
		if (!window_rows) {
			return false;
		}
	}
	if (window_first >= 0 && index >= window_first && index < window_first + window_count) {
		return true;
	}

	int first = index - ROW_POOL;
	if (first < 0) {
		first = 0;
	}
	if (first + WINDOW_ROWS > entry_count) {
		first = entry_count - WINDOW_ROWS;
	}
	if (first < 0) {
		first = 0;
	}

	int filled = 0;
	audiobookdb_index_window(book_index, first, WINDOW_ROWS, window_fill_cb, &filled);
	window_first = first;
	window_count = filled;
	return index >= first && index < first + filled;
}

// The name of a book and its path. False when the row is not there -- a stale
// handle, or an index past the end.
static bool row_at(int index, const char **name_out, const char **path_out) {
	if (index < 0 || index >= entry_count || !window_cover(index)) {
		return false;
	}
	const winrow_t *row = &window_rows[index - window_first];
	if (name_out) {
		*name_out = row->name;
	}
	if (path_out) {
		*path_out = row->path;
	}
	return true;
}

static const char *books_empty_text(void) {
	switch (books_list) {
	case LIST_FINISHED:
		return tr("audiobook_finished_empty");
	case LIST_CONTINUE:
		return tr("audiobook_continue_empty");
	case LIST_LIBRARY:
		return tr("audiobook_empty_note");
	default:
		return tr("audiobook_list_empty");
	}
}

static void reload_books(void) {
	// Unbind first, and set the index by hand afterwards: row_bind() is a no-op
	// when the index has not moved, so switching between two lists of the same
	// length would otherwise leave every row showing the old titles.
	for (int i = 0; i < ROW_POOL; i++) {
		row_bind(&rows[i], -1);
		rows[i].index = -1;
	}

	audiobookdb_index_close(book_index);
	book_index = books_index_open();
	entry_count = audiobookdb_index_count(book_index);
	window_first = -1;
	window_count = 0;

	lv_obj_set_height(book_body, entry_count > 0 ? entry_count * ROW_PITCH : 1);
	lv_obj_scroll_to_y(book_list, 0, LV_ANIM_OFF);

	lv_label_set_text(empty_label, books_empty_text());
	show_empty(empty_label, entry_count);
	lv_image_set_src(books_sort_icon, sort_icon(list_sort(books_list)));

	refresh_playmarks();
	window_update();
}

// Opens the books page on one of its lists. `value` is the author or the
// series, `title` what the page is headed with.
static void books_open(list_id_t list, const char *value, const char *title) {
	books_list = list;
	snprintf(books_value, sizeof(books_value), "%s", value ? value : "");
	lv_label_set_text(books_title, title);
	reload_books();
	switch_screen(books_screen);
}

// Plays a book from where it was left: a book in one file from its saved
// second, a folder book as a queue of its parts from the part and second it
// was left at.
static bool collect_part(const char *path, const char *title, void *user) {
	(void)title;
	char ***list = user;
	size_t n = 0;
	while ((*list) && (*list)[n]) {
		n++;
	}
	char **grown = realloc(*list, (n + 2) * sizeof(char *));
	if (!grown) {
		return false;
	}
	*list = grown;
	grown[n] = strdup(path);
	grown[n + 1] = NULL;
	return grown[n] != NULL;
}

// Starts `book` in `file` at `seconds`: a single-file book directly, a folder
// book as a queue of its parts. A file that is no longer one of the book's
// parts starts the book from its first, from the beginning.
static void play_from(const char *book, const char *file, double seconds) {
	if (!audiobookdb_is_folder_book(book)) {
		if (seconds > 1.0) {
			device_state_play_file_at(file, seconds);
		} else {
			device_state_play_file(file);
		}
		return;
	}

	char **list = NULL;
	audiobookdb_parts_for_each(book, collect_part, &list);
	int count = 0;
	int start = -1;
	while (list && list[count]) {
		if (start < 0 && strcmp(list[count], file) == 0) {
			start = count;
		}
		count++;
	}
	if (count > 0) {
		double at = start >= 0 && seconds > 1.0 ? seconds : -1.0;
		device_state_play_list_ordered_at((const char *const *)list, count, start >= 0 ? start : 0, at);
	}
	for (int i = 0; i < count; i++) {
		free(list[i]);
	}
	free(list);
}

static void play_book(const char *book) {
	char file[512];
	double resume = 0;
	if (!audiobook_resume_point(book, file, sizeof(file), &resume)) {
		return;
	}
	play_from(book, file, resume);

	audiobookdb_touch(book); // the order by last listened moves it to the top
	player_refresh_now_playing();
	player_sheet_open(true);
}

void audiobooks_play_at(const char *book, const char *file, double seconds) {
	if (!book || !book[0] || !file || !file[0]) {
		return;
	}
	play_from(book, file, seconds);
	// Written through: this is where the book now stands, whatever comes next.
	audiobookdb_save_position(book, file, seconds);
	audiobookdb_touch(book);
	player_refresh_now_playing();
}

static void book_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return; // a swipe across the list, not a tap
	}

	lv_obj_t *button = lv_event_get_current_target(e);
	int index = -1;
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].button == button) {
			index = rows[i].index;
			break;
		}
	}
	const char *found = NULL;
	if (!row_at(index, NULL, &found) || !found[0]) {
		return;
	}
	char book[512];
	snprintf(book, sizeof(book), "%s", found);
	play_book(book);
}

static void books_sort_cb(lv_event_t *e) { sort_menu_show(lv_event_get_current_target(e), books_list, reload_books); }

static void books_loaded_cb(lv_event_t *e) {
	(void)e;
	// A scan, or a book reaching its end, may have happened since.
	refresh_stale();
	refresh_playmarks();
	window_update();
}

static void build_books_page(gui_config_t *cfg) {
	lv_obj_add_style(books_screen, &theme_style_screen, 0);
	books_title = settingsrow_title(books_screen, cfg, "audiobooks");
	settingsrow_title_corner_slots(books_title, cfg, 1);
	lv_obj_t *sort_btn = corner_button(books_screen, cfg, 0, &icon_sort_az, books_sort_cb);
	books_sort_icon = lv_obj_get_child(sort_btn, 0);

	row_width = cfg->screen_width - 2 * cfg->padding;
	book_list = make_viewport(books_screen, cfg, &book_body, &empty_label, row_width, books_scroll_cb);

	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &rows[i];

		row->button = lv_btn_create(book_body);
		lv_obj_set_size(row->button, row_width, ROW_HEIGHT);
		lv_obj_set_x(row->button, 0);
		lv_obj_add_style(row->button, &theme_style_card, 0);
		lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row->button, ROW_RADIUS, 0);
		lv_obj_set_style_border_width(row->button, 0, 0);
		lv_obj_set_style_shadow_width(row->button, 0, 0);
		lv_obj_set_style_pad_all(row->button, ROW_PAD, 0);
		lv_obj_set_style_pad_column(row->button, ROW_PAD, 0);
		lv_obj_set_hidden(row->button, true);
		lv_obj_set_event_bubble(row->button, true); // so the player sheet can be dragged in
		lv_obj_add_event_cb(row->button, book_clicked_cb, LV_EVENT_CLICKED, NULL);
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
		// cut the name there, with the ellipsis. A cap and not a height, so a
		// one-line name stays centred.
		lv_obj_set_style_max_height(row->label, 2 * lv_font_get_line_height(&font_ui_24) + lv_obj_get_style_text_line_space(row->label, LV_PART_MAIN), 0);

		// Out of the flex layout: it sits in the row's own left padding.
		row->playmark = lv_obj_create(row->button);
		lv_obj_set_ignore_layout(row->playmark, true);
		lv_obj_set_size(row->playmark, PLAYMARK_WIDTH, PLAYMARK_HEIGHT);
		lv_obj_align(row->playmark, LV_ALIGN_LEFT_MID, PLAYMARK_INSET - ROW_PAD, 0);
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

	thumb_timer = lv_timer_create(thumb_timer_cb, THUMB_POLL_MS, NULL);
	lv_timer_pause(thumb_timer);

	player_sheet_attach_drag(books_screen, true);
	switcher_attach_back_gesture(books_screen);
	lv_obj_add_event_cb(books_screen, books_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// the authors and series page
//
// The same windowed pool, with a name and a count of books on each row. The
// names are few enough to hold -- one per author or series, not per book.
// ---------------------------------------------------------------------------

#define NAME_ROW_HEIGHT 88
#define NAME_ROW_PITCH (NAME_ROW_HEIGHT + ROW_GAP)

typedef struct {
	lv_obj_t *button;
	lv_obj_t *label;
	lv_obj_t *count;
	int index;
} name_row_t;

typedef struct {
	char *name;
	int books;
} name_entry_t;

static lv_obj_t *names_title;
static lv_obj_t *names_sort_icon;
static lv_obj_t *names_list;
static lv_obj_t *names_body;
static lv_obj_t *names_empty;
static name_row_t name_rows[ROW_POOL];
static name_entry_t *names;
static int name_count;
static int name_capacity;
static list_id_t names_kind = LIST_AUTHORS;

static void names_clear(void) {
	for (int i = 0; i < name_count; i++) {
		free(names[i].name);
	}
	name_count = 0;
}

static bool names_add_cb(const char *name, int books, void *user) {
	(void)user;
	if (name_count == name_capacity) {
		int grown = name_capacity ? name_capacity * 2 : 64;
		name_entry_t *bigger = realloc(names, (size_t)grown * sizeof(*bigger));
		if (!bigger) {
			return false;
		}
		names = bigger;
		name_capacity = grown;
	}
	names[name_count].name = strdup(name);
	if (!names[name_count].name) {
		return false;
	}
	names[name_count].books = books;
	name_count++;
	return true;
}

// The name a row shows: the books with no author are one row of their own.
static const char *shown_name(const char *name) { return name[0] ? name : tr("audiobook_unknown_author"); }

static void name_row_bind(name_row_t *row, int index) {
	if (row->index == index) {
		return;
	}
	row->index = index;
	if (index < 0 || index >= name_count) {
		lv_obj_set_hidden(row->button, true);
		return;
	}
	lv_obj_set_hidden(row->button, false);
	lv_obj_set_y(row->button, index * NAME_ROW_PITCH);
	lv_label_set_text(row->label, shown_name(names[index].name));
	lv_label_set_text_fmt(row->count, "%d", names[index].books);
}

static void names_window_update(void) {
	int scroll = lv_obj_get_scroll_y(names_list);
	if (scroll < 0) {
		scroll = 0;
	}
	int first = (scroll / NAME_ROW_PITCH) - 1;
	if (first + ROW_POOL > name_count) {
		first = name_count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}
	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		name_row_bind(&name_rows[index % ROW_POOL], index < name_count ? index : -1);
	}
}

static void names_scroll_cb(lv_event_t *e) {
	(void)e;
	names_window_update();
}

static void reload_names(void) {
	for (int i = 0; i < ROW_POOL; i++) {
		name_row_bind(&name_rows[i], -1);
		name_rows[i].index = -1;
	}
	names_clear();

	sort_choice_t choice = list_sort(names_kind);
	bool by_added = choice == SORT_NEW || choice == SORT_OLD;
	bool desc = choice == SORT_ZA || choice == SORT_NEW;
	audiobookdb_names_for_each(names_kind == LIST_SERIES ? AUDIOBOOK_NAMES_SERIES : AUDIOBOOK_NAMES_AUTHORS, by_added, desc, names_add_cb, NULL);

	lv_obj_set_height(names_body, name_count > 0 ? name_count * NAME_ROW_PITCH : 1);
	lv_obj_scroll_to_y(names_list, 0, LV_ANIM_OFF);
	lv_label_set_text(names_empty, tr(names_kind == LIST_SERIES ? "audiobook_series_empty" : "audiobook_empty_note"));
	show_empty(names_empty, name_count);
	lv_image_set_src(names_sort_icon, sort_icon(choice));
	names_window_update();
}

static void names_open(list_id_t kind) {
	names_kind = kind;
	lv_label_set_text(names_title, tr(kind == LIST_SERIES ? "audiobook_series" : "audiobook_authors"));
	reload_names();
	switch_screen(names_screen);
}

static void name_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	lv_obj_t *button = lv_event_get_current_target(e);
	for (int i = 0; i < ROW_POOL; i++) {
		if (name_rows[i].button != button) {
			continue;
		}
		int index = name_rows[i].index;
		if (index < 0 || index >= name_count) {
			return;
		}
		char value[256];
		snprintf(value, sizeof(value), "%s", names[index].name);
		books_open(names_kind == LIST_SERIES ? LIST_SERIES_BOOKS : LIST_AUTHOR_BOOKS, value, shown_name(value));
		return;
	}
}

static void names_sort_cb(lv_event_t *e) { sort_menu_show(lv_event_get_current_target(e), names_kind, reload_names); }

static void names_loaded_cb(lv_event_t *e) {
	(void)e;
	names_window_update();
}

static void build_names_page(gui_config_t *cfg) {
	lv_obj_add_style(names_screen, &theme_style_screen, 0);
	names_title = settingsrow_title(names_screen, cfg, "audiobook_authors");
	settingsrow_title_corner_slots(names_title, cfg, 1);
	lv_obj_t *sort_btn = corner_button(names_screen, cfg, 0, &icon_sort_az, names_sort_cb);
	names_sort_icon = lv_obj_get_child(sort_btn, 0);

	int width = cfg->screen_width - 2 * cfg->padding;
	names_list = make_viewport(names_screen, cfg, &names_body, &names_empty, width, names_scroll_cb);

	for (int i = 0; i < ROW_POOL; i++) {
		name_row_t *row = &name_rows[i];
		row->button = lv_btn_create(names_body);
		lv_obj_set_size(row->button, width, NAME_ROW_HEIGHT);
		lv_obj_add_style(row->button, &theme_style_card, 0);
		lv_obj_add_style(row->button, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row->button, ROW_RADIUS, 0);
		lv_obj_set_style_border_width(row->button, 0, 0);
		lv_obj_set_style_shadow_width(row->button, 0, 0);
		lv_obj_set_style_pad_hor(row->button, 20, 0);
		lv_obj_set_style_pad_column(row->button, 12, 0);
		lv_obj_set_hidden(row->button, true);
		lv_obj_set_event_bubble(row->button, true);
		lv_obj_add_event_cb(row->button, name_clicked_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		row->label = lv_label_create(row->button);
		lv_label_set_long_mode(row->label, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(row->label, 1);
		lv_obj_set_height(row->label, lv_font_get_line_height(&font_ui_24));
		lv_obj_add_style(row->label, &theme_style_text, 0);
		lv_obj_set_style_text_font(row->label, &font_ui_24, 0);

		row->count = lv_label_create(row->button);
		lv_obj_add_style(row->count, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(row->count, &font_ui_22, 0);

		row->index = -1;
	}

	player_sheet_attach_drag(names_screen, true);
	switcher_attach_back_gesture(names_screen);
	lv_obj_add_event_cb(names_screen, names_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// the section page: four tiles, the bookmarks, the finished books and the
// options
// ---------------------------------------------------------------------------

static void open_library(void) { books_open(LIST_LIBRARY, NULL, tr("audiobook_library")); }
static void open_series(void) { names_open(LIST_SERIES); }
static void open_authors(void) { names_open(LIST_AUTHORS); }
static void open_continue(void) { books_open(LIST_CONTINUE, NULL, tr("audiobook_continue")); }

static void finished_cb(lv_event_t *e) {
	(void)e;
	books_open(LIST_FINISHED, NULL, tr("audiobook_finished"));
}

static void bookmarks_cb(lv_event_t *e) {
	(void)e;
	audiobookextras_open_bookmarks();
}

// An index written by an older scan has no authors, series or folder books.
// It is read again once, the first time the section is opened, through the
// usual scan page.
static bool upgrade_offered;

// With no books indexed the page says a scan is needed instead of its tiles.
static lv_obj_t *section_grid;
static lv_obj_t *section_empty;

static void section_scan_cb(lv_event_t *e) {
	(void)e;
	start_scan(NULL);
}

static void section_loaded_cb(lv_event_t *e) {
	(void)e;
	gridpage_show_empty(section_grid, section_empty, !audiobookdb_scan_running() && audiobookdb_count() == 0);
	if (!upgrade_offered && audiobookdb_needs_rescan()) {
		upgrade_offered = true;
		// After this page's own load has finished, not inside it.
		lv_async_call(start_scan, NULL);
	}
}

static void build_section_page(gui_config_t *cfg) {
	const grid_entry_t entries[] = {
		{"audiobook_library", &icon_menu_audiobook_library, NULL, open_library},
		{"audiobook_series", &icon_menu_audiobook_series, NULL, open_series},
		{"audiobook_authors", &icon_menu_audiobook_author, NULL, open_authors},
		{"audiobook_continue", &icon_menu_audiobook_continue, NULL, open_continue},
	};
	// The Music page's grid of two by three, so the tiles are the same size.
	section_grid =
		gridpage_build(audiobooks_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, true);
	section_empty =
		gridpage_empty_panel(audiobooks_screen, cfg, &icon_book_headphones, "audiobook_no_database", section_scan_cb);

	// The options, to their left the finished books, and to the left of those
	// the bookmarks -- the same glyph the ebook shelf opens its bookmarks with.
	settingsrow_title_corner_slots(settingsrow_title(audiobooks_screen, cfg, "audiobooks"), cfg, 3);
	lv_obj_t *options_btn = corner_button(audiobooks_screen, cfg, 0, &icon_music_settings, NULL);
	lv_obj_add_event_cb(options_btn, switch_screen_cb, LV_EVENT_CLICKED, audiobooksettings_screen);
	corner_button(audiobooks_screen, cfg, 1, &icon_book_finished, finished_cb);
	corner_button(audiobooks_screen, cfg, 2, &icon_bookmark, bookmarks_cb);

	lv_obj_add_event_cb(audiobooks_screen, section_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// the options page
// ---------------------------------------------------------------------------

static void scan_row_cb(lv_event_t *e);
// A dim line under a row, explaining what the row does not say by itself.
static void option_note(lv_obj_t *parent, const char *text) {
	lv_obj_t *note = lv_label_create(parent);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr(text));
}

static void paint_choice(lv_obj_t *btn, bool on);

// ---------------------------------------------------------------------------
// A toggle whose choices live on pills underneath it, and disappear with it.
//
// The same idea as automatic screen-off on the Display page -- options that
// only mean something while the switch is on should not be sitting there while
// it is off -- but with pills instead of a slider, because "end of chapter" is
// not a point on a scale of minutes.
//
// The card is LV_SIZE_CONTENT rather than two fixed heights, so hiding the row
// collapses it by exactly the right amount and five pills may wrap onto a
// second line without anything having to be measured.
// ---------------------------------------------------------------------------

typedef struct {
	lv_obj_t *card;
	lv_obj_t *toggle;
	lv_obj_t *pills;
} toggle_pills_t;

static void toggle_pills_expanded(const toggle_pills_t *tp, bool expanded) {
	if (!tp || !tp->pills) {
		return;
	}
	if (expanded) {
		lv_obj_set_hidden(tp->pills, false);
	} else {
		lv_obj_set_hidden(tp->pills, true);
	}
}

static void build_toggle_pills(lv_obj_t *parent, const char *title, lv_event_cb_t toggle_cb, toggle_pills_t *out) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_row(card, 18, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_event_bubble(card, true);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	// The name and the switch share the top line.
	lv_obj_t *head = lv_obj_create(card);
	lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(head, 0, 0);
	lv_obj_set_style_border_width(head, 0, 0);
	lv_obj_set_style_pad_all(head, 0, 0);
	lv_obj_set_scrollable(head, false);
	lv_obj_set_event_bubble(head, true);
	lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *name = lv_label_create(head);
	lv_label_set_text(name, tr(title));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	lv_obj_t *toggle = settingsrow_switch(head, toggle_cb);

	lv_obj_t *pills = lv_obj_create(card);
	lv_obj_set_size(pills, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(pills, 0, 0);
	lv_obj_set_style_border_width(pills, 0, 0);
	lv_obj_set_style_pad_all(pills, 0, 0);
	lv_obj_set_style_pad_gap(pills, 10, 0);
	lv_obj_set_scrollable(pills, false);
	lv_obj_set_event_bubble(pills, true);
	// Wrapping, because the four minute pills plus "end of chapter" are more
	// than one line of this screen.
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	out->card = card;
	out->toggle = toggle;
	out->pills = pills;
}

static lv_obj_t *make_pill(lv_obj_t *parent, const char *text, int value, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 56);
	lv_obj_set_style_pad_hor(btn, 18, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // Adwaita pill button
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)value);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_center(label);

	return btn;
}

// --- the three options ---

#define REWIND_CHOICES 4

static lv_obj_t *stop_chapter_switch;
static lv_obj_t *duration_total_pill;
static lv_obj_t *duration_chapter_pill;
static settingsrow_duration_t sleep_row;
static toggle_pills_t rewind_card;
static lv_obj_t *rewind_pill[REWIND_CHOICES];
static const int REWIND_VALUES[REWIND_CHOICES] = {3, 5, 7, 10};

static void refresh_settings_page(void) {
	if (!stop_chapter_switch) {
		return;
	}

	bool per_chapter = audiobook_duration_per_chapter();
	settingsrow_pill_active(duration_total_pill, !per_chapter);
	settingsrow_pill_active(duration_chapter_pill, per_chapter);

	if (audiobook_stop_at_chapter_end()) {
		lv_obj_add_state(stop_chapter_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(stop_chapter_switch, LV_STATE_CHECKED);
	}

	// The control centre switches this timer too, so the switch and the
	// wheels are read again rather than trusted from the last visit.
	settingsrow_duration_set_minutes(&sleep_row, sleeptimer_minutes(SLEEPTIMER_AUDIOBOOK));
	settingsrow_duration_expanded(&sleep_row, sleeptimer_enabled(SLEEPTIMER_AUDIOBOOK));
	settingsrow_duration_repaint(&sleep_row);

	bool rewind_on = audiobook_rewind_enabled();
	if (rewind_on) {
		lv_obj_add_state(rewind_card.toggle, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(rewind_card.toggle, LV_STATE_CHECKED);
	}
	toggle_pills_expanded(&rewind_card, rewind_on);

	int seconds = audiobook_rewind_seconds();
	for (int i = 0; i < REWIND_CHOICES; i++) {
		paint_choice(rewind_pill[i], REWIND_VALUES[i] == seconds);
	}
}

static void duration_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	audiobook_set_duration_per_chapter(lv_event_get_user_data(e) != NULL);
	config_save();
	refresh_settings_page();
}

static void stop_chapter_cb(lv_event_t *e) {
	audiobook_set_stop_at_chapter_end(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void sleep_toggle_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_enabled(SLEEPTIMER_AUDIOBOOK, lv_obj_has_state(sleep_row.toggle, LV_STATE_CHECKED));
	refresh_settings_page();
}

static void sleep_wheel_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_minutes(SLEEPTIMER_AUDIOBOOK, settingsrow_duration_minutes(&sleep_row));
}

static void rewind_toggle_cb(lv_event_t *e) {
	audiobook_set_rewind_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	refresh_settings_page();
}

static void rewind_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	audiobook_set_rewind_seconds((int)(intptr_t)lv_event_get_user_data(e));
	refresh_settings_page();
}

static void settings_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_settings_page();
}

static void build_settings_page(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(audiobooksettings_screen, cfg, "audiobooks");
	settingsrow_add(container, "change_controls", NULL, switch_screen_cb, audiobookcontrols_screen);

	// What the progress bar on the player stands for: the whole book, or the
	// chapter being listened to. Only a book with chapter marks is affected.
	lv_obj_t *duration_pills;
	settingsrow_pills(container, "audiobook_show_duration", &duration_pills);
	duration_total_pill = settingsrow_pill(duration_pills, "audiobook_duration_total", 0, duration_pick_cb);
	duration_chapter_pill = settingsrow_pill(duration_pills, "audiobook_duration_chapter", 1, duration_pick_cb);

	settingsrow_toggle(container, "audiobook_stop_at_end_of_chapter", &stop_chapter_switch, stop_chapter_cb);

	// The same wheels as the music page. "End of chapter" is not among the
	// choices here -- it is not a length, and the row above is the switch for
	// it.
	settingsrow_toggle_duration(container, "sleep_timer", sleep_toggle_cb, sleep_wheel_cb, &sleep_row);
	settingsrow_duration_set_minutes(&sleep_row, sleeptimer_minutes(SLEEPTIMER_AUDIOBOOK));

	build_toggle_pills(container, "audiobook_rewind_after_pause", rewind_toggle_cb, &rewind_card);
	static const char *const REWIND_LABELS[REWIND_CHOICES] = {"audiobook_3_sec", "audiobook_5_sec", "audiobook_7_sec", "audiobook_10_sec"};
	for (int i = 0; i < REWIND_CHOICES; i++) {
		rewind_pill[i] = make_pill(rewind_card.pills, REWIND_LABELS[i], REWIND_VALUES[i], rewind_pick_cb);
	}

	// An action, not a page: it asks before it starts, so no chevron.
	settingsrow_action(container, "audiobook_scan_audiobooks_2", scan_row_cb, NULL);
	// Said on the row itself: a scan that finds nothing because the books are
	// somewhere else looks like a scan that is broken.
	option_note(container, "audiobook_folder_note");

	refresh_settings_page();
	theme_register_refresh(refresh_settings_page);
	lv_obj_add_event_cb(audiobooksettings_screen, settings_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	// And when the control centre switches the timer with the page under it.
	lv_obj_add_event_cb(audiobooksettings_screen, settings_loaded_cb, LV_EVENT_REFRESH, NULL);
}

// ---------------------------------------------------------------------------
// Change controls: how far each of the two transport buttons jumps
//
// Two settings, not one, and that is the point of the page: going back thirty
// seconds to catch a sentence again and going forward ten to step over a pause
// are different sized moves, and there is no reason the two buttons should
// have to agree. -10 with +30 is a perfectly sensible pair.
// ---------------------------------------------------------------------------

#define SKIP_CHOICES 3

static lv_obj_t *back_choice[SKIP_CHOICES];
static lv_obj_t *forward_choice[SKIP_CHOICES];
static const int SKIP_VALUES[SKIP_CHOICES] = {AUDIOBOOK_SKIP_SHORT, AUDIOBOOK_SKIP_LONG, AUDIOBOOK_SKIP_HUGE};

static void paint_choice(lv_obj_t *btn, bool on) {
	if (!btn) {
		return;
	}
	lv_obj_set_style_bg_color(btn, on ? theme()->accent : theme()->surface_pressed, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(btn, 0), on ? lv_color_white() : theme()->text_primary, 0);
}

static void refresh_control_buttons(void) {
	int back = audiobook_skip_back();
	int forward = audiobook_skip_forward();

	for (int i = 0; i < SKIP_CHOICES; i++) {
		paint_choice(back_choice[i], SKIP_VALUES[i] == back);
		paint_choice(forward_choice[i], SKIP_VALUES[i] == forward);
	}
}

static void pick_back_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	audiobook_set_skip_back((int)(intptr_t)lv_event_get_user_data(e));
	refresh_control_buttons();
	player_refresh_now_playing(); // the button it names changes glyph
}

static void pick_forward_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	audiobook_set_skip_forward((int)(intptr_t)lv_event_get_user_data(e));
	refresh_control_buttons();
	player_refresh_now_playing();
}

static lv_obj_t *make_skip_choice(lv_obj_t *parent, const char *text, int seconds, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 64);
	// Three across the card instead of two, so tighter than the Appearance
	// page's pair: enough that -60 does not fall off the right edge.
	lv_obj_set_style_pad_hor(btn, 22, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0); // Adwaita pill button
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)seconds);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_center(label);

	return btn;
}

// One card per button: its name, then its pills. The same shape as the
// Appearance page's theme card, because it is the same kind of choice.
static lv_obj_t *make_control_card(lv_obj_t *parent, const char *title) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_gap(card, 18, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_event_bubble(card, true);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *name = lv_label_create(card);
	lv_label_set_text(name, tr(title));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	lv_obj_t *row = lv_obj_create(card);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_gap(row, 14, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	return row;
}

static void controls_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_control_buttons();
}

static void build_controls_page(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(audiobookcontrols_screen, cfg, "change_controls");

	lv_obj_t *back_row = make_control_card(container, "back_button");
	back_choice[0] = make_skip_choice(back_row, "-10", AUDIOBOOK_SKIP_SHORT, pick_back_cb);
	back_choice[1] = make_skip_choice(back_row, "-30", AUDIOBOOK_SKIP_LONG, pick_back_cb);
	back_choice[2] = make_skip_choice(back_row, "-60", AUDIOBOOK_SKIP_HUGE, pick_back_cb);

	lv_obj_t *forward_row = make_control_card(container, "forward_button");
	forward_choice[0] = make_skip_choice(forward_row, "+10", AUDIOBOOK_SKIP_SHORT, pick_forward_cb);
	forward_choice[1] = make_skip_choice(forward_row, "+30", AUDIOBOOK_SKIP_LONG, pick_forward_cb);
	forward_choice[2] = make_skip_choice(forward_row, "+60", AUDIOBOOK_SKIP_HUGE, pick_forward_cb);

	refresh_control_buttons();
	theme_register_refresh(refresh_control_buttons);
	lv_obj_add_event_cb(audiobookcontrols_screen, controls_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// the scan page: the music scan's layout with its own icon and words
// ---------------------------------------------------------------------------

#define SCAN_POLL_MS 200

static lv_obj_t *scan_count_label;
static lv_obj_t *scan_status_label;
static lv_obj_t *scan_ok_button;
static lv_obj_t *scan_cancel_button;
static lv_timer_t *scan_poll_timer;

static char scan_root[512];

static void scan_show_finished(int found) {
	lv_label_set_text_fmt(scan_count_label, "%d", found);
	lv_label_set_text(scan_status_label, found == 1 ? tr("audiobook_found") : tr("audiobook_found_count"));

	lv_obj_set_hidden(scan_cancel_button, true);
	lv_obj_set_hidden(scan_ok_button, false);
	power_hold_screen_on(false);
}

static void scan_poll_cb(lv_timer_t *timer) {
	int found = audiobookdb_scan_found();
	lv_label_set_text_fmt(scan_count_label, "%d", found);

	if (audiobookdb_scan_running()) {
		return;
	}

	lv_timer_pause(timer);
	scan_show_finished(found);
}

static void scan_ok_cb(lv_event_t *e) {
	(void)e;
	switch_screen(audiobooks_screen);
	// The way here was menu -> Audiobooks -> options -> scan, and walking back
	// through a finished scan would only confuse. From the list, back now means
	// the menu.
	screen_history_reset();
}

static void scan_cancel_cb(lv_event_t *e) {
	(void)e;
	audiobookdb_scan_stop();
	lv_timer_pause(scan_poll_timer);
	power_hold_screen_on(false);
	switch_screen(audiobooks_screen);
	screen_history_reset();
}

static void scan_begin(void) {
	lv_label_set_text(scan_count_label, "0");
	lv_label_set_text(scan_status_label, tr("audiobook_found_count"));
	lv_obj_set_hidden(scan_ok_button, true);
	lv_obj_set_hidden(scan_cancel_button, false);

	if (!audiobookdb_scan_start(scan_root)) {
		lv_label_set_text(scan_status_label, tr("no_card_to_scan"));
		lv_obj_set_hidden(scan_cancel_button, true);
		lv_obj_set_hidden(scan_ok_button, false);
		return;
	}

	power_hold_screen_on(true);
	lv_timer_reset(scan_poll_timer);
	lv_timer_resume(scan_poll_timer);
}

// Same contract as the music scan: minutes of work, so it asks first.
static void start_scan(void *user) {
	(void)user;
	switch_screen(audiobookscan_screen);
	scan_begin();
}

static void scan_row_cb(lv_event_t *e) {
	(void)e;
	confirm_show("audiobook_scan_audiobooks",
				 "audiobook_rescan_confirm_note",
				 "scan", start_scan, NULL);
}

static lv_obj_t *scan_make_button(const char *text, lv_color_t colour, lv_event_cb_t cb, gui_config_t *cfg) {
	lv_obj_t *button = lv_btn_create(audiobookscan_screen);
	lv_obj_set_size(button, 240, 68);
	lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -(cfg->padding * 2));
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(button, colour, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_set_style_text_color(label, lv_color_white(), 0);
	lv_obj_center(label);

	return button;
}

static void build_scan_page(gui_config_t *cfg) {
	// The folder and not the whole card: see the note in audiobookdb.c. Built
	// once here so the scan and the line under the button cannot disagree about
	// where books live.
	snprintf(scan_root, sizeof(scan_root), "%s/%s", cfg->sd_root_path ? cfg->sd_root_path : "", AUDIOBOOKDB_FOLDER);

	lv_obj_add_style(audiobookscan_screen, &theme_style_screen, 0);

	lv_obj_t *title = settingsrow_title(audiobookscan_screen, cfg, "scan_2");
	lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

	int content_top = settingsrow_content_top(cfg);

	lv_obj_t *container = lv_obj_create(audiobookscan_screen);
	lv_obj_set_size(container, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(container, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(container, 0, 0);
	lv_obj_set_style_border_width(container, 0, 0);
	lv_obj_set_style_radius(container, 0, 0);
	lv_obj_set_style_pad_all(container, cfg->padding, 0);
	lv_obj_set_style_pad_gap(container, 10, 0);
	lv_obj_set_scrollable(container, false);
	lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *book = lv_image_create(container);
	lv_image_set_src(book, &icon_book_headphones);
	lv_obj_add_style(book, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(book, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_bottom(book, 16, 0);

	scan_count_label = lv_label_create(container);
	lv_label_set_text(scan_count_label, "0");
	lv_obj_set_style_text_color(scan_count_label, theme()->accent, 0);
	lv_obj_set_style_text_font(scan_count_label, &font_ui_32, 0);

	scan_status_label = lv_label_create(container);
	lv_label_set_text(scan_status_label, tr("audiobook_found_count"));
	lv_obj_add_style(scan_status_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(scan_status_label, &font_ui_26, 0);

	scan_cancel_button = scan_make_button("cancel", lv_color_make(210, 66, 58), scan_cancel_cb, cfg);
	scan_ok_button = scan_make_button("ok", theme()->accent, scan_ok_cb, cfg);
	lv_obj_set_hidden(scan_ok_button, true);

	scan_poll_timer = lv_timer_create(scan_poll_cb, SCAN_POLL_MS, NULL);
	lv_timer_pause(scan_poll_timer);
}

void audiobooks_init(gui_config_t *cfg) {
	books_screen = lv_obj_create(NULL);
	names_screen = lv_obj_create(NULL);
	build_section_page(cfg);
	build_books_page(cfg);
	build_names_page(cfg);
	build_settings_page(cfg);
	build_controls_page(cfg);
	build_scan_page(cfg);
}
