#include "lrclib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "src/system/core/json.h"
#include "src/system/net/http.h"

#ifndef LRCLIB_API
#define LRCLIB_API "https://lrclib.net/api"
#endif
// A search can keep the connection quiet for well over ten seconds while
// LRCLIB looks, and the TLS handshake on this CPU takes a few more.
#define LRCLIB_TIMEOUT_SECS 25
// LRCLIB drops a share of requests under load -- no answer, a 5xx or a 429 --
// and the same request a moment later goes through. Tries in all, with a
// growing pause between them.
#define LRCLIB_ATTEMPTS 3
#define LRCLIB_RETRY_PAUSE_SECS 2
// The free-text search after a search by tags that found nothing: one try,
// and shorter. It is the slow one of the two on LRCLIB's side, and the tags
// have already had their answer.
#define LRCLIB_FREE_ATTEMPTS 1
#define LRCLIB_FREE_TIMEOUT_SECS 15
// A search answers with up to twenty records, each with both kinds of lyrics.
#define LRCLIB_MAX_BYTES (1024 * 1024)
// What /api/get itself allows between the length asked for and the record's.
#define LRCLIB_DURATION_SLACK 2

// The query string of a search by tags; false when it does not fit.
static bool build_query(char *out, size_t size, const char *title, const char *artist) {
	char t[768], a[768];
	http_url_encode(title, t, sizeof(t));
	http_url_encode(artist, a, sizeof(a));
	int n = snprintf(out, size, "track_name=%s&artist_name=%s", t, a);
	return n > 0 && (size_t)n < size;
}

// The same two as free text, for a record filed with the tags spelled another
// way.
static bool build_free_query(char *out, size_t size, const char *title, const char *artist) {
	char words[600], q[1800];
	snprintf(words, sizeof(words), "%s %s", title, artist);
	http_url_encode(words, q, sizeof(q));
	int n = snprintf(out, size, "q=%s", q);
	return n > 0 && (size_t)n < size;
}

// A string field of a record, copied out whole; NULL when it is missing, null
// or empty.
static char *record_text(const json_doc_t *doc, int record, const char *key) {
	int tok = json_get(doc, record, key);
	if (tok < 0 || doc->toks[tok].type != JSON_STRING) {
		return NULL;
	}
	size_t size = (size_t)(doc->toks[tok].end - doc->toks[tok].start) + 1;
	char *text = malloc(size);
	if (!text) {
		return NULL;
	}
	if (!json_str(doc, tok, text, size) || !text[0]) {
		free(text);
		return NULL;
	}
	return text;
}

// GET with the retries above. A 404 or another 4xx is an answer and is not
// asked again.
static bool api_get(const char *url, int attempts, int timeout_secs, char **body, int *status) {
	for (int attempt = 1; attempt <= attempts; attempt++) {
		*status = 0;
		if (http_get_ex(url, NULL, false, body, NULL, LRCLIB_MAX_BYTES, timeout_secs, status)) {
			return true;
		}
		bool transient = *status <= 0 || *status == 429 || *status >= 500;
		const char *why = http_last_error();
		fprintf(stderr, "lrclib: attempt %d/%d failed (HTTP %d%s%s)\n", attempt, attempts, *status, why ? ": " : "", why ? why : "");
		if (!transient || attempt == attempts) {
			break;
		}
		sleep((unsigned)(LRCLIB_RETRY_PAUSE_SECS * attempt));
	}
	return false;
}

// One search. Appends the records with words to *list; *instrumental notes one
// that says the track has none.
static lrclib_result_t search(const char *query, int attempts, int timeout_secs, lrclib_record_t **list, int *count, bool *instrumental) {
	char url[3200];
	int n = snprintf(url, sizeof(url), LRCLIB_API "/search?%s", query);
	if (n < 0 || (size_t)n >= sizeof(url)) {
		return LRCLIB_NOT_FOUND;
	}

	char *body = NULL;
	int status = 0;
	if (!api_get(url, attempts, timeout_secs, &body, &status)) {
		return status == 404 ? LRCLIB_NOT_FOUND : LRCLIB_NETWORK;
	}

	lrclib_result_t result = LRCLIB_NETWORK;
	json_doc_t doc;
	if (json_parse(body, &doc)) {
		int root = json_root(&doc);
		if (root >= 0 && doc.toks[root].type == JSON_ARRAY) {
			result = LRCLIB_NOT_FOUND;
			int total = json_len(&doc, root);
			lrclib_record_t *grown = total > 0 ? realloc(*list, sizeof(**list) * (size_t)(*count + total)) : NULL;
			if (grown) {
				*list = grown;
			}
			for (int i = 0; grown && i < total; i++) {
				int record = json_at(&doc, root, i);
				if (record < 0 || doc.toks[record].type != JSON_OBJECT) {
					continue;
				}
				lrclib_record_t *r = &(*list)[*count];
				memset(r, 0, sizeof(*r));
				r->text = record_text(&doc, record, "syncedLyrics");
				r->synced = r->text != NULL;
				if (!r->text) {
					r->text = record_text(&doc, record, "plainLyrics");
				}
				if (!r->text) {
					*instrumental |= json_obj_bool(&doc, record, "instrumental", false);
					continue;
				}
				json_obj_str(&doc, record, "trackName", r->track, sizeof(r->track));
				json_obj_str(&doc, record, "artistName", r->artist, sizeof(r->artist));
				json_obj_str(&doc, record, "albumName", r->album, sizeof(r->album));
				r->duration = (int)(json_double(&doc, json_get(&doc, record, "duration"), 0.0) + 0.5);
				(*count)++;
			}
		}
		json_free(&doc);
	}
	free(body);
	return result;
}

