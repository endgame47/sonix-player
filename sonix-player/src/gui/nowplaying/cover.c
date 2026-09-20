#include "cover.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "src/system/db/sqlite3.h"

#include "src/system/library/albumart.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/image/jpeg_planes.h"
#include "src/system/image/jpeg_scaled.h"
#include "src/system/image/png_scaled.h"
#include "src/system/image/stb_image_decl.h"
#include "src/system/core/utils.h"

// The backdrop is blurred at half resolution and stretched back up. Blurring
// the small copy (rather than relying on the stretch alone) is what keeps it
// smooth: a plain upscale of a heavily shrunk image shows the interpolation
// facets as visible blocks.
#define BACKDROP_SCALE_DIVISOR 2

// Box blur radius, in pixels of the half-size image. Repeated passes of a box
// blur approximate a gaussian closely enough that nothing looks boxy.
#define BACKDROP_BLUR_RADIUS 9
#define BACKDROP_BLUR_PASSES 3

// How far the backdrop is pushed towards black (dark theme) or white (light
// theme) so the text on top of it stays readable. 100 = untouched.
#define BACKDROP_BRIGHTNESS_PCT 42

static bool backdrop_light;
static bool backdrop_upright;

void cover_set_backdrop_light(bool light) { backdrop_light = light; }

void cover_set_backdrop_upright(bool upright) { backdrop_upright = upright; }

// ---------------------------------------------------------------------------
// raw (decoded, RGB888) images
// ---------------------------------------------------------------------------

typedef struct {
	uint8_t *pixels; // RGB888, w*h*3 bytes
	int w;
	int h;
} raw_image_t;

// A rectangle of a source image, in source pixels.
typedef struct {
	int x;
	int y;
	int w;
	int h;
} crop_t;

static inline uint16_t rgb_to_rgb565(uint32_t r, uint32_t g, uint32_t b) {
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void raw_free(raw_image_t *img) {
	if (!img)
		return;
	free(img->pixels);
	img->pixels = NULL;
}

// How much memory a single decode may take.
//
// On the device the budget is small and re-read every time: a quarter of what is
// free now, capped at 8 MB. It must not be derived from MemAvailable, which is
// mostly page cache -- mapped fonts, the player's own code, the audio decoder's
// read-ahead. The kernel cannot kill the player (it runs at oom_score_adj -500),
// so a large allocation is served by evicting all of that cache, and every glyph
// draw and instruction fetch then becomes a page fault against slow flash.
//
// A refused cover is a glyph; a thrashing kernel is a frozen device. The cap is
// what keeps the second from happening, and a larger one buys no artwork: the
// JPEG path decodes at half, a quarter or an eighth (see jpeg_planes.h), so even
// a poster-sized progressive cover fits in two megabytes. The host build keeps a
// large budget so a PC music library loses no artwork.
#define DECODE_BUDGET_MIN_BYTES (2 * 1024 * 1024)
#ifdef HOST_BUILD
#define DECODE_BUDGET_MAX_BYTES (64 * 1024 * 1024)
#define DECODE_BUDGET_DIVISOR 1
#define DECODE_BUDGET_RESERVE_BYTES (3 * 1024 * 1024)
#else
#define DECODE_BUDGET_MAX_BYTES (8 * 1024 * 1024)
#define DECODE_BUDGET_DIVISOR 4
#define DECODE_BUDGET_RESERVE_BYTES 0
#endif

static size_t read_mem_available(void) {
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) {
		return 0;
	}

	char line[256];
	size_t kb = 0;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "MemAvailable: %zu kB", &kb) == 1) {
			break;
		}
	}

	fclose(f);
	return kb * 1024;
}

static size_t decode_budget_bytes(void) {
	// Recomputed on every call: what was free at boot says nothing about what is
	// free after an hour of playing. /proc/meminfo costs a few microseconds and
	// this only runs on the worker thread.
	size_t available = read_mem_available();
	size_t budget = available / DECODE_BUDGET_DIVISOR;

	if (budget > DECODE_BUDGET_RESERVE_BYTES) {
		budget -= DECODE_BUDGET_RESERVE_BYTES;
	}

	if (budget < DECODE_BUDGET_MIN_BYTES)
		budget = DECODE_BUDGET_MIN_BYTES;
	if (budget > DECODE_BUDGET_MAX_BYTES)
		budget = DECODE_BUDGET_MAX_BYTES;

	return budget;
}

// Peak bytes per source pixel while stb_image decodes a PNG: four channels out,
// plus the filtered scanline buffer. JPEGs need no estimate: the plane decoder
// works their peak out from the frame header and refuses on its own.
#define PNG_DECODE_BYTES_PER_PIXEL 8

