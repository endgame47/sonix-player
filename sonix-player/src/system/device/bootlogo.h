#ifndef BOOTLOGO_H
#define BOOTLOGO_H

#include <stdbool.h>

// The picture shown while the player boots: which one, and the user's own.
//
// It is drawn by `/etc/init.d/S11jpeg_display_shell`, long before the player
// runs and before any filesystem holding a configuration is mounted. The
// choice therefore lives in raw NAND, in a block of /dev/mtd5 that holds
// nothing else, as 13 bytes at 0x20000:
//
//     theme:T,aA,cC
//
//   T  1 light, 2 dark
//   A  the accent, 0-5, the index into theme.c's ACCENT_COLORS
//   C  1 when BOOTLOGO_CUSTOM_FILE is the logo to show
//
// The first seven bytes are the stock firmware's own marker, which its script
// reads as "theme:1" or "theme:2". The script shows
// /etc/boot-logo/<light|dark>-theme-<colour>.jpeg, or the custom file when C
// is 1; with C at 1 it also mounts userdata early so the file is there in time.
//
// The marker is written with the stock tools, `flash_erase` on the block and
// `nandwrite` of one page, on a thread of its own, and only when it differs
// from what is on the flash: NAND has a finite number of erase cycles.

// The user's own logo, on the userdata partition.
#define BOOTLOGO_CUSTOM_FILE "/usr/data/bootlogo.jpeg"

// Brings the marker in line with the theme, the accent and whether a custom
// logo exists. Returns at once; the flash work happens on a worker. A no-op on
// a device without /dev/mtd5 and on the host build.
void bootlogo_follow(bool dark, int accent);

// Whether a custom logo is set.
bool bootlogo_has_custom(void);

// Makes `image_path` (a JPEG or a PNG) the boot logo: scaled to cover
// `width` x `height`, cropped to it around the centre, and saved as a baseline
// JPEG at BOOTLOGO_CUSTOM_FILE. Blocks for the decode and the encode, so it is
// called off the UI thread. False when the picture cannot be read or the file
// cannot be written; the previous logo is then left as it was.
bool bootlogo_make_custom(const char *image_path, int width, int height);

// Back to the theme's logo: deletes the custom file.
void bootlogo_clear_custom(void);

#endif /* BOOTLOGO_H */
