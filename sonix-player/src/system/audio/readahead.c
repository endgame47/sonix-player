#include "readahead.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/utils.h"

// How much of the audio itself is read past where it begins, which covers the
// decoder's first reads and the frames that fill the queue; and how much of
// the end of the file, where ID3v1 and APEv2 tags, an Ogg stream's last page
// (its length) and the trailing chunks of a WAV live.
#define HEAD_BYTES (1024L * 1024L)
#define TAIL_BYTES (512L * 1024L)

// How far into a file the tags and headers before the audio are followed. A
// cover embedded at full size can make that several megabytes; past this it is
// not worth the card's time.
#define META_MAX_BYTES (16L * 1024L * 1024L)

// A cover embedded in the file is read by one thing only: the player's cover,
// to tell whether the next track carries the same picture as the one on screen
// (the tag reader and the decoders step over it). Once a track has been seen
// that answer is kept (picture_known), and its picture is left on the card --
// a record's tracks all carry the same one, read for nothing at every track.
// Pictures smaller than this are read all the same: splitting the read around
// them saves less than it costs.
#define PICTURE_SKIP_MIN (32L * 1024L)

// The head of a FLAC picture block that is read in any case: its type, MIME
// type, description and dimensions, which the tag reader does read.
#define FLAC_PICTURE_HEAD 4096L

// The most pictures stepped over in one file.
#define MAX_HOLES 8

// Read in pieces this size, so a newer request is noticed between them.
#define CHUNK_BYTES (64 * 1024)

// The most MP4 atoms and FLAC metadata blocks walked to find the audio. Real
// files have a handful; a damaged one must not keep the walk going.
#define MAX_BOXES 64
#define MAX_FLAC_BLOCKS 128
#define ID3_MAX_FRAMES 256

typedef struct {
	off_t start, end;
} range_t;

// Before the holes are cut out of them: the moov atom, the head and the tail.
#define MAX_PLANNED 3
// After: each hole can split one range in two.
#define MAX_RANGES (MAX_PLANNED + MAX_HOLES)

// The parts of the file the pictures take, gathered while the file is walked.
// `want` is false when the pictures are to be read like everything else.
typedef struct {
	bool want;
	int count;
	range_t at[MAX_HOLES];
} holes_t;

