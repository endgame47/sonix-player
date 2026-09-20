#ifndef GRIDPAGE_H
#define GRIDPAGE_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The tiled page layout, shared by the main menu and the Music section so the
// two look and behave identically.

typedef struct {
	const char *label;
	const lv_image_dsc_t *icon;

	// Where the tile goes. NULL for a section that does not exist yet: the
	// tile is dimmed and says so when tapped, rather than silently doing
	// nothing.
	lv_obj_t **target;

	// Alternative to `target` for tiles whose destination needs preparing
	// before it can be shown (the library index pages load their list first).
	// Checked only when `target` is NULL; entries that set neither are the
	// dimmed not-yet-available tiles.
	void (*action)(void);
} grid_entry_t;

// Fills `screen` with a `columns` x `rows` grid of tiles sized from the config.
// The grid is also a surface the player can be dragged in from, including from
// the tiles themselves.
// `clear_corner_buttons` starts the grid below the floating back button and
// any icon in the opposite corner, so the cards never sit under them.
// Repaints one tile in place: its picture and its caption. For the page whose
// tiles are not fixed -- Music, where Browse and Playlists trade places -- so
// the grid does not have to be built again to swap two of them.
void gridpage_set_tile(lv_obj_t *grid, int index, const lv_image_dsc_t *icon, const char *label);

lv_obj_t *gridpage_build(lv_obj_t *screen, gui_config_t *cfg, const grid_entry_t *entries, int count, int columns, int rows,
					bool clear_corner_buttons);

#endif /* GRIDPAGE_H */
