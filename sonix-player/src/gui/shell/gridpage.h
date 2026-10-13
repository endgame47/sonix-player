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
// Repaints one tile in place: its picture and its caption, so a page whose
// tiles change does not have to build its grid again.
void gridpage_set_tile(lv_obj_t *grid, int index, const lv_image_dsc_t *icon, const char *label);

lv_obj_t *gridpage_build(lv_obj_t *screen, gui_config_t *cfg, const grid_entry_t *entries, int count, int columns, int rows,
					bool clear_corner_buttons);

// Puts a different set of tiles on a grid gridpage_build() made, with the same
// arguments as it took: for a page whose tiles the user arranges.
void gridpage_set_entries(lv_obj_t *grid, gui_config_t *cfg, const grid_entry_t *entries, int count, int columns,
						  int rows, bool clear_corner_buttons);

// Lets a grid built with more entries than `columns` x `rows` scroll to the
// rest, the tiles keeping the size that grid gives them.
void gridpage_set_scrolling(lv_obj_t *grid);

// Shows the scrollbar of a grid that scrolls for a moment, so a page whose
// tiles run past the screen says so when it is opened. Nothing when the tiles
// all fit.
void gridpage_hint_scroll(lv_obj_t *grid);

// What a section page shows instead of its tiles while its index is empty: a
// picture, a line saying a scan is needed and a Scan button calling `scan_cb`.
// Hidden until gridpage_show_empty() is told otherwise.
lv_obj_t *gridpage_empty_panel(lv_obj_t *screen, gui_config_t *cfg, const lv_image_dsc_t *icon, const char *text,
							   lv_event_cb_t scan_cb);

// Either the tiles or the panel.
void gridpage_show_empty(lv_obj_t *grid, lv_obj_t *panel, bool empty);

// The same panel saying there is no card instead -- none in the slot, or the
// card is at a computer over USB -- with no Scan button: the index lives on
// the card, so there is nothing to scan. While it shows, the card is looked
// for every second; when it is back, `card_back` is called (the page decides
// again between its tiles and the scan panel).
void gridpage_show_no_card(lv_obj_t *grid, lv_obj_t *panel, void (*card_back)(void));

#endif /* GRIDPAGE_H */
