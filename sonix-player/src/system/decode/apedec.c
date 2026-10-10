#include "apedec.h"

#include "ape/apecore.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

#define MAC_FORMAT_FLAG_8_BIT 1
#define MAC_FORMAT_FLAG_HAS_PEAK_LEVEL 4
#define MAC_FORMAT_FLAG_24_BIT 8
#define MAC_FORMAT_FLAG_HAS_SEEK_ELEMENTS 16
#define MAC_FORMAT_FLAG_CREATE_WAV_HEADER 32

// The byte window a frame streams through. Every refill leaves at least 32 KB
// of new data after whatever the decoder had not consumed yet.
#define WINDOW_BYTES (64 * 1024)

// A frame decoded whole (file versions before 3.93) holds at most 73728 blocks,
// well under a megabyte compressed; a seek table asking for more is broken.
#define WHOLE_FRAME_MAX_BYTES (8 * 1024 * 1024)

// Far above anything an encoder writes -- an hour at 44.1 kHz is about 540
// frames -- and small enough that the table can always be allocated.
#define MAX_FRAMES (1u << 20)

typedef struct {
	int64_t pos;  // where the frame's bytes start, aligned to the word grid
	int64_t size; // bytes from pos, a multiple of four
	uint32_t skip; // offset of the frame into those bytes (bits before 3.81)
} ape_frame_t;

typedef struct {
	int fileversion;
	int compression;
	int flags;
	int bps;
	int channels;
	int samplerate;
	uint32_t blocksperframe;
	uint32_t finalframeblocks;
	uint32_t totalframes;
	uint64_t totalsamples;
	ape_frame_t *frames; // NULL when only the header was asked for
} ape_info_t;

struct apedec {
	FILE *f;
	ape_info_t info;
	ape_core_t *core;
	bool whole; // frames decoded in one call (before 3.93)

	// The frame being decoded.
	uint32_t frame;			// index of the next frame to start when frame_left is 0
	uint32_t frame_left;	// blocks of it still to come
	bool broken;			// the rest of it plays as silence
	int64_t read_pos;		// next file byte of the frame to read
	int64_t read_left;		// file bytes of the frame not read yet
	bool pad_pending;		// two zero bytes still owed at its end (before 3.95)

	uint8_t *window; // WINDOW_BYTES, or the whole frame in whole mode
	size_t window_cap;

	// The chunk being handed out.
	const int32_t *left;
	const int32_t *right;
	uint32_t avail;
	uint32_t used;

	int32_t *silence; // APE_CORE_CHUNK zeros
	uint64_t position;
	char name[96]; // for the log
};

