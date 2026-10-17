#ifndef CHARGESCREEN_H
#define CHARGESCREEN_H

#include <stdbool.h>

// The charging screen of a player that is off on its cable, as the stock
// player has it: a battery with its level, lit for a while and again at a
// short press of the power key. Holding the key starts the player; pulling the
// cable powers the device off.
//
// The PMIC (AXP2101) powers the device on when a cable goes in, and the boot
// then reaches the player like any other. What tells the two starts apart is
// the PMIC's power-on status (REG20h, bit 0 the key) with the cable in, and one
// byte of its data buffer (REG07h), which keeps its value across a restart for
// as long as the PMIC has power. On the R3 Pro II the cable goes to the MP2731
// charger and the status reads the key either way: there a start with the
// cable in and the key not held is a charge. The mark:
//
//     CHARGESCREEN_MARK_NONE       powered off: a start by the cable is a charge
//     CHARGESCREEN_MARK_STARTED    the player is running, or has been: a restart
//                                  of it is not a charge
//     CHARGESCREEN_MARK_REQUESTED  powered off with the cable in: the restart
//                                  that follows is the charging screen
//
// The screen is drawn straight to the framebuffer, before anything of the
// player has started: no LVGL, no audio, no radio.

#define CHARGESCREEN_MARK_NONE 0
#define CHARGESCREEN_MARK_STARTED 1
#define CHARGESCREEN_MARK_REQUESTED 2

// Shows the charging screen when this start is a charge, until the power key
// is held (then it returns, and the player carries on starting) or the cable
// is pulled (then it powers the device off and does not return). Returns at
// once otherwise. Call before anything else has opened the display or the
// input devices. A no-op on the host.
void chargescreen_run_if_wanted(void);

// Writes the mark. False when the PMIC could not be written, and always on the
// host.
bool chargescreen_mark(int mark);

#endif /* CHARGESCREEN_H */
