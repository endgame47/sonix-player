#include "art_shrink.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/image/jpeg_planes.h"
#include "src/system/image/jpeg_scaled.h"
#include "src/system/image/png_scaled.h"

// Only the JPEG writer is wanted, into memory: no stdio variants.
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
// Static, so the writers not called here are unused, which is not a fault.
#pragma GCC diagnostic ignored "-Wunused-function"
#include "src/system/image/stb_image_write.h"

// Below this the original goes as it is: re-encoding would save little and
// cost a decode.
#define SMALL_ENOUGH_BYTES (160 * 1024)

// The most a progressive JPEG's plane decode may take. The same order as the
// interface's own budget for a cover.
#define PLANES_BUDGET_BYTES (12u * 1024u * 1024u)

typedef struct {
	uint8_t *data;
	size_t len;
	size_t cap;
	bool failed;
} sink_t;

static void sink_write(void *context, void *data, int size) {
	sink_t *s = context;
	if (s->failed || size <= 0) {
		return;
	}
	if (s->len + (size_t)size > s->cap) {
		size_t want = s->cap ? s->cap * 2 : 64 * 1024;
		while (want < s->len + (size_t)size) {
			want *= 2;
		}
		uint8_t *grown = realloc(s->data, want);
		if (!grown) {
			s->failed = true;
			return;
		}
		s->data = grown;
		s->cap = want;
	}
	memcpy(s->data + s->len, data, (size_t)size);
	s->len += (size_t)size;
}

// RGB888 shrunk by area averaging: each output pixel is the mean of the source
// pixels it covers. Fixed point, one row of accumulators.
static uint8_t *shrink_rgb(const uint8_t *src, int sw, int sh, int dw, int dh) {
	uint8_t *dst = malloc((size_t)dw * (size_t)dh * 3);
	uint32_t *acc = calloc((size_t)dw * 3, sizeof(uint32_t));
	uint32_t *cnt = calloc((size_t)dw, sizeof(uint32_t));
	if (!dst || !acc || !cnt) {
		free(dst);
		free(acc);
		free(cnt);
		return NULL;
	}
	int row = 0;
	for (int y = 0; y < sh; y++) {
		int dy = (int)((int64_t)y * dh / sh);
		if (dy != row) {
			for (int x = 0; x < dw; x++) {
				uint32_t n = cnt[x] ? cnt[x] : 1;
				for (int c = 0; c < 3; c++) {
					dst[((size_t)row * dw + x) * 3 + c] = (uint8_t)(acc[x * 3 + c] / n);
				}
			}
			memset(acc, 0, (size_t)dw * 3 * sizeof(uint32_t));
			memset(cnt, 0, (size_t)dw * sizeof(uint32_t));
			row = dy;
		}
		const uint8_t *line = src + (size_t)y * sw * 3;
		for (int x = 0; x < sw; x++) {
			int dx = (int)((int64_t)x * dw / sw);
			acc[dx * 3] += line[x * 3];
			acc[dx * 3 + 1] += line[x * 3 + 1];
			acc[dx * 3 + 2] += line[x * 3 + 2];
			cnt[dx]++;
		}
	}
	for (int x = 0; x < dw; x++) {
		uint32_t n = cnt[x] ? cnt[x] : 1;
		for (int c = 0; c < 3; c++) {
			dst[((size_t)row * dw + x) * 3 + c] = (uint8_t)(acc[x * 3 + c] / n);
		}
	}
	free(acc);
	free(cnt);
	return dst;
}

uint8_t *art_shrink_to_jpeg(const uint8_t *data, size_t size, int max_side, int quality, size_t *out_len) {
	if (!data || size < 4 || max_side < 16 || !out_len || size <= SMALL_ENOUGH_BYTES) {
		return NULL;
	}

	int w = 0, h = 0;
	uint8_t *rgb = NULL;
	if (data[0] == 0xFF && data[1] == 0xD8) {
		// TJpgDec keeps the short side at twice its argument where it can: half
		// the wanted side leaves the shrink below one reduction's worth to do,
		// and a 3000-pixel cover comes out at 750 rather than 1500.
		rgb = jpeg_scaled_decode(data, size, max_side / 2, &w, &h, NULL, NULL);
		if (!rgb) {
			rgb = jpeg_planes_decode(data, size, max_side, PLANES_BUDGET_BYTES, &w, &h, NULL);
		}
	} else if (data[0] == 0x89 && data[1] == 'P') {
		rgb = png_scaled_decode(data, size, max_side, &w, &h);
	}
	if (!rgb || w <= 0 || h <= 0) {
		free(rgb);
		return NULL;
	}

	int longest = w > h ? w : h;
	if (longest > max_side) {
		int dw = (int)((int64_t)w * max_side / longest);
		int dh = (int)((int64_t)h * max_side / longest);
		dw = dw < 1 ? 1 : dw;
		dh = dh < 1 ? 1 : dh;
		uint8_t *small = shrink_rgb(rgb, w, h, dw, dh);
		free(rgb);
		if (!small) {
			return NULL;
		}
		rgb = small;
		w = dw;
		h = dh;
	}

	sink_t sink = {0};
	int ok = stbi_write_jpg_to_func(sink_write, &sink, w, h, 3, rgb, quality);
	free(rgb);
	if (!ok || sink.failed || sink.len == 0 || sink.len >= size) {
		free(sink.data);
		return NULL;
	}
	*out_len = sink.len;
	return sink.data;
}

uint8_t *art_encode_jpeg(const uint8_t *rgb, int w, int h, int quality, size_t *out_len) {
	if (!rgb || w <= 0 || h <= 0 || !out_len) {
		return NULL;
	}
	sink_t sink = {0};
	int ok = stbi_write_jpg_to_func(sink_write, &sink, w, h, 3, rgb, quality);
	if (!ok || sink.failed || sink.len == 0) {
		free(sink.data);
		return NULL;
	}
	*out_len = sink.len;
	return sink.data;
}
