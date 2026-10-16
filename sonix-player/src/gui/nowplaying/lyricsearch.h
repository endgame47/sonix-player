#ifndef LYRICSEARCH_H
#define LYRICSEARCH_H

#include "src/gui/shell/gui.h"
#include "src/system/library/lrclib.h"

#include "lvgl/lvgl.h"

// The lyrics LRCLIB found for a track, to pick one from: a list of the records,
// likeliest first, each with its title, artist, album, length and whether it is
// timed. Tapping one shows its words in full, with Apply to save them beside
// the track; back from there returns to the list, and back from the list to the
// player.

extern lv_obj_t *lyricsearch_screen;

void lyricsearch_init(gui_config_t *cfg);

// Opens the list for the file at `path`, over the player. Takes the records,
// which are freed when the page is left.
void lyricsearch_open(const char *path, lrclib_record_t *records, int count);

#endif /* LYRICSEARCH_H */
