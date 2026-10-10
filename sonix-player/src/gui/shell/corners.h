#ifndef CORNERS_H
#define CORNERS_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

// Rounded screen corners: four black quarter-circle masks, one in each corner
// of the display, on the system layer so they sit above every page, sheet,
// dialog and the screensaver alike. They take no touches.
//
// A setting on the Appearance page ("ui"/"rounded_corners", off by default).

void corners_init(gui_config_t *cfg);

bool corners_enabled(void);
void corners_set_enabled(bool enabled);

#endif
