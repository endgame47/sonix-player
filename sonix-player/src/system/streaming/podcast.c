#include "podcast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/system/core/config.h"
#include "src/system/net/http.h"
#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/core/sha1.h"
#include "src/system/streaming/streamkeys.h"

// The default endpoint. Configurable for the same reason the Qobuz and Tidal
// ones are: point it at a fake server and exercise the whole flow without
// spending real calls.
#define PODCAST_API_DEFAULT "https://api.podcastindex.org/api/1.0"

// The most a response is expected to carry. A page of sixty episodes with long
// descriptions stays well under half a megabyte, but the lists can now ask for
// up to 240 at once because the directory has no offset parameter. Four
// megabytes covers the worst case and still cuts off anything else.
#define PODCAST_BODY_MAX (4 * 1024 * 1024)
#define PODCAST_TIMEOUT_SECS 15

static __thread char last_error[256];

const char *podcast_last_error(void) { return last_error; }

static void set_error(const char *what) { snprintf(last_error, sizeof(last_error), "%s", what ? what : ""); }

bool podcast_configured(void) { return streamkeys_podcast_key() && streamkeys_podcast_secret(); }

// The ceiling the directory accepts for `max`: a thousand, per its docs. The
// lists ask for more than one screenful at a time when scrolled to the bottom,
// since the directory has no offset, so the right limit here is the API's and
// not that of a single on-screen page.
#define PODCAST_API_MAX 1000

static const char *api_base(void) { return config_get("podcast", "api_base", PODCAST_API_DEFAULT); }

// The ISO code of the interface language, so a user running the player in
// Italian does not get an all-English chart. The directory takes a
// comma-separated list: the regional tags many Brazilian and Chinese feeds
// declare are listed beside the bare code.
//
// The table lives here rather than in lang.c because this is its only caller:
// lang.c works in language names, which is what a menu needs, and teaching it
// ISO codes for one caller would put weight in the wrong place.
static const char *interface_language_code(void) {
	static const struct {
		const char *name;
		const char *code;
	} MAP[] = {
		{"Italiano", "it"},
		{"English", "en"},
		{"Deutsch", "de"},
		{"Fran\xC3\xA7\x61is", "fr"},
		{"Espa\xC3\xB1ol", "es"},
		{"Japanese", "ja"},
		{"Nederlands", "nl"},
		{"Polski", "pl"},
		{"Russian", "ru"},
		{"Chinese", "zh,zh-cn"},
		{"Portugu\xC3\xAAs BR", "pt,pt-br"},
	};
	const char *current = lang_current();
	for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
		if (current && strcmp(current, MAP[i].name) == 0) {
			return MAP[i].code;
		}
	}
	return "en";
}

// ---------------------------------------------------------------------------
// the signed request
// ---------------------------------------------------------------------------

