#include "../src/system/audio/kiss_fft.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void) {
	kiss_fft_cfg config = kiss_fft_alloc(4, 0, NULL, NULL);
	assert(config != NULL);

	kiss_fft_cpx input[4] = {{1.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};
	kiss_fft_cpx output[4];
	kiss_fft(config, input, output);

	for (int i = 0; i < 4; i++) {
		assert(fabsf(output[i].r - 1.0f) < 0.0001f);
		assert(fabsf(output[i].i) < 0.0001f);
	}

	kiss_fft_free(config);
	puts("kiss fft test passed");
	return 0;
}
