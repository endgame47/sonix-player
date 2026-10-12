#ifndef ART_SHRINK_H
#define ART_SHRINK_H

#include <stddef.h>
#include <stdint.h>

// Cover art made small enough to send over a slow link.
//
// Embedded covers are often one to five megabytes: a poster-sized JPEG, or a
// PNG. Over Bluetooth that is seconds per track, all of it on the same radio
// as the headphones' audio. This decodes the picture at a reduced scale (the
// same decoders the interface draws covers with, so a large file never costs
// its full resolution in memory), fits it within `max_side` on its longest
// edge, and encodes the result as a JPEG of `quality`.
//
// Returns a malloc'd JPEG and its length, or NULL when the picture cannot be
// decoded or is already small enough to send as it is -- the caller then sends
// the original.
uint8_t *art_shrink_to_jpeg(const uint8_t *data, size_t size, int max_side, int quality, size_t *out_len);

// An RGB888 picture of `w` x `h` encoded as a JPEG of `quality` (1..100; above
// 90 the colour is kept at full resolution). Returns a malloc'd JPEG and its
// length, or NULL when the encoder fails or runs out of memory.
uint8_t *art_encode_jpeg(const uint8_t *rgb, int w, int h, int quality, size_t *out_len);

#endif /* ART_SHRINK_H */
