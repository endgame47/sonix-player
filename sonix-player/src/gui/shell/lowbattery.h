#ifndef LOWBATTERY_H
#define LOWBATTERY_H

#include "src/gui/shell/gui.h"

// The battery running out, on battery power only.
//
// At 5 % a notice says so, once per discharge. At 1 % the player switches
// itself off, after a ten second countdown on screen that a charger plugged in
// calls off. Left alone, the cell sags under load until the system resets, and
// the device boots straight back into the same empty cell, again and again; a
// shutdown through power_shutdown() leaves it off until the power key or a
// cable.
//
// The level is read every five seconds, also with the screen dark, and the
// countdown turns the screen on.

void lowbattery_init(gui_config_t *cfg);

#endif /* LOWBATTERY_H */
