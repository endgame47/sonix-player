#include "podcastrss.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "src/system/core/lang.h"
#include "src/system/net/http.h"

// A feed with a few hundred episodes and their show notes runs to a few
// megabytes. A longer one is cut here, as the catalogue's answers are, and the
// episodes past the cut -- the oldest, in the usual order -- are left out.
#define RSS_BODY_MAX (4 * 1024 * 1024)
#define RSS_TIMEOUT_SECS 20

// The most episodes kept of one feed: as many as the podcast's page holds.
#define RSS_MAX_ITEMS 1000

// How long the last feed read answers for itself before it is fetched again.
#define RSS_CACHE_SECS 600

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------------------
// ids
// ---------------------------------------------------------------------------

static uint64_t fnv1a(const char *a, const char *b) {
	uint64_t h = 1469598103934665603ULL;
	for (const char *s = a; s && *s; s++) {
		h ^= (unsigned char)*s;
		h *= 1099511628211ULL;
	}
	if (b) {
		h ^= '\n';
		h *= 1099511628211ULL;
		for (const char *s = b; *s; s++) {
			h ^= (unsigned char)*s;
			h *= 1099511628211ULL;
		}
	}
	return h;
}

static long long make_id(const char *a, const char *b) {
	return PODCASTRSS_ID_BASE | (long long)(fnv1a(a, b) & (uint64_t)(PODCASTRSS_ID_BASE - 1));
}

bool podcastrss_is_id(long long id) { return id >= PODCASTRSS_ID_BASE; }

long long podcastrss_feed_id(const char *url) { return make_id(url, NULL); }

// ---------------------------------------------------------------------------
// where each feed is
// ---------------------------------------------------------------------------

typedef struct {
	long long id;
	char url[PODCAST_FEED_URL_MAX];
} known_t;

static known_t *known;
static int known_count;
static int known_capacity;

void podcastrss_remember(long long feed_id, const char *url) {
	if (!podcastrss_is_id(feed_id) || !url || !url[0]) {
		return;
	}
	pthread_mutex_lock(&lock);
	for (int i = 0; i < known_count; i++) {
		if (known[i].id == feed_id) {
			pthread_mutex_unlock(&lock);
			return;
		}
	}
	if (known_count == known_capacity) {
		int grown = known_capacity ? known_capacity * 2 : 16;
		known_t *bigger = realloc(known, (size_t)grown * sizeof(*known));
		if (!bigger) {
			pthread_mutex_unlock(&lock);
			return;
		}
		known = bigger;
		known_capacity = grown;
	}
	known[known_count].id = feed_id;
	snprintf(known[known_count].url, sizeof(known[0].url), "%s", url);
	known_count++;
	pthread_mutex_unlock(&lock);
}

bool podcastrss_url_for(long long feed_id, char *out, size_t size) {
	bool found = false;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < known_count && !found; i++) {
		if (known[i].id == feed_id) {
			snprintf(out, size, "%s", known[i].url);
			found = true;
		}
	}
	pthread_mutex_unlock(&lock);
	return found;
}

// ---------------------------------------------------------------------------
// reading the XML
//
// Not a general XML parser: the elements a podcast feed is read for are found
// by name between the start of an <item> and its end, which is all RSS asks
// for and what survives the feeds found in the wild.
// ---------------------------------------------------------------------------

// The next '<' in [p, end) that opens markup, past any CDATA section: show
// notes carry HTML in CDATA, "<item>" and "</title>" included.
static const char *next_markup(const char *p, const char *end) {
	while (p && p < end) {
		const char *lt = memchr(p, '<', (size_t)(end - p));
		if (!lt) {
			return NULL;
		}
		if (end - lt >= 9 && strncmp(lt, "<![CDATA[", 9) == 0) {
			const char *stop = strstr(lt + 9, "]]>");
			if (!stop || stop >= end) {
				return NULL;
			}
			p = stop + 3;
			continue;
		}
		return lt;
	}
	return NULL;
}

// The next start tag `name` in [p, end): "<name" followed by '>', '/' or space,
// so "<title" does not match "<titles". NULL when there is none.
static const char *find_tag(const char *p, const char *end, const char *name) {
	size_t n = strlen(name);
	while (p && p < end) {
		const char *lt = next_markup(p, end);
		if (!lt || lt + 1 + n >= end) {
			return NULL;
		}
		if (strncmp(lt + 1, name, n) == 0) {
			char c = lt[1 + n];
			if (c == '>' || c == '/' || isspace((unsigned char)c)) {
				return lt;
			}
		}
		p = lt + 1;
	}
	return NULL;
}

