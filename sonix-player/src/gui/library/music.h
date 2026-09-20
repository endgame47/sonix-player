#ifndef MUSIC_H
#define MUSIC_H

#include "src/gui/shell/gui.h"

#include "src/misc/lv_types.h"

// The Music page: the library's index tiles (all tracks, albums, artists,
// album artists, genres) plus the file browser, with library settings,
// playlists, favourites and search on the corner buttons.
extern lv_obj_t *music_screen;

void music_init(gui_config_t *cfg);

// Puts Browse and Playlists where the "playlists first" option says they go:
// the sixth tile and the second corner button swap picture, caption and
// destination. Called at startup and whenever the option changes.
void music_refresh_layout(void);

#endif /* MUSIC_H */
