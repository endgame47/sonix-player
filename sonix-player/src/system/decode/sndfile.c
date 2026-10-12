#include "sndfile.h"

#include "src/system/core/utils.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The slice of libsndfile's interface used here, restated so nothing has to be
// installed to build -- the same rules aacdec.c follows:
//
//  * only the handle crosses the boundary as an opaque value;
//  * SF_INFO is described in full because it has to be: libsndfile writes into
//    it. Its six fields have been these six, in this order, since 1.0.0, and
//    the sf_count_t at the front is a 64-bit signed count on every build that
//    has ever shipped -- the device carries 1.2.2;
//  * every symbol is looked up by name and a missing one turns the whole thing
//    off rather than crashing.

typedef int64_t sf_count_t;

typedef struct {
	sf_count_t frames;
	int samplerate;
	int channels;
	int format;
	int sections;
	int seekable;
} SF_INFO;

#define SFM_READ 0x10

#define SF_FORMAT_SUBMASK 0x0000FFFF
#define SF_FORMAT_PCM_S8 0x0001
#define SF_FORMAT_PCM_16 0x0002
#define SF_FORMAT_PCM_24 0x0003
#define SF_FORMAT_PCM_32 0x0004
#define SF_FORMAT_PCM_U8 0x0005
#define SF_FORMAT_FLOAT 0x0006
#define SF_FORMAT_DOUBLE 0x0007

#define SEEK_SET_SF 0

struct sf_api {
	void *(*open)(const char *path, int mode, SF_INFO *info);
	int (*close)(void *);
	sf_count_t (*readf_short)(void *, short *ptr, sf_count_t frames);
	sf_count_t (*readf_int)(void *, int *ptr, sf_count_t frames);
	sf_count_t (*seek)(void *, sf_count_t frames, int whence);
	const char *(*strerror)(void *);
};

static struct sf_api api;
static void *lib_handle;
static bool load_tried;
static bool load_ok;
static pthread_mutex_t load_lock = PTHREAD_MUTEX_INITIALIZER;
static char last_error[160];

struct sndfile {
	void *handle;
	int channels;
	int sample_rate;
	int bits;
	uint64_t frames;
};

// ---------------------------------------------------------------------------
// loading
// ---------------------------------------------------------------------------

static bool bind_symbol(void **slot, const char *name) {
	*slot = dlsym(lib_handle, name);
	if (!*slot) {
		snprintf(last_error, sizeof(last_error), "libsndfile: %s is missing", name);
		return false;
	}
	return true;
}

static void load_once(void) {
	if (load_tried) {
		return;
	}
	load_tried = true;

	// The soname, not the file name: whichever point release the device
	// carries is the one that answers.
	static const char *const NAMES[] = {"libsndfile.so.1", "libsndfile.so"};
	for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]) && !lib_handle; i++) {
		lib_handle = dlopen(NAMES[i], RTLD_NOW | RTLD_LOCAL);
	}
	if (!lib_handle) {
		snprintf(last_error, sizeof(last_error), "libsndfile not found: %s", dlerror());
		return;
	}

	bool ok = bind_symbol((void **)&api.open, "sf_open") && bind_symbol((void **)&api.close, "sf_close") &&
			  bind_symbol((void **)&api.readf_short, "sf_readf_short") &&
			  bind_symbol((void **)&api.readf_int, "sf_readf_int") && bind_symbol((void **)&api.seek, "sf_seek") &&
			  bind_symbol((void **)&api.strerror, "sf_strerror");
	if (!ok) {
		dlclose(lib_handle);
		lib_handle = NULL;
		return;
	}

	load_ok = true;
	printf("sndfile: libsndfile available (AIFF, CAF)\n");
}

bool sndfile_available(void) {
	pthread_mutex_lock(&load_lock);
	load_once();
	bool ok = load_ok;
	pthread_mutex_unlock(&load_lock);
	return ok;
}

const char *sndfile_last_error(void) { return last_error[0] ? last_error : "no error"; }

