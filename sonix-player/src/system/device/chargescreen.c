#include "chargescreen.h"

#ifdef HOST_BUILD

void chargescreen_run_if_wanted(void) {}

bool chargescreen_mark(int mark) {
	(void)mark;
	return false;
}

#else

#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <time.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "src/system/core/config.h"
#include "src/system/core/respath.h"
#include "src/system/device/axpcharge.h"
#include "src/system/device/bootlogo.h"
#include "src/system/device/power.h"

#define AXP_REG_STATUS1 0x00
#define AXP_STATUS1_VBUS_GOOD 0x20
#define AXP_REG_MARK 0x07
#define AXP_REG_PWRON_STATUS 0x20
#define AXP_PWRON_KEY 0x01
#define AXP_REG_IRQ_STATUS0 0x48

#define FB_DEV "/dev/fb0"
#define BLANK_NODE "/sys/class/graphics/fb0/blank"
#define BACKLIGHT_NODE "/sys/class/backlight/backlight_pwm0/brightness"
#define BACKLIGHT_MAX_NODE "/sys/class/backlight/backlight_pwm0/max_brightness"
#define SUPPLY_DIR "/sys/class/power_supply"

// The R3 Pro II's RGB LED takes a pattern; the R1 has a red and a blue LED of
// their own (led.c). Patterns: 1 the start-up aqua, 5 off, 6 the charging red.
#define LED_PATTERN_NODE "/sys/class/leds/sgm31324-leds/led_pattern"
#define LED_RED_TRIGGER_NODE "/sys/class/leds/red/trigger"
#define LED_BLUE_NODE "/sys/class/leds/blue/brightness"

// The power key's device, by the name its driver registers.
#define KEYS_NAME "md-gpio-keys"
#define KEYS_FALLBACK "/dev/input/event0"

#define SHOWN_MS 10000		 // how long the battery stays lit
#define LONG_PRESS_MS 1000	 // the power key held: start the player
#define RELEASE_WAIT_MS 8000 // then the key is waited for, up to this long
#define POLL_MS 500
#define CABLE_GONE_MS 6000	 // the cable not seen for this long: off
#define CABLE_GRACE_MS 20000 // since the boot: the supplies are not trusted sooner
#define SETTLE_US 60000		 // an unblank re-initialises the panel; power.c waits the same

// The level written when the settings name none, and the dimmest taken from
// them: the same floor as power.c's.
#define BRIGHTNESS_DEFAULT 60
#define BRIGHTNESS_FLOOR 20

typedef enum { CHARGE_LED_RED, CHARGE_LED_START, CHARGE_LED_OFF } charge_led_t;

static int fb_fd = -1;
static uint8_t *fb_mem;
static size_t fb_map_size;
static struct fb_var_screeninfo fb_var;
static int fb_stride; // bytes per line
static int fb_pages;
static bool screen_on;
static int brightness;

static FT_Library ft_lib;
static FT_Face ft_face;

// ---------------------------------------------------------------------------
// sysfs
// ---------------------------------------------------------------------------

static void write_str(const char *path, const char *value) {
	FILE *f = fopen(path, "w");
	if (f) {
		fputs(value, f);
		fclose(f);
	}
}

static int read_int(const char *path, int fallback) {
	FILE *f = fopen(path, "r");
	int v = fallback;
	if (f) {
		if (fscanf(f, "%d", &v) != 1) {
			v = fallback;
		}
		fclose(f);
	}
	return v;
}

static bool read_line(const char *path, char *out, size_t size) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	bool ok = fgets(out, (int)size, f) != NULL;
	fclose(f);
	if (ok) {
		out[strcspn(out, "\r\n")] = '\0';
	}
	return ok;
}

// The level of the first battery that reports one above zero, as the status
// bar takes it (system.c): a second gauge node can answer every read with 0.
static int battery_percent(int fallback) {
	DIR *dir = opendir(SUPPLY_DIR);
	if (!dir) {
		return fallback;
	}
	int percent = -1;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char path[320], type[32];
		snprintf(path, sizeof(path), SUPPLY_DIR "/%s/type", de->d_name);
		if (!read_line(path, type, sizeof(type)) || strcmp(type, "Battery") != 0) {
			continue;
		}
		snprintf(path, sizeof(path), SUPPLY_DIR "/%s/capacity", de->d_name);
		int value = read_int(path, -1);
		if (value > 0 && percent <= 0) {
			percent = value;
		} else if (value == 0 && percent < 0) {
			percent = 0;
		}
	}
	closedir(dir);
	if (percent < 0) {
		return fallback;
	}
	return percent > 100 ? 100 : percent;
}

