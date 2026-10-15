#include "bootlogo.h"

#include "src/system/core/utils.h"
#include "src/system/image/art_shrink.h"
#include "src/system/image/jpeg_planes.h"
#include "src/system/image/jpeg_scaled.h"
#include "src/system/image/png_scaled.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// See bootlogo.h for the marker and where it is read.
#define MTD_DEVICE "/dev/mtd5"
#define MARKER_OFFSET "0x20000"
#define MARKER_LENGTH 13 // "theme:T,aA,cC"
#define ACCENT_COUNT 6

#define MARKER_READ_FILE "/tmp/.bootlogo_marker"
#define MARKER_WRITE_FILE "/tmp/.bootlogo_marker_new"
#define CUSTOM_TMP_FILE BOOTLOGO_CUSTOM_FILE ".tmp"

// Pictures larger than this are refused rather than read into memory.
#define SOURCE_MAX_BYTES (32L * 1024 * 1024)

// The most a progressive JPEG's plane decode may take, as for cover art.
#define PLANES_BUDGET_BYTES (12u * 1024u * 1024u)

// Above 90 the encoder keeps the colour at full resolution: logos are mostly
// sharp coloured edges.
#define CUSTOM_JPEG_QUALITY 92

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool worker_running;
static bool have_state; // false until the theme has said what it is
static bool want_dark;
static int want_accent;
static unsigned generation; // bumped by every request, so the worker sees a new one

bool bootlogo_has_custom(void) { return access(BOOTLOGO_CUSTOM_FILE, R_OK) == 0; }

// The marker on the flash now, NUL-terminated in `out` (MARKER_LENGTH + 1
// bytes). An erased page reads back as 0xFF bytes, which never match a marker.
static bool read_marker(char *out) {
	memset(out, 0, MARKER_LENGTH + 1);

	// nanddump wants somewhere to put its bytes and will not simply hand them
	// back.
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "nanddump -q -s %s -l %d %s -a > %s 2>/dev/null", MARKER_OFFSET, MARKER_LENGTH,
			 MTD_DEVICE, MARKER_READ_FILE);
	if (system(cmd) != 0) {
		return false;
	}

	FILE *f = fopen(MARKER_READ_FILE, "rb");
	if (!f) {
		return false;
	}
	size_t got = fread(out, 1, MARKER_LENGTH, f);
	fclose(f);
	remove(MARKER_READ_FILE);
	out[got] = '\0';
	return true;
}

// Erases the block and writes `marker` into its first page; nandwrite -p pads
// the page with 0xFF.
static bool write_marker(const char *marker) {
	FILE *f = fopen(MARKER_WRITE_FILE, "wb");
	if (!f) {
		return false;
	}
	bool written = fwrite(marker, 1, strlen(marker), f) == strlen(marker);
	if (fclose(f) != 0 || !written) {
		remove(MARKER_WRITE_FILE);
		return false;
	}

	char cmd[256];
	snprintf(cmd, sizeof(cmd), "flash_erase -q %s %s 1", MTD_DEVICE, MARKER_OFFSET);
	if (system(cmd) != 0) {
		fprintf(stderr, "bootlogo: flash_erase failed\n");
		remove(MARKER_WRITE_FILE);
		return false;
	}
	snprintf(cmd, sizeof(cmd), "nandwrite -q -s %s -p %s %s", MARKER_OFFSET, MTD_DEVICE, MARKER_WRITE_FILE);
	bool ok = system(cmd) == 0;
	remove(MARKER_WRITE_FILE);
	if (!ok) {
		fprintf(stderr, "bootlogo: nandwrite failed\n");
	}
	return ok;
}

static void *worker_main(void *arg) {
	(void)arg;
	thread_be_background("boot logo");

	for (;;) {
		pthread_mutex_lock(&lock);
		unsigned seen = generation;
		bool dark = want_dark;
		int accent = want_accent;
		pthread_mutex_unlock(&lock);

		char wanted[MARKER_LENGTH + 1];
		snprintf(wanted, sizeof(wanted), "theme:%d,a%d,c%d", dark ? 2 : 1, accent, bootlogo_has_custom() ? 1 : 0);

		char current[MARKER_LENGTH + 1];
		if (!read_marker(current) || strcmp(current, wanted) != 0) {
			if (write_marker(wanted)) {
				printf("bootlogo: %s written to %s\n", wanted, MTD_DEVICE);
			}
		}

		pthread_mutex_lock(&lock);
		if (generation == seen) {
			worker_running = false;
			pthread_mutex_unlock(&lock);
			return NULL;
		}
		pthread_mutex_unlock(&lock);
	}
}

