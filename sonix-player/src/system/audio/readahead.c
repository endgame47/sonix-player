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
// cover embedded at full size can make that several megabytes, and the tag
// reader and the cover loader both read it; past this it is not worth the
// card's time.
#define META_MAX_BYTES (16L * 1024L * 1024L)

// Read in pieces this size, so a newer request is noticed between them.
#define CHUNK_BYTES (64 * 1024)

// The most MP4 atoms and FLAC metadata blocks walked to find the audio. Real
// files have a handful; a damaged one must not keep the walk going.
#define MAX_BOXES 64
#define MAX_FLAC_BLOCKS 128

typedef struct {
	off_t start, end;
} range_t;

#define MAX_RANGES 3

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool started;
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
// included. `at` is where "fLaC" is.
static off_t flac_audio_start(int fd, off_t at, off_t size) {
	off_t pos = at + 4;
	for (int i = 0; i < MAX_FLAC_BLOCKS && pos + 4 <= size; i++) {
		unsigned char h[4];
		if (pread(fd, h, sizeof(h), pos) != (ssize_t)sizeof(h)) {
			break;
		}
		off_t length = ((off_t)h[1] << 16) | ((off_t)h[2] << 8) | h[3];
		pos += 4 + length;
		if (h[0] & 0x80) {
			break; // the last block
		}
	}
	return pos < size ? pos : size;
}

// The parts of the file opening it will read, in `out`; returns how many.
static int plan(int fd, off_t size, range_t *out) {
	int n = 0;
	unsigned char h[16];
	memset(h, 0, sizeof(h));
	ssize_t got = pread(fd, h, sizeof(h), 0);
	off_t audio = 0;

	if (got >= 10 && memcmp(h, "ID3", 3) == 0) {
		// An ID3v2 tag: its size is four 7-bit bytes, plus a footer when the
		// flag says so. A FLAC stream can follow one.
		off_t tag = ((off_t)(h[6] & 0x7F) << 21) | ((off_t)(h[7] & 0x7F) << 14) | ((off_t)(h[8] & 0x7F) << 7) |
					(h[9] & 0x7F);
		audio = 10 + tag + ((h[5] & 0x10) ? 10 : 0);
		unsigned char m[4];
		if (audio + 4 <= size && pread(fd, m, sizeof(m), audio) == (ssize_t)sizeof(m) && memcmp(m, "fLaC", 4) == 0) {
			audio = flac_audio_start(fd, audio, size);
		}
	} else if (got >= 4 && memcmp(h, "fLaC", 4) == 0) {
		audio = flac_audio_start(fd, 0, size);
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
			if (memcmp(b + 4, "moov", 4) == 0 && n < MAX_RANGES - 2) {
				off_t end = pos + (box < META_MAX_BYTES ? box : META_MAX_BYTES);
				out[n++] = (range_t){pos, end < size ? end : size};
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
// before it was done.
static bool read_ahead(const char *path, long *kb_out) {
	*kb_out = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return true; // gone or unreadable: nothing to do, and nothing to retry
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
		close(fd);
		return true;
	}

	range_t ranges[MAX_RANGES];
	int n = plan(fd, st.st_size, ranges);

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
		bool finished = read_ahead(path, &kb);

		pthread_mutex_lock(&lock);
		if (finished) {
			snprintf(done, sizeof(done), "%s", path);
		}
		pthread_mutex_unlock(&lock);

		if (finished) {
			fprintf(stderr, "readahead: '%s', %ld KB in %ld ms\n", path, kb, now_ms() - began);
		}
	}
	return NULL;
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