static bool vbus_good(bool *known) {
	uint8_t status;
	*known = axpcharge_reg_read(AXP_REG_STATUS1, &status);
	return *known && (status & AXP_STATUS1_VBUS_GOOD);
}

// The PMIC's VBUS-good bit, which answers from the first moment, or any supply
// that is not a battery and says it is online. Both, because on the R3 Pro II
// the cable reaches the MP2731 charger and not necessarily the PMIC's input.
static bool cable_in(void) {
	bool known;
	if (vbus_good(&known)) {
		return true;
	}
	DIR *dir = opendir(SUPPLY_DIR);
	if (!dir) {
		return true; // cannot tell: not a reason to power off
	}
	bool in = false;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char path[320];
		snprintf(path, sizeof(path), SUPPLY_DIR "/%s/online", de->d_name);
		if (read_int(path, 0) == 1) {
			in = true;
		}
	}
	closedir(dir);
	return in;
}

static void led(charge_led_t state) {
	switch (state) {
	case CHARGE_LED_RED:
		write_str(LED_PATTERN_NODE, "6");
		write_str(LED_RED_TRIGGER_NODE, "breathing");
		write_str(LED_BLUE_NODE, "0");
		break;
	case CHARGE_LED_START:
		write_str(LED_PATTERN_NODE, "1");
		write_str(LED_RED_TRIGGER_NODE, "none");
		write_str(LED_BLUE_NODE, "50");
		break;
	case CHARGE_LED_OFF:
		write_str(LED_PATTERN_NODE, "5");
		write_str(LED_RED_TRIGGER_NODE, "none");
		write_str(LED_BLUE_NODE, "0");
		break;
	}
}

static long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// ---------------------------------------------------------------------------
// Drawing
//
// assets/icons/battery-device-off.svg standing in the middle of the screen,
// green from the bottom to the level, the bolt of zap-device-off.svg in yellow
// over it, and the percentage under it in the interface's bold font. Both
// icons are drawn here from their geometry, in their own 24-unit boxes, with
// antialiased edges; every size is scaled from a 480-pixel-wide screen.
// ---------------------------------------------------------------------------

typedef struct {
	uint8_t *mem;
	int w, h;
	int stride; // bytes
	int bpp;
} canvas_t;

typedef struct {
	uint8_t r, g, b;
} rgb_t;

static const rgb_t OUTLINE = {255, 255, 255};
static const rgb_t FILL = {1, 219, 17};
static const rgb_t BOLT = {255, 204, 0};
static const rgb_t TEXT = {255, 255, 255};

// Pixels per icon unit for the battery on a 480-pixel-wide screen, and the
// bolt's height as a share of the body's.
#define BATTERY_UNIT_PX 14.0f
#define BOLT_SCALE 0.42f

// The bolt's outline, from tools/charge_zap.py. The icon strokes it 2 units
// wide with round joins, on top of the fill.
static const float ZAP_POINTS[][2] = {
	{15.914f, 4.000f}, {15.990f, 3.336f}, {15.771f, 2.705f}, {15.300f, 2.232f}, {14.670f, 2.011f}, {14.006f, 2.084f}, {13.440f, 2.439f}, {4.440f, 11.439f}, {4.115f, 11.926f}, {4.000f, 12.500f}, {4.114f, 13.074f}, {4.439f, 13.560f}, {4.926f, 13.886f}, {5.500f, 14.000f}, {9.502f, 14.000f}, {9.658f, 14.025f}, {9.799f, 14.098f}, {9.910f, 14.211f}, {9.979f, 14.353f}, {10.001f, 14.510f}, {9.973f, 14.666f}, {8.086f, 20.000f}, {8.010f, 20.664f}, {8.229f, 21.295f}, {8.701f, 21.768f}, {9.331f, 21.990f}, {9.995f, 21.915f}, {10.561f, 21.560f}, {19.561f, 12.560f}, {19.886f, 12.073f}, {20.000f, 11.499f}, {19.885f, 10.926f}, {19.560f, 10.439f}, {19.074f, 10.114f}, {18.500f, 10.000f}, {14.503f, 10.000f}, {14.346f, 9.975f}, {14.205f, 9.902f}, {14.094f, 9.789f}, {14.024f, 9.646f}, {14.002f, 9.489f}, {14.031f, 9.333f},
};
#define ZAP_COUNT ((int)(sizeof(ZAP_POINTS) / sizeof(ZAP_POINTS[0])))

