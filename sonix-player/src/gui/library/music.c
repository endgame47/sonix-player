#include "music.h"

#include "lvgl/lvgl.h"

#include <stdio.h>
#include <string.h>

#include "src/gui/library/browser.h"
#include "src/gui/library/libraryscan.h"
#include "src/gui/nowplaying/coverflow.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/library/medialist.h"
#include "src/gui/settings/musicsettings.h"
#include "src/gui/library/playlistpage.h"
#include "src/gui/library/search.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"

lv_obj_t *music_screen;

// The index tiles load their list from the database and then switch screen,
// so each gets a tiny opener instead of a target screen pointer.
static void open_all_tracks(void) { medialist_open(tr("music_all_tracks"), LIBRARY_LIST_TRACKS, LIBRARY_FILTER_NONE, NULL); }
static void open_albums(void) { medialist_open(tr("albums"), LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_NONE, NULL); }
static void open_artists(void) { medialist_open(tr("artists"), LIBRARY_LIST_ARTISTS, LIBRARY_FILTER_NONE, NULL); }
static void open_album_artists(void) {
	medialist_open(tr("music_album_artists"), LIBRARY_LIST_ALBUM_ARTISTS, LIBRARY_FILTER_NONE, NULL);
}
static void open_genres(void) { medialist_open(tr("music_genres"), LIBRARY_LIST_GENRES, LIBRARY_FILTER_NONE, NULL); }
static void open_years(void) { medialist_open(tr("music_year"), LIBRARY_LIST_YEARS, LIBRARY_FILTER_NONE, NULL); }
static void open_favourites(void) {
	medialist_open(tr("favourites"), LIBRARY_LIST_FAVOURITES, LIBRARY_FILTER_NONE, NULL);
}
static void open_most_played(void) {
	medialist_open(tr("music_most_played"), LIBRARY_LIST_MOST_PLAYED, LIBRARY_FILTER_NONE, NULL);
}
static void open_browse(void) { switch_screen(browser_screen); }
static void open_playlists(void) { playlistpage_open(); }

static lv_obj_t *tile_grid;
static gui_config_t *grid_cfg;

// ---------------------------------------------------------------------------
// Which tiles, in which order
//
// Every tile is in use to begin with, in the order of TILES. The user can put
// them in another order and leave some out (Music settings > Library >
// Rearrange main page); at least one always stays. Both lists are
// kept in the config by the tiles' names, so a tile added by a later build is
// simply one the saved lists do not mention, and joins the end of the tiles in
// use.
// ---------------------------------------------------------------------------

static const struct {
	const char *id; // its name in the config, never translated
	grid_entry_t entry;
} TILES[MUSIC_TILE_COUNT] = {
	[MUSIC_TILE_ALL] = {"all", {"music_all_tracks", &icon_menu_all, NULL, open_all_tracks}},
	[MUSIC_TILE_ALBUMS] = {"albums", {"albums", &icon_menu_album, NULL, open_albums}},
	[MUSIC_TILE_ARTISTS] = {"artists", {"artists", &icon_menu_artist, NULL, open_artists}},
	[MUSIC_TILE_ALBUM_ARTISTS] = {"album_artists",
								  {"music_album_artists", &icon_menu_album_artist, NULL, open_album_artists}},
	[MUSIC_TILE_GENRES] = {"genres", {"music_genres", &icon_menu_genre, NULL, open_genres}},
	[MUSIC_TILE_YEARS] = {"years", {"music_year", &icon_menu_year, NULL, open_years}},
	[MUSIC_TILE_FAVOURITES] = {"favourites", {"favourites", &icon_menu_favourites, NULL, open_favourites}},
	[MUSIC_TILE_MOST_PLAYED] = {"most_played",
								{"music_most_played", &icon_menu_most_played, NULL, open_most_played}},
	[MUSIC_TILE_PLAYLISTS] = {"playlists", {"playlists", &icon_menu_playlist, NULL, open_playlists}},
	[MUSIC_TILE_BROWSE] = {"browse", {"music_browse", &icon_menu_explorer, NULL, open_browse}},
};

static music_tile_t in_use[MUSIC_TILE_COUNT];
static int in_use_n;
static music_tile_t hidden[MUSIC_TILE_COUNT];
static int hidden_n;
static bool order_loaded;

static int tile_by_id(const char *id, size_t len) {
	for (int i = 0; i < MUSIC_TILE_COUNT; i++) {
		if (strlen(TILES[i].id) == len && strncmp(TILES[i].id, id, len) == 0) {
			return i;
		}
	}
	return -1;
}

static bool listed(const music_tile_t *list, int n, music_tile_t tile) {
	for (int i = 0; i < n; i++) {
		if (list[i] == tile) {
			return true;
		}
	}
	return false;
}

