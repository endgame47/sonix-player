#include "coverflow.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/system/core/config.h"
#include "src/system/playback/device_state.h"
#include "src/system/core/lang.h"
#include "src/system/library/library.h"
#include "src/system/playback/playlist.h"
#include "src/system/core/utils.h"

// ---------------------------------------------------------------------------
// The crate
//
// A column of records seen from the front. The one in the middle faces the
// viewer; the ones above and below are turned away from it, so each shows its
// outer edge nearer and its inner edge -- the one towards the middle --
// further. The inner edges converge and everything runs back towards a point
// behind the middle record.
//
// The turn is a projection, and it is continuous. A square tilted away from a
// camera does not project to a shorter square: the far edge is narrower than
// the near one, so the outline is a trapezoid, and the picture inside crowds
// towards the far edge. LVGL cannot draw that -- its transforms are affine, and
// no affine map narrows one edge against another -- so every frame of a drag
// works one out per pixel.
//
// The per-frame cost is bounded and constant. Only two records are ever part
// way through their turn -- the two either side of the middle, since anything a
// whole place away is already turned as far as it goes -- and their two heights
// always add up to the same number, so the work is exactly
//
//     CF_MID_W * (CF_MID_W + CF_LEAN_H)
//
// however fast the column is moving, and nothing at all when it is still. It
// also only happens while a finger is down.
//
// What gets turned is not the cover but the RECORD: a square built once per
// place, holding the card and, on it, the artwork -- or the album glyph when
// there is none. So the container turns with the picture, the glyph turns with
// the card, and a place whose cover has not arrived shows a record with nothing
// printed on it rather than somebody else's sleeve.
// ---------------------------------------------------------------------------

// Two records above the middle and two below are what fits. The column is
// walked through the positions in between, so one extra place is carried: at
// any moment one of the six is off an edge.
#define CF_BEFORE 2
#define CF_SLOTS 6

// The record: the size a square is decoded, built and drawn at.
#define CF_MID_W 210

// The height a fully turned record comes out at, and how wide its far edge is
// as a percentage of its near one. Together they are the angle.
#define CF_LEAN_H 90
#define CF_FAR_PCT 62

// How much of the colour survives at the far edge, out of 256, and how the
// light falls off on the way there.
//
// The fall is a square root of the distance rather than the distance itself,
// which keeps the near two thirds close to full colour and puts nearly all the
// darkening in the last strip, where the surface really is turning away. A
// straight-line fall to a darker far edge reads as dirt instead: a third of
// every record smeared uniformly grey, with no sense of anything being lit.
#define CF_FAR_SHADE 178

// How far apart the places are, centre to centre. Less than the middle record
// is tall on purpose: the record beside it tucks behind it, which is what gives
// the column a front and a back instead of a stack of tiles.
#define CF_PITCH_NEAR 118 // between the middle and the place next to it
#define CF_PITCH_FAR 86   // between two turned places further out

// How far the finger travels to move the column by one record.
#define CF_STEP_PX 96

// The travel that separates a tap from a drag.
#define CF_DRAG_COMMIT_PX 8

// The throw: a release carries on at the speed the finger had, slowing to a
// stop, and settles on whichever record it ended nearest.
#define CF_FLING_MS 260
#define CF_SETTLE_MS 220
#define CF_FLING_MAX_PLACES 6

// The band at the bottom of the Musica page the opening pull has to start in,
// and the travel that hands the sheet over to the finger.
#define CF_EDGE_H 56
#define CF_EDGE_COMMIT_PX 12

// The grip at the top: how far down it sits and how big a target it is.
#define CF_BAR_TOP 2
#define CF_BAR_HIT_W 180
#define CF_BAR_HIT_H 46

// The slide, when the finger lets go part way.
#define CF_SHEET_MS 260

// The turn, and the card behind the record.
#define CF_FLIP_MS 130
#define CF_BACK_W 380
#define CF_BACK_PAD 18
#define CF_BACK_BODY_W (CF_BACK_W - 2 * CF_BACK_PAD)
#define CF_BACK_H_MIN 120
#define CF_BACK_LIST_MAX 400
#define CF_BACK_ROW_H 50
#define CF_BACK_ROWS 64
#define CF_BACK_RADIUS 16

// The scroll position is kept in 16.16 of a place, so a drag moves the column
// by fractions of a record and every record's angle moves with it.
#define CF_ONE 65536

// The light the record in the middle throws.
//
// Not LVGL's box shadow, for two reasons.
//
// The first is where it stops. A box shadow reaches about half its blur plus
// its spread and then ends -- the fall is close enough to a straight line that
// the last step to nothing is a visible ring.
//
// The second is banding. The panel is RGB565, so a long ramp of one colour over
// artwork crosses a quantisation step every few pixels and each of those steps
// is a contour line. Nothing in a style can dither that, because nothing in a
// style knows where the pixel is.
//
// So the halo is painted here instead: one 8-bit alpha map, whose colour comes
// from the image's recolour and therefore still changes per album without the
// map being repainted, with a cubic fall to exactly nothing and an ordered
// dither of a few levels laid over it. An A8 image with no transform and no
// clipping radius is drawn by LVGL as a plain mask blend -- the same work a box
// shadow does, without the blur.
//
// It is painted around a rectangle sitting inside the record, so the light
// clears the sleeve on every side; with no vertical offset it rings the sleeve
// instead of pooling under it. That rectangle follows the record's height,
// which shrinks as the record turns, so a sleeve seen at an angle throws a
// flatter light than one facing the front.
#define CF_GLOW_INSET 12
#define CF_GLOW_MIN_H 10
#define CF_GLOW_RADIUS 20
#define CF_GLOW_REACH 52 // how far past the caster the light still reaches
#define CF_GLOW_OPA 210	 // and how strong it is against the caster's own edge

// How many levels of alpha the dither moves a pixel by, either way. An RGB565
// step over artwork is worth some eight to ten levels of alpha, so half of one
// is enough to break a contour and little enough not to read as noise.
#define CF_GLOW_DITHER 5

#define CF_GLOW_MAX_W (CF_MID_W - 2 * CF_GLOW_INSET + 2 * CF_GLOW_REACH)
#define CF_GLOW_MAX_H CF_GLOW_MAX_W

typedef struct {
	lv_obj_t *image;         // the record: the only object a place has
	lv_image_dsc_t warp_dsc; // points at `warp`
	uint8_t *face;           // the square that gets turned: card + artwork or glyph
	uint8_t *warp;           // the turned copy, RGB565A8
	int warp_max_h;          // how tall this place's turned copy can ever be
	int warp_h;              // how tall it is now, -1 for nothing drawn yet
	int index;               // the album it is bound to, -1 for none
	bool requested;
	bool settled; // the worker has answered, one way or the other
	bool has_cover;
	uint32_t tone; // the sleeve's dominant colour, for the glow under the middle
} cf_slot_t;

static cf_slot_t slots[CF_SLOTS];
static lv_obj_t *sheet; // the page itself, which slides in from the bottom edge
static lv_obj_t *viewport;
static lv_obj_t *bar_hit;
static lv_obj_t *title_label;
static lv_obj_t *empty_label;
static lv_timer_t *poll_timer;

// The back of the record: the track list a tap on the middle cover turns it
// around to show.
static lv_obj_t *back_veil;
static lv_obj_t *back_card;
static lv_obj_t *back_body;
static lv_obj_t *back_title;
static char back_album[256]; // the name as the library spells it, not as the title shows it
static lv_obj_t *back_sub;
static lv_obj_t *back_list;
static library_index_t *back_ix;
static int back_count;
static bool flipped;
static bool flipping;

static library_index_t *albums;
static int album_count;
static int32_t scroll;
static int32_t scroll_max;
static int centre;
static int mid_x;
static int mid_y;
static lv_obj_t *glow_obj;
static lv_image_dsc_t glow_dsc;
static uint8_t *glow_map; // the alpha map, always CF_GLOW_MAX_W wide whatever is drawn in it
static int glow_map_w = -1, glow_map_h = -1; // the caster it was painted for
// Filled in as the places are laid out: the record nearest the middle wins.
static int32_t glow_best;
static int glow_bottom;
static int glow_height;
static uint32_t glow_tone;
static int sheet_h;
static bool sheet_open;
static bool sheet_ready; // the buffers are allocated and the album list is open

static bool enabled;
static bool enabled_loaded;

// One window of album names and paths around the middle, so drawing the crate
// does not go to the database for every place every time.
#define CF_WINDOW 16
typedef struct {
	char name[192];
	char path[512];
	bool has_path;
} cf_row_t;
static cf_row_t window_rows[CF_WINDOW];
static int window_first = -1;
static int window_count;

static void relayout(void);
static void sheet_park(void);

// At most one read from the thumbnail database per pass over the places: see
// the note in slot_bind().
static bool shortcut_taken;

// ---------------------------------------------------------------------------
// the setting
// ---------------------------------------------------------------------------

bool coverflow_enabled(void) {
	if (!enabled_loaded) {
		enabled_loaded = true;
		enabled = config_get_int("library", "coverflow", 0) != 0;
	}
	return enabled;
}

void coverflow_set_enabled(bool on) {
	if (coverflow_enabled() == on) {
		return;
	}
	enabled = on;
	config_set_int("library", "coverflow", on ? 1 : 0);
	config_save();
}