// Paints `c` over what is there, `a` of 255 of it.
static void blend(const canvas_t *cv, int x, int y, rgb_t c, int a) {
	if (a <= 0 || x < 0 || y < 0 || x >= cv->w || y >= cv->h) {
		return;
	}
	if (a > 255) {
		a = 255;
	}
	uint8_t *p = cv->mem + (size_t)y * cv->stride + (size_t)x * (cv->bpp / 8);
	int r, g, b;
	if (cv->bpp == 16) {
		uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
		r = (v >> 8) & 0xF8;
		g = (v >> 3) & 0xFC;
		b = (v << 3) & 0xF8;
	} else {
		b = p[0];
		g = p[1];
		r = p[2];
	}
	r += (c.r - r) * a / 255;
	g += (c.g - g) * a / 255;
	b += (c.b - b) * a / 255;
	if (cv->bpp == 16) {
		uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
		p[0] = (uint8_t)v;
		p[1] = (uint8_t)(v >> 8);
	} else {
		p[0] = (uint8_t)b;
		p[1] = (uint8_t)g;
		p[2] = (uint8_t)r;
		p[3] = 0xFF;
	}
}

typedef struct {
	float cx, cy; // centre
	float hw, hh; // half width and half height
	float r;	  // corner radius
} rrect_t;

// How much of the pixel at (x, y) lies inside `rr`, 0 to 1: from the signed
// distance of its centre to the rounded rectangle.
static float rrect_cover(const rrect_t *rr, int x, int y) {
	float qx = fabsf((float)x + 0.5f - rr->cx) - (rr->hw - rr->r);
	float qy = fabsf((float)y + 0.5f - rr->cy) - (rr->hh - rr->r);
	float ox = qx > 0 ? qx : 0;
	float oy = qy > 0 ? qy : 0;
	float inside = qx > qy ? qx : qy;
	float d = sqrtf(ox * ox + oy * oy) + (inside < 0 ? inside : 0) - rr->r;
	float cover = 0.5f - d;
	return cover < 0 ? 0 : (cover > 1 ? 1 : cover);
}

static void rrect_bounds(const rrect_t *rr, int *x0, int *y0, int *x1, int *y1) {
	*x0 = (int)floorf(rr->cx - rr->hw) - 1;
	*y0 = (int)floorf(rr->cy - rr->hh) - 1;
	*x1 = (int)ceilf(rr->cx + rr->hw) + 1;
	*y1 = (int)ceilf(rr->cy + rr->hh) + 1;
}

static void fill_rrect(const canvas_t *cv, const rrect_t *rr, rgb_t c) {
	int x0, y0, x1, y1;
	rrect_bounds(rr, &x0, &y0, &x1, &y1);
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			blend(cv, x, y, c, (int)(rrect_cover(rr, x, y) * 255.0f + 0.5f));
		}
	}
}

// The ring between `outer` and `inner`.
static void stroke_rrect(const canvas_t *cv, const rrect_t *outer, const rrect_t *inner, rgb_t c) {
	int x0, y0, x1, y1;
	rrect_bounds(outer, &x0, &y0, &x1, &y1);
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			float cover = rrect_cover(outer, x, y) - rrect_cover(inner, x, y);
			blend(cv, x, y, c, (int)(cover * 255.0f + 0.5f));
		}
	}
}

// `rr` from `top` down: the fill of a battery `top` pixels into it.
static void fill_rrect_below(const canvas_t *cv, const rrect_t *rr, float top, rgb_t c) {
	int x0, y0, x1, y1;
	rrect_bounds(rr, &x0, &y0, &x1, &y1);
	for (int y = y0; y <= y1; y++) {
		float below = (float)y + 1.0f - top;
		float rows = below < 0 ? 0 : (below > 1 ? 1 : below);
		if (rows <= 0) {
			continue;
		}
		for (int x = x0; x <= x1; x++) {
			blend(cv, x, y, c, (int)(rrect_cover(rr, x, y) * rows * 255.0f + 0.5f));
		}
	}
}

