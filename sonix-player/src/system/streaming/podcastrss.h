#ifndef PODCASTRSS_H
#define PODCASTRSS_H

#include <stdbool.h>
#include <stddef.h>

#include "src/system/streaming/podcast.h"

// Podcasts read straight from their RSS feed, for the ones the catalogue does
// not have: an address typed into the search, or a feed of an OPML file that
// Podcast Index does not know.
//
// Such a podcast and its episodes get ids of their own, made from the feed's
// address and the episode's guid, in a range far above any the catalogue hands
// out. With those ids the rest of the section -- the podcast's page, the
// cache, the downloads, where each episode was left -- treats them exactly as
// it treats the catalogue's; podcast.c sends the requests for these ids here.
//
// The reader takes RSS 2.0 as podcasts publish it: the channel's title, author
// and artwork, and per item the title, the enclosure, the guid, the date and
// the length. Anything it does not recognise is skipped.
//
// Everything here blocks: call it from a worker, never from the UI thread.

// The ids from here up are this reader's.
#define PODCASTRSS_ID_BASE 0x4000000000000000LL

bool podcastrss_is_id(long long id);

// The id of the podcast at `url`.
long long podcastrss_feed_id(const char *url);

// Where the podcast `feed_id` is read from, kept for this run: every feed read
// is remembered, and the followed ones are told at startup. False when the
// address is not known; podcast.c then asks podcastsubs_address(), which keeps
// them across restarts.
void podcastrss_remember(long long feed_id, const char *url);
bool podcastrss_url_for(long long feed_id, char *out, size_t size);

// Reads the feed at `url` into `feed`. False with podcast_last_error() set when
// it cannot be fetched or is not a podcast feed.
bool podcastrss_read_feed(const char *url, podcast_feed_t *feed);

// The same calls as podcast.h's, for an id of this reader. The last feed read
// is kept for a few minutes, so a list asked for again with a higher limit
// does not fetch it again.
int podcastrss_episodes(long long feed_id, podcast_episode_t *out, int max, int *raw_count);
bool podcastrss_feed(long long feed_id, podcast_feed_t *out);
bool podcastrss_episode(long long feed_id, long long episode_id, podcast_episode_t *out);

#endif /* PODCASTRSS_H */