// ---------------------------------------------------------------------------
// The projection
//
// For a square standing at an angle about a horizontal axis: call t the
// distance down the finished bitmap, 0 at the near edge and 1 at the far one,
// and s the ratio of the far edge's width to the near one. The plane's depth
// grows evenly across the record but not across the screen, and dividing by it
// gives both numbers this needs:
//
//     denom = (1 - t) + t*s      how wide this row is, as a fraction of the
//                                near edge. Linear, because the sloping sides
//                                of a projected square are still straight lines.
//
//     v     = t*s / denom        which row of the source shows here. NOT t:
//                                that division is the perspective, the reason
//                                the far half of the record carries more of the
//                                picture than the near half.
//
// A plain squeeze is this with s fixed at 1.
//
// The angle is not quantised: `height` comes straight from how far the place is
// from the middle, and everything else follows from it. Fixed point 16.16, and
// 64-bit only in the per-row arithmetic -- sixty to two hundred rows, not per
// pixel.
// ---------------------------------------------------------------------------

// The far edge that goes with a height, in 16.16 of the near edge: one number
// decides the whole shape.
//
// In 16.16 and not in percent. Percent has only thirty-nine values between
// facing the viewer and fully turned, over a hundred and twenty heights, so the
// far edge moved three pixels at a time and stood still in between -- which is
// visible as a stutter at exactly the moment a record comes round to the front.
static int32_t height_to_far(int height) {
	int turn = CF_MID_W - height; // 0 flat, CF_MID_W - CF_LEAN_H fully turned
	int span = CF_MID_W - CF_LEAN_H;
	int32_t full = (int32_t)(((int64_t)CF_FAR_PCT << 16) / 100);
	return 65536 - (int32_t)(((int64_t)(65536 - full) * turn) / span);
}

// Turns `src` (a CF_MID_W square, RGB565) into a trapezoid `height` tall in
// `dst` (RGB565A8: the colour map followed by the alpha map, the alpha stride
// half the colour one -- which is what LVGL assumes, see lv_draw_sw_img.c).
//
// `near_bottom` tells the two sides apart. A record BELOW the middle leans back
// with its bottom edge nearest, so the table is walked backwards -- and the
// picture is read backwards with it, or the record comes out upside down.
// Square root in 16.16, by halving the interval. Sixteen turns, no floating
// point, and it runs once per row and not once per pixel.
static int32_t isqrt16(int32_t x) {
	if (x <= 0) {
		return 0;
	}
	int32_t lo = 0, hi = 65536;
	while (lo < hi) {
		int32_t mid = (lo + hi + 1) >> 1;
		if ((int64_t)mid * mid <= ((int64_t)x << 16)) {
			lo = mid;
		} else {
			hi = mid - 1;
		}
	}
	return lo;
}

static void warp_square(const uint16_t *src, int height, bool near_bottom, uint8_t *dst) {
	const int W = CF_MID_W;
	size_t rgb_bytes = (size_t)W * height * 2;
	const int32_t s = height_to_far(height);

	for (int y = 0; y < height; y++) {
		// The centre of the row, not its top edge: sampling the edge shifts the
		// whole picture up by half a row and loses the last one.
		int row_in_table = near_bottom ? height - 1 - y : y;
		int32_t t = (int32_t)(((int64_t)(2 * row_in_table + 1) << 16) / (2 * height));
		int32_t ts = (int32_t)(((int64_t)t * s) >> 16);
		int32_t denom = (65536 - t) + ts;
		int32_t v = (int32_t)(((int64_t)ts << 16) / denom);
		if (near_bottom) {
			v = 65536 - v;
		}
		// Brightness follows the width, so it fades at the rate the projection
		// narrows rather than at a rate of its own -- but through a square root,
		// so the near part of the record keeps its colour and the fall is
		// gathered at the far edge. A record facing the viewer has no far edge
		// to fade towards, and dividing by the span between the two edges would
		// be dividing by nothing: it keeps all its colour.
		int32_t span = 65536 - s;
		unsigned shade = 256u;
		if (span > 0) {
			int32_t lit = (int32_t)(((int64_t)(denom - s) << 16) / span); // 0 far, 65536 near
			if (lit < 0) {
				lit = 0;
			}
			shade = (unsigned)(CF_FAR_SHADE + (int)(((int64_t)(256 - CF_FAR_SHADE) * isqrt16(lit)) >> 16));
		}

		// The width in sixteenths of a pixel, so the two sloping sides land on
		// a fraction of a pixel and are drawn as one. Rounded to whole pixels
		// they were a staircase, and a staircase is what a leaning record must
		// not have: it is the one edge the eye follows all the way down.
		int32_t width16 = (int32_t)(((int64_t)W * denom) >> 12); // 16ths
		int width = (int)(width16 >> 4);
		if (width < 1) {
			width = 1;
			width16 = 16;
		}
		// What is left over after the whole pixels, split between the two sides:
		// the row overhangs by half of it at each end, not by all of it at both.
		int edge = (int)(width16 & 15);
		int x0 = (W - width) / 2;
		int line = (int)(((int64_t)v * W) >> 16);
		if (line < 0) {
			line = 0;
		} else if (line >= W) {
			line = W - 1;
		}

		const uint16_t *srow = src + (size_t)line * W;
		uint16_t *drgb = (uint16_t *)dst + (size_t)y * W;
		uint8_t *dalpha = dst + rgb_bytes + (size_t)y * W;

		// The alpha of a row is one run of solid between two runs of nothing:
		// three memsets, rather than a store per pixel inside the tight loop.
		if (x0) {
			memset(dalpha, 0, (size_t)x0);
		}
		memset(dalpha + x0, 0xFF, (size_t)width);
		if (W - x0 - width > 0) {
			memset(dalpha + x0 + width, 0, (size_t)(W - x0 - width));
		}

		// And the fraction of a pixel the row overhangs by, one on each side.
		// This is the whole of the antialiasing: the sides of the trapezoid move
		// by less than a pixel per row, so without it they climb in visible
		// steps of three or four rows at a time.
		uint8_t soft = (uint8_t)(edge * 255 / 32);
		if (soft) {
			if (x0 > 0) {
				dalpha[x0 - 1] = soft;
			}
			if (x0 + width < W) {
				dalpha[x0 + width] = soft;
			}
		}

		// Across the row the picture is stretched to the width this row projects
		// to: the division by depth has already been done, once, in picking that
		// width and that source row.
		int32_t step = (int32_t)(((int64_t)W << 16) / width);
		int32_t u = step / 2;
		uint16_t *d = drgb + x0;
		for (int x = 0; x < width; x++) {
			int sx = u >> 16;
			u += step;
			if (sx >= W) {
				sx = W - 1;
			}
			uint16_t c = srow[sx];
			unsigned r = (((c >> 11) & 0x1Fu) * shade) >> 8;
			unsigned g = (((c >> 5) & 0x3Fu) * shade) >> 8;
			unsigned b = ((c & 0x1Fu) * shade) >> 8;
			d[x] = (uint16_t)((r << 11) | (g << 5) | b);
		}

		// The two soft pixels take the colour of the one beside them: their
		// alpha is what draws the edge, and a black pixel under it would put a
		// dark fringe all the way down both sides.
		if (soft) {
			if (x0 > 0) {
				drgb[x0 - 1] = d[0];
			}
			if (x0 + width < W) {
				drgb[x0 + width] = d[width - 1];
			}
		}
	}
}

// ---------------------------------------------------------------------------
// The face of a record
//
// Built once per place per album: the card, and on it either the artwork or the
// album glyph. Everything the crate draws goes through here, so whatever a
// place is showing is turned by the same arithmetic -- there is no object
// standing in front of the projection, staying flat.
// ---------------------------------------------------------------------------

static uint16_t rgb565_of(lv_color_t c) {
	return (uint16_t)(((c.red & 0xF8) << 8) | ((c.green & 0xFC) << 3) | (c.blue >> 3));
}

static void face_fill(uint16_t *face, lv_color_t colour) {
	uint16_t c = rgb565_of(colour);
	int n = CF_MID_W * CF_MID_W;
	for (int i = 0; i < n; i++) {
		face[i] = c;
	}
}

