#ifndef AUDIO_VISUALIZER_H
#define AUDIO_VISUALIZER_H

#include <stddef.h>
#include <stdint.h>

#define VISUALIZER_BANDS 48
#define VISUALIZER_WINDOW_SIZE 1024

void visualizer_reset(void);
void visualizer_feed(const int16_t *samples, size_t frames, int channels, int sample_rate, int bits);
void visualizer_feed_pcm(const void *samples, size_t frames, int channels, int sample_rate, int bits);
void visualizer_get_bands(uint8_t *bands);
void visualizer_release(void);

#endif
