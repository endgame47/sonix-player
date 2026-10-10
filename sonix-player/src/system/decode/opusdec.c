#include "opusdec.h"

// opusfile.h itself includes <opus_multistream.h> unprefixed, so the opus/
// directory must be on the -I path: on the host pkg-config handles it, on the
// target the Makefile points at $(OPUS_PREFIX)/include/opus.
#include <opusfile.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// A gap in the stream (OP_HOLE) is not an error: it is a missing or corrupt
// piece, and opusfile expects the read to be retried. But a broken file
// returning gaps forever would hang the player inside a while loop, so they are
// counted.
#define MAX_HOLES 64

struct opusdec {
	OggOpusFile *of;
	int channels;
	bool force_stereo; // more than two channels: take the stereo downmix
	uint64_t frames;
};

// opusfile has two reads: op_read() gives the channels the file really has, and
// op_read_stereo() always gives two, mixing as needed. Above two channels the
// latter is used: the rest of the player is built on mono and stereo, and the
// library's downmix beats a hand-rolled one.
static int channels_of(const OggOpusFile *of) {
	int ch = op_channel_count(of, -1);
	return (ch >= 1 && ch <= 8) ? ch : 0;
}

// ---------------------------------------------------------------------------
// Opening without the comments
//
// opusfile reads the whole comment header when it opens a file and keeps it
// for as long as the file is open. A cover in an Opus file is a base64
// comment, so a ten-megabyte picture costs the reassembled packet and the
// parsed copy at once -- about twice its encoded size, on a player with tens of
// megabytes in all -- and is read off the card before the first sample.
// Playback reads nothing from the comments: tags, covers and chapters come
// from oggtags.c, and the gain opusfile applies is the header's.
//
// So when the comment header is large, opusfile is handed the file with that
// header swapped for an empty one. The stream it sees is
//
//     [OpusHead page] [OpusTags page, no comments] [the file from the first
//                                                   page after the comments]
//
// The two pages at the front are built here; everything after them is the
// file's own bytes at a fixed offset. The OpusHead page is renumbered so that
// the empty comment page carries the sequence number of the last original
// comment page: the audio pages then follow without a gap, and libogg sees
// nothing to report. Only the plainest layout is handled -- one Opus stream
// starting the file, its comment pages next to it -- and anything else opens
// the usual way.
// ---------------------------------------------------------------------------

// Comment headers smaller than this open the usual way: what they cost is
// small, and the usual way is what every other file goes through.
#define SLIM_MIN_TAG_BYTES (256 * 1024)

// The longest Ogg page header: 27 bytes and up to 255 lacing values.
#define OGG_HEADER_MAX (27 + 255)

// The two pages built here: the OpusHead page, which holds one packet of at
// most 254 bytes, and the empty OpusTags page.
#define SLIM_HEAD_MAX (2 * OGG_HEADER_MAX + 255 + 16)

typedef struct {
	void *file; // the stream op_fopen() returned
	OpusFileCallbacks inner;
	unsigned char head[SLIM_HEAD_MAX];
	opus_int64 head_len;
	opus_int64 skip; // where the file continues after the comment pages
	long tag_bytes;	 // the comment header left on the card
	opus_int64 file_size;
	opus_int64 pos;		 // position in the stream opusfile sees
	opus_int64 file_pos; // where the file is, -1 when not known
} slim_src_t;

// The checksum of a page, its own checksum field taken as zero: Ogg's CRC-32,
// polynomial 0x04c11db7, no reflection, no final inversion. Bit by bit, as the
// pages it is ever run over are a few hundred bytes.
static uint32_t ogg_page_crc(const unsigned char *page, size_t len) {
	uint32_t crc = 0;
	for (size_t i = 0; i < len; i++) {
		uint32_t byte = (i >= 22 && i < 26) ? 0 : page[i];
		crc ^= byte << 24;
		for (int j = 0; j < 8; j++) {
			crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u : crc << 1;
		}
	}
	return crc;
}

