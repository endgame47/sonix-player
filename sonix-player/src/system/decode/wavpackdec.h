#ifndef WAVPACK_DECODE_H
#define WAVPACK_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// WavPack: .wv files.
//
// Like Opus, this library is not borrowed from the firmware -- the device does
// not ship it at all -- but statically compiled into the binary by the Makefile.
//
// What those hundred and fifty kilobytes buy:
//
//   * lossless up to 32-bit integer and floating point;
//   * hybrid mode, i.e. a lossy .wv at fixed bitrate plus a .wvc correction
//     file beside it that restores lossless -- when the .wvc is present it is
//     picked up automatically, with no involvement from the caller;
//   * DSD inside WavPack, which the library decimates to 24 bit. A DSD .wv
//     therefore plays, though by that route and not as DoP.
//
// Samples always come out as int32 right-justified at the file's real depth;
// this wrapper shifts them left for the player's 32-bit path, as dr_flac and
// libsndfile already do.

typedef struct wavpackdec wavpackdec_t;

// NULL when the file cannot be opened; the reason goes to the log.
wavpackdec_t *wavpackdec_open(const char *filepath);
void wavpackdec_close(wavpackdec_t *w);

int wavpackdec_channels(const wavpackdec_t *w);
int wavpackdec_sample_rate(const wavpackdec_t *w);
int wavpackdec_bits(const wavpackdec_t *w); // the file's depth, 24 for float
uint64_t wavpackdec_total_frames(const wavpackdec_t *w);

// Interleaved frames. The 32-bit form is left-justified, like every other
// decoder here, so a 24-bit .wv reaches the DAC with all twenty-four bits.
uint64_t wavpackdec_read_s16(wavpackdec_t *w, uint64_t frames, short *out);
uint64_t wavpackdec_read_s32(wavpackdec_t *w, uint64_t frames, int32_t *out);

bool wavpackdec_seek(wavpackdec_t *w, uint64_t frame);

#endif /* WAVPACK_DECODE_H */
