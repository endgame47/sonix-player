#include "search.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/library/medialist.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/library/playlists.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"

lv_obj_t *search_screen;

// ---------------------------------------------------------------------------
// The search page
//
// The query is typed in full and run by the keyboard's search key. Its hits
// come back as three lists -- tracks, albums, artists -- each one whole, with
// a pill per list to choose which one is on screen.
//
// A list holds row ids, not rows (library_index_t): a query that matches most
// of the card costs four bytes a hit. What is drawn is a pool of rows that
// follows the scroll, bound to the hits under it, the way the library lists
// work.
// ---------------------------------------------------------------------------

#define SEARCH_THUMB 56
#define SEARCH_ROW_H 76
#define SEARCH_ROW_GAP 8
#define SEARCH_PITCH (SEARCH_ROW_H + SEARCH_ROW_GAP)
#define SEARCH_THUMB_POLL_MS 200
#define SEARCH_PILLS_H 56
#define SEARCH_PILLS_GAP 10

// Rows on screen at once on the tallest panel, plus one beyond each edge. One
// coverloader slot each, out of the page's COVERLOADER_SEARCH_COUNT.
#define SEARCH_POOL 12

typedef enum {
	CAT_TRACKS = 0,
	CAT_ALBUMS,
	CAT_ARTISTS,
	CAT_COUNT,
} category_t;

static const library_list_t CAT_KIND[CAT_COUNT] = {LIBRARY_LIST_TRACKS, LIBRARY_LIST_ALBUMS, LIBRARY_LIST_ARTISTS};
static const char *const CAT_LABEL[CAT_COUNT] = {"search_tracks_2", "albums", "artists"};

static lv_obj_t *field;		// the textarea the keyboard types into
static lv_obj_t *clear_btn; // the x that empties it
static keyboard_t *keyboard;
static lv_obj_t *pills;		// the three category pills
static lv_obj_t *pill[CAT_COUNT];
static lv_obj_t *pill_name[CAT_COUNT];
static lv_obj_t *pill_count[CAT_COUNT];
static lv_obj_t *results;	  // the scrolling list
static lv_obj_t *body;		  // inside it, as tall as every hit of the category
static lv_obj_t *empty_label; // "no results"
static void search_keyboard_show(bool visible);
static int results_top; // where the scrolling area starts
static int screen_h;
static int keyboard_height;

// The query the results answer, and the hits of each category. NULL handles
// while no search stands.
static char searched[256];
static library_index_t *cat_ix[CAT_COUNT];
static int cat_count[CAT_COUNT];
static int cat_scroll[CAT_COUNT];
static category_t current_cat = CAT_TRACKS;

typedef struct {
	lv_obj_t *button;
	lv_obj_t *icon;
	lv_obj_t *title;
	lv_obj_t *subtitle;
	lv_obj_t *chevron;
	int index; // the hit bound, -1 for none
	char name[256]; // a track's title, an album's value, an artist
	char path[512]; // a track's file, an album's first track
	// The artwork, through slot COVERLOADER_SEARCH_BASE + the row's place in
	// the pool.
	const lv_image_dsc_t *glyph; // what the row shows without a picture
	bool requested;
	bool settled; // nothing more to wait for (arrived, or there is none)
	bool has_image;
	cover_image_t image;
} row_t;

static row_t rows[SEARCH_POOL];
static lv_timer_t *thumb_timer;

// ---------------------------------------------------------------------------
// Artwork
// ---------------------------------------------------------------------------

static int row_slot(const row_t *row) { return COVERLOADER_SEARCH_BASE + (int)(row - rows); }

// Lets go of the row's picture and of any decode still pending for it, and
// puts its glyph back.
static void row_drop_art(row_t *row) {
	coverloader_release(row_slot(row));
	if (row->has_image) {
		if (row->icon) {
			lv_image_set_src(row->icon, row->glyph ? row->glyph : &icon_music2);
			lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);
		}
		cover_free(&row->image);
	}
	row->has_image = false;
	row->requested = false;
	row->settled = false;
}

