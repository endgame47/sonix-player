#ifndef AUDIO_KISS_FFT_H
#define AUDIO_KISS_FFT_H

#include <stddef.h>

typedef float kiss_fft_scalar;

typedef struct {
	kiss_fft_scalar r;
	kiss_fft_scalar i;
} kiss_fft_cpx;

typedef struct kiss_fft_state *kiss_fft_cfg;

kiss_fft_cfg kiss_fft_alloc(int nfft, int inverse_fft, void *mem, size_t *lenmem);
void kiss_fft(kiss_fft_cfg cfg, const kiss_fft_cpx *input, kiss_fft_cpx *output);
void kiss_fft_free(kiss_fft_cfg cfg);

#endif
