#ifndef PODCASTSTATE_H
#define PODCASTSTATE_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/podcast.h"

// Where each podcast episode was left, and whether it has been heard.
//
// One row per episode in <card>/.local/podcast.db, next to the followed
// podcasts. An episode the catalogue knows is identified by its episode id,
// whether it was streamed into the cache or kept in the Podcast folder: the
// two are the same episode, and stopping one halfway must leave the other
// halfway too. Any other file in the Podcast folder -- the ones copied there
// by hand -- is identified by its path under the card.
//
// The row also holds the title, the podcast's name, its id and its artwork,
// as they were when the episode last played: "Continue listening" draws from
// them without asking the catalogue, and an episode streamed into the cache
// is gone from the card by the next boot.

typedef struct {
	char path[512];						// absolute: its file in the Podcast folder when it has one,
										// otherwise the file it last played from; "" if unknown
	char title[PODCAST_TITLE_MAX];
	char show[PODCAST_TITLE_MAX];		// the podcast's name
	char image[PODCAST_URL_MAX];		// the podcast's artwork address, for an episode of the catalogue
	long long feed_id;					// 0 for a file copied by hand
	long long episode_id;				// 0 for a file copied by hand
	double position;					// seconds; 0 when finished or never started
	double duration;					// seconds; 0 when not known yet
	bool played;
	long long touched;					// grows at every write: higher is more recent
} podcaststate_t;

// The `touched` of the episode playing now, which is newer than anything
// written.
#define PODCASTSTATE_TOUCHED_NOW 0x7fffffffffffffffLL

// Where the card is. NULL or empty forgets it. Once at startup and on every
// card change, like podcastsubs_set_root().
void podcaststate_set_root(const char *sd_root);

// The player's note of how far an episode has got, from the GUI thread on
// every refresh. Throttled: written every fifteen seconds while playing, and
// at once with `force` (a pause, a seek, a change of track). The writing is done
// by a thread of its own, so a slow card does not hold up the interface.
//
// `total` is the episode's length, and is what decides that an episode has
// been heard: it is marked so once it gets within podcaststate_margin() of the
// end, and its position goes back to zero.
void podcaststate_note(const char *path, double seconds, double total, bool force);

// Writes the episode playing now through, before returning. The power going
// off, the card going away.
void podcaststate_flush(void);

// Where `path` should start: the second it was left at, or 0 when it was never
// started, was heard to the end, or was left in its first seconds.
double podcaststate_resume_point(const char *path);

// The state of one episode, by file or by catalogue id. False when it has
// none: never played.
bool podcaststate_get(const char *path, podcaststate_t *out);
bool podcaststate_get_episode(long long episode_id, podcaststate_t *out);

// Marks an episode heard or not, from its menu. Heard puts its position back to
// zero; not heard leaves the position as it was.
void podcaststate_set_played(const char *path, bool played);

// Its file has been deleted. A file copied by hand is forgotten; an episode of
// the catalogue keeps its state and only loses the file.
void podcaststate_forget(const char *path);

// An episode of the catalogue that has a file in the Podcast folder: its row,
// when it has one, points there from now on. Called when a download completes
// and for files saved before that was done.
void podcaststate_downloaded(long long episode_id, const char *path);

// Every row whose file is inside `folder` (absolute), and every row of the
// podcast `feed_id`. The callback runs once per row, on the caller's thread.
// Return how many rows were delivered.
typedef void (*podcaststate_cb_t)(const podcaststate_t *state, void *user);
int podcaststate_in_folder(const char *folder, podcaststate_cb_t cb, void *user);
int podcaststate_for_feed(long long feed_id, podcaststate_cb_t cb, void *user);

// The episodes left halfway, most recent first: what "Continue listening"
// offers. Returns how many were put in `out`.
int podcaststate_in_progress(podcaststate_t *out, int max);

// Marks an episode of the catalogue heard or not, from its row in a podcast's
// page, without a file to name it by: it may never have been downloaded.
// `image` is the podcast's artwork, for "Continue listening".
void podcaststate_set_played_episode(const podcast_episode_t *episode, const char *image, bool played);

// A number that changes whenever a state is written: a page showing states
// draws them again when it moves.
unsigned podcaststate_revision(void);

// How close to its end an episode counts as heard, in seconds: 30, 60 or 120.
// [podcast] played_margin.
int podcaststate_margin(void);
void podcaststate_set_margin(int seconds);

#endif /* PODCASTSTATE_H */
