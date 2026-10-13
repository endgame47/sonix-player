#ifndef PODCAST_H
#define PODCAST_H

#include <stdbool.h>
#include <stddef.h>

// Podcast Index, the catalogue behind the Podcasts section.
//
// Why this and not the RSS feeds directly: a podcast is an RSS feed, and in
// theory downloading it would be enough. In practice it is not, for two
// reasons:
//
//   * search. Without a catalogue, the only way to find a podcast is to know
//     its URL by heart and type it on an on-screen keyboard. Nobody does that.
//   * the format. Podcast RSS is XML with four namespaces on it (itunes:,
//     podcast:, media:, content:), and the wild contains everything -- feeds
//     with broken XML, dates in five formats, duplicated tags. This tree has no
//     XML parser, and writing one that survives the real world is a far bigger
//     job than it looks.
//
// Podcast Index does both jobs: it searches, and it returns already normalized
// JSON, for which a parser is already here. It is also an open catalogue with
// no advertising, which in 2026 is not a given.
//
// The signature: every request carries three headers -- the key, the time, and
// the SHA-1 of key+secret+time in hex. The time must be the real one to within
// a few minutes, and this device loses the clock at every power-off and gets it
// back from NTP at boot, so without network the time is wrong and the catalogue
// answers 401. See podcast_last_error(), which recognizes that case and says so
// plainly instead of leaving "401".
//
// Everything in here blocks. Call only from the page's worker thread, never
// from the UI thread.

#define PODCAST_TITLE_MAX 160
#define PODCAST_AUTHOR_MAX 120
#define PODCAST_URL_MAX 512
#define PODCAST_PAGE_LIMIT 60
// The feed's own address, the RSS. Shorter than the other addresses: it is
// kept for every followed podcast and in every list of podcasts, and a feed
// address is rarely a third of this.
#define PODCAST_FEED_URL_MAX 384

// The ids are 64-bit, and not out of generosity: on this device `long` is
// 32-bit, and Podcast Index numbers episodes with eleven digits (16795090626
// and the like). With a 32-bit `long` strtol does not get such a number wrong,
// it saturates -- every episode collapses onto 2147483647, sharing one cache
// file and one sidecar row. Feed ids are small and would fit, but they travel
// with episode ids in too many places to be worth distinguishing.

// A podcast: the feed, not an episode.
typedef struct {
	long long id; // the Podcast Index id, which is how it is found again
	char title[PODCAST_TITLE_MAX];
	char author[PODCAST_AUTHOR_MAX];
	char image[PODCAST_URL_MAX];
	char url[PODCAST_FEED_URL_MAX]; // the RSS feed; "" when not known
	int episode_count;				 // 0 when the catalogue does not say
} podcast_feed_t;

typedef struct {
	long long id;
	long long feed_id;
	char title[PODCAST_TITLE_MAX];
	char feed_title[PODCAST_TITLE_MAX];
	char image[PODCAST_URL_MAX];
	char enclosure[PODCAST_URL_MAX]; // the audio itself
	char mime[64];					 // the declared type, for the extension
	long published;					 // unix time, 0 when unknown
	int duration_secs;				 // 0 when unknown
} podcast_episode_t;

// Whether a key pair is present. Without one the section disables itself and
// says so.
bool podcast_configured(void);

// Why the last call failed, already translated for display.
const char *podcast_last_error(void);

// How many entries the catalogue sent in the last response, before the
// unusable ones were discarded: this is the number that says whether the
// response filled the requested limit, and hence whether asking for more is
// worthwhile.
int podcast_last_raw_count(void);

// Searches podcasts by name. Returns how many it put in `out`, -1 on error.
int podcast_search(const char *query, podcast_feed_t *out, int max);

// The current chart.
int podcast_trending(podcast_feed_t *out, int max);

// A podcast's episodes, most recent first.
int podcast_episodes(long long feed_id, podcast_episode_t *out, int max);

// A single podcast, by id. Needed by "My podcasts", which stores ids rather
// than titles, so a podcast that is renamed stays the right one.
bool podcast_feed(long long feed_id, podcast_feed_t *out);

// A podcast found by the address of its feed, the way an OPML file names it.
// 1 when found, 0 when the catalogue does not have it, -1 when the catalogue
// could not be asked (podcast_last_error() says why).
int podcast_feed_by_url(const char *url, podcast_feed_t *out);

// A single episode. Needed by the player, which has the id from the sidecar and
// wants to know which podcast it belongs to.
bool podcast_episode(long long episode_id, podcast_episode_t *out);

// ---------------------------------------------------------------------------
// The two skip buttons, which on podcasts are their own and not the audiobook
// ones.
//
// The two cases look alike -- back because a sentence was missed, forward to
// jump an advert -- but are not listened to the same way: in an audiobook the
// back skip picks up the thread of a story, in a podcast the forward skip jumps
// a half-minute sponsor. Wanting -10/+30 on podcasts and -30/+10 on books
// should not be a choice: they are two settings, in two different sections of
// the configuration file.
#define PODCAST_SKIP_SHORT 10
#define PODCAST_SKIP_LONG 30
#define PODCAST_SKIP_HUGE 60

int podcast_skip_back(void);
int podcast_skip_forward(void);
void podcast_set_skip_back(int seconds);
void podcast_set_skip_forward(int seconds);

// Playback speed for episodes, in thousandths (1000 is normal), apart from the
// audiobook one in audiobook.h. Same range, [podcast] speed in the config.
int podcast_speed_permille(void);
void podcast_set_speed_permille(int permille);
double podcast_speed(void);

// Stop at the end of the episode: when this one finishes, the queue stays where
// it is instead of starting the next. The audiobook's chapter-end switch under
// another name, and for the same listener -- somebody following one thing at a
// time, who does not want the next hour starting on its own.
bool podcast_stop_at_episode_end(void);
void podcast_set_stop_at_episode_end(bool on);

#endif /* PODCAST_H */