// Fetches `path` (which starts with /) and returns the body for the caller to
// free. NULL on error, with last_error already written.
static char *api_get(const char *path) {
	const char *key = streamkeys_podcast_key();
	const char *secret = streamkeys_podcast_secret();
	if (!key || !secret) {
		set_error(tr("api_keys_unavailable"));
		return NULL;
	}

	// Wall-clock seconds. Podcast Index tolerates a few minutes of skew and
	// answers 401 beyond that; the 401 handling below recognises that case.
	long now = (long)time(NULL);

	char joined[256];
	snprintf(joined, sizeof(joined), "%s%s%ld", key, secret, now);
	char signature[SHA1_HEX_LEN];
	sha1_hex(joined, strlen(joined), signature);

	// The User-Agent is not politeness: the directory requires it and refuses
	// requests without one.
	char headers[512];
	snprintf(headers, sizeof(headers),
			 "X-Auth-Key: %s\r\n"
			 "X-Auth-Date: %ld\r\n"
			 "Authorization: %s\r\n"
			 "User-Agent: " HTTP_USER_AGENT "\r\n",
			 key, now, signature);

	char url[PODCAST_URL_MAX + 256];
	snprintf(url, sizeof(url), "%s%s", api_base(), path);

	char *body = NULL;
	size_t len = 0;
	int status = 0;

	// want_error_body is true, which matters: with it http_get_ex returns true
	// on a 4xx and hands over the body, where the directory explains itself.
	// It also means the status has to be checked below rather than in the
	// failure branch, which now only sees an unreachable network and 5xx.
	if (!http_get_ex(url, headers, true, &body, &len, PODCAST_BODY_MAX, PODCAST_TIMEOUT_SECS, &status)) {
		if (status >= 500) {
			set_error(tr("podcast_the_podcast_directory_is_not_answering"));
		} else if (status > 0) {
			snprintf(last_error, sizeof(last_error), tr("podcast_directory_status"), status);
		} else {
			const char *why = http_last_error();
			set_error(why && why[0] ? why : tr("podcast_i_cannot_reach_the_podcast_directory"));
		}
		free(body);
		return NULL;
	}

	if (status >= 400) {
		if (status == 401 || status == 403) {
			// A 401 from this directory means two very different things. The
			// first is wrong keys. The second is the clock: the signature
			// contains the time, the directory allows only a few minutes of
			// skew, and this device loses the time at every power-off and gets
			// it back from NTP as soon as there is a network. A player started
			// without Wi-Fi believes it is 1970 and has every request refused.
			//
			// A year before 2020 is the unmistakable sign that NTP has not run
			// yet, so the keys are not at fault and saying so points at the
			// right fix.
			struct tm tm_now;
			time_t t = (time_t)now;
			if (gmtime_r(&t, &tm_now) && tm_now.tm_year + 1900 < 2020) {
				set_error(tr("podcast_clock_behind"));
			} else {
				set_error(tr("podcast_the_podcast_directory_refused_the_keys"));
			}
		} else {
			snprintf(last_error, sizeof(last_error), tr("podcast_directory_status"), status);
		}
		free(body);
		return NULL;
	}

	last_error[0] = '\0';
	return body;
}

// ---------------------------------------------------------------------------
// from JSON to structs
// ---------------------------------------------------------------------------

static void read_feed(const json_doc_t *doc, int obj, podcast_feed_t *out) {
	memset(out, 0, sizeof(*out));
	out->id = json_obj_llong(doc, obj, "id", 0);
	json_obj_str(doc, obj, "title", out->title, sizeof(out->title));
	json_obj_str(doc, obj, "author", out->author, sizeof(out->author));

	// "image" is the podcast's own, "artwork" the already-square one the
	// directory derives. Prefer artwork when present: the list thumbnails are
	// square and a rectangular cover looks distorted in them.
	if (!json_obj_str(doc, obj, "artwork", out->image, sizeof(out->image)) || !out->image[0]) {
		json_obj_str(doc, obj, "image", out->image, sizeof(out->image));
	}
	out->episode_count = (int)json_obj_long(doc, obj, "episodeCount", 0);
	json_obj_str(doc, obj, "url", out->url, sizeof(out->url));
}

static void read_episode(const json_doc_t *doc, int obj, podcast_episode_t *out) {
	memset(out, 0, sizeof(*out));
	out->id = json_obj_llong(doc, obj, "id", 0);
	out->feed_id = json_obj_llong(doc, obj, "feedId", 0);
	json_obj_str(doc, obj, "title", out->title, sizeof(out->title));
	json_obj_str(doc, obj, "feedTitle", out->feed_title, sizeof(out->feed_title));
	json_obj_str(doc, obj, "enclosureUrl", out->enclosure, sizeof(out->enclosure));
	json_obj_str(doc, obj, "enclosureType", out->mime, sizeof(out->mime));
	out->published = json_obj_long(doc, obj, "datePublished", 0);
	out->duration_secs = (int)json_obj_long(doc, obj, "duration", 0);

	// The episode's own artwork when it has one, the podcast's otherwise. Many
	// episodes have none and the field comes back empty rather than absent.
	if (!json_obj_str(doc, obj, "image", out->image, sizeof(out->image)) || !out->image[0]) {
		json_obj_str(doc, obj, "feedImage", out->image, sizeof(out->image));
	}
}

// How many entries the directory sent in the last response, before the
// unusable ones (episodes with no audio) were dropped. This is the count that
// says whether the response was full up to the requested ceiling, and so
// whether there is probably more to ask for; judging that on the cleaned count
// is wrong, because a single discard mid-page makes it look short when it is
// not. Per thread, like last_error.
static __thread int last_raw_count;

int podcast_last_raw_count(void) { return last_raw_count; }

