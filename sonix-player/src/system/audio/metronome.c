#include "metronome.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/system/audio/audio.h"
#include "src/system/core/utils.h"
#include "src/system/streaming/radio.h"

#define RATE 44100
#define CHUNK 441 // 10 ms
// Short, so the click is heard when the pendulum says it is: the output is
// opened with this much buffer, and every timestamp below is shifted by it.
#define BUFFER_MS 46

// Each click: a bright tone and its octave, struck in a millisecond and dying
// away in a few more, faded to nothing at the end. The first beat of a bar a
// fifth higher and louder.
#define CLICK_FRAMES (RATE * 60 / 1000)
#define CLICK_ATTACK_S 0.001
#define CLICK_FADE_FRAMES (RATE * 5 / 1000)
#define STRONG_HZ 1760.0
#define WEAK_HZ 1320.0

// Each open waits out a busy device for two seconds (audio.c); a paused track
// lets go of it within five.
#define OPEN_TRIES 3

// The beats the page may still ask about: enough for the output's buffer.
#define MARKS 16

typedef struct {
	uint64_t heard_us; // when the click reaches the output, CLOCK_MONOTONIC
	uint32_t interval_us;
	uint32_t count;
	int beat;
} mark_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int bpm_now = 100;
static int beats_now = 4;
static bool bar_restart;
static mark_t marks[MARKS];
static int marks_count;
static int marks_head; // next slot to write

static pthread_t thread;
static bool live;			// a thread exists and has not been joined
static volatile bool done;	// it has returned
static volatile bool stop_flag;

static int16_t click_strong[CLICK_FRAMES];
static int16_t click_weak[CLICK_FRAMES];

static uint64_t now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void synth_click(int16_t *out, double hz, double level, double decay_ms) {
	for (int i = 0; i < CLICK_FRAMES; i++) {
		double t = (double)i / RATE;
		double env = (t < CLICK_ATTACK_S ? t / CLICK_ATTACK_S : 1.0) * exp(-t * 1000.0 / decay_ms);
		if (i >= CLICK_FRAMES - CLICK_FADE_FRAMES) {
			env *= (double)(CLICK_FRAMES - i) / CLICK_FADE_FRAMES;
		}
		double tone = (sin(2.0 * M_PI * hz * t) + 0.35 * sin(4.0 * M_PI * hz * t)) / 1.35;
		out[i] = (int16_t)lrint(32767.0 * level * env * tone);
	}
}

static void publish(const mark_t *m) {
	pthread_mutex_lock(&lock);
	marks[marks_head] = *m;
	marks_head = (marks_head + 1) % MARKS;
	if (marks_count < MARKS) {
		marks_count++;
	}
	pthread_mutex_unlock(&lock);
}

// Opens the output, then writes ten milliseconds at a time: silence, with a
// click wherever a beat falls. Paced by the PCM: every write blocks until there
// is room. An output that never makes it wait is paced by the clock instead, so
// the loop cannot run away and the beats keep time.
static void *clicker_main(void *unused) {
	(void)unused;
	// The page does not start it over anything playing (metronomepage.c), and
	// whatever started in the meantime has the output.
	if (audio_get_status() == AUDIO_STATUS_PLAYING || radio_is_playing() || radio_is_connecting()) {
		printf("metronome: something started playing; not starting\n");
		done = true;
		return NULL;
	}
	// A paused track keeps its place, and keeps the DAC for a few seconds
	// after the pause (audio.c): it is waited out rather than stopped.
	bool open = false;
	for (int tries = 0; tries < OPEN_TRIES && !stop_flag && !open; tries++) {
		open = audio_external_begin_yielding(RATE, 2, 16, BUFFER_MS);
	}
	if (!open) {
		if (!stop_flag) {
			fprintf(stderr, "metronome: no PCM; nothing to click on\n");
		}
		done = true;
		return NULL;
	}
	thread_be_realtime("metronome", 10);

	int16_t out[CHUNK * 2];
	uint64_t frame = 0;
	// When frame 0 would be heard, less one buffer.
	uint64_t origin_us = now_us();
	double last_beat = 0;
	bool any = false;
	int bar_pos = 0;
	uint32_t count = 0;
	const int16_t *click = NULL;
	int click_at = 0;

	while (!stop_flag) {
		if (audio_external_wanted()) {
			// A track, a station or a receiver started: it has the output.
			printf("metronome: something else wants the output; stopping\n");
			break;
		}
		pthread_mutex_lock(&lock);
		double interval = (double)RATE * 60.0 / bpm_now;
		int beats = beats_now;
		bool restart = bar_restart;
		bar_restart = false;
		pthread_mutex_unlock(&lock);
		if (restart) {
			bar_pos = 0;
		}

		mark_t found[CHUNK / 8 + 1];
		int offsets[CHUNK / 8 + 1];
		int found_count = 0;
		for (int f = 0; f < CHUNK; f++) {
			double at = (double)(frame + (uint64_t)f);
			if (!any || at >= last_beat + interval) {
				// On the grid when the beat is the one expected; from here
				// when the tempo has just gone up past where the grid was.
				last_beat = any && at - (last_beat + interval) < 1.0 ? last_beat + interval : at;
				any = true;
				int beat = bar_pos % beats;
				click = beats > 1 && beat == 0 ? click_strong : click_weak;
				click_at = 0;
				if (found_count < (int)(sizeof(found) / sizeof(found[0]))) {
					found[found_count] = (mark_t){0, (uint32_t)(interval * 1e6 / RATE), count, beat};
					offsets[found_count] = f;
					found_count++;
				}
				bar_pos = (beat + 1) % beats;
				count++;
			}
			int16_t s = 0;
			if (click) {
				s = click[click_at++];
				if (click_at >= CLICK_FRAMES) {
					click = NULL;
				}
			}
			out[f * 2] = s;
			out[f * 2 + 1] = s;
		}

		uint64_t before = now_us();
		int written = audio_external_write(out, CHUNK);
		uint64_t written_at = now_us();
		frame += CHUNK;
		uint64_t frame_us = frame * 1000000u / RATE;
		if (written > 0 && written_at - before >= 1000) {
			// The output made the write wait: it is keeping time, and the
			// clock below follows it.
			origin_us = written_at - frame_us + (uint64_t)BUFFER_MS * 1000u;
		} else if (origin_us + frame_us > written_at + (uint64_t)BUFFER_MS * 1000u) {
			// An output that takes everything at once, or none at all: real
			// time is kept here instead, one buffer ahead at most.
			uint64_t lead = origin_us + frame_us - written_at - (uint64_t)BUFFER_MS * 1000u;
			usleep((useconds_t)lead);
			written_at += lead;
		}
		// The write returned with the buffer full again, so the last frame of
		// the chunk is heard one buffer from now, and the others before it.
		for (int i = 0; i < found_count; i++) {
			found[i].heard_us =
				written_at + (uint64_t)BUFFER_MS * 1000u - (uint64_t)(CHUNK - offsets[i]) * 1000000u / RATE;
			publish(&found[i]);
		}
	}

	audio_external_end();
	done = true;
	return NULL;
}