// The end tag "</name>" from p on, or NULL.
static const char *find_end(const char *p, const char *end, const char *name) {
	char close[64];
	int n = snprintf(close, sizeof(close), "</%s>", name);
	while (p && p < end) {
		const char *lt = next_markup(p, end);
		if (!lt || lt + n > end) {
			return NULL;
		}
		if (strncmp(lt, close, (size_t)n) == 0) {
			return lt;
		}
		p = lt + 1;
	}
	return NULL;
}

// Turns the character references and the five named entities, plus a few that
// feeds use as if XML had them, into UTF-8, in place.
static void decode_entities(char *s) {
	static const struct {
		const char *name;
		const char *text;
	} NAMED[] = {
		{"amp", "&"},		{"lt", "<"},			{"gt", ">"},			 {"quot", "\""},
		{"apos", "'"},		{"nbsp", " "},			{"rsquo", "\xE2\x80\x99"}, {"lsquo", "\xE2\x80\x98"},
		{"rdquo", "\xE2\x80\x9D"}, {"ldquo", "\xE2\x80\x9C"}, {"ndash", "\xE2\x80\x93"}, {"mdash", "\xE2\x80\x94"},
		{"hellip", "\xE2\x80\xA6"},
	};
	char *w = s;
	for (char *r = s; *r;) {
		if (*r != '&') {
			*w++ = *r++;
			continue;
		}
		char *semi = strchr(r, ';');
		if (!semi || semi - r > 10) {
			*w++ = *r++;
			continue;
		}
		bool done = false;
		if (r[1] == '#') {
			unsigned long code = r[2] == 'x' || r[2] == 'X' ? strtoul(r + 3, NULL, 16) : strtoul(r + 2, NULL, 10);
			if (code > 0 && code < 0x110000) {
				if (code < 0x80) {
					*w++ = (char)code;
				} else if (code < 0x800) {
					*w++ = (char)(0xC0 | (code >> 6));
					*w++ = (char)(0x80 | (code & 0x3F));
				} else if (code < 0x10000) {
					*w++ = (char)(0xE0 | (code >> 12));
					*w++ = (char)(0x80 | ((code >> 6) & 0x3F));
					*w++ = (char)(0x80 | (code & 0x3F));
				} else {
					*w++ = (char)(0xF0 | (code >> 18));
					*w++ = (char)(0x80 | ((code >> 12) & 0x3F));
					*w++ = (char)(0x80 | ((code >> 6) & 0x3F));
					*w++ = (char)(0x80 | (code & 0x3F));
				}
				done = true;
			}
		} else {
			for (size_t i = 0; i < sizeof(NAMED) / sizeof(NAMED[0]) && !done; i++) {
				size_t n = strlen(NAMED[i].name);
				if ((size_t)(semi - r - 1) == n && strncmp(r + 1, NAMED[i].name, n) == 0) {
					for (const char *t = NAMED[i].text; *t;) {
						*w++ = *t++;
					}
					done = true;
				}
			}
		}
		if (done) {
			r = semi + 1;
		} else {
			*w++ = *r++;
		}
	}
	*w = '\0';
}

// Whitespace runs to one space, and none at either end.
static void squeeze(char *s) {
	char *w = s;
	bool space = true;
	for (char *r = s; *r; r++) {
		if (isspace((unsigned char)*r)) {
			if (!space) {
				*w++ = ' ';
				space = true;
			}
		} else {
			*w++ = *r;
			space = false;
		}
	}
	if (w > s && w[-1] == ' ') {
		w--;
	}
	*w = '\0';
}

// The text of the element whose start tag is at `tag`: CDATA taken as it is,
// entities decoded elsewhere. False when the element is empty or not closed.
static bool element_text(const char *tag, const char *end, const char *name, char *out, size_t size) {
	out[0] = '\0';
	const char *gt = memchr(tag, '>', (size_t)(end - tag));
	if (!gt || gt[-1] == '/') {
		return false;
	}
	const char *body = gt + 1;
	const char *close = find_end(body, end, name);
	if (!close) {
		return false;
	}
	size_t used = 0;
	const char *p = body;
	while (p < close && used + 1 < size) {
		if (close - p >= 9 && strncmp(p, "<![CDATA[", 9) == 0) {
			const char *stop = strstr(p + 9, "]]>");
			if (!stop || stop > close) {
				stop = close;
			}
			for (const char *c = p + 9; c < stop && used + 1 < size; c++) {
				out[used++] = *c;
			}
			p = stop + 3;
			continue;
		}
		out[used++] = *p++;
	}
	out[used] = '\0';
	decode_entities(out);
	squeeze(out);
	return out[0] != '\0';
}