// How far (x, y) is from the bolt's outline, negative inside it, in units.
static float zap_distance(float x, float y) {
	float best = 1e9f;
	bool inside = false;
	for (int i = 0, j = ZAP_COUNT - 1; i < ZAP_COUNT; j = i++) {
		float ax = ZAP_POINTS[j][0], ay = ZAP_POINTS[j][1];
		float bx = ZAP_POINTS[i][0], by = ZAP_POINTS[i][1];
		float ex = bx - ax, ey = by - ay;
		float t = ((x - ax) * ex + (y - ay) * ey) / (ex * ex + ey * ey);
		t = t < 0 ? 0 : (t > 1 ? 1 : t);
		float dx = x - ax - ex * t, dy = y - ay - ey * t;
		float d = dx * dx + dy * dy;
		if (d < best) {
			best = d;
		}
		if ((ay > y) != (by > y) && x < ax + (y - ay) * ex / ey) {
			inside = !inside;
		}
	}
	float d = sqrtf(best);
	return inside ? -d : d;
}

// The bolt, its 24-unit box `k` pixels a unit with its corner at (ox, oy).
static void draw_zap(const canvas_t *cv, float ox, float oy, float k, rgb_t c) {
	int x0 = (int)floorf(ox + 2 * k), x1 = (int)ceilf(ox + 22 * k);
	int y0 = (int)floorf(oy), y1 = (int)ceilf(oy + 24 * k);
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			float d = zap_distance(((float)x + 0.5f - ox) / k, ((float)y + 0.5f - oy) / k) - 1.0f;
			float cover = 0.5f - d * k;
			cover = cover < 0 ? 0 : (cover > 1 ? 1 : cover);
			blend(cv, x, y, c, (int)(cover * 255.0f + 0.5f));
		}
	}
}

// `text` centred on `cx`, its baseline at `baseline`. Nothing without a font.
static void draw_text(const canvas_t *cv, const char *text, int px, int cx, int baseline, rgb_t c) {
	if (!ft_face || FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)px) != 0) {
		return;
	}
	int width = 0;
	for (const char *p = text; *p; p++) {
		if (FT_Load_Char(ft_face, (unsigned char)*p, FT_LOAD_DEFAULT) == 0) {
			width += (int)(ft_face->glyph->advance.x >> 6);
		}
	}
	int pen = cx - width / 2;
	for (const char *p = text; *p; p++) {
		if (FT_Load_Char(ft_face, (unsigned char)*p, FT_LOAD_RENDER) != 0) {
			continue;
		}
		FT_GlyphSlot g = ft_face->glyph;
		if (g->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
			for (unsigned row = 0; row < g->bitmap.rows; row++) {
				const uint8_t *src = g->bitmap.buffer + (ptrdiff_t)row * g->bitmap.pitch;
				for (unsigned col = 0; col < g->bitmap.width; col++) {
					blend(cv, pen + g->bitmap_left + (int)col, baseline - g->bitmap_top + (int)row, c, src[col]);
				}
			}
		}
		pen += (int)(g->advance.x >> 6);
	}
}

static void draw_page(const canvas_t *cv, int percent) {
	for (int y = 0; y < cv->h; y++) {
		memset(cv->mem + (size_t)y * cv->stride, 0, (size_t)cv->w * (cv->bpp / 8));
		if (cv->bpp == 32) {
			for (int x = 0; x < cv->w; x++) {
				cv->mem[(size_t)y * cv->stride + (size_t)x * 4 + 3] = 0xFF;
			}
		}
	}

	float u = (float)cv->w / 480.0f;
	float k = BATTERY_UNIT_PX * u;
	float cx = (float)cv->w / 2.0f;
	float cy = (float)cv->h * 0.42f; // where the body's centre goes

	// The icon's box is 24 x 24 units with the body's centre at (12, 14): a
	// 12 x 16 rectangle at (6, 6) with corners of 2, stroked 2 wide, and the
	// cap, the line from (10, 2) to (14, 2) stroked 2 wide with round ends.
	float ox = cx - 12 * k, oy = cy - 14 * k;
	rrect_t body = {ox + 12 * k, oy + 14 * k, 7 * k, 9 * k, 3 * k};
	rrect_t hollow = {body.cx, body.cy, 5 * k, 7 * k, 1 * k};
	rrect_t cap = {ox + 12 * k, oy + 2 * k, 3 * k, 1 * k, 1 * k};
	rrect_t inside = {body.cx, body.cy, hollow.hw - 0.9f * k, hollow.hh - 0.9f * k, 0.4f * k};

	fill_rrect(cv, &cap, OUTLINE);
	stroke_rrect(cv, &body, &hollow, OUTLINE);

	if (percent > 0) {
		float level = inside.hh * 2.0f * (float)percent / 100.0f;
		if (level < 3 * u) {
			level = 3 * u;
		}
		fill_rrect_below(cv, &inside, inside.cy + inside.hh - level, FILL);
	}

	// The bolt's box centred on the body; the bolt is 22 of its 24 units tall.
	float kz = body.hh * 2.0f * BOLT_SCALE / 22.0f;
	draw_zap(cv, body.cx - 12 * kz, body.cy - 12 * kz, kz, BOLT);

	char text[8];
	snprintf(text, sizeof(text), "%d%%", percent);
	int px = (int)(56 * u);
	draw_text(cv, text, px, (int)cx, (int)(body.cy + body.hh + 34 * u + (float)px * 0.75f), TEXT);
}

