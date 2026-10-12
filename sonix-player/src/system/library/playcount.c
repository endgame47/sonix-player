#include "playcount.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "src/system/lastfm/lastfm.h"
#include "src/system/library/library.h"

// A longer step between two calls than this is the device having been asleep,
// not listening.
#define STEP_MAX_SECS 5.0

// Back at the start of the same file, after being this far into it: played
// again. The same test Last.fm's counting makes.
#define AGAIN_NEAR_START_SECS 5.0
#define AGAIN_BACK_SECS 10.0

// GUI thread only.
static struct {
	char file[512];
	bool skip;
	bool counted;
	bool was_playing;
	double listened;
	double last_tick;
	double last_position;
} play;

static double now_seconds(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void playcount_note_playback(const playcount_playback_t *now) {
	double t = now_seconds();
	const char *file = now->file ? now->file : "";
	bool same = play.file[0] && strcmp(play.file, file) == 0;
	bool again = same && now->position < AGAIN_NEAR_START_SECS && now->position + AGAIN_BACK_SECS < play.last_position;

	if (now->playing && file[0] && (!same || again)) {
		snprintf(play.file, sizeof(play.file), "%s", file);
		play.skip = now->skip;
		play.counted = false;
		play.was_playing = true;
		play.listened = 0;
		play.last_tick = t;
		play.last_position = now->position;
		return;
	}
	if (!same) {
		play.file[0] = '\0';
		return;
	}

	if (now->playing && play.was_playing) {
		double step = t - play.last_tick;
		if (step > 0 && step < STEP_MAX_SECS) {
			play.listened += step;
		}
	}
	play.was_playing = now->playing;
	play.last_tick = t;
	play.last_position = now->position;

	if (!play.counted && !play.skip && lastfm_play_counts(now->duration, play.listened)) {
		play.counted = true;
		library_note_play(play.file);
	}
}
