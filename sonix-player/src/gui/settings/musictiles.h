#ifndef MUSICTILES_H
#define MUSICTILES_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Music settings > Display options > Library > Rearrange main page: the Music
// page's tiles, arranged by dragging them. The tiles on the page are the list
// on top, in their order; the ones left out are the list underneath. A row is
// dragged between the two to take a tile off the page or put it back, and
// within the first to move it. The order itself lives in music.c, which owns
// the tiles; this page is only the way to edit it.

extern lv_obj_t *musictiles_screen;

void musictiles_init(gui_config_t *cfg);

#endif /* MUSICTILES_H */
