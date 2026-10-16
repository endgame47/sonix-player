#ifndef PLAYHISTORY_H
#define PLAYHISTORY_H

#include <stdbool.h>

// The history of what the queue has played, as the queue page shows it.
//
// A track goes into it when it is left: played to the end, skipped forwards or
// backwards, replaced by another track or by a station, or run out at the end
// of the queue. Every time, so a track heard twice is there twice. Audiobooks,
// podcast episodes and what a phone sends over DLNA are not tracks of the queue
// in this sense and are kept out.
//
// The entries are written to the library (library_history_add) the moment the
// track is left; nothing is held back in here but the track loaded now. Safe to
// call from any thread: a Qobuz or Tidal page starts its queue from a worker.

// A track has been loaded. `title` and `artist` are what it is to be shown by;
// `kept_out` says it is not to go into the history. With `waiting`, it only
// counts once it starts playing (playhistory_resumed): the track a reboot put
// back, paused, was not played again just by coming back.
void playhistory_started(const char *path, const char *title, const char *artist, bool kept_out, bool waiting);

// The track loaded has started playing after a pause.
void playhistory_resumed(void);

// The track loaded is left. Writes it down, once: a second call before the
// next track starts does nothing.
void playhistory_left(void);

#endif // PLAYHISTORY_H