static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t le32(const unsigned char *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int64_t file_size_of(FILE *f) {
	if (fseeko(f, 0, SEEK_END) != 0) {
		return -1;
	}
	off_t size = ftello(f);
	return size < 0 ? -1 : (int64_t)size;
}

// ---------------------------------------------------------------------------
// The header
// ---------------------------------------------------------------------------

// Bytes of ID3v2 in front of the stream, 0 when there is none. Several tags in
// a row are allowed.
static int64_t id3v2_length(FILE *f) {
	int64_t at = 0;
	for (int n = 0; n < 4; n++) {
		unsigned char h[10];
		if (fseeko(f, (off_t)at, SEEK_SET) != 0 || fread(h, 1, sizeof(h), f) != sizeof(h)) {
			break;
		}
		if (memcmp(h, "ID3", 3) != 0 || (h[6] | h[7] | h[8] | h[9]) & 0x80) {
			break;
		}
		int64_t len = ((int64_t)h[6] << 21) | ((int64_t)h[7] << 14) | ((int64_t)h[8] << 7) | h[9];
		at += 10 + len + ((h[5] & 0x10) ? 10 : 0);
	}
	return at;
}

// Reads the descriptor, the header and, with `want_frames`, the seek table into
// the frame list. The layout and the frame arithmetic are FFmpeg's ape.c.
static bool parse_header(FILE *f, const char *name, ape_info_t *info, bool want_frames) {
	memset(info, 0, sizeof(*info));

	int64_t file_size = file_size_of(f);
	int64_t junk = id3v2_length(f);
	if (file_size <= 0 || junk >= file_size || fseeko(f, (off_t)junk, SEEK_SET) != 0) {
		return false;
	}

	unsigned char b[64];
	if (fread(b, 1, 6, f) != 6 || memcmp(b, "MAC ", 4) != 0) {
		fprintf(stderr, "ape: %s is not a Monkey's Audio file\n", name);
		return false;
	}
	int version = (int16_t)le16(b + 4);
	if (version < 3800 || version > 3990) {
		fprintf(stderr, "ape: %s is file version %d, which is not supported\n", name, version);
		return false;
	}

	uint32_t descriptorlength = 0, headerlength, seektablelength, wavheaderlength, wavtaillength;
	uint16_t compression, flags, bps, channels;
	uint32_t blocksperframe, finalframeblocks, totalframes, samplerate;

	if (version >= 3980) {
		if (fread(b, 1, 46, f) != 46) {
			return false;
		}
		descriptorlength = le32(b + 2);
		headerlength = le32(b + 6);
		seektablelength = le32(b + 10);
		wavheaderlength = le32(b + 14);
		wavtaillength = le32(b + 26);
		if (descriptorlength < 52 || descriptorlength > 4096 || headerlength < 24 || headerlength > 4096) {
			return false;
		}
		if (descriptorlength > 52 && fseeko(f, descriptorlength - 52, SEEK_CUR) != 0) {
			return false;
		}
		if (fread(b, 1, 24, f) != 24) {
			return false;
		}
		compression = le16(b);
		flags = le16(b + 2);
		blocksperframe = le32(b + 4);
		finalframeblocks = le32(b + 8);
		totalframes = le32(b + 12);
		bps = le16(b + 16);
		channels = le16(b + 18);
		samplerate = le32(b + 20);
		if (fseeko(f, (off_t)(junk + descriptorlength + headerlength), SEEK_SET) != 0) {
			return false;
		}
	} else {
		headerlength = 32;
		if (fread(b, 1, 26, f) != 26) {
			return false;
		}
		compression = le16(b);
		flags = le16(b + 2);
		channels = le16(b + 4);
		samplerate = le32(b + 6);
		wavheaderlength = le32(b + 10);
		wavtaillength = le32(b + 14);
		totalframes = le32(b + 18);
		finalframeblocks = le32(b + 22);

		if (flags & MAC_FORMAT_FLAG_HAS_PEAK_LEVEL) {
			if (fseeko(f, 4, SEEK_CUR) != 0) {
				return false;
			}
			headerlength += 4;
		}
		if (flags & MAC_FORMAT_FLAG_HAS_SEEK_ELEMENTS) {
			if (fread(b, 1, 4, f) != 4) {
				return false;
			}
			seektablelength = le32(b);
			if (seektablelength > UINT32_MAX / 4) {
				return false;
			}
			seektablelength *= 4;
			headerlength += 4;
		} else {
			if (totalframes > UINT32_MAX / 4) {
				return false;
			}
			seektablelength = totalframes * 4;
		}

		if (flags & MAC_FORMAT_FLAG_8_BIT) {
			bps = 8;
		} else if (flags & MAC_FORMAT_FLAG_24_BIT) {
			bps = 24;
		} else {
			bps = 16;
		}

		if (version >= 3950) {
			blocksperframe = 73728 * 4;
		} else if (version >= 3900 || (version >= 3800 && compression >= 4000)) {
			blocksperframe = 73728;
		} else {
			blocksperframe = 9216;
		}

		// Skip any stored wav header
		if (!(flags & MAC_FORMAT_FLAG_CREATE_WAV_HEADER) && fseeko(f, wavheaderlength, SEEK_CUR) != 0) {
			return false;
		}
	}

	if (!totalframes || totalframes > MAX_FRAMES) {
		fprintf(stderr, "ape: %s declares %u frames\n", name, (unsigned)totalframes);
		return false;
	}
	if (seektablelength / 4 < totalframes) {
		fprintf(stderr, "ape: %s has %u seek entries for %u frames\n", name, (unsigned)(seektablelength / 4),
				(unsigned)totalframes);
		return false;
	}
	if (channels < 1 || channels > 2 || (bps != 8 && bps != 16 && bps != 24) || samplerate < 1 ||
		samplerate > 768000 || !blocksperframe || blocksperframe > (1u << 24) || !finalframeblocks ||
		finalframeblocks > blocksperframe) {
		fprintf(stderr, "ape: %s declares %u ch, %u bit, %u Hz, %u blocks per frame, %u in the last -- no good\n",
				name, (unsigned)channels, (unsigned)bps, (unsigned)samplerate, (unsigned)blocksperframe,
				(unsigned)finalframeblocks);
		return false;
	}

	info->fileversion = version;
	info->compression = compression;
	info->flags = flags;
	info->bps = bps;
	info->channels = channels;
	info->samplerate = (int)samplerate;
	info->blocksperframe = blocksperframe;
	info->finalframeblocks = finalframeblocks;
	info->totalframes = totalframes;
	info->totalsamples = (uint64_t)blocksperframe * (totalframes - 1) + finalframeblocks;

	if (!want_frames) {
		return true;
	}

	int64_t firstframe =
		junk + (int64_t)descriptorlength + headerlength + (int64_t)seektablelength + wavheaderlength;
	if (version < 3810) {
		firstframe += totalframes;
	}
	int64_t seektable_at = ftello(f);

	uint32_t *table = malloc((size_t)totalframes * sizeof(*table));
	ape_frame_t *frames = calloc(totalframes, sizeof(*frames));
	if (!table || !frames) {
		free(table);
		free(frames);
		return false;
	}
	unsigned char *raw = (unsigned char *)table;
	if (fread(raw, 4, totalframes, f) != totalframes) {
		fprintf(stderr, "ape: %s: the seek table is cut short\n", name);
		free(table);
		free(frames);
		return false;
	}
	for (uint32_t i = 0; i < totalframes; i++) {
		table[i] = le32(raw + (size_t)i * 4);
	}

	frames[0].pos = firstframe;
	frames[0].skip = 0;
	for (uint32_t i = 1; i < totalframes; i++) {
		frames[i].pos = (int64_t)table[i] + junk;
		frames[i - 1].size = frames[i].pos - frames[i - 1].pos;
		frames[i].skip = (uint32_t)((frames[i].pos - frames[0].pos) & 3);
	}
	free(table);

	int64_t final_size = file_size - frames[totalframes - 1].pos - (int64_t)wavtaillength;
	final_size -= final_size & 3;
	if (final_size <= 0) {
		final_size = (int64_t)finalframeblocks * 8;
	}
	frames[totalframes - 1].size = final_size;

	for (uint32_t i = 0; i < totalframes; i++) {
		if (frames[i].skip) {
			frames[i].pos -= frames[i].skip;
			frames[i].size += frames[i].skip;
		}
		// A negative or absurd size marks a broken frame; it plays as silence.
		if (frames[i].size > INT_MAX - 3) {
			frames[i].size = -1;
		}
		if (frames[i].size > 0) {
			frames[i].size = (frames[i].size + 3) & ~3;
		}
	}

	if (version < 3810) {
		if (fseeko(f, (off_t)(seektable_at + seektablelength), SEEK_SET) != 0) {
			free(frames);
			return false;
		}
		for (uint32_t i = 0; i < totalframes; i++) {
			int bits = fgetc(f);
			if (bits == EOF) {
				fprintf(stderr, "ape: %s: the bit table is cut short\n", name);
				free(frames);
				return false;
			}
			if (i && bits && frames[i - 1].size > 0) {
				frames[i - 1].size += 4;
			}
			frames[i].skip <<= 3;
			frames[i].skip += (uint32_t)bits;
		}
	}

	info->frames = frames;
	return true;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

static uint32_t frame_blocks(const apedec_t *a, uint32_t i) {
	return i + 1 == a->info.totalframes ? a->info.finalframeblocks : a->info.blocksperframe;
}

// Tops the window up with the next bytes of the frame, keeping what the decoder
// has not consumed. Reads come in whole words, so the word grid of the frame
// carries over from one refill to the next.
static bool refill(ape_core_t *core, void *arg) {
	apedec_t *a = arg;
	const uint8_t *ptr, *end;
	ape_core_window(core, &ptr, &end);

	size_t keep = (size_t)(end - ptr);
	if (keep > a->window_cap / 2) {
		return false; // the decoder is not consuming; nothing sensible to add
	}
	memmove(a->window, ptr, keep);

	size_t room = (a->window_cap - keep - 2) & ~(size_t)3;
	size_t want = a->read_left < (int64_t)room ? (size_t)a->read_left : room;
	size_t got = 0;
	if (want > 0) {
		if (fseeko(a->f, (off_t)a->read_pos, SEEK_SET) == 0) {
			got = fread(a->window + keep, 1, want, a->f) & ~(size_t)3;
		}
		if (got < want) {
			a->read_left = 0; // the file ends early
		} else {
			a->read_left -= (int64_t)got;
		}
		a->read_pos += (int64_t)got;
		ape_core_swap_words(a->window + keep, (uint32_t)got);
	}

	size_t len = keep + got;
	bool more = got > 0;
	if (a->read_left == 0 && a->pad_pending) {
		a->window[len++] = 0;
		a->window[len++] = 0;
		a->pad_pending = false;
		more = true;
	}
	ape_core_set_window(core, a->window, a->window + len);
	return more;
}

static void mark_broken(apedec_t *a, const char *what) {
	if (!a->broken) {
		fprintf(stderr, "ape: %s: frame %u %s, playing it as silence\n", a->name, (unsigned)a->frame, what);
	}
	a->broken = true;
}

// Sets up frame a->frame and leaves a->frame_left at its block count.
static void begin_frame(apedec_t *a) {
	const ape_frame_t *fr = &a->info.frames[a->frame];
	a->frame_left = frame_blocks(a, a->frame);
	a->broken = false;

	if (fr->size <= 0) {
		mark_broken(a, "has no data");
		return;
	}

	a->read_pos = fr->pos;
	a->read_left = fr->size;
	a->pad_pending = a->info.fileversion < 3950;

	if (!a->whole) {
		ape_core_set_window(a->core, a->window, a->window);
		if (ape_core_start_frame(a->core, a->window, a->window, fr->skip, a->frame_left, refill, a) < 0) {
			mark_broken(a, "has a bad header");
		}
		return;
	}

	if (fr->size > WHOLE_FRAME_MAX_BYTES) {
		mark_broken(a, "is too large");
		return;
	}
	size_t need = (size_t)fr->size + 8;
	if (need > a->window_cap) {
		uint8_t *grown = realloc(a->window, need);
		if (!grown) {
			mark_broken(a, "does not fit in memory");
			return;
		}
		a->window = grown;
		a->window_cap = need;
	}
	size_t got = 0;
	if (fseeko(a->f, (off_t)fr->pos, SEEK_SET) == 0) {
		got = fread(a->window, 1, (size_t)fr->size, a->f) & ~(size_t)3;
	}
	ape_core_swap_words(a->window, (uint32_t)got);
	size_t len = got;
	if (a->pad_pending) {
		a->window[len++] = 0;
		a->window[len++] = 0;
		a->pad_pending = false;
	}
	if (ape_core_start_frame(a->core, a->window, a->window + len, fr->skip, a->frame_left, NULL, NULL) < 0) {
		mark_broken(a, "has a bad header");
	}
}

// Makes the next chunk of samples available in a->left/right. False at the end.
static bool next_chunk(apedec_t *a) {
	if (a->frame_left == 0) {
		if (a->frame >= a->info.totalframes) {
			return false;
		}
		begin_frame(a);
	}

	uint32_t n = a->frame_left;
	if (!a->whole || a->broken) {
		n = n < APE_CORE_CHUNK ? n : APE_CORE_CHUNK;
	}

	if (!a->broken) {
		const int32_t *l, *r;
		if (ape_core_decode(a->core, (int)n, &l, &r) == 0) {
			a->left = l;
			a->right = r;
		} else {
			mark_broken(a, "does not decode");
			n = n < APE_CORE_CHUNK ? n : APE_CORE_CHUNK;
		}
	}
	if (a->broken) {
		a->left = a->silence;
		a->right = a->silence;
	}

	a->avail = n;
	a->used = 0;
	a->frame_left -= n;
	if (a->frame_left == 0) {
		a->frame++;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------

apedec_t *apedec_open(const char *filepath) {
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		fprintf(stderr, "ape: %s does not open\n", filepath);
		return NULL;
	}

	apedec_t *a = calloc(1, sizeof(*a));
	if (!a) {
		fclose(f);
		return NULL;
	}
	a->f = f;
	const char *slash = strrchr(filepath, '/');
	snprintf(a->name, sizeof(a->name), "%s", slash ? slash + 1 : filepath);

	if (!parse_header(f, a->name, &a->info, true)) {
		apedec_close(a);
		return NULL;
	}

	a->core = ape_core_new(a->info.fileversion, a->info.compression, a->info.flags, a->info.bps, a->info.channels);
	if (!a->core) {
		fprintf(stderr, "ape: %s: version %d, level %d, %d bit, %d ch is not supported\n", a->name,
				a->info.fileversion, a->info.compression, a->info.bps, a->info.channels);
		apedec_close(a);
		return NULL;
	}
	a->whole = ape_core_whole_frames(a->core);
	a->window_cap = WINDOW_BYTES;
	a->window = malloc(a->window_cap);
	a->silence = calloc(APE_CORE_CHUNK, sizeof(*a->silence));
	if (!a->window || !a->silence) {
		apedec_close(a);
		return NULL;
	}

	printf("ape: %s -- v%d.%02d, level %d, %d Hz, %d ch, %d bit, %llu frames\n", a->name,
		   a->info.fileversion / 1000, (a->info.fileversion % 1000) / 10, a->info.compression, a->info.samplerate,
		   a->info.channels, a->info.bps, (unsigned long long)a->info.totalsamples);
	return a;
}

void apedec_close(apedec_t *a) {
	if (!a) {
		return;
	}
	ape_core_free(a->core);
	free(a->info.frames);
	free(a->window);
	free(a->silence);
	if (a->f) {
		fclose(a->f);
	}
	free(a);
}

int apedec_channels(const apedec_t *a) { return a ? a->info.channels : 0; }
int apedec_sample_rate(const apedec_t *a) { return a ? a->info.samplerate : 0; }
int apedec_bits(const apedec_t *a) { return a ? a->info.bps : 16; }
uint64_t apedec_total_frames(const apedec_t *a) { return a ? a->info.totalsamples : 0; }

bool apedec_probe(const char *filepath, int *rate, int *bits) {
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		return false;
	}
	ape_info_t info;
	bool ok = parse_header(f, filepath, &info, false);
	fclose(f);
	if (ok) {
		*rate = info.samplerate;
		*bits = info.bps;
	}
	return ok;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

// A sample at the stream's depth, kept inside it: a broken stream can decode to
// anything, and a wrapped value is a full-scale click.
static inline int32_t clamp_depth(int32_t v, int bps) {
	int32_t hi = (1 << (bps - 1)) - 1;
	return v > hi ? hi : (v < -hi - 1 ? -hi - 1 : v);
}

uint64_t apedec_read_s32(apedec_t *a, uint64_t frames, int32_t *out) {
	if (!a || !out) {
		return 0;
	}
	int shift = 32 - a->info.bps;
	int ch = a->info.channels;
	uint64_t done = 0;
	while (done < frames) {
		if (a->used >= a->avail && !next_chunk(a)) {
			break;
		}
		while (a->used < a->avail && done < frames) {
			int32_t l = clamp_depth(a->left[a->used], a->info.bps);
			*out++ = (int32_t)((uint32_t)l << shift);
			if (ch == 2) {
				int32_t r = clamp_depth(a->right[a->used], a->info.bps);
				*out++ = (int32_t)((uint32_t)r << shift);
			}
			a->used++;
			done++;
		}
	}
	a->position += done;
	return done;
}

uint64_t apedec_read_s16(apedec_t *a, uint64_t frames, short *out) {
	if (!a || !out) {
		return 0;
	}
	int bps = a->info.bps;
	int ch = a->info.channels;
	uint64_t done = 0;
	while (done < frames) {
		if (a->used >= a->avail && !next_chunk(a)) {
			break;
		}
		while (a->used < a->avail && done < frames) {
			for (int c = 0; c < ch; c++) {
				int32_t v = clamp_depth(c ? a->right[a->used] : a->left[a->used], bps);
				*out++ = (short)(bps >= 16 ? v >> (bps - 16) : v * (1 << (16 - bps)));
			}
			a->used++;
			done++;
		}
	}
	a->position += done;
	return done;
}

bool apedec_seek(apedec_t *a, uint64_t frame) {
	if (!a) {
		return false;
	}
	a->avail = a->used = 0;
	a->frame_left = 0;
	if (frame >= a->info.totalsamples) {
		a->frame = a->info.totalframes;
		a->position = a->info.totalsamples;
		return true;
	}

	a->frame = (uint32_t)(frame / a->info.blocksperframe);
	uint64_t drop = frame - (uint64_t)a->frame * a->info.blocksperframe;
	while (drop > 0) {
		if (a->used >= a->avail && !next_chunk(a)) {
			return false;
		}
		uint32_t here = a->avail - a->used;
		uint32_t take = drop < here ? (uint32_t)drop : here;
		a->used += take;
		drop -= take;
	}
	a->position = frame;
	return true;
}

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

// Where the items of an APE tag are: after the audio, before an ID3v1 tag if
// there is one.
typedef struct {
	int64_t items_at;
	uint32_t items_bytes;
	uint32_t count;
	bool v1; // APEv1: every item is text
} ape_tag_t;

static bool find_tag(FILE *f, ape_tag_t *tag) {
	int64_t size = file_size_of(f);
	if (size < 32) {
		return false;
	}
	int64_t footer_at = size - 32;

	unsigned char id3[3];
	if (size >= 128 + 32 && fseeko(f, (off_t)(size - 128), SEEK_SET) == 0 && fread(id3, 1, 3, f) == 3 &&
		memcmp(id3, "TAG", 3) == 0) {
		footer_at = size - 128 - 32;
	}

	unsigned char foot[32];
	if (fseeko(f, (off_t)footer_at, SEEK_SET) != 0 || fread(foot, 1, 32, f) != 32 ||
		memcmp(foot, "APETAGEX", 8) != 0) {
		return false;
	}
	uint32_t version = le32(foot + 8);
	uint32_t tag_size = le32(foot + 12); // items and footer, not the header
	uint32_t count = le32(foot + 16);
	if (tag_size < 32 || (int64_t)tag_size > footer_at + 32 || count > 65536) {
		return false;
	}
	tag->items_at = footer_at + 32 - tag_size;
	tag->items_bytes = tag_size - 32;
	tag->count = count;
	tag->v1 = version < 2000;
	return true;
}

// Walks the items, handing each one's name, flags, value size and value offset
// to `fn` until it returns false.
static void walk_items(FILE *f, const ape_tag_t *tag,
					   bool (*fn)(void *user, const char *key, uint32_t flags, uint32_t size, int64_t at),
					   void *user) {
	int64_t at = tag->items_at;
	int64_t end = tag->items_at + tag->items_bytes;
	for (uint32_t i = 0; i < tag->count && at + 9 <= end; i++) {
		unsigned char h[8];
		if (fseeko(f, (off_t)at, SEEK_SET) != 0 || fread(h, 1, 8, f) != 8) {
			return;
		}
		uint32_t size = le32(h);
		uint32_t flags = tag->v1 ? 0 : le32(h + 4);

		char key[256];
		size_t k = 0;
		int c;
		while (k + 1 < sizeof(key) && (c = fgetc(f)) != EOF && c != 0) {
			key[k++] = (char)c;
		}
		key[k] = '\0';
		if (c != 0) {
			return; // an unterminated or overlong key: the tag is damaged
		}

		int64_t value_at = at + 8 + (int64_t)k + 1;
		if (value_at + size > end) {
			return;
		}
		if (!fn(user, key, flags, size, value_at)) {
			return;
		}
		at = value_at + size;
	}
}

// The item names of interest, compared case-insensitively. APEv2 has no naming
// standard: the album artist appears as "Album Artist", "AlbumArtist" and
// "album_artist" (ffmpeg), the year as "Year" and "date"; whichever is there
// answers.
static const char *const TAG_ITEMS[] = {
	"Title",		 "Artist", "Album", "Album Artist",	 "AlbumArtist",			  "album_artist",
	"Genre",		 "Track",  "Year",	"date",			 "replaygain_track_gain", "replaygain_track_peak",
	"replaygain_album_gain", "replaygain_album_peak", "Compilation",
};

typedef struct {
	FILE *f;
	void (*fn)(void *user, const char *key, const char *value);
	void *user;
} text_walk_t;

static bool text_item(void *user, const char *key, uint32_t flags, uint32_t size, int64_t at) {
	text_walk_t *w = user;
	if ((flags & 6) != 0) {
		return true; // binary or an external locator
	}
	for (size_t i = 0; i < sizeof(TAG_ITEMS) / sizeof(TAG_ITEMS[0]); i++) {
		if (strcasecmp(key, TAG_ITEMS[i]) != 0) {
			continue;
		}
		char value[256];
		size_t n = size < sizeof(value) - 1 ? size : sizeof(value) - 1;
		if (fseeko(w->f, (off_t)at, SEEK_SET) != 0 || fread(value, 1, n, w->f) != n) {
			return false;
		}
		value[n] = '\0'; // a list of values is NUL-separated: the first one stays
		if (value[0]) {
			w->fn(w->user, TAG_ITEMS[i], value);
		}
		break;
	}
	return true;
}

void apedec_tags(const char *filepath, void (*fn)(void *user, const char *key, const char *value), void *user) {
	if (!filepath || !fn) {
		return;
	}
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		return;
	}
	ape_tag_t tag;
	if (find_tag(f, &tag)) {
		text_walk_t w = {f, fn, user};
		walk_items(f, &tag, text_item, &w);
	}
	fclose(f);
}

typedef struct {
	int64_t at[2];
	uint32_t size[2];
} cover_walk_t;

static bool cover_item(void *user, const char *key, uint32_t flags, uint32_t size, int64_t at) {
	cover_walk_t *w = user;
	if ((flags & 6) != 2) {
		return true;
	}
	int slot = strcasecmp(key, "Cover Art (Front)") == 0 ? 0 : (strcasecmp(key, "Cover Art (Back)") == 0 ? 1 : -1);
	if (slot >= 0 && !w->size[slot]) {
		w->at[slot] = at;
		w->size[slot] = size;
	}
	return true;
}

bool apedec_cover_at(const char *filepath, int64_t *offset, uint32_t *size) {
	if (!filepath || !offset || !size) {
		return false;
	}
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		return false;
	}

	ape_tag_t tag;
	cover_walk_t w = {{0, 0}, {0, 0}};
	if (find_tag(f, &tag)) {
		walk_items(f, &tag, cover_item, &w);
	}

	bool found = false;
	for (int slot = 0; slot < 2 && !found; slot++) {
		// The value is a file name, a NUL, then the image.
		if (w.size[slot] == 0 || fseeko(f, (off_t)w.at[slot], SEEK_SET) != 0) {
			continue;
		}
		uint32_t skip = 0;
		int c = EOF;
		while (skip < w.size[slot] && (c = fgetc(f)) != EOF) {
			skip++;
			if (c == 0) {
				break;
			}
		}
		if (skip > 0 && skip < w.size[slot] && c == 0) {
			*offset = w.at[slot] + skip;
			*size = w.size[slot] - skip;
			found = true;
		}
	}

	fclose(f);
	return found;
}
