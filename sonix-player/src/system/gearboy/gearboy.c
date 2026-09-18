#include "gearboy.h"

// The core sits behind the Makefile's GB switch. Without it this file is only
// its stub shell: the page is absent (see morepage.c) and nobody calls in, but
// main.c still compiles unchanged.
#ifndef GB_CORE
#define GB_CORE 0
#endif

#if !GB_CORE

#include <stddef.h>

bool gearboy_start(const char *rom_path, const char *title) {
	(void)rom_path;
	(void)title;
	return false;
}
void gearboy_stop(void) {}
bool gearboy_running(void) { return false; }
bool gearboy_failed(void) { return true; }
const char *gearboy_title(void) { return ""; }
const uint16_t *gearboy_screen(void) { return NULL; }
bool gearboy_present(void) { return false; }
uint32_t gearboy_frame_count(void) { return 0; }
void gearboy_set_keys(uint16_t mask) { (void)mask; }
void gearboy_set_paused(bool paused) { (void)paused; }
bool gearboy_paused(void) { return false; }
void gearboy_request_savestate(void) {}
void gearboy_request_loadstate(void) {}
int gearboy_take_state_result(void) { return 0; }
void gearboy_refresh_video_settings(void) {}

#else

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

#include "src/gb/gbcore.h"
#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/gearboy/gbdb.h"

// APU rate. 44100 is what this DAC plays without any resampling, and what
// Gearboy is tuned for.
#define SAMPLE_RATE 44100

// ALSA buffer depth. 46 ms is both the emulator's clock (see gearboy.h) and the
// delay between finger and sound: shorter turns every system hiccup into a
// dropout, longer makes the game feel late.
#define AUDIO_BUFFER_MS 46

// Audio frames written per iteration while paused. 738 per channel is what one
// real frame produces at 44100 Hz, so the paused loop ticks at the same rate as
// the running one and resuming finds the buffer neither empty nor full.
#define PAUSE_FRAMES 738

// The only savestate slot the menu offers. See gbcore.h for why the number
// exists at all.
#define STATE_SLOT 1

static pthread_t worker;
static bool thread_live;
static volatile bool stop_flag;
static volatile bool running_flag;
static volatile bool failed_flag;

// ---------------------------------------------------------------------------
// Why the panel's double buffer is not enough on its own
//
// The panel swaps two pages with FBIOPAN_DISPLAY, so no half-drawn page is ever
// scanned out. That does not help if the tear is already in the image: writing
// the 3x upscale from the emulator thread while the UI thread reads it seams the
// picture, because the reader is much the slower of the two -- those 414 KB go
// into the mapped framebuffer, which is uncached on this SoC.
//
// So the two jobs are split:
//
//   * the core writes the small 160x144 frame, alternating between two buffers;
//   * the UI thread does the 3x upscale just before invalidating, reading the
//     buffer the core has just finished.
//
// The large buffer then has one writer and one reader and they are the same
// thread, so a seam is impossible by construction, and the core never waits on
// anyone.
//
// The two small buffers are guarded by a sequence counter rather than a lock:
// the reader samples the count before and after and retries if the core has
// caught up with it. Nobody ever blocks.
// ---------------------------------------------------------------------------

#define GB_FRAME_BUFFERS 2

static uint16_t *screen;					// 480x432, the upscaled image
static uint16_t *gb_frames[GB_FRAME_BUFFERS]; // 160x144, as they leave the core
static int16_t *audio_buf;					// one frame's samples

// Which of the two the core is writing, and which one it has finished.
// `frame_count` doubles as the sequence number.
static volatile int gb_write_index;
static volatile int gb_ready_index = -1;

static volatile uint32_t frame_count;
static volatile uint16_t key_mask;
static volatile bool paused_flag;

// Requests from the in-game menu and their outcome. Plain bools and an int
// written by one thread and read by the other: neither can observe a half
// value, and a request cannot be lost because the requester waits for the
// result.
static volatile bool want_savestate;
static volatile bool want_loadstate;
static volatile int state_result;

static char rom_path[512];
static char title[GEARBOY_TITLE_MAX];

// ---------------------------------------------------------------------------
// video settings
// ---------------------------------------------------------------------------

