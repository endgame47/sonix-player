#include "wavpackdec.h"

#include <wavpack/wavpack.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct wavpackdec {
	WavpackContext *wpc;
	int channels;
	int sample_rate;
	int bits;
	bool is_float;
	uint64_t frames;

	// Only the 16-bit read needs this: WavpackUnpackSamples writes int32 and
	// nothing else, so samples come in wide and are narrowed afterwards. The
	// 32-bit read decodes straight into the caller's buffer.
	int32_t *scratch;
	uint64_t scratch_frames;
};

// ---------------------------------------------------------------------------
// opening
// ---------------------------------------------------------------------------

// OPEN_WVC: opens a .wvc correction file sitting next to the .wv on its own, so
//           a hybrid file is lossless again without anyone asking.
// OPEN_DSD_AS_PCM: a .wv holding DSD is decimated to 24 bit instead of coming
//           out as a bitstream, which is the only form this path can play.
// OPEN_NORMALIZE: applies to floating-point files only, bringing them to
//           +/-1.0 so the integer conversion below is a multiply and nothing
//           more.
// No OPEN_TAGS: apedec.c reads the tags and the cover from the file, and with
// the flag the library holds the whole tag, cover included, while the file
// plays.
//
// OPEN_2CH_MAX is deliberately absent: with it WavpackUnpackSamples keeps
// reasoning in terms of the file's channel count while writing fewer, and the
// buffer size the caller has to pass stops being obvious. Taking the channels
// the file has is simpler.
#define OPEN_FLAGS (OPEN_WVC | OPEN_DSD_AS_PCM | OPEN_NORMALIZE)

wavpackdec_t *wavpackdec_open(const char *filepath) {
	char err[80];
	err[0] = '\0';

	WavpackContext *wpc = WavpackOpenFileInput(filepath, err, OPEN_FLAGS, 0);
	if (!wpc) {
		fprintf(stderr, "wavpack: %s does not open: %s\n", filepath, err[0] ? err : "no reason given");
		return NULL;
	}

	int channels = WavpackGetNumChannels(wpc);
	int rate = (int)WavpackGetSampleRate(wpc);
	int bits = WavpackGetBitsPerSample(wpc);
	if (channels < 1 || channels > 8 || rate <= 0 || bits < 1 || bits > 32) {
		fprintf(stderr, "wavpack: %s declares %d channels, %d Hz, %d bit -- no good\n", filepath, channels, rate,
				bits);
		WavpackCloseFile(wpc);
		return NULL;
	}

	wavpackdec_t *w = calloc(1, sizeof(*w));
	if (!w) {
		WavpackCloseFile(wpc);
		return NULL;
	}
	w->wpc = wpc;
	w->channels = channels;
	w->sample_rate = rate;
	w->is_float = (WavpackGetMode(wpc) & MODE_FLOAT) != 0;
	// A floating-point file has no integer depth; 24 bit is what it is
	// converted to and what gets reported.
	w->bits = w->is_float ? 24 : bits;

	int64_t total = WavpackGetNumSamples64(wpc);
	w->frames = total > 0 ? (uint64_t)total : 0;

	printf("wavpack: %s -- %d Hz, %d ch, %d bit%s, %llu frame\n", filepath, w->sample_rate, w->channels, w->bits,
		   w->is_float ? " (float)" : "", (unsigned long long)w->frames);
	return w;
}

void wavpackdec_close(wavpackdec_t *w) {
	if (!w) {
		return;
	}
	if (w->wpc) {
		WavpackCloseFile(w->wpc);
	}
	free(w->scratch);
	free(w);
}

int wavpackdec_channels(const wavpackdec_t *w) { return w ? w->channels : 0; }
int wavpackdec_sample_rate(const wavpackdec_t *w) { return w ? w->sample_rate : 0; }
int wavpackdec_bits(const wavpackdec_t *w) { return w ? w->bits : 16; }
uint64_t wavpackdec_total_frames(const wavpackdec_t *w) { return w ? w->frames : 0; }