// Starts the worker, or leaves the running one to pick the request up.
static void kick(void) {
	if (access(MTD_DEVICE, W_OK) != 0) {
		return;
	}

	pthread_mutex_lock(&lock);
	if (!have_state) {
		pthread_mutex_unlock(&lock);
		return;
	}
	generation++;
	if (worker_running) {
		pthread_mutex_unlock(&lock);
		return;
	}
	worker_running = true;
	pthread_mutex_unlock(&lock);

	pthread_t thread;
	if (pthread_create(&thread, NULL, worker_main, NULL) != 0) {
		pthread_mutex_lock(&lock);
		worker_running = false;
		pthread_mutex_unlock(&lock);
		return;
	}
	pthread_detach(thread);
}

void bootlogo_follow(bool dark, int accent) {
	if (accent < 0 || accent >= ACCENT_COUNT) {
		accent = 0;
	}
	pthread_mutex_lock(&lock);
	have_state = true;
	want_dark = dark;
	want_accent = accent;
	pthread_mutex_unlock(&lock);
	kick();
}

void bootlogo_clear_custom(void) {
	if (remove(BOOTLOGO_CUSTOM_FILE) == 0) {
		kick();
	}
}

// The whole file in memory, or NULL.
static uint8_t *read_source(const char *path, size_t *size_out) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	uint8_t *data = NULL;
	if (fseek(f, 0, SEEK_END) == 0) {
		long size = ftell(f);
		if (size > 16 && size <= SOURCE_MAX_BYTES && fseek(f, 0, SEEK_SET) == 0) {
			data = malloc((size_t)size);
			if (data && fread(data, 1, (size_t)size, f) == (size_t)size) {
				*size_out = (size_t)size;
			} else {
				free(data);
				data = NULL;
			}
		}
	}
	fclose(f);
	return data;
}

// RGB888 at a size no smaller than the decoders can manage for `box`, the
// longest edge of the logo.
static uint8_t *decode_source(const uint8_t *data, size_t size, int box, int *w, int *h) {
	if (data[0] == 0xFF && data[1] == 0xD8) {
		uint8_t *rgb = jpeg_scaled_decode(data, size, box / 2, w, h, NULL, NULL);
		if (!rgb) {
			rgb = jpeg_planes_decode(data, size, box * 2, PLANES_BUDGET_BYTES, w, h, NULL);
		}
		return rgb;
	}
	if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
		return png_scaled_decode(data, size, box, w, h);
	}
	return NULL;
}

