/*
 * The single translation unit that compiles stb_image. Only JPEG, PNG and GIF
 * are built in: the first two are the formats used for embedded cover art and
 * for cover.jpg/folder.png files, GIF is what many radio stations' icons are,
 * and leaving the rest out keeps the target binary meaningfully smaller.
 *
 * jpeg_planes_decode() lives here too, because it is the one caller that needs
 * stb's internals: it takes the decoded Y/Cb/Cr planes before stb interleaves
 * them and averages them straight down to the size the interface asked for. See
 * jpeg_planes.h for why.
 */

#define STB_IMAGE_IMPLEMENTATION

#include "src/system/image/stb_image_decl.h"

#include "src/system/image/jpeg_planes.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// The frame header alone: size, component count, subsampling and whether the
// file is progressive, without allocating a single plane. stb's STBI__SCAN_header
// pass stops right before the allocations, which is what a memory budget needs
// to decide with.
typedef struct {
	int w;
	int h;
	int comps;
	bool progressive;
	size_t plane_bytes; // all component planes together, MCU padding included
} jpeg_shape_t;

static bool jpeg_read_shape(const uint8_t *data, size_t size, jpeg_shape_t *out) {
	stbi__jpeg *j = (stbi__jpeg *)malloc(sizeof(*j));
	if (!j) {
		return false;
	}

	stbi__context s;
	stbi__start_mem(&s, data, (int)size);
	j->s = &s;
	stbi__setup_jpeg(j);

	bool ok = stbi__decode_jpeg_header(j, STBI__SCAN_header) != 0;
	if (ok) {
		out->w = s.img_x;
		out->h = s.img_y;
		out->comps = s.img_n;
		out->progressive = j->progressive != 0;

		// stb's plane geometry, worked out the same way process_frame_header
		// does it: every component is padded out to whole MCUs, so a 1200x1200
		// 4:2:0 file carries 1200x1200 + 2 * 600x600 samples, not 3 * 1200x1200.
		int h_max = 1, v_max = 1;
		for (int i = 0; i < s.img_n; i++) {
			if (j->img_comp[i].h > h_max) {
				h_max = j->img_comp[i].h;
			}
			if (j->img_comp[i].v > v_max) {
				v_max = j->img_comp[i].v;
			}
		}
		int mcu_x = (s.img_x + h_max * 8 - 1) / (h_max * 8);
		int mcu_y = (s.img_y + v_max * 8 - 1) / (v_max * 8);

		out->plane_bytes = 0;
		for (int i = 0; i < s.img_n; i++) {
			out->plane_bytes += (size_t)(mcu_x * j->img_comp[i].h * 8) * (size_t)(mcu_y * j->img_comp[i].v * 8);
		}
		ok = out->w > 0 && out->h > 0 && out->plane_bytes > 0;
	}

	free(j);
	return ok;
}

// What the decode will cost at its worst moment, for a given `keep`.
//
// Two moments compete. While the scans are read a progressive file holds the
// coefficient store alongside the sample planes; afterwards what remains is the
// sample planes plus the output. The larger of the two has to fit.
//
// `keep` is how many coefficients per row and column of each 8x8 block are kept
// (see stb_image.h). At 8 that is upstream: 64 shorts a block, and sample planes
// at full resolution. Below 8 the store is keep*keep shorts plus eight bytes of
// non-zero flags, and the planes shrink to keep/8 in each direction -- so
// halving `keep` cuts the whole thing by roughly four.
static size_t jpeg_peak_bytes(const jpeg_shape_t *shape, size_t out_bytes, int keep) {
	size_t during, after, planes;

	if (!shape->progressive || keep >= 8) {
		planes = shape->plane_bytes;
		during = shape->progressive ? planes * 3 : planes;
	} else {
		size_t blocks = shape->plane_bytes / 64; // one 8x8 block per 64 samples
		size_t store = blocks * ((size_t)keep * keep * sizeof(short) + 2 * sizeof(uint32_t));
		planes = shape->plane_bytes * (size_t)keep * (size_t)keep / 64;
		during = store + planes;
	}

	after = planes + out_bytes;

	// Tables, the Huffman LUTs and stb's line buffers: small next to the planes,
	// but not nothing.
	return (during > after ? during : after) + 64 * 1024;
}