// Decodes compressed JPEG/PNG bytes to RGB888. Alpha is dropped rather than
// composited: covers are opaque in practice, and three channels keep the
// intermediate buffer smaller.
//
// max_box is the largest output anybody wants from this decode. Baseline JPEGs,
// which is what album art overwhelmingly is, go through the vendored TJpgDec
// with its 1/2, 1/4, 1/8 output scaling: a 3000x3000 cover comes out as 750x750
// in under two megabytes instead of a 45 MB full-resolution pass this device
// cannot afford. That is how the stock player shows poster-sized covers on the
// same 64 MB. Progressive JPEGs and PNGs fall back to stb_image under the
// memory budget.
static bool decode_raw(const uint8_t *data, size_t size, int max_box, raw_image_t *out) {
	memset(out, 0, sizeof(*out));

	if (!data || size == 0)
		return false;

	if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8 && max_box > 0) {
		int w = 0, h = 0, src_w = 0, src_h = 0;
		uint8_t *pixels = jpeg_scaled_decode(data, size, max_box, &w, &h, &src_w, &src_h);
		if (pixels) {
			// Both sizes, because the output on its own says nothing about what
			// happened: TJpgDec only ever halves, and a picture already smaller
			// than the box comes out untouched, so one number alone would make a
			// 360x360 cover look like something that had been reduced.
			fprintf(stderr, "cover: decoded jpeg %dx%d -> %dx%d\n", src_w, src_h, w, h);
			out->pixels = pixels;
			out->w = w;
			out->h = h;
			return true;
		}

		// Not readable by TJpgDec, which in practice means progressive. Those go
		// through stb, but not through its ordinary loader: that one interleaves
		// a full-resolution RGB buffer next to the planes it was built from,
		// which for a 1200x1200 cover peaks around 11 MB -- refused by any sane
		// budget on a 64 MB device.
		//
		// The plane decoder averages the components down to the size actually
		// wanted, and, when even that does not fit, decodes at half, a quarter or
		// an eighth by keeping only the low-frequency corner of each block. It
		// picks the best scale the budget allows and says which in the log. The
		// 2x margin is the same one the TJpgDec path leaves for the resampler
		// below.
		size_t peak = 0;
		pixels = jpeg_planes_decode(data, size, max_box * 2, decode_budget_bytes(), &w, &h, &peak);
		if (pixels) {
			fprintf(stderr, "cover: decoded progressive jpeg to %dx%d (planes, peak about %zu KB)\n", w, h,
					peak / 1024);
			out->pixels = pixels;
			out->w = w;
			out->h = h;
			return true;
		}
		if (peak > 0) {
			fprintf(stderr, "cover: skipping jpeg artwork, decoding it would need about %zu KB (budget %zu KB)\n",
					peak / 1024, decode_budget_bytes() / 1024);
		}
		return false;
	}

	if (size >= 4 && data[0] == 0x89 && data[1] == 'P' && max_box > 0) {
		// The streaming decoder, not stb: stb inflates the whole image first,
		// which for a 3000x3000 file is far more than a 64 MB device can spare.
		// This one works scanline by scanline and downsamples as it goes, so the
		// source size stops mattering. The 2x margin gives the final resampler
		// enough to average over, as on the JPEG path.
		int w = 0, h = 0;
		uint8_t *pixels = png_scaled_decode(data, size, max_box * 2, &w, &h);
		if (pixels) {
			fprintf(stderr, "cover: decoded png to %dx%d (scaled)\n", w, h);
			out->pixels = pixels;
			out->w = w;
			out->h = h;
			return true;
		}
		// Interlaced or 16-bit: falls through to stb, under the memory budget.
	}

	int w = 0, h = 0, comp = 0;
	if (!stbi_info_from_memory(data, (int)size, &w, &h, &comp))
		return false;
	if (w <= 0 || h <= 0)
		return false;
	int bpp = PNG_DECODE_BYTES_PER_PIXEL;
	size_t needed = (size_t)w * (size_t)h * (size_t)bpp;
	size_t budget = decode_budget_bytes();
	if (needed > budget) {
		// Better no artwork than an allocation that wedges the whole player
		// halfway down a folder.
		fprintf(stderr, "cover: skipping %dx%d artwork, decoding it would need about %zu KB (budget %zu KB)\n", w, h,
				needed / 1024, budget / 1024);
		return false;
	}

	uint8_t *pixels = stbi_load_from_memory(data, (int)size, &w, &h, &comp, 3);
	if (!pixels) {
		fprintf(stderr, "cover: decode of %dx%d artwork failed\n", w, h);
		return false;
	}

	// One line per decode: this is the most expensive thing the browser does, and
	// the log shows how many decodes a folder actually triggers.
	fprintf(stderr, "cover: decoded %dx%d (%d bytes/px)\n", w, h, bpp);

	out->pixels = pixels;
	out->w = w;
	out->h = h;
	return true;
}

// The largest centred rectangle of the source with the target's aspect ratio.
// This turns a square cover into an edge-to-edge banner: the sides stay put and
// the overflow at top and bottom is simply never sampled.
static crop_t crop_to_aspect(int src_w, int src_h, int dst_w, int dst_h) {
	crop_t c;

	if ((int64_t)src_w * dst_h > (int64_t)src_h * dst_w) {
		c.h = src_h;
		c.w = (int)((int64_t)src_h * dst_w / dst_h);
	} else {
		c.w = src_w;
		c.h = (int)((int64_t)src_w * dst_h / dst_w);
	}
	if (c.w < 1)
		c.w = 1;
	if (c.h < 1)
		c.h = 1;

	c.x = (src_w - c.w) / 2;
	c.y = (src_h - c.h) / 2;
	return c;
}

// Resamples a crop of the source into a dst_w x dst_h RGB888 buffer, optionally
// upside down. It averages over the source footprint when shrinking, which
// keeps thumbnails from turning into noise, and interpolates when growing, so
// small covers stretched edge to edge do not come out blocky.
static uint8_t *resample_rgb(const raw_image_t *src, const crop_t *crop, int dst_w, int dst_h, bool flip_v) {
	if (dst_w < 1 || dst_h < 1)
		return NULL;

	uint8_t *dst = malloc((size_t)dst_w * dst_h * 3);
	if (!dst)
		return NULL;

	bool magnify = (dst_w >= crop->w);

	for (int y = 0; y < dst_h; y++) {
		int out_row = flip_v ? (dst_h - 1 - y) : y;
		uint8_t *dst_row = dst + (size_t)out_row * dst_w * 3;

		for (int x = 0; x < dst_w; x++) {
			uint32_t r, g, b;

			if (magnify) {
				// Bilinear: sample the source at the centre of this output pixel.
				float fx = ((float)x + 0.5f) * crop->w / dst_w - 0.5f;
				float fy = ((float)y + 0.5f) * crop->h / dst_h - 0.5f;
				if (fx < 0)
					fx = 0;
				if (fy < 0)
					fy = 0;

				int x0 = (int)fx, y0 = (int)fy;
				int x1 = x0 + 1 < crop->w ? x0 + 1 : x0;
				int y1 = y0 + 1 < crop->h ? y0 + 1 : y0;
				float tx = fx - x0, ty = fy - y0;

				const uint8_t *p00 = src->pixels + (((size_t)(crop->y + y0) * src->w) + crop->x + x0) * 3;
				const uint8_t *p10 = src->pixels + (((size_t)(crop->y + y0) * src->w) + crop->x + x1) * 3;
				const uint8_t *p01 = src->pixels + (((size_t)(crop->y + y1) * src->w) + crop->x + x0) * 3;
				const uint8_t *p11 = src->pixels + (((size_t)(crop->y + y1) * src->w) + crop->x + x1) * 3;

				float w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty);
				float w01 = (1 - tx) * ty, w11 = tx * ty;

				r = (uint32_t)(p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11);
				g = (uint32_t)(p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11);
				b = (uint32_t)(p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11);
			} else {
				// Box average over every source pixel this output pixel covers.
				int sy0 = crop->y + (int)((int64_t)y * crop->h / dst_h);
				int sy1 = crop->y + (int)((int64_t)(y + 1) * crop->h / dst_h);
				int sx0 = crop->x + (int)((int64_t)x * crop->w / dst_w);
				int sx1 = crop->x + (int)((int64_t)(x + 1) * crop->w / dst_w);
				if (sy1 <= sy0)
					sy1 = sy0 + 1;
				if (sx1 <= sx0)
					sx1 = sx0 + 1;

				uint32_t sr = 0, sg = 0, sb = 0, n = 0;
				for (int sy = sy0; sy < sy1; sy++) {
					const uint8_t *row = src->pixels + (size_t)sy * src->w * 3;
					for (int sx = sx0; sx < sx1; sx++) {
						const uint8_t *px = row + (size_t)sx * 3;
						sr += px[0];
						sg += px[1];
						sb += px[2];
						n++;
					}
				}
				r = sr / n;
				g = sg / n;
				b = sb / n;
			}

			uint8_t *out_px = dst_row + (size_t)x * 3;
			out_px[0] = (uint8_t)r;
			out_px[1] = (uint8_t)g;
			out_px[2] = (uint8_t)b;
		}
	}

	return dst;
}

