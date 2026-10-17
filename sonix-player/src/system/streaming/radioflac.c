#include "radioflac.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// The declarations only: the implementation is compiled once in decode.c.
#include "src/system/decode/dr_flac.h"

// How far into the stream the first frame (in Ogg, the first FLAC header) is
// looked for before the stream is given up on.
#define SEARCH_LIMIT_BYTES (1024 * 1024)

// 27 header bytes, 255 lacing values and 255 segments of 255 bytes.
#define OGG_PAGE_MAX (27 + 255 + 255 * 255)

// Taken from the source per read while a page is being put together.
#define OGG_READ_CHUNK 8192

// A packet longer than this is not a FLAC frame and is dropped.
#define PACKET_MAX (1024 * 1024)

// The first packet of an Ogg FLAC stream: 0x7F, "FLAC", the mapping's version
// (two bytes), the header packet count (two), then "fLaC" and the STREAMINFO
// block with its four-byte header.
#define OGG_FLAC_FIRST_PACKET 51
#define OGG_FLAC_NATIVE_AT 9

// "fLaC" and the STREAMINFO block: how each link reaches the decoder.
#define NATIVE_HEADER_BYTES 42

#define CHANNELS_MAX 8

struct radioflac {
	radioflac_read_fn read;
	void *user;
	bool ogg;

	drflac *dec;
	bool source_ended;
	bool opened_once;

	// Bytes taken from the source while a decoder is being opened, counted
	// against SEARCH_LIMIT_BYTES.
	bool opening;
	long opening_bytes;

	int rate, channels, bits;

	char title[256];
	bool title_new;

	// Ogg only from here on.

	// Source bytes not yet taken as a page.
	unsigned char *in;
	int in_len;

	// The native stream being handed to the decoder, `out_at` bytes of it read.
	unsigned char *out;
	size_t out_len, out_at, out_cap;

	// The packet being put together across pages.
	unsigned char *packet;
	size_t packet_len, packet_cap;
	bool packet_open;	 // its start has been seen
	bool packet_dropped; // past PACKET_MAX: the rest of it is thrown away

	uint32_t serial;
	bool have_serial;
	uint32_t sequence;

	// The current link's header has gone to the decoder. A header after it
	// starts the next link: it is held, and the decoder is told the stream has
	// ended, until the decoder has played out this one.
	bool link_started;
	bool link_ended;
	unsigned char next_header[NATIVE_HEADER_BYTES];
	bool next_header_held;
};

static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// ---------------------------------------------------------------------------
// Comments
// ---------------------------------------------------------------------------

// The value of `key` ("TITLE=") in one comment, into `out` when it is still
// empty: the first of several comments with the same key is the one kept.
static void comment_field(const char *comment, size_t len, const char *key, char *out, size_t size) {
	size_t key_len = strlen(key);
	if (out[0] || len <= key_len || strncasecmp(comment, key, key_len) != 0) {
		return;
	}
	size_t n = len - key_len;
	if (n >= size) {
		n = size - 1;
	}
	memcpy(out, comment + key_len, n);
	out[n] = '\0';
}

// A VORBIS_COMMENT block's body: the vendor string, a count, then the
// comments, every length a little-endian 32-bit word.
static void take_comments(radioflac_t *f, const unsigned char *p, size_t len) {
	if (len < 4) {
		return;
	}
	uint32_t vendor = le32(p);
	if (vendor > len - 4) {
		return;
	}
	size_t at = 4 + vendor;
	if (len - at < 4) {
		return;
	}
	uint32_t count = le32(p + at);
	at += 4;

	char artist[128] = "";
	char title[128] = "";
	for (uint32_t i = 0; i < count && len - at >= 4; i++) {
		uint32_t n = le32(p + at);
		at += 4;
		if (n > len - at) {
			break;
		}
		comment_field((const char *)p + at, n, "ARTIST=", artist, sizeof(artist));
		comment_field((const char *)p + at, n, "TITLE=", title, sizeof(title));
		at += n;
	}

	char line[sizeof(f->title)];
	if (artist[0] && title[0]) {
		snprintf(line, sizeof(line), "%s - %s", artist, title);
	} else if (title[0] || artist[0]) {
		snprintf(line, sizeof(line), "%s", title[0] ? title : artist);
	} else {
		return;
	}
	if (strcmp(line, f->title) != 0) {
		snprintf(f->title, sizeof(f->title), "%s", line);
		f->title_new = true;
	}
}