// Lays an ARGB8888 glyph (which is what every icon in this player is) over the
// card, tinted, with its own alpha.
//
// Drawn at its own size, one pixel to one pixel. The glyph handed in must be
// rasterised at the size it is drawn at (icon_album_big): enlarging a small
// icon to fill a 210 px record puts a staircase on every curve.
static void face_draw_glyph(uint16_t *face, const lv_image_dsc_t *glyph, lv_color_t tint, unsigned opa) {
	int gw = (int)glyph->header.w;
	int gh = (int)glyph->header.h;
	if (gw <= 0 || gh <= 0 || gw > CF_MID_W || gh > CF_MID_W) {
		return;
	}
	int ox = (CF_MID_W - gw) / 2;
	int oy = (CF_MID_W - gh) / 2;
	unsigned tr = tint.red, tg = tint.green, tb = tint.blue;

	for (int y = 0; y < gh; y++) {
		const uint8_t *srow = glyph->data + (size_t)y * glyph->header.stride;
		uint16_t *drow = face + (size_t)(oy + y) * CF_MID_W + ox;
		for (int x = 0; x < gw; x++) {
			unsigned a = srow[x * 4 + 3] * opa / 255;
			if (!a) {
				continue;
			}
			uint16_t c = drow[x];
			unsigned r = ((c >> 11) & 0x1F) << 3;
			unsigned g = ((c >> 5) & 0x3F) << 2;
			unsigned b = (c & 0x1F) << 3;
			r = (tr * a + r * (255 - a)) / 255;
			g = (tg * a + g * (255 - a)) / 255;
			b = (tb * a + b * (255 - a)) / 255;
			drow[x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
		}
	}
}

// The artwork, centred on the card. Whatever size it came back at: a sleeve
// that is not square keeps its shape and the card shows around it, which is
// what a record with a smaller label printed on it looks like.
static void face_draw_cover(uint16_t *face, const cover_image_t *cover) {
	int w = (int)cover->dsc.header.w;
	int h = (int)cover->dsc.header.h;
	if (!cover->pixels || w <= 0 || h <= 0) {
		return;
	}
	int stride = (int)cover->dsc.header.stride / 2;
	int ox = (CF_MID_W - w) / 2;
	int oy = (CF_MID_W - h) / 2;
	int x0 = ox < 0 ? -ox : 0;
	int y0 = oy < 0 ? -oy : 0;
	int x1 = ox + w > CF_MID_W ? CF_MID_W - ox : w;
	int y1 = oy + h > CF_MID_W ? CF_MID_W - oy : h;

	for (int y = y0; y < y1; y++) {
		const uint16_t *srow = (const uint16_t *)cover->pixels + (size_t)y * stride;
		uint16_t *drow = face + (size_t)(oy + y) * CF_MID_W + ox;
		memcpy(drow + x0, srow + x0, (size_t)(x1 - x0) * 2);
	}
}

// The record with its sleeve printed on it. The cover is not kept afterwards:
// what the crate draws from is the face.
static void face_paint(cf_slot_t *slot, const cover_image_t *cover) {
	if (!slot->face) {
		return;
	}
	face_fill((uint16_t *)slot->face, theme()->surface);
	face_draw_cover((uint16_t *)slot->face, cover);
	slot->tone = cover_dominant_tone(cover);
	slot->has_cover = true;
	slot->warp_h = -1; // the turned copy is out of date
}

// The empty record: the card and the album glyph, and nothing else.
static void face_reset(cf_slot_t *slot) {
	if (!slot->face) {
		return;
	}
	face_fill((uint16_t *)slot->face, theme()->surface);
	face_draw_glyph((uint16_t *)slot->face, &icon_album_big, theme()->text_secondary, 90);
	slot->tone = 0; // no sleeve, no colour: the glow falls back to the accent
	slot->has_cover = false;
	slot->warp_h = -1; // the turned copy is out of date
}

// ---------------------------------------------------------------------------
// the album window
// ---------------------------------------------------------------------------

typedef struct {
	int written;
} fill_t;

static bool window_fill_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)artist;
	fill_t *fill = user;
	if (fill->written >= CF_WINDOW) {
		return false;
	}
	cf_row_t *row = &window_rows[fill->written++];
	snprintf(row->name, sizeof(row->name), "%s", name ? name : "");
	row->has_path = path && path[0];
	snprintf(row->path, sizeof(row->path), "%s", row->has_path ? path : "");
	return true;
}

// Makes sure `index` is inside the cached window, re-reading it if not. A
// screenful at a time rather than a row at a time: the crate is walked, so the
// next read is nearly always the one after this.
static void window_cover(int index) {
	if (!albums || album_count <= 0) {
		window_first = -1;
		window_count = 0;
		return;
	}
	if (window_first >= 0 && index >= window_first && index < window_first + window_count) {
		return;
	}

	int first = index - CF_WINDOW / 2;
	if (first + CF_WINDOW > album_count) {
		first = album_count - CF_WINDOW;
	}
	if (first < 0) {
		first = 0;
	}

	fill_t fill = {0};
	library_index_window(albums, first, CF_WINDOW, window_fill_cb, &fill);
	window_first = fill.written > 0 ? first : -1;
	window_count = fill.written;
}

static const cf_row_t *album_at(int index) {
	if (index < 0 || index >= album_count) {
		return NULL;
	}
	window_cover(index);
	if (window_first < 0 || index < window_first || index >= window_first + window_count) {
		return NULL;
	}
	return &window_rows[index - window_first];
}

// ---------------------------------------------------------------------------
// the places
// ---------------------------------------------------------------------------

static int slot_id(int i) { return COVERLOADER_COVERFLOW_BASE + i; }

// Where the middle of a place sits, as an offset down the column. `d` is 16.16
// places and may be anything in between.
static int place_offset(int32_t d) {
	int32_t away = d < 0 ? -d : d;
	int32_t near_part = away > CF_ONE ? CF_ONE : away;
	int32_t far_part = away > CF_ONE ? away - CF_ONE : 0;
	int32_t px = (int32_t)(((int64_t)near_part * CF_PITCH_NEAR + (int64_t)far_part * CF_PITCH_FAR) >> 16);
	return d < 0 ? -px : px;
}

// How tall a place this far from the middle is drawn: full height facing the
// viewer, CF_LEAN_H once it is a whole place away, and every value in between.
static int place_height(int32_t d) {
	int32_t away = d < 0 ? -d : d;
	if (away > CF_ONE) {
		away = CF_ONE;
	}
	return CF_MID_W - (int)(((int64_t)away * (CF_MID_W - CF_LEAN_H)) >> 16);
}

// The tallest a place can ever be drawn. Only the two either side of the middle
// come round to facing the viewer; the rest are always turned as far as they
// go, and their turned copy is a third of the size.
static int slot_max_height(int i) {
	int away = i <= CF_BEFORE ? CF_BEFORE - i : i - CF_BEFORE - 1;
	return away == 0 ? CF_MID_W : CF_LEAN_H;
}

// Turns a place's face to the height its position asks for, and hands the
// result to LVGL. Does nothing when the height has not moved: the picture on
// screen is already the right one.
static void slot_turn(int i, int height) {
	cf_slot_t *slot = &slots[i];
	if (!slot->face || !slot->warp) {
		return;
	}
	if (height > slot->warp_max_h) {
		height = slot->warp_max_h;
	}
	if (slot->warp_h == height) {
		return;
	}

	warp_square((const uint16_t *)slot->face, height, i > CF_BEFORE, slot->warp);
	slot->warp_h = height;
	slot->warp_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	slot->warp_dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
	slot->warp_dsc.header.w = CF_MID_W;
	slot->warp_dsc.header.h = (uint32_t)height;
	slot->warp_dsc.header.stride = CF_MID_W * 2;
	slot->warp_dsc.data_size = (uint32_t)CF_MID_W * height * 3;
	slot->warp_dsc.data = slot->warp;
	// The image cache is off in this build (LV_CACHE_DEF_SIZE is 0), so the same
	// descriptor with new contents and a new height is simply read again.
	lv_image_set_src(slot->image, &slot->warp_dsc);
}