// One separable box-blur pass over an RGB888 buffer, using a running sum so the
// cost does not grow with the radius. scratch must hold w*h*3 bytes.
static void box_blur_pass(uint8_t *img, uint8_t *scratch, int w, int h, int radius) {
	if (radius < 1)
		return;

	// horizontal: img -> scratch
	for (int y = 0; y < h; y++) {
		const uint8_t *row = img + (size_t)y * w * 3;
		uint8_t *out = scratch + (size_t)y * w * 3;

		int32_t sum[3] = {0, 0, 0};
		int count = 0;
		for (int x = 0; x <= radius && x < w; x++) {
			sum[0] += row[x * 3];
			sum[1] += row[x * 3 + 1];
			sum[2] += row[x * 3 + 2];
			count++;
		}

		for (int x = 0; x < w; x++) {
			out[x * 3] = (uint8_t)(sum[0] / count);
			out[x * 3 + 1] = (uint8_t)(sum[1] / count);
			out[x * 3 + 2] = (uint8_t)(sum[2] / count);

			int add = x + radius + 1;
			int drop = x - radius;
			if (add < w) {
				sum[0] += row[add * 3];
				sum[1] += row[add * 3 + 1];
				sum[2] += row[add * 3 + 2];
				count++;
			}
			if (drop >= 0) {
				sum[0] -= row[drop * 3];
				sum[1] -= row[drop * 3 + 1];
				sum[2] -= row[drop * 3 + 2];
				count--;
			}
		}
	}

	// vertical: scratch -> img
	for (int x = 0; x < w; x++) {
		int32_t sum[3] = {0, 0, 0};
		int count = 0;
		for (int y = 0; y <= radius && y < h; y++) {
			const uint8_t *px = scratch + ((size_t)y * w + x) * 3;
			sum[0] += px[0];
			sum[1] += px[1];
			sum[2] += px[2];
			count++;
		}

		for (int y = 0; y < h; y++) {
			uint8_t *out = img + ((size_t)y * w + x) * 3;
			out[0] = (uint8_t)(sum[0] / count);
			out[1] = (uint8_t)(sum[1] / count);
			out[2] = (uint8_t)(sum[2] / count);

			int add = y + radius + 1;
			int drop = y - radius;
			if (add < h) {
				const uint8_t *px = scratch + ((size_t)add * w + x) * 3;
				sum[0] += px[0];
				sum[1] += px[1];
				sum[2] += px[2];
				count++;
			}
			if (drop >= 0) {
				const uint8_t *px = scratch + ((size_t)drop * w + x) * 3;
				sum[0] -= px[0];
				sum[1] -= px[1];
				sum[2] -= px[2];
				count--;
			}
		}
	}
}

// Ordered-dither offsets, one per position of a 4x4 tile. Added before the 565
// truncation they spread the quantisation error around, turning the backdrop's
// banded gradients into a smooth ramp. Range is +-half of the 5-bit step.
static const int8_t BAYER4[4][4] = {
	{-4, 2, -3, 3},
	{4, -2, 5, -1},
	{-3, 3, -4, 2},
	{5, -1, 4, -2},
};

static inline uint8_t dither_channel(int value, int offset) {
	value += offset;
	if (value < 0)
		value = 0;
	if (value > 255)
		value = 255;
	return (uint8_t)value;
}

// pack_rgb565 with the ordered dither above. Used for the blurred backdrops,
// whose soft gradients are exactly where 16-bit banding shows.
static bool pack_rgb565_dithered(const uint8_t *rgb, int w, int h, cover_image_t *out) {
	uint16_t *buf = malloc((size_t)w * h * sizeof(uint16_t));
	if (!buf)
		return false;

	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const uint8_t *px = rgb + ((size_t)y * w + x) * 3;
			int offset = BAYER4[y & 3][x & 3];
			buf[(size_t)y * w + x] = rgb_to_rgb565(dither_channel(px[0], offset), dither_channel(px[1], offset),
												   dither_channel(px[2], offset));
		}
	}

	memset(out, 0, sizeof(*out));
	out->pixels = (uint8_t *)buf;
	out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	out->dsc.header.w = (uint32_t)w;
	out->dsc.header.h = (uint32_t)h;
	out->dsc.header.stride = (uint32_t)w * 2;
	out->dsc.data_size = (uint32_t)w * h * 2;
	out->dsc.data = out->pixels;
	return true;
}

