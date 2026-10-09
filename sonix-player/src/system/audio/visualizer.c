// The now-playing visualizer's audio side.
//
// The playback thread decodes in blocks of about 90 ms (4096 frames at
// 44.1 kHz) and writes each one to ALSA in a single call, so whatever it
// analyses itself changes about eleven times a second, and an FFT run there
// costs the one thread that must never be late. Here it does neither: it
// mixes each finished block down to mono into a ring buffer and returns.
//
// The UI timer asks for a frame whenever it draws. It takes the
// VIS_FFT_SIZE samples being heard at that moment, windows them, runs the FFT
// and folds the bins into AUDIO_VISUALIZER_BINS bands on a log scale, in dB.
// What is heard is not the newest block: the device still holds up to its
// whole buffer in front of it (743 ms at 44.1 kHz with eight periods of 4096
// frames), which the feed asks ALSA for. And a block is written at once but
// plays for its whole length, so the window walks on at the playback rate
// from where the device was when the block went in: every frame drawn sees
// audio that moved on since the last one, in time with the sound.
//
// The ring only fills while the UI keeps asking: a visualizer that stopped
// drawing (closed, screen off, page left) stops the copying too, within
// VIS_POLL_TIMEOUT_MS, whatever path it left by.

#include "src/system/audio/visualizer.h"
#include "src/system/audio/audio.h"
#include "src/system/audio/kiss_fft.h"

#include <alsa/asoundlib.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define VIS_FFT_SIZE		1024	// 23 ms at 44.1 kHz, bins 43 Hz apart
#define VIS_RING_SIZE		65536	// a power of two: the device buffer, a block and the window
#define VIS_RING_MASK		(VIS_RING_SIZE - 1)
#define VIS_BASE_RATE		44100	// higher rates are averaged down towards this
#define VIS_POLL_TIMEOUT_MS	250	// no frame asked for this long: stop copying
#define VIS_STALE_MS		150	// after the last block has played out: silence
#define VIS_FREQ_LOW		50.0f	// the bands span this...
#define VIS_FREQ_HIGH		16000.0f	// ...to this, or to near Nyquist
#define VIS_DB_RANGE		60.0f	// a band reads 0 at -60 dB and 1 at 0 dB
#define VIS_TILT_DB_PER_OCT	3.0f	// music falls off with frequency; this lifts it back

static pthread_mutex_t vis_lock = PTHREAD_MUTEX_INITIALIZER;
static bool vis_enabled;
static long vis_poll_ms;	// the last time the UI asked for a frame

static float ring[VIS_RING_SIZE];
static uint32_t ring_pos;	// samples written in all; the ring index is its low bits
static uint32_t ring_filled;	// samples written, up to VIS_RING_SIZE
static int ring_rate;		// rate of the samples in the ring
static int ring_factor;		// input frames averaged into one sample
static float acc;		// the average being built across blocks
static int acc_n;

static uint32_t block_start;	// ring_pos before the newest block
static uint32_t block_len;	// samples it added
static uint32_t block_queued;	// samples the device held in front of it
static long block_ms;		// when it was written

static long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void audio_set_visualizer_enabled(bool enabled) {
	pthread_mutex_lock(&vis_lock);
	vis_enabled = enabled;
	vis_poll_ms = now_ms();
	ring_pos = 0;
	ring_filled = 0;
	ring_rate = 0;
	ring_factor = 0;
	acc = 0.0f;
	acc_n = 0;
	block_start = 0;
	block_len = 0;
	block_queued = 0;
	block_ms = 0;
	pthread_mutex_unlock(&vis_lock);
}

static bool vis_wanted(long now) {
	return vis_enabled && now - vis_poll_ms <= VIS_POLL_TIMEOUT_MS;
}

void visualizer_feed(const void *buf, int frames, int channels, int bits, int rate, snd_pcm_t *pcm) {
	if (!buf || frames <= 0 || channels <= 0 || rate <= 0 || (bits != 16 && bits != 32)) {
		return;
	}
	long now = now_ms();
	pthread_mutex_lock(&vis_lock);
	bool wanted = vis_wanted(now);
	pthread_mutex_unlock(&vis_lock);
	if (!wanted) {
		return;
	}
	// What the device still has to play before this block.
	snd_pcm_sframes_t queued = 0;
	if (!pcm || snd_pcm_delay(pcm, &queued) < 0 || queued < 0) {
		queued = 0;
	}

	pthread_mutex_lock(&vis_lock);
	if (!vis_wanted(now)) {
		pthread_mutex_unlock(&vis_lock);
		return;
	}

	// 88.2 kHz and up are averaged down by a whole factor, so the window
	// covers about the same time and the bins about the same width at every
	// rate. The average is a rough low-pass, which is all a picture needs.
	int factor = rate >= 2 * VIS_BASE_RATE ? rate / VIS_BASE_RATE : 1;
	if (factor != ring_factor || rate / factor != ring_rate) {
		ring_factor = factor;
		ring_rate = rate / factor;
		ring_filled = 0;
		acc = 0.0f;
		acc_n = 0;
	}
	const float scale = 1.0f / ((bits == 16 ? 32768.0f : 2147483648.0f) * (float)channels * (float)factor);

	block_start = ring_pos;
	for (int f = 0; f < frames; f++) {
		float sum = 0.0f;
		if (bits == 16) {
			const int16_t *s = (const int16_t *)buf + (size_t)f * (size_t)channels;
			for (int c = 0; c < channels; c++) {
				sum += (float)s[c];
			}
		} else {
			const int32_t *s = (const int32_t *)buf + (size_t)f * (size_t)channels;
			for (int c = 0; c < channels; c++) {
				sum += (float)s[c];
			}
		}
		acc += sum;
		if (++acc_n == factor) {
			ring[ring_pos & VIS_RING_MASK] = acc * scale;
			ring_pos++;
			if (ring_filled < VIS_RING_SIZE) {
				ring_filled++;
			}
			acc = 0.0f;
			acc_n = 0;
		}
	}
	block_len = ring_pos - block_start;
	block_queued = (uint32_t)(queued / factor);
	block_ms = now;
	pthread_mutex_unlock(&vis_lock);
}

