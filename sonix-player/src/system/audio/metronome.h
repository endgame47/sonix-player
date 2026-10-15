#ifndef METRONOME_H
#define METRONOME_H

#include <stdbool.h>
#include <stdint.h>

// The metronome's sound: a click on every beat, the first of each bar higher
// and louder, synthesised here and written to the external PCM (audio.h).
// Never together with music: it does not start over anything playing, and a
// track, a station or a receiver started while it clicks takes the output and
// stops it.
//
// The beats are counted in samples, not on a timer, so the tempo holds for as
// long as the DAC's clock does. A change of tempo applies from the beat that
// is sounding: the next one falls one new interval after it.

#define METRONOME_BPM_MIN 30
#define METRONOME_BPM_MAX 250
#define METRONOME_BEATS_MAX 7

// Starts the thread that opens the output and clicks, and returns at once.
// False when it is already running or the thread cannot be made.
bool metronome_start(void);

// Stops the clicking and hands the output back. Waits for the thread.
void metronome_stop(void);

bool metronome_running(void);

// Any thread. Clamped to the ranges above.
void metronome_set_bpm(int bpm);
int metronome_bpm(void);

// Beats in a bar, 1 to METRONOME_BEATS_MAX; with 1 no beat is accented. A
// change starts a new bar on the next beat.
void metronome_set_beats(int beats);
int metronome_beats(void);

// The beat being heard now, for drawing it: its place in the bar from 0, how
// many beats have sounded since the start (so a pendulum knows which way it is
// going), and how far into it the listener is, 0 to 1 of the interval. The
// time is when the click reaches the output, not when it was written, so the
// picture keeps step with the sound. False while nothing has sounded yet.
bool metronome_position(int *beat, uint32_t *count, float *phase);

#endif /* METRONOME_H */
