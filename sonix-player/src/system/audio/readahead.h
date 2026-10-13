#ifndef READAHEAD_H
#define READAHEAD_H

#include <stdbool.h>

// ---------------------------------------------------------------------------
// The next track, read off the card before it is needed.
//
// Gapless playback keeps the PCM open at the end of a track with a little
// under a second of audio still queued in it, and the next track has to be
// opened and decoded into it before that runs out. Opening a file reads its
// tags, its headers and its first frames off the card -- a few milliseconds
// when the card has been read recently, but on a file never read before, with
// covers being decoded beside it, enough to miss the queue and leave a gap.
//
// So while a track plays, the parts of the next one that opening it reads are
// read here first, into the kernel's page cache: the start of the file up to
// and a little past where its audio begins, its end (where many formats keep
// tags or an index), and an MP4's moov atom wherever it is. Nothing read here
// is used directly: the decoder and the tag reader open the file exactly as
// they always do and find those pages already in memory. That is what makes it
// work the same for every format without touching any of them.
//
// One file at a time, the newest asked for wins, on a thread below the
// interface.
// ---------------------------------------------------------------------------

// The cover embedded in a track is only read to compare it with the one on
// screen. `known` answers, for a path, whether that comparison can already be
// made without reading it -- and then the picture is not read ahead. Called on
// the read-ahead's own thread. Unset, every picture is read.
void readahead_set_picture_known(bool (*known)(const char *path));

// Reads ahead the parts of `path` that opening it will need. Returns at once;
// asking for the file already read, or being read, does nothing. Anything
// that is not a plain file (a track of a CUE sheet, a stream) is ignored.
void readahead_file(const char *path);

#endif // READAHEAD_H
