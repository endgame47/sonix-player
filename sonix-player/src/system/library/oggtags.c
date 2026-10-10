#include "oggtags.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

// An Ogg page: "OggS", version, header type, granule position (8), serial (4),
// sequence (4), CRC (4), segment count, then that many lacing values and the
// data. A packet is a run of segments ended by one shorter than 255 bytes, and
// may continue onto the next page of its stream.
#define OGG_HEADER_BYTES 27
#define OGG_HEADER_TYPE_BOS 0x02

// How far into the file the first page of the stream is looked for, and how
// many comments a header may claim: past either the file is not taken for one.
#define OGG_MAX_BOS_PAGES 16
#define OGG_MAX_COMMENTS 100000

// Longest key looked at in a long comment; a key is a few dozen ASCII letters.
#define OGG_KEY_MAX 64

struct oggtags_value {
	FILE *f;
	uint32_t serial;
	uint8_t lacing[255];
	int segments; // on the current page
	int segment;  // the one being read
	uint32_t in_segment; // bytes of it still unread
	bool packet_over;
	bool broken;
	off_t skip; // bytes passed over but not yet seeked past: one seek per page
	uint32_t left; // bytes of the value handed to `large` still unread
};

static uint32_t le32(const uint8_t *b) {
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static bool skip_flush(oggtags_value_t *r) {
	if (r->skip && fseeko(r->f, r->skip, SEEK_CUR) != 0) {
		return false;
	}
	r->skip = 0;
	return true;
}

// Reads the header of the next page of the stream, skipping the pages of any
// other stream multiplexed with it.
static bool page_next(oggtags_value_t *r) {
	if (!skip_flush(r)) {
		return false;
	}
	for (;;) {
		uint8_t h[OGG_HEADER_BYTES];
		if (fread(h, 1, sizeof(h), r->f) != sizeof(h) || memcmp(h, "OggS", 4) != 0) {
			return false;
		}
		int segments = h[26];
		if (fread(r->lacing, 1, (size_t)segments, r->f) != (size_t)segments) {
			return false;
		}
		if (le32(h + 14) == r->serial) {
			r->segments = segments;
			r->segment = 0;
			r->in_segment = segments > 0 ? r->lacing[0] : 0;
			return true;
		}
		long data = 0;
		for (int i = 0; i < segments; i++) {
			data += r->lacing[i];
		}
		if (fseeko(r->f, (off_t)data, SEEK_CUR) != 0) {
			return false;
		}
	}
}

// Reads `n` bytes of the packet into `buf`, or skips them when `buf` is NULL.
// Returns how many: fewer at the end of the packet or of the file.
static size_t packet_read(oggtags_value_t *r, void *buf, size_t n) {
	size_t done = 0;
	while (done < n && !r->broken) {
		if (r->in_segment == 0) {
			if (r->packet_over) {
				break;
			}
			// The segment just finished: a short one ends the packet.
			if (r->segment < r->segments && r->lacing[r->segment] < 255) {
				r->packet_over = true;
				break;
			}
			if (++r->segment >= r->segments) {
				if (!page_next(r)) {
					r->broken = true;
					break;
				}
			} else {
				r->in_segment = r->lacing[r->segment];
			}
			continue;
		}
		size_t take = n - done < r->in_segment ? n - done : r->in_segment;
		if (buf) {
			if (!skip_flush(r) || fread((uint8_t *)buf + done, 1, take, r->f) != take) {
				r->broken = true;
				break;
			}
		} else {
			r->skip += (off_t)take;
		}
		r->in_segment -= (uint32_t)take;
		done += take;
	}
	return done;
}

size_t oggtags_value_read(oggtags_value_t *value, void *buf, size_t n) {
	if (!value || !buf) {
		return 0;
	}
	if (n > value->left) {
		n = value->left;
	}
	size_t got = packet_read(value, buf, n);
	value->left -= (uint32_t)got;
	return got;
}

// Finds the stream's first page and the codec it carries. The identification
// header is alone on that page, so the comment header starts on the stream's
// next page.
static bool stream_find(oggtags_value_t *r, bool *opus) {
	for (int n = 0; n < OGG_MAX_BOS_PAGES; n++) {
		uint8_t h[OGG_HEADER_BYTES];
		if (fread(h, 1, sizeof(h), r->f) != sizeof(h) || memcmp(h, "OggS", 4) != 0 ||
			!(h[5] & OGG_HEADER_TYPE_BOS)) {
			return false;
		}
		int segments = h[26];
		if (fread(r->lacing, 1, (size_t)segments, r->f) != (size_t)segments) {
			return false;
		}
		long data = 0;
		for (int i = 0; i < segments; i++) {
			data += r->lacing[i];
		}
		uint8_t id[8] = {0};
		size_t peek = data < (long)sizeof(id) ? (size_t)data : sizeof(id);
		if (fread(id, 1, peek, r->f) != peek || fseeko(r->f, (off_t)(data - (long)peek), SEEK_CUR) != 0) {
			return false;
		}
		bool is_opus = peek == 8 && memcmp(id, "OpusHead", 8) == 0;
		bool is_vorbis = peek >= 7 && memcmp(id, "\x01vorbis", 7) == 0;
		if (is_opus || is_vorbis) {
			r->serial = le32(h + 14);
			*opus = is_opus;
			return true;
		}
	}
	return false;
}

bool oggtags_key_is_picture(const char *key, size_t key_len) {
	return (key_len == 22 && strncasecmp(key, "METADATA_BLOCK_PICTURE", 22) == 0) ||
		   (key_len == 8 && strncasecmp(key, "COVERART", 8) == 0);
}

static bool read_le32(oggtags_value_t *r, uint32_t *out) {
	uint8_t b[4];
	if (packet_read(r, b, 4) != 4) {
		return false;
	}
	*out = le32(b);
	return true;
}

bool oggtags_walk(const char *path, size_t max_text, bool (*small)(void *user, const char *comment, size_t len),
				  bool (*large)(void *user, const char *key, size_t key_len, uint32_t value_len,
								oggtags_value_t *value),
				  void *user) {
	FILE *f = path ? fopen(path, "rb") : NULL;
	if (!f) {
		return false;
	}
	oggtags_value_t r;
	memset(&r, 0, sizeof(r));
	r.f = f;

	bool opus = false;
	bool ok = stream_find(&r, &opus) && page_next(&r);

	// "OpusTags", or packet type 3 and "vorbis"; then the vendor string.
	uint8_t magic[8];
	size_t magic_len = opus ? 8 : 7;
	uint32_t vendor = 0, count = 0;
	ok = ok && packet_read(&r, magic, magic_len) == magic_len &&
		 memcmp(magic, opus ? "OpusTags" : "\x03vorbis", magic_len) == 0 && read_le32(&r, &vendor) &&
		 packet_read(&r, NULL, vendor) == vendor && read_le32(&r, &count) && count <= OGG_MAX_COMMENTS;

	for (uint32_t i = 0; ok && i < count; i++) {
		uint32_t len = 0;
		if (!read_le32(&r, &len)) {
			break;
		}
		if (len <= max_text) {
			char *text = malloc((size_t)len + 1);
			if (!text) {
				break;
			}
			bool whole = packet_read(&r, text, len) == len;
			text[len] = '\0';
			bool go_on = whole && (!small || small(user, text, len));
			free(text);
			if (!go_on) {
				break;
			}
			continue;
		}

		// A long one: the key first, a byte at a time up to the '='.
		char key[OGG_KEY_MAX];
		size_t key_len = 0;
		uint32_t read = 0;
		bool has_key = false;
		while (read < len && key_len < sizeof(key)) {
			char c;
			if (packet_read(&r, &c, 1) != 1) {
				break;
			}
			read++;
			if (c == '=') {
				has_key = true;
				break;
			}
			key[key_len++] = c;
		}
		r.left = len - read;
		if (has_key && large && !large(user, key, key_len, r.left, &r)) {
			break;
		}
		if (packet_read(&r, NULL, r.left) != r.left) {
			break;
		}
		r.left = 0;
	}

	fclose(f);
	return ok;
}