bool metronome_start(void) {
	if (live && !done) {
		return false;
	}
	metronome_stop();

	synth_click(click_strong, STRONG_HZ, 0.80, 14.0);
	synth_click(click_weak, WEAK_HZ, 0.55, 10.0);

	pthread_mutex_lock(&lock);
	marks_count = 0;
	marks_head = 0;
	bar_restart = true;
	pthread_mutex_unlock(&lock);

	stop_flag = false;
	done = false;
	if (pthread_create(&thread, NULL, clicker_main, NULL) != 0) {
		done = true;
		return false;
	}
	live = true;
	printf("metronome: started at %d bpm, %d beats in a bar\n", metronome_bpm(), metronome_beats());
	return true;
}

void metronome_stop(void) {
	if (!live) {
		return;
	}
	stop_flag = true;
	pthread_join(thread, NULL);
	live = false;
	printf("metronome: stopped\n");
}

bool metronome_running(void) { return live && !done; }

void metronome_set_bpm(int bpm) {
	if (bpm < METRONOME_BPM_MIN) {
		bpm = METRONOME_BPM_MIN;
	} else if (bpm > METRONOME_BPM_MAX) {
		bpm = METRONOME_BPM_MAX;
	}
	pthread_mutex_lock(&lock);
	bpm_now = bpm;
	pthread_mutex_unlock(&lock);
}

int metronome_bpm(void) {
	pthread_mutex_lock(&lock);
	int bpm = bpm_now;
	pthread_mutex_unlock(&lock);
	return bpm;
}

void metronome_set_beats(int beats) {
	if (beats < 1) {
		beats = 1;
	} else if (beats > METRONOME_BEATS_MAX) {
		beats = METRONOME_BEATS_MAX;
	}
	pthread_mutex_lock(&lock);
	if (beats != beats_now) {
		beats_now = beats;
		bar_restart = true;
	}
	pthread_mutex_unlock(&lock);
}

int metronome_beats(void) {
	pthread_mutex_lock(&lock);
	int beats = beats_now;
	pthread_mutex_unlock(&lock);
	return beats;
}

bool metronome_position(int *beat, uint32_t *count, float *phase) {
	uint64_t now = now_us();
	bool found = false;
	pthread_mutex_lock(&lock);
	// Newest first: the latest beat the listener has already heard.
	for (int i = 1; i <= marks_count && !found; i++) {
		const mark_t *m = &marks[(marks_head - i + MARKS) % MARKS];
		if (m->heard_us > now) {
			continue;
		}
		float p = m->interval_us ? (float)(now - m->heard_us) / (float)m->interval_us : 0.0f;
		*beat = m->beat;
		*count = m->count;
		*phase = p > 1.0f ? 1.0f : p;
		found = true;
	}
	pthread_mutex_unlock(&lock);
	return found;
}
