#ifndef PODCASTPAGE_H
#define PODCASTPAGE_H

#include "lvgl.h"
#include "src/gui/nowplaying/cover.h"
#include "src/gui/shell/gui.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcaststate.h"

void podcastpage_init(gui_config_t *cfg);

// The section's screen, for the streaming grid. There is only one: the two
// entries are pills -- like the audiobooks' all / recent / finished -- and a
// pill changes what is underneath rather than navigating elsewhere, so the
// list sits on the same screen.
extern lv_obj_t *podcast_screen;

// Where leaving the player returns to after a podcast was opened from there.
// The same object as podcast_screen; both names remain because they say
// different things to the reader.
extern lv_obj_t *podcast_list_screen;

// Opens a podcast's page. Called by "show podcast" in the track menu, which
// read the id from the sidecar file of the episode playing. False when it
// cannot be done (section disabled).
bool podcastpage_open_feed(long long feed_id, const char *title);

// The podcast settings page, which holds the podcast sleep timer: what the
// control centre's sleep button for podcasts opens on a long press.
lv_obj_t *podcastpage_settings_screen(void);

// Back to the section on the followed podcasts: where the OPML import ends.
void podcastpage_open_followed(void);

// ---------------------------------------------------------------------------
// A podcast of the catalogue, for its page (podcastcard.c). The page draws;
// the requests go through this page's worker, which is the one that talks to
// the network.
// ---------------------------------------------------------------------------

// The most a podcast's list holds, and the step it grows by.
#define PODCASTPAGE_FEED_MAX 1000
#define PODCASTPAGE_FEED_STEP 100

// Asks for the first `want` episodes of a podcast -- the same request with a
// higher limit is the next step of the same list. `with_info` asks for the
// podcast's own record too (artwork, author, feed address). The answer comes
// to podcastcard_feed_loaded() with the number returned here.
unsigned podcastpage_feed_load(long long feed_id, int want, bool with_info);

// The list of the last answer, on the GUI thread; NULL past its end.
int podcastpage_feed_count(void);
const podcast_episode_t *podcastpage_feed_episode(int index);
// The limit it was asked with, and whether the catalogue may have more.
int podcastpage_feed_want(void);
bool podcastpage_feed_more(void);

// The podcast the sidecar files are written for when its episodes are played
// or kept: what the player's star follows it with.
void podcastpage_set_browsing(const podcast_feed_t *feed);

// Plays episode `index` of the list, with the ones after it queued.
void podcastpage_feed_play(int index);
// Puts episode `index` after the one playing. False when it cannot.
bool podcastpage_feed_play_next(int index);
// Keeps episode `index` on the card, in the folder named after `feed_title`,
// with the card that follows the download.
void podcastpage_feed_download(int index, const char *feed_title);
// The same for several episodes of the list, one after the other behind the
// same card. Those already on the card are left out.
void podcastpage_feed_download_many(const int *indices, int count, const char *feed_title);
// An episode of the catalogue left halfway that is not in the list drawn:
// its podcast's list is asked for, and it plays with the ones after it.
void podcastpage_resume(const podcaststate_t *state);

// The cover at the head of a podcast's page, from its address.
void podcastpage_hero_request(const char *url, int size);
bool podcastpage_hero_take(cover_image_t *out, bool *finished);
void podcastpage_hero_release(void);

#endif /* PODCASTPAGE_H */
