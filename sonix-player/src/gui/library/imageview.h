#ifndef IMAGEVIEW_H
#define IMAGEVIEW_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// A picture on a page of its own: the file manager opens JPEG, PNG and GIF
// files here (the first frame of a GIF). The picture is fitted whole into the
// page below the heading, never enlarged, and decoded on a thread of its own
// so a large photo does not hold up the interface; the page is up at once,
// with a spinner until the picture is ready.

extern lv_obj_t *imageview_screen;

void imageview_init(gui_config_t *cfg);

// Switches to the page and starts decoding `path`. When the file turns out
// not to be a picture this player can decode, the page says so instead.
void imageview_open(const char *path);

#endif /* IMAGEVIEW_H */