static void ogg_page_seal(unsigned char *page, size_t len) {
	uint32_t crc = ogg_page_crc(page, len);
	page[22] = (unsigned char)crc;
	page[23] = (unsigned char)(crc >> 8);
	page[24] = (unsigned char)(crc >> 16);
	page[25] = (unsigned char)(crc >> 24);
}

static uint32_t le32(const unsigned char *p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put_le32(unsigned char *p, uint32_t v) {
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

// The page header at `offset`: 27 bytes and the lacing values, `hdr_len` long,
// with `body_len` bytes of packet data after it.
static bool page_header_at(FILE *f, long offset, unsigned char *hdr, int *hdr_len, long *body_len) {
	if (fseek(f, offset, SEEK_SET) != 0 || fread(hdr, 1, 27, f) != 27 || memcmp(hdr, "OggS", 4) != 0 ||
		hdr[4] != 0) {
		return false;
	}
	int segments = hdr[26];
	if (segments > 0 && fread(hdr + 27, 1, (size_t)segments, f) != (size_t)segments) {
		return false;
	}
	long body = 0;
	for (int i = 0; i < segments; i++) {
		body += hdr[27 + i];
	}
	*hdr_len = 27 + segments;
	*body_len = body;
	return true;
}

// Works out the stream described above for the file open on `f`. False -- and
// the file opens the usual way -- when it is laid out any other way, or its
// comment header is small.
static bool slim_plan(FILE *f, slim_src_t *src) {
	bool ok = false;
	unsigned char hdr[OGG_HEADER_MAX];
	int hdr_len = 0;
	long body = 0;

	// The OpusHead page: the first of its stream, one packet that ends on it.
	if (!page_header_at(f, 0, hdr, &hdr_len, &body) || !(hdr[5] & 0x02) || (hdr[5] & 0x01) || hdr[26] != 1 ||
		hdr[27] == 255 || body < 19 || body > 255) {
		goto out;
	}
	unsigned char head_page[OGG_HEADER_MAX + 255];
	memcpy(head_page, hdr, (size_t)hdr_len);
	if (fread(head_page + hdr_len, 1, (size_t)body, f) != (size_t)body ||
		memcmp(head_page + hdr_len, "OpusHead", 8) != 0) {
		goto out;
	}
	long head_page_len = hdr_len + body;
	// Checked, because it is about to be sealed again: a damaged OpusHead --
	// its channel count, its gain -- must not come out of here looking sound.
	if (ogg_page_crc(head_page, (size_t)head_page_len) != le32(head_page + 22)) {
		goto out;
	}
	uint32_t serial = le32(hdr + 14);
	uint32_t head_seq = le32(hdr + 18);

	// The comment pages: the same stream, the first one starting the packet,
	// the last one ending it with its final segment.
	long offset = head_page_len;
	long tag_bytes = 0;
	uint32_t last_seq = head_seq;
	bool first = true;
	for (;;) {
		if (!page_header_at(f, offset, hdr, &hdr_len, &body) || le32(hdr + 14) != serial || (hdr[5] & 0x06) ||
			le32(hdr + 18) != last_seq + 1) {
			goto out;
		}
		if (first) {
			unsigned char magic[8];
			if ((hdr[5] & 0x01) || hdr[26] == 0 || fread(magic, 1, 8, f) != 8 ||
				memcmp(magic, "OpusTags", 8) != 0) {
				goto out;
			}
			first = false;
		} else if (!(hdr[5] & 0x01)) {
			goto out; // a new packet before the comment header ended
		}
		last_seq = le32(hdr + 18);
		tag_bytes += body;
		offset += hdr_len + body;
		int segments = hdr[26];
		int ends = -1; // the segment the packet ends on
		for (int i = 0; i < segments; i++) {
			if (hdr[27 + i] < 255) {
				ends = i;
				break;
			}
		}
		if (ends >= 0) {
			if (ends != segments - 1) {
				goto out; // more on the page after the comments
			}
			break;
		}
	}
	if (tag_bytes < SLIM_MIN_TAG_BYTES || last_seq == 0 || fseek(f, 0, SEEK_END) != 0) {
		goto out;
	}
	long size = ftell(f);
	if (size < offset) {
		goto out;
	}

	// The OpusHead page as it was, numbered one before the empty comment page.
	memcpy(src->head, head_page, (size_t)head_page_len);
	put_le32(src->head + 18, last_seq - 1);
	ogg_page_seal(src->head, (size_t)head_page_len);

	// The comment page: a vendor string and a comment count, both empty.
	unsigned char *tags = src->head + head_page_len;
	memset(tags, 0, 27 + 1 + 16);
	memcpy(tags, "OggS", 4);
	put_le32(tags + 14, serial);
	put_le32(tags + 18, last_seq);
	tags[26] = 1;
	tags[27] = 16;
	memcpy(tags + 28, "OpusTags", 8);
	ogg_page_seal(tags, 27 + 1 + 16);

	src->head_len = head_page_len + 27 + 1 + 16;
	src->skip = offset;
	src->tag_bytes = tag_bytes;
	src->file_size = size;
	ok = true;
out:
	return ok;
}

static int slim_read(void *stream, unsigned char *ptr, int nbytes) {
	slim_src_t *src = stream;
	if (nbytes <= 0) {
		return 0;
	}
	if (src->pos < src->head_len) {
		opus_int64 n = src->head_len - src->pos;
		if (n > nbytes) {
			n = nbytes;
		}
		memcpy(ptr, src->head + src->pos, (size_t)n);
		src->pos += n;
		return (int)n;
	}
	opus_int64 at = src->pos - src->head_len + src->skip;
	if (src->file_pos != at) {
		if ((*src->inner.seek)(src->file, at, SEEK_SET) != 0) {
			src->file_pos = -1;
			return -1;
		}
		src->file_pos = at;
	}
	int got = (*src->inner.read)(src->file, ptr, nbytes);
	if (got > 0) {
		src->pos += got;
		src->file_pos += got;
	} else if (got < 0) {
		src->file_pos = -1;
	}
	return got;
}

static int slim_seek(void *stream, opus_int64 offset, int whence) {
	slim_src_t *src = stream;
	opus_int64 base;
	switch (whence) {
	case SEEK_SET:
		base = 0;
		break;
	case SEEK_CUR:
		base = src->pos;
		break;
	case SEEK_END:
		base = src->head_len + (src->file_size - src->skip);
		break;
	default:
		return -1;
	}
	if (offset < -base) {
		return -1;
	}
	src->pos = base + offset;
	return 0;
}

static opus_int64 slim_tell(void *stream) { return ((slim_src_t *)stream)->pos; }

static int slim_close(void *stream) {
	slim_src_t *src = stream;
	int ret = (*src->inner.close)(src->file);
	free(src);
	return ret;
}

static const OpusFileCallbacks SLIM_CALLBACKS = {slim_read, slim_seek, slim_tell, slim_close};

// opusfile on `filepath` without its comments, or NULL when the file is not
// one the stream above is built for.
static OggOpusFile *slim_open(const char *filepath, int *err) {
	slim_src_t *src = calloc(1, sizeof(*src));
	if (!src) {
		return NULL;
	}
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		free(src);
		return NULL;
	}
	// The plan and the stream read the same open file, not two opens of one
	// name that a copy to the card could tell apart.
	int fd = slim_plan(f, src) ? dup(fileno(f)) : -1;
	fclose(f);
	if (fd < 0) {
		free(src);
		return NULL;
	}
	src->file = op_fdopen(&src->inner, fd, "rb");
	if (!src->file) {
		close(fd);
		free(src);
		return NULL;
	}
	src->file_pos = -1;
	// On failure opusfile leaves the stream to the caller.
	OggOpusFile *of = op_open_callbacks(src, &SLIM_CALLBACKS, NULL, 0, err);
	if (!of) {
		slim_close(src);
		return NULL;
	}
	printf("opus: %s opened without its %ld bytes of comments\n", filepath, src->tag_bytes);
	return of;
}

opusdec_t *opusdec_open(const char *filepath) {
	int err = 0;
	OggOpusFile *of = slim_open(filepath, &err);
	if (!of) {
		of = op_open_file(filepath, &err);
	}
	if (!of) {
		fprintf(stderr, "opus: %s does not open (error %d)\n", filepath, err);
		return NULL;
	}

	int ch = channels_of(of);
	if (ch == 0) {
		fprintf(stderr, "opus: %s has a channel count we do not know how to handle\n", filepath);
		op_free(of);
		return NULL;
	}

	opusdec_t *o = calloc(1, sizeof(*o));
	if (!o) {
		op_free(of);
		return NULL;
	}
	o->of = of;
	o->force_stereo = (ch > 2);
	o->channels = o->force_stereo ? 2 : ch;

	// op_pcm_total() is -1 on a non-seekable stream (a pipe): there the duration
	// simply is not known, and zero is how the rest of the player says so.
	ogg_int64_t total = op_pcm_total(of, -1);
	o->frames = total > 0 ? (uint64_t)total : 0;

	printf("opus: %s -- 48000 Hz, %d ch%s, %llu frame\n", filepath, o->channels,
		   o->force_stereo ? " (downmixed from multichannel)" : "", (unsigned long long)o->frames);
	return o;
}

void opusdec_close(opusdec_t *o) {
	if (!o) {
		return;
	}
	if (o->of) {
		op_free(o->of);
	}
	free(o);
}

int opusdec_channels(const opusdec_t *o) { return o ? o->channels : 0; }

// Not read from a field: Opus decodes at 48 kHz and nothing else.
int opusdec_sample_rate(const opusdec_t *o) { return o ? 48000 : 0; }

uint64_t opusdec_total_frames(const opusdec_t *o) { return o ? o->frames : 0; }

// On the 16-bit path opusfile scales by 32753 instead of 32768 (`OP_GAIN` in
// opusfile.c) to keep headroom against clipping, since decoded Opus can exceed
// full scale between samples. That is -0.004 dB against a full-scale decode,
// which on a lossy codec costs less than the headroom is worth.
uint64_t opusdec_read_s16(opusdec_t *o, uint64_t frames, short *out) {
	if (!o || !out || frames == 0) {
		return 0;
	}

	uint64_t done = 0;
	int holes = 0;

	// op_read() returns how many frames per channel it produced, and produces as
	// many as suits it -- normally one packet at a time. It has to be called
	// again until the caller's buffer is full.
	while (done < frames) {
		uint64_t want_frames = frames - done;
		// The count opusfile wants is in total samples, channels included, and
		// has to fit in an int.
		uint64_t want_samples = want_frames * (uint64_t)o->channels;
		if (want_samples > (uint64_t)INT_MAX) {
			want_samples = (uint64_t)INT_MAX;
		}

		short *dst = out + done * (uint64_t)o->channels;
		int got = o->force_stereo ? op_read_stereo(o->of, dst, (int)want_samples)
								  : op_read(o->of, dst, (int)want_samples, NULL);

		if (got == OP_HOLE) {
			// Missing piece: retry, but not forever.
			if (++holes > MAX_HOLES) {
				fprintf(stderr, "opus: too many consecutive gaps; stopping here\n");
				break;
			}
			continue;
		}
		if (got <= 0) {
			break; // 0 = end of stream, negative = a real error
		}

		holes = 0;
		done += (uint64_t)got;
	}

	return done;
}

bool opusdec_seek(opusdec_t *o, uint64_t frame) {
	if (!o) {
		return false;
	}
	return op_pcm_seek(o->of, (ogg_int64_t)frame) == 0;
}
