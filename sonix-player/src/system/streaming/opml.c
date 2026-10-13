#include "opml.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/streaming/podcastdl.h"

// An export from a phone app with a few hundred podcasts is well under a
// hundred kilobytes; anything far larger is not a subscription list.
#define OPML_MAX_BYTES (2 * 1024 * 1024)

// ---------------------------------------------------------------------------
// finding the files
// ---------------------------------------------------------------------------

static bool has_opml_extension(const char *name) {
	const char *dot = strrchr(name, '.');
	return dot && strcasecmp(dot, ".opml") == 0;
}

static int path_cmp(const void *a, const void *b) {
	const char *x = strrchr((const char *)a, '/');
	const char *y = strrchr((const char *)b, '/');
	return strcasecmp(x ? x + 1 : (const char *)a, y ? y + 1 : (const char *)b);
}

static int find_in(const char *dir_path, char (*out)[512], int count, int max) {
	DIR *dir = opendir(dir_path);
	if (!dir) {
		return count;
	}
	struct dirent *de;
	while (count < max && (de = readdir(dir)) != NULL) {
		// Dot files too: a Mac leaves a "._name.opml" beside every file it
		// copies, and that is not a list.
		if (de->d_name[0] == '.' || !has_opml_extension(de->d_name)) {
			continue;
		}
		char full[512];
		if (snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name) >= (int)sizeof(full)) {
			continue;
		}
		struct stat st;
		if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		snprintf(out[count++], 512, "%s", full);
	}
	closedir(dir);
	return count;
}

int opml_find_files(const char *card_root, char (*out)[512], int max) {
	if (!card_root || !card_root[0] || !out || max <= 0) {
		return 0;
	}
	int count = find_in(card_root, out, 0, max);
	char folder[512];
	snprintf(folder, sizeof(folder), "%.480s/%s", card_root, PODCASTDL_FOLDER);
	count = find_in(folder, out, count, max);
	qsort(out, (size_t)count, sizeof(out[0]), path_cmp);
	return count;
}

// ---------------------------------------------------------------------------
// reading
// ---------------------------------------------------------------------------

static void put_utf8(unsigned long cp, char **at, const char *end) {
	char buf[4];
	int n;
	if (cp < 0x80) {
		buf[0] = (char)cp;
		n = 1;
	} else if (cp < 0x800) {
		buf[0] = (char)(0xC0 | (cp >> 6));
		buf[1] = (char)(0x80 | (cp & 0x3F));
		n = 2;
	} else if (cp < 0x10000) {
		buf[0] = (char)(0xE0 | (cp >> 12));
		buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		buf[2] = (char)(0x80 | (cp & 0x3F));
		n = 3;
	} else if (cp < 0x110000) {
		buf[0] = (char)(0xF0 | (cp >> 18));
		buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		buf[3] = (char)(0x80 | (cp & 0x3F));
		n = 4;
	} else {
		return;
	}
	if (*at + n >= end) {
		return;
	}
	memcpy(*at, buf, (size_t)n);
	*at += n;
}

// An attribute value from `from` to `to`, entities resolved, into `out`.
static void unescape(const char *from, const char *to, char *out, size_t size) {
	char *at = out;
	const char *end = out + size;
	while (from < to && at + 1 < end) {
		if (*from != '&') {
			*at++ = *from++;
			continue;
		}
		const char *semi = memchr(from, ';', (size_t)(to - from));
		if (!semi || semi - from > 10) {
			*at++ = *from++;
			continue;
		}
		size_t len = (size_t)(semi - from - 1);
		const char *name = from + 1;
		unsigned long cp = 0;
		bool known = true;
		if (len == 3 && strncmp(name, "amp", 3) == 0) {
			cp = '&';
		} else if (len == 2 && strncmp(name, "lt", 2) == 0) {
			cp = '<';
		} else if (len == 2 && strncmp(name, "gt", 2) == 0) {
			cp = '>';
		} else if (len == 4 && strncmp(name, "quot", 4) == 0) {
			cp = '"';
		} else if (len == 4 && strncmp(name, "apos", 4) == 0) {
			cp = '\'';
		} else if (len >= 2 && name[0] == '#') {
			char number[12];
			snprintf(number, sizeof(number), "%.*s", (int)len - 1, name + 1);
			if (number[0] == 'x' || number[0] == 'X') {
				cp = strtoul(number + 1, NULL, 16);
			} else {
				cp = strtoul(number, NULL, 10);
			}
			known = cp > 0;
		} else {
			known = false;
		}
		if (!known) {
			*at++ = *from++;
			continue;
		}
		put_utf8(cp, &at, end);
		from = semi + 1;
	}
	*at = '\0';

	// Leading and trailing white space, which a hand-edited file often has.
	char *start = out;
	while (*start && isspace((unsigned char)*start)) {
		start++;
	}
	if (start != out) {
		memmove(out, start, strlen(start) + 1);
	}
	size_t n = strlen(out);
	while (n > 0 && isspace((unsigned char)out[n - 1])) {
		out[--n] = '\0';
	}
}