// How likely a record is to be the track's: within the slack of its length
// above all, then timed, then from the same album, then the closer length.
static int rank_of(const lrclib_record_t *r, int duration, const char *album) {
	int score = 0;
	if (duration > 0 && r->duration > 0) {
		int off = abs(r->duration - duration);
		if (off <= LRCLIB_DURATION_SLACK) {
			score += 1000;
		}
		score += off < 99 ? 99 - off : 0;
	}
	if (r->synced) {
		score += 200;
	}
	if (album && album[0] && strcasecmp(r->album, album) == 0) {
		score += 100;
	}
	return score;
}

// Best first. An insertion sort, which keeps LRCLIB's own order between equals;
// there are never more than a few dozen.
static void rank(lrclib_record_t *list, int count, int duration, const char *album) {
	for (int i = 1; i < count; i++) {
		lrclib_record_t moving = list[i];
		int score = rank_of(&moving, duration, album);
		int j = i;
		while (j > 0 && rank_of(&list[j - 1], duration, album) < score) {
			list[j] = list[j - 1];
			j--;
		}
		list[j] = moving;
	}
}

lrclib_result_t lrclib_search(const char *title, const char *artist, const char *album, int duration_secs, lrclib_record_t **records, int *count) {
	*records = NULL;
	*count = 0;
	if (!title || !title[0] || !artist || !artist[0]) {
		return LRCLIB_NO_TAGS;
	}

	char query[2400];
	if (!build_query(query, sizeof(query), title, artist)) {
		return LRCLIB_NO_TAGS;
	}
	lrclib_record_t *list = NULL;
	int found = 0;
	bool instrumental = false;
	lrclib_result_t result = search(query, LRCLIB_ATTEMPTS, LRCLIB_TIMEOUT_SECS, &list, &found, &instrumental);
	if (result == LRCLIB_NOT_FOUND && found == 0 && build_free_query(query, sizeof(query), title, artist)) {
		// LRCLIB has already said it has nothing under these tags: a free-text
		// search that does not answer leaves that as the answer, rather than
		// reporting LRCLIB as unreachable. What it does find is in `found`.
		search(query, LRCLIB_FREE_ATTEMPTS, LRCLIB_FREE_TIMEOUT_SECS, &list, &found, &instrumental);
	}

	if (found == 0) {
		free(list);
		if (result == LRCLIB_NOT_FOUND && instrumental) {
			result = LRCLIB_INSTRUMENTAL;
		}
		fprintf(stderr, "lrclib: '%s' by '%s': %d\n", title, artist, (int)result);
		return result;
	}

	rank(list, found, duration_secs > 0 && duration_secs <= 3600 ? duration_secs : 0, album);
	fprintf(stderr, "lrclib: '%s' by '%s': %d records\n", title, artist, found);
	*records = list;
	*count = found;
	return LRCLIB_OK;
}

void lrclib_free(lrclib_record_t *records, int count) {
	if (!records) {
		return;
	}
	for (int i = 0; i < count; i++) {
		free(records[i].text);
	}
	free(records);
}

// "<dir>/<file name without its extension>.lrc".
static bool lrc_path_for(const char *path, char *out, size_t size) {
	const char *slash = strrchr(path, '/');
	const char *base = slash ? slash + 1 : path;
	const char *dot = strrchr(base, '.');
	size_t stem = dot && dot != base ? (size_t)(dot - path) : strlen(path);
	int n = snprintf(out, size, "%.*s.lrc", (int)stem, path);
	return n > 0 && (size_t)n < size;
}

// Written to a temporary name and renamed, so a card pulled halfway leaves no
// half file for lyrics_load() to find.
bool lrclib_save(const char *path, const char *text) {
	char target[1100], temp[1110];
	if (!path || !text || !lrc_path_for(path, target, sizeof(target))) {
		return false;
	}
	snprintf(temp, sizeof(temp), "%s.part", target);
	FILE *f = fopen(temp, "wb");
	if (!f) {
		return false;
	}
	size_t len = strlen(text);
	bool ok = fwrite(text, 1, len, f) == len;
	if (ok && len > 0 && text[len - 1] != '\n') {
		ok = fputc('\n', f) != EOF;
	}
	ok = fflush(f) == 0 && ok;
	ok = fsync(fileno(f)) == 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(temp, target) != 0) {
		unlink(temp);
		fprintf(stderr, "lrclib: could not write %s\n", target);
		return false;
	}
	fprintf(stderr, "lrclib: saved %s (%zu bytes)\n", target, len);
	return true;
}
