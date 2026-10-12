#ifndef MUSIC_H
#define MUSIC_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Music page: the library's index tiles (all tracks, albums, artists,
// album artists, genres, years), the favourites, the most played, the playlists
// and the file browser, with music settings and search on the corner buttons.
extern lv_obj_t *music_screen;

void music_init(gui_config_t *cfg);

// The tiles the page can show. The order here is the one they start in.
typedef enum {
	MUSIC_TILE_ALL,
	MUSIC_TILE_ALBUMS,
	MUSIC_TILE_ARTISTS,
	MUSIC_TILE_ALBUM_ARTISTS,
	MUSIC_TILE_GENRES,
	MUSIC_TILE_YEARS,
	MUSIC_TILE_FAVOURITES,
	MUSIC_TILE_MOST_PLAYED,
	MUSIC_TILE_PLAYLISTS,
	MUSIC_TILE_BROWSE,
	MUSIC_TILE_COUNT,
} music_tile_t;

// The tiles on the page, in order, and the ones left out. Read by the page that
// arranges them (Music settings > Display options > Library > Rearrange main
// page), which moves them with the two calls below; every move is saved and the
// page's tiles are rebuilt.
int music_tiles_in_use_count(void);
music_tile_t music_tiles_in_use_at(int position);
int music_tiles_hidden_count(void);
music_tile_t music_tiles_hidden_at(int position);

// Puts `tile` at `position` among the tiles on the page, from wherever it was.
void music_tiles_move_in_use(music_tile_t tile, int position);

// Takes `tile` off the page, to `position` among the ones left out. False, and
// nothing moved, when it is the last tile on the page: one always stays.
bool music_tiles_move_hidden(music_tile_t tile, int position);

// What a tile is called (a translation tag) and its picture.
const char *music_tile_tag(music_tile_t tile);
const lv_image_dsc_t *music_tile_icon(music_tile_t tile);

// The page's tiles made again from the saved order.
void music_rebuild_tiles(void);

#endif /* MUSIC_H */
