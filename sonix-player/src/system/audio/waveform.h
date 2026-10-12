#ifndef WAVEFORM_H
#define WAVEFORM_H

#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// The shape of a track, for the player's alternative layout to draw instead of
// a progress bar.
//
// One byte per column, 0..255, how loud that slice of the track is on average
// -- not its loudest sample, which on any modern master is full scale in every
// slice and draws a rectangle.
//
// Forty-eight columns across a 480 px panel is six pixels and a gap each. Many
// more than that and a bar is thinner than the rounding on everything else in
// the player, and the whole strip reads as hatching rather than as a shape.
//
// Where it comes from: a worker decodes the file once, at the lowest priority
// the scheduler has, and the answer is kept in a file on the card so it is only
// ever done once per track. The columns are handed out as the decode finishes
// them, so the shape fills in from the left while the track plays. Where a
// column is not ready the caller draws whatever it draws for "not known yet"
// -- there is no placeholder here, because a flat line and a quiet track look
// the same and only the caller knows which it is showing.
//
// The decode is real work and the device has one core, so the worker is
// deliberately timid: SCHED_IDLE, one track at a time, and dropped the moment
// the track changes. It also waits for the audio side to be left alone for a
// few seconds before it reads anything, and stops when a track starts or a PCM
// opens: a whole file read off the card at that moment takes the free memory a
// USB DAC's driver is allocating its buffers from. And it hands the file's
// pages back to the kernel behind itself, so the read fills no cache.
// It does NOT stop when the screen goes dark -- that is the
// one moment nothing else wants the core, and the answer is kept, so the work
// done then is work nobody ever waits for.
// ---------------------------------------------------------------------------

#define WAVEFORM_BARS 48

// Where waveform.dat lives -- normally .local on the card. Call once at
// startup, before anything asks for a waveform. A directory that cannot be
// written to leaves the cache off: everything still works, it is just computed
// again next time.
void waveform_set_cache_dir(const char *sd_root);

// Starts the worker. Safe to call more than once.
void waveform_start(void);

// The track the player is showing. Answers from the cache at once when it has
// it, and otherwise puts the worker to work on it. Calling it with a different
// path abandons whatever was being computed: nobody is going to look at the old
// one.
//
// Returns how many columns of `out` are filled, from the left: WAVEFORM_BARS
// for the whole shape, fewer while the worker is still reading the file -- the
// columns done so far, scaled against the loudest of them, so the page can draw
// the shape as it is worked out -- and 0 when nothing is known yet or the file
// has no shape.
//
// `out` is WAVEFORM_BARS bytes. Safe to call from the interface thread; it does
// no file work of its own once the cache has been read.
int waveform_get(const char *path, uint8_t *out);

// Whether the worker is busy on `path` right now, so the page can say "working
// on it" rather than "there is nothing here".
bool waveform_pending(const char *path);

// Told when the screen goes dark or comes back. It does NOT stop the worker: a
// shape is worked out for the file, not for the moment, and is kept in
// waveform.dat, so a dark screen is the best time there is to compute one --
// the interface is not drawing and an idle-class thread finally gets the core.
// All this does is record the state and nudge the worker on the way back.
void waveform_set_screen_on(bool on);

#endif // WAVEFORM_H