// Reads one saved list ("albums,artists,...") into `list`, skipping names this
// build does not know and any tile already placed in either list.
static int parse_list(const char *text, music_tile_t *list) {
	int n = 0;
	const char *at = text ? text : "";
	while (*at) {
		const char *end = strchr(at, ',');
		size_t len = end ? (size_t)(end - at) : strlen(at);
		int tile = tile_by_id(at, len);
		if (tile >= 0 && !listed(in_use, in_use_n, (music_tile_t)tile) && !listed(hidden, hidden_n, (music_tile_t)tile) &&
			!listed(list, n, (music_tile_t)tile)) {
			list[n++] = (music_tile_t)tile;
		}
		if (!end) {
			break;
		}
		at = end + 1;
	}
	return n;
}

static void order_load(void) {
	if (order_loaded) {
		return;
	}
	order_loaded = true;
	in_use_n = 0;
	hidden_n = 0;
	in_use_n = parse_list(config_get("music", "tiles", ""), in_use);
	hidden_n = parse_list(config_get("music", "tiles_hidden", ""), hidden);
	// Whatever neither list names is in use, at the end, in the usual order.
	for (int i = 0; i < MUSIC_TILE_COUNT; i++) {
		if (!listed(in_use, in_use_n, (music_tile_t)i) && !listed(hidden, hidden_n, (music_tile_t)i)) {
			in_use[in_use_n++] = (music_tile_t)i;
		}
	}
	if (in_use_n == 0 && hidden_n > 0) {
		in_use[in_use_n++] = hidden[--hidden_n]; // never an empty page
	}
}

static void save_list(const char *key, const music_tile_t *list, int n) {
	char text[160] = "";
	size_t used = 0;
	for (int i = 0; i < n; i++) {
		int wrote = snprintf(text + used, sizeof(text) - used, "%s%s", i ? "," : "", TILES[list[i]].id);
		if (wrote < 0 || (size_t)wrote >= sizeof(text) - used) {
			break;
		}
		used += (size_t)wrote;
	}
	config_set("music", key, text);
}

// The page's tiles, rebuilt from the lists, and the lists written down.
static void order_changed(void) {
	save_list("tiles", in_use, in_use_n);
	save_list("tiles_hidden", hidden, hidden_n);
	config_save();
	music_rebuild_tiles();
}

static void list_remove(music_tile_t *list, int *n, music_tile_t tile) {
	for (int i = 0; i < *n; i++) {
		if (list[i] == tile) {
			memmove(&list[i], &list[i + 1], (size_t)(*n - i - 1) * sizeof(*list));
			(*n)--;
			return;
		}
	}
}

static void list_insert(music_tile_t *list, int *n, music_tile_t tile, int position) {
	if (position < 0) {
		position = 0;
	}
	if (position > *n) {
		position = *n;
	}
	memmove(&list[position + 1], &list[position], (size_t)(*n - position) * sizeof(*list));
	list[position] = tile;
	(*n)++;
}

int music_tiles_in_use_count(void) {
	order_load();
	return in_use_n;
}

music_tile_t music_tiles_in_use_at(int position) {
	order_load();
	return position >= 0 && position < in_use_n ? in_use[position] : MUSIC_TILE_ALL;
}

int music_tiles_hidden_count(void) {
	order_load();
	return hidden_n;
}

music_tile_t music_tiles_hidden_at(int position) {
	order_load();
	return position >= 0 && position < hidden_n ? hidden[position] : MUSIC_TILE_ALL;
}

void music_tiles_move_in_use(music_tile_t tile, int position) {
	order_load();
	if (tile < 0 || tile >= MUSIC_TILE_COUNT) {
		return;
	}
	list_remove(in_use, &in_use_n, tile);
	list_remove(hidden, &hidden_n, tile);
	list_insert(in_use, &in_use_n, tile, position);
	order_changed();
}

bool music_tiles_move_hidden(music_tile_t tile, int position) {
	order_load();
	if (tile < 0 || tile >= MUSIC_TILE_COUNT) {
		return false;
	}
	// The last tile in use stays: a Music page with nothing on it has no way
	// into the library at all.
	if (listed(in_use, in_use_n, tile) && in_use_n <= 1) {
		return false;
	}
	list_remove(in_use, &in_use_n, tile);
	list_remove(hidden, &hidden_n, tile);
	list_insert(hidden, &hidden_n, tile, position);
	order_changed();
	return true;
}

const char *music_tile_tag(music_tile_t tile) {
	return tile >= 0 && tile < MUSIC_TILE_COUNT ? TILES[tile].entry.label : "";
}

const lv_image_dsc_t *music_tile_icon(music_tile_t tile) {
	return tile >= 0 && tile < MUSIC_TILE_COUNT ? TILES[tile].entry.icon : NULL;
}

void music_rebuild_tiles(void) {
	if (!tile_grid || !grid_cfg) {
		return;
	}
	order_load();
	grid_entry_t entries[MUSIC_TILE_COUNT];
	for (int i = 0; i < in_use_n; i++) {
		entries[i] = TILES[in_use[i]].entry;
	}
	// Tiles at the size of the other sections' six: past the third row the
	// page scrolls.
	gridpage_set_entries(tile_grid, grid_cfg, entries, in_use_n, 2, 3, true);
	lv_obj_scroll_to_y(tile_grid, 0, LV_ANIM_OFF);
}