static void on_meta(void *user, drflac_metadata *m) {
	radioflac_t *f = user;
	if (m->type == DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT && m->pRawData) {
		take_comments(f, m->pRawData, m->rawDataSize);
	}
}

// ---------------------------------------------------------------------------
// The source
// ---------------------------------------------------------------------------

static int source_read(radioflac_t *f, unsigned char *dst, int max) {
	if (f->source_ended || (f->opening && f->opening_bytes >= SEARCH_LIMIT_BYTES)) {
		return 0;
	}
	int n = f->read(f->user, dst, max);
	if (n <= 0) {
		f->source_ended = true;
		return 0;
	}
	if (f->opening) {
		f->opening_bytes += n;
	}
	return n;
}

// dr_flac takes a short read for the end of the stream, so every read is
// filled whole unless the source has ended.
static size_t fill_native(radioflac_t *f, unsigned char *dst, size_t want) {
	size_t got = 0;
	while (got < want) {
		size_t room = want - got;
		int n = source_read(f, dst + got, room > (size_t)(1 << 30) ? (1 << 30) : (int)room);
		if (n <= 0) {
			break;
		}
		got += (size_t)n;
	}
	return got;
}

// ---------------------------------------------------------------------------
// Ogg
// ---------------------------------------------------------------------------

static uint32_t ogg_crc_table[256];

static void ogg_crc_init(void) {
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t r = i << 24;
		for (int b = 0; b < 8; b++) {
			r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : r << 1;
		}
		ogg_crc_table[i] = r;
	}
}

// The page's checksum is taken over the whole page with its own field, bytes
// 22 to 25, read as zero.
static bool ogg_crc_ok(const unsigned char *page, size_t len) {
	uint32_t crc = 0;
	for (size_t i = 0; i < len; i++) {
		unsigned char byte = (i >= 22 && i < 26) ? 0 : page[i];
		crc = (crc << 8) ^ ogg_crc_table[((crc >> 24) ^ byte) & 0xFF];
	}
	return crc == le32(page + 22);
}

static bool out_append(radioflac_t *f, const unsigned char *data, size_t len) {
	if (f->out_len + len > f->out_cap) {
		size_t cap = f->out_cap ? f->out_cap : 65536;
		while (cap < f->out_len + len) {
			cap *= 2;
		}
		unsigned char *grown = realloc(f->out, cap);
		if (!grown) {
			return false;
		}
		f->out = grown;
		f->out_cap = cap;
	}
	memcpy(f->out + f->out_len, data, len);
	f->out_len += len;
	return true;
}

static bool packet_append(radioflac_t *f, const unsigned char *data, size_t len) {
	if (f->packet_len + len > PACKET_MAX) {
		return false;
	}
	if (f->packet_len + len > f->packet_cap) {
		size_t cap = f->packet_cap ? f->packet_cap : 32768;
		while (cap < f->packet_len + len) {
			cap *= 2;
		}
		unsigned char *grown = realloc(f->packet, cap);
		if (!grown) {
			return false;
		}
		f->packet = grown;
		f->packet_cap = cap;
	}
	memcpy(f->packet + f->packet_len, data, len);
	f->packet_len += len;
	return true;
}

static bool is_first_packet(const unsigned char *p, size_t len) { return len >= OGG_FLAC_FIRST_PACKET && p[0] == 0x7F && memcmp(p + 1, "FLAC", 4) == 0 && memcmp(p + OGG_FLAC_NATIVE_AT, "fLaC", 4) == 0 && (p[13] & 0x7F) == DRFLAC_METADATA_BLOCK_TYPE_STREAMINFO; }

// One whole packet of the followed stream.
static void take_packet(radioflac_t *f, const unsigned char *p, size_t len) {
	if (is_first_packet(p, len)) {
		// Marked as the last metadata block: the comments that follow are read
		// here, not by the decoder.
		unsigned char header[NATIVE_HEADER_BYTES];
		memcpy(header, p + OGG_FLAC_NATIVE_AT, NATIVE_HEADER_BYTES);
		header[4] |= 0x80;
		if (f->link_started) {
			memcpy(f->next_header, header, sizeof(header));
			f->next_header_held = true;
			f->link_ended = true;
		} else if (out_append(f, header, sizeof(header))) {
			f->link_started = true;
		}
		return;
	}
	// A frame: the sync code, fourteen ones and a reserved zero.
	if (len >= 2 && p[0] == 0xFF && (p[1] & 0xFE) == 0xF8) {
		if (f->link_started && !f->link_ended) {
			out_append(f, p, len);
		}
		return;
	}
	if (len >= 4 && (p[0] & 0x7F) == DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) {
		take_comments(f, p + 4, len - 4);
	}
}