// ---------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------

static bool fb_open(void) {
	fb_fd = open(FB_DEV, O_RDWR | O_CLOEXEC);
	if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var) < 0) {
		return false;
	}
	if (fb_var.bits_per_pixel != 16 && fb_var.bits_per_pixel != 32) {
		return false;
	}
	// Two pages, as the player asks for them, so whichever page is scanned out
	// shows the same picture.
	struct fb_var_screeninfo two = fb_var;
	two.yres_virtual = two.yres * 2;
	two.xres_virtual = two.xres;
	two.yoffset = 0;
	if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &two) == 0) {
		ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var);
	}
	struct fb_fix_screeninfo fix;
	if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		return false;
	}
	fb_stride = (int)fix.line_length;
	size_t page = (size_t)fb_stride * fb_var.yres;
	fb_pages = fb_var.yres_virtual >= fb_var.yres * 2 && fix.smem_len >= page * 2 ? 2 : 1;
	fb_map_size = page * (size_t)fb_pages;
	if (fix.smem_len < fb_map_size) {
		return false;
	}
	fb_mem = mmap(NULL, fb_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_mem == MAP_FAILED) {
		fb_mem = NULL;
		return false;
	}
	return true;
}

static void fb_close(void) {
	if (fb_mem) {
		munmap(fb_mem, fb_map_size);
		fb_mem = NULL;
	}
	if (fb_fd >= 0) {
		close(fb_fd);
		fb_fd = -1;
	}
}

static void paint(int percent) {
	if (!fb_mem) {
		return;
	}
	canvas_t cv = {fb_mem, (int)fb_var.xres, (int)fb_var.yres, fb_stride, (int)fb_var.bits_per_pixel};
	draw_page(&cv, percent);
	size_t page = (size_t)fb_stride * fb_var.yres;
	if (fb_pages == 2) {
		memcpy(fb_mem + page, fb_mem, page);
	}
}

// The panel back from a blank, dark until backlight_on().
static void unblank(void) {
	write_str(BLANK_NODE, "0");
	usleep(SETTLE_US);
	// The unblank can reset the mode: put it back, as the player's wake does.
	if (fb_mem) {
		fb_var.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
		ioctl(fb_fd, FBIOPUT_VSCREENINFO, &fb_var);
	}
}

static void backlight_on(void) {
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", brightness);
	write_str(BACKLIGHT_NODE, buf);
	screen_on = true;
}

static void show_battery(int percent) {
	if (fb_mem) {
		paint(percent);
		fb_var.yoffset = 0;
		ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var);
	}
}

// Without a framebuffer it still lights the panel, on whatever it shows, so
// the timeout and the key can put it out again.
static void light(int percent) {
	bool was_on = screen_on;
	if (!was_on) {
		unblank();
	}
	show_battery(percent);
	if (!was_on) {
		backlight_on();
	}
}

// The boot logo, as the start of any other boot shows it, in place of the
// battery: the player draws over it once it is up.
static void show_boot_logo(int percent) {
	bool was_on = screen_on;
	if (!was_on) {
		unblank();
	}
	if (!bootlogo_show_now(config_get_bool("ui", "dark_theme", true), (int)config_get_int("ui", "accent", 0))) {
		show_battery(percent);
	}
	if (!was_on) {
		backlight_on();
	}
}

