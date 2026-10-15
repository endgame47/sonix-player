#ifndef BOOTLOGOPAGE_H
#define BOOTLOGOPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Settings > Appearance > Boot logo: the theme's own logo, or one of the
// pictures in the Bootlogo folder of the microSD, fitted to the screen. What
// is stored where is bootlogo.h; this page is only the way to choose.

extern lv_obj_t *bootlogopage_screen;

void bootlogopage_init(gui_config_t *cfg);

#endif /* BOOTLOGOPAGE_H */