// Packs an RGB888 buffer into the cover_image_t LVGL draws from. Takes
// ownership of nothing: the source buffer is still the caller's to free.
static bool pack_rgb565(const uint8_t *rgb, int w, int h, cover_image_t *out) {
	uint16_t *buf = malloc((size_t)w * h * sizeof(uint16_t));
	if (!buf)
		return false;

	for (int i = 0; i < w * h; i++) {
		buf[i] = rgb_to_rgb565(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
	}

	memset(out, 0, sizeof(*out));
	out->pixels = (uint8_t *)buf;
	out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	out->dsc.header.w = (uint32_t)w;
	out->dsc.header.h = (uint32_t)h;
	out->dsc.header.stride = (uint32_t)w * 2;
	out->dsc.data_size = (uint32_t)w * h * 2;
	out->dsc.data = out->pixels;
	return true;
}

// Builds the visible picture: COVER_FIT_COVER crops to the box's aspect ratio,
// anything else fits the whole image inside the box.
static bool make_scaled(const raw_image_t *src, int box_w, int box_h, cover_fit_t fit, cover_image_t *out) {
	memset(out, 0, sizeof(*out));
	if (box_w < 1 || box_h < 1)
		return false;

	crop_t crop;
	int dst_w, dst_h;

	if (fit == COVER_FIT_COVER) {
		crop = crop_to_aspect(src->w, src->h, box_w, box_h);
		dst_w = box_w;
		dst_h = box_h;
	} else {
		// Whole picture, scaled down to fit the box, never blown up: a small
		// cover looks better small than stretched.
		crop = (crop_t){0, 0, src->w, src->h};
		dst_w = src->w;
		dst_h = src->h;

		if (dst_w > box_w) {
			dst_h = (int)((int64_t)dst_h * box_w / dst_w);
			dst_w = box_w;
		}
		if (dst_h > box_h) {
			dst_w = (int)((int64_t)dst_w * box_h / dst_h);
			dst_h = box_h;
		}
		if (dst_w < 1)
			dst_w = 1;
		if (dst_h < 1)
			dst_h = 1;
	}

	uint8_t *rgb = resample_rgb(src, &crop, dst_w, dst_h, false);
	if (!rgb)
		return false;

	bool ok = pack_rgb565(rgb, dst_w, dst_h, out);
	free(rgb);
	return ok;
}

// Builds the upside-down, blurred, dimmed copy that sits behind the controls.
// The blur is a shrink, box-blur passes, then an interpolated stretch: visually
// a soft gaussian-like smear for a few hundred kilobytes of work.
static bool make_backdrop(const raw_image_t *src, int box_w, int box_h, bool from_bottom, cover_image_t *out) {
	memset(out, 0, sizeof(*out));
	if (box_w < 1 || box_h < 1)
		return false;

	crop_t crop = crop_to_aspect(src->w, src->h, box_w, box_h);
	if (from_bottom) {
		crop.y = src->h - crop.h; // the band the block will be drawn over
		if (crop.y < 0)
			crop.y = 0;
	}

	int small_w = box_w / BACKDROP_SCALE_DIVISOR;
	int small_h = box_h / BACKDROP_SCALE_DIVISOR;
	if (small_w < 2)
		small_w = 2;
	if (small_h < 2)
		small_h = 2;

	// Flip while shrinking, so the stretch back up only has to interpolate and
	// no separate flip pass is needed. Not for a bottom band: that one stands
	// in for the pixels underneath it and has to face the same way. Nor when
	// the backdrop is the whole screen with the sleeve shown over it, where the
	// two are plainly the same picture and one of them being upside down is the
	// only thing anybody sees.
	bool flip = !from_bottom && !backdrop_upright;
	uint8_t *small = resample_rgb(src, &crop, small_w, small_h, flip);
	if (!small)
		return false;

	uint8_t *scratch = malloc((size_t)small_w * small_h * 3);
	if (scratch) {
		for (int i = 0; i < BACKDROP_BLUR_PASSES; i++) {
			box_blur_pass(small, scratch, small_w, small_h, BACKDROP_BLUR_RADIUS);
		}
		free(scratch);
	}

	raw_image_t tiny = {.pixels = small, .w = small_w, .h = small_h};
	crop_t whole = {0, 0, small_w, small_h};

	uint8_t *rgb = resample_rgb(&tiny, &whole, box_w, box_h, false);
	free(small);
	if (!rgb)
		return false;

	// Push the backdrop away from the text colour: whatever the cover happens to
	// be, the title has to stay readable on top of it.
	for (int i = 0; i < box_w * box_h * 3; i++) {
		if (backdrop_light) {
			rgb[i] = (uint8_t)(255 - ((255 - rgb[i]) * BACKDROP_BRIGHTNESS_PCT) / 100);
		} else {
			rgb[i] = (uint8_t)((rgb[i] * BACKDROP_BRIGHTNESS_PCT) / 100);
		}
	}

	bool ok = pack_rgb565_dithered(rgb, box_w, box_h, out);
	free(rgb);
	return ok;
}

// A blurred, dimmed copy of a picture that has already been scaled and packed.
//
// Not make_backdrop() with different numbers: that one starts from the decoded
// source and is built while the track is being loaded, because it is wanted on
// every track. This is asked for from the sleeve already on screen, at the
// moment the VU meters are opened, and by then the decoded source has long been
// freed.
//
// Same treatment as the backdrop, so the sleeve behind the needles and the
// block behind the controls read as one surface: shrink, box-blur, stretch,
// and the same push away from the text colour.
// The one colour that stands for a sleeve: the pool of light under the record
// in Cover Flow, and what the player's alternative layout tints itself with.
//
// A plain average of a cover comes out mud: every sleeve has a lot of near-grey
// in it and the greys drown the one colour a person would name. So each pixel
// is weighted by how far it is from grey, and the near-greys count for almost
// nothing. What comes back is then pushed towards full saturation, because a
// glow is light and not paint: a washed-out tint reads as a smudge on the
// screen rather than as something lit.
//
// Every fourth pixel each way, which is a sixteenth of the work for a number
// that does not change in the last decimal.
//
// Two things here keep a black-and-white sleeve from coming out green.
//
// The first is how a 565 pixel is opened out. Shifting the five- and six-bit
// fields up by three and two leaves green a step ahead of red and blue on every
// grey there is: white arrives as 248,252,248 and mid grey as 96,100,96 -- four
// units of pure green, which the stretch below takes to full strength.
// Replicating the top bits down into the gap instead makes white 255,255,255
// and leaves the rest off by a unit or two either way, with no colour
// preferred.
//
// The second is the floor. Two units of rounding noise in a grey average is
// still a hue, and stretching it produces a confident colour out of nothing. So
// a sleeve whose average is that close to grey is taken at its word and lights
// the screen white.
#define TONE_CHROMA_FLOOR 14
#define TONE_MONO 0xF2EEE8 // barely warm white: light, not paint

uint32_t cover_dominant_tone(const cover_image_t *cover) {
	int w = (int)cover->dsc.header.w;
	int h = (int)cover->dsc.header.h;
	int stride = (int)cover->dsc.header.stride / 2;
	if (!cover->pixels || w <= 0 || h <= 0) {
		return 0;
	}

	uint64_t ar = 0, ag = 0, ab = 0, weight = 0;
	for (int y = 0; y < h; y += 4) {
		const uint16_t *row = (const uint16_t *)cover->pixels + (size_t)y * stride;
		for (int x = 0; x < w; x += 4) {
			uint16_t c = row[x];
			unsigned r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
			unsigned r = (r5 << 3) | (r5 >> 2), g = (g6 << 2) | (g6 >> 4), b = (b5 << 3) | (b5 >> 2);
			unsigned hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
			unsigned lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
			// Distance from grey, and a little for being bright at all: a sleeve
			// that really is black and white still has to light something.
			unsigned k = (hi - lo) * 4 + hi / 8 + 1;
			ar += (uint64_t)r * k;
			ag += (uint64_t)g * k;
			ab += (uint64_t)b * k;
			weight += k;
		}
	}
	if (!weight) {
		return 0;
	}
	unsigned r = (unsigned)(ar / weight), g = (unsigned)(ag / weight), b = (unsigned)(ab / weight);

	// Pull it away from grey: the gap between the strongest channel and the
	// weakest is stretched, and the middle one moves with it, so the hue stays
	// where it was and only the strength changes.
	unsigned hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
	unsigned lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
	if (hi - lo < TONE_CHROMA_FLOOR) {
		return TONE_MONO; // no colour worth finding: this sleeve is grey
	}
	if (hi > lo) {
		unsigned reach = hi > 200 ? 200 : hi; // a dark average is not stretched to full
		r = lo + (r - lo) * 255 / (hi - lo) * reach / 200;
		g = lo + (g - lo) * 255 / (hi - lo) * reach / 200;
		b = lo + (b - lo) * 255 / (hi - lo) * reach / 200;
	}
	if (r > 255) {
		r = 255;
	}
	if (g > 255) {
		g = 255;
	}
	if (b > 255) {
		b = 255;
	}

	// And lift the whole thing if it is still dark, so a black sleeve throws a
	// faint light of its own colour instead of a shadow. Taken after the
	// stretch above, which has usually already done most of it.
	hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
	if (hi > 0 && hi < 180) {
		r = r * 180 / hi;
		g = g * 180 / hi;
		b = b * 180 / hi;
	}
	return (r << 16) | (g << 8) | b;
}

bool cover_blur_copy(const cover_image_t *src, cover_image_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (!src || !src->pixels) {
		return false;
	}

	int w = (int)src->dsc.header.w;
	int h = (int)src->dsc.header.h;
	int stride = (int)src->dsc.header.stride / 2;
	if (w < 2 || h < 2 || stride < w) {
		return false;
	}

	// Back out to RGB888: the blur and the resampler both work there, and five
	// and six bit channels have nowhere to put the intermediate values.
	uint8_t *rgb = malloc((size_t)w * h * 3);
	if (!rgb) {
		return false;
	}
	for (int y = 0; y < h; y++) {
		const uint16_t *row = (const uint16_t *)src->pixels + (size_t)y * stride;
		uint8_t *drow = rgb + (size_t)y * w * 3;
		for (int x = 0; x < w; x++) {
			uint16_t c = row[x];
			unsigned r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
			drow[x * 3 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
			drow[x * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
			drow[x * 3 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
		}
	}

	raw_image_t whole_img = {.pixels = rgb, .w = w, .h = h};
	crop_t whole = {0, 0, w, h};
	int small_w = w / BACKDROP_SCALE_DIVISOR;
	int small_h = h / BACKDROP_SCALE_DIVISOR;
	if (small_w < 2) {
		small_w = 2;
	}
	if (small_h < 2) {
		small_h = 2;
	}
	uint8_t *small = resample_rgb(&whole_img, &whole, small_w, small_h, false);
	free(rgb);
	if (!small) {
		return false;
	}

	uint8_t *scratch = malloc((size_t)small_w * small_h * 3);
	if (scratch) {
		for (int i = 0; i < BACKDROP_BLUR_PASSES; i++) {
			box_blur_pass(small, scratch, small_w, small_h, BACKDROP_BLUR_RADIUS);
		}
		free(scratch);
	}

	raw_image_t tiny = {.pixels = small, .w = small_w, .h = small_h};
	crop_t tiny_whole = {0, 0, small_w, small_h};
	uint8_t *big = resample_rgb(&tiny, &tiny_whole, w, h, false);
	free(small);
	if (!big) {
		return false;
	}

	for (int i = 0; i < w * h * 3; i++) {
		if (backdrop_light) {
			big[i] = (uint8_t)(255 - ((255 - big[i]) * BACKDROP_BRIGHTNESS_PCT) / 100);
		} else {
			big[i] = (uint8_t)((big[i] * BACKDROP_BRIGHTNESS_PCT) / 100);
		}
	}

	bool ok = pack_rgb565_dithered(big, w, h, out);
	free(big);
	return ok;
}

// ---------------------------------------------------------------------------
// building the finished images
//
// Decoding is the one operation here that can ask for more memory than the
// device has. Everything downstream works on the scaled result and costs a few
// hundred kilobytes at most, so all the risk sits in a single call, which is
// what makes it worth isolating.
// ---------------------------------------------------------------------------

typedef struct {
	int w;
	int h;
	cover_fit_t fit;
	bool backdrop; // build the blurred, flipped copy instead of a plain scale
	// The backdrop comes from the BOTTOM band of the source instead of a
	// centred crop of it, and is not flipped. What the screensaver's
	// full-screen picture wants: there the blurred block sits over the picture
	// itself, so it has to be a blurred continuation of the part it covers --
	// a mirrored centre crop would read as a different photograph.
	bool from_bottom;
} image_request_t;

// Decodes once and produces every requested image from that one decode. The
// expensive part is the JPEG, not the scaling, so a track change asking for
// both the cover and its backdrop still only decodes once.
static bool build_images(const uint8_t *data, size_t size, const image_request_t *reqs, int n, cover_image_t *outs) {
	for (int i = 0; i < n; i++) {
		memset(&outs[i], 0, sizeof(outs[i]));
	}

	// Nobody asked for anything. Without this the box below comes out zero, both
	// scaled decoders are skipped (they need a box to aim at) and the picture
	// goes through the unbounded path at full resolution -- the one thing this
	// file exists to avoid.
	if (n < 1) {
		return false;
	}

	// The largest edge anybody asked for decides how much resolution the decode
	// must keep; everything smaller is resampled from that.
	int max_box = 0;
	for (int i = 0; i < n; i++) {
		if (reqs[i].w > max_box)
			max_box = reqs[i].w;
		if (reqs[i].h > max_box)
			max_box = reqs[i].h;
	}

	raw_image_t raw;
	if (!decode_raw(data, size, max_box, &raw)) {
		return false;
	}

	bool first_ok = false;
	for (int i = 0; i < n; i++) {
		bool ok;
		if (reqs[i].backdrop) {
			ok = make_backdrop(&raw, reqs[i].w, reqs[i].h, reqs[i].from_bottom, &outs[i]);
		} else {
			ok = make_scaled(&raw, reqs[i].w, reqs[i].h, reqs[i].fit, &outs[i]);
		}
		if (i == 0) {
			first_ok = ok;
		}
	}

	raw_free(&raw);
	return first_ok;
}

// ---------------------------------------------------------------------------
// bounded decoding
//
// No helper processes: the decode stays on the worker thread and is refused
// when it does not comfortably fit the memory actually free. The refusal is
// cheap (stbi_info reads only the header), the negative result is cached on disk
// like any other, and a folder whose cover is genuinely too big shows a glyph
// instead of gambling the whole player on an optimistic malloc.
//
// Isolating the decode in a forked child is not an option here. fork() on a
// single 1.2 GHz core copies the page tables of a ~25 MB process while holding
// mmap_sem, stalling every other thread's page faults, and fork() from a
// multithreaded process can catch another thread holding the malloc lock,
// leaving the child deadlocked in its first allocation.
// ---------------------------------------------------------------------------

static bool build_images_safely(const uint8_t *data, size_t size, const image_request_t *reqs, int n,
								cover_image_t *outs) {
	// The budget guard lives inside decode_raw and applies to the stb path only:
	// baseline JPEGs go through the scaled decoder, whose memory use is set by
	// the requested output rather than the source, so a poster-sized cover needs
	// no refusing at all.
	return build_images(data, size, reqs, n, outs);
}

// ---------------------------------------------------------------------------
// Walking the sources
//
// Finding artwork and decoding it are two different ways to fail, and the
// difference matters twice over.
//
// A track can carry a picture the decoder will not take -- a progressive JPEG
// well past the memory budget, an APIC truncated by a bad tagger. Committing to
// the first bytes found would leave that track showing nothing even with a
// perfectly good cover.jpg beside it, so every source is tried in turn until
// one decodes.
//
// And a decode that was refused is not the same answer as "there is no artwork
// here". The first depends on how much memory happened to be free; the second
// is a property of the files. Only the second is worth remembering on disk (see
// the thumbnail cache), which is why the outcome is reported as three states
// rather than a bool.
// ---------------------------------------------------------------------------

typedef enum {
	COVER_BUILD_OK,		// an image was produced
	COVER_BUILD_NONE,	// no source carried a picture
	COVER_BUILD_FAILED, // a picture was found, none of them would decode
} cover_build_t;

static cover_build_t build_from_sources(const char *path, bool is_dir, const image_request_t *reqs, int n,
										cover_image_t *outs) {
	bool had_bytes = false;

	// Noted for the crash handler and cleared on the way out, so a log that
	// still names a file says the fault landed inside the decode rather than
	// merely at the same moment as one -- which is as far as the log line
	// printed below can settle it, since that one comes from this thread while
	// the interface runs on another.
	crumb_set_artwork(path);

	// Zeroed here rather than only inside build_images: a path with no artwork
	// at all never reaches that function, and the caller still reads these.
	for (int i = 0; i < n; i++) {
		memset(&outs[i], 0, sizeof(outs[i]));
	}

	for (int i = 0; i < ALBUMART_CANDIDATES; i++) {
		albumart_t art;
		bool loaded = is_dir ? albumart_load_dir_candidate(path, i, &art) : albumart_load_candidate(path, i, &art);
		if (!loaded) {
			continue;
		}

		had_bytes = true;
		bool ok = build_images_safely(art.data, art.size, reqs, n, outs);
		albumart_free(&art);
		if (ok) {
			crumb_set_artwork(NULL);
			return COVER_BUILD_OK;
		}

		// A build reports on its first image only, so a failure can still have
		// left a backdrop behind. Clear every slot before the next candidate
		// overwrites the pointers.
		for (int j = 0; j < n; j++) {
			cover_free(&outs[j]);
		}
	}

	crumb_set_artwork(NULL);
	return had_bytes ? COVER_BUILD_FAILED : COVER_BUILD_NONE;
}

// ---------------------------------------------------------------------------
// public loaders
// ---------------------------------------------------------------------------

bool cover_load_for_file(const char *filepath, int box_w, int box_h, cover_fit_t fit, cover_image_t *out) {
	memset(out, 0, sizeof(*out));

	image_request_t req = {.w = box_w, .h = box_h, .fit = fit, .backdrop = false};
	return build_from_sources(filepath, false, &req, 1, out) == COVER_BUILD_OK;
}

bool cover_load_for_dir(const char *dirpath, int box_w, int box_h, cover_fit_t fit, cover_image_t *out) {
	memset(out, 0, sizeof(*out));

	image_request_t req = {.w = box_w, .h = box_h, .fit = fit, .backdrop = false};
	return build_from_sources(dirpath, true, &req, 1, out) == COVER_BUILD_OK;
}

// A picture that is simply a file, with no track and no folder behind it.
// AirPlay is the caller: shairport writes the artwork the phone sent to
// /tmp/cover-<md5>.jpg and reports the path, so there is nothing to look up.
bool cover_load_image_file(const char *path, int box_w, int box_h, cover_fit_t fit, cover_image_t *out) {
	memset(out, 0, sizeof(*out));

	if (!path || !path[0])
		return false;

	FILE *f = fopen(path, "rb");
	if (!f)
		return false;

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (size <= 0 || (size_t)size > ALBUMART_MAX_BYTES) {
		fclose(f);
		return false;
	}

	uint8_t *data = malloc((size_t)size);
	if (!data) {
		fclose(f);
		return false;
	}

	size_t got = fread(data, 1, (size_t)size, f);
	fclose(f);

	if (got != (size_t)size) {
		free(data);
		return false;
	}

	image_request_t req = {.w = box_w, .h = box_h, .fit = fit, .backdrop = false};
	bool ok = build_images_safely(data, got, &req, 1, out);
	free(data);
	return ok;
}

// A picture that is already in memory. The EPUB shelf is the caller: a book's
// cover is a member of its ZIP, so there is no file to open -- and writing one
// out just to read it back would put a temporary file on the card for every
// book on the shelf.
bool cover_load_image_memory(const void *data, size_t size, int box_w, int box_h, cover_fit_t fit,
							 cover_image_t *out) {
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (!data || !size || size > ALBUMART_MAX_BYTES) {
		return false;
	}

	image_request_t req = {.w = box_w, .h = box_h, .fit = fit, .backdrop = false};
	return build_images_safely((const uint8_t *)data, size, &req, 1, out);
}

bool cover_load_screensaver_images(const char *path, int w, int h, cover_image_t *image_out, int strip_w,
								  int strip_h, cover_image_t *strip_out) {
	if (image_out)
		memset(image_out, 0, sizeof(*image_out));
	if (strip_out)
		memset(strip_out, 0, sizeof(*strip_out));
	if (!path || !path[0] || !image_out || !strip_out)
		return false;

	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || (size_t)size > ALBUMART_MAX_BYTES) {
		fclose(f);
		return false;
	}
	uint8_t *data = malloc((size_t)size);
	if (!data) {
		fclose(f);
		return false;
	}
	size_t got = fread(data, 1, (size_t)size, f);
	fclose(f);
	if (got != (size_t)size) {
		free(data);
		return false;
	}

	// One decode, two pictures, as on the player screen -- except that the
	// blurred one comes from the bottom of the same photograph rather than from
	// the middle of it.
	image_request_t reqs[2] = {
		{.w = w, .h = h, .fit = COVER_FIT_COVER, .backdrop = false, .from_bottom = false},
		{.w = strip_w, .h = strip_h, .fit = COVER_FIT_COVER, .backdrop = true, .from_bottom = true},
	};
	cover_image_t results[2];
	bool ok = build_images_safely(data, got, reqs, 2, results);
	free(data);

	if (!ok) {
		cover_free(&results[0]);
		cover_free(&results[1]);
		return false;
	}
	*image_out = results[0];
	*strip_out = results[1];
	return true;
}

// The size, with four kilobytes from each end folded in. Not the whole picture:
// hashing four megabytes on this processor would cost more than the question is
// worth, and two different covers that agree on their length and on both ends
// do not happen outside a deliberate attempt.
static uint64_t bytes_id(const uint8_t *data, size_t size) {
	uint64_t h = 1469598103934665603ULL;
	h ^= (uint64_t)size;
	h *= 1099511628211ULL;

	size_t edge = size < 4096 ? size : 4096;
	for (size_t i = 0; i < edge; i++) {
		h ^= data[i];
		h *= 1099511628211ULL;
	}
	for (size_t i = size - edge; i < size; i++) {
		h ^= data[i];
		h *= 1099511628211ULL;
	}
	// Never zero: that is the answer for "no artwork", and a picture that
	// hashed to it would read as one that is not there.
	return h ? h : 1;
}

uint64_t cover_source_id(const char *filepath) {
	if (!filepath || !filepath[0]) {
		return 0;
	}
	// The first source that has bytes, which is the one the decode will start
	// from. It may yet fail to decode and send the build on to the next
	// candidate -- but that outcome follows from these same bytes, so two
	// tracks that agree here agree about the picture that ends up on screen.
	for (int i = 0; i < ALBUMART_CANDIDATES; i++) {
		albumart_t art;
		if (!albumart_load_candidate(filepath, i, &art)) {
			continue;
		}
		uint64_t id = art.size ? bytes_id(art.data, art.size) : 0;
		albumart_free(&art);
		if (id) {
			return id;
		}
	}
	return 0;
}

bool cover_load_player_images(const char *filepath, int cover_w, int cover_h, cover_image_t *cover_out,
							  int backdrop_w, int backdrop_h, cover_image_t *backdrop_out) {
	if (cover_out)
		memset(cover_out, 0, sizeof(*cover_out));
	if (backdrop_out)
		memset(backdrop_out, 0, sizeof(*backdrop_out));

	// One decode feeds both images: the expensive part is the JPEG, not the
	// scaling, so a track change decodes once rather than twice.
	image_request_t reqs[2] = {0}; // a caller may want neither image
	cover_image_t results[2];
	int n = 0;
	int cover_index = -1, backdrop_index = -1;

	if (cover_out) {
		cover_index = n;
		reqs[n++] = (image_request_t){.w = cover_w, .h = cover_h, .fit = COVER_FIT_COVER, .backdrop = false};
	}
	if (backdrop_out) {
		backdrop_index = n;
		reqs[n++] = (image_request_t){.w = backdrop_w, .h = backdrop_h, .fit = COVER_FIT_COVER, .backdrop = true};
	}

	cover_build_t built = build_from_sources(filepath, false, reqs, n, results);
	if (built == COVER_BUILD_NONE) {
		// A podcast episode with no cover file beside it: fetch it now, on the
		// loader thread, which can afford to wait on a network request, then
		// try again. This covers episodes started from the queue, whose tags
		// carry the cover URL but whose jpg was never downloaded.
		if (!podcastcache_ensure_cover(filepath)) {
			return false;
		}
		built = build_from_sources(filepath, false, reqs, n, results);
	}
	bool ok = (built == COVER_BUILD_OK);

	// A missing backdrop is survivable; the controls just stay plain.
	if (cover_index >= 0)
		*cover_out = results[cover_index];
	if (backdrop_index >= 0)
		*backdrop_out = results[backdrop_index];

	return ok;
}

void cover_free(cover_image_t *img) {
	if (!img)
		return;
	free(img->pixels);
	memset(img, 0, sizeof(*img));
}

// ---------------------------------------------------------------------------
// on-disk thumbnail cache
//
// The same mechanism the stock player uses: resized artwork is kept on the card
// so browsing a library never decodes the original twice. What is stored is the
// already-scaled RGB565 thumbnail: a 44x44 entry is under 4 KB and loading one
// is a single read, with no decoder involved. The first pass over a big folder
// decodes, every pass after it reads back a few kilobytes. "No artwork here" is
// stored too, so empty folders stop being rescanned as well.
//
// A database, not a directory of one file per entry: thousands of small files in
// one directory is the worst thing to ask of a FAT filesystem, where listing and
// cleaning up are both slow and every entry costs a directory entry. It is a
// single SQLite file named thumbnails under .local, alongside library.db. The
// key is a hash of path + size + mtime + source size, so replacing a cover
// invalidates its entry by itself; the value is the pixels, or an empty row
// meaning nothing was found there.
// ---------------------------------------------------------------------------

// Only thumbnails are worth keeping on disk. The player's full-size cover is
// decoded once per track change, which is not worth a megabyte of cache.
//
// The album carousel sets the ceiling: it decodes at 210, and a carousel that
// re-decoded every cover on every visit would be the one page that never got
// cheaper the second time. An entry at that size is worth keeping for the few
// hundred albums a card holds, but not for anything larger.
#define THUMB_DISK_MAX_SIZE 216

static sqlite3 *thumb_db;
// The thumbnail worker writes while the GUI thread may be reading, and a card
// change closes and reopens the database under both: one connection, guarded by
// this lock.
static pthread_mutex_t thumb_db_lock = PTHREAD_MUTEX_INITIALIZER;

// Everything the player keeps beside the user's music lives under one hidden
// directory, so a card ends up with a single .local folder rather than one dot
// entry per feature.
#define THUMB_DB_SUBPATH ".local/thumbnails"

// An earlier release kept the cache as a directory at the path the database now
// wants, so on first open it is emptied and removed. Everything inside it was
// written by this player (16 hex-digit names), and the content is rebuilt in the
// database on the first pass over the lists.
static void thumb_wipe_old_dir(const char *dir) {
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *e;
	char victim[600];
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
			continue;
		}
		if (snprintf(victim, sizeof(victim), "%s/%s", dir, e->d_name) < (int)sizeof(victim)) {
			unlink(victim);
		}
	}
	closedir(d);
	rmdir(dir);
	printf("cover: old folder cache removed (%s)\n", dir);
}

// Schema plus pragma. synchronous=OFF because this is only a cache: an entry
// lost to a card pulled mid-write recomputes itself, and every fsync saved is
// smoother scrolling.
static bool thumb_db_prepare_schema(sqlite3 *db) {
	char *err = NULL;
	if (sqlite3_exec(db,
					 "PRAGMA synchronous=OFF;"
					 // SQLite's own default is 2 MB of page cache per
					 // connection, which browsing one big folder fills and
					 // keeps filled for the rest of the session. Every other
					 // database in the player (library, audiobooks,
					 // subscriptions, radio) asks for 64 to 256 kB.
					 "PRAGMA cache_size=-256;"
					 "CREATE TABLE IF NOT EXISTS thumbs("
					 " key TEXT PRIMARY KEY,"
					 " w INTEGER NOT NULL,"
					 " h INTEGER NOT NULL,"
					 " pixels BLOB)",
					 NULL, NULL, &err) == SQLITE_OK) {
		return true;
	}
	fprintf(stderr, "cover: thumbnails db refused: %s\n", err ? err : "?");
	sqlite3_free(err);
	return false;
}

// Opens or creates <base>/.local/thumbnails and adopts it. Call with the lock
// already held.
static bool try_cache_db(const char *base) {
	char parent[512];
	char path[512];

	// A base path long enough to overflow these would leave a truncated name,
	// which is worse than simply having no cache.
	if (snprintf(parent, sizeof(parent), "%s/.local", base) >= (int)sizeof(parent)) {
		return false;
	}
	mkdir(parent, 0755);

	if (snprintf(path, sizeof(path), "%s/%s", base, THUMB_DB_SUBPATH) >= (int)sizeof(path)) {
		return false;
	}

	// The old cache directory occupies the same name as the database file, so
	// clear it out of the way first.
	struct stat st;
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
		thumb_wipe_old_dir(path);
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
			return false; // it would not go away, so no cache here
		}
	}

	sqlite3 *db = NULL;
	if (sqlite3_open(path, &db) != SQLITE_OK) {
		fprintf(stderr, "cover: cannot open %s: %s\n", path, db ? sqlite3_errmsg(db) : "?");
		sqlite3_close(db);
		return false;
	}

	if (!thumb_db_prepare_schema(db)) {
		// A corrupt database, typically a card pulled mid-write, does not heal:
		// delete the file and start clean, since it is only a cache.
		sqlite3_close(db);
		unlink(path);
		db = NULL;
		if (sqlite3_open(path, &db) != SQLITE_OK || !thumb_db_prepare_schema(db)) {
			sqlite3_close(db);
			return false;
		}
	}

	thumb_db = db;
	printf("cover: thumbnail cache in %s\n", path);
	return true;
}

