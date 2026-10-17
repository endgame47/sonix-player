#ifndef RADIOFLAC_H
#define RADIOFLAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A FLAC station: native FLAC, or FLAC in Ogg, from a source that only goes
// forward.
//
// Ogg is taken apart here rather than by dr_flac, which follows one logical
// stream and goes silent at the next link of a chained stream. Icecast servers
// chain one link per song, each opening with its own header and its own
// comments, and the comments are where the song's title travels. Every link is
// handed to dr_flac as a native stream of its own.
//
// A native stream joined midway, with no header in front, is opened from its
// first whole frame.

typedef struct radioflac radioflac_t;

// Fills `dst` with up to `max` bytes, blocking until there are some. 0 when
// the source has ended or been stopped.
typedef int (*radioflac_read_fn)(void *user, unsigned char *dst, int max);

// Nothing is read until the first radioflac_read(). NULL when out of memory.
radioflac_t *radioflac_open(bool ogg, radioflac_read_fn read, void *user);

// Decodes up to `max_frames` sample frames into `pcm`, interleaved and
// left-justified in 32 bits whatever the source's depth, so `pcm` needs room
// for max_frames * 8 samples. The format can change between two calls (a new
// link of a chain): radioflac_format() says what the last call returned.
//
// 0 when the stream has ended; -1 when nothing in it could be opened as FLAC.
int radioflac_read(radioflac_t *f, int32_t *pcm, int max_frames);

// What the last radioflac_read() returned.
void radioflac_format(const radioflac_t *f, int *rate, int *channels, int *bits);

// The song the stream's own comments name, as "Artist - Title", once each time
// it changes. False when it has not changed since the last call.
bool radioflac_take_title(radioflac_t *f, char *out, size_t size);

void radioflac_close(radioflac_t *f);

#endif /* RADIOFLAC_H */
