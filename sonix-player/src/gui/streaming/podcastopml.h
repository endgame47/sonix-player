#ifndef PODCASTOPML_H
#define PODCASTOPML_H

#include "lvgl.h"
#include "src/gui/shell/gui.h"

// Following podcasts from an OPML file, and writing the followed ones to one.
//
// Import looks for .opml files in the card's root and in its Podcast folder,
// the same two places the playlist import looks. Each feed in the chosen file
// is looked up in the catalogue by its address, and the ones it has can be
// followed in one go. The file itself is never changed.
//
// Export writes the followed podcasts to <card>/Podcast/Sonix.opml, for
// another app or another player.

void podcastopml_init(gui_config_t *cfg);

// Opens the import: straight on the file when there is only one.
void podcastopml_open_import(void);

// Writes the export and says how it went.
void podcastopml_export(void);

#endif /* PODCASTOPML_H */