static void slot_bind(int i) {
	cf_slot_t *slot = &slots[i];
	int base = scroll >> 16;
	int index = base + i - CF_BEFORE;
	int32_t d = ((int32_t)(i - CF_BEFORE) << 16) - (scroll - (base << 16));

	const cf_row_t *album = album_at(index);
	if (!album) {
		if (slot->index != -1) {
			if (slot->requested) {
				coverloader_release(slot_id(i));
				slot->requested = false;
			}
			slot->index = -1;
			slot->settled = false;
		}
		lv_obj_add_flag(slot->image, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	lv_obj_remove_flag(slot->image, LV_OBJ_FLAG_HIDDEN);

	if (slot->index != index) {
		if (slot->requested) {
			coverloader_release(slot_id(i));
			slot->requested = false;
		}
		slot->index = index;
		slot->settled = false;
		// A record this place has never held. What shows until its sleeve
		// arrives is a blank record -- the card and the album glyph, turned by
		// the same arithmetic as everything else. The five that merely moved up
		// a place do not come through here: faces_rotate() brought their faces
		// with them.
		face_reset(slot);
	}

	// The record arriving at the middle is the one being looked at, and if its
	// sleeve is already on the card there is no reason for it to queue behind a
	// worker for it: cover_thumb_cached() never decodes, so a hit is one small
	// read and the picture is there in this frame.
	//
	// One per pass, and only for the two places either side of the middle. The
	// rest go to the worker, which is what keeps a fast flick from turning into
	// six reads on the interface thread.
	if (!slot->settled && !shortcut_taken && album->has_path && (i == CF_BEFORE || i == CF_BEFORE + 1)) {
		cover_image_t cached;
		if (cover_thumb_cached(album->path, CF_MID_W, &cached)) {
			face_paint(slot, &cached);
			cover_free(&cached);
			slot->settled = true;
			shortcut_taken = true;
			if (slot->requested) {
				coverloader_release(slot_id(i));
				slot->requested = false;
			}
		}
	}

	// Everything else is asked for straight away, moving or not. Holding the
	// requests back while the column moved is what emptied the crate during a
	// scroll.
	if (!slot->requested && !slot->settled) {
		if (album->has_path) {
			coverloader_request(slot_id(i), album->path, CF_MID_W);
			slot->requested = true;
		} else {
			slot->settled = true;
		}
	}

	int height = place_height(d);
	slot_turn(i, height);
	if (slot->warp_h > 0) {
		height = slot->warp_h;
	}
	int y = mid_y + place_offset(d) - height / 2;
	lv_obj_set_pos(slot->image, mid_x - CF_MID_W / 2, y);

	// The glow belongs to whichever record is nearest the middle, and it is
	// worked out here because this is the only place that knows where a place
	// ended up. It swells as the record comes round to face the front: the
	// height IS how far round it is, so the same number does both jobs.
	int32_t away = d < 0 ? -d : d;
	if (away < glow_best) {
		glow_best = away;
		glow_bottom = y + height;
		glow_height = height;
		glow_tone = slot->tone;
	}
}

// ---------------------------------------------------------------------------
// Moving the column without losing the pictures
//
// When the column advances by one place, five of the six records on screen are
// the same five: they have simply moved up a place. Rebinding each slot from
// scratch would throw all five faces away and ask for them again, emptying the
// crate for as long as the finger keeps moving.
//
// So the faces move with the records. What stays with the slot is its turned
// copy, because that belongs to the place and not to the record: a place above
// the middle turns the other way from one below it, and the two by the middle
// are the only ones that can be seen straight. The turned copy is therefore
// marked out of date and worked out again, which is two or three warps -- the
// same order as one frame of a drag, and once per place crossed rather than
// once per frame.
// ---------------------------------------------------------------------------

static int layout_base = INT32_MIN;

static void faces_rotate(int delta) {
	uint8_t *face[CF_SLOTS];
	bool has[CF_SLOTS];
	uint32_t tone[CF_SLOTS];
	int index[CF_SLOTS];
	bool settled[CF_SLOTS];

	for (int i = 0; i < CF_SLOTS; i++) {
		// Anything in flight is addressed by the slot that asked for it, and
		// that slot is about to be showing a different record. Let them all go;
		// slot_bind asks again for whatever is still missing.
		if (slots[i].requested) {
			coverloader_release(slot_id(i));
			slots[i].requested = false;
		}
		face[i] = slots[i].face;
		has[i] = slots[i].has_cover;
		tone[i] = slots[i].tone;
		index[i] = slots[i].index;
		settled[i] = slots[i].settled;
	}

	// The buffers nobody inherits, handed out to the places that came up empty.
	uint8_t *spare[CF_SLOTS];
	int spares = 0;
	for (int i = 0; i < CF_SLOTS; i++) {
		int to = i - delta;
		if (to < 0 || to >= CF_SLOTS) {
			spare[spares++] = face[i];
		}
	}

	int next_spare = 0;
	for (int i = 0; i < CF_SLOTS; i++) {
		int from = i + delta;
		if (from >= 0 && from < CF_SLOTS) {
			slots[i].face = face[from];
			slots[i].has_cover = has[from];
			slots[i].tone = tone[from];
			slots[i].index = index[from];
			slots[i].settled = settled[from];
		} else {
			slots[i].face = spare[next_spare++];
			slots[i].has_cover = false;
			slots[i].index = -1;
			slots[i].settled = false;
			face_reset(&slots[i]);
		}
		// The face this place is holding is not the one its turned copy was made
		// from, whichever way it got here.
		slots[i].warp_h = -1;
	}
}

// Paints the alpha map for a caster of gw x gh.
//
// Every pixel's value is a function of one number: how far it lies outside the
// rounded rectangle of the caster. `qx` and `qy` are the distances past the
// straight part of each side, so both being at or below zero is inside, one of
// them above is a flank -- where the answer is that one alone -- and both above
// is a corner, the only place a square root is needed. That is some twenty
// thousand pixels of the eighty thousand, which is why this can be done on
// every frame of a drag.
//
// The buffer is always the full width so the rows of a short map still line up:
// LVGL reads the stride from the descriptor, not from the width.
static void glow_paint(int gw, int gh) {
	static const uint8_t bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

	int w = gw + 2 * CF_GLOW_REACH;
	int h = gh + 2 * CF_GLOW_REACH;
	int r = CF_GLOW_RADIUS;
	int hw = gw / 2, hh = gh / 2;
	if (r > hw) {
		r = hw;
	}
	if (r > hh) {
		r = hh;
	}
	int cx = CF_GLOW_REACH + hw, cy = CF_GLOW_REACH + hh;

	// The fall, worked out once: (1 - d/reach) squared. A straight line ends in
	// a visible step however faint it is made; a square arrives at nothing with
	// no slope left, which is what makes the edge of the light impossible to
	// find. A cube does the same but spends so much of the reach near nothing
	// that the halo turns into a rim.
	static uint8_t fall[CF_GLOW_REACH + 1];
	static bool fall_ready;
	if (!fall_ready) {
		for (int d = 0; d <= CF_GLOW_REACH; d++) {
			int inv = 256 - d * 256 / CF_GLOW_REACH;
			fall[d] = (uint8_t)(((int64_t)CF_GLOW_OPA * inv * inv) >> 16);
		}
		fall_ready = true;
	}

	// The horizontal term does not change down a column.
	static int qxs[CF_GLOW_MAX_W];
	for (int x = 0; x < w; x++) {
		int off = x - cx;
		qxs[x] = (off < 0 ? -off : off) - (hw - r);
	}

	for (int y = 0; y < h; y++) {
		int off = y - cy;
		int qy = (off < 0 ? -off : off) - (hh - r);
		uint8_t *row = glow_map + (size_t)y * CF_GLOW_MAX_W;
		const uint8_t *dither = &bayer[(y & 3) * 4];

		for (int x = 0; x < w; x++) {
			int qx = qxs[x];
			int d;
			if (qx > 0 && qy > 0) {
				d = (int)utils_isqrt32((uint32_t)(qx * qx + qy * qy)) - r;
			} else {
				d = (qx > qy ? qx : qy) - r;
			}
			if (d < 0) {
				d = 0;
			}
			if (d >= CF_GLOW_REACH) {
				row[x] = 0;
				continue;
			}

			int a = fall[d];
			// Only where there is room to move it. At the very top and the very
			// bottom of the ramp there is no band to break, and a dither there
			// is just speckle in the dark or a flat patch made rough.
			if (a > CF_GLOW_DITHER && a < 255 - CF_GLOW_DITHER) {
				a += (dither[x & 3] - 8) * CF_GLOW_DITHER / 8;
			}
			row[x] = (uint8_t)(a < 0 ? 0 : (a > 255 ? 255 : a));
		}
	}

	glow_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	glow_dsc.header.cf = LV_COLOR_FORMAT_A8;
	glow_dsc.header.w = (uint32_t)w;
	glow_dsc.header.h = (uint32_t)h;
	glow_dsc.header.stride = CF_GLOW_MAX_W;
	glow_dsc.data = glow_map;
	glow_dsc.data_size = (uint32_t)CF_GLOW_MAX_W * (uint32_t)h;
	lv_image_set_src(glow_obj, &glow_dsc);
	lv_obj_invalidate(glow_obj);
}

// Puts the pool of light where the last pass over the places said it goes.
static void place_glow(void) {
	if (!glow_obj || !glow_map) {
		return;
	}
	if (glow_best == INT32_MAX || album_count <= 0) {
		lv_obj_add_flag(glow_obj, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	// Full strength only when the record is facing the front, nothing at all
	// when it is fully turned: a pool of light under a record seen edge-on is
	// light with nothing to come from.
	int span = CF_MID_W - CF_LEAN_H;
	int upright = glow_height - CF_LEAN_H;
	if (upright < 0) {
		upright = 0;
	}
	int opa = CF_GLOW_OPA * upright / span;
	if (opa <= 0) {
		lv_obj_add_flag(glow_obj, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	// Inside the record and centred on it, so what escapes is a ring and not a
	// pool. The width never changes -- a record's width does not -- while the
	// height is whatever the turn has left of it.
	int gw = CF_MID_W - CF_GLOW_INSET * 2;
	int gh = glow_height - CF_GLOW_INSET * 2;
	if (gh < CF_GLOW_MIN_H) {
		gh = CF_GLOW_MIN_H;
	}

	// The map is repainted only when the caster's shape changes, which during a
	// drag is once a frame and while the column is still is never.
	if (gw != glow_map_w || gh != glow_map_h) {
		glow_paint(gw, gh);
		glow_map_w = gw;
		glow_map_h = gh;
	}

	int top = glow_bottom - glow_height + (glow_height - gh) / 2;
	lv_obj_remove_flag(glow_obj, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_pos(glow_obj, mid_x - gw / 2 - CF_GLOW_REACH, top - CF_GLOW_REACH);
	// The colour is the image's recolour, which is what lets one alpha map serve
	// every album.
	lv_obj_set_style_image_recolor(glow_obj, lv_color_hex(glow_tone ? glow_tone : lv_color_to_u32(theme()->accent)),
								   0);
	lv_obj_set_style_image_recolor_opa(glow_obj, LV_OPA_COVER, 0);
	lv_obj_set_style_image_opa(glow_obj, (lv_opa_t)opa, 0);
}

static void relayout(void) {
	shortcut_taken = false;
	glow_best = INT32_MAX;
	int base = scroll >> 16;
	if (layout_base != INT32_MIN && base != layout_base && slots[0].face) {
		int delta = base - layout_base;
		if (delta > -CF_SLOTS && delta < CF_SLOTS) {
			faces_rotate(delta);
		} else {
			for (int i = 0; i < CF_SLOTS; i++) {
				if (slots[i].requested) {
					coverloader_release(slot_id(i));
					slots[i].requested = false;
				}
				slots[i].index = -1;
				slots[i].settled = false;
				face_reset(&slots[i]);
			}
		}
	}
	layout_base = base;

	for (int i = 0; i < CF_SLOTS; i++) {
		slot_bind(i);
	}
	place_glow();
}

static void refresh_title(void) {
	const cf_row_t *album = album_at(centre);
	scrolltext_set(title_label, album ? album->name : "");
	if (album_count <= 0) {
		lv_obj_remove_flag(empty_label, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(empty_label, LV_OBJ_FLAG_HIDDEN);
	}
}

// Moves the column to an absolute position and redraws it. The name under the
// crate follows the record nearest the middle, so it changes halfway between
// two rather than only when everything has come to rest.
static void scroll_set(int32_t value) {
	if (value < 0) {
		value = 0;
	}
	if (value > scroll_max) {
		value = scroll_max;
	}
	if (value == scroll) {
		return;
	}
	scroll = value;
	int nearest = (int)((scroll + CF_ONE / 2) >> 16);
	relayout();
	if (nearest != centre) {
		centre = nearest;
		refresh_title();
	}
}

static void scroll_anim_cb(void *var, int32_t v) {
	(void)var;
	scroll_set(v);
}

// The column has come to rest.
static void scroll_idle(void) {
	relayout();
	lv_timer_resume(poll_timer);
}

static void scroll_anim_done_cb(lv_anim_t *a) {
	(void)a;
	scroll_idle();
}

// Collects whatever the worker has finished. The timer pauses itself the moment
// nothing is outstanding, so a crate sitting still costs nothing.
static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	lv_timer_pause(poll_timer);
	if (!sheet_ready) {
		return;
	}

	bool waiting = false;
	bool changed = false;
	for (int i = 0; i < CF_SLOTS; i++) {
		cf_slot_t *slot = &slots[i];
		if (!slot->requested || slot->settled) {
			continue;
		}
		cover_image_t image;
		bool finished = false;
		if (coverloader_take(slot_id(i), &image, &finished)) {
			// The struct was copied, so the descriptor has to be pointed at this
			// copy's own buffer.
			image.dsc.data = image.pixels;
			// Printed onto the record, and then let go: a second copy of the
			// sleeve beside the face would be a hundred kilobytes nothing reads.
			face_paint(slot, &image);
			cover_free(&image);
			slot->settled = true;
			changed = true;
		} else if (finished) {
			slot->settled = true; // no artwork: the blank record stays
		} else {
			waiting = true;
		}
	}
	if (changed) {
		relayout();
	}
	if (waiting) {
		lv_timer_resume(poll_timer);
	}
}

// ---------------------------------------------------------------------------
// The back of the record
//
// A tap on the middle cover turns it round: the record narrows to nothing about
// its middle -- and since the card is printed on the same bitmap as the sleeve,
// the whole thing turns and not only what is on it -- and the back opens out of
// nothing in its place, the same way. Closing runs the two steps backwards.
//
// The back carries the album's tracks and nothing else: no artwork and no
// glyph, because the picture is the thing that has just been turned over. It is
// built out of the same pieces popover.c uses: a black veil at sixty per cent,
// a surface-coloured card with a sixteen-pixel radius, no border, no shadow and
// no outline of LVGL's, rows separated by a hairline of the secondary text
// colour at twenty per cent.
// ---------------------------------------------------------------------------

typedef struct {
	int written;
	char artist[192];
} track_fill_t;

static void track_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return; // a swipe across the list, not a tap
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (!back_ix || index < 0 || index >= back_count) {
		return;
	}

	// The queue is this album, so next and previous walk the record rather than
	// the folder the file happens to live in. The handle is cloned, not handed
	// over: the card keeps its own to draw from.
	if (device_state_play_index(library_index_clone(back_ix), index)) {
		// Say which record is playing: "play consecutive albums" needs to know
		// what is ending to pick what comes next. The name comes from back_album
		// and not from the title label: the label is in LV_LABEL_LONG_DOT, which
		// overwrites its own buffer with dots, so a name too long for the card
		// would be looked up in the library as "Somethin..." and never found.
		playlist_set_album(back_album);
		player_refresh_now_playing();
	}
	sheet_park();
	switch_screen(player_screen);
}

static bool track_fill_cb(const char *name, const char *path, const char *artist, void *user) {
	(void)path;
	track_fill_t *fill = user;
	if (fill->written >= CF_BACK_ROWS) {
		return false;
	}
	if (fill->written == 0 && artist && artist[0]) {
		snprintf(fill->artist, sizeof(fill->artist), "%s", artist);
	}
	int number = ++fill->written;

	lv_obj_t *row = lv_obj_create(back_list);
	lv_obj_remove_style_all(row);
	lv_obj_set_size(row, lv_pct(100), CF_BACK_ROW_H);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 12, 0);
	lv_obj_set_style_radius(row, 8, 0);
	lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(row, track_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(number - 1));

	// The running number stands in for the artwork the rows do not carry: it is
	// what makes the list read as the back of one record rather than as a
	// fragment of the library.
	lv_obj_t *num = lv_label_create(row);
	lv_obj_set_width(num, 26);
	lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_add_style(num, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(num, &font_ui_18, 0);
	lv_label_set_text_fmt(num, "%d", number);

	lv_obj_t *label = lv_label_create(row);
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_flex_grow(label, 1);
	// One line, always. LV_LABEL_LONG_DOT wraps before it truncates, and a long
	// title on two lines grows the row into the one under it.
	lv_obj_set_height(label, lv_font_get_line_height(&font_ui_20));
	lv_obj_set_style_text_color(label, theme()->text_primary, 0);
	lv_obj_set_style_text_font(label, &font_ui_20, 0);
	lv_label_set_text(label, name ? name : "");

	// A hairline under every row but the last, drawn as a child of the row so it
	// travels with it and needs no second pass over the list.
	lv_obj_t *sep = lv_obj_create(row);
	lv_obj_remove_style_all(sep);
	lv_obj_set_size(sep, lv_pct(100), 1);
	lv_obj_set_style_bg_color(sep, theme()->text_secondary, 0);
	lv_obj_set_style_bg_opa(sep, LV_OPA_20, 0);
	lv_obj_add_flag(sep, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_align(sep, LV_ALIGN_BOTTOM_MID, 0, 0);
	return true;
}

static void back_close_index(void) {
	if (back_ix) {
		library_index_close(back_ix);
		back_ix = NULL;
	}
	back_count = 0;
}

static void back_fill(const char *album) {
	lv_obj_clean(back_list);
	back_close_index();
	snprintf(back_album, sizeof(back_album), "%s", album ? album : "");
	lv_label_set_text(back_title, back_album);
	lv_label_set_text(back_sub, "");
	if (!back_album[0]) {
		return;
	}

	back_ix =
		library_index_open(LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, back_album, LIBRARY_ORDER_DEFAULT, false);
	if (!back_ix) {
		return;
	}
	track_fill_t fill = {0};
	library_index_window(back_ix, 0, CF_BACK_ROWS, track_fill_cb, &fill);
	back_count = library_index_count(back_ix);

	if (fill.artist[0]) {
		lv_label_set_text(back_sub, fill.artist);
	}
	if (fill.written > 0) {
		// The last row keeps no hairline: a line under the final entry reads as
		// a row that failed to load.
		lv_obj_t *last = lv_obj_get_child(back_list, fill.written - 1);
		if (last) {
			lv_obj_t *sep = lv_obj_get_child(last, -1);
			if (sep) {
				lv_obj_add_flag(sep, LV_OBJ_FLAG_HIDDEN);
			}
		}
	}

	// How tall the card has to be, worked out once, here. A card sized to its
	// contents would be re-measured on every frame of the turn, and re-measuring
	// it means laying out two dozen rows again -- for a height that cannot have
	// changed, since only the width is moving.
	lv_obj_update_layout(back_body);
	int height = lv_obj_get_height(back_body) + 2 * CF_BACK_PAD;
	if (height < CF_BACK_H_MIN) {
		height = CF_BACK_H_MIN;
	}
	lv_obj_set_height(back_card, height);
	lv_obj_align(back_card, LV_ALIGN_CENTER, 0, 0);
}

// Both halves of the turn narrow an object about its own middle, so both have
// to move it as they resize it.
static void card_width_cb(void *obj, int32_t v) {
	lv_obj_set_width((lv_obj_t *)obj, v);
	lv_obj_align((lv_obj_t *)obj, LV_ALIGN_CENTER, 0, 0);
}

static void record_width_cb(void *obj, int32_t v) {
	lv_obj_t *image = (lv_obj_t *)obj;
	lv_image_set_scale_x(image, (uint32_t)((int64_t)LV_SCALE_NONE * v / CF_MID_W));
	lv_obj_set_x(image, mid_x - CF_MID_W / 2);
}

static void back_grown_cb(lv_anim_t *a) {
	(void)a;
	flipping = false;
}

static void record_narrowed_cb(lv_anim_t *a) {
	(void)a;
	lv_obj_add_flag(slots[CF_BEFORE].image, LV_OBJ_FLAG_HIDDEN);

	lv_obj_set_width(back_card, 0);
	lv_obj_align(back_card, LV_ALIGN_CENTER, 0, 0);
	lv_obj_remove_flag(back_veil, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(back_veil);

	lv_anim_t g;
	lv_anim_init(&g);
	lv_anim_set_var(&g, back_card);
	lv_anim_set_exec_cb(&g, card_width_cb);
	lv_anim_set_values(&g, 0, CF_BACK_W);
	lv_anim_set_duration(&g, CF_FLIP_MS);
	lv_anim_set_completed_cb(&g, back_grown_cb);
	lv_anim_start(&g);
}

static void flip_open(void) {
	const cf_row_t *album = album_at(centre);
	if (!album || flipping || flipped || album_count <= 0) {
		return;
	}
	flipping = true;
	flipped = true;
	back_fill(album->name);

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, slots[CF_BEFORE].image);
	lv_anim_set_exec_cb(&a, record_width_cb);
	lv_anim_set_values(&a, CF_MID_W, 0);
	lv_anim_set_duration(&a, CF_FLIP_MS);
	lv_anim_set_completed_cb(&a, record_narrowed_cb);
	lv_anim_start(&a);
}

static void record_widened_cb(lv_anim_t *a) {
	(void)a;
	lv_image_set_scale_x(slots[CF_BEFORE].image, LV_SCALE_NONE);
	flipping = false;
	relayout();
}

static void card_shrunk_cb(lv_anim_t *a) {
	(void)a;
	lv_obj_add_flag(back_veil, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(slots[CF_BEFORE].image, LV_OBJ_FLAG_HIDDEN);

	lv_anim_t a2;
	lv_anim_init(&a2);
	lv_anim_set_var(&a2, slots[CF_BEFORE].image);
	lv_anim_set_exec_cb(&a2, record_width_cb);
	lv_anim_set_values(&a2, 0, CF_MID_W);
	lv_anim_set_duration(&a2, CF_FLIP_MS);
	lv_anim_set_completed_cb(&a2, record_widened_cb);
	lv_anim_start(&a2);
}

static void flip_close(bool animate) {
	if (!flipped) {
		return;
	}
	flipped = false;
	lv_anim_delete(back_card, card_width_cb);
	lv_anim_delete(slots[CF_BEFORE].image, record_width_cb);

	if (!animate) {
		lv_obj_add_flag(back_veil, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(slots[CF_BEFORE].image, LV_OBJ_FLAG_HIDDEN);
		record_widened_cb(NULL);
		back_close_index();
		return;
	}

	flipping = true;
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, back_card);
	lv_anim_set_exec_cb(&a, card_width_cb);
	lv_anim_set_values(&a, lv_obj_get_width(back_card), 0);
	lv_anim_set_duration(&a, CF_FLIP_MS);
	lv_anim_set_completed_cb(&a, card_shrunk_cb);
	lv_anim_start(&a);
}

static void back_veil_clicked_cb(lv_event_t *e) {
	(void)e;
	flip_close(true);
}

// ---------------------------------------------------------------------------
// The sheet
//
// Not a page. It comes up over the Musica grid the way the control centre comes
// down over whatever is underneath, and by the same three calls: the gesture
// moves it a pixel at a time, and only the last stretch after the release is
// animated. Its buffers exist only while it is up.
//
// They come from big_alloc() and not from malloc(), because "exist only while it
// is up" has to mean the kernel gets the pages back. A face is under the C
// library's mmap threshold, so free() would only move it to a free list and the
// resident set would stay where it was. See the note over big_alloc().
// ---------------------------------------------------------------------------

static size_t face_bytes(void) { return (size_t)CF_MID_W * CF_MID_W * 2; }

static size_t warp_bytes(const cf_slot_t *slot) { return (size_t)CF_MID_W * slot->warp_max_h * 3; }

static void buffers_free(void) {
	for (int i = 0; i < CF_SLOTS; i++) {
		cf_slot_t *slot = &slots[i];
		if (slot->requested) {
			coverloader_release(slot_id(i));
			slot->requested = false;
		}
		lv_image_set_src(slot->image, NULL);
		big_free(slot->face, face_bytes());
		big_free(slot->warp, warp_bytes(slot));
		slot->face = NULL;
		slot->warp = NULL;
		slot->warp_h = -1;
		slot->index = -1;
		slot->settled = false;
		slot->has_cover = false;
	}

	// The halo's map goes with them: it belongs to the sheet being up, not to
	// the life of the process, since the player may never open this page.
	if (glow_obj) {
		lv_image_set_src(glow_obj, NULL);
		lv_obj_add_flag(glow_obj, LV_OBJ_FLAG_HIDDEN);
	}
	big_free(glow_map, (size_t)CF_GLOW_MAX_W * CF_GLOW_MAX_H);
	glow_map = NULL;
	glow_map_w = -1;
	glow_map_h = -1;
}

static bool buffers_alloc(void) {
	glow_map = big_alloc((size_t)CF_GLOW_MAX_W * CF_GLOW_MAX_H);
	glow_map_w = -1;
	glow_map_h = -1;
	for (int i = 0; i < CF_SLOTS; i++) {
		cf_slot_t *slot = &slots[i];
		slot->warp_max_h = slot_max_height(i);
		slot->face = big_alloc(face_bytes());
		slot->warp = big_alloc(warp_bytes(slot));
		if (!slot->face || !slot->warp || !glow_map) {
			buffers_free();
			return false;
		}
		slot->index = -1;
		slot->warp_h = -1;
		slot->settled = false;
		slot->requested = false;
		face_reset(slot);
	}
	return true;
}

// Everything the sheet has to have in hand before it becomes visible, whether
// it is being animated in or dragged in a pixel at a time.
static bool sheet_prepare(void) {
	if (sheet_ready) {
		return true;
	}
	if (!library_is_open()) {
		gui_notify_no_card();
		return false;
	}
	if (!buffers_alloc()) {
		return false;
	}

	albums = library_index_open(LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_NONE, NULL, LIBRARY_ORDER_DEFAULT, false);
	album_count = albums ? library_index_count(albums) : 0;
	scroll_max = album_count > 0 ? (int32_t)(album_count - 1) << 16 : 0;
	window_first = -1;
	window_count = 0;
	scroll = 0;
	centre = 0;
	layout_base = INT32_MIN;
	sheet_ready = true;

	relayout();
	refresh_title();
	lv_timer_resume(poll_timer);

	lv_obj_remove_flag(sheet, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(sheet);
	// The clock, battery and volume belong over the sheet, not under it.
	topbar_bring_to_front();
	return true;
}

// Puts everything down. Called when the sheet has finished sliding away, and
// when something else takes the screen from under it.
static void sheet_park(void) {
	lv_anim_delete(sheet, NULL);
	lv_anim_delete(NULL, scroll_anim_cb);
	sheet_open = false;
	lv_obj_set_y(sheet, sheet_h);
	lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);
	if (!sheet_ready) {
		return;
	}
	flip_close(false);
	back_close_index();
	lv_timer_pause(poll_timer);
	buffers_free();
	if (albums) {
		library_index_close(albums);
		albums = NULL;
	}
	album_count = 0;
	window_first = -1;
	window_count = 0;
	layout_base = INT32_MIN;
	sheet_ready = false;
}

static void sheet_anim_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

static void sheet_parked_cb(lv_anim_t *a) {
	(void)a;
	if (!sheet_open) {
		sheet_park();
	}
}

static void sheet_slide_to(int y, bool animate) {
	lv_anim_delete(sheet, sheet_anim_y_cb);
	if (!animate) {
		lv_obj_set_y(sheet, y);
		if (!sheet_open) {
			sheet_park();
		}
		return;
	}
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, sheet);
	lv_anim_set_exec_cb(&a, sheet_anim_y_cb);
	lv_anim_set_values(&a, lv_obj_get_y(sheet), y);
	lv_anim_set_duration(&a, CF_SHEET_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, sheet_parked_cb);
	lv_anim_start(&a);
}

static int drag_from_y;
static bool sheet_dragging;

static bool sheet_drag_begin(void) {
	if (sheet_dragging) {
		return true;
	}
	lv_anim_delete(sheet, sheet_anim_y_cb);
	if (!sheet_open && !sheet_prepare()) {
		return false;
	}
	sheet_dragging = true;
	drag_from_y = sheet_open ? 0 : sheet_h;
	lv_obj_set_y(sheet, drag_from_y);
	return true;
}

static void sheet_drag_update(int dy) {
	if (!sheet_dragging) {
		return;
	}
	int y = drag_from_y + dy;
	if (y < 0) {
		y = 0;
	}
	if (y > sheet_h) {
		y = sheet_h;
	}
	lv_obj_set_y(sheet, y);
}

static void sheet_drag_end(void) {
	if (!sheet_dragging) {
		return;
	}
	sheet_dragging = false;

	// A quarter of the travel decides it, in whichever direction the gesture
	// started: the same point of no return as the control centre.
	int y = lv_obj_get_y(sheet);
	int travel = y - drag_from_y;
	bool open = drag_from_y == 0 ? travel < sheet_h / 4 : travel < -sheet_h / 4;

	sheet_open = open;
	sheet_slide_to(open ? 0 : sheet_h, true);
}

void coverflow_open(void) {
	if (sheet_open) {
		return;
	}
	if (!sheet_prepare()) {
		return;
	}
	sheet_open = true;
	lv_obj_set_y(sheet, sheet_h);
	sheet_slide_to(0, true);
}

// ---------------------------------------------------------------------------
// the gesture on the column
// ---------------------------------------------------------------------------

static void column_drag_cb(lv_event_t *e) {
	static lv_point_t start;
	static int32_t start_scroll;
	static int32_t last_y;
	static uint32_t last_tick;
	static int32_t velocity; // 16.16 pixels per millisecond
	static bool tracking;
	static bool engaged;

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &start);
		lv_anim_delete(NULL, scroll_anim_cb); // a finger on the column stops a throw
		start_scroll = scroll;
		last_y = start.y;
		last_tick = lv_tick_get();
		velocity = 0;
		tracking = album_count > 0 && !flipped && !flipping;
		engaged = false;
		return;
	}
	if (!tracking) {
		return;
	}

	lv_point_t p;
	lv_indev_get_point(indev, &p);
	int dy = p.y - start.y;

	if (code == LV_EVENT_PRESSING) {
		if (!engaged) {
			int adx = p.x - start.x;
			adx = adx < 0 ? -adx : adx;
			int ady = dy < 0 ? -dy : dy;
			if (ady < CF_DRAG_COMMIT_PX || ady <= adx) {
				return;
			}
			engaged = true;
			// The travel that armed the drag is not thrown away, but it must not
			// jump the column either: the position it started from is moved up to
			// meet the finger.
			start_scroll = scroll + (int32_t)(((int64_t)dy << 16) / CF_STEP_PX);
		}

		uint32_t now = lv_tick_get();
		uint32_t elapsed = now - last_tick;
		if (elapsed > 0) {
			// Smoothed over the last few reads: one stray sample at the lift
			// would otherwise decide the whole throw.
			int32_t sample = (int32_t)((((int64_t)(p.y - last_y)) << 16) / (int32_t)elapsed);
			velocity += (sample - velocity) / 2;
			last_y = p.y;
			last_tick = now;
		}

		scroll_set(start_scroll - (int32_t)(((int64_t)dy << 16) / CF_STEP_PX));
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		if (!engaged) {
			// It never moved: a tap. On the middle record that turns it round.
			if (code == LV_EVENT_RELEASED && !player_sheet_drag_active() && !switcher_back_drag_active()) {
				lv_area_t middle;
				lv_obj_get_coords(slots[CF_BEFORE].image, &middle);
				if (p.x >= middle.x1 && p.x <= middle.x2 && p.y >= middle.y1 && p.y <= middle.y2) {
					flip_open();
				}
			}
			return;
		}
		engaged = false;

		if (lv_tick_elaps(last_tick) > 80) {
			velocity = 0; // the finger stopped before it lifted
		}
		int32_t coast = (int32_t)(((int64_t)velocity * CF_FLING_MS) / CF_STEP_PX);
		int32_t limit = CF_FLING_MAX_PLACES * CF_ONE;
		if (coast > limit) {
			coast = limit;
		}
		if (coast < -limit) {
			coast = -limit;
		}
		int32_t target = scroll - coast;
		int32_t rounded = ((target + CF_ONE / 2) >> 16) << 16;
		if (rounded < 0) {
			rounded = 0;
		}
		if (rounded > scroll_max) {
			rounded = scroll_max;
		}

		int32_t travel = rounded > scroll ? rounded - scroll : scroll - rounded;
		if (travel == 0) {
			scroll_idle();
			return;
		}
		uint32_t ms = CF_SETTLE_MS + (uint32_t)((travel * 40) >> 16);
		if (ms > 700) {
			ms = 700;
		}

		lv_anim_t a;
		lv_anim_init(&a);
		lv_anim_set_var(&a, &scroll);
		lv_anim_set_exec_cb(&a, scroll_anim_cb);
		lv_anim_set_values(&a, scroll, rounded);
		lv_anim_set_duration(&a, ms);
		lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
		lv_anim_set_completed_cb(&a, scroll_anim_done_cb);
		lv_anim_start(&a);
	}
}

// ---------------------------------------------------------------------------
// the grip at the top
//
// The way out. The sheet follows the finger down and stays wherever it is let
// go of, exactly as it followed the finger up on the way in.
// ---------------------------------------------------------------------------

static void bar_drag_cb(lv_event_t *e) {
	static lv_point_t start;
	static bool tracking;

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &start);
		tracking = !flipping;
		return;
	}
	if (!tracking) {
		return;
	}

	lv_point_t p;
	lv_indev_get_point(indev, &p);

	if (code == LV_EVENT_PRESSING) {
		if (!sheet_dragging) {
			flip_close(false);
			sheet_drag_begin();
		}
		sheet_drag_update(p.y - start.y);
		return;
	}
	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		sheet_drag_end();
	}
}

// ---------------------------------------------------------------------------
// the pull that brings the sheet up
//
// There is no strip along the bottom of the Musica page: an object pinned to
// the bottom edge would sit over the last few pixels of the two bottom tiles
// and eat the taps that land there. The gesture lives on the grid itself
// instead -- the object the tiles bubble their presses to -- and refuses to
// start unless the finger went down in the bottom band. A tap is untouched
// either way, because nothing happens until the finger has travelled upwards.
// ---------------------------------------------------------------------------

// True from the moment the upward pull is recognised until one tick after the
// gesture's last event. The pull starts on a tile of the Musica grid, and that
// tile gets its CLICKED all the same when the finger lifts: without this the
// sheet comes up with a page opened behind it.
static bool edge_committed;

bool coverflow_drag_active(void) { return edge_committed; }

// Runs from lv_async_call, a tick after RELEASED and the CLICKED that follows
// it: the tap this flag exists to swallow has already been discarded by then.
static void edge_committed_clear_cb(void *unused) {
	(void)unused;
	edge_committed = false;
}

static void edge_drag_cb(lv_event_t *e) {
	static bool tracking;
	static bool armed;
	static lv_point_t start;

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}
	lv_event_code_t code = lv_event_get_code(e);
	int band = (int)(uintptr_t)lv_event_get_user_data(e);

	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &start);
		tracking = coverflow_enabled() && !sheet_open && start.y >= band;
		armed = false;
		return;
	}
	if (!tracking) {
		return;
	}

	lv_point_t p;
	lv_indev_get_point(indev, &p);
	int dy = p.y - start.y;

	if (code == LV_EVENT_PRESSING) {
		if (!armed) {
			int dx = p.x - start.x;
			int adx = dx < 0 ? -dx : dx;
			if (dy > -CF_EDGE_COMMIT_PX || -dy <= adx) {
				return;
			}
			if (!sheet_drag_begin()) {
				tracking = false;
				return;
			}
			armed = true;
			edge_committed = true;
		}
		// The travel that armed it counts, so the sheet is already under the
		// finger rather than a dozen pixels behind it.
		sheet_drag_update(dy);
		return;
	}
	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		if (armed) {
			armed = false;
			sheet_drag_end();
			lv_async_call(edge_committed_clear_cb, NULL);
		}
	}
}