// The path every list shares: fetch, check that the directory said yes, and
// walk the named array.
static int fetch_list(const char *path, const char *array_key, void *out, int max, bool episodes) {
	last_raw_count = 0;
	char *body = api_get(path);
	if (!body) {
		return -1;
	}

	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		set_error(tr("podcast_directory_unparsable"));
		free(body);
		return -1;
	}

	int root = json_root(&doc);

	// The directory answers 200 even when it found nothing: the truth is in
	// "status", which is the string "true" or "false".
	char status[16] = "";
	if (json_obj_str(&doc, root, "status", status, sizeof(status)) && status[0] &&
		(status[0] == 'f' || status[0] == 'F')) {
		char description[200] = "";
		json_obj_str(&doc, root, "description", description, sizeof(description));
		set_error(description[0] ? description : tr("podcast_the_podcast_directory_found_nothing"));
		json_free(&doc);
		free(body);
		return -1;
	}

	int arr = json_get(&doc, root, array_key);
	int count = 0;
	int len = json_len(&doc, arr);
	last_raw_count = len;
	for (int i = 0; i < len && count < max; i++) {
		int item = json_at(&doc, arr, i);
		if (item < 0) {
			continue;
		}
		if (episodes) {
			podcast_episode_t *e = &((podcast_episode_t *)out)[count];
			read_episode(&doc, item, e);
			// An episode with no audio is not an episode: showing it would
			// give a row that does nothing when touched.
			if (e->id != 0 && e->enclosure[0]) {
				count++;
			}
		} else {
			podcast_feed_t *f = &((podcast_feed_t *)out)[count];
			read_feed(&doc, item, f);
			if (f->id != 0 && f->title[0]) {
				count++;
			}
		}
	}

	json_free(&doc);
	free(body);
	return count;
}

// ---------------------------------------------------------------------------
// the calls
// ---------------------------------------------------------------------------

int podcast_search(const char *query, podcast_feed_t *out, int max) {
	if (!query || !query[0] || !out || max <= 0) {
		return -1;
	}
	char encoded[512];
	http_url_encode(query, encoded, sizeof(encoded));

	char path[640];
	// No `clean`: it would drop podcasts marked explicit, which is not the
	// player's choice to make for the listener.
	snprintf(path, sizeof(path), "/search/byterm?q=%s&max=%d", encoded, max > PODCAST_API_MAX ? PODCAST_API_MAX : max);
	return fetch_list(path, "feeds", out, max, false);
}

int podcast_trending(podcast_feed_t *out, int max) {
	if (!out || max <= 0) {
		return -1;
	}
	char path[128];
	// The interface language alone, not ",en": with English appended the chart
	// filled up with American podcasts, which always trend, and the requested
	// language disappeared among them. When that language yields nothing at
	// all, fall back to English: a foreign chart beats an empty page.
	snprintf(path, sizeof(path), "/podcasts/trending?max=%d&lang=%s",
			 max > PODCAST_API_MAX ? PODCAST_API_MAX : max, interface_language_code());
	int n = fetch_list(path, "feeds", out, max, false);
	if (n == 0 && strcmp(interface_language_code(), "en") != 0) {
		snprintf(path, sizeof(path), "/podcasts/trending?max=%d&lang=en",
				 max > PODCAST_API_MAX ? PODCAST_API_MAX : max);
		n = fetch_list(path, "feeds", out, max, false);
	}
	return n;
}

int podcast_episodes(long long feed_id, podcast_episode_t *out, int max) {
	if (feed_id <= 0 || !out || max <= 0) {
		return -1;
	}
	char path[128];
	snprintf(path, sizeof(path), "/episodes/byfeedid?id=%lld&max=%d", feed_id,
			 max > PODCAST_API_MAX ? PODCAST_API_MAX : max);
	return fetch_list(path, "items", out, max, true);
}

