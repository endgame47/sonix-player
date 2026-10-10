#include "vorbischap.h"

#include "src/system/core/utils.h"
#include "src/system/decode/dr_flac.h"
#include "src/system/library/oggtags.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Far more than any book has; a file claiming more is cut there.
#define MAX_CHAPTERS 1024

typedef struct {
	long number;
	double start; // < 0 until the time tag is seen
	char title[128];
} entry_t;

typedef struct {
	entry_t *items;
	int count;
	int capacity;
} collect_t;

static entry_t *entry_for(collect_t *c, long number) {
	for (int i = 0; i < c->count; i++) {
		if (c->items[i].number == number) {
			return &c->items[i];
		}
	}
	if (c->count >= MAX_CHAPTERS) {
		return NULL;
	}
	if (c->count == c->capacity) {
		int grown = c->capacity ? c->capacity * 2 : 32;
		entry_t *bigger = realloc(c->items, (size_t)grown * sizeof(*bigger));
		if (!bigger) {
			return NULL;
		}
		c->items = bigger;
		c->capacity = grown;
	}
	entry_t *e = &c->items[c->count++];
	e->number = number;
	e->start = -1;
	e->title[0] = '\0';
	return e;
}

// "HH:MM:SS.mmm", "H:MM:SS", "MM:SS.mmm" -> seconds, or -1.
static double parse_time(const char *text, size_t len) {
	char buf[40];
	if (len == 0 || len >= sizeof(buf)) {
		return -1;
	}
	memcpy(buf, text, len);
	buf[len] = '\0';

	double parts[3];
	int count = 0;
	char *p = buf;
	while (count < 3) {
		if (!isdigit((unsigned char)*p)) {
			return -1;
		}
		char *end = NULL;
		parts[count++] = strtod(p, &end);
		if (*end == ':') {
			p = end + 1;
			continue;
		}
		if (*end != '\0') {
			return -1;
		}
		break;
	}
	if (count < 2) {
		return -1;
	}
	double seconds = 0;
	for (int i = 0; i < count; i++) {
		seconds = seconds * 60 + parts[i];
	}
	return seconds;
}

// One "KEY=value" comment, not NUL-terminated.
static void take_comment(collect_t *c, const char *comment, size_t len) {
	if (len < 9 || strncasecmp(comment, "CHAPTER", 7) != 0 || !isdigit((unsigned char)comment[7])) {
		return;
	}
	const char *eq = memchr(comment, '=', len);
	if (!eq) {
		return;
	}
	const char *digits = comment + 7;
	const char *after = digits;
	long number = 0;
	while (after < eq && isdigit((unsigned char)*after)) {
		number = number * 10 + (*after - '0');
		if (number > 100000) {
			return;
		}
		after++;
	}
	const char *value = eq + 1;
	size_t value_len = len - (size_t)(value - comment);

	if (after == eq) {
		double start = parse_time(value, value_len);
		if (start < 0) {
			return;
		}
		entry_t *e = entry_for(c, number);
		if (e) {
			e->start = start;
		}
	} else if ((size_t)(eq - after) == 4 && strncasecmp(after, "NAME", 4) == 0) {
		entry_t *e = entry_for(c, number);
		if (e) {
			size_t n = value_len < sizeof(e->title) - 1 ? value_len : sizeof(e->title) - 1;
			memcpy(e->title, value, n);
			e->title[n] = '\0';
		}
	}
}

static void flac_comments(void *user, drflac_metadata *meta) {
	if (meta->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
		return;
	}
	drflac_vorbis_comment_iterator iter;
	drflac_init_vorbis_comment_iterator(&iter, meta->data.vorbis_comment.commentCount, meta->data.vorbis_comment.pComments);
	drflac_uint32 len;
	const char *comment;
	while ((comment = drflac_next_vorbis_comment(&iter, &len)) != NULL) {
		take_comment(user, comment, len);
	}
}

// Long comments are pictures, never chapters: skipped unread.
#define CHAPTER_TEXT_MAX (64 * 1024)

static bool ogg_comment(void *user, const char *comment, size_t len) {
	take_comment((collect_t *)user, comment, len);
	return true;
}

static int by_start(const void *a, const void *b) {
	const entry_t *x = a;
	const entry_t *y = b;
	if (x->start != y->start) {
		return x->start < y->start ? -1 : 1;
	}
	return x->number < y->number ? -1 : (x->number > y->number);
}

int vorbischap_read(const char *path, vorbischap_t *out, int max) {
	if (!path) {
		return 0;
	}
	collect_t c = {0};
	if (has_extension(path, ".opus") || has_extension(path, ".ogg") || has_extension(path, ".oga")) {
		oggtags_walk(path, CHAPTER_TEXT_MAX, ogg_comment, NULL, &c);
	} else if (has_extension(path, ".flac")) {
		drflac *flac = drflac_open_file_with_metadata(path, flac_comments, &c, NULL);
		if (flac) {
			drflac_close(flac);
		}
	}

	// Only the chapters with a time; a name alone marks nothing.
	int kept = 0;
	for (int i = 0; i < c.count; i++) {
		if (c.items[i].start >= 0) {
			c.items[kept++] = c.items[i];
		}
	}
	if (kept > 1) {
		qsort(c.items, (size_t)kept, sizeof(*c.items), by_start);
	}
	for (int i = 0; out && i < kept && i < max; i++) {
		out[i].start = c.items[i].start;
		snprintf(out[i].title, sizeof(out[i].title), "%s", c.items[i].title);
	}
	free(c.items);
	return kept;
}

bool vorbischap_present(const char *path) { return vorbischap_read(path, NULL, 0) > 0; }