// The five palettes for original Game Boy titles, lightest to darkest. The
// first is the DMG pea green; the rest are the usual Gearboy alternatives.
static const uint8_t GB_PALETTES[5][4][3] = {
	// Original (DMG)
	{{0x9B, 0xBC, 0x0F}, {0x8B, 0xAC, 0x0F}, {0x30, 0x62, 0x30}, {0x0F, 0x38, 0x0F}},
	// Sharp
	{{0xF5, 0xFA, 0xEF}, {0x86, 0xC2, 0x70}, {0x2F, 0x69, 0x57}, {0x0B, 0x19, 0x20}},
	// Black and white
	{{0xFF, 0xFF, 0xFF}, {0xB6, 0xB6, 0xB6}, {0x67, 0x67, 0x67}, {0x00, 0x00, 0x00}},
	// Autumn
	{{0xFF, 0xF6, 0xD3}, {0xF9, 0xA8, 0x75}, {0xEB, 0x6B, 0x6F}, {0x7C, 0x3F, 0x58}},
	// Soft
	{{0xE0, 0xF8, 0xD0}, {0x88, 0xC0, 0x70}, {0x34, 0x68, 0x56}, {0x08, 0x18, 0x20}},
};

// The shader is read only by the UI thread (the upscale runs there), so a plain
// int written by the settings page, on that same thread, is enough. Palette and
// colour correction instead have to be applied to the core on the other thread:
// the page raises a flag and the loop applies them between frames.
static int g_shader; // 0 pixel perfect, 1 GBC, 2 GBC dot matrix, 3 GB dot matrix
static volatile bool want_video_settings;

static int clamp_setting(long v, int max) { return v < 0 ? 0 : v > max ? max : (int)v; }

static void apply_core_video_settings(gb_core_t *core) {
	int pal = clamp_setting(config_get_int("gearboy", "palette", 0), 4);
	gb_core_set_dmg_palette(core, GB_PALETTES[pal]);
	gb_core_set_color_correction(core, config_get_int("gearboy", "gbc_correction", 1) != 0);
}

void gearboy_refresh_video_settings(void) {
	g_shader = clamp_setting(config_get_int("gearboy", "shader", 0), 3);
	want_video_settings = true; // the core thread does the rest, if it is running
}

bool gearboy_running(void) { return running_flag; }
bool gearboy_failed(void) { return failed_flag; }
const char *gearboy_title(void) { return title; }
const uint16_t *gearboy_screen(void) { return screen; }

static void blit3x(const uint16_t *src, uint16_t *dst);
static void blit3x_grid(const uint16_t *src, uint16_t *dst, int shader);

bool gearboy_present(void) {
	if (!screen || gb_ready_index < 0) {
		return false;
	}

	// Two attempts: if the core catches up during the upscale the frame is
	// redone, but retrying forever is worse than an occasional seam.
	for (int attempt = 0; attempt < 2; attempt++) {
		uint32_t before = frame_count;
		int index = gb_ready_index;
		if (index < 0 || index >= GB_FRAME_BUFFERS || !gb_frames[index]) {
			return false;
		}
		__sync_synchronize();
		if (g_shader != 0) {
			blit3x_grid(gb_frames[index], screen, g_shader);
		} else {
			blit3x(gb_frames[index], screen);
		}
		__sync_synchronize();
		// Only once the core has advanced by as many frames as there are
		// buffers can it have started overwriting this one.
		if (frame_count - before < GB_FRAME_BUFFERS) {
			return true;
		}
	}
	return true;
}
uint32_t gearboy_frame_count(void) { return frame_count; }
void gearboy_set_keys(uint16_t mask) { key_mask = mask; }
bool gearboy_paused(void) { return paused_flag; }

void gearboy_set_paused(bool paused) {
	if (paused) {
		// No key survives a pause: fingers leave the glass to reach the menu,
		// and without this the direction they were holding would stay pressed
		// until play resumes.
		key_mask = 0;
	}
	paused_flag = paused;
}

void gearboy_request_savestate(void) { want_savestate = true; }
void gearboy_request_loadstate(void) { want_loadstate = true; }

int gearboy_take_state_result(void) {
	int r = state_result;
	state_result = 0;
	return r;
}