// An attribute of the start tag at `tag`, entities decoded.
static bool tag_attribute(const char *tag, const char *end, const char *attr, char *out, size_t size) {
	out[0] = '\0';
	const char *gt = memchr(tag, '>', (size_t)(end - tag));
	if (!gt) {
		return false;
	}
	size_t n = strlen(attr);
	for (const char *p = tag; p + n + 2 < gt; p++) {
		if (!isspace((unsigned char)p[0]) || strncmp(p + 1, attr, n) != 0) {
			continue;
		}
		const char *q = p + 1 + n;
		while (q < gt && isspace((unsigned char)*q)) {
			q++;
		}
		if (*q != '=') {
			continue;
		}
		q++;
		while (q < gt && isspace((unsigned char)*q)) {
			q++;
		}
		char quote = *q;
		if (quote != '"' && quote != '\'') {
			continue;
		}
		const char *v = q + 1;
		const char *stop = memchr(v, quote, (size_t)(gt - v));
		if (!stop) {
			return false;
		}
		size_t len = (size_t)(stop - v);
		if (len >= size) {
			len = size - 1;
		}
		memcpy(out, v, len);
		out[len] = '\0';
		decode_entities(out);
		squeeze(out);
		return out[0] != '\0';
	}
	return false;
}

// The text of the first element `name` in [p, end).
static bool first_text(const char *p, const char *end, const char *name, char *out, size_t size) {
	const char *tag = find_tag(p, end, name);
	return tag && element_text(tag, end, name, out, size);
}

// ---------------------------------------------------------------------------
// dates and lengths
// ---------------------------------------------------------------------------

static int month_of(const char *s) {
	static const char *const MONTHS[12] = {"jan", "feb", "mar", "apr", "may", "jun",
										   "jul", "aug", "sep", "oct", "nov", "dec"};
	for (int i = 0; i < 12; i++) {
		if (strncasecmp(s, MONTHS[i], 3) == 0) {
			return i;
		}
	}
	return -1;
}

// The offset of a zone from UTC, in seconds: "+0200", "-0500", "GMT", "EST".
static long zone_offset(const char *z) {
	if (*z == '+' || *z == '-') {
		int hhmm = atoi(z + 1);
		long secs = (long)(hhmm / 100) * 3600 + (long)(hhmm % 100) * 60;
		return *z == '-' ? -secs : secs;
	}
	static const struct {
		const char *name;
		int hours;
	} ZONES[] = {{"EST", -5}, {"EDT", -4}, {"CST", -6}, {"CDT", -5}, {"MST", -7}, {"MDT", -6}, {"PST", -8}, {"PDT", -7}};
	for (size_t i = 0; i < sizeof(ZONES) / sizeof(ZONES[0]); i++) {
		if (strncasecmp(z, ZONES[i].name, 3) == 0) {
			return (long)ZONES[i].hours * 3600;
		}
	}
	return 0; // GMT, UT, Z, or something unknown
}

// RFC 822 as RSS has it ("Wed, 09 Oct 2025 06:00:00 +0200"), and the ISO 8601
// some feeds put there instead. 0 when it is neither.
static long parse_date(const char *s) {
	struct tm when;
	memset(&when, 0, sizeof(when));
	int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
	if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) >= 3 && y > 1900) {
		when.tm_year = y - 1900;
		when.tm_mon = mo - 1;
		when.tm_mday = d;
		when.tm_hour = h;
		when.tm_min = mi;
		when.tm_sec = se;
		return (long)timegm(&when);
	}
	const char *p = strchr(s, ',');
	p = p ? p + 1 : s;
	char month[16] = "";
	char clock[16] = "";
	char zone[16] = "";
	if (sscanf(p, " %d %15s %d %15s %15s", &d, month, &y, clock, zone) < 3) {
		return 0;
	}
	sscanf(clock, "%d:%d:%d", &h, &mi, &se);
	int m = month_of(month);
	if (m < 0) {
		return 0;
	}
	if (y < 100) {
		y += y < 70 ? 2000 : 1900;
	}
	when.tm_year = y - 1900;
	when.tm_mon = m;
	when.tm_mday = d;
	when.tm_hour = h;
	when.tm_min = mi;
	when.tm_sec = se;
	return (long)timegm(&when) - zone_offset(zone);
}

// "1:02:03", "62:03" or "3723" seconds. 0 for anything past a week, which is
// not a length but a typo.
#define DURATION_MAX (7L * 24 * 3600)