void cover_set_cache_dir(const char *sd_root) {
	pthread_mutex_lock(&thumb_db_lock);

	if (thumb_db) {
		sqlite3_close(thumb_db);
		thumb_db = NULL;
	}

	bool ok = sd_root && sd_root[0] && try_cache_db(sd_root);
	if (!ok) {
		// The card may be missing or read-only; a tmpfs cache still saves the
		// rescan cost within a single run.
		ok = try_cache_db("/tmp");
	}

	pthread_mutex_unlock(&thumb_db_lock);

	if (!ok) {
		printf("cover: thumbnail cache disabled\n");
	}
	fprintf(stderr, "cover: %zu KB free, decode budget %zu KB\n", read_mem_available() / 1024,
			decode_budget_bytes() / 1024);
}

// FNV-1a. The cache key has to change when the source does, so the file's size
// and timestamp go into it: replacing a cover.jpg invalidates the thumbnail
// without anybody having to clear the cache.
static uint64_t thumb_hash(const char *path, int box, bool is_dir) {
	struct stat st;
	uint64_t h = 1469598103934665603ULL;

	char material[700];
	if (stat(path, &st) == 0) {
		snprintf(material, sizeof(material), "%s|%d|%c|%lld|%lld", path, box, is_dir ? 'd' : 'f',
				 (long long)st.st_mtime, (long long)st.st_size);
	} else {
		snprintf(material, sizeof(material), "%s|%d|%c|?", path, box, is_dir ? 'd' : 'f');
	}

	for (const char *p = material; *p; p++) {
		h ^= (uint8_t)*p;
		h *= 1099511628211ULL;
	}
	return h;
}

