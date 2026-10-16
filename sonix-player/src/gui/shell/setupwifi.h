#ifndef SETUPWIFI_H
#define SETUPWIFI_H

#include "src/gui/shell/gui.h"

// The Wi-Fi step of the first-boot setup, between the language and the date
// and time: a list of the networks in range to join one from, and a button to
// skip the step. Nothing else is on the page -- no status bar, no way back --
// as on the other setup pages.
//
// Joining one moves straight on to the date and time, where the network can
// set them (timeset_show_first_boot()). Skipping moves on too, with the radio
// left as it was found.

void setupwifi_init(gui_config_t *cfg);

// Brings the page up. On a device without Wi-Fi, or with a network already
// joined, it goes straight on to the date and time.
void setupwifi_show(void);

#endif /* SETUPWIFI_H */