// The saves directory, created on first use. Returns an empty string, which
// gbcore reads as "next to the ROM", when there is no card: without a card
// there would be no ROM either, and an invented path is worse than none.
static const char *saves_dir(void) {
	static char path[512];
	const char *dir = gbdb_saves_dir();
	if (!dir || !dir[0]) {
		return "";
	}
	snprintf(path, sizeof(path), "%s", dir);

	// Two levels by hand instead of a recursive mkdir: there are only two, both
	// known, and "Games" always exists already since the ROMs live in it.
	char *last = strrchr(path, '/');
	if (last) {
		*last = '\0';
		mkdir(path, 0777);
		*last = '/';
	}
	mkdir(path, 0777);
	return path;
}

// ---------------------------------------------------------------------------
// the upscale
// ---------------------------------------------------------------------------
//
// 160x144 -> 480x432, three times in each direction with no filtering. Integer
// scale means every pixel becomes an exact 3x3 square with nothing to
// interpolate, which is also the right way to show a Game Boy: a blurred pixel
// is no longer a pixel.
//
// The loop walks the source in pairs: two 16-bit pixels become six, which is
// three aligned 32-bit stores instead of six 16-bit ones. That matters on MIPS,
// where an unaligned 32-bit store costs an exception. 160 is even, so no tail
// case is needed.
// The two repeated rows are memcpys of the row just built; libc beats any loop
// written here.
static void blit3x(const uint16_t *src, uint16_t *dst) {
	const int dst_w = GEARBOY_SCREEN_W;
	const size_t row_bytes = (size_t)dst_w * 2;

	for (int y = 0; y < GB_HEIGHT; y++) {
		const uint16_t *s = src + (size_t)y * GB_WIDTH;
		uint16_t *row = dst + (size_t)(y * 3) * dst_w;
		uint32_t *d = (uint32_t *)row;

		for (int x = 0; x < GB_WIDTH; x += 2) {
			uint32_t p0 = s[x];
			uint32_t p1 = s[x + 1];
			d[0] = p0 | (p0 << 16);
			d[1] = p0 | (p1 << 16);
			d[2] = p1 | (p1 << 16);
			d += 3;
		}

		memcpy(row + dst_w, row, row_bytes);
		memcpy(row + 2 * (size_t)dst_w, row, row_bytes);
	}
}

// The "shaders": the pixel grid of the real panels, drawn into the upscale.
// Each game pixel is a 3x3 square whose third column and third row are dimmed,
// so the square reads as an LCD pixel again instead of a flat block.
//
//   GBC            light grid (75%): the dense colour panel
//   GBC Dot matrix strong grid (50%): clearly visible matrix
//   GB Dot matrix  strong grid plus an even darker corner: the rounded cell of
//                  the original Game Boy panel
//
// RGB565 arithmetic without splitting channels: >>1 masked with 0x7BEF is 50%,
// adding >>2 masked with 0x39E7 gives 75%.
static inline uint16_t dim75(uint16_t p) { return (uint16_t)(((p >> 1) & 0x7BEF) + ((p >> 2) & 0x39E7)); }
static inline uint16_t dim50(uint16_t p) { return (uint16_t)((p >> 1) & 0x7BEF); }
static inline uint16_t dim25(uint16_t p) { return (uint16_t)((p >> 2) & 0x39E7); }

static void blit3x_grid(const uint16_t *src, uint16_t *dst, int shader) {
	const int dst_w = GEARBOY_SCREEN_W;
	const size_t row_bytes = (size_t)dst_w * 2;
	const bool strong = shader >= 2;	  // both dot matrix modes
	const bool corner = shader == 3;	  // GB dot matrix only

	for (int y = 0; y < GB_HEIGHT; y++) {
		const uint16_t *s = src + (size_t)y * GB_WIDTH;
		uint16_t *row0 = dst + (size_t)(y * 3) * dst_w;
		uint16_t *row2 = row0 + 2 * (size_t)dst_w;

		// First two rows of the square: full, full, dimmed column.
		uint16_t *d = row0;
		for (int x = 0; x < GB_WIDTH; x++) {
			uint16_t p = s[x];
			d[0] = p;
			d[1] = p;
			d[2] = strong ? dim50(p) : dim75(p);
			d += 3;
		}
		memcpy(row0 + dst_w, row0, row_bytes);

		// Third row: fully dimmed, with the corner dimmed further when asked;
		// that is what rounds the cell.
		d = row2;
		for (int x = 0; x < GB_WIDTH; x++) {
			uint16_t p = s[x];
			uint16_t dimmed = strong ? dim50(p) : dim75(p);
			d[0] = dimmed;
			d[1] = dimmed;
			d[2] = corner ? dim25(p) : dimmed;
			d += 3;
		}
	}
}