static void hole_add(holes_t *holes, off_t start, off_t end) {
	if (!holes->want || holes->count >= MAX_HOLES || end - start < PICTURE_SKIP_MIN) {
		return;
	}
	holes->at[holes->count++] = (range_t){start, end};
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool started;
static bool (*picture_known)(const char *path);
static char wanted[1024]; // the file asked for
static char done[1024];	  // the file last finished, whatever the outcome

static long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool still_wanted(const char *path) {
	pthread_mutex_lock(&lock);
	bool same = strcmp(wanted, path) == 0;
	pthread_mutex_unlock(&lock);
	return same;
}

static uint32_t be32(const unsigned char *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Where a FLAC stream's frames begin: past every metadata block, the picture
// included. `at` is where "fLaC" is. Picture blocks are noted in `holes`, all
// but their head.
static off_t flac_audio_start(int fd, off_t at, off_t size, holes_t *holes) {
	off_t pos = at + 4;
	for (int i = 0; i < MAX_FLAC_BLOCKS && pos + 4 <= size; i++) {
		unsigned char h[4];
		if (pread(fd, h, sizeof(h), pos) != (ssize_t)sizeof(h)) {
			break;
		}
		off_t length = ((off_t)h[1] << 16) | ((off_t)h[2] << 8) | h[3];
		if ((h[0] & 0x7F) == 6) {
			hole_add(holes, pos + 4 + FLAC_PICTURE_HEAD, pos + 4 + length);
		}
		pos += 4 + length;
		if (h[0] & 0x80) {
			break; // the last block
		}
	}
	return pos < size ? pos : size;
}

static off_t syncsafe(const unsigned char *p) {
	return ((off_t)(p[0] & 0x7F) << 21) | ((off_t)(p[1] & 0x7F) << 14) | ((off_t)(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

// The pictures of an ID3v2 tag whose ten-byte header is `h`, ending at `end`:
// APIC frames, PIC in version 2.2. A tag unsynchronised as a whole is left
// alone, since its frame sizes are not where the bytes are. Anything that does
// not look like a frame ends the walk; at worst a picture is read after all.
static void id3_pictures(int fd, const unsigned char *h, off_t end, holes_t *holes) {
	int version = h[3];
	if (!holes->want || version < 2 || version > 4 || ((h[5] & 0x80) && version < 4)) {
		return;
	}
	off_t pos = 10;
	if ((h[5] & 0x40) && version >= 3) {
		unsigned char x[4];
		if (pread(fd, x, sizeof(x), pos) != (ssize_t)sizeof(x)) {
			return;
		}
		pos += version == 4 ? syncsafe(x) : 4 + (off_t)be32(x);
	}
	int header = version == 2 ? 6 : 10;
	for (int i = 0; i < ID3_MAX_FRAMES && pos + header <= end; i++) {
		unsigned char f[10];
		if (pread(fd, f, (size_t)header, pos) != (ssize_t)header) {
			return;
		}
		int id_len = version == 2 ? 3 : 4;
		for (int k = 0; k < id_len; k++) {
			if (!((f[k] >= 'A' && f[k] <= 'Z') || (f[k] >= '0' && f[k] <= '9'))) {
				return; // padding, or not a frame
			}
		}
		off_t length;
		if (version == 2) {
			length = ((off_t)f[3] << 16) | ((off_t)f[4] << 8) | f[5];
		} else if (version == 3) {
			length = (off_t)be32(f + 4);
		} else {
			length = syncsafe(f + 4);
		}
		off_t body = pos + header;
		if (length <= 0 || body + length > end) {
			return;
		}
		if ((version == 2 && memcmp(f, "PIC", 3) == 0) || (version > 2 && memcmp(f, "APIC", 4) == 0)) {
			hole_add(holes, body, body + length);
		}
		pos = body + length;
	}
}

// One MP4 box header at `pos`: its type in `type`, its length and the length
// of the header itself. False past `end` or on a length that cannot be.
static bool mp4_box(int fd, off_t pos, off_t end, char type[4], off_t *box, off_t *header) {
	unsigned char b[16];
	if (pos + 8 > end || pread(fd, b, sizeof(b), pos) < 8) {
		return false;
	}
	*box = be32(b);
	*header = 8;
	if (*box == 1) {
		if (pos + 16 > end) {
			return false;
		}
		*box = ((off_t)be32(b + 8) << 32) | be32(b + 12);
		*header = 16;
	} else if (*box == 0) {
		*box = end - pos;
	}
	memcpy(type, b + 4, 4);
	return *box >= *header && pos + *box <= end;
}

// The first child of type `want` among the boxes from `pos` to `end`; its
// contents in `start` and `stop`.
static bool mp4_child(int fd, off_t pos, off_t end, const char *want, off_t *start, off_t *stop) {
	for (int i = 0; i < MAX_BOXES; i++) {
		char type[4];
		off_t box, header;
		if (!mp4_box(fd, pos, end, type, &box, &header)) {
			return false;
		}
		if (memcmp(type, want, 4) == 0) {
			*start = pos + header;
			*stop = pos + box;
			return true;
		}
		pos += box;
	}
	return false;
}

// The cover of an MP4, which lives in moov/udta/meta/ilst/covr.
static void mp4_pictures(int fd, off_t moov_start, off_t moov_end, holes_t *holes) {
	off_t a, b, c, d, e, f, g, k;
	if (!holes->want || !mp4_child(fd, moov_start, moov_end, "udta", &a, &b) ||
		!mp4_child(fd, a, b, "meta", &c, &d)) {
		return;
	}
	// meta is a full box: a version and flags come before its children.
	if (!mp4_child(fd, c + 4, d, "ilst", &e, &f) || !mp4_child(fd, e, f, "covr", &g, &k)) {
		return;
	}
	hole_add(holes, g, k);
}

// `in` with the holes taken out, into `out`; returns how many ranges that
// makes.
static int cut_holes(const range_t *in, int n, const holes_t *holes, range_t *out, int max) {
	int count = 0;
	for (int i = 0; i < n; i++) {
		off_t at = in[i].start;
		while (at < in[i].end) {
			// The nearest hole that starts at or after `at` inside this range,
			// or one `at` is already in.
			off_t until = in[i].end;
			off_t resume = in[i].end;
			for (int h = 0; h < holes->count; h++) {
				const range_t *hole = &holes->at[h];
				if (hole->end <= at || hole->start >= in[i].end) {
					continue;
				}
				off_t from = hole->start > at ? hole->start : at;
				if (from < until) {
					until = from;
					resume = hole->end;
				}
			}
			if (until > at && count < max) {
				out[count++] = (range_t){at, until};
			}
			at = resume > until ? resume : until;
		}
	}
	return count;
}

// The parts of the file opening it will read, in `out`; returns how many.
// With `holes->want`, the pictures are left out of them.
static int plan(int fd, off_t size, range_t *out, holes_t *holes) {
	int n = 0;
	unsigned char h[16];
	memset(h, 0, sizeof(h));
	ssize_t got = pread(fd, h, sizeof(h), 0);
	off_t audio = 0;

	if (got >= 10 && memcmp(h, "ID3", 3) == 0) {
		// An ID3v2 tag: its size is four 7-bit bytes, plus a footer when the
		// flag says so. A FLAC stream can follow one.
		off_t tag = syncsafe(h + 6);
		audio = 10 + tag + ((h[5] & 0x10) ? 10 : 0);
		id3_pictures(fd, h, 10 + tag < size ? 10 + tag : size, holes);
		unsigned char m[4];
		if (audio + 4 <= size && pread(fd, m, sizeof(m), audio) == (ssize_t)sizeof(m) && memcmp(m, "fLaC", 4) == 0) {
			audio = flac_audio_start(fd, audio, size, holes);
		}
	} else if (got >= 4 && memcmp(h, "fLaC", 4) == 0) {
		audio = flac_audio_start(fd, 0, size, holes);
	} else if (got >= 8 && memcmp(h + 4, "ftyp", 4) == 0) {
		// MP4: the top-level boxes. moov, which says where every sample is,
		// is read whole wherever it sits -- often at the end -- and the audio
		// starts inside mdat.
		off_t pos = 0;
		for (int i = 0; i < MAX_BOXES && pos + 8 <= size; i++) {
			unsigned char b[16];
			if (pread(fd, b, sizeof(b), pos) < 8) {
				break;
			}
			off_t box = be32(b);
			off_t header = 8;
			if (box == 1) {
				box = ((off_t)be32(b + 8) << 32) | be32(b + 12);
				header = 16;
			} else if (box == 0) {
				box = size - pos;
			}
			if (box < header) {
				break;
			}
			if (memcmp(b + 4, "moov", 4) == 0 && n < MAX_PLANNED - 2) {
				off_t end = pos + (box < META_MAX_BYTES ? box : META_MAX_BYTES);
				out[n++] = (range_t){pos, end < size ? end : size};
				mp4_pictures(fd, pos + header, pos + box < size ? pos + box : size, holes);
			} else if (memcmp(b + 4, "mdat", 4) == 0 && audio == 0) {
				audio = pos + header;
			}
			pos += box;
		}
	}

	if (audio > META_MAX_BYTES) {
		audio = META_MAX_BYTES;
	}
	off_t head_end = audio + HEAD_BYTES;
	if (head_end > size) {
		head_end = size;
	}
	out[n++] = (range_t){0, head_end};

	off_t tail_start = size - TAIL_BYTES;
	if (tail_start < head_end) {
		tail_start = head_end;
	}
	if (tail_start < size) {
		out[n++] = (range_t){tail_start, size};
	}
	return n;
}

// Reads `path` into the page cache. False when a newer file was asked for
// before it was done. `skipped_kb` is how much of it the pictures left out.
static bool read_ahead(const char *path, long *kb_out, long *skipped_kb) {
	*kb_out = 0;
	*skipped_kb = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return true; // gone or unreadable: nothing to do, and nothing to retry
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
		close(fd);
		return true;
	}

	pthread_mutex_lock(&lock);
	bool (*known)(const char *) = picture_known;
	pthread_mutex_unlock(&lock);
	holes_t holes = {.want = known && known(path)};

	range_t planned[MAX_PLANNED];
	int planned_n = plan(fd, st.st_size, planned, &holes);
	range_t ranges[MAX_RANGES];
	int n = cut_holes(planned, planned_n, &holes, ranges, MAX_RANGES);
	for (int i = 0; i < holes.count; i++) {
		*skipped_kb += (long)((holes.at[i].end - holes.at[i].start) / 1024);
	}

	char *buffer = malloc(CHUNK_BYTES);
	if (!buffer) {
		close(fd);
		return true;
	}

	bool finished = true;
	long bytes = 0;
	for (int i = 0; i < n && finished; i++) {
		for (off_t at = ranges[i].start; at < ranges[i].end; at += CHUNK_BYTES) {
			if (!still_wanted(path)) {
				finished = false;
				break;
			}
			size_t want = ranges[i].end - at < CHUNK_BYTES ? (size_t)(ranges[i].end - at) : CHUNK_BYTES;
			ssize_t got = pread(fd, buffer, want, at);
			if (got <= 0) {
				break;
			}
			bytes += got;
		}
	}

	free(buffer);
	close(fd);
	*kb_out = bytes / 1024;
	return finished;
}

static void *worker_main(void *arg) {
	(void)arg;
	// Below the interface but not starved by it: the read has to be done
	// before the track it is for comes up, a few seconds from now.
	thread_be_low_priority("readahead");

	for (;;) {
		char path[sizeof(wanted)];
		pthread_mutex_lock(&lock);
		while (!wanted[0] || strcmp(wanted, done) == 0) {
			pthread_cond_wait(&wake, &lock);
		}
		snprintf(path, sizeof(path), "%s", wanted);
		pthread_mutex_unlock(&lock);

		long began = now_ms();
		long kb = 0;
		long skipped_kb = 0;
		bool finished = read_ahead(path, &kb, &skipped_kb);

		pthread_mutex_lock(&lock);
		if (finished) {
			snprintf(done, sizeof(done), "%s", path);
		}
		pthread_mutex_unlock(&lock);

		if (finished && skipped_kb > 0) {
			fprintf(stderr, "readahead: '%s', %ld KB in %ld ms, cover already known (%ld KB left on the card)\n",
					path, kb, now_ms() - began, skipped_kb);
		} else if (finished) {
			fprintf(stderr, "readahead: '%s', %ld KB in %ld ms\n", path, kb, now_ms() - began);
		}
	}
	return NULL;
}

void readahead_set_picture_known(bool (*known)(const char *path)) {
	pthread_mutex_lock(&lock);
	picture_known = known;
	pthread_mutex_unlock(&lock);
}

void readahead_file(const char *path) {
	if (!path || !path[0] || strlen(path) >= sizeof(wanted)) {
		return;
	}
	// A track of a CUE sheet is not a file of its own, and neither is a stream:
	// only a plain file is worth reading ahead.
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		return;
	}

	pthread_mutex_lock(&lock);
	if (strcmp(wanted, path) == 0) {
		pthread_mutex_unlock(&lock);
		return; // already asked for: being read, or read
	}
	snprintf(wanted, sizeof(wanted), "%s", path);
	bool start = !started;
	started = true;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);

	if (start) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, worker_main, NULL) != 0) {
			pthread_mutex_lock(&lock);
			started = false;
			wanted[0] = '\0';
			pthread_mutex_unlock(&lock);
			fprintf(stderr, "readahead: no thread; the next track is opened from the card\n");
			return;
		}
		pthread_detach(thread);
	}
}
