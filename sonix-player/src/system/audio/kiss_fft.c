#include "kiss_fft.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct kiss_fft_state {
	int nfft;
};

kiss_fft_cfg kiss_fft_alloc(int nfft, int inverse_fft, void *mem, size_t *lenmem)
{
	(void)inverse_fft;
	if (nfft <= 0 || (nfft & (nfft - 1)) != 0) {
		return NULL;
	}

	size_t state_size = sizeof(struct kiss_fft_state);
	if (lenmem != NULL) {
		*lenmem = state_size;
	}
	if (mem == NULL) {
		mem = malloc(state_size);
	}
	if (mem == NULL) {
		return NULL;
	}

	struct kiss_fft_state *state = mem;
	state->nfft = nfft;
	return state;
}

void kiss_fft(kiss_fft_cfg cfg, const kiss_fft_cpx *input, kiss_fft_cpx *output)
{
	if (cfg == NULL || input == NULL || output == NULL) {
		return;
	}

	kiss_fft_cpx working[1024];
	memcpy(working, input, (size_t)cfg->nfft * sizeof(kiss_fft_cpx));

	for (int i = 1, bit_reversed = 0; i < cfg->nfft; i++) {
		int bit = cfg->nfft >> 1;
		while (bit_reversed & bit) {
			bit_reversed ^= bit;
			bit >>= 1;
		}
		bit_reversed ^= bit;
		if (i < bit_reversed) {
			kiss_fft_cpx temporary = working[i];
			working[i] = working[bit_reversed];
			working[bit_reversed] = temporary;
		}
	}

	for (int block_size = 2; block_size <= cfg->nfft; block_size <<= 1) {
		const float angle = -2.0f * (float)acos(-1.0) / (float)block_size;
		const float twiddle_real = cosf(angle);
		const float twiddle_imag = sinf(angle);
		const int half = block_size / 2;

		for (int start = 0; start < cfg->nfft; start += block_size) {
			float factor_real = 1.0f;
			float factor_imag = 0.0f;
			for (int offset = 0; offset < half; offset++) {
				const kiss_fft_cpx even = working[start + offset];
				const kiss_fft_cpx odd = working[start + offset + half];
				const kiss_fft_cpx product = {
					odd.r * factor_real - odd.i * factor_imag,
					odd.r * factor_imag + odd.i * factor_real
				};
				working[start + offset].r = even.r + product.r;
				working[start + offset].i = even.i + product.i;
				working[start + offset + half].r = even.r - product.r;
				working[start + offset + half].i = even.i - product.i;

				const float next_real = factor_real * twiddle_real - factor_imag * twiddle_imag;
				factor_imag = factor_real * twiddle_imag + factor_imag * twiddle_real;
				factor_real = next_real;
			}
		}
	}

	memcpy(output, working, (size_t)cfg->nfft * sizeof(kiss_fft_cpx));
}

void kiss_fft_free(kiss_fft_cfg cfg)
{
	free(cfg);
}