bool sndfile_handles(const char *filepath) {
	// AIFF and CAF only. libsndfile also reads Wave64, RF64 and Sun/NeXT, but
	// claiming them would add four extensions to scan in every folder for file
	// types nobody here has.
	static const char *const EXTS[] = {".aif", ".aiff", ".aifc", ".caf"};
	for (size_t i = 0; i < sizeof(EXTS) / sizeof(EXTS[0]); i++) {
		if (has_extension(filepath, EXTS[i])) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// reading
// ---------------------------------------------------------------------------

// What the file really holds, so a 24-bit AIFF is played as a hi-res track
// rather than squeezed into sixteen. A floating-point subformat is treated as
// 32: it is at least that wide.
static int bits_of(int format) {
	switch (format & SF_FORMAT_SUBMASK) {
	case SF_FORMAT_PCM_S8:
	case SF_FORMAT_PCM_U8:
	case SF_FORMAT_PCM_16:
		return 16;
	case SF_FORMAT_PCM_24:
		return 24;
	case SF_FORMAT_PCM_32:
	case SF_FORMAT_FLOAT:
	case SF_FORMAT_DOUBLE:
		return 32;
	default:
		// A compressed subformat -- ALAC in a CAF, IMA/MS ADPCM, GSM.
		// libsndfile decodes them all to 16-bit cleanly.
		return 16;
	}
}

sndfile_t *sndfile_open(const char *filepath) {
	if (!sndfile_available()) {
		return NULL;
	}

	SF_INFO info;
	memset(&info, 0, sizeof(info));

	void *handle = api.open(filepath, SFM_READ, &info);
	if (!handle) {
		fprintf(stderr, "sndfile: %s does not open: %s\n", filepath, api.strerror(NULL));
		return NULL;
	}
	if (info.channels < 1 || info.channels > 8 || info.samplerate <= 0) {
		api.close(handle);
		return NULL;
	}

	sndfile_t *s = calloc(1, sizeof(*s));
	if (!s) {
		api.close(handle);
		return NULL;
	}
	s->handle = handle;
	s->channels = info.channels;
	s->sample_rate = info.samplerate;
	s->bits = bits_of(info.format);
	s->frames = info.frames > 0 ? (uint64_t)info.frames : 0;

	printf("sndfile: %s -- %d Hz, %d ch, %d bit, %llu frame\n", filepath, s->sample_rate, s->channels, s->bits,
		   (unsigned long long)s->frames);
	return s;
}

void sndfile_close(sndfile_t *s) {
	if (!s) {
		return;
	}
	if (s->handle) {
		api.close(s->handle);
	}
	free(s);
}

int sndfile_channels(const sndfile_t *s) { return s ? s->channels : 0; }
int sndfile_sample_rate(const sndfile_t *s) { return s ? s->sample_rate : 0; }
int sndfile_bits(const sndfile_t *s) { return s ? s->bits : 16; }
uint64_t sndfile_total_frames(const sndfile_t *s) { return s ? s->frames : 0; }

uint64_t sndfile_read_s16(sndfile_t *s, uint64_t frames, short *out) {
	if (!s || !out || frames == 0) {
		return 0;
	}
	sf_count_t got = api.readf_short(s->handle, out, (sf_count_t)frames);
	return got > 0 ? (uint64_t)got : 0;
}

uint64_t sndfile_read_s32(sndfile_t *s, uint64_t frames, int32_t *out) {
	if (!s || !out || frames == 0) {
		return 0;
	}
	// sf_readf_int already left-justifies: libsndfile's contract is that the
	// sample fills the top of the int whatever the file's own depth is, which
	// is exactly what the rest of this player expects from a 32-bit read.
	sf_count_t got = api.readf_int(s->handle, (int *)out, (sf_count_t)frames);
	return got > 0 ? (uint64_t)got : 0;
}

bool sndfile_seek(sndfile_t *s, uint64_t frame) {
	if (!s) {
		return false;
	}
	return api.seek(s->handle, (sf_count_t)frame, SEEK_SET_SF) >= 0;
}