// Builds the cache key. False means this size is not worth caching. Whether the
// database is open at all is decided under the lock, in load and store.
static bool thumb_key(char *out, size_t out_size, const char *path, int box, bool is_dir) {
	if (box > THUMB_DISK_MAX_SIZE) {
		return false;
	}
	snprintf(out, out_size, "%016llx", (unsigned long long)thumb_hash(path, box, is_dir));
	return true;
}

// Reads a cached thumbnail. Returns false on a miss; on a hit, has_image tells
// whether there was artwork at all.
static bool thumb_db_load(const char *key, cover_image_t *out, bool *has_image) {
	bool hit = false;

	pthread_mutex_lock(&thumb_db_lock);
	if (!thumb_db) {
		pthread_mutex_unlock(&thumb_db_lock);
		return false;
	}

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(thumb_db, "SELECT w, h, pixels FROM thumbs WHERE key = ?1", -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		if (sqlite3_step(stmt) == SQLITE_ROW) {
			int w = sqlite3_column_int(stmt, 0);
			int h = sqlite3_column_int(stmt, 1);

			if (w == 0 || h == 0) {
				*has_image = false; // cached "no artwork here"
				hit = true;
			} else if (w > 0 && h > 0 && w <= 0xFFFF && h <= 0xFFFF) {
				size_t bytes = (size_t)w * h * 2;
				const void *blob = sqlite3_column_blob(stmt, 2);
				// A row whose blob size does not match its dimensions comes from an
				// interrupted write and counts as a miss: decode again and
				// overwrite it.
				if (blob && (size_t)sqlite3_column_bytes(stmt, 2) == bytes) {
					uint8_t *pixels = malloc(bytes);
					if (pixels) {
						memcpy(pixels, blob, bytes);
						memset(out, 0, sizeof(*out));
						out->pixels = pixels;
						out->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
						out->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
						out->dsc.header.w = (uint32_t)w;
						out->dsc.header.h = (uint32_t)h;
						out->dsc.header.stride = (uint32_t)w * 2;
						out->dsc.data_size = (uint32_t)bytes;
						out->dsc.data = pixels;
						*has_image = true;
						hit = true;
					}
				}
			}
		}
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&thumb_db_lock);
	return hit;
}