static void take_page(radioflac_t *f, const unsigned char *page, int head) {
	unsigned type = page[5];
	uint32_t serial = le32(page + 14);
	uint32_t sequence = le32(page + 18);
	int segs = page[26];
	const unsigned char *lacing = page + 27;
	const unsigned char *data = page + head;

	bool gap = false;
	if (type & 0x02) {
		// The start of a logical stream: followed only when it is FLAC, which
		// the mapping puts alone in this page.
		if (segs == 0 || lacing[0] == 255 || !is_first_packet(data, lacing[0])) {
			return;
		}
		f->serial = serial;
		f->have_serial = true;
		f->packet_len = 0;
		f->packet_open = false;
		f->packet_dropped = false;
	} else {
		if (!f->have_serial || serial != f->serial) {
			return;
		}
		gap = sequence != f->sequence + 1;
	}
	f->sequence = sequence;

	size_t at = 0;
	int i = 0;
	bool continued = (type & 0x01) != 0;
	if (!continued || gap) {
		f->packet_len = 0;
		f->packet_open = false;
		f->packet_dropped = false;
	}
	if (continued && !f->packet_open) {
		// The start of this packet was never seen.
		while (i < segs) {
			at += lacing[i];
			if (lacing[i++] < 255) {
				break;
			}
		}
	}

	for (; i < segs; i++) {
		if (!f->packet_dropped && !packet_append(f, data + at, lacing[i])) {
			f->packet_dropped = true;
		}
		f->packet_open = true;
		at += lacing[i];
		if (lacing[i] < 255) {
			if (!f->packet_dropped) {
				take_packet(f, f->packet, f->packet_len);
			}
			f->packet_len = 0;
			f->packet_open = false;
			f->packet_dropped = false;
		}
	}
}

static int find_capture(const unsigned char *p, int len) {
	for (int i = 0; i + 4 <= len; i++) {
		if (p[i] == 'O' && p[i + 1] == 'g' && p[i + 2] == 'g' && p[i + 3] == 'S') {
			return i;
		}
	}
	return -1;
}

static void in_drop(radioflac_t *f, int n) {
	memmove(f->in, f->in + n, (size_t)(f->in_len - n));
	f->in_len -= n;
}

// Reads and takes one page. False when the source has nothing more.
static bool ogg_next_page(radioflac_t *f) {
	for (;;) {
		int at = find_capture(f->in, f->in_len);
		if (at < 0) {
			// The tail could be the start of a capture pattern.
			if (f->in_len > 3) {
				in_drop(f, f->in_len - 3);
			}
		} else if (at > 0) {
			in_drop(f, at);
		}

		if (at >= 0 && f->in_len >= 27) {
			int head = 27 + f->in[26];
			if (f->in_len >= head) {
				int body = 0;
				for (int i = 0; i < f->in[26]; i++) {
					body += f->in[27 + i];
				}
				if (f->in_len >= head + body) {
					if (f->in[4] == 0 && ogg_crc_ok(f->in, (size_t)(head + body))) {
						take_page(f, f->in, head);
						in_drop(f, head + body);
						return true;
					}
					// "OggS" inside other data: looked for again past it.
					in_drop(f, 1);
					continue;
				}
			}
		}

		int room = OGG_PAGE_MAX - f->in_len;
		int n = source_read(f, f->in + f->in_len, room < OGG_READ_CHUNK ? room : OGG_READ_CHUNK);
		if (n <= 0) {
			return false;
		}
		f->in_len += n;
	}
}

static size_t fill_ogg(radioflac_t *f, unsigned char *dst, size_t want) {
	size_t got = 0;
	while (got < want) {
		if (f->out_at < f->out_len) {
			size_t n = f->out_len - f->out_at;
			if (n > want - got) {
				n = want - got;
			}
			memcpy(dst + got, f->out + f->out_at, n);
			f->out_at += n;
			got += n;
			continue;
		}
		f->out_at = 0;
		f->out_len = 0;
		if (f->link_ended || !ogg_next_page(f)) {
			break;
		}
	}
	return got;
}

