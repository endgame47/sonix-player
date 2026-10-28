#ifndef EQSETTINGS_H
#define EQSETTINGS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The equalizer's presets, behind the gear in its corner:
// saving the current curve under a name, the user's own, and the ready-made
// ones.
//
// The user's presets are one small file each in <card>/.local/eq, so they can
// be copied off the device and back.

void eqsettings_init(gui_config_t *cfg);

// Opens the page, read again from the card.
void eqsettings_open(void);

// Called after a preset is applied or saved, so the equalizer page can put its
// bands where the new gains are.
void eqsettings_set_reload_cb(void (*cb)(void));

#endif /* EQSETTINGS_H */
