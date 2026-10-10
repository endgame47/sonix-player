#ifndef APE_DECODE_H
#define APE_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Monkey's Audio: .ape files, file versions 3.80 to 3.99, 8, 16 and 24 bit,
// mono and stereo, every compression level. The codec is FFmpeg's, in
// ape/apecore.c; this file reads the container and streams the frames.
//
// A frame is read from the card a window at a time, so memory does not grow
// with the frame, and every frame is found through the seek table, so a damaged
// one costs its own few seconds (played as silence) and not the rest of the
// track.

typedef struct apedec apedec_t;

// NULL when the file cannot be opened or is not one this decoder plays; the
// reason goes to the log.
apedec_t *apedec_open(const char *filepath);
void apedec_close(apedec_t *a);

int apedec_channels(const apedec_t *a);
int apedec_sample_rate(const apedec_t *a);
int apedec_bits(const apedec_t *a);
uint64_t apedec_total_frames(const apedec_t *a);

// Interleaved frames. The 32-bit form is left-justified, like every other
// decoder here.
uint64_t apedec_read_s16(apedec_t *a, uint64_t frames, short *out);
uint64_t apedec_read_s32(apedec_t *a, uint64_t frames, int32_t *out);

// Exact to the frame: decoding restarts at the APE frame holding `frame` and
// what comes before it is decoded and dropped.
bool apedec_seek(apedec_t *a, uint64_t frame);

// Rate and depth from the header alone, for the library scan. False when the
// file is not an .ape this decoder would play.
bool apedec_probe(const char *filepath, int *rate, int *bits);

// APEv2 (or APEv1) tags at the end of the file, an .ape's or a .wv's: every
// text item named in the list inside apedec.c is handed to the callback with
// its tag name and its first value. Binary items are skipped unread.
void apedec_tags(const char *filepath, void (*fn)(void *user, const char *key, const char *value), void *user);

// Where the image in "Cover Art (Front)", else "Cover Art (Back)", lies in the
// file: past the file name and the NUL that start the item's value. Serves .wv
// files as well, which carry the same tag. False when there is none.
bool apedec_cover_at(const char *filepath, int64_t *offset, uint32_t *size);

#endif /* APE_DECODE_H */