static void dark(void) {
	write_str(BACKLIGHT_NODE, "1");
	write_str(BLANK_NODE, "4"); // FB_BLANK_POWERDOWN
	screen_on = false;
}

static void font_open(void) {
	// The interface's bold face, MiSans Bold, under either name fonts.c takes.
	static const char *const FILES[] = {
		SONIX_RESOURCE_DIR "/fonts/bold.ttf",
		SONIX_RESOURCE_DIR "/fonts/bold.otf",
	};
	if (FT_Init_FreeType(&ft_lib) != 0) {
		return;
	}
	for (size_t i = 0; i < sizeof(FILES) / sizeof(FILES[0]); i++) {
		if (access(FILES[i], R_OK) == 0 && FT_New_Face(ft_lib, FILES[i], 0, &ft_face) == 0) {
			return;
		}
	}
	ft_face = NULL;
}

static void font_close(void) {
	if (ft_face) {
		FT_Done_Face(ft_face);
		ft_face = NULL;
	}
	if (ft_lib) {
		FT_Done_FreeType(ft_lib);
		ft_lib = NULL;
	}
}

// [screen] brightness, clamped to what the panel takes.
static int configured_brightness(void) {
	int max = read_int(BACKLIGHT_MAX_NODE, 100);
	int value = (int)config_get_int("screen", "brightness", -1);
	if (value < 0) {
		value = BRIGHTNESS_DEFAULT;
	} else if (value < BRIGHTNESS_FLOOR) {
		value = BRIGHTNESS_FLOOR;
	}
	return value > max ? max : value;
}