void coverflow_attach_edge(lv_obj_t *grid, gui_config_t *cfg) {
	if (!grid) {
		return;
	}
	void *band = (void *)(uintptr_t)(cfg->screen_height - CF_EDGE_H);
	lv_obj_add_event_cb(grid, edge_drag_cb, LV_EVENT_PRESSED, band);
	lv_obj_add_event_cb(grid, edge_drag_cb, LV_EVENT_PRESSING, band);
	lv_obj_add_event_cb(grid, edge_drag_cb, LV_EVENT_RELEASED, band);
	lv_obj_add_event_cb(grid, edge_drag_cb, LV_EVENT_PRESS_LOST, band);
}

// ---------------------------------------------------------------------------
// building it
// ---------------------------------------------------------------------------

// The theme changed under the page.
//
// The title's colour is set on the object rather than through a style, so it
// does not follow on its own and would leave the album name white on white in
// the light theme. The records are worse than that -- the card is PAINTED into
// every face, so a theme change leaves six squares carrying the old colour.
// They are
// thrown away and asked for again, which is a handful of reads from the
// thumbnail database and happens when somebody changes the theme, not while
// anything is moving.
static void refresh_theme(void) {
	if (title_label) {
		lv_obj_set_style_text_color(title_label, theme()->text_primary, 0);
	}
	if (!sheet_ready) {
		return;
	}
	for (int i = 0; i < CF_SLOTS; i++) {
		if (slots[i].requested) {
			coverloader_release(slot_id(i));
			slots[i].requested = false;
		}
		slots[i].index = -1;
		slots[i].settled = false;
		face_reset(&slots[i]);
	}
	layout_base = INT32_MIN;
	relayout();
	lv_timer_resume(poll_timer);
}