static void art_drop_pictures(void) {
	for (int i = 0; i < SEARCH_POOL; i++) {
		row_drop_art(&rows[i]);
	}
	if (thumb_timer) {
		lv_timer_pause(thumb_timer);
	}
}

// Same wait as the streaming pages, and for the same reason: tapping a hit goes
// to the player, and coming straight back must not cost a dozen decodes off the
// card.
#define ART_DROP_DELAY_MS 20000

static lv_timer_t *art_drop_timer;

static void art_drop_cb(lv_timer_t *timer) {
	lv_timer_pause(timer);
	art_drop_pictures();

	// And back to the kernel, not merely back to the arena. A thumbnail is
	// 7200 bytes, well under the 96 kB mmap threshold main.c sets, so the
	// thumbnails come out of the heap rather than out of their own mappings
	// and freeing them alone leaves the arena holding the space.
	malloc_trim(0);
}

// Hands each bound row's artwork request to the loader and collects the
// results as they arrive.
static void thumbs_update(void) {
	bool pending = false;

	for (int i = 0; i < SEARCH_POOL; i++) {
		row_t *row = &rows[i];
		if (row->index < 0 || row->settled || !row->path[0] || current_cat == CAT_ARTISTS) {
			continue;
		}

		int slot = row_slot(row);
		if (!row->requested) {
			coverloader_request(slot, row->path, SEARCH_THUMB);
			row->requested = true;
		}

		bool finished = false;
		cover_image_t image;
		if (coverloader_take(slot, &image, &finished)) {
			row->image = image;
			row->has_image = true;
			row->settled = true;
			// Album art must not be tinted the way the glyphs are.
			lv_image_set_src(row->icon, &row->image.dsc);
			lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_TRANSP, 0);
		} else if (finished) {
			row->settled = true; // this file carries no artwork
		} else {
			pending = true;
		}
	}

	if (pending) {
		lv_timer_resume(thumb_timer);
	}
}

static void thumb_timer_cb(lv_timer_t *timer) {
	lv_timer_pause(timer); // thumbs_update turns it back on while work remains
	if (lv_screen_active() == search_screen) {
		thumbs_update();
	}
}

// ---------------------------------------------------------------------------
// The menu a track hit opens on a long press
// ---------------------------------------------------------------------------

// The popover holds at most six rows.
#define MENU_PLAYLISTS_MAX 6

static char menu_path[512];
static char menu_name[256];
static lv_obj_t *menu_anchor;
// The playlists that hold the track, gathered when the menu opens.
static char menu_playlists[MENU_PLAYLISTS_MAX][128];
static int menu_playlist_count;

static void menu_fav_action(void *user) {
	(void)user;
	if (menu_path[0]) {
		library_fav_toggle(menu_path, menu_name, "");
	}
}

// Where "add to queue" puts it, the same as the library lists' menu. The
// queue on disk is written now so a power-off does not lose the addition.
static void menu_queue_action(void *user) {
	(void)user;
	if (menu_path[0] && playlist_enqueue(menu_path)) {
		device_state_queue_changed();
		toast_success("added_to_the_queue");
	}
}

static void menu_playlist_action(void *user) {
	(void)user;
	if (menu_path[0]) {
		playlistpage_add_track(menu_path);
	}
}

static void menu_remove_from(const char *playlist) {
	if (playlists_remove_track(playlist, menu_path)) {
		toast_success("medialist_track_removed");
	} else {
		gui_notify_popup("medialist_remove_failed");
	}
}

static void menu_remove_one_action(void *user) {
	int i = (int)(intptr_t)user;
	if (i >= 0 && i < menu_playlist_count && menu_path[0]) {
		menu_remove_from(menu_playlists[i]);
	}
}

static void remove_menu_show_async(void *user) {
	(void)user;
	if (menu_playlist_count < 1 || !gui_obj_alive(menu_anchor)) {
		return;
	}
	popover_item_t items[MENU_PLAYLISTS_MAX];
	for (int i = 0; i < menu_playlist_count; i++) {
		items[i] = (popover_item_t){menu_playlists[i], menu_remove_one_action, (void *)(intptr_t)i, false};
	}
	popover_show(menu_anchor, items, menu_playlist_count);
}