// A single write, made atomic by SQLite: no more .tmp file and rename, since
// the database journal does the same job.
static void thumb_db_store(const char *key, const cover_image_t *img, bool has_image) {
	pthread_mutex_lock(&thumb_db_lock);
	if (!thumb_db) {
		pthread_mutex_unlock(&thumb_db_lock);
		return;
	}

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(thumb_db, "INSERT OR REPLACE INTO thumbs(key, w, h, pixels) VALUES(?1, ?2, ?3, ?4)", -1,
						   &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
		if (has_image && img->pixels) {
			int w = (int)img->dsc.header.w;
			int h = (int)img->dsc.header.h;
			sqlite3_bind_int(stmt, 2, w);
			sqlite3_bind_int(stmt, 3, h);
			sqlite3_bind_blob(stmt, 4, img->pixels, (int)((size_t)w * h * 2), SQLITE_STATIC);
		} else {
			sqlite3_bind_int(stmt, 2, 0);
			sqlite3_bind_int(stmt, 3, 0);
			sqlite3_bind_null(stmt, 4);
		}
		sqlite3_step(stmt); // a failure here only costs a future re-decode
		sqlite3_finalize(stmt);
	}

	pthread_mutex_unlock(&thumb_db_lock);
}

// ---------------------------------------------------------------------------
// browser thumbnails
// ---------------------------------------------------------------------------

