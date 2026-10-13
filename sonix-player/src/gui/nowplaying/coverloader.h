#ifndef COVERLOADER_H
#define COVERLOADER_H

#include <stdbool.h>

#include "src/gui/nowplaying/cover.h"

// Artwork loading, off the UI thread.
//
// Decoding a cover takes anything from a fraction of a second to several on
// this hardware, so it must never happen on the interface thread: LVGL would
// sit there until the JPEG came back, and scrolling only makes it worse
// because every newly visible row queues another decode.
//
// Requests go to a worker thread, the interface carries on drawing, and the
// finished picture is collected on the next timer tick.
//
// Requests are addressed by slot rather than by handle. A list has a fixed
// number of rows, each row is one slot, and asking a slot for something new
// abandons whatever it was waiting for -- which is exactly what scrolling does.

// Browser rows use slots 0..9; the library index pages (medialist.c) use
// 12..23 for the name lists and 24..35 for the track lists; the search page
// takes 36..59 -- its twelve track hits and twelve album hits; the audiobook
// list takes 60..71 for its twelve rows; one artist's records take 72..83; the
// album carousel takes 84..89; and the cover at the head of an album's tracks
// takes 90; the podcasts on the card take 91..102 for their twelve rows, 103
// for the cover at the head of one of them and 104..107 for the episodes being
// listened to -- so the pools never trample each other's requests.
#define COVERLOADER_SLOTS 108
#define COVERLOADER_SEARCH_BASE 36
#define COVERLOADER_SEARCH_COUNT 24
#define COVERLOADER_AUDIOBOOKS_BASE 60

// The album carousel's six places take 84..89 -- five that can be seen and one
// that is always just off an edge, because the column scrolls through the
// positions in between and not only from one record to the next. It asks for
// bigger pictures than any list does -- 210 px against the lists' 72 -- but
// never more than six at a time, and only while its page is the one on screen.
#define COVERLOADER_COVERFLOW_BASE 84

// The big cover above an album's tracks, when the album page is laid out with
// one (Music > Library). One picture, the size of most of the
// screen's width.
#define COVERLOADER_ALBUM_HEAD 90

// Starts the worker. Safe to call more than once.
void coverloader_start(void);

// Asks for the artwork of `path` at `size` pixels, for `slot`. Returns
// immediately. Any earlier request on this slot is abandoned.
void coverloader_request(int slot, const char *path, int size);

// The same, with the picture made from the player's own cover of the file at
// player_w x player_h and scaled to `size` (cover_load_shared): the player's
// table of covers serves it, and is filled by it. What an album's head asks
// with.
void coverloader_request_shared(int slot, const char *path, int size, int player_w, int player_h);

// Forgets the slot: anything pending is abandoned and anything already decoded
// for it is freed.
void coverloader_release(int slot);

// Collects a finished image. Returns false while the worker is still busy, or
// if there was no artwork. `found` says which of the two it was, so a caller
// can stop asking.
bool coverloader_take(int slot, cover_image_t *out, bool *finished);

// ---------------------------------------------------------------------------
// The player's artwork -- the big cover and its blurred backdrop.
//
// A track change means megabytes read off the card, a full JPEG decode and two
// blur passes, which on the X1600E is seconds of work -- and track changes also
// happen on their own when a song ends, so it can never be done on the UI
// thread.
//
// One job, newest wins: asking again abandons whatever was in flight.
// ---------------------------------------------------------------------------

// Asks for the playing track's cover (cover_w x cover_h, CONTAIN) and blurred
// backdrop (backdrop_w x backdrop_h). Returns immediately.
void coverloader_request_player(const char *path, int cover_w, int cover_h, int backdrop_w, int backdrop_h);

// Collects the finished pair. `finished` goes true exactly once per request;
// the return value says whether there was artwork. The backdrop can be empty
// even when the cover is not.
bool coverloader_take_player(cover_image_t *cover, cover_image_t *backdrop, bool *finished);

// Asks only WHAT the artwork of `path` is, without decoding any of it: the
// answer is cover_source_id()'s number, and the player compares it with the one
// it already has on screen. A track whose picture is the one already up costs
// this and nothing else -- no decode, no resample, no blur,
// and a cover that does not blink.
//
// Its own job, not a mode of the one above: the two are asked in sequence for
// the same track, and a job that could be either would have the second request
// cancel the first.
void coverloader_request_player_id(const char *path);

// Collects that answer. `finished` goes true exactly once per request; `id` is
// 0 when the track has no artwork at all.
bool coverloader_take_player_id(uint64_t *id, bool *finished);

// Makes the player's cover of `path` at cover_w x cover_h and keeps it in the
// cache (cover_prefetch_player()), for the track that is likely to come next.
// Nothing comes back. The lowest priority there is: it waits for every other
// job, and a newer request replaces one not yet started.
void coverloader_prefetch_player(const char *path, int cover_w, int cover_h);

// ---------------------------------------------------------------------------
// The screensaver's photograph -- the full-screen picture and the blurred block
// over its lower part.
//
// Same job, same reason. The wake hook runs with the panel still blank and the
// interface thread waiting, so nothing may be decoded there: the picture is
// asked for while the screen is dark and collected at the wake, leaving the
// wake itself a pointer swap.
// ---------------------------------------------------------------------------

// Asks for `path` at w x h and its blurred strip at strip_w x strip_h. Returns
// immediately; any earlier screensaver request is abandoned.
void coverloader_request_screensaver(const char *path, int w, int h, int strip_w, int strip_h);

// Collects the finished pair. `finished` goes true exactly once per request.
bool coverloader_take_screensaver(cover_image_t *image, cover_image_t *strip, bool *finished);

// Abandons anything in flight and frees anything already decoded for it.
void coverloader_release_screensaver(void);

#endif /* COVERLOADER_H */