// One playlist holds the track: it leaves that one. Several: a second menu
// names them, and the track leaves the one picked. That menu is built on the
// next pass of the loop, because this runs inside the click of a row the
// popover is about to delete.
static void menu_remove_action(void *user) {
	(void)user;
	if (menu_playlist_count == 1) {
		menu_remove_from(menu_playlists[0]);
	} else if (menu_playlist_count > 1) {
		lv_async_call(remove_menu_show_async, NULL);
	}
}

static bool collect_playlist_cb(const char *name, void *user) {
	(void)user;
	if (playlists_has_track(name, menu_path)) {
		snprintf(menu_playlists[menu_playlist_count], sizeof(menu_playlists[0]), "%s", name);
		menu_playlist_count++;
	}
	return menu_playlist_count < MENU_PLAYLISTS_MAX;
}

static void track_long_pressed_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	row_t *row = lv_event_get_user_data(e);
	if (!row || current_cat != CAT_TRACKS || row->index < 0 || !row->path[0]) {
		return;
	}

	// The lift that ends this press would otherwise arrive as a click and
	// start the track.
	lv_indev_t *indev = lv_indev_active();
	if (indev) {
		lv_indev_wait_release(indev);
	}

	snprintf(menu_path, sizeof(menu_path), "%s", row->path);
	snprintf(menu_name, sizeof(menu_name), "%s", row->name);
	menu_anchor = row->button;

	menu_playlist_count = 0;
	playlists_for_each(collect_playlist_cb, NULL);

	popover_item_t items[4];
	int n = 0;
	items[n++] = (popover_item_t){
		library_fav_contains(menu_path) ? "remove_from_favourites" : "medialist_add_to_favourites", menu_fav_action, NULL,
		false};
	items[n++] = (popover_item_t){"add_to_queue", menu_queue_action, NULL, false};
	items[n++] = (popover_item_t){"add_to_playlist", menu_playlist_action, NULL, false};
	if (menu_playlist_count > 0) {
		items[n++] = (popover_item_t){"medialist_remove_from_playlist", menu_remove_action, NULL, false};
	}
	popover_show(menu_anchor, items, n);
}

// ---------------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------------

static void run_search(const char *query, bool keep_place);

// One hit, read out of a window of the index.
typedef struct {
	char name[256];
	char path[512];
	char artist[256];
} hit_t;

typedef struct {
	hit_t *hits;
	int count;
} window_ctx_t;

static bool window_cb(const char *name, const char *path, const char *artist, void *user) {
	window_ctx_t *ctx = user;
	hit_t *hit = &ctx->hits[ctx->count++];
	snprintf(hit->name, sizeof(hit->name), "%s", name ? name : "");
	snprintf(hit->path, sizeof(hit->path), "%s", path ? path : "");
	snprintf(hit->artist, sizeof(hit->artist), "%s", artist ? artist : "");
	return true;
}

static void row_bind(row_t *row, int index, const hit_t *hit) {
	row_drop_art(row);
	row->index = index;
	snprintf(row->name, sizeof(row->name), "%s", hit->name);
	snprintf(row->path, sizeof(row->path), "%s", hit->path);

	switch (current_cat) {
	case CAT_TRACKS:
		row->glyph = &icon_music2;
		break;
	case CAT_ALBUMS:
		row->glyph = &icon_album;
		break;
	default:
		// An artist row never carries artwork: the glyph is the final image,
		// not a stand-in.
		row->glyph = &icon_artist;
		row->path[0] = '\0';
		break;
	}
	lv_image_set_src(row->icon, row->glyph);
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);

	// An album comes named by its value (name and key, see library.h).
	char shown[256];
	library_album_title(hit->name, shown, sizeof(shown));
	lv_label_set_text(row->title, shown);

	// Who it is by, under the title: a track's artist, an album's artist.
	if (current_cat != CAT_ARTISTS && hit->artist[0]) {
		lv_label_set_text(row->subtitle, hit->artist);
		lv_obj_set_hidden(row->subtitle, false);
	} else {
		lv_obj_set_hidden(row->subtitle, true);
	}
	lv_obj_set_hidden(row->chevron, current_cat == CAT_TRACKS);

	lv_obj_set_y(row->button, index * SEARCH_PITCH);
	lv_obj_set_hidden(row->button, false);
}