// Marks the frame just finished as the good one and moves the core to the other
// buffer. The barrier before the counter is not a formality: MIPS does not
// order stores, so without it the UI thread can see the new count while the
// tail of the frame is still in flight.
static void publish_frame(void) {
	__sync_synchronize();
	gb_ready_index = gb_write_index;
	frame_count++;
	gb_write_index = (gb_write_index + 1) % GB_FRAME_BUFFERS;
}

// ---------------------------------------------------------------------------
// the worker thread
// ---------------------------------------------------------------------------

static void *emu_worker(void *unused) {
	(void)unused;

	// Declared before the two gotos to `done` so neither jumps over an
	// initialisation.
	const char *saves = "";

	gb_core_t *core = gb_core_create();
	if (!core) {
		fprintf(stderr, "gearboy: the core does not start (memory)\n");
		failed_flag = true;
		goto done;
	}

	gb_core_set_sample_rate(core, SAMPLE_RATE);

	// Real boot ROMs when the option is on and the files exist. This has to
	// happen before loading the ROM, since that reset is where the core decides
	// whether to use them. Missing files or option off: boot straight into the
	// game.
	if (config_get_int("gearboy", "bootrom", 0) != 0) {
		const char *bios = gbdb_bios_dir();
		if (bios[0]) {
			char dmg[560], cgb[560];
			snprintf(dmg, sizeof(dmg), "%s/dmg_boot.bin", bios);
			snprintf(cgb, sizeof(cgb), "%s/cgb_boot.bin", bios);
			if (gb_core_set_bootroms(core, dmg, cgb)) {
				printf("gearboy: boot roms loaded from %s\n", bios);
			} else {
				printf("gearboy: boot roms asked for but missing in %s\n", bios);
			}
		}
	}

	apply_core_video_settings(core);
	g_shader = clamp_setting(config_get_int("gearboy", "shader", 0), 3);
	want_video_settings = false;

	// forceDMG stays false: the cartridge header already says whether the game
	// is colour, and forcing original Game Boy on a GBC title would show four
	// greys where the game has 32768 colours.
	if (!gb_core_load_file(core, rom_path, false)) {
		fprintf(stderr, "gearboy: %s does not load\n", rom_path);
		failed_flag = true;
		goto done;
	}

	// Cartridge RAM from the .sav in Games/Saves. On a cartridge without a
	// battery this does nothing, and that is not an error.
	saves = saves_dir();
	gb_core_load_ram(core, saves);

	printf("gearboy: %s (%s) -- %s\n", title, gb_core_rom_name(core), gb_core_is_color(core) ? "colour" : "DMG");

	// If the PCM does not open the game still runs, silently: a mute game beats
	// a game that will not start. Without it there is no clock either, so the
	// loop has to pace itself.
	bool have_audio = audio_external_begin_latency(SAMPLE_RATE, 2, 16, AUDIO_BUFFER_MS);
	if (!have_audio) {
		fprintf(stderr, "gearboy: no audio; playing silently\n");
	}

	while (!stop_flag) {
		// Video settings changed by the page are applied here, between frames,
		// the only moment the core is idle.
		if (want_video_settings) {
			want_video_settings = false;
			apply_core_video_settings(core);
		}

		// While paused the core does not run, but the PCM keeps being fed
		// silence: that is what paces the loop (see gearboy.h) and what keeps
		// the DAC from underrunning and clicking on resume.
		if (paused_flag) {
			// The menu's two requests are served here, the only point where the
			// core is stopped with no frame in flight through it.
			if (want_savestate) {
				want_savestate = false;
				state_result = gb_core_save_state(core, saves, STATE_SLOT) ? 1 : -1;
			}
			if (want_loadstate) {
				want_loadstate = false;
				bool ok = gb_core_load_state(core, saves, STATE_SLOT);
				state_result = ok ? 2 : -2;
				if (ok) {
					// The loaded state lives inside the core; the frame buffers
					// still hold the pre-load image. Running one frame (16 ms,
					// discarding its audio) makes the screen show the point
					// actually restored rather than the one left behind.
					int unused_samples = 0;
					gb_core_run_frame(core, gb_frames[gb_write_index], audio_buf, &unused_samples);
					publish_frame();
				}
			}

			if (have_audio) {
				memset(audio_buf, 0, (size_t)PAUSE_FRAMES * 2 * sizeof(*audio_buf));
				audio_external_write(audio_buf, PAUSE_FRAMES);
			} else {
				usleep(16000);
			}
			continue;
		}

		gb_core_set_keys(core, key_mask);

		int samples = 0;
		gb_core_run_frame(core, gb_frames[gb_write_index], audio_buf, &samples);
		publish_frame();

		if (have_audio && samples > 0) {
			// samples counts interleaved values, two per audio frame, while the
			// write wants frames. The thread blocks in here, and that wait is
			// what paces everything else.
			audio_external_write(audio_buf, samples / 2);
		} else if (!have_audio) {
			// No PCM, no clock. A flat 16 ms is not the exact 16.743, but with
			// no sound to stay in step with, nobody hears the difference.
			usleep(16000);
		}
	}

	if (have_audio) {
		audio_external_end();
	}

	// Flush cartridge RAM before dropping the core, or the player's progress is
	// lost.
	gb_core_save_ram(core, saves);

done:
	if (core) {
		gb_core_destroy(core);
	}
	running_flag = false;
	return NULL;
}