// Averages one component plane down to dst_w x dst_h. The plane is stored at
// `w2` stride with `cx` x `cy` real samples in it; the rest is MCU padding and
// must not be sampled.
static void jpeg_planes_shrink(const stbi_uc *src, int stride, int cx, int cy, stbi_uc *dst, int dst_w, int dst_h) {
	for (int y = 0; y < dst_h; y++) {
		int sy0 = (int)((int64_t)y * cy / dst_h);
		int sy1 = (int)((int64_t)(y + 1) * cy / dst_h);
		if (sy1 <= sy0) {
			sy1 = sy0 + 1;
		}
		if (sy1 > cy) {
			sy1 = cy;
		}

		for (int x = 0; x < dst_w; x++) {
			int sx0 = (int)((int64_t)x * cx / dst_w);
			int sx1 = (int)((int64_t)(x + 1) * cx / dst_w);
			if (sx1 <= sx0) {
				sx1 = sx0 + 1;
			}
			if (sx1 > cx) {
				sx1 = cx;
			}

			unsigned sum = 0, n = 0;
			for (int sy = sy0; sy < sy1; sy++) {
				const stbi_uc *row = src + (size_t)sy * stride;
				for (int sx = sx0; sx < sx1; sx++) {
					sum += row[sx];
					n++;
				}
			}
			dst[(size_t)y * dst_w + x] = n ? (stbi_uc)(sum / n) : 0;
		}
	}
}