// ---------------------------------------------------------------------------
// The decoder's callbacks
// ---------------------------------------------------------------------------

static size_t on_read(void *user, void *buf, size_t want) {
	radioflac_t *f = user;
	return f->ogg ? fill_ogg(f, buf, want) : fill_native(f, buf, want);
}

// Forward from where the stream is, which is all dr_flac asks of a stream it
// opens (to step over an ID3 tag or a metadata block).
static drflac_bool32 on_seek(void *user, int offset, drflac_seek_origin origin) {
	radioflac_t *f = user;
	if (origin != DRFLAC_SEEK_CUR || offset < 0) {
		return DRFLAC_FALSE;
	}
	unsigned char scratch[4096];
	while (offset > 0) {
		size_t want = offset < (int)sizeof(scratch) ? (size_t)offset : sizeof(scratch);
		if (on_read(f, scratch, want) != want) {
			return DRFLAC_FALSE;
		}
		offset -= (int)want;
	}
	return DRFLAC_TRUE;
}

// ---------------------------------------------------------------------------
// The API
// ---------------------------------------------------------------------------

radioflac_t *radioflac_open(bool ogg, radioflac_read_fn read, void *user) {
	radioflac_t *f = calloc(1, sizeof(*f));
	if (!f) {
		return NULL;
	}
	f->read = read;
	f->user = user;
	f->ogg = ogg;
	if (ogg) {
		ogg_crc_init();
		f->in = malloc(OGG_PAGE_MAX);
		if (!f->in) {
			free(f);
			return NULL;
		}
	}
	return f;
}

// Opens a decoder on the next link, or the stream. False when there is none.
static bool open_decoder(radioflac_t *f) {
	if (f->ogg && f->link_ended) {
		f->link_ended = false;
		f->link_started = false;
		if (f->next_header_held && out_append(f, f->next_header, NATIVE_HEADER_BYTES)) {
			f->link_started = true;
		}
		f->next_header_held = false;
	}

	f->opening = true;
	f->opening_bytes = 0;
	f->dec = drflac_open_with_metadata_relaxed(on_read, on_seek, NULL, on_meta, drflac_container_native, f, NULL);
	f->opening = false;
	if (!f->dec) {
		return false;
	}

	if (f->dec->sampleRate == 0 || f->dec->channels == 0 || f->dec->channels > CHANNELS_MAX || f->dec->bitsPerSample == 0 || f->dec->bitsPerSample > 32) {
		fprintf(stderr, "radio: FLAC header out of range (%u Hz, %u ch, %u bits)\n", f->dec->sampleRate, f->dec->channels, f->dec->bitsPerSample);
		drflac_close(f->dec);
		f->dec = NULL;
		return false;
	}
	fprintf(stderr, "radio: FLAC %s%u Hz, %u ch, %u bits\n", f->ogg ? "in Ogg, " : "", f->dec->sampleRate, f->dec->channels, f->dec->bitsPerSample);
	return true;
}

int radioflac_read(radioflac_t *f, int32_t *pcm, int max_frames) {
	if (!f || max_frames <= 0) {
		return 0;
	}
	for (;;) {
		if (!f->dec) {
			if (!open_decoder(f)) {
				return f->opened_once ? 0 : -1;
			}
			f->opened_once = true;
		}

		drflac_uint64 n = drflac_read_pcm_frames_s32(f->dec, (drflac_uint64)max_frames, pcm);
		if (n > 0) {
			f->rate = (int)f->dec->sampleRate;
			f->channels = (int)f->dec->channels;
			f->bits = (int)f->dec->bitsPerSample;
			return (int)n;
		}

		drflac_close(f->dec);
		f->dec = NULL;
		if (!f->ogg || !f->link_ended) {
			return 0;
		}
	}
}

void radioflac_format(const radioflac_t *f, int *rate, int *channels, int *bits) {
	*rate = f ? f->rate : 0;
	*channels = f ? f->channels : 0;
	*bits = f ? f->bits : 0;
}

bool radioflac_take_title(radioflac_t *f, char *out, size_t size) {
	if (!f || !f->title_new) {
		return false;
	}
	f->title_new = false;
	snprintf(out, size, "%s", f->title);
	return true;
}

void radioflac_close(radioflac_t *f) {
	if (!f) {
		return;
	}
	if (f->dec) {
		drflac_close(f->dec);
	}
	free(f->in);
	free(f->out);
	free(f->packet);
	free(f);
}
