#ifndef COVER_H
#define COVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

// A cover picture already decoded and scaled to the size it is drawn at, in
// the display's own pixel format, so drawing it costs nothing more than a blit.
typedef struct {
	lv_image_dsc_t dsc;	 // hand this straight to lv_image_set_src()
	uint8_t *pixels;	 // pixel buffer referenced by dsc.data
} cover_image_t;

// Where the on-disk thumbnail cache lives -- normally a hidden folder on the
// card, which is what the stock player does too (it keeps its own JPEGs under
// .hiby/.../cache/). Call this once at startup, before the browser opens.
// Passing NULL, or a directory that cannot be written to, falls back to /tmp.
void cover_set_cache_dir(const char *sd_root);

// How a picture is mapped onto its box.
typedef enum {
	COVER_FIT_CONTAIN, // whole picture visible, letterboxed, never upscaled
	COVER_FIT_COVER,   // fills the box edge to edge, centre-cropping the rest
} cover_fit_t;

// Decodes the cover art of a track (embedded picture first, then a cover file
// in its folder) into a box_w x box_h box.
bool cover_load_for_file(const char *filepath, int box_w, int box_h, cover_fit_t fit, cover_image_t *out);

// Same for a folder: only looks at cover.jpg / folder.png / ... inside it.
bool cover_load_for_dir(const char *dirpath, int box_w, int box_h, cover_fit_t fit, cover_image_t *out);

// A picture given by path, with no track and no folder to search: the file
// itself is the artwork. AirPlay is the caller -- shairport writes what the
// phone sent to /tmp/cover-<md5>.jpg and hands over that path.
bool cover_load_image_file(const char *path, int box_w, int box_h, cover_fit_t fit, cover_image_t *out);

// The same from bytes already in memory, for a picture that has no path of its
// own: the cover of an EPUB, which lives inside the book's ZIP and would have
// to be written to the card first for the call above to reach it.
bool cover_load_image_memory(const void *data, size_t size, int box_w, int box_h, cover_fit_t fit,
							 cover_image_t *out);

// What the artwork for this file IS, as one number, without decoding it.
//
// The player asks this before it throws a cover away: two tracks of the same
// album carry the same picture, and there is no reason to decode it, resample
// it and blur it again for the second one -- which is a second of work on this
// processor and a cover that blinks out and back for no change at all.
//
// The answer is about the BYTES and not about the folder or the album tag, so
// it is right in the cases a tag cannot settle: a picture embedded identically
// in every track of a record, a cover.jpg that all the tracks of a folder fall
// back to, and a compilation whose tracks really do carry different pictures.
// Whichever source would answer for the artwork is the one hashed, so a
// picture that cannot be decoded compares as itself and both tracks fall back
// to the same place.
//
// 0 means there is no artwork at all. Cheap: it reads the picture but never
// decodes it, and hashes its size with a slice of each end.
uint64_t cover_source_id(const char *filepath);

// Decodes the artwork once and builds both images the player screen needs: the
// artwork itself, and the upside-down blurred copy drawn behind the controls.
// Either output may be skipped by passing NULL for its size. Returns false if
// the track has no artwork at all, leaving both outputs zeroed.
bool cover_load_player_images(const char *filepath, int cover_w, int cover_h, cover_image_t *cover_out,
							  int backdrop_w, int backdrop_h, cover_image_t *backdrop_out);

// The two pictures the screensaver needs from one photograph: the picture
// itself filling the screen, and the blurred block along the bottom that the
// clock and the track name sit on.
//
// Not cover_load_player_images() with different numbers: there the blurred
// block sits BELOW the artwork and is a mirrored centre crop of it, while here
// it sits ON TOP of a picture that fills the whole screen, so it has to be the
// bottom band of that same picture, blurred and the right way up. Anything
// else reads as a second photograph laid over the first.
//
// One decode feeds both. False when the file cannot be read or decoded, with
// both outputs zeroed.
bool cover_load_screensaver_images(const char *path, int w, int h, cover_image_t *image_out, int strip_w,
								   int strip_h, cover_image_t *strip_out);

// A blurred, dimmed copy of a picture already scaled and packed, the same size
// as the original. What the sleeve turns into when the VU meters are opened
// over it, treated exactly like the block behind the controls so the two read
// as one surface. The caller owns `out` and must cover_free() it.
bool cover_blur_copy(const cover_image_t *src, cover_image_t *out);

// The one colour that stands for a sleeve, as 0xRRGGBB. Cover Flow throws it
// under the record in the middle; the player's alternative layout tints the
// title pill, the waveform and the star with it. A grey sleeve comes back a
// warm white rather than a colour squeezed out of rounding noise. Zero when
// there is nothing to look at.
uint32_t cover_dominant_tone(const cover_image_t *cover);

void cover_free(cover_image_t *img);

// How the blurred backdrop is toned down so text stays readable on top of it:
// a dark UI darkens the picture, a light one washes it out towards white.
// Takes effect the next time a backdrop is built.
void cover_set_backdrop_light(bool light);

// Whether the blurred backdrop is built the right way up.
//
// Behind the controls it is turned over: it stands below the sleeve and reads
// as its reflection. Filling the screen with the sleeve shown on top of it, it
// has to face the same way the sleeve does. Takes effect the next time a
// backdrop is built; the screensaver's bottom band is never flipped either way.
void cover_set_backdrop_upright(bool upright);

// ---------------------------------------------------------------------------
// Browser thumbnails
//
// The browser owns each thumbnail and frees it the moment its row leaves the
// screen, so the memory held by artwork is proportional to what is visible and
// not to how far the user has scrolled. Reloading is cheap because the scaled
// thumbnail is kept on the card (see cover_set_cache_dir): a row coming back
// into view is a few-kilobyte read, not a decode.
//
// For a track, the folder's artwork is looked up first -- every file in an
// album resolves to the same picture, so they all share one cache entry.
// ---------------------------------------------------------------------------

// Loads a square thumbnail. The caller owns `out` and must cover_free() it.
// Returns false when there is no artwork for this path.
bool cover_thumb_load(const char *path, bool is_dir, int size, cover_image_t *out);

// Like cover_thumb_load, but only looks in the on-disk thumbnail cache --
// never decodes. For pages that want covers instantly or not at all: the queue
// list builds a hundred rows in one go, where a hundred small cache reads are
// affordable and a hundred decodes are not.
bool cover_thumb_cached(const char *path, int size, cover_image_t *out);

#endif // COVER_H