static int parse_duration(const char *s) {
	double total = 0;
	if (!strchr(s, ':')) {
		total = atof(s);
	} else {
		for (const char *p = s; *p;) {
			total = total * 60 + atof(p);
			const char *colon = strchr(p, ':');
			if (!colon || total > DURATION_MAX) {
				break;
			}
			p = colon + 1;
		}
	}
	return total > 0 && total <= DURATION_MAX ? (int)total : 0;
}

// ---------------------------------------------------------------------------
// one feed
// ---------------------------------------------------------------------------

typedef struct {
	podcast_feed_t feed;
	podcast_episode_t *episodes;
	int count;
} parsed_t;

// The channel's own artwork: itunes:image, which is square, before the RSS
// <image>.
static void channel_image(const char *head, const char *end, char *out, size_t size) {
	const char *tag = find_tag(head, end, "itunes:image");
	if (tag && tag_attribute(tag, end, "href", out, size)) {
		return;
	}
	const char *image = find_tag(head, end, "image");
	if (image) {
		const char *stop = find_end(image, end, "image");
		if (stop) {
			first_text(image, stop, "url", out, size);
		}
	}
}

// The channel's title: the first <title> that is not the one inside <image>.
static void channel_title(const char *head, const char *end, char *out, size_t size) {
	const char *image = find_tag(head, end, "image");
	const char *image_end = image ? find_end(image, end, "image") : NULL;
	const char *tag = find_tag(head, end, "title");
	if (tag && image && image_end && tag > image && tag < image_end) {
		tag = find_tag(image_end, end, "title");
	}
	if (tag) {
		element_text(tag, end, "title", out, size);
	}
}

static int by_date_cmp(const void *a, const void *b) {
	const podcast_episode_t *x = a, *y = b;
	if (x->published != y->published) {
		return x->published < y->published ? 1 : -1;
	}
	return 0;
}

static bool parse(const char *url, const char *body, size_t len, parsed_t *out) {
	memset(out, 0, sizeof(*out));
	const char *end = body + len;
	const char *channel = find_tag(body, end, "channel");
	if (!channel) {
		return false;
	}
	const char *first_item = find_tag(channel, end, "item");
	const char *head_end = first_item ? first_item : end;

	podcast_feed_t *feed = &out->feed;
	feed->id = podcastrss_feed_id(url);
	snprintf(feed->url, sizeof(feed->url), "%s", url);
	channel_title(channel, head_end, feed->title, sizeof(feed->title));
	if (!first_text(channel, head_end, "itunes:author", feed->author, sizeof(feed->author))) {
		first_text(channel, head_end, "author", feed->author, sizeof(feed->author));
	}
	channel_image(channel, head_end, feed->image, sizeof(feed->image));
	if (!feed->title[0]) {
		return false;
	}

	int capacity = 0;
	for (const char *item = first_item; item && out->count < RSS_MAX_ITEMS;) {
		const char *item_end = find_end(item, end, "item");
		if (!item_end) {
			break; // cut off by the size limit
		}
		const char *enclosure = find_tag(item, item_end, "enclosure");
		char audio[PODCAST_URL_MAX] = "";
		if (enclosure && tag_attribute(enclosure, item_end, "url", audio, sizeof(audio))) {
			if (out->count == capacity) {
				int grown = capacity ? capacity * 2 : 64;
				podcast_episode_t *bigger = realloc(out->episodes, (size_t)grown * sizeof(*bigger));
				if (!bigger) {
					break;
				}
				out->episodes = bigger;
				capacity = grown;
			}
			podcast_episode_t *e = &out->episodes[out->count];
			memset(e, 0, sizeof(*e));
			snprintf(e->enclosure, sizeof(e->enclosure), "%s", audio);
			tag_attribute(enclosure, item_end, "type", e->mime, sizeof(e->mime));
			first_text(item, item_end, "title", e->title, sizeof(e->title));
			char text[256];
			// The guid is what names an episode for good; the audio address
			// stands in for a feed that gives none.
			if (!first_text(item, item_end, "guid", text, sizeof(text))) {
				snprintf(text, sizeof(text), "%.*s", (int)sizeof(text) - 1, audio);
			}
			e->id = make_id(url, text);
			e->feed_id = feed->id;
			snprintf(e->feed_title, sizeof(e->feed_title), "%s", feed->title);
			if (first_text(item, item_end, "pubDate", text, sizeof(text))) {
				e->published = parse_date(text);
			}
			if (first_text(item, item_end, "itunes:duration", text, sizeof(text))) {
				e->duration_secs = parse_duration(text);
			}
			const char *art = find_tag(item, item_end, "itunes:image");
			if (!art || !tag_attribute(art, item_end, "href", e->image, sizeof(e->image))) {
				snprintf(e->image, sizeof(e->image), "%s", feed->image);
			}
			if (!e->title[0]) {
				snprintf(e->title, sizeof(e->title), "%s", feed->title);
			}
			out->count++;
		}
		item = find_tag(item_end, end, "item");
	}
	feed->episode_count = out->count;
	// Newest first, as the catalogue lists them; most feeds already are.
	if (out->count > 1) {
		qsort(out->episodes, (size_t)out->count, sizeof(*out->episodes), by_date_cmp);
	}
	return true;
}