// ---------------------------------------------------------------------------
// conversion
// ---------------------------------------------------------------------------

// Floating-point files arrive as floats inside 32-bit words (the bit pattern,
// not the value). OPEN_NORMALIZE has already brought them to +/-1.0, so all
// that is left is scaling and clipping, because a float can exceed unity and an
// integer cannot.
static int32_t float_word_to_q24(int32_t word) {
	float f;
	memcpy(&f, &word, sizeof(f));
	float scaled = f * 8388608.0f; // 2^23
	if (scaled > 8388607.0f) {
		scaled = 8388607.0f;
	} else if (scaled < -8388608.0f) {
		scaled = -8388608.0f;
	}
	return (int32_t)scaled;
}

// From right-justified at the file's depth to left-justified in 32 bits: the
// contract dr_flac and libsndfile keep, and what the player's 32-bit path
// expects.
static void widen_in_place(int32_t *buf, uint64_t count, int bits, bool is_float) {
	if (is_float) {
		for (uint64_t i = 0; i < count; i++) {
			buf[i] = float_word_to_q24(buf[i]) << 8;
		}
		return;
	}
	int shift = 32 - bits;
	if (shift <= 0) {
		return; // already 32 bit, nothing to shift
	}
	for (uint64_t i = 0; i < count; i++) {
		buf[i] = (int32_t)((uint32_t)buf[i] << shift);
	}
}

static bool ensure_scratch(wavpackdec_t *w, uint64_t frames) {
	if (w->scratch && w->scratch_frames >= frames) {
		return true;
	}
	int32_t *grown = realloc(w->scratch, (size_t)frames * (size_t)w->channels * sizeof(int32_t));
	if (!grown) {
		return false;
	}
	w->scratch = grown;
	w->scratch_frames = frames;
	return true;
}

// ---------------------------------------------------------------------------
// reading
// ---------------------------------------------------------------------------

uint64_t wavpackdec_read_s32(wavpackdec_t *w, uint64_t frames, int32_t *out) {
	if (!w || !out || frames == 0) {
		return 0;
	}
	// WavpackUnpackSamples counts in frames and writes channels * frames words,
	// which is exactly the caller's buffer size, so it decodes in place there
	// and is widened in place afterwards.
	uint32_t got = WavpackUnpackSamples(w->wpc, out, (uint32_t)frames);
	if (got == 0) {
		return 0;
	}
	widen_in_place(out, (uint64_t)got * (uint64_t)w->channels, w->bits, w->is_float);
	return got;
}

uint64_t wavpackdec_read_s16(wavpackdec_t *w, uint64_t frames, short *out) {
	if (!w || !out || frames == 0) {
		return 0;
	}
	if (!ensure_scratch(w, frames)) {
		return 0;
	}

	uint32_t got = WavpackUnpackSamples(w->wpc, w->scratch, (uint32_t)frames);
	if (got == 0) {
		return 0;
	}

	uint64_t count = (uint64_t)got * (uint64_t)w->channels;
	if (w->is_float) {
		for (uint64_t i = 0; i < count; i++) {
			out[i] = (short)(float_word_to_q24(w->scratch[i]) >> 8);
		}
	} else if (w->bits >= 16) {
		int shift = w->bits - 16;
		for (uint64_t i = 0; i < count; i++) {
			out[i] = (short)(w->scratch[i] >> shift);
		}
	} else {
		int shift = 16 - w->bits;
		for (uint64_t i = 0; i < count; i++) {
			out[i] = (short)(w->scratch[i] << shift);
		}
	}
	return got;
}

bool wavpackdec_seek(wavpackdec_t *w, uint64_t frame) {
	if (!w) {
		return false;
	}
	return WavpackSeekSample64(w->wpc, (int64_t)frame) != 0;
}
