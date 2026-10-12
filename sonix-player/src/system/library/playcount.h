#ifndef PLAYCOUNT_H
#define PLAYCOUNT_H

#include <stdbool.h>

// ---------------------------------------------------------------------------
// How many times each track has been played, for the Most played list.
//
// A play counts by Last.fm's rule (lastfm_play_counts): a track of at least 30
// seconds, played for half its length or for four minutes. Only time actually
// playing counts -- not a pause, not the device asleep -- and a track started
// again, by repeat-one or by hand, is a new play. Counted whether Last.fm is in
// use or not, and written to the library (library_note_play), which keeps it
// only for tracks it has indexed.
// ---------------------------------------------------------------------------

typedef struct {
	bool playing;
	const char *file; // the loaded file, "" for none
	double position;  // seconds
	double duration;  // seconds, 0 when unknown
	bool skip;		  // a station, an audiobook or a podcast: never counted
} playcount_playback_t;

// GUI thread, about once a second, with what is playing now.
void playcount_note_playback(const playcount_playback_t *now);

#endif // PLAYCOUNT_H