static void row_unbind(row_t *row) {
	row_drop_art(row);
	row->index = -1;
	row->name[0] = '\0';
	row->path[0] = '\0';
	lv_obj_set_hidden(row->button, true);
}

// Binds the pool to the hits under the scroll position. A row keeps the hit
// it shows for as long as that hit stays in the window, so scrolling by one
// row rebinds one row and not the pool.
static void window_update(void) {
	library_index_t *ix = cat_ix[current_cat];
	int count = cat_count[current_cat];
	if (!ix || count <= 0) {
		for (int i = 0; i < SEARCH_POOL; i++) {
			row_unbind(&rows[i]);
		}
		return;
	}

	int first = (int)lv_obj_get_scroll_y(results) / SEARCH_PITCH - 1;
	if (first > count - SEARCH_POOL) {
		first = count - SEARCH_POOL;
	}
	if (first < 0) {
		first = 0;
	}
	int last = first + SEARCH_POOL; // exclusive
	if (last > count) {
		last = count;
	}

	bool wanted = false;
	for (int index = first; index < last; index++) {
		if (rows[index % SEARCH_POOL].index != index) {
			wanted = true;
		}
	}
	if (wanted) {
		static hit_t hits[SEARCH_POOL];
		window_ctx_t ctx = {hits, 0};
		library_index_window(ix, first, last - first, window_cb, &ctx);
		if (ctx.count < last - first) {
			// The handle is from before a rescan or a card change: the hits
			// are asked for again, and this list comes back where it was.
			if (library_index_stale(ix)) {
				run_search(searched, true);
			}
			return;
		}
		for (int index = first; index < last; index++) {
			row_t *row = &rows[index % SEARCH_POOL];
			if (row->index != index) {
				row_bind(row, index, &hits[index - first]);
			}
		}
	}
	for (int i = 0; i < SEARCH_POOL; i++) {
		if (rows[i].index >= 0 && (rows[i].index < first || rows[i].index >= last)) {
			row_unbind(&rows[i]);
		}
	}
	if (thumb_timer) {
		lv_timer_resume(thumb_timer);
	}
}

static void scroll_cb(lv_event_t *e) {
	(void)e;
	window_update();
}

static void row_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	row_t *row = lv_event_get_user_data(e);
	if (!row || row->index < 0) {
		return;
	}

	switch (current_cat) {
	case CAT_TRACKS: {
		// The queue is the whole list of hits, so next and previous walk the
		// search the way they walk any library list.
		library_index_t *ix = cat_ix[CAT_TRACKS];
		if (ix && library_index_stale(ix)) {
			run_search(searched, true);
			return;
		}
		if (ix && device_state_play_index(library_index_clone(ix), row->index)) {
			player_refresh_now_playing();
		} else if (row->path[0]) {
			player_play_file(row->path);
		}
		switch_screen(player_screen);
		break;
	}
	case CAT_ALBUMS:
		if (row->name[0]) {
			medialist_open(row->name, LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, row->name);
		}
		break;
	default:
		medialist_open_artist(row->name);
		break;
	}
}

