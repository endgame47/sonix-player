#ifndef PODCASTSUBS_H
#define PODCASTSUBS_H

#include <stdbool.h>

#include "src/system/streaming/podcast.h"

// Followed podcasts, kept on the card.
//
// Not held by the service, unlike Qobuz and Tidal favourites: Podcast Index has
// no accounts. It is only a catalogue -- searched and read, never logged into
// -- so the list of what is followed belongs to this device and lives on the
// card with everything else.
//
// The list is <card>/.local/podcast.db, an SQLite database like the library and
// audiobook ones; an older podcast-subs.ini, if still present, is imported on
// first start and set aside. The feed id is stored rather than the name, so a
// podcast that changes its title stays the right one; the title and artwork
// held here exist only to avoid querying the catalogue before the list can be
// drawn.
//
// The maximum is a real cap, not a convenience: the list stays in memory for
// the life of the process, and a corrupt or hand-inflated file must not be able
// to eat the RAM playback needs.

#define PODCASTSUBS_MAX 100

// Sets the card root and reloads the list. NULL or empty clears it.
void podcastsubs_set_root(const char *sd_root);

// Reads the list again when podcast.db was deleted or replaced since it was
// last read or written. The list lives in memory, so without this a deleted
// database went on showing its podcasts until a restart.
void podcastsubs_reload_if_replaced(void);

// How many are followed, and the list itself: most recently followed first.
int podcastsubs_count(void);
int podcastsubs_list(podcast_feed_t *out, int max);

bool podcastsubs_is_followed(long long feed_id);

// Follow or unfollow. Saves immediately: powering the player off an instant
// after tapping the star must not lose the change.
void podcastsubs_follow(const podcast_feed_t *feed);
void podcastsubs_unfollow(long long feed_id);

// Follows several at once with a single write: the OPML import. Ones already
// followed are skipped, and nothing is dropped to make room -- the list stops
// at PODCASTSUBS_MAX. Returns how many were added.
int podcastsubs_follow_batch(const podcast_feed_t *feeds, int count);

// The followed podcasts whose feed address is not known yet: followed from the
// player's star, which knows only the id, or before the address was kept.
// Fills `ids_out` and returns how many.
int podcastsubs_missing_urls(long long *ids_out, int max);

// Records a podcast's feed address, once the catalogue has given it.
void podcastsubs_set_url(long long feed_id, const char *url);

// Whether a podcast with this feed address is followed.
bool podcastsubs_is_followed_url(const char *url);

// What a newer record of a followed podcast knows that the list does not: how
// many episodes it has, and its feed address, author and artwork where the list
// has none. Nothing for one not followed.
void podcastsubs_update(const podcast_feed_t *feed);

// The followed podcasts without a feed address or an episode count, for the
// page to ask the catalogue about. Fills `ids_out` and returns how many.
int podcastsubs_missing_details(long long *ids_out, int max);

// The address of a podcast read from its feed (podcastrss.h), kept in the same
// database: its id is a hash of the address and cannot be turned back into it,
// and its episodes on the card or half heard outlive the run it was found in.
// Kept for one searched by its address and for one no longer followed.
void podcastsubs_keep_address(long long feed_id, const char *url);
bool podcastsubs_address(long long feed_id, char *out, size_t size);

#endif /* PODCASTSUBS_H */