bool audio_get_visualizer_frame(float spectrum[AUDIO_VISUALIZER_BINS], float *level, float *peak) {
	static kiss_fft_cfg cfg;
	static bool cfg_tried;
	static float window[VIS_FFT_SIZE];
	static kiss_fft_cpx in[VIS_FFT_SIZE];
	static kiss_fft_cpx out[VIS_FFT_SIZE];
	static float mag[VIS_FFT_SIZE / 2 + 1];
	float samples[VIS_FFT_SIZE];

	memset(spectrum, 0, sizeof(float) * AUDIO_VISUALIZER_BINS);
	*level = 0.0f;
	*peak = 0.0f;

	if (!cfg_tried) {
		cfg_tried = true;
		cfg = kiss_fft_alloc(VIS_FFT_SIZE, 0, NULL, NULL);
		for (int i = 0; i < VIS_FFT_SIZE; i++) {
			window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(VIS_FFT_SIZE - 1)));
		}
	}

	long now = now_ms();
	pthread_mutex_lock(&vis_lock);
	vis_poll_ms = now;
	int rate = ring_rate;
	long age = now - block_ms;
	bool live = vis_enabled && cfg && rate > 0 && block_len > 0 &&
				age <= (long)((int64_t)(block_queued + block_len) * 1000 / rate) + VIS_STALE_MS;
	if (live) {
		// The sample being heard: where the device was when the newest block
		// went in, moved on at the playback rate since.
		uint32_t walked = (uint32_t)((int64_t)age * rate / 1000);
		uint32_t ahead = block_queued + block_len;
		uint32_t end = ring_pos - ahead + (walked < ahead ? walked : ahead);
		uint32_t start = end - VIS_FFT_SIZE;
		// Too early in the stream, or too far back for the ring.
		if (ring_pos - start > ring_filled) {
			live = false;
		} else {
			for (int i = 0; i < VIS_FFT_SIZE; i++) {
				samples[i] = ring[(start + (uint32_t)i) & VIS_RING_MASK];
			}
		}
	}
	pthread_mutex_unlock(&vis_lock);
	if (!live) {
		return false;
	}

	float sumsq = 0.0f;
	float pk = 0.0f;
	for (int i = 0; i < VIS_FFT_SIZE; i++) {
		sumsq += samples[i] * samples[i];
		pk = fmaxf(pk, fabsf(samples[i]));
		in[i].r = (kiss_fft_scalar)(samples[i] * window[i]);
		in[i].i = 0;
	}
	*level = fminf(1.0f, sqrtf(sumsq / VIS_FFT_SIZE));
	*peak = fminf(1.0f, pk);

	kiss_fft(cfg, in, out);
	for (int i = 0; i <= VIS_FFT_SIZE / 2; i++) {
		mag[i] = hypotf(out[i].r, out[i].i);
	}

	// A full-scale sine through the Hann window peaks at FFT_SIZE / 4: 0 dB.
	const float ref = VIS_FFT_SIZE / 4.0f;
	const float bin_hz = (float)rate / VIS_FFT_SIZE;
	const float high = fminf(VIS_FREQ_HIGH, (float)rate * 0.45f);
	const float ratio = high / VIS_FREQ_LOW;
	for (int b = 0; b < AUDIO_VISUALIZER_BINS; b++) {
		float f0 = VIS_FREQ_LOW * powf(ratio, (float)b / AUDIO_VISUALIZER_BINS);
		float f1 = VIS_FREQ_LOW * powf(ratio, (float)(b + 1) / AUDIO_VISUALIZER_BINS);
		float k0 = fmaxf(1.0f, f0 / bin_hz);
		float k1 = fminf((float)(VIS_FFT_SIZE / 2), f1 / bin_hz);
		float m;
		if (k1 - k0 < 1.0f) {
			// Narrower than a bin (the low bands): read between the two
			// bins around its centre, so neighbouring bars do not repeat
			// the same value.
			float k = (k0 + k1) / 2.0f;
			int i = (int)k;
			float t = k - (float)i;
			m = mag[i] * (1.0f - t) + mag[i + 1 <= VIS_FFT_SIZE / 2 ? i + 1 : i] * t;
		} else {
			m = 0.0f;
			for (int i = (int)k0; i <= (int)k1; i++) {
				m = fmaxf(m, mag[i]);
			}
		}
		float db = 20.0f * log10f(m / ref + 1e-9f);
		db += VIS_TILT_DB_PER_OCT * log2f(sqrtf(f0 * f1) / 1000.0f);
		spectrum[b] = fminf(1.0f, fmaxf(0.0f, (db + VIS_DB_RANGE) / VIS_DB_RANGE));
	}
	return true;
}
