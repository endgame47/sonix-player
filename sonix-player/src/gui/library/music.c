#include "music.h"

#include "lvgl/lvgl.h"

#include "src/gui/library/browser.h"
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
#include "src/system/core/lang.h"

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

// Browse and Playlists trade places.
//
// The tile is the sixth one and the corner button is the second: which of the
// two holds which is the "playlists first" option, and nothing else about
// either of them changes. So there is one opener for each position rather than
// one for each destination.
static lv_obj_t *tile_grid;
static lv_obj_t *playlists_btn;
static lv_obj_t *playlists_icon;

static void open_tile_slot(void) {
	if (musicsettings_playlists_first()) {
		playlistpage_open();
	} else {
		switch_screen(browser_screen);
	}
}

static void playlists_cb(lv_event_t *e) {
	(void)e;
	if (musicsettings_playlists_first()) {
		switch_screen(browser_screen);
	} else {
		playlistpage_open();
	}
}

static void search_cb(lv_event_t *e) {
	(void)e;
	search_open();
}

static void favourites_cb(lv_event_t *e) {
	(void)e;
	medialist_open(tr("favourites"), LIBRARY_LIST_FAVOURITES, LIBRARY_FILTER_NONE, NULL);
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
	const grid_entry_t entries[] = {
		{"music_all_tracks", &icon_menu_all, NULL, open_all_tracks},
		{"albums", &icon_menu_album, NULL, open_albums},
		{"artists", &icon_menu_artist, NULL, open_artists},
		{"music_album_artists", &icon_menu_album_artist, NULL, open_album_artists},
		{"music_genres", &icon_menu_genre, NULL, open_genres},
		// An opener rather than a target: what this tile is depends on the
		// option, and music_refresh_layout() paints it accordingly.
		{"music_browse", &icon_menu_explorer, NULL, open_tile_slot},
	};

	lv_obj_t *grid = gridpage_build(music_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, true);
	tile_grid = grid;

	// The pull from the bottom edge that opens the album carousel. It goes on
	// the grid because that is where a tile's press stops bubbling.
	coverflow_attach_edge(grid, cfg);

	// Four buttons in the corner, so the heading has to be told to clear them.
	settingsrow_title_corner_slots(settingsrow_title(music_screen, cfg, "music"), cfg, 4);

	lv_obj_t *settings_btn = corner_button(cfg, 0, &icon_music_settings);
	lv_obj_add_event_cb(settings_btn, switch_screen_cb, LV_EVENT_CLICKED, musicsettings_screen);

	playlists_btn = corner_button(cfg, 1, &icon_list_music);
	playlists_icon = lv_obj_get_child(playlists_btn, 0);
	lv_obj_add_event_cb(playlists_btn, playlists_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *favourites_btn = corner_button(cfg, 2, &icon_star_corner);
	lv_obj_add_event_cb(favourites_btn, favourites_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *search_btn = corner_button(cfg, 3, &icon_search);
	lv_obj_add_event_cb(search_btn, search_cb, LV_EVENT_CLICKED, NULL);

	music_refresh_layout();
}

void music_refresh_layout(void) {
	bool playlists_first = musicsettings_playlists_first();

	gridpage_set_tile(tile_grid, 5, playlists_first ? &icon_menu_playlist : &icon_menu_explorer,
					  playlists_first ? "playlists" : "music_browse");

	if (playlists_icon) {
		lv_image_set_src(playlists_icon, playlists_first ? &icon_folder_corner : &icon_list_music);
	}
}