static stbi_uc clamp_byte(int v) { return (stbi_uc)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

uint8_t *jpeg_planes_decode(const uint8_t *data, size_t size, int box, size_t budget_bytes, int *out_w, int *out_h,
							size_t *peak_bytes) {
	if (peak_bytes) {
		*peak_bytes = 0;
	}
	if (!data || size == 0 || box < 1 || !out_w || !out_h) {
		return NULL;
	}

	jpeg_shape_t shape;
	if (!jpeg_read_shape(data, size, &shape)) {
		return NULL;
	}
	// Four-component (CMYK/YCCK) files need Adobe's inverted transform; they do
	// not turn up as cover art and the ordinary loader can have them.
	if (shape.comps != 1 && shape.comps != 3) {
		return NULL;
	}

	// The best resolution the budget will take. Full first; then, on a
	// progressive file, the reduced decodes -- half, quarter, eighth -- each
	// about a quarter of the memory of the one before it.
	//
	// `box` already carries the 2x margin the caller leaves for its own
	// resampler, so the size the picture will really be shown at is half of it --
	// or the file's own size, when the file is the smaller of the two. A
	// reduction is allowed down to half of that again, which is a 2x blow-up at
	// worst: soft, but a cover. Below it the picture stops being one.
	// The floor comes from the smaller of `shown` and the file's own longest
	// edge, so a file smaller than the box is not held to a resolution it never
	// had.
	int shown = box / 2;
	int natural = shape.w > shape.h ? shape.w : shape.h;
	if (natural < shown) {
		shown = natural;
	}
	int floor_edge = shown / 2;

	static const int KEEP_STEPS[] = {8, 4, 2, 1};
	int keep = 8, dst_w = 0, dst_h = 0;
	size_t peak = 0;
	bool fits = false;

	for (size_t step = 0; step < sizeof(KEEP_STEPS) / sizeof(KEEP_STEPS[0]); step++) {
		int try_keep = KEEP_STEPS[step];
		int src_w = shape.w * try_keep / 8;
		int src_h = shape.h * try_keep / 8;

		if (try_keep < 8) {
			if (!shape.progressive) {
				break; // only a progressive file has a coefficient plane to shrink
			}
			if (src_w < 1 || src_h < 1 || (src_w < floor_edge && src_h < floor_edge)) {
				break;
			}
		}

		int w = src_w, h = src_h;
		if (w > box || h > box) {
			if (src_w >= src_h) {
				w = box;
				h = (int)((int64_t)src_h * box / src_w);
			} else {
				h = box;
				w = (int)((int64_t)src_w * box / src_h);
			}
		}
		if (w < 1) {
			w = 1;
		}
		if (h < 1) {
			h = 1;
		}

		keep = try_keep;
		dst_w = w;
		dst_h = h;
		peak = jpeg_peak_bytes(&shape, (size_t)w * h * 3, try_keep);
		if (budget_bytes == 0 || peak <= budget_bytes) {
			fits = true;
			break;
		}
	}

	size_t out_bytes = (size_t)dst_w * (size_t)dst_h * 3;
	if (peak_bytes) {
		*peak_bytes = peak;
	}
	if (budget_bytes > 0 && !fits) {
		return NULL; // refused before a single plane was allocated
	}
	if (dst_w < 1 || dst_h < 1) {
		return NULL;
	}

	stbi__jpeg *j = (stbi__jpeg *)malloc(sizeof(*j));
	if (!j) {
		return NULL;
	}

	stbi__context s;
	stbi__start_mem(&s, data, (int)size);
	j->s = &s;
	stbi__setup_jpeg(j);
	j->scale_keep = keep;

	if (!stbi__decode_jpeg_image(j)) {
		stbi__free_jpeg_components(j, shape.comps, 0);
		free(j);
		return NULL;
	}

	// One shrunk plane per component, before the colour conversion rather than
	// after: on a 4:2:0 file the chroma planes are already a quarter of the size,
	// and nothing is ever held at full resolution in RGB.
	stbi_uc *small[3] = {NULL, NULL, NULL};
	bool ok = true;
	for (int i = 0; i < shape.comps && ok; i++) {
		small[i] = (stbi_uc *)malloc((size_t)dst_w * dst_h);
		if (!small[i]) {
			ok = false;
			break;
		}
		// The component's valid extent, at the scale the decode actually ran at.
		// img_comp[].x and .y are full-resolution by design (see stb_image.h).
		int cx = (j->img_comp[i].x * keep + 7) / 8;
		int cy = (j->img_comp[i].y * keep + 7) / 8;
		if (cx < 1) {
			cx = 1;
		}
		if (cy < 1) {
			cy = 1;
		}
		jpeg_planes_shrink(j->img_comp[i].data, j->img_comp[i].w2, cx, cy, small[i], dst_w, dst_h);
	}

	stbi__free_jpeg_components(j, shape.comps, 0);
	free(j);

	uint8_t *rgb = ok ? (uint8_t *)malloc(out_bytes) : NULL;
	if (rgb) {
		for (size_t p = 0; p < (size_t)dst_w * dst_h; p++) {
			if (shape.comps == 1) {
				rgb[p * 3] = rgb[p * 3 + 1] = rgb[p * 3 + 2] = small[0][p];
				continue;
			}
			// JFIF YCbCr, in the same fixed-point form stb uses for its own
			// colour conversion.
			int y = small[0][p];
			int cb = small[1][p] - 128;
			int cr = small[2][p] - 128;
			rgb[p * 3] = clamp_byte(y + ((91881 * cr) >> 16));
			rgb[p * 3 + 1] = clamp_byte(y - ((22554 * cb + 46802 * cr) >> 16));
			rgb[p * 3 + 2] = clamp_byte(y + ((116130 * cb) >> 16));
		}
	}

	for (int i = 0; i < 3; i++) {
		free(small[i]);
	}

	if (!rgb) {
		return NULL;
	}
	*out_w = dst_w;
	*out_h = dst_h;
	return rgb;
}