static int open_keys(void) {
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	while (d && (e = readdir(d)) != NULL) {
		if (strncmp(e->d_name, "event", 5) != 0) {
			continue;
		}
		char node[32];
		if ((size_t)snprintf(node, sizeof(node), "/dev/input/%s", e->d_name) >= sizeof(node)) {
			continue;
		}
		int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			continue;
		}
		char name[128] = {0};
		if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 && strcmp(name, KEYS_NAME) == 0) {
			closedir(d);
			return fd;
		}
		close(fd);
	}
	if (d) {
		closedir(d);
	}
	return open(KEYS_FALLBACK, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

// Waits for the power key to come up, `ms` at most: held through the restart
// of the panel and the player it would reach the PMIC's own power-off.
static void wait_release(int keys, int ms) {
	long until = now_ms() + ms;
	while (keys >= 0 && now_ms() < until) {
		struct pollfd pfd = {.fd = keys, .events = POLLIN};
		if (poll(&pfd, 1, (int)(until - now_ms())) <= 0) {
			return;
		}
		struct input_event ev;
		while (read(keys, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
			if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 0) {
				return;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// The API
// ---------------------------------------------------------------------------

bool chargescreen_mark(int mark) { return axpcharge_reg_write(AXP_REG_MARK, (uint8_t)mark); }

// The charge limit the settings ask for, as powersettings_apply() sets it once
// the interface is up: the drivers start every boot without it.
static void apply_charge_limit(void) {
	if (axpcharge_applies()) {
		axpcharge_set(config_get_bool("power", "charge_limit_80", false), config_get_bool("power", "charge_500ma", false));
	} else {
		power_set_charge_limit((int)config_get_int("power", "charge_limit", 100));
	}
}

// The power key, down at this moment.
static bool power_key_down(void) {
	int keys = open_keys();
	if (keys < 0) {
		return false;
	}
	unsigned long bits[KEY_MAX / (8 * sizeof(unsigned long)) + 1];
	memset(bits, 0, sizeof(bits));
	bool down = ioctl(keys, EVIOCGKEY(sizeof(bits)), bits) >= 0 && (bits[KEY_POWER / (8 * sizeof(unsigned long))] >> (KEY_POWER % (8 * sizeof(unsigned long))) & 1);
	close(keys);
	return down;
}

// The R3 Pro II: the cable reaches the MP2731 charger, not the PMIC, and the
// status names the power-on pin for a start by the cable as for one by the
// key. A charge is a start with the cable in and the key not held; the
// interrupt status is logged for the record.
static bool wanted_mp2731(uint8_t source, bool cable) {
	uint8_t irq[3] = {0};
	for (int i = 0; i < 3; i++) {
		axpcharge_reg_read((uint8_t)(AXP_REG_IRQ_STATUS0 + i), &irq[i]);
	}
	bool key = power_key_down();
	bool by_cable = cable && !key;
	printf("charge: power-on status 0x%02x, irq status %02x %02x %02x, cable %s, key %s, %s\n", source, irq[0], irq[1], irq[2], cable ? "in" : "out", key ? "down" : "up", by_cable ? "a charge" : "not a charge");
	return by_cable;
}

static bool wanted(void) {
	uint8_t mark, source;
	if (!axpcharge_reg_read(AXP_REG_MARK, &mark)) {
		return false;
	}
	// Powered off with the cable in. A request outlives a restart that never
	// came, so it holds only while the cable is still there.
	if (mark == CHARGESCREEN_MARK_REQUESTED) {
		bool cable = cable_in();
		printf("charge: powered off with the cable in, %s\n", cable ? "still in" : "now out");
		if (!cable) {
			chargescreen_mark(CHARGESCREEN_MARK_NONE);
		}
		return cable;
	}
	if (mark != CHARGESCREEN_MARK_NONE || !axpcharge_reg_read(AXP_REG_PWRON_STATUS, &source)) {
		return false;
	}
	bool cable = cable_in();
	if (!axpcharge_applies()) {
		return wanted_mp2731(source, cable);
	}
	// Not by the key, with the cable in: the status says VBUS.
	bool by_cable = !(source & AXP_PWRON_KEY) && cable;
	printf("charge: power-on status 0x%02x, cable %s, %s\n", source, cable ? "in" : "out", by_cable ? "lit by the cable" : "not a charge");
	return by_cable;
}

void chargescreen_run_if_wanted(void) {
	if (!wanted()) {
		return;
	}

	if (!fb_open()) {
		fprintf(stderr, "charge: no usable framebuffer\n");
	}
	font_open();
	brightness = configured_brightness();
	led(CHARGE_LED_RED);
	apply_charge_limit();

	int keys = open_keys();
	int percent = battery_percent(0);
	int shown_percent = percent;
	screen_on = false;
	light(percent);
	long shown_until = now_ms() + SHOWN_MS;
	long held_since = 0;
	long gone_since = 0;

	for (;;) {
		int wait = POLL_MS;
		if (held_since) {
			long left = held_since + LONG_PRESS_MS - now_ms();
			wait = left < 0 ? 0 : (left < POLL_MS ? (int)left : POLL_MS);
		}
		struct pollfd pfd = {.fd = keys, .events = POLLIN};
		if (keys >= 0 && poll(&pfd, 1, wait) > 0) {
			struct input_event ev;
			while (read(keys, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
				if (ev.type != EV_KEY || ev.code != KEY_POWER) {
					continue;
				}
				if (ev.value == 1) {
					held_since = now_ms();
				} else if (ev.value == 0 && held_since) {
					// A short press: dark if lit, the battery again if not.
					held_since = 0;
					if (screen_on) {
						dark();
					} else {
						percent = battery_percent(percent);
						shown_percent = percent;
						light(percent);
						shown_until = now_ms() + SHOWN_MS;
					}
				}
			}
		} else if (keys < 0) {
			usleep((useconds_t)wait * 1000);
		}

		long now = now_ms();
		if (held_since && now - held_since >= LONG_PRESS_MS) {
			printf("charge: power key held, starting the player\n");
			led(CHARGE_LED_START);
			show_boot_logo(battery_percent(percent));
			wait_release(keys, RELEASE_WAIT_MS);
			if (keys >= 0) {
				close(keys);
			}
			fb_close();
			font_close();
			return;
		}

		axpcharge_tick();
		percent = battery_percent(percent);
		if (screen_on && percent != shown_percent) {
			shown_percent = percent;
			paint(percent);
		}
		if (screen_on && now >= shown_until) {
			dark();
		}

		// Out for a while, not for one read: the supply can blink. The grace is
		// counted from the boot (CLOCK_MONOTONIC), not from here.
		if (cable_in() || now < CABLE_GRACE_MS) {
			gone_since = 0;
		} else if (!gone_since) {
			gone_since = now;
		} else if (now - gone_since >= CABLE_GONE_MS) {
			printf("charge: the cable is out, powering off\n");
			fflush(NULL);
			chargescreen_mark(CHARGESCREEN_MARK_NONE);
			led(CHARGE_LED_OFF);
			dark();
			sync();
			int rc = system("poweroff");
			(void)rc;
			sleep(8);
			reboot(RB_POWER_OFF);
		}
	}
}

#endif /* HOST_BUILD */