// The value of attribute `want` inside the tag text [tag, tag_end), case
// ignored in the name. False when the tag does not have it.
static bool attribute(const char *tag, const char *tag_end, const char *want, char *out, size_t size) {
	size_t want_len = strlen(want);
	const char *p = tag;
	while (p < tag_end) {
		while (p < tag_end && (isspace((unsigned char)*p) || *p == '/')) {
			p++;
		}
		const char *name = p;
		while (p < tag_end && !isspace((unsigned char)*p) && *p != '=' && *p != '>' && *p != '/') {
			p++;
		}
		size_t name_len = (size_t)(p - name);
		while (p < tag_end && isspace((unsigned char)*p)) {
			p++;
		}
		if (p >= tag_end || *p != '=') {
			if (name_len == 0) {
				p++;
			}
			continue;
		}
		p++;
		while (p < tag_end && isspace((unsigned char)*p)) {
			p++;
		}
		const char *value;
		const char *value_end;
		if (p < tag_end && (*p == '"' || *p == '\'')) {
			char quote = *p++;
			value = p;
			while (p < tag_end && *p != quote) {
				p++;
			}
			value_end = p;
			if (p < tag_end) {
				p++;
			}
		} else {
			value = p;
			while (p < tag_end && !isspace((unsigned char)*p) && *p != '>') {
				p++;
			}
			value_end = p;
		}
		if (name_len == want_len && strncasecmp(name, want, want_len) == 0) {
			unescape(value, value_end, out, size);
			return true;
		}
	}
	return false;
}

int opml_read(const char *path, opml_feed_t *out, int max) {
	if (!path || !out || max <= 0) {
		return -1;
	}
	FILE *f = fopen(path, "rb");
	if (!f) {
		return -1;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return -1;
	}
	long size = ftell(f);
	if (size <= 0 || size > OPML_MAX_BYTES) {
		fclose(f);
		return size == 0 ? 0 : -1;
	}
	rewind(f);
	char *text = malloc((size_t)size + 1);
	if (!text) {
		fclose(f);
		return -1;
	}
	size_t got = fread(text, 1, (size_t)size, f);
	fclose(f);
	text[got] = '\0';

	int count = 0;
	const char *p = text;
	const char *end = text + got;
	while (count < max && p < end) {
		const char *open = strchr(p, '<');
		if (!open) {
			break;
		}
		if (strncasecmp(open + 1, "outline", 7) != 0 ||
			!(isspace((unsigned char)open[8]) || open[8] == '/' || open[8] == '>')) {
			p = open + 1;
			continue;
		}
		// The tag's end: the first '>' outside quotes.
		const char *q = open + 8;
		char quote = 0;
		while (q < end && (quote || *q != '>')) {
			if (quote && *q == quote) {
				quote = 0;
			} else if (!quote && (*q == '"' || *q == '\'')) {
				quote = *q;
			}
			q++;
		}
		opml_feed_t feed;
		memset(&feed, 0, sizeof(feed));
		if (attribute(open + 8, q, "xmlUrl", feed.url, sizeof(feed.url)) && feed.url[0]) {
			if (!attribute(open + 8, q, "title", feed.title, sizeof(feed.title)) || !feed.title[0]) {
				attribute(open + 8, q, "text", feed.title, sizeof(feed.title));
			}
			if (!feed.title[0]) {
				snprintf(feed.title, sizeof(feed.title), "%.*s", (int)sizeof(feed.title) - 1, feed.url);
			}
			bool seen = false;
			for (int i = 0; i < count && !seen; i++) {
				seen = strcmp(out[i].url, feed.url) == 0;
			}
			if (!seen) {
				out[count++] = feed;
			}
		}
		p = q < end ? q + 1 : end;
	}
	free(text);
	return count;
}

// ---------------------------------------------------------------------------
// writing
// ---------------------------------------------------------------------------

static void write_escaped(FILE *f, const char *text) {
	for (const char *c = text; *c; c++) {
		switch (*c) {
		case '&':
			fputs("&amp;", f);
			break;
		case '<':
			fputs("&lt;", f);
			break;
		case '>':
			fputs("&gt;", f);
			break;
		case '"':
			fputs("&quot;", f);
			break;
		default:
			fputc(*c, f);
			break;
		}
	}
}

bool opml_write(const char *path, const opml_feed_t *feeds, int count) {
	if (!path || !path[0] || (!feeds && count > 0)) {
		return false;
	}
	char tmp[560];
	snprintf(tmp, sizeof(tmp), "%.540s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return false;
	}
	fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", f);
	fputs("<opml version=\"2.0\">\n", f);
	fputs("  <head>\n    <title>Sonix Player</title>\n  </head>\n", f);
	fputs("  <body>\n", f);
	for (int i = 0; i < count; i++) {
		fputs("    <outline type=\"rss\" text=\"", f);
		write_escaped(f, feeds[i].title);
		fputs("\" title=\"", f);
		write_escaped(f, feeds[i].title);
		fputs("\" xmlUrl=\"", f);
		write_escaped(f, feeds[i].url);
		fputs("\"/>\n", f);
	}
	fputs("  </body>\n</opml>\n", f);
	bool ok = fflush(f) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path) != 0) {
		remove(tmp);
		return false;
	}
	sync();
	return true;
}