bool podcast_feed(long long feed_id, podcast_feed_t *out) {
	if (feed_id <= 0 || !out) {
		return false;
	}
	char path[128];
	snprintf(path, sizeof(path), "/podcasts/byfeedid?id=%lld", feed_id);

	char *body = api_get(path);
	if (!body) {
		return false;
	}
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		set_error(tr("podcast_directory_unparsable"));
		free(body);
		return false;
	}
	// Here the field is "feed", not "feeds": it is a single object.
	int feed = json_get(&doc, json_root(&doc), "feed");
	bool ok = false;
	if (feed >= 0) {
		read_feed(&doc, feed, out);
		ok = out->id != 0;
	}
	json_free(&doc);
	free(body);
	if (!ok) {
		set_error(tr("podcast_gone"));
	}
	return ok;
}

int podcast_feed_by_url(const char *url, podcast_feed_t *out) {
	if (!url || !url[0] || !out) {
		return -1;
	}
	char encoded[1200];
	http_url_encode(url, encoded, sizeof(encoded));
	char path[1300];
	snprintf(path, sizeof(path), "/podcasts/byfeedurl?url=%s", encoded);

	char *body = api_get(path);
	if (!body) {
		return -1;
	}
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		set_error(tr("podcast_directory_unparsable"));
		free(body);
		return -1;
	}
	// A match is the single object "feed"; no match is "feed" as an empty
	// array, with status still "true".
	int found = 0;
	int feed = json_get(&doc, json_root(&doc), "feed");
	if (feed >= 0) {
		// An empty array reads as no id at all.
		read_feed(&doc, feed, out);
		if (out->id > 0) {
			found = 1;
			// The address asked for is the one the user knows the podcast by;
			// the catalogue may hold a newer one after a move, and that is the
			// one worth keeping.
			if (!out->url[0]) {
				snprintf(out->url, sizeof(out->url), "%s", url);
			}
		}
	}
	json_free(&doc);
	free(body);
	return found;
}

bool podcast_episode(long long episode_id, podcast_episode_t *out) {
	if (episode_id <= 0 || !out) {
		return false;
	}
	char path[128];
	snprintf(path, sizeof(path), "/episodes/byid?id=%lld", episode_id);

	char *body = api_get(path);
	if (!body) {
		return false;
	}
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		set_error(tr("podcast_directory_unparsable"));
		free(body);
		return false;
	}
	int episode = json_get(&doc, json_root(&doc), "episode");
	bool ok = false;
	if (episode >= 0) {
		read_episode(&doc, episode, out);
		ok = out->id != 0;
	}
	json_free(&doc);
	free(body);
	if (!ok) {
		set_error(tr("podcast_episode_gone"));
	}
	return ok;
}

// ---------------------------------------------------------------------------
// the two skip intervals
// ---------------------------------------------------------------------------

// Only the three values the buttons can draw: a hand-edited config file must
// not be able to give the player a seven-hundred-second skip behind an icon
// that says ten.
static int clamp_skip(int value) {
	if (value == PODCAST_SKIP_LONG) {
		return PODCAST_SKIP_LONG;
	}
	if (value == PODCAST_SKIP_HUGE) {
		return PODCAST_SKIP_HUGE;
	}
	return PODCAST_SKIP_SHORT;
}

// The defaults are not the audiobook ones (-10/+10): on a podcast the forward
// skip is almost always used to jump a sponsor read, and thirty seconds is that
// length.
int podcast_skip_back(void) { return clamp_skip(config_get_int("podcast", "skip_back", PODCAST_SKIP_SHORT)); }

int podcast_skip_forward(void) { return clamp_skip(config_get_int("podcast", "skip_forward", PODCAST_SKIP_LONG)); }

void podcast_set_skip_back(int seconds) { config_set_int("podcast", "skip_back", clamp_skip(seconds)); }

void podcast_set_skip_forward(int seconds) { config_set_int("podcast", "skip_forward", clamp_skip(seconds)); }

int podcast_speed_permille(void) {
	long value = config_get_int("podcast", "speed", 1000);
	if (value < 250 || value > 4000) {
		value = 1000;
	}
	return (int)value;
}

void podcast_set_speed_permille(int permille) {
	if (permille < 250 || permille > 4000) {
		permille = 1000;
	}
	config_set_int("podcast", "speed", permille);
}

double podcast_speed(void) { return (double)podcast_speed_permille() / 1000.0; }

bool podcast_stop_at_episode_end(void) { return config_get_int("podcast", "stop_episode_end", 0) != 0; }

void podcast_set_stop_at_episode_end(bool on) { config_set_int("podcast", "stop_episode_end", on ? 1 : 0); }