bool gearboy_start(const char *rom_path_in, const char *title_in) {
	if (!rom_path_in || !rom_path_in[0]) {
		return false;
	}
	if (running_flag) {
		return false;
	}

	// A previous thread may have exited on its own without being joined: a ROM
	// that fails to load returns immediately.
	if (thread_live) {
		pthread_join(worker, NULL);
		thread_live = false;
	}

	// The PCM takes a single writer, and music under a game is not wanted
	// anyway: the game brings its own audio.
	audio_stop();

	if (!screen) {
		screen = calloc(1, (size_t)GEARBOY_SCREEN_W * GEARBOY_SCREEN_H * 2);
	}
	for (int i = 0; i < GB_FRAME_BUFFERS; i++) {
		if (!gb_frames[i]) {
			// GB_FRAME_MAX_PIXELS, not GB_WIDTH*GB_HEIGHT: see gbcore.h for
			// the overflow behind that difference.
			gb_frames[i] = calloc(1, (size_t)GB_FRAME_MAX_PIXELS * 2);
		}
	}
	if (!audio_buf) {
		audio_buf = calloc(GB_AUDIO_MAX_SAMPLES, sizeof(*audio_buf));
	}
	if (!screen || !gb_frames[0] || !gb_frames[1] || !audio_buf) {
		fprintf(stderr, "gearboy: not enough memory for the buffers\n");
		failed_flag = true;
		return false;
	}

	snprintf(rom_path, sizeof(rom_path), "%s", rom_path_in);
	snprintf(title, sizeof(title), "%s", title_in && title_in[0] ? title_in : rom_path_in);

	frame_count = 0;
	gb_write_index = 0;
	gb_ready_index = -1;
	key_mask = 0;
	paused_flag = false;
	want_savestate = false;
	want_loadstate = false;
	state_result = 0;
	stop_flag = false;
	failed_flag = false;
	running_flag = true;

	if (pthread_create(&worker, NULL, emu_worker, NULL) != 0) {
		fprintf(stderr, "gearboy: the thread does not start\n");
		running_flag = false;
		return false;
	}
	thread_live = true;
	return true;
}

void gearboy_stop(void) {
	if (!thread_live) {
		return;
	}
	// If the game was paused the loop is spinning in the silence branch, which
	// checks stop_flag like the rest, so no wakeup is needed. The pause flag
	// still has to be cleared or the next game would start already paused.
	stop_flag = true;
	paused_flag = false;
	pthread_join(worker, NULL);
	thread_live = false;
	running_flag = false;
	title[0] = '\0';

	// About half a megabyte across the upscaled image, the core frames and the
	// samples. The thread has ended so nothing writes them any more; making
	// sure nothing reads them is the caller's job (see gearboy.h).
	free(screen);
	screen = NULL;
	for (int i = 0; i < GB_FRAME_BUFFERS; i++) {
		free(gb_frames[i]);
		gb_frames[i] = NULL;
	}
	free(audio_buf);
	audio_buf = NULL;
}

#endif /* GB_CORE */