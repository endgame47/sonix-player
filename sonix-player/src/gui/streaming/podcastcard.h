#ifndef PODCASTCARD_H
#define PODCASTCARD_H

#include <stdbool.h>

#include "lvgl.h"
#include "src/gui/shell/gui.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcaststate.h"

// A podcast's page, the same for the two places a podcast can come from:
//
//   * the card -- the Podcast folder, where Sonix keeps the episodes it
//     downloads and where anybody can copy their own. One podcast per folder;
//     files straight in the Podcast folder are one more podcast of their own.
//     Works with no network at all;
//   * the catalogue -- the episodes come from Podcast Index through the podcast
//     page's worker (podcastpage.c), a step at a time as the list is scrolled.
//
// Two screens, one per source, so a podcast on the card can open the same
// podcast in the catalogue and the back gesture returns to the first.

#define PODCASTCARD_MAX_SHOWS 128

// The coverloader slots the podcasts on the card use (see coverloader.h).
#define PODCASTCARD_SLOT_ROWS 91	   // twelve, for the rows of the list
#define PODCASTCARD_SLOT_HERO 103	   // the cover at the head of a podcast's page
#define PODCASTCARD_SLOT_LISTENING 104 // four, for the episodes being listened to

typedef struct {
	char name[256];		// the folder's name; "" for the files straight in Podcast
	char folder[600];	// absolute
	char first[600];	// one of its episodes: the file the cover is looked up from
	int episodes;
	int unheard;		// not heard to the end
	bool fresh;			// one or more never started
	bool downloaded;	// holds episodes Sonix downloaded (a .tags beside them)
	long long feed_id;	// the catalogue's podcast those downloads belong to; 0 when none
} podcastcard_show_t;

void podcastcard_init(gui_config_t *cfg);

// The two pages.
extern lv_obj_t *podcastcard_screen;
extern lv_obj_t *podcastcard_feed_screen;

// The podcasts on the card, alphabetically, the loose files last. Reads the
// card: call when a list is about to be drawn, not on every frame.
int podcastcard_shows(podcastcard_show_t *out, int max);

// The name a show is listed under: its folder's, or "Other episodes".
const char *podcastcard_show_name(const podcastcard_show_t *show);

// Opens a show's page.
void podcastcard_open(const podcastcard_show_t *show);

// Opens the page of a podcast of the catalogue. `feed` needs its id; whatever
// else it lacks (artwork, author) is asked of the catalogue.
void podcastcard_open_feed(const podcast_feed_t *feed);

// The catalogue's answer to the request podcastpage_feed_load() made, on the
// GUI thread: how many episodes (podcastpage_feed_count()), or -1 with the
// reason. `info` is the podcast's own record when it was asked for, else NULL.
void podcastcard_feed_loaded(unsigned ticket, int count, const char *error, const podcast_feed_t *info);

// How many episodes the card holds for the podcast of this title: the folder a
// download from the catalogue goes into. 0 when there is none.
int podcastcard_count_for_title(const char *title);

// The order a show's episodes are listed in: newest first unless this is on.
// [podcast] card_oldest_first.
bool podcastcard_oldest_first(void);
void podcastcard_set_oldest_first(bool on);

// Plays an episode that is on the card, with the episodes after it in its
// folder queued behind it, from where it was left. False when the file is not
// there (an episode streamed into a cache that has since been emptied).
bool podcastcard_play(const podcaststate_t *state);

#endif /* PODCASTCARD_H */