static void search_cb(lv_event_t *e) {
	(void)e;
	search_open();
}

// With no tracks indexed the tiles would all open empty lists: the page says
// a scan is needed instead, and the button asks which folders to scan.
static lv_obj_t *empty_panel;

static void scan_cb(lv_event_t *e) {
	(void)e;
	libraryscan_choose_folders();
}

// "More below": a chevron that drops off the bottom edge as the page opens
// with tiles under the fold. One short animation of a small picture, moved and
// faded together, and hidden again once it is over.
#define MORE_HINT_TRAVEL 40	   // from just above the edge to past it
#define MORE_HINT_DELAY_MS 150 // long enough for the page to be seen first
#define MORE_HINT_MS 450

static lv_obj_t *more_hint;

static void more_hint_exec(void *obj, int32_t y) {
	lv_obj_set_style_translate_y(obj, y, 0);
	lv_obj_set_style_image_opa(obj, (lv_opa_t)(LV_OPA_COVER - y * LV_OPA_COVER / MORE_HINT_TRAVEL), 0);
}

static void more_hint_done(lv_anim_t *a) {
	(void)a;
	lv_obj_set_hidden(more_hint, true);
}

static void more_hint_play(void) {
	if (!more_hint) {
		return;
	}
	lv_anim_delete(more_hint, more_hint_exec);
	more_hint_exec(more_hint, 0);
	lv_obj_set_hidden(more_hint, false);
	lv_obj_move_foreground(more_hint);

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, more_hint);
	lv_anim_set_exec_cb(&a, more_hint_exec);
	lv_anim_set_values(&a, 0, MORE_HINT_TRAVEL);
	lv_anim_set_delay(&a, MORE_HINT_DELAY_MS);
	lv_anim_set_duration(&a, MORE_HINT_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
	lv_anim_set_completed_cb(&a, more_hint_done);
	lv_anim_start(&a);
}

// The tiles, or what stands in for them: a note that the card is missing --
// the index lives on it -- or the scan panel while nothing is indexed.
static void show_tiles_or_panel(void) {
	if (!gui_card_available()) {
		gridpage_show_no_card(tile_grid, empty_panel, show_tiles_or_panel);
		return;
	}
	bool empty = !library_scan_running() && library_track_count() == 0;
	gridpage_show_empty(tile_grid, empty_panel, empty);
	// More tiles than the screen holds: the scrollbar shows for a moment, and
	// the chevron, while there is more below where the page stands.
	if (!empty) {
		gridpage_hint_scroll(tile_grid);
		lv_obj_update_layout(tile_grid);
		if (lv_obj_get_scroll_bottom(tile_grid) > 0) {
			more_hint_play();
		}
	}
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	show_tiles_or_panel();
}

// The corner buttons share everything but icon and action.
static lv_obj_t *corner_button(gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph) {
	lv_obj_t *button = lv_btn_create(music_screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (56 + 6), cfg->padding + cfg->top_bar_height);

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);

	return button;
}

void music_init(gui_config_t *cfg) {
	// Built empty and filled from the saved order. With more tiles than the
	// three rows other sections have, the page scrolls to the rest.
	grid_cfg = cfg;
	lv_obj_t *grid = gridpage_build(music_screen, cfg, NULL, 0, 2, 3, true);
	tile_grid = grid;
	gridpage_set_scrolling(grid);
	music_rebuild_tiles();

	// The pull from the bottom edge that opens the album carousel. It goes on
	// the grid because that is where a tile's press stops bubbling.
	coverflow_attach_edge(grid, cfg);

	// Two buttons in the corner, so the heading has to be told to clear them.
	settingsrow_title_corner_slots(settingsrow_title(music_screen, cfg, "music"), cfg, 2);

	lv_obj_t *settings_btn = corner_button(cfg, 0, &icon_music_settings);
	lv_obj_add_event_cb(settings_btn, switch_screen_cb, LV_EVENT_CLICKED, musicsettings_screen);

	lv_obj_t *search_btn = corner_button(cfg, 1, &icon_search);
	lv_obj_add_event_cb(search_btn, search_cb, LV_EVENT_CLICKED, NULL);

	empty_panel = gridpage_empty_panel(music_screen, cfg, &icon_music_note, "music_no_database", scan_cb);

	// Over the tiles but never in the way: a press goes through it to the tile,
	// or to the pull from the bottom edge. Floating, because on its way down it
	// leaves the screen, and a child past the edge makes the screen itself
	// scrollable -- its scrollbar came up over the page's for the length of the
	// animation.
	more_hint = lv_image_create(music_screen);
	lv_image_set_src(more_hint, &icon_chevron_down);
	lv_obj_add_style(more_hint, &theme_style_icon, 0);
	lv_obj_align(more_hint, LV_ALIGN_BOTTOM_MID, 0, -MORE_HINT_TRAVEL / 2);
	lv_obj_set_clickable(more_hint, false);
	lv_obj_set_floating(more_hint, true);
	lv_obj_set_hidden(more_hint, true);
	lv_obj_add_event_cb(music_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