// `src` scaled to cover dw x dh and cropped to it around the centre. One scale
// for both axes: each output pixel averages the source pixels it covers when
// shrinking, and is interpolated between the nearest four when enlarging.
static uint8_t *cover_resample(const uint8_t *src, int sw, int sh, int dw, int dh) {
	uint8_t *dst = malloc((size_t)dw * (size_t)dh * 3);
	if (!dst) {
		return NULL;
	}

	double scale = (double)dw / sw > (double)dh / sh ? (double)dw / sw : (double)dh / sh;
	double step = 1.0 / scale; // source pixels per output pixel
	double left = (sw - dw * step) / 2.0;
	double top = (sh - dh * step) / 2.0;

	for (int y = 0; y < dh; y++) {
		uint8_t *out = dst + (size_t)y * dw * 3;
		for (int x = 0; x < dw; x++, out += 3) {
			if (step >= 1.0) {
				int x0 = (int)(left + x * step);
				int x1 = (int)(left + (x + 1) * step);
				int y0 = (int)(top + y * step);
				int y1 = (int)(top + (y + 1) * step);
				x1 = x1 > x0 ? x1 : x0 + 1;
				y1 = y1 > y0 ? y1 : y0 + 1;
				x1 = x1 < sw ? x1 : sw;
				y1 = y1 < sh ? y1 : sh;
				uint32_t sum[3] = {0, 0, 0};
				uint32_t n = 0;
				for (int sy = y0; sy < y1; sy++) {
					const uint8_t *p = src + ((size_t)sy * sw + x0) * 3;
					for (int sx = x0; sx < x1; sx++, p += 3) {
						sum[0] += p[0];
						sum[1] += p[1];
						sum[2] += p[2];
						n++;
					}
				}
				n = n ? n : 1;
				out[0] = (uint8_t)(sum[0] / n);
				out[1] = (uint8_t)(sum[1] / n);
				out[2] = (uint8_t)(sum[2] / n);
			} else {
				double fx = left + (x + 0.5) * step - 0.5;
				double fy = top + (y + 0.5) * step - 0.5;
				fx = fx < 0 ? 0 : (fx > sw - 1 ? sw - 1 : fx);
				fy = fy < 0 ? 0 : (fy > sh - 1 ? sh - 1 : fy);
				int ix = (int)fx;
				int iy = (int)fy;
				int nx = ix + 1 < sw ? ix + 1 : ix;
				int ny = iy + 1 < sh ? iy + 1 : iy;
				double ax = fx - ix;
				double ay = fy - iy;
				const uint8_t *p00 = src + ((size_t)iy * sw + ix) * 3;
				const uint8_t *p10 = src + ((size_t)iy * sw + nx) * 3;
				const uint8_t *p01 = src + ((size_t)ny * sw + ix) * 3;
				const uint8_t *p11 = src + ((size_t)ny * sw + nx) * 3;
				for (int c = 0; c < 3; c++) {
					double top_row = p00[c] + (p10[c] - p00[c]) * ax;
					double bottom_row = p01[c] + (p11[c] - p01[c]) * ax;
					out[c] = (uint8_t)(top_row + (bottom_row - top_row) * ay + 0.5);
				}
			}
		}
	}
	return dst;
}

// `len` bytes to BOOTLOGO_CUSTOM_FILE through a temporary file, so a write
// cut short never leaves half a picture under the real name.
static bool save_custom(const uint8_t *jpeg, size_t len) {
	FILE *f = fopen(CUSTOM_TMP_FILE, "wb");
	if (!f) {
		return false;
	}
	bool ok = fwrite(jpeg, 1, len, f) == len;
	ok = fflush(f) == 0 && ok;
	ok = fsync(fileno(f)) == 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(CUSTOM_TMP_FILE, BOOTLOGO_CUSTOM_FILE) != 0) {
		remove(CUSTOM_TMP_FILE);
		return false;
	}
	return true;
}

bool bootlogo_make_custom(const char *image_path, int width, int height) {
	if (!image_path || width <= 0 || height <= 0) {
		return false;
	}

	size_t size = 0;
	uint8_t *data = read_source(image_path, &size);
	if (!data) {
		fprintf(stderr, "bootlogo: cannot read %s\n", image_path);
		return false;
	}

	int sw = 0, sh = 0;
	uint8_t *rgb = decode_source(data, size, width > height ? width : height, &sw, &sh);
	free(data);
	if (!rgb || sw <= 0 || sh <= 0) {
		free(rgb);
		fprintf(stderr, "bootlogo: %s is not a picture that can be decoded\n", image_path);
		return false;
	}

	uint8_t *logo = cover_resample(rgb, sw, sh, width, height);
	free(rgb);
	if (!logo) {
		return false;
	}

	size_t len = 0;
	uint8_t *jpeg = art_encode_jpeg(logo, width, height, CUSTOM_JPEG_QUALITY, &len);
	free(logo);
	if (!jpeg) {
		return false;
	}

	bool ok = save_custom(jpeg, len);
	free(jpeg);
	if (!ok) {
		fprintf(stderr, "bootlogo: cannot write %s\n", BOOTLOGO_CUSTOM_FILE);
		return false;
	}

	printf("bootlogo: %s (%dx%d) saved as the boot logo, %zu bytes\n", image_path, sw, sh, len);
	kick();
	return true;
}