static cover_build_t thumb_load_uncached(const char *path, bool is_dir, int size, cover_image_t *out) {
	image_request_t req = {.w = size, .h = size, .fit = COVER_FIT_CONTAIN, .backdrop = false};
	return build_from_sources(path, is_dir, &req, 1, out);
}

bool cover_thumb_cached(const char *path, int size, cover_image_t *out) {
	memset(out, 0, sizeof(*out));

	char key[32];
	if (!thumb_key(key, sizeof(key), path, size, false)) {
		return false;
	}

	bool has_image = false;
	if (thumb_db_load(key, out, &has_image)) {
		return has_image;
	}
	return false;
}

bool cover_thumb_load(const char *path, bool is_dir, int size, cover_image_t *out) {
	memset(out, 0, sizeof(*out));

	// A track answers for itself: its embedded art, or a cover file next to it.
	// Asking the folder first -- the "an album shares one picture" shortcut --
	// breaks as soon as a folder holds tracks from different albums, since every
	// row below the first would show the first track's cover. The per-file disk
	// cache keeps the cost at one decode per file, ever.

	char key[32];
	bool cacheable = thumb_key(key, sizeof(key), path, size, is_dir);

	// Disk first: a hit costs one small read instead of a decode, which is what
	// makes scrolling back over a folder cost nothing.
	if (cacheable) {
		bool has_image = false;
		if (thumb_db_load(key, out, &has_image)) {
			return has_image;
		}
	}

	cover_build_t built = thumb_load_uncached(path, is_dir, size, out);

	// "There is no artwork here" is worth storing too: a folder without any is
	// otherwise rescanned from scratch every time it scrolls past.
	//
	// A refused or failed decode is not stored. The decode budget is a quarter
	// of the memory free at that instant, so the same file can be refused now
	// and decode fine a minute later -- and the key is built from the file's own
	// size and mtime, so nothing would ever invalidate the entry. Storing a
	// failure would leave that track without a cover for good.
	if (cacheable && built != COVER_BUILD_FAILED) {
		thumb_db_store(key, out, built == COVER_BUILD_OK);
	}

	return built == COVER_BUILD_OK;
}