void coverflow_init(gui_config_t *cfg) {
	sheet_h = cfg->screen_height;

	sheet = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(sheet);
	lv_obj_add_style(sheet, &theme_style_screen, 0);
	lv_obj_set_size(sheet, cfg->screen_width, cfg->screen_height);
	lv_obj_set_pos(sheet, 0, sheet_h);
	lv_obj_remove_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);
	// The sheet covers the page underneath: a press that lands on it must not
	// reach through to whatever it is covering.
	lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);

	// The grip. It sits just under the status bar, above everything, and the
	// column is not allowed to reach it.
	bar_hit = lv_obj_create(sheet);
	lv_obj_remove_style_all(bar_hit);
	lv_obj_set_size(bar_hit, CF_BAR_HIT_W, CF_BAR_HIT_H);
	lv_obj_align(bar_hit, LV_ALIGN_TOP_MID, 0, cfg->top_bar_height + CF_BAR_TOP);
	lv_obj_remove_flag(bar_hit, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(bar_hit, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(bar_hit, bar_drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(bar_hit, bar_drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(bar_hit, bar_drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(bar_hit, bar_drag_cb, LV_EVENT_PRESS_LOST, NULL);

	lv_obj_t *bar = lv_image_create(bar_hit);
	lv_image_set_src(bar, &icon_control_center_line);
	lv_obj_add_style(bar, &theme_style_icon, 0);
	lv_obj_set_style_image_opa(bar, LV_OPA_60, 0);
	lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);

	// Everything that scrolls lives in here and is clipped to it, which is how a
	// record on its way past the top edge stops at the grip instead of sliding
	// over it.
	int top = cfg->top_bar_height + CF_BAR_TOP + CF_BAR_HIT_H;
	int bottom = 74; // the album name below the crate
	int height = cfg->screen_height - top - bottom;
	viewport = lv_obj_create(sheet);
	lv_obj_remove_style_all(viewport);
	lv_obj_set_size(viewport, cfg->screen_width, height);
	lv_obj_set_pos(viewport, 0, top);
	lv_obj_remove_flag(viewport, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(viewport, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(viewport, column_drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(viewport, column_drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(viewport, column_drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(viewport, column_drag_cb, LV_EVENT_PRESS_LOST, NULL);

	mid_x = cfg->screen_width / 2;
	// From the numbers, not from the object: a size just set is not a size LVGL
	// has laid out yet, so asking for it here returns zero.
	mid_y = height / 2;

	// The light. Created before the records, so it stays behind all six of them:
	// the part of the halo that falls inside the sleeve has to be covered by the
	// sleeve, or the picture would sit in a coloured fog. Painted and placed per
	// frame in place_glow().
	//
	// An image and not a plain object, and that is not only about how it is
	// drawn. A plain object is clickable the moment it is made, and this one
	// covers the middle of the record, so it would swallow every press landing
	// on the sleeve: the record could not be tapped to turn it over and a drag
	// beginning on it would move nothing. An image is not clickable, so the
	// press reaches the column underneath.
	// The map itself is not made here: it belongs to the sheet being up, and
	// buffers_alloc() asks for it along with the faces.
	glow_obj = lv_image_create(viewport);
	lv_obj_add_flag(glow_obj, LV_OBJ_FLAG_HIDDEN);

	for (int i = 0; i < CF_SLOTS; i++) {
		cf_slot_t *slot = &slots[i];
		slot->index = -1;
		slot->warp_h = -1;
		slot->image = lv_image_create(viewport);
		lv_obj_add_flag(slot->image, LV_OBJ_FLAG_EVENT_BUBBLE);
		lv_obj_add_flag(slot->image, LV_OBJ_FLAG_HIDDEN);
	}

	// Nearest last, so the middle record sits on top of the pile and the ones
	// beside it tuck behind it. Set once: a place never changes its distance
	// from the middle, so re-stacking them on every frame of a drag would be six
	// invalidations a frame for an order that cannot have changed.
	for (int away = CF_SLOTS; away >= 0; away--) {
		for (int i = 0; i < CF_SLOTS; i++) {
			int dist = i - CF_BEFORE;
			if ((dist < 0 ? -dist : dist) == away) {
				lv_obj_move_foreground(slots[i].image);
			}
		}
	}

	// One line, and it travels when the name outgrows it -- the same scrolling
	// label the player, the control centre and the screensaver use, faded at the
	// edge rather than cut off. An album called "Agaetis byrjun" fits; one
	// called "The Rise and Fall of Ziggy Stardust and the Spiders from Mars"
	// does not, and a row of dots is a worse answer than reading it.
	title_label = lv_label_create(sheet);
	lv_obj_set_width(title_label, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(title_label, &font_ui_24, 0);
	scrolltext_apply(title_label);
	lv_obj_align(title_label, LV_ALIGN_BOTTOM_MID, 0, -34);

	empty_label = lv_label_create(sheet);
	lv_label_set_long_mode(empty_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(empty_label, cfg->screen_width - 2 * cfg->padding);
	lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(empty_label, &theme_style_text_dim, 0);
	lv_label_set_text(empty_label, tr("library_empty_note"));
	lv_obj_align(empty_label, LV_ALIGN_CENTER, 0, 0);
	lv_obj_add_flag(empty_label, LV_OBJ_FLAG_HIDDEN);

	// The back of the record. Built once and kept hidden: it is the same card
	// every time, only its contents change.
	back_veil = lv_obj_create(sheet);
	lv_obj_remove_style_all(back_veil);
	lv_obj_set_size(back_veil, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(back_veil, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(back_veil, LV_OPA_60, 0);
	lv_obj_remove_flag(back_veil, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(back_veil, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(back_veil, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(back_veil, back_veil_clicked_cb, LV_EVENT_CLICKED, NULL);

	back_card = lv_obj_create(back_veil);
	lv_obj_add_style(back_card, &theme_style_card, 0);
	lv_obj_set_size(back_card, CF_BACK_W, CF_BACK_H_MIN);
	lv_obj_set_style_radius(back_card, CF_BACK_RADIUS, 0);
	// theme_style_card sets three colours and nothing else, so what LVGL's own
	// default put on the object -- a two-pixel border, a five-pixel radius, an
	// outline on focus -- is still there until it is taken off by hand.
	lv_obj_set_style_border_width(back_card, 0, 0);
	lv_obj_set_style_outline_width(back_card, 0, 0);
	lv_obj_set_style_shadow_width(back_card, 0, 0);
	lv_obj_set_style_pad_all(back_card, 0, 0);
	lv_obj_align(back_card, LV_ALIGN_CENTER, 0, 0);
	lv_obj_remove_flag(back_card, LV_OBJ_FLAG_SCROLLABLE);
	// A tap on the card is not a tap on the veil: the list under it is scrolled
	// with a finger, and every scroll would otherwise close the card.
	lv_obj_remove_flag(back_card, LV_OBJ_FLAG_EVENT_BUBBLE);
	lv_obj_add_flag(back_card, LV_OBJ_FLAG_CLICKABLE);

	// What is printed on the back, at a width that never changes. The card
	// narrows over it and clips it, which is what turning looks like from the
	// front -- and it means the turn does not re-flow the text: with the
	// contents sized to the card, every frame of it re-wrapped the album title
	// and re-laid out twenty-odd rows underneath.
	back_body = lv_obj_create(back_card);
	lv_obj_remove_style_all(back_body);
	lv_obj_set_width(back_body, CF_BACK_BODY_W);
	lv_obj_set_height(back_body, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(back_body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(back_body, 0, 0);
	lv_obj_align(back_body, LV_ALIGN_CENTER, 0, 0);
	lv_obj_remove_flag(back_body, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag(back_body, LV_OBJ_FLAG_EVENT_BUBBLE);

	// The shared style and not a colour of its own. This card is built once at
	// startup and only ever has its text replaced, so a hand-set colour here
	// was the palette of the moment the player booted in -- and stayed that way
	// for the life of the process, while everything around it followed the
	// theme. The album name on the back of the record was white on white.
	back_title = lv_label_create(back_body);
	lv_label_set_long_mode(back_title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(back_title, lv_pct(100));
	lv_obj_add_style(back_title, &theme_style_text, 0);
	lv_obj_set_style_text_font(back_title, &font_ui_24_bold, 0);

	back_sub = lv_label_create(back_body);
	lv_label_set_long_mode(back_sub, LV_LABEL_LONG_DOT);
	lv_obj_set_width(back_sub, lv_pct(100));
	lv_obj_add_style(back_sub, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(back_sub, &font_ui_18, 0);
	lv_obj_set_style_pad_top(back_sub, 2, 0);
	lv_obj_set_style_pad_bottom(back_sub, 12, 0);

	back_list = lv_obj_create(back_body);
	lv_obj_remove_style_all(back_list);
	lv_obj_set_width(back_list, lv_pct(100));
	lv_obj_set_height(back_list, LV_SIZE_CONTENT);
	// Short records make a short card; long ones stop here and scroll.
	lv_obj_set_style_max_height(back_list, CF_BACK_LIST_MAX, 0);
	lv_obj_set_flex_flow(back_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(back_list, 0, 0);
	lv_obj_set_scroll_dir(back_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(back_list, LV_SCROLLBAR_MODE_OFF);

	// One frame, not a couple of hundred milliseconds: while the column is
	// being scrolled this is what decides how long a record entering from an
	// edge stays blank AFTER its cover has already been read. It pauses itself
	// the moment nothing is outstanding, so a crate standing still costs
	// nothing at all.
	poll_timer = lv_timer_create(poll_cb, 33, NULL);
	lv_timer_pause(poll_timer);

	refresh_theme();
	theme_register_refresh(refresh_theme);

	coverloader_start();
}
