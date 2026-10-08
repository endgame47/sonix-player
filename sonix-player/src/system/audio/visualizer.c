#include "visualizer.h"
#include "kiss_fft.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t visualizer_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint8_t bands[VISUALIZER_BANDS];
static kiss_fft_cfg fft_config;

static void analyse(const int16_t *samples, size_t frames, int channels, int sample_rate) {
	kiss_fft_cpx complex[VISUALIZER_WINDOW_SIZE] = {{0.0f, 0.0f}};
	const size_t count = frames < VISUALIZER_WINDOW_SIZE ? frames : VISUALIZER_WINDOW_SIZE;
	const double scale = 1.0 / (double)(channels * 32768.0);

	for (size_t i = 0; i < count; i++) {
		int64_t mixed = 0;
		for (int channel = 0; channel < channels; channel++) {
			mixed += (int64_t)samples[i * (size_t)channels + (size_t)channel];
		}
		complex[i].r = (float)((double)mixed * scale);
	}

	if (fft_config == NULL) {
		fft_config = kiss_fft_alloc(VISUALIZER_WINDOW_SIZE, 0, NULL, NULL);
	}
	if (fft_config == NULL) {
		return;
	}
	kiss_fft(fft_config, complex, complex);

	for (int i = 0; i < VISUALIZER_BANDS; i++) {
		const double frequency = 20.0 * pow(20000.0 / 20.0, (double)i / (VISUALIZER_BANDS - 1));
		const int bin = (int)fmin((double)(VISUALIZER_WINDOW_SIZE - 1), frequency * VISUALIZER_WINDOW_SIZE / (double)sample_rate);
		const double magnitude = hypot((double)complex[bin].r, (double)complex[bin].i);
		bands[i] = (uint8_t)fmin(255.0, magnitude * 255.0);
	}
}

void visualizer_reset(void) {
	pthread_mutex_lock(&visualizer_mutex);
	memset(bands, 0, sizeof(bands));
	pthread_mutex_unlock(&visualizer_mutex);
}

void visualizer_feed(const int16_t *samples, size_t frames, int channels, int sample_rate, int bits) {
	if (!samples || frames == 0 || channels <= 0 || sample_rate <= 0 || bits != 16) {
		return;
	}
	pthread_mutex_lock(&visualizer_mutex);
	analyse(samples, frames, channels, sample_rate);
	pthread_mutex_unlock(&visualizer_mutex);
}

void visualizer_feed_pcm(const void *samples, size_t frames, int channels, int sample_rate, int bits) {
	if (!samples || frames == 0 || channels <= 0 || sample_rate <= 0) {
		return;
	}

	if (bits == 16) {
		visualizer_feed((const int16_t *)samples, frames, channels, sample_rate, bits);
		return;
	}
	if (bits != 32) {
		return;
	}

	const int32_t *source = (const int32_t *)samples;
	const size_t sample_count = frames * (size_t)channels;
	int16_t *converted = malloc(sample_count * sizeof(*converted));
	if (!converted) {
		return;
	}
	for (size_t i = 0; i < sample_count; i++) {
		const int64_t value = source[i];
		if (value >= 32768) {
			converted[i] = 32767;
		} else if (value <= -32768) {
			converted[i] = -32768;
		} else {
			converted[i] = (int16_t)value;
		}
	}

	pthread_mutex_lock(&visualizer_mutex);
	analyse(converted, frames, channels, sample_rate);
	pthread_mutex_unlock(&visualizer_mutex);
	free(converted);
}

void visualizer_get_bands(uint8_t *out) {
	if (!out) {
		return;
	}
	pthread_mutex_lock(&visualizer_mutex);
	memcpy(out, bands, sizeof(bands));
	pthread_mutex_unlock(&visualizer_mutex);
}

void visualizer_release(void) { pthread_mutex_destroy(&visualizer_mutex); }