static void row_create(row_t *row) {
	lv_obj_t *button = lv_btn_create(body);
	row->button = button;
	row->index = -1;
	lv_obj_set_size(button, lv_pct(100), SEARCH_ROW_H);
	lv_obj_add_style(button, &theme_style_card, 0);
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(button, 12, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_hor(button, 16, 0);
	lv_obj_set_event_bubble(button, true);
	lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(button, 12, 0);
	lv_obj_add_event_cb(button, row_clicked_cb, LV_EVENT_CLICKED, row);
	lv_obj_add_event_cb(button, track_long_pressed_cb, LV_EVENT_LONG_PRESSED, row);
	lv_obj_set_hidden(button, true);

	row->icon = lv_image_create(button);
	lv_obj_add_style(row->icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);
	lv_obj_set_size(row->icon, SEARCH_THUMB, SEARCH_THUMB);
	lv_image_set_inner_align(row->icon, LV_IMAGE_ALIGN_CENTER);
	lv_obj_set_style_radius(row->icon, 8, 0);
	lv_obj_set_style_clip_corner(row->icon, true, 0);

	lv_obj_t *texts = lv_obj_create(button);
	lv_obj_set_flex_grow(texts, 1);
	lv_obj_set_height(texts, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(texts, 0, 0);
	lv_obj_set_style_border_width(texts, 0, 0);
	lv_obj_set_style_pad_all(texts, 0, 0);
	lv_obj_set_style_pad_row(texts, 2, 0);
	lv_obj_set_scrollable(texts, false);
	lv_obj_set_clickable(texts, false);
	lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);

	row->title = lv_label_create(texts);
	lv_label_set_long_mode(row->title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(row->title, lv_pct(100));
	lv_obj_set_height(row->title, lv_font_get_line_height(&font_ui_24));
	lv_obj_add_style(row->title, &theme_style_text, 0);
	lv_obj_set_style_text_font(row->title, &font_ui_24, 0);

	row->subtitle = lv_label_create(texts);
	lv_label_set_long_mode(row->subtitle, LV_LABEL_LONG_DOT);
	lv_obj_set_width(row->subtitle, lv_pct(100));
	lv_obj_set_height(row->subtitle, lv_font_get_line_height(&font_ui_18));
	lv_obj_add_style(row->subtitle, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(row->subtitle, &font_ui_18, 0);

	row->chevron = lv_image_create(button);
	lv_image_set_src(row->chevron, &icon_chevron_right);
	lv_obj_add_style(row->chevron, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(row->chevron, LV_OPA_60, 0);
}

// ---------------------------------------------------------------------------
// The categories
// ---------------------------------------------------------------------------

static void pill_paint(category_t cat) {
	bool on = cat == current_cat;
	lv_obj_set_style_bg_color(pill[cat], on ? theme()->accent : theme()->surface_pressed, 0);
	lv_obj_set_style_text_color(pill_name[cat], on ? lv_color_white() : theme()->text_primary, 0);
	lv_obj_set_style_text_color(pill_count[cat], on ? lv_color_white() : theme()->text_secondary, 0);
}

static void pills_paint(void) {
	for (int i = 0; i < CAT_COUNT; i++) {
		pill_paint((category_t)i);
	}
}

// Puts one category's hits on screen, at the place that list was left.
static void show_category(category_t cat) {
	current_cat = cat;
	pills_paint();
	for (int i = 0; i < SEARCH_POOL; i++) {
		row_unbind(&rows[i]);
	}
	int count = cat_count[cat];
	lv_obj_set_height(body, count > 0 ? count * SEARCH_PITCH - SEARCH_ROW_GAP : 0);
	lv_obj_update_layout(results);
	lv_obj_scroll_to_y(results, cat_scroll[cat], LV_ANIM_OFF);
	window_update();
}

static void pill_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	category_t cat = (category_t)(intptr_t)lv_event_get_user_data(e);
	if (cat == current_cat || cat_count[cat] <= 0) {
		return;
	}
	cat_scroll[current_cat] = (int)lv_obj_get_scroll_y(results);
	show_category(cat);
}

static lv_obj_t *pill_create(category_t cat) {
	lv_obj_t *btn = lv_btn_create(pills);
	lv_obj_set_height(btn, SEARCH_PILLS_H);
	lv_obj_set_flex_grow(btn, 1);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_pad_hor(btn, 8, 0);
	lv_obj_set_style_pad_ver(btn, 4, 0);
	lv_obj_set_style_pad_row(btn, 0, 0);
	lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(btn, pill_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)cat);

	// The name over the number of hits: two short lines fit every language,
	// where "Исполнители (1234)" on one would not.
	pill_name[cat] = lv_label_create(btn);
	lv_label_set_text(pill_name[cat], tr(CAT_LABEL[cat]));
	lv_label_set_long_mode(pill_name[cat], LV_LABEL_LONG_DOT);
	lv_obj_set_style_max_width(pill_name[cat], lv_pct(100), 0);
	lv_obj_set_style_text_font(pill_name[cat], &font_ui_20, 0);

	pill_count[cat] = lv_label_create(btn);
	lv_obj_set_style_text_font(pill_count[cat], &font_ui_16, 0);
	return btn;
}

// ---------------------------------------------------------------------------
// Running the query
// ---------------------------------------------------------------------------

// Puts the hits away: no pills, no rows, no handles.
static void results_clear(void) {
	for (int i = 0; i < SEARCH_POOL; i++) {
		row_unbind(&rows[i]);
	}
	for (int i = 0; i < CAT_COUNT; i++) {
		library_index_close(cat_ix[i]);
		cat_ix[i] = NULL;
		cat_count[i] = 0;
		cat_scroll[i] = 0;
	}
	searched[0] = '\0';
	lv_obj_set_height(body, 0);
	lv_obj_set_hidden(pills, true);
	lv_obj_set_hidden(results, true);
	lv_obj_set_hidden(empty_label, true);
}

// Asks the library for all three lists. `keep_place` is for the same query
// asked again because the library changed under it: the category and the
// scroll positions stay where they were.
static void run_search(const char *query, bool keep_place) {
	char wanted[sizeof(searched)];
	snprintf(wanted, sizeof(wanted), "%s", query ? query : "");
	int scroll[CAT_COUNT];
	memcpy(scroll, cat_scroll, sizeof(scroll));
	category_t was = current_cat;

	results_clear();
	if (!wanted[0]) {
		return;
	}
	if (!library_is_open()) {
		gui_notify_no_card();
		return;
	}

	snprintf(searched, sizeof(searched), "%s", wanted);
	int total = 0;
	for (int i = 0; i < CAT_COUNT; i++) {
		cat_ix[i] = library_index_open(CAT_KIND[i], LIBRARY_FILTER_SEARCH, wanted, LIBRARY_ORDER_DEFAULT, false);
		cat_count[i] = library_index_count(cat_ix[i]);
		total += cat_count[i];
		if (keep_place) {
			cat_scroll[i] = scroll[i];
		}

		char text[16];
		snprintf(text, sizeof(text), "%d", cat_count[i]);
		lv_label_set_text(pill_count[i], text);
		// A category with nothing in it has no pill: there is nothing to see.
		lv_obj_set_hidden(pill[i], cat_count[i] == 0);
	}

	if (total == 0) {
		lv_obj_set_hidden(empty_label, false);
		return;
	}

	// The category last looked at, while it has hits; else the first that has.
	category_t cat = was;
	if (cat_count[cat] == 0) {
		for (int i = 0; i < CAT_COUNT; i++) {
			if (cat_count[i] > 0) {
				cat = (category_t)i;
				break;
			}
		}
	}
	lv_obj_set_hidden(pills, false);
	lv_obj_set_hidden(results, false);
	show_category(cat);
}

// The caret's own look: accent-coloured and a little thicker than LVGL's
// hairline, drawn as the cursor part's left border. Applied to the focused
// state too, because LVGL's built-in theme styles the cursor on
// LV_PART_CURSOR|LV_STATE_FOCUSED, and would otherwise hand the caret back to
// the default on the first tap in the field.
static void caret_apply_style(void) { keyboard_style_caret(field); }

// Shows the clear button only when there is something to clear.
static void refresh_clear_button(void) {
	if (!clear_btn) {
		return;
	}
	const char *text = lv_textarea_get_text(field);
	if (text && text[0]) {
		lv_obj_set_hidden(clear_btn, false);
	} else {
		lv_obj_set_hidden(clear_btn, true);
	}
}

static void clear_clicked_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(field, "");
	results_clear();
	refresh_clear_button();
	search_keyboard_show(true); // ready for the next query
}

// The results answer the query they were run with: once the text says
// something else, they go, and the next press of the search key brings the new
// ones.
static void field_changed_cb(lv_event_t *e) {
	(void)e;
	refresh_clear_button();
	const char *text = lv_textarea_get_text(field);
	if (searched[0] && strcmp(text ? text : "", searched) != 0) {
		results_clear();
	}
}

// The accent key on the keyboard: run the query and hand the page to the
// results.
static void kb_search_cb(lv_event_t *e) {
	(void)e;
	run_search(lv_textarea_get_text(field), false);
	search_keyboard_show(false); // the results get the whole page
}

// The results area stops exactly where the keyboard starts, and grows into the
// freed space when the keyboard goes away. Full height with a keyboard's worth
// of bottom padding instead would let the list scroll far past its last row and
// slide the hits under the keys.
static void layout_results(void) {
	bool kb_visible = keyboard_is_visible(keyboard);
	int top = results_top + SEARCH_PILLS_H + SEARCH_PILLS_GAP;
	int height = screen_h - top - (kb_visible ? keyboard_height : 0);
	if (height < 60) {
		height = 60;
	}
	lv_obj_set_height(results, height);
	window_update();
}

static void search_keyboard_show(bool visible) {
	keyboard_set_visible(keyboard, visible);
	layout_results();
}

static void field_clicked_cb(lv_event_t *e) {
	(void)e;
	search_keyboard_show(true);
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	if (art_drop_timer) {
		lv_timer_pause(art_drop_timer); // back before the wait ran out
	}

	// Back from a hit the results are still there to pick another; a page with
	// nothing on it is a page to type in.
	if (!searched[0]) {
		search_keyboard_show(true);
		return;
	}
	for (int i = 0; i < CAT_COUNT; i++) {
		if (cat_ix[i] && library_index_stale(cat_ix[i])) {
			cat_scroll[current_cat] = (int)lv_obj_get_scroll_y(results);
			run_search(searched, true);
			return;
		}
	}
	window_update();
	thumbs_update(); // whatever was dropped is asked for again
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	if (art_drop_timer) {
		lv_timer_reset(art_drop_timer);
		lv_timer_resume(art_drop_timer);
	}
}

static void refresh_theme(void) {
	if (pill[0]) {
		pills_paint();
	}
}

// ---------------------------------------------------------------------------

void search_open(void) { switch_screen(search_screen); }

void search_init(gui_config_t *cfg) {
	search_screen = lv_obj_create(NULL);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	settingsrow_title(search_screen, cfg, "search_2");

	int top = settingsrow_content_top(cfg);
	int keyboard_h = 316; // tall enough for honest fingertip-sized keys
	screen_h = cfg->screen_height;
	keyboard_height = keyboard_h;
	results_top = top + 62 + 10;

	field = lv_textarea_create(search_screen);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_placeholder_text(field, tr("search"));
	// Tall enough for the 24 px line plus its padding: at 56 the text does not
	// quite fit and the content bobs on every keystroke.
	lv_obj_set_size(field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, cfg->padding, top);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_radius(field, 12, 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_all(field, 14, 0);
	lv_obj_set_style_text_font(field, &font_ui_24, 0);
	lv_obj_add_event_cb(field, field_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(field, field_clicked_cb, LV_EVENT_CLICKED, NULL);

	caret_apply_style();

	// The clear button, over the field's right edge: wipes the query and the
	// results in one tap. Hidden while the field is empty.
	clear_btn = lv_btn_create(search_screen);
	lv_obj_set_size(clear_btn, 56, 56);
	lv_obj_align(clear_btn, LV_ALIGN_TOP_RIGHT, -cfg->padding - 4, top + (62 - 56) / 2);
	lv_obj_set_style_bg_opa(clear_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(clear_btn, 0, 0);
	lv_obj_set_style_border_width(clear_btn, 0, 0);
	lv_obj_set_style_pad_all(clear_btn, 0, 0);
	lv_obj_set_hidden(clear_btn, true);
	lv_obj_add_event_cb(clear_btn, clear_clicked_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *clear_icon = lv_image_create(clear_btn);
	lv_image_set_src(clear_icon, &icon_clear);
	lv_obj_add_style(clear_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(clear_icon, LV_OPA_70, 0);
	lv_obj_center(clear_icon);

	// Room for it, so a long query never runs underneath the x.
	lv_obj_set_style_pad_right(field, 60, 0);

	// The three categories, in a row under the field.
	pills = lv_obj_create(search_screen);
	lv_obj_set_size(pills, cfg->screen_width - 2 * cfg->padding, SEARCH_PILLS_H);
	lv_obj_align(pills, LV_ALIGN_TOP_LEFT, cfg->padding, results_top);
	lv_obj_set_style_bg_opa(pills, 0, 0);
	lv_obj_set_style_border_width(pills, 0, 0);
	lv_obj_set_style_pad_all(pills, 0, 0);
	lv_obj_set_style_pad_column(pills, 10, 0);
	lv_obj_set_scrollable(pills, false);
	lv_obj_set_event_bubble(pills, true);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW);
	for (int i = 0; i < CAT_COUNT; i++) {
		pill[i] = pill_create((category_t)i);
	}
	pills_paint();
	lv_obj_set_hidden(pills, true);

	results = lv_obj_create(search_screen);
	lv_obj_set_width(results, cfg->screen_width);
	lv_obj_align(results, LV_ALIGN_TOP_LEFT, 0, results_top + SEARCH_PILLS_H + SEARCH_PILLS_GAP);
	lv_obj_set_style_bg_opa(results, 0, 0);
	lv_obj_set_style_border_width(results, 0, 0);
	lv_obj_set_style_radius(results, 0, 0);
	lv_obj_set_style_pad_hor(results, cfg->padding, 0);
	lv_obj_set_style_pad_ver(results, 0, 0);
	lv_obj_set_style_pad_bottom(results, 12, 0);
	lv_obj_set_scroll_dir(results, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(results, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_event_bubble(results, true);
	lv_obj_add_event_cb(results, scroll_cb, LV_EVENT_SCROLL, NULL);
	lv_obj_set_hidden(results, true);

	// The rows are placed by hand, at index * pitch, on a body as tall as the
	// whole list: the scroll bar and the fling then cover every hit while only
	// the pool exists.
	body = lv_obj_create(results);
	lv_obj_set_size(body, lv_pct(100), 0);
	lv_obj_set_style_bg_opa(body, 0, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_radius(body, 0, 0);
	lv_obj_set_style_pad_all(body, 0, 0);
	lv_obj_set_scrollable(body, false);
	lv_obj_set_event_bubble(body, true);
	for (int i = 0; i < SEARCH_POOL; i++) {
		row_create(&rows[i]);
	}

	empty_label = lv_label_create(search_screen);
	lv_label_set_text(empty_label, tr("no_results"));
	lv_obj_add_style(empty_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(empty_label, &font_ui_24, 0);
	lv_obj_align(empty_label, LV_ALIGN_TOP_LEFT, cfg->padding, results_top + 16);
	lv_obj_set_hidden(empty_label, true);

	keyboard = keyboard_create(search_screen, cfg->screen_width, keyboard_h, field, &icon_search, NULL, kb_search_cb,
							   NULL);
	layout_results();

	thumb_timer = lv_timer_create(thumb_timer_cb, SEARCH_THUMB_POLL_MS, NULL);
	lv_timer_pause(thumb_timer);
	art_drop_timer = lv_timer_create(art_drop_cb, ART_DROP_DELAY_MS, NULL);
	lv_timer_pause(art_drop_timer);

	theme_register_refresh(refresh_theme);
	lv_obj_add_event_cb(search_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(search_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	switcher_attach_back_gesture(search_screen);
	player_sheet_attach_drag(search_screen, true);
}