static bool fetch(const char *url, parsed_t *out) {
	char *body = NULL;
	size_t len = 0;
	int status = 0;
	if (!http_get_ex(url, "User-Agent: " HTTP_USER_AGENT "\r\n", false, &body, &len, RSS_BODY_MAX, RSS_TIMEOUT_SECS,
					 &status)) {
		const char *why = http_last_error();
		podcast_set_last_error(why && why[0] ? why : tr("podcast_rss_unreachable"));
		free(body);
		return false;
	}
	bool ok = parse(url, body, len, out);
	free(body);
	if (!ok) {
		free(out->episodes);
		out->episodes = NULL;
		podcast_set_last_error(tr("podcast_rss_not_a_feed"));
	}
	return ok;
}

// ---------------------------------------------------------------------------
// the last feed read
// ---------------------------------------------------------------------------

static parsed_t cache;
static struct timespec cache_at;

static long seconds_since(const struct timespec *t) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long)(now.tv_sec - t->tv_sec);
}

// Puts a feed just read in the cache; `fresh` is taken over.
static void cache_store(parsed_t *fresh) {
	pthread_mutex_lock(&lock);
	free(cache.episodes);
	cache = *fresh;
	clock_gettime(CLOCK_MONOTONIC, &cache_at);
	pthread_mutex_unlock(&lock);
	fresh->episodes = NULL;
}

// The feed `feed_id`, read now unless the cache has it. Called with the lock
// not held; returns with it held on success.
static bool cache_hold(long long feed_id) {
	pthread_mutex_lock(&lock);
	if (cache.feed.id == feed_id && seconds_since(&cache_at) < RSS_CACHE_SECS) {
		return true;
	}
	pthread_mutex_unlock(&lock);

	char url[PODCAST_FEED_URL_MAX];
	if (!podcastrss_url_for(feed_id, url, sizeof(url))) {
		podcast_set_last_error(tr("podcast_gone"));
		return false;
	}
	parsed_t fresh;
	if (!fetch(url, &fresh)) {
		return false;
	}
	cache_store(&fresh);
	pthread_mutex_lock(&lock);
	if (cache.feed.id != feed_id) {
		pthread_mutex_unlock(&lock); // replaced meanwhile by another reader
		podcast_set_last_error(tr("podcast_gone"));
		return false;
	}
	return true;
}

bool podcastrss_read_feed(const char *url, podcast_feed_t *feed) {
	if (!url || !url[0] || !feed) {
		return false;
	}
	parsed_t fresh;
	if (!fetch(url, &fresh)) {
		return false;
	}
	*feed = fresh.feed;
	podcastrss_remember(feed->id, url);
	cache_store(&fresh);
	return true;
}

int podcastrss_episodes(long long feed_id, podcast_episode_t *out, int max, int *raw_count) {
	if (raw_count) {
		*raw_count = 0;
	}
	if (!out || max <= 0 || !cache_hold(feed_id)) {
		return -1;
	}
	int n = cache.count < max ? cache.count : max;
	if (n > 0) {
		memcpy(out, cache.episodes, sizeof(*out) * (size_t)n);
	}
	if (raw_count) {
		// Full to the limit when there are more: the page asks for the next
		// step, which the cache answers without the network.
		*raw_count = n;
	}
	pthread_mutex_unlock(&lock);
	return n;
}

bool podcastrss_feed(long long feed_id, podcast_feed_t *out) {
	if (!out || !cache_hold(feed_id)) {
		return false;
	}
	*out = cache.feed;
	pthread_mutex_unlock(&lock);
	return true;
}

bool podcastrss_episode(long long feed_id, long long episode_id, podcast_episode_t *out) {
	if (!out || !cache_hold(feed_id)) {
		return false;
	}
	bool found = false;
	for (int i = 0; i < cache.count && !found; i++) {
		if (cache.episodes[i].id == episode_id) {
			*out = cache.episodes[i];
			found = true;
		}
	}
	pthread_mutex_unlock(&lock);
	if (!found) {
		podcast_set_last_error(tr("podcast_that_episode_is_gone"));
	}
	return found;
}
