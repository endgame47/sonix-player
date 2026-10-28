#ifndef EQPAGE_H
#define EQPAGE_H

#include <stdbool.h>

#include "lvgl/lvgl.h"
#include "src/gui/shell/gui.h"

// The graphic equalizer's page (eq.h): the ten bands as points on the curve
// they make, dragged up and down or stepped half a dB at a time, the bands'
// width, and what the engine takes off the level so the curve cannot clip.

void eqpage_init(gui_config_t *cfg);
lv_obj_t *eqpage_screen(void);

// Puts everything where the engine's settings are now: after a preset, an
// output change, or the switch flipped from outside the page.
void eqpage_refresh(void);

#endif /* EQPAGE_H */
