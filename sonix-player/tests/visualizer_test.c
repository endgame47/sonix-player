#include "../src/system/audio/visualizer.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void feed_tone(uint32_t frequency, int16_t *samples) {
	for (size_t i = 0; i < VISUALIZER_WINDOW_SIZE; i++) {
		double phase = 2.0 * M_PI * frequency * (double)i / 44100.0;
		samples[i] = (int16_t)(32767.0 * sin(phase));
	}
	visualizer_feed(samples, VISUALIZER_WINDOW_SIZE, 1, 44100, 16);
}

int main(void) {
	visualizer_reset();

	int16_t low_tone[VISUALIZER_WINDOW_SIZE];
	int16_t high_tone[VISUALIZER_WINDOW_SIZE];
	feed_tone(80, low_tone);

	uint8_t bands[VISUALIZER_BANDS];
	visualizer_get_bands(bands);
	uint8_t low_band = 0;
	for (int i = 0; i < 12; i++) {
		low_band += bands[i];
	}

	visualizer_reset();
	feed_tone(8000, high_tone);
	visualizer_get_bands(bands);
	uint8_t high_band = 0;
	for (int i = VISUALIZER_BANDS - 12; i < VISUALIZER_BANDS; i++) {
		high_band += bands[i];
	}

	assert(low_band > high_band);

	visualizer_reset();
	visualizer_feed((const int16_t[VISUALIZER_WINDOW_SIZE]){0}, VISUALIZER_WINDOW_SIZE, 1, 44100, 16);
	visualizer_get_bands(bands);
	for (int i = 0; i < VISUALIZER_BANDS; i++) {
		assert(bands[i] == 0);
	}

	puts("visualizer test passed");
	return 0;
}
