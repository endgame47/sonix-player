#include "audio.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/decode/decode.h"
#include "src/system/decode/growfile.h"
#include "src/system/audio/replaygain.h"
#include "src/system/audio/speed.h"
#include "src/system/audio/swvolume.h"
#include "src/system/audio/eq.h"
#include "src/system/core/config.h"
#include "src/system/device/power.h"
#include "src/system/core/utils.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>

#include <sched.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <alloca.h>
#include <alsa/asoundlib.h>
#include <time.h>

// Millisecond timestamps for the play/start/end log lines, so the ordering and
// the gaps between them can be read back.
static long log_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


// The host build has no sound card: ALSA falls back to a null device that
// swallows a whole track in a few milliseconds, so playback there is over
// before it starts and states like "paused" are unreachable. Pacing the writes
// to real time makes the simulator behave like a device for anything that
// depends on timing.
#ifdef HOST_BUILD
static void host_pace(snd_pcm_sframes_t frames, int rate) {
	if (frames > 0 && rate > 0) {
		usleep((useconds_t)((frames * 1000000.0) / rate));
	}
}
#else
#define host_pace(frames, rate) ((void)0)
#endif

// Stops a stream for a pause, and says how many bytes of already-decoded audio
// were thrown away with it.
//
// The stream is DROPPED, not paused. This driver does not survive
// snd_pcm_pause(): a track left paused and then skipped wedges the playback
// thread inside ALSA until the player is restarted. The stock player cannot
// pause the hardware either -- it runs on tinyalsa, which has no pause call at
// all, and simply stops feeding the device.
//
// The cost of dropping is that whatever the DAC had buffered (up to ~0.7 s with
// the deep buffer here) is discarded unheard, so the caller winds its source
// back by the returned amount.
// One per turn of a decode loop, for the watchdog.
//
// The playback thread runs SCHED_RR, and this device has one core: a turn that
// never blocks starves everything else, the interface included, and the log
// stops because nothing else is scheduled to write to it. From outside that is
// a frozen player with nothing in the log -- and from the watchdog's side a
// thread in state R says only "running", not whether it is making progress.
// This number says which: racing away while the interface is stalled is a spin,
// standing still is a thread stuck in a call that never came back.
static volatile unsigned loop_turns;

unsigned audio_loop_turns(void) { return loop_turns; }

static int64_t pause_stop_stream(snd_pcm_t *pcm, int frame_bytes) {
	// No stream is a fair answer, not a fault: the device is handed back after
	// a few seconds of pause and this is called on the way into pause. Alsa
	// would answer a null handle with an assert, and an abort in here costs the
	// whole audio thread.
	if (!pcm) {
		return 0;
	}

	snd_pcm_sframes_t queued = 0;
	if (snd_pcm_delay(pcm, &queued) < 0 || queued < 0) {
		queued = 0; // buffered amount unknown: resume a touch late
	}

	snd_pcm_drop(pcm);

	fprintf(stderr, "audio[%ld]: paused (stream dropped, %ld frames unplayed)\n", log_ms(), (long)queued);
	return (int64_t)queued * frame_bytes;
}

// Resuming from pause must cope with whatever happened to the PCM while it sat
// paused: a long screen-off can leave it SUSPENDED (or dropped back to SETUP),
// and a write to a device still stuck in PAUSED blocks forever with no error.
// Whatever the state, this leaves the stream RUNNING or freshly PREPARED so the
// next write starts it.
static void pcm_restart_after_pause(snd_pcm_t *pcm) {
	snd_pcm_state_t st = snd_pcm_state(pcm);

	// A system suspend can leave the device suspended underneath; that has to
	// be undone before anything else will work.
	if (st == SND_PCM_STATE_SUSPENDED) {
		int err = snd_pcm_resume(pcm);
		int tries = 0;
		while (err == -EAGAIN && tries++ < 10) {
			usleep(100 * 1000);
			err = snd_pcm_resume(pcm);
		}
	}

	// The stream was DROPPED when the pause began (see the pause branch), so
	// there is nothing to un-pause: it just needs preparing again before the
	// next write.
	int err = snd_pcm_prepare(pcm);
	fprintf(stderr, "audio[%ld]: restarting after pause (state %d, prepare %s)\n", log_ms(), (int)st,
			err < 0 ? snd_strerror(err) : "ok");
}


typedef struct {
	int channels;
	int sample_rate;
	int bits_per_sample;
	long data_offset;
	long data_size;
} wav_info_t;

// The ALSA device playback opens. "default" is the DAC behind the jacks;
// Bluetooth swaps in a bluealsa PCM name so the stream goes to the
// headphones instead. Read under the mutex, because it is set from the
// interface thread while the playback thread may be about to open a file.
// It gets a lock of its own rather than sharing audio_mutex: open_pcm_device()
// runs on the playback thread at points where that mutex is already held, and
// a second acquisition there would be a deadlock rather than a bug report.
#define AUDIO_DEFAULT_PCM "default"
static char output_pcm[160] = AUDIO_DEFAULT_PCM;
static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t playback_thread;
static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t audio_cond = PTHREAD_COND_INITIALIZER;

static audio_command_t audio_command = AUDIO_CMD_NONE;
static audio_status_t playback_status = AUDIO_STATUS_STOPPED;
static char current_filepath[512] = {0};
static bool play_request = false;
// When the pending play request was made. The playback thread clears it as it
// picks the request up, so a value that stops moving is a thread that has
// stopped coming back for work -- which is what a stall looks like from
// outside: the interface answers, the request is logged, and no track starts.
// Read by the watchdog in main.c.
static long play_request_ms;
static bool write_fail_retry_used = false; // one automatic track retry per play request

// Set by audio_notify_resume(): a system suspend leaves an open stream in
// SND_PCM_STATE_SUSPENDED, and a paused track keeps its stream open across
// the whole standby. The playback thread clears the flag by putting the
// device back on its feet, so nothing later meets a suspended handle.
static bool resume_recover_pending = false;

// Raised by the suspend path: as soon as the playback thread sees it, it
// closes the PCM the gapless path is holding instead of waiting for it to
// time out.
static volatile bool hold_flush_request = false;

// True while play_file() is running -- including while paused. This is the
// distinction "playback_status == STOPPED" cannot make: the status changes as
// soon as the command arrives, but the track context (open decoder, FILE* on
// the card, thread inside the track loop) only dies when play_file() returns.
// The suspend path waits on this, not on the status: the microSD loses power
// in mem, and a descriptor left open across it leads to the reboot on the
// first play afterwards.
static volatile bool playback_context_active = false;

// Fresh restart after mem: the first play is not an unpause. The stock binary
// has a dedicated flag (player+0xBB8, set_param(0x1e,1) just before mem) that
// the next play consumes to rebuild the track instead of resuming a
// pre-suspend state. Same idea here: path and position are saved before the
// freeze, and the next audio_resume() turns them into an audio_play_at() --
// fresh decoder, fresh output route, fresh PCM.
static bool restart_fresh_pending = false;
static char restart_fresh_path[512];
static double restart_fresh_pos;

// ---------------------------------------------------------------------------
// Re-initialising the output route after waking from mem.
//
// mem powers the HBC3000 (the FPGA audio bridge) down; its kernel resume is a
// no-op (plat_data+0x48 == 0), so the hardware stays off -- but the software
// route ("Output Port Switch") survives in kernel RAM. On the first play
// auto_set_output() asks for the same route X as before mem: the machine driver
// callback sees X==X, takes the "no change" shortcut and never reaches
// hbc3000_enable() (the chip's real power-on/program-RAM step). hbc3000_start()
// then runs on dead hardware, the AIC reset fails and the device reboots.
//
// So after wake and before opening any PCM, a real X -> Y -> X transition is
// forced on the actual ALSA control. Two different values make the kernel run
// the full route change, which goes through hbc3000_enable() and powers the
// chip back up. Nothing is listened to on Y: it is only the way to traverse the
// re-init branch.
//
// Y has to be a different PORT, not merely a different number. Routes 1 and 2
// are the two faces of the 3.5 mm socket and the driver reconfigures nothing
// moving between them (see write_output_route in alsa-controls.c), so a Y of 2
// for an X of 1 would traverse nothing and leave the HBC3000 off. There are two
// ports, so the choice is between the balanced one and either of the
// single-ended pair.
void audio_force_output_reinit_after_resume(void) {
	// 1 = 3.5 mm line out, 2 = 3.5 mm headphone, 3 = 4.4 mm balanced.
	int x = detect_output();
	int y = output_reinit_partner(x);
	fprintf(stderr, "audio: output re-init after resume, %d -> %d -> %d\n", x, y, x);
#ifndef HOST_BUILD
	alsa_set_control("Output Port Switch", y);
	usleep(120 * 1000); // settle: how long the driver takes over a route change
	alsa_set_control("Output Port Switch", x);
	usleep(120 * 1000);

	// X was written by hand, bypassing the alsa-controls.c cache: realign it so
	// the play's first auto_set_output() sees X==X and does not touch the route
	// again with a stream already coming.
	alsa_controls_note_output(x);
#endif
}

// True while the playback thread holds an open PCM handle. The suspend path
// waits on this: no ALSA object may be alive when the SoC stops.
static volatile bool pcm_device_open = false;

// How long a paused stream may keep the device -- and with it the CS43198 and
// the amplifier -- powered before it is handed back. The usual way to leave the
// player is a track paused, and holding the whole analogue chain up for hours
// is the bulk of the standby drain. The stock player closes its device whenever
// it is not feeding it, which is why its "pop" is a familiar sound on these
// players.
#define PAUSED_DEVICE_CLOSE_MS 5000

// True when the stream is going out over Bluetooth rather than to the DAC.
static bool output_is_bluetooth(void) {
	char device[sizeof(output_pcm)];
	audio_get_output_device(device, sizeof(device));
	return strncmp(device, "bluealsa", 8) == 0;
}

// ...and over Bluetooth the wait is zero.
//
// bluealsa's PCM is not a sound card. The A2DP transport is acquired when the
// stream starts and released the moment it is dropped, and snd_pcm_prepare()
// does not take it back: writes after an unpause are accepted by the plugin and
// go nowhere.
//
// So on Bluetooth the pause hands the device back immediately and the unpause
// opens a fresh stream, which is the same path a track change takes. Nothing is
// lost by it: there is no DAC to keep warm at that end of the link.
static long paused_device_close_ms(void) { return output_is_bluetooth() ? 0 : PAUSED_DEVICE_CLOSE_MS; }

bool audio_output_is_bluetooth(void) { return output_is_bluetooth(); }

// ---------------------------------------------------------------------------
// "Dissolvenza": the volume ramp at each end of a track.
//
// One decoder, one stream: two tracks cannot overlap the way a mixing desk
// would cross-fade them, so this is the honest version of the effect -- the
// last seconds of a track ramp down to silence and the first seconds of the
// next ramp up from it. Back to back, that is what a listener hears as a
// cross-fade.
//
// The gain is Q10 fixed point (1024 = unity): plain integer multiplies on the
// sample, no floating point anywhere near the per-sample path.
// ---------------------------------------------------------------------------

static volatile bool fade_enabled;
static volatile int fade_secs = 3;

void audio_set_fade(bool enabled, int seconds) {
	if (seconds < 1) {
		seconds = 1;
	}
	if (seconds > 12) {
		seconds = 12;
	}
	fade_enabled = enabled;
	fade_secs = seconds;
	fprintf(stderr, "audio: fade %s (%d s)\n", enabled ? "on" : "off", seconds);
}

// 0..1024 for a position in the track. Unity everywhere but the two ends.
static int fade_gain_q10(double pos_secs, double total_secs) {
	if (!fade_enabled) {
		return 1024;
	}

	double span = (double)fade_secs;
	int gain = 1024;

	if (pos_secs < span) {
		int in_gain = (int)((pos_secs / span) * 1024.0);
		gain = in_gain < gain ? in_gain : gain;
	}
	if (total_secs > span * 2 && pos_secs > total_secs - span) {
		double left = total_secs - pos_secs;
		int out_gain = left > 0 ? (int)((left / span) * 1024.0) : 0;
		gain = out_gain < gain ? out_gain : gain;
	}

	if (gain < 0) {
		gain = 0;
	}
	if (gain > 1024) {
		gain = 1024;
	}
	return gain;
}

static void fade_apply_s16(short *samples, int count, int gain) {
	if (gain >= 1024) {
		return;
	}
	for (int i = 0; i < count; i++) {
		samples[i] = (short)(((int)samples[i] * gain) >> 10);
	}
}

static void fade_apply_s32(int32_t *samples, int count, int gain) {
	if (gain >= 1024) {
		return;
	}
	for (int i = 0; i < count; i++) {
		samples[i] = (int32_t)(((int64_t)samples[i] * gain) >> 10);
	}
}
static bool stop_thread = false;

static double seek_target_secs = -1.0;
static bool seek_request = false;

// Set true by the playback thread when a track plays through to its end (as
// opposed to being stopped/replaced by the user). Consumed via
// audio_take_completion() so the controller can auto-advance.
static bool track_completed = false;

// Marks the end of a track's playback routine. The catch is that tearing down
// the old track happens *after* audio_play() has already announced the new one
// (the thread only notices `play_request` once it drops out of its write loop,
// and snd_pcm_drain() can sit there for a while). Blindly writing STOPPED here
// would overwrite the new track's PLAYING and leave the UI showing a stopped
// player for a track that is in fact starting -- so when another file is
// already queued up, leave the status alone.
// Caller must hold audio_mutex.
static void mark_stopped_unless_replaced(void) {
	if (!play_request) {
		playback_status = AUDIO_STATUS_STOPPED;
	}
}

static double progress_current_secs = 0.0;

// Where the bar was when the track was paused.
//
// Pausing winds the decoder back over the frames that were queued but never
// heard, so resuming carries on from the sound and not from the file. That is
// right for the audio and wrong for the display: the position on screen jumps
// backwards by however deep the output buffer is, which on this player is most
// of a second.
//
// So the reported position has a floor while the rewind is in effect. The bar
// stays where the music stopped, resuming replays the buffered moment behind
// it, and the floor lifts by itself as soon as real progress passes it. -1
// means no floor.
static double progress_floor_secs = -1.0;

// Playback speed, for audiobooks. It lives here rather than being handed in
// with the track because it can be changed while one is playing, from the
// player's own pop-over. 1.0 is untouched and costs nothing: speed.c bypasses
// itself entirely at that factor.
static double playback_speed = 1.0;

// What speed.c pulls its input through.
typedef struct {
	decoder_t *dec;
} decoder_fill_t;

static int decoder_fill(void *user, short *dst, int frames) {
	decoder_fill_t *ctx = user;
	return (int)decoder_read_pcm_frames_s16(ctx->dec, (uint64_t)frames, dst);
}
static double progress_total_secs = 0.0;

static int stream_sample_rate = 0;
static int stream_channels = 0;
static int stream_bits = 0; // source bit depth (16/24/32), for the format line
// DSD, for the format line: 64/128/256, or 0 when the track is not DSD. A DSD
// track's real identity is its multiple, not "24 bit at 176.4 kHz" -- that is
// only what the carrier looks like.
static int stream_dsd_multiple = 0;
// Also for the format line and the details page: what is inside the file and
// at how many kbps. A lossy format has no bit depth of its own -- see
// decoder_is_lossy() -- and is described by bitrate instead.
static int stream_bitrate_kbps = 0;
static bool stream_lossy = false;
static char stream_codec[16] = "";

static int parse_wav(const char *filepath, wav_info_t *info, FILE **file_out) {
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		fprintf(stderr, "Audio: Failed to open file: %s\n", filepath);
		return -1;
	}

	char riff_header[12];
	if (fread(riff_header, 1, 12, f) != 12) {
		fclose(f);
		return -1;
	}

	if (memcmp(riff_header, "RIFF", 4) != 0 || memcmp(riff_header + 8, "WAVE", 4) != 0) {
		fclose(f);
		return -1; // Not a valid WAVE file
	}

	info->channels = 0;
	info->sample_rate = 0;
	info->bits_per_sample = 0;
	info->data_offset = 0;
	info->data_size = 0;

	struct {
		char id[4];
		uint32_t size;
	} chunk;

	while (fread(&chunk, 1, sizeof(chunk), f) == sizeof(chunk)) {
		if (memcmp(chunk.id, "fmt ", 4) == 0) {
			struct {
				uint16_t format;
				uint16_t channels;
				uint32_t rate;
				uint32_t byterate;
				uint16_t align;
				uint16_t bps;
			} fmt;
			if (chunk.size < 16) {
				fclose(f);
				return -1;
			}
			if (fread(&fmt, 1, 16, f) != 16) {
				fclose(f);
				return -1;
			}
			info->channels = fmt.channels;
			info->sample_rate = fmt.rate;
			info->bits_per_sample = fmt.bps;

			if (chunk.size > 16) {
				fseek(f, chunk.size - 16, SEEK_CUR);
			}
		} else if (memcmp(chunk.id, "data", 4) == 0) {
			info->data_offset = ftell(f);
			info->data_size = chunk.size;
			break;
		} else {
			// Chunk sizes are padded to even boundaries in RIFF.
			uint32_t skip_sz = (chunk.size + 1) & ~1;
			fseek(f, skip_sz, SEEK_CUR);
		}
	}

	if (info->channels == 0 || info->sample_rate == 0 || info->bits_per_sample == 0 || info->data_offset == 0) {
		fclose(f);
		return -1;
	}

	*file_out = f;
	return 0;
}

// When non-zero, the next open_pcm_device() sizes the buffer to this many
// milliseconds instead of its own 750.
//
// It exists for the emulator, whose need is the reverse of the one the 750 ms
// serve: a game has to sound a button press now, not three quarters of a second
// later, and the short buffer doubles as its metronome -- writing 738 samples
// per frame to a blocking PCM lets ALSA keep the 59.73 fps pace instead of an
// estimating usleep. The short reserve is affordable because the emulator does
// not read the card while running: the ROM is already entirely in memory.
static int pcm_forced_buffer_ms;

// ---------------------------------------------------------------------------
// Gapless: the PCM that stays open between tracks
//
// Without it, the end of a track drains the PCM (snd_pcm_drain waits for the
// card to play everything out), closes it and parks the thread; the interface
// notices on the next tick of its timer, opens the next file, reopens the PCM
// and starts writing again. Drain, close, a timer tick and two opens sit
// between the last sample of one track and the first of the next -- a few
// tenths of a second on this device, which is a defect on a live album or a
// concept record where one track runs into the next.
//
// With it, the PCM is neither drained nor closed at the end of a track: it
// stays open with its queue still inside -- up to 750 ms of music already
// handed to the card, which keeps playing on its own. The next track opens
// while that queue drains and appends to the same queue, so the card is never
// left with nothing to play and there is no silence.
//
// The conditions for holding it are deliberately strict:
//
//   * the track must have ended on its own. After next, stop, or another
//     track, the previous queue is not wanted: snd_pcm_drop as usual.
//   * no DoP. A DSD stream has the DAC in a mode of its own, and appending PCM
//     to it is full-volume noise.
//   * the next track must have exactly the same format: channels, rate and
//     depth. An open PCM is configured on those three numbers and cannot be
//     reconfigured without closing it, which is the very thing being avoided.
//     A 44.1 kHz album plays gapless; the 96 kHz track after it reopens and
//     the gap returns, as it should.
//   * and the same output. Switching to Bluetooth midway is a different PCM.
//
// If the next track never comes, the thread waits a little over half a second
// (GAPLESS_HOLD_MS, less than the queue the card holds), then drains and
// closes as it would have done immediately. End of album therefore costs half
// a second of PCM held open, inaudibly.
// ---------------------------------------------------------------------------

// How long a PCM is held open while no next track arrives. 600 ms: less than
// the ~750 the card has queued -- otherwise it would run dry first -- and more
// than one tick of the timer that advances the queue.
#define GAPLESS_HOLD_MS 600

// How long a file that is still downloading is waited on before it is declared
// unreadable. Six seconds in half-second steps: long enough for a slow network
// to deliver the header, short enough not to look like a freeze -- and the
// wait gives up on the first command anyway. See play_decoded_file().
#define DECODER_GROW_RETRIES 12
#define DECODER_GROW_RETRY_MS 500

static bool gapless_enabled;

// The pointer sits under a small lock of its own, held only briefly: another
// thread can come to close it (the emulator claiming the PCM for itself, see
// audio_external_begin_latency), and closing the same handle twice is a crash.
// The actual drain happens outside the lock -- snd_pcm_drain waits for the
// card to finish, and nobody should queue up behind that.
static pthread_mutex_t held_lock = PTHREAD_MUTEX_INITIALIZER;
static snd_pcm_t *held_pcm;
static int held_channels;
static int held_rate;
static int held_bits;
static snd_pcm_uframes_t held_period;
static char held_device[sizeof(output_pcm)];
static int held_route;

void audio_set_gapless(bool enabled) {
	gapless_enabled = enabled;
	printf("audio: gapless %s\n", enabled ? "on" : "off");
}

bool audio_get_gapless(void) { return gapless_enabled; }

// How long a stream is given to play out what it still holds before it is
// dropped instead. Comfortably more than the deepest buffer this player opens,
// and short enough that a link which is never going to drain does not take the
// playback thread with it.
#define DRAIN_TIMEOUT_MS 3000

// ...and how long it may make no progress at all before it is taken for
// finished. The buffer going down slowly is a stream still playing out; the
// buffer not going down is one nobody is reading.
#define DRAIN_STALL_MS 200

// How long a period should last above 192 kHz, and how large one is ever
// allowed to get. The length is what 4096 frames last at 44.1 kHz, which is the
// sizing every rate below has been using all along; the cap is there because
// the frames are what the driver has to find DMA memory for, and a request it
// cannot meet is settled downwards rather than refused.
#define PERIOD_TARGET_MS 90
#define PERIOD_FRAMES_MAX 32768

// How much audio one turn of the decode loop handles.
//
// Not the same question as how deep the card's buffer is, and it has to be
// asked separately: the driver settles the period wherever its DMA memory runs
// out, so a long one asked for at 705.6 kHz can still come back short. The loop
// would then turn a hundred and seventy times a second whatever was requested,
// and the thread doing the turning is real-time on the one core the interface
// also wants. It is not the work that hurts there, it is being interrupted that
// often to do it.
//
// So a turn reads and writes whole periods until it has about this much, and
// blocks once instead of eight times. snd_pcm_writei takes any number of
// frames; more than a period is queued and waited on, which is the same waiting
// done in one piece.
#define CHUNK_TARGET_MS 90

static snd_pcm_uframes_t chunk_for(snd_pcm_uframes_t period, int sample_rate) {
	snd_pcm_uframes_t want = (snd_pcm_uframes_t)(((long)sample_rate * CHUNK_TARGET_MS) / 1000);
	snd_pcm_uframes_t chunk = period;
	while (chunk < want && chunk + period > chunk) {
		chunk += period;
	}
	return chunk;
}

// How long the end of a track waits after closing a bluealsa PCM, before the
// next track opens one. See the call site.
#define BT_REOPEN_SETTLE_MS 200

// Whether this handle -- not the route installed right now -- writes to
// bluealsa. Asked of the handle, because the two can differ: the headphones
// dropping flips the route back to the jack while the track being finished is
// still on the old PCM, and every question below is about that PCM.
// snd_pcm_name() answers with the name the handle was opened under, so it is
// also right when an open fell back to the jack on its own.
static bool pcm_is_bluetooth(snd_pcm_t *pcm) {
	const char *name = pcm ? snd_pcm_name(pcm) : NULL;
	return name && strncmp(name, "bluealsa", 8) == 0;
}

// Plays out what the stream still holds, and comes back whatever happens.
//
// snd_pcm_drain() on a blocking handle waits for the buffer to empty, and over
// Bluetooth that can be for ever. A bluealsa PCM is not a sound card: there is
// no clock consuming the buffer, only the A2DP link, and headphones walking out
// of range are enough for it to stop consuming anything. A playback thread that
// goes into alsa-lib there never comes out: the track never ends, the next play
// request is never picked up, and the handle is never closed, which keeps
// bluealsa's client alive with its transport acquired so the headphones cannot
// re-establish it either.
//
// So over Bluetooth the waiting is done here instead: ask how much is still
// queued, sleep, ask again, and give up at the deadline. snd_pcm_delay() reads
// the stream's pointer and cannot block. On a real card the ordinary drain is
// used, bounded by a clock that always runs.
static void pcm_drain_bounded(snd_pcm_t *pcm) {
	if (!pcm) {
		return;
	}
	if (!pcm_is_bluetooth(pcm)) {
		snd_pcm_drain(pcm);
		return;
	}

	long waited_ms = 0;
	long stalled_ms = 0;
	snd_pcm_sframes_t previous = -1;
	for (;;) {
		snd_pcm_sframes_t left = 0;
		// A handle that cannot say (an xrun, a plugin that has given up) has
		// nothing left worth waiting for.
		if (snd_pcm_delay(pcm, &left) < 0 || left <= 0) {
			break;
		}

		// Gone as soon as it stops going down, not at the far end of the
		// deadline. At the natural end of a track over A2DP the queue empties
		// to within a period and then simply stops, because nothing is asking
		// bluealsa for more; sitting out the whole deadline for that would put
		// three seconds of silence between one track and the next. A fifth of a
		// second without progress is proof enough.
		if (previous >= 0 && left >= previous) {
			stalled_ms += 20;
		} else {
			stalled_ms = 0;
		}
		previous = left;

		if (stalled_ms >= DRAIN_STALL_MS || waited_ms >= DRAIN_TIMEOUT_MS) {
			fprintf(stderr, "audio[%ld]: the Bluetooth PCM will not drain (%ld frames left after %ld ms): letting it go\n",
					log_ms(), (long)left, waited_ms);
			break;
		}
		usleep(20 * 1000);
		waited_ms += 20;
	}
	// Either it played out or it never will; in both cases the stream stops
	// here rather than in a wait with no end.
	snd_pcm_drop(pcm);
}

// Closes the held PCM, letting it play out what it still has queued. Playback
// thread only: nothing else touches held_pcm.
static void gapless_release(void) {
	pthread_mutex_lock(&held_lock);
	snd_pcm_t *pcm = held_pcm;
	held_pcm = NULL;
	pthread_mutex_unlock(&held_lock);

	if (!pcm) {
		return;
	}
	fprintf(stderr, "audio[%ld]: gapless: closing the held PCM\n", log_ms());
	if (snd_pcm_state(pcm) == SND_PCM_STATE_PREPARED) {
		snd_pcm_start(pcm);
	}
	// Drain rather than drop: what is in there is the real tail of the track
	// that just ended, and it has to be heard. With a deadline, because a
	// handle that will never drain must not take the thread with it.
	pcm_drain_bounded(pcm);
	snd_pcm_close(pcm);
	pcm_device_open = false;
}

// True while a PCM is currently being held.
static bool gapless_holding(void) {
	pthread_mutex_lock(&held_lock);
	bool held = held_pcm != NULL;
	pthread_mutex_unlock(&held_lock);
	return held;
}

// The track ended on its own and the format can be reused, so keep the PCM.
static void gapless_hold(snd_pcm_t *pcm, int channels, int rate, int bits, snd_pcm_uframes_t period) {
	pthread_mutex_lock(&held_lock);
	held_pcm = pcm;
	pthread_mutex_unlock(&held_lock);
	held_channels = channels;
	held_rate = rate;
	held_bits = bits;
	held_period = period;
	held_route = alsa_output_key();
	// The device this handle is really on. Taken from the handle and not from
	// the route, for the reason given at keep_open: the route can already have
	// moved on, and then the next track would be handed a PCM to somewhere it
	// did not ask for.
	const char *opened_as = snd_pcm_name(pcm);
	snprintf(held_device, sizeof(held_device), "%s", opened_as ? opened_as : AUDIO_DEFAULT_PCM);
	fprintf(stderr, "audio[%ld]: gapless: holding the PCM (%d ch, %d Hz, %d bit)\n", log_ms(), channels, rate, bits);
}

// The held PCM, when it is the one needed. On a format mismatch it is closed
// and NULL returned, so the caller opens a device as usual.
static snd_pcm_t *gapless_take(int channels, int rate, int bits, snd_pcm_uframes_t *period_out) {
	if (!gapless_holding()) {
		return NULL;
	}

	char device[sizeof(output_pcm)];
	audio_get_output_device(device, sizeof(device));

	// The output route counts too: headphones can have been plugged in between
	// the two tracks, and reprogramming the codec under a live stream is
	// exactly what the stock binary never does.
	if (channels != held_channels || rate != held_rate || bits != held_bits || strcmp(device, held_device) != 0 ||
		alsa_output_key() != held_route) {
		fprintf(stderr, "audio[%ld]: gapless: different format (%d/%d/%d against %d/%d/%d), reopening\n", log_ms(), channels,
				rate, bits, held_channels, held_rate, held_bits);
		gapless_release();
		return NULL;
	}

	pthread_mutex_lock(&held_lock);
	snd_pcm_t *pcm = held_pcm;
	held_pcm = NULL;
	pthread_mutex_unlock(&held_lock);
	if (!pcm) {
		return NULL;
	}
	if (period_out) {
		*period_out = held_period;
	}
	fprintf(stderr, "audio[%ld]: gapless: reusing the open PCM\n", log_ms());
	return pcm;
}

// The output as an external source holds it, and who to ask for it back. Both
// are read in open_pcm_device() without external_lock: that pointer is what the
// retry loop is about to find busy anyway, so a stale read costs at worst one
// request that was not needed, or one that arrives a moment late.
static snd_pcm_t *external_pcm;
static bool (*external_release_cb)(void);

void audio_set_external_release_cb(bool (*cb)(void)) { external_release_cb = cb; }

// Asks whoever holds the output for it back, if anyone does.
//
// The AirPlay receiver holds the PCM for as long as it is switched on -- it
// cannot tell that the phone has finished, because shairport keeps its socket
// open for its whole life. Without this, a local track or a streamed one would
// meet a busy device, retry for two seconds and give up.
//
// The request does not block: the hand-back happens on the AirPlay thread
// within its poll, and the retry loop in open_pcm_device() is already built to
// wait out a device that is busy for a moment.
//
// Never fires on the external path itself: audio_external_begin_latency() only
// reaches open_pcm_device() with external_pcm still NULL.
static void release_external_output(void) {
	if (external_pcm && external_release_cb) {
		external_release_cb();
	}
}

static void bt_seek_probe_cancel(void);

// Which bluealsa plugin the ALSA loader actually mapped, said once.
//
// The plugin is a file in the firmware, not part of this build, and the two
// that can be at that path are not interchangeable: the stock 4.1.1 drains with
// poll(..., -1) and never reports SND_PCM_STATE_DRAINING, the 4.3.1 does both.
// The path alone does not say which is there, so the size goes with it -- it is
// what tells a replaced file from the original without opening a shell.
static void log_bluealsa_plugin(void) {
	static bool said;
	if (said) {
		return;
	}
	said = true;

	FILE *maps = fopen("/proc/self/maps", "r");
	if (!maps) {
		return;
	}
	char line[512];
	while (fgets(line, sizeof(line), maps)) {
		char *at = strstr(line, "/libasound_module_pcm_bluealsa.so");
		if (!at) {
			continue;
		}
		char *start = strrchr(line, ' ');
		if (!start) {
			break;
		}
		start++;
		start[strcspn(start, "\r\n")] = '\0';
		struct stat st;
		if (stat(start, &st) == 0) {
			fprintf(stderr, "audio: bluealsa plugin %s (%ld bytes)\n", start, (long)st.st_size);
		} else {
			fprintf(stderr, "audio: bluealsa plugin %s\n", start);
		}
		break;
	}
	fclose(maps);
}

static snd_pcm_t *open_pcm_device(int channels, int sample_rate, int bits_per_sample, snd_pcm_uframes_t *period_size_out) {
	snd_pcm_t *pcm_handle = NULL;

	release_external_output();

	// A track change closes the old PCM and opens a new one back to back. On
	// real hardware the close is not always instantaneous -- the driver can
	// report EBUSY for a moment while the old stream is torn down -- and one
	// failed open here means the new track silently never starts. So the open
	// is retried briefly instead of being given up on the first attempt.
	char device[sizeof(output_pcm)];
	audio_get_output_device(device, sizeof(device));

	// bluealsa reports the same race differently. Where a card driver says EBUSY
	// while the old stream is torn down, the plugin has to re-acquire the A2DP
	// transport over D-Bus first and answers ENODEV, EIO or ETIMEDOUT until it
	// has -- so on Bluetooth those count as "not yet" too, since the link can
	// need a few hundred milliseconds.
	bool bluetooth = strncmp(device, "bluealsa", 8) == 0;

	// A bluealsa name is worth opening only while there is something on the
	// other end of it. When there is not, opening it is at best two seconds of
	// retries per track and at worst a wait inside the plugin, which talks to
	// the bluealsa daemon over D-Bus and has no deadline of its own -- and all
	// of that happens before the fallback below is ever reached, so a dropped
	// link would leave nothing playing at all, jack included.
	//
	// Asked of bluetooth.c and not guessed: it is the side that hears the
	// AudioSink notifications, and its answer is a read of two booleans.
	if (bluetooth && !bluetooth_audio_active()) {
		fprintf(stderr, "audio: '%s' has no sink any more; the jack takes it back\n", device);
		audio_set_output_device(NULL);
		snprintf(device, sizeof(device), "%s", AUDIO_DEFAULT_PCM);
		bluetooth = false;
	}

	// Non-blocking, over Bluetooth only. It is what lets pcm_write_bluetooth()
	// put a deadline on a write that would otherwise wait for room that may
	// never come again. A sound card keeps a blocking handle: there the clock
	// guarantees the room.
	int mode = bluetooth ? SND_PCM_NONBLOCK : 0;

	// How long an open is given before the fallback below takes over.
	//
	// Two seconds is right for a sound card: what holds it is a driver tearing
	// down the previous stream. A bluealsa open is a different thing -- the
	// plugin has to make bluealsa take the A2DP transport back from bluez over
	// D-Bus, and on one core, with an encoder thread of its own at real-time
	// priority, that round trip is not bounded by anything this player controls.
	//
	// And the cost of giving up early is paid in the wrong place: the fallback
	// plays the track out of the jack, which with the headphones still on the
	// listener's head is silence, one track at a time until an open happens to
	// finish in time.
	//
	// So while the radio still says there is a sink, wait for it.
	int err = -EBUSY;
	int attempts = 0;
	int budget = (bluetooth && bluetooth_audio_active()) ? 160 : 40; // eight seconds, or two
	for (; attempts < budget; attempts++) {
		err = snd_pcm_open(&pcm_handle, device, SND_PCM_STREAM_PLAYBACK, mode);
		if (err >= 0) {
			break;
		}
		bool transient = (err == -EBUSY || err == -EAGAIN) ||
						 (bluetooth && (err == -ENODEV || err == -EIO || err == -ETIMEDOUT || err == -ECONNREFUSED));
		if (!transient) {
			break; // a real error, not a teardown race: no point retrying
		}
		usleep(50 * 1000);
	}

	// Headphones that walked out of range take their PCM with them, and a
	// track that refuses to start is a worse outcome than one coming out of
	// the wrong hole. Fall back to the jack, and say so in the log.
	if (err < 0 && strcmp(device, AUDIO_DEFAULT_PCM) != 0) {
		fprintf(stderr, "audio: '%s' would not open (%s); falling back to " AUDIO_DEFAULT_PCM "\n", device,
				snd_strerror(err));
		err = snd_pcm_open(&pcm_handle, AUDIO_DEFAULT_PCM, SND_PCM_STREAM_PLAYBACK, 0);

		// And stop asking for it, so the dead name does not stay installed and
		// cost every following track the same wait while the interface, the
		// volume curve and the power rules all believe the sound is leaving
		// over Bluetooth. Only when the radio agrees the sink is gone: a link
		// that is merely busy for a moment must keep its route, or one failed
		// open would move the listener to the jack for good.
		if (err >= 0 && bluetooth && !bluetooth_audio_active()) {
			audio_set_output_device(NULL);
		}
	}

	if (err < 0) {
		fprintf(stderr, "Audio: Cannot open PCM device '%s': %s (after %d attempts)\n", device, snd_strerror(err),
				attempts);
		return NULL;
	}
	if (attempts > 0) {
		fprintf(stderr, "audio[%ld]: opening '%s' took %d attempts (%d ms)\n", log_ms(), device, attempts,
				attempts * 50);
	}

	snd_pcm_hw_params_t *hw_params;
	snd_pcm_hw_params_alloca(&hw_params);
	snd_pcm_hw_params_any(pcm_handle, hw_params);
	snd_pcm_hw_params_set_access(pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);

	snd_pcm_format_t format;
	if (bits_per_sample == 8) {
		format = SND_PCM_FORMAT_U8;
	} else if (bits_per_sample == 16) {
		format = SND_PCM_FORMAT_S16_LE;
	} else if (bits_per_sample == 24) {
		format = SND_PCM_FORMAT_S24_3LE;
	} else if (bits_per_sample == 32) {
		format = SND_PCM_FORMAT_S32_LE;
	} else {
		fprintf(stderr, "Audio: Unsupported bits per sample: %d\n", bits_per_sample);
		snd_pcm_close(pcm_handle);
		return NULL;
	}

	if (snd_pcm_hw_params_set_format(pcm_handle, hw_params, format) < 0) {
		fprintf(stderr, "audio: format %s refused by the device\n", snd_pcm_format_name(format));
		snd_pcm_close(pcm_handle);
		return NULL;
	}
	// Checked, because a refusal here is silent and sounds like something else
	// entirely. If the device will not do this channel count the parameters
	// keep whatever the hardware allows, and frames written in the layout the
	// caller believes in then come out at the wrong speed with the channels
	// interleaved wrongly -- which is heard as a completely different piece of
	// audio, not as an error.
	if (snd_pcm_hw_params_set_channels(pcm_handle, hw_params, channels) < 0) {
		unsigned int got = (unsigned int)channels;
		snd_pcm_hw_params_set_channels_near(pcm_handle, hw_params, &got);
		fprintf(stderr, "audio: %d channel(s) refused, device settled on %u\n", channels, got);
	}

	// The rate is set exactly, not "near": this DAC follows the track (the CS43198
	// is fed whatever the stream runs at), and silently resampling a 96 kHz file
	// down to 44.1 is precisely what "the DAC does not really change rate" would
	// look like. If the hardware truly cannot do this rate, fall back to the
	// nearest one rather than failing the track -- and say so in the log.
	unsigned int val = sample_rate;
	int dir = 0;
	if (snd_pcm_hw_params_set_rate(pcm_handle, hw_params, val, 0) < 0) {
		snd_pcm_hw_params_set_rate_near(pcm_handle, hw_params, &val, &dir);
		fprintf(stderr, "audio: %d Hz unavailable, device settled on %u Hz\n", sample_rate, val);
	}

	// A deep buffer, on purpose: ~0.75 s at 44.1 kHz. The playback thread can
	// stall for seconds when a big album cover decodes at a track change
	// (memory pressure evicts code pages, the SD is saturated, the thread
	// page-faults), and the AIC driver answers a starved stream with EIO. Four
	// periods of 1024 frames (93 ms) cannot ride that out; this absorbs most of
	// it. Responsiveness does not suffer: stops and switches drop the buffer,
	// they never drain it.
	unsigned int periods = 8;
	snd_pcm_uframes_t period_size = 4096;

	// ...as long as 4096 frames are still a period. Past 96 kHz they stop being
	// one and become a fragment, and the reserve goes with them: 4096 frames are
	// 93 ms at 44.1 kHz, 23 ms at 176.4 kHz, and 5.8 ms at 705.6 kHz, where
	// DSD256 goes over DoP. The whole eight-period buffer at 705.6 kHz was
	// 46 ms, and the stream starts on a full buffer, so that was also all it
	// ever had in hand against an interface sharing the one core. The first
	// thing that takes the core for longer than that is an underrun, a prepare
	// and a rewrite.
	//
	// So above 96 kHz the period is chosen by how long it lasts instead, and the
	// reserve comes out where it is at 44.1 kHz. Everything at or below keeps
	// exactly the sizing it has always had.
	if (sample_rate > 96000) {
		// Doubled while the next step is still near the target rather than past
		// it: a period is a power of two here, and always taking the first one
		// longer than the target would ask for 170 ms at 192 kHz to hit 90.
		snd_pcm_uframes_t want = (snd_pcm_uframes_t)(((long)sample_rate * PERIOD_TARGET_MS) / 1000);
		while (period_size * 2 <= want + want / 2 && period_size * 2 <= PERIOD_FRAMES_MAX) {
			period_size *= 2;
		}
	}

	// Two ways of asking for a buffer, selected by a config key. The default is
	// the frame-based sizing above.
	//
	// The stock player's way, when "audio"/"buffer_time_ms" is set: the size is
	// asked for in TIME and ALSA picks the frames. A value of 200 is roughly
	// what a normal player uses.
	//   [audio]
	//   buffer_time_ms = 200
	// The emulator asks for a much shorter buffer of its own and overrides both:
	// see pcm_forced_buffer_ms.
	int buffer_time_ms = pcm_forced_buffer_ms > 0 ? pcm_forced_buffer_ms : (int)config_get_int("audio", "buffer_time_ms", 0);
	if (buffer_time_ms > 0) {
		unsigned int buffer_time = (unsigned int)buffer_time_ms * 1000u;   // microseconds
		unsigned int period_time = buffer_time / 4;
		snd_pcm_hw_params_set_buffer_time_near(pcm_handle, hw_params, &buffer_time, &dir);
		snd_pcm_hw_params_set_period_time_near(pcm_handle, hw_params, &period_time, &dir);
		fprintf(stderr, "audio: stock-style sizing, buffer %u us / period %u us\n", buffer_time,
				period_time);
	} else {
		snd_pcm_hw_params_set_periods_near(pcm_handle, hw_params, &periods, &dir);
		snd_pcm_hw_params_set_period_size_near(pcm_handle, hw_params, &period_size, &dir);
	}

	err = snd_pcm_hw_params(pcm_handle, hw_params);
	if (err < 0) {
		fprintf(stderr, "Audio: Cannot apply HW parameters: %s\n", snd_strerror(err));
		snd_pcm_close(pcm_handle);
		return NULL;
	}

	// Read back what the hardware actually settled on, not what was asked for:
	// this line is the answer to "is the DAC really following the track?" --
	// play a 24/96 FLAC after a 16/44.1 MP3 and the log has to change.
	{
		unsigned int actual_rate = 0;
		snd_pcm_format_t actual_format = SND_PCM_FORMAT_UNKNOWN;
		unsigned int actual_channels = 0;
		int rdir = 0;
		snd_pcm_hw_params_get_rate(hw_params, &actual_rate, &rdir);
		snd_pcm_hw_params_get_format(hw_params, &actual_format);
		snd_pcm_hw_params_get_channels(hw_params, &actual_channels);
		// The device name belongs on this line: without it the log cannot tell
		// a track that went to the headphones from one that went to the jack,
		// since the two otherwise print identically.
		fprintf(stderr, "audio[%ld]: PCM open on '%s': %u Hz %s %u ch (asked for %d Hz / %d bit)\n", log_ms(), device,
				actual_rate, snd_pcm_format_name(actual_format), actual_channels, sample_rate, bits_per_sample);
	}

	// A seek measurement belongs to the stream it was taken on. A track change
	// between the seek and the measurement gives a fresh, still-filling PCM and
	// a number that says nothing about the seek.
	bt_seek_probe_cancel();

	if (bluetooth) {
		log_bluealsa_plugin();
	}

	// What ALSA settled on, whichever way it was asked: with time-based sizing
	// the frames were its choice from the start, and with frame-based sizing a
	// long period can be more than the driver will allocate, so "near" is not
	// "what was asked for". The write loop is driven by this number and the
	// buffer it writes from is allocated to it.
	{
		snd_pcm_uframes_t actual_period = 0;
		if (snd_pcm_hw_params_get_period_size(hw_params, &actual_period, &dir) >= 0 && actual_period > 0) {
			period_size = actual_period;
		}
		snd_pcm_uframes_t actual_buffer = 0;
		if (snd_pcm_hw_params_get_buffer_size(hw_params, &actual_buffer) >= 0 && actual_buffer > 0) {
			unsigned int rate = val ? val : (unsigned int)sample_rate;
			fprintf(stderr, "audio: buffer %lu frames (%u ms), period %lu frames (%u ms)\n",
					(unsigned long)actual_buffer, (unsigned int)((actual_buffer * 1000u) / rate),
					(unsigned long)period_size, (unsigned int)((period_size * 1000u) / rate));
		}
	}

	// The software parameters are always set, and the important one is the
	// start threshold: the stream only starts on a full buffer.
	//
	// Otherwise the stream starts on the first period written -- 25 ms of
	// reserve at 176.4 kHz -- and any system hiccup (the SD card contended with
	// a download, a cover being decoded) turns straight into an underrun. After
	// a seek or an unpause that becomes a "Broken pipe" on every write: the
	// stream restarts with a single period queued and dies before the next one.
	// Starting on a full buffer gives every start and restart the whole buffer
	// as reserve.
	{
		snd_pcm_uframes_t buffer_frames = 0;
		snd_pcm_hw_params_get_buffer_size(hw_params, &buffer_frames);

		snd_pcm_sw_params_t *sw_params;
		snd_pcm_sw_params_alloca(&sw_params);
		if (snd_pcm_sw_params_current(pcm_handle, sw_params) >= 0) {
			snd_pcm_sw_params_set_avail_min(pcm_handle, sw_params, period_size);
			// Start threshold = the whole buffer (see above).
			if (buffer_frames > 0) {
				snd_pcm_sw_params_set_start_threshold(pcm_handle, sw_params, buffer_frames);
			}
			snd_pcm_sw_params(pcm_handle, sw_params);
		}
	}

	*period_size_out = period_size;
	pcm_device_open = true;
	return pcm_handle;
}

// Writes one buffer of frames, recovering from the errors a real device
// throws instead of killing the track on the first one:
//
//   -EPIPE    underrun -> prepare and rewrite
//   -ESTRPIPE the device was suspended -> resume (then prepare) and rewrite
//   -EIO      the stream went bad underneath -- on this hardware an
//             output-route switch (headphones in/out, line-out toggle) can
//             invalidate the open stream -- so the device is closed and
//             reopened from scratch, then the write retried
//
// Returns the frames written, or negative once recovery has failed several
// times in a row; *pcm may be replaced (or NULL) on return.
static bool playback_reader_should_abort(void);

// How long a write to a Bluetooth PCM may make no progress before the link is
// taken for gone. The same two seconds the wait below already used for a card
// with a stopped clock: long enough for a hiccup on the air, short enough that
// the queue does not stand still.
#define BT_WRITE_STALL_MS 2000

// After a seek on a Bluetooth PCM: one line, a second and a half later, saying
// how much audio is queued.
//
// It separates the two possible explanations of stuttering after a seek. A full
// queue (around 700 ms) means the player is keeping up and the trouble is on
// the air or in the sink; a queue near zero means it is not keeping up, and the
// place to look is here. From outside the two look identical.
//
// Bluetooth only, after a seek only, once per seek.
#define BT_SEEK_PROBE_MS 1500
static long bt_seek_probe_at;

// How long a track takes to start COMING OUT, on the headphones.
//
// Two different causes look identical from outside, and this tells them apart:
//
//   * the stream starts and simply is not heard -- the link is the place to
//     look, and this line says RUNNING after a few hundred ms;
//   * the stream never starts, because the start threshold is the whole buffer
//     (some seven hundred milliseconds of audio) and the CPU does not fill it
//     fast enough -- and this line says PREPARED, and for how long.
//
// One line per track, bluealsa only, like the seek probe. RUNNING says only
// that the plugin started, not that anything is consuming: if the A2DP stream
// is not really streaming, the frames sit in bluealsa's queue. A second line
// shortly after carries the queue depth, and a queue that stays full means
// nothing on the other side is taking anything away.
#define BT_START_GIVEUP_MS 5000
#define BT_START_FLOW_MS 2000
static long bt_start_at;
static long bt_start_flow_at;
static bool bt_start_said;
static long long bt_start_frames;
static int bt_start_rate;

// The A2DP stream state, as bluez has it, appended to the lines below.
//
// Everything this file can see says the same thing whether the headphones are
// playing or not: the writes go through, the queue drains at the right rate,
// the encoder keeps up. Only bluez knows whether the stream those frames are
// encoded into is active, and a stream that is idle or pending renders nothing
// however perfectly it is fed. Asked for at the start and read two seconds
// later, off the worker, because the playback thread does not wait on D-Bus.
static void bt_streams_text(char *out, size_t size) {
	char streams[192];
	int age = -1;
	if (bluetooth_a2dp_streams(streams, (int)sizeof(streams), &age) && streams[0]) {
		snprintf(out, size, "; A2DP stream %s (read %d ms ago)", streams, age);
	} else {
		out[0] = '\0';
	}
}

static void bt_start_probe_arm(snd_pcm_t *pcm) {
	bt_start_at = (pcm && pcm_is_bluetooth(pcm)) ? log_ms() : 0;
	bt_start_flow_at = 0;
	bt_start_said = false;
	bt_start_frames = 0;
	bt_start_rate = 0;
	if (bt_start_at) {
		bluetooth_request_a2dp_streams();
	}
}

static void bt_start_probe_note(snd_pcm_t *pcm, snd_pcm_sframes_t written, int rate) {
	if (!bt_start_at || !pcm) {
		return;
	}
	if (written > 0) {
		bt_start_frames += written;
	}
	bt_start_rate = rate;
	long now = log_ms();

	if (bt_start_said) {
		if (bt_start_flow_at && now >= bt_start_flow_at) {
			long elapsed = now - bt_start_at;
			bt_start_flow_at = 0;
			snd_pcm_sframes_t queued = 0;
			if (snd_pcm_delay(pcm, &queued) < 0) {
				queued = -1;
			}
			// Written minus still queued is what the encoder has taken. Against
			// the frames the elapsed time is worth, it says in one number
			// whether anything is consuming the stream.
			long long taken = bt_start_frames - (queued > 0 ? queued : 0);
			long long realtime = rate > 0 ? (long long)((double)rate * elapsed / 1000.0) : 0;
			char streams[224];
			bt_streams_text(streams, sizeof(streams));
			fprintf(stderr,
					"audio[%ld]: %ld ms after the start: %lld frames written, %ld queued, %lld taken out of %lld of real "
					"time%s\n",
					now, elapsed, bt_start_frames, (long)queued, taken, realtime, streams);
		}
		return;
	}

	long elapsed = now - bt_start_at;
	snd_pcm_state_t state = snd_pcm_state(pcm);
	if (state == SND_PCM_STATE_RUNNING) {
		bt_start_said = true;
		bt_start_flow_at = now + BT_START_FLOW_MS;
		// Asked again here so the line below reads an answer from after the
		// stream started, not one from before the PCM was even open.
		bluetooth_request_a2dp_streams();
		fprintf(stderr, "audio[%ld]: the track started coming out after %ld ms (%lld frames written)\n", now, elapsed,
				bt_start_frames);
		return;
	}
	if (elapsed >= BT_START_GIVEUP_MS) {
		bt_start_said = true;
		char streams[224];
		bt_streams_text(streams, sizeof(streams));
		fprintf(stderr, "audio[%ld]: after %ld ms the track has still not come out (state %s, %lld frames written)%s\n",
				now, elapsed, snd_pcm_state_name(state), bt_start_frames, streams);
	}
}

static void bt_seek_probe_arm(snd_pcm_t *pcm) {
	bt_seek_probe_at = (pcm && pcm_is_bluetooth(pcm)) ? log_ms() + BT_SEEK_PROBE_MS : 0;
}

static void bt_seek_probe_cancel(void) { bt_seek_probe_at = 0; }

static void bt_seek_probe_report(snd_pcm_t *pcm) {
	if (!bt_seek_probe_at || log_ms() < bt_seek_probe_at) {
		return;
	}
	bt_seek_probe_at = 0;
	if (!pcm) {
		return;
	}
	snd_pcm_sframes_t queued = 0;
	if (snd_pcm_delay(pcm, &queued) < 0) {
		queued = -1;
	}
	fprintf(stderr, "audio[%ld]: %d ms after the seek the Bluetooth queue is %ld frames (state %s)\n", log_ms(),
			BT_SEEK_PROBE_MS, (long)queued, snd_pcm_state_name(snd_pcm_state(pcm)));
}

// Writes a whole period to a bluealsa PCM without ever blocking indefinitely.
//
// snd_pcm_writei() on a blocking handle waits for room, and over A2DP that room
// can stop coming for good -- there is no clock consuming the buffer, only a
// link that may have gone away, and the playback thread parks in poll() with a
// play request pending and no track ever starting. The wait above catches the
// case where the stream is RUNNING and the poll says nothing; it does not catch
// the write itself deciding to wait.
//
// So the Bluetooth PCM is opened non-blocking (open_pcm_device) and the waiting
// is done here, in slices, with a deadline. A partial write is not a loss here:
// the rest of the period is written on the next turn round the loop.
static snd_pcm_sframes_t pcm_write_bluetooth(snd_pcm_t *pcm, const void *buf, snd_pcm_uframes_t frames,
											 int frame_bytes) {
	const char *from = (const char *)buf;
	snd_pcm_uframes_t done = 0;
	long last_progress_ms = log_ms();

	while (done < frames) {
		snd_pcm_sframes_t n = snd_pcm_writei(pcm, from + done * (size_t)frame_bytes, frames - done);
		if (n > 0) {
			done += (snd_pcm_uframes_t)n;
			last_progress_ms = log_ms();
			continue;
		}
		if (n < 0 && n != -EAGAIN) {
			// A real error (-EPIPE, -ESTRPIPE, -ENODEV...): the caller knows
			// how to recover from each, and knows nothing about what has
			// already gone out, so anything written so far is reported first.
			return done > 0 ? (snd_pcm_sframes_t)done : n;
		}

		// No room -- either -EAGAIN, or a write that took nothing and said so
		// without complaining. A command waiting is a better reason to come
		// back than any deadline: whoever pressed stop or next is not waiting
		// for this.
		if (playback_reader_should_abort()) {
			return done > 0 ? (snd_pcm_sframes_t)done : -EINTR;
		}

		// The deadline is read off the clock, and not from how the wait below
		// came back. Counting only the polls that TIMED OUT would leave one way
		// through with no deadline at all: a poll that keeps saying "there is
		// room" while the write keeps saying there is not. bluealsa answers
		// exactly that way -- its poll follows the FIFO, the write follows the
		// transport, and the two disagree the moment the link stops draining --
		// so an indefinite block would simply become an indefinite spin.
		long since = log_ms() - last_progress_ms;
		if (since < 0) {
			since = 0; // the millisecond counter wrapped; start the window again
			last_progress_ms = log_ms();
		}
		if (since >= BT_WRITE_STALL_MS) {
			fprintf(stderr, "audio[%ld]: the Bluetooth PCM has taken nothing for %ld ms: calling it lost\n",
					log_ms(), since);
			return done > 0 ? (snd_pcm_sframes_t)done : -EIO;
		}

		int ready = snd_pcm_wait(pcm, 100);
		if (ready < 0) {
			return done > 0 ? (snd_pcm_sframes_t)done : (snd_pcm_sframes_t)ready;
		}
		if (ready > 0) {
			// Told there is room by a poll the write has just contradicted.
			// Ten milliseconds, so a disagreement that lasts becomes a wait and
			// not a thread spinning on one core against a device that is not
			// listening.
			usleep(10 * 1000);
		}
	}
	return (snd_pcm_sframes_t)done;
}

static snd_pcm_sframes_t pcm_write_recover(snd_pcm_t **pcm, const void *buf, snd_pcm_uframes_t frames, int channels,
										   int rate, int bits, snd_pcm_uframes_t *period) {
	for (int attempt = 0; attempt < 8; attempt++) {
		if (!*pcm) {
			return -EIO; // a previous recovery closed it and could not get it back
		}

		// Never write blind to a RUNNING stream. The blocking writei waits for
		// room in the card's buffer; after a storm of underruns this hardware
		// can sit with a full buffer and a stopped clock, and that room never
		// frees again, leaving the thread inside ALSA out of reach of any
		// wake-up.
		//
		// But waiting only makes sense while the card is consuming: consumption
		// is what frees room, and a stopped clock is what the wait is meant to
		// detect. On a freshly PREPARED stream -- track start, post-seek,
		// post-underrun -- this driver's poll signals nothing until the stream
		// has started, so waiting there would declare a healthy device stuck and
		// close it. A write to a prepared stream cannot block: the room is there
		// by definition, and the write is what starts it.
		bool room = true;
		snd_pcm_state_t pcm_state = snd_pcm_state(*pcm);
		if (pcm_state == SND_PCM_STATE_RUNNING || pcm_state == SND_PCM_STATE_DRAINING) {
			room = false;
			for (int slice = 0; slice < 20 && !room; slice++) {
				if (playback_reader_should_abort()) {
					return -EINTR; // a command is pending: the main loop handles it
				}
				int r = snd_pcm_wait(*pcm, 100);
				if (r != 0) {
					room = true; // ready (1) or error (<0): the writei will say which
				}
			}
		}

		snd_pcm_sframes_t written;
		if (!room) {
			fprintf(stderr, "audio[%ld]: buffer never free in 2 s: device wedged, rebuilding it\n", log_ms());
			written = -EIO; // skip the writei and go straight to the rebuild
		} else if (pcm_is_bluetooth(*pcm)) {
			written = pcm_write_bluetooth(*pcm, buf, frames, channels * (bits / 8));
		} else {
			written = snd_pcm_writei(*pcm, buf, frames);
		}
		if (written >= 0) {
			return written;
		}

		fprintf(stderr, "audio[%ld]: write error: %s (recovery %d)\n", log_ms(), snd_strerror((int)written),
				attempt + 1);

		if (written == -EPIPE) {
			snd_pcm_prepare(*pcm);
			continue;
		}
		if (written == -ESTRPIPE) {
			int r = -EAGAIN;
			for (int t = 0; r == -EAGAIN && t < 20; t++) {
				r = snd_pcm_resume(*pcm);
				if (r == -EAGAIN) {
					usleep(100 * 1000);
				}
			}
			if (r < 0) {
				snd_pcm_prepare(*pcm);
			}
			continue;
		}

		// -EIO and anything else: rebuild the device from scratch. The stall
		// that produces the EIO (a busy DMA, a route settling) needs a real
		// moment to clear, so the backoff grows with each attempt; reopening
		// immediately only earns another EIO.
		snd_pcm_close(*pcm);
		*pcm = NULL;
		usleep((attempt + 1) * 150 * 1000);
		// The route, not just the PCM. Reopening the stream without redoing the
		// codec routing leaves the DAC in a state where high-rate streams never
		// start again -- 96 kHz tracks die while 48 kHz ones still play -- and
		// only a reboot clears it. Same move as pcm_reroute, for the same
		// reason.
		auto_set_output();
		*pcm = open_pcm_device(channels, rate, bits, period);
		if (!*pcm) {
			return written; // it really is gone
		}
		fprintf(stderr, "audio[%ld]: pcm reopened (recovery %d)\n", log_ms(), attempt + 1);
	}
	return -EIO;
}



// Mid-track jack change (headphones in/out while playing): the stock engine
// sets a reinit flag and its device layer reopens on the new route; this is
// the same move between two writes -- the only moment no stream is live, so
// the codec switch cannot stall anything.
static bool pcm_reroute(snd_pcm_t **pcm, int channels, int rate, int bits, snd_pcm_uframes_t *period) {
	fprintf(stderr, "audio[%ld]: jack changed, rerouting\n", log_ms());
	if (*pcm) {
		snd_pcm_drop(*pcm);
		snd_pcm_close(*pcm);
		*pcm = NULL;
	}
	usleep(150 * 1000); // let the switch settle before the codec is poked
	auto_set_output();
	*pcm = open_pcm_device(channels, rate, bits, period);
	return *pcm != NULL;
}

// A write failure that survived every in-place recovery still gets one clean
// retry of the whole track: close everything, pause, and play the same file
// again from the top, so a failed start costs a half-second hiccup rather than
// silence. One retry only, then a real stop.
static bool retry_track_once(const char *filepath, const char *reason) {
	pthread_mutex_lock(&audio_mutex);
	if (write_fail_retry_used) {
		pthread_mutex_unlock(&audio_mutex);
		return false;
	}
	write_fail_retry_used = true;
	strncpy(current_filepath, filepath, sizeof(current_filepath) - 1);
	current_filepath[sizeof(current_filepath) - 1] = '\0';
	play_request = true;
	play_request_ms = log_ms();
	audio_command = AUDIO_CMD_PLAY;
	playback_status = AUDIO_STATUS_PLAYING;
	pthread_cond_signal(&audio_cond);
	pthread_mutex_unlock(&audio_mutex);
	fprintf(stderr, "audio[%ld]: retrying '%s' (%s)\n", log_ms(), filepath, reason);
	return true;
}

// ---------------------------------------------------------------------------
// Rebuffering
//
// Only tracks that are played while still downloading need this. The rule is
// the usual one for anything playing off a network: play while there is enough
// ahead, and when too little is left, stop and wait until there is plenty
// again. The two thresholds differ on purpose -- stop at two seconds, resume
// at eight -- otherwise the wait would be re-entered on every buffer, which is
// stuttering by another name.
// ---------------------------------------------------------------------------

#define REBUFFER_LOW_SECS 2.0	// below this much ahead, stop
#define REBUFFER_HIGH_SECS 8.0	// and do not resume before this much is back
#define REBUFFER_MAX_WAIT_MS 60000

// Seconds of music ahead of the play position, or -1 when the file is no
// longer growing (and there is then nothing to wait for).
static double growing_headroom_secs(decoder_t *dec, int sample_rate, uint64_t at_frame) {
	uint64_t reachable = decoder_seekable_limit(dec);
	if (reachable == 0 || sample_rate <= 0) {
		return -1.0;
	}
	if (reachable <= at_frame) {
		return 0.0;
	}
	return (double)(reachable - at_frame) / (double)sample_rate;
}

static void rebuffer_if_starved(decoder_t *dec, int sample_rate, uint64_t at_frame) {
	double head = growing_headroom_secs(dec, sample_rate, at_frame);
	if (head < 0 || head >= REBUFFER_LOW_SECS) {
		return;
	}

	fprintf(stderr, "audio[%ld]: buffer run out (%.1f s ahead): waiting\n", log_ms(), head);

	int waited_ms = 0;
	while (waited_ms < REBUFFER_MAX_WAIT_MS) {
		// A new command -- stop, pause, another track -- outranks the wait:
		// leave at once and let the main loop deal with it.
		pthread_mutex_lock(&audio_mutex);
		bool leave = audio_command != AUDIO_CMD_PLAY || play_request || stop_thread || seek_request;
		pthread_mutex_unlock(&audio_mutex);
		if (leave) {
			return;
		}

		// The download finished (or died): nothing more is coming, carry on
		// with what is there.
		if (!decoder_is_growing(dec)) {
			return;
		}

		head = growing_headroom_secs(dec, sample_rate, at_frame);
		if (head < 0 || head >= REBUFFER_HIGH_SECS) {
			fprintf(stderr, "audio[%ld]: resuming with %.1f s buffered\n", log_ms(), head);
			return;
		}

		usleep(100 * 1000);
		waited_ms += 100;
	}

	fprintf(stderr, "audio[%ld]: a minute of waiting and the buffer will not fill: carrying on anyway\n", log_ms());
}

// The safety net for when the estimate above is wrong.
//
// The check above works from a ratio (bytes downloaded over total bytes,
// carried onto frames) and is only an estimate: it can report headroom while
// the decoder is already reading at the edge of the download, where reads
// trickle and the card underruns continuously.
//
// A short read from a growing file is not an estimate: it is proof that the
// edge has been reached. So this waits on real bytes: it takes where the
// download has got to, computes the file's rate (total bytes over total
// duration, both known), and does not resume before REBUFFER_HIGH_SECS
// seconds of file are in hand beyond that point.
static void rebuffer_after_short_read(decoder_t *dec, int sample_rate, uint64_t at_frame) {
	long done = 0, total = 0;
	if (!decoder_grow_span(dec, &done, &total) || total <= 0) {
		return; // not growing (any more): nothing to wait for
	}

	// The flag can be stale: raised while playing at the edge, followed by a
	// backwards seek. When there is already enough ahead of the play position
	// there is no starvation to wait out, and waiting anyway would leave the
	// music stuck at the seek target with the data already on disk.
	double head = growing_headroom_secs(dec, sample_rate, at_frame);
	if (head >= REBUFFER_LOW_SECS) {
		return;
	}
	uint64_t total_frames = decoder_total_pcm_frames(dec);
	if (total_frames == 0 || sample_rate <= 0) {
		return;
	}
	double total_secs = (double)total_frames / (double)sample_rate;
	if (total_secs <= 0) {
		return;
	}
	double file_rate = (double)total / total_secs; // file bytes per second of music

	long target = done + (long)(file_rate * REBUFFER_HIGH_SECS);
	if (target > total) {
		target = total;
	}

	fprintf(stderr, "audio[%ld]: reading at the edge of the download (%ld KB): waiting for the buffer\n", log_ms(),
			done / 1024);

	int waited_ms = 0;
	while (waited_ms < REBUFFER_MAX_WAIT_MS) {
		pthread_mutex_lock(&audio_mutex);
		bool leave = audio_command != AUDIO_CMD_PLAY || play_request || stop_thread || seek_request;
		pthread_mutex_unlock(&audio_mutex);
		if (leave) {
			return; // a command outranks the wait
		}

		if (!decoder_grow_span(dec, &done, &total)) {
			fprintf(stderr, "audio[%ld]: the download is finished, resuming\n", log_ms());
			return;
		}
		if (done >= target) {
			fprintf(stderr, "audio[%ld]: resuming with the buffer full (%ld KB)\n", log_ms(), done / 1024);
			return;
		}

		usleep(100 * 1000);
		waited_ms += 100;
	}
	fprintf(stderr, "audio[%ld]: a minute at the edge and it is not growing: carrying on anyway\n", log_ms());
}

// Play routine for uncompressed WAV files: PCM data is streamed straight from
// the file, no decode step needed.
static void play_wav_file(const char *filepath) {
	FILE *f = NULL;
	wav_info_t info;
	if (parse_wav(filepath, &info, &f) < 0) {
		fprintf(stderr, "Audio: Failed to parse WAV metadata: %s\n", filepath);
		pthread_mutex_lock(&audio_mutex);
		audio_command = AUDIO_CMD_STOP;
		playback_status = AUDIO_STATUS_STOPPED;
		pthread_mutex_unlock(&audio_mutex);
		return;
	}

	double bytes_per_sec = info.sample_rate * info.channels * (info.bits_per_sample / 8.0);
	pthread_mutex_lock(&audio_mutex);
	progress_total_secs = (double)info.data_size / bytes_per_sec;
	progress_current_secs = 0.0;
	progress_floor_secs = -1.0;
	stream_sample_rate = info.sample_rate;
	stream_channels = info.channels;
	stream_bits = info.bits_per_sample;
	// See the decoder path: a restore already has PAUSE queued, so it must
	// not flash through PLAYING on its way to paused.
	playback_status = (audio_command == AUDIO_CMD_PAUSE) ? AUDIO_STATUS_PAUSED : AUDIO_STATUS_PLAYING;
	pthread_mutex_unlock(&audio_mutex);

	snd_pcm_uframes_t period_size;
	snd_pcm_t *pcm_handle = open_pcm_device(info.channels, info.sample_rate, info.bits_per_sample, &period_size);
	if (!pcm_handle) {
		fclose(f);
		pthread_mutex_lock(&audio_mutex);
		audio_command = AUDIO_CMD_STOP;
		playback_status = AUDIO_STATUS_STOPPED;
		pthread_mutex_unlock(&audio_mutex);
		return;
	}

	// From here, how long this track takes to start coming out is timed.
	bt_start_probe_arm(pcm_handle);

	int frame_bytes = info.channels * (info.bits_per_sample / 8);
	long paused_at_ms = 0; // when the current pause began (device power-down clock)
	char *buffer = malloc(period_size * frame_bytes);
	if (!buffer) {
		fprintf(stderr, "Audio: Out of memory for period buffer\n");
		snd_pcm_close(pcm_handle);
		fclose(f);
		pthread_mutex_lock(&audio_mutex);
		audio_command = AUDIO_CMD_STOP;
		playback_status = AUDIO_STATUS_STOPPED;
		pthread_mutex_unlock(&audio_mutex);
		return;
	}

	// int64_t and not long: on the device (MIPS o32) long is 32 bit, and this
	// counts PCM bytes. At 44.1 kHz 16-bit stereo that is 176,400 bytes a
	// second, so a 32-bit sign flips after three and three quarter hours --
	// the middle of a long audiobook, not an edge case. A negative count makes
	// progress_current_secs negative and also decides whether a track finished
	// or was cut short (cut_short, below), so the queue would advance on its
	// own mid-book.
	int64_t bytes_played = 0;
	bool is_paused = false;
	bool played_to_the_end = false;
	int route_check = 0;
	int track_route = alsa_output_key(); // the output auto_set_output just applied

	while (1) {
		loop_turns++;
		pthread_mutex_lock(&audio_mutex);
		if (audio_command == AUDIO_CMD_STOP || play_request) {
			pthread_mutex_unlock(&audio_mutex);
			break;
		}
		if (seek_request) {
			double target = seek_target_secs;
			seek_request = false;
			pthread_mutex_unlock(&audio_mutex);

			int64_t target_byte_pos = (int64_t)(target * bytes_per_sec);
			target_byte_pos = (target_byte_pos / frame_bytes) * frame_bytes;
			if (target_byte_pos < 0)
				target_byte_pos = 0;
			if (target_byte_pos > info.data_size)
				target_byte_pos = info.data_size;

			fseek(f, info.data_offset + target_byte_pos, SEEK_SET);

			// The device may have been handed back during a long pause (see the
			// PAUSE branch below), and then there is no stream to flush. It has
			// to be checked: alsa answers a null handle with an assert, which
			// aborts -- the crash handler parks the audio thread and the player
			// goes silent until it is restarted, with the interface still
			// running as if nothing had happened.
			//
			// Nothing else needs doing when the device is gone. The file is
			// already at the new position, the track stays paused, and the
			// unpause opens a fresh stream from there.
			if (pcm_handle) {
				snd_pcm_drop(pcm_handle);
						snd_pcm_prepare(pcm_handle);
				is_paused = false;
				bt_seek_probe_arm(pcm_handle);
			}

			bytes_played = target_byte_pos;
			pthread_mutex_lock(&audio_mutex);
			progress_current_secs = (double)bytes_played / bytes_per_sec;
			pthread_mutex_unlock(&audio_mutex);
			continue;
		}
		if (audio_command == AUDIO_CMD_PAUSE) {
			bool entering = !is_paused;
			if (entering) {
				is_paused = true;
				playback_status = AUDIO_STATUS_PAUSED;
			}
			pthread_mutex_unlock(&audio_mutex);

			if (entering) {
				int64_t rewind_bytes = pause_stop_stream(pcm_handle, frame_bytes);
				if (rewind_bytes > 0) {
					// What was still in the buffer was never heard: wind the
					// file back over it so resuming picks up where the sound
					// actually stopped.
					bytes_played -= rewind_bytes;
					if (bytes_played < 0) {
						bytes_played = 0;
					}
					fseek(f, info.data_offset + bytes_played, SEEK_SET);
					pthread_mutex_lock(&audio_mutex);
					progress_floor_secs = progress_current_secs; // the bar stays put
					progress_current_secs = (double)bytes_played / bytes_per_sec;
					pthread_mutex_unlock(&audio_mutex);
				}
			}

			// Straight out of a system suspend the device is left SUSPENDED
			// underneath a paused track: put it back on its feet now, rather
			// than letting the next thing that touches it find it that way.
			pthread_mutex_lock(&audio_mutex);
			bool recover = resume_recover_pending;
			resume_recover_pending = false;
			pthread_mutex_unlock(&audio_mutex);
			if (recover && pcm_handle) {
				pcm_restart_after_pause(pcm_handle);
			}

			// Paused long enough: give the device back so the DAC and the amp
			// can power down. The file stays open and the position stays
			// where it is; the unpause below opens a fresh stream.
			if (entering) {
				paused_at_ms = log_ms();
			}
			if (pcm_handle && (log_ms() - paused_at_ms) >= paused_device_close_ms()) {
				snd_pcm_close(pcm_handle);
				pcm_handle = NULL;
				pcm_device_open = false;
				fprintf(stderr, "audio[%ld]: device released while paused (DAC powered down)\n", log_ms());
			}

			// With the device handed back there is nothing to be quick about:
			// a fifth of a second between checks instead of a twentieth means
			// four fewer wake-ups a second for however many hours the player
			// sits paused in a pocket. The unpause still feels instant.
			usleep(pcm_handle ? 50000 : 200000);
			continue;
		} else {
			// RESUME is a one-shot command: consumed here, back to
			// AUDIO_CMD_PLAY. A command left at RESUME poisons every "is a
			// command pending?" test (audio_command != AUDIO_CMD_PLAY) -- the
			// growfile waits would give up immediately, the wait for room in
			// the card's buffer would return -EINTR and throw away a block of
			// samples per pass, and the "retry after rebuffering" path at the
			// end of a track would loop without ever declaring the end.
			if (audio_command == AUDIO_CMD_RESUME) {
				audio_command = AUDIO_CMD_PLAY;
			}
			if (is_paused) {
				// An unpause always reopens the device from scratch, even after
				// a one-second pause. Reusing the stopped stream (dropped at
				// the pause, prepared now) leaves this driver in a bad state:
				// the audio restarts stuttering, and the real-time audio thread
				// on a single core never yields the CPU, so the interface
				// stutters with it. The open-then-write path is the one every
				// track start, prev and next takes. The cost is a few
				// milliseconds of open, inaudible under a finger that has just
				// pressed play.
				if (pcm_handle) {
					snd_pcm_close(pcm_handle);
					pcm_handle = NULL;
					pcm_device_open = false;
					// Breathing room after the close: the same as the 120 ms
					// on a track's way out and the 150 ms in pcm_reroute. On
					// this driver an open glued to a close comes back busy or
					// half-broken, and a stream opened that way stutters.
					usleep(150 * 1000);
				}
				// Route before stream, as play_file does at every track start:
				// during the pause the codec may have gone to rest (amp down,
				// mute), and opening a PCM over a half-asleep route is the
				// other way to get stuttering music. No stream is live here, so
				// poking the codec cannot stall anything.
				auto_set_output();
				{
					// The device was handed back during the pause: take it
					// again. The file is already positioned where the sound
					// stopped, so nothing else has to move.
					snd_pcm_uframes_t new_period = period_size;
					pcm_handle = open_pcm_device(info.channels, info.sample_rate, info.bits_per_sample, &new_period);
					if (!pcm_handle) {
						fprintf(stderr, "Audio: cannot reopen the device after a pause\n");
						audio_command = AUDIO_CMD_STOP;
						playback_status = AUDIO_STATUS_STOPPED;
						pthread_mutex_unlock(&audio_mutex);
						break;
					}
					if (new_period > period_size) {
						char *bigger = realloc(buffer, new_period * frame_bytes);
						if (!bigger) {
							snd_pcm_close(pcm_handle);
							pcm_handle = NULL;
							pcm_device_open = false;
							audio_command = AUDIO_CMD_STOP;
							playback_status = AUDIO_STATUS_STOPPED;
							pthread_mutex_unlock(&audio_mutex);
							break;
						}
						buffer = bigger;
					}
					period_size = new_period;
					fprintf(stderr, "audio[%ld]: device taken back for the unpause\n", log_ms());
					// A reopen is a fresh start for the sink as much as a new
					// track is, and the clock has to start again with it: left
					// armed from the track, the probe would charge the whole
					// pause to the restart.
					bt_start_probe_arm(pcm_handle);
				}
				is_paused = false;
				playback_status = AUDIO_STATUS_PLAYING;
			}
		}
		pthread_mutex_unlock(&audio_mutex);

		// Every ~32 buffers: has the jack changed?
		if (++route_check >= 32) {
			route_check = 0;
			int now_route = alsa_output_key();
			if (now_route != track_route) {
				track_route = now_route;
				if (!pcm_reroute(&pcm_handle, info.channels, info.sample_rate, info.bits_per_sample,
								 &period_size)) {
					fprintf(stderr, "Audio: reroute failed\n");
					pthread_mutex_lock(&audio_mutex);
					audio_command = AUDIO_CMD_STOP;
					playback_status = AUDIO_STATUS_STOPPED;
					pthread_mutex_unlock(&audio_mutex);
					break;
				}
			}
		}

		size_t read_bytes = fread(buffer, 1, period_size * frame_bytes, f);
		if (read_bytes > 0) {
			// 16- and 32-bit WAV go through the EQ chain at their own width;
			// packed 24-bit (S24_3LE) has no in-place representation the
			// filters can use, so it plays untouched rather than mangled.
			if (info.bits_per_sample == 16) {
				replaygain_process((short *)buffer, (int)(read_bytes / frame_bytes), info.channels);
				eq_process((short *)buffer, (int)(read_bytes / frame_bytes), info.channels, info.sample_rate);
				// Soundfield last, on the finished stereo pair -- the stock
				// module sits at the end of the chain too, taking whatever the
				// tone shaping left it and widening that.
				soundfield_process((short *)buffer, (int)(read_bytes / frame_bytes), info.channels);
				crossfeed_process((short *)buffer, (int)(read_bytes / frame_bytes), info.channels,
								  info.sample_rate);
				balance_process((short *)buffer, (int)(read_bytes / frame_bytes), info.channels);
			} else if (info.bits_per_sample == 32) {
				replaygain_process_s32((int32_t *)buffer, (int)(read_bytes / frame_bytes), info.channels);
				eq_process_s32((int32_t *)buffer, (int)(read_bytes / frame_bytes), info.channels,
							   info.sample_rate);
				soundfield_process_s32((int32_t *)buffer, (int)(read_bytes / frame_bytes), info.channels);
				crossfeed_process_s32((int32_t *)buffer, (int)(read_bytes / frame_bytes), info.channels,
									  info.sample_rate);
				balance_process_s32((int32_t *)buffer, (int)(read_bytes / frame_bytes), info.channels);
			}
		}
		if (read_bytes <= 0) {
			// As in the decoded path: a file that gave nothing has not been
			// played, and must not be reported as finished.
			if (bytes_played == 0) {
				fprintf(stderr, "audio[%ld]: '%s' returned no audio at all\n", log_ms(), filepath);
				if (retry_track_once(filepath, "nothing read")) {
					break;
				}
				pthread_mutex_lock(&audio_mutex);
				audio_command = AUDIO_CMD_STOP;
				playback_status = AUDIO_STATUS_STOPPED;
				pthread_mutex_unlock(&audio_mutex);
				break;
			}

			// Track completed naturally -- unless a stop or another track
			// arrived while the last read was blocked. That is not an end but
			// an interrupted read, and calling it "finished" advances the queue
			// on top of what the user just asked for.
			//
			// It happens with tracks played while still downloading:
			// abandoning the download makes the read in flight return empty.
			pthread_mutex_lock(&audio_mutex);
			bool natural_end = audio_command != AUDIO_CMD_STOP && !play_request;
			audio_command = AUDIO_CMD_STOP;
			playback_status = AUDIO_STATUS_STOPPED;
			track_completed = natural_end;
			pthread_mutex_unlock(&audio_mutex);
			played_to_the_end = natural_end;
			break;
		}

		snd_pcm_uframes_t frames_to_write = read_bytes / frame_bytes;
		if (fade_enabled) {
			int gain = fade_gain_q10((double)bytes_played / bytes_per_sec, (double)info.data_size / bytes_per_sec);
			int samples = (int)frames_to_write * info.channels;
			if (info.bits_per_sample > 16) {
				fade_apply_s32((int32_t *)buffer, samples, gain);
			} else {
				fade_apply_s16((short *)buffer, samples, gain);
			}
		}

		// The volume, when it is this side of the cable and not in a register:
		// a device on the USB-C port with no control of its own. A no-op, one
		// comparison deep, on every other route.
		if (swvolume_active()) {
			int samples = (int)frames_to_write * info.channels;
			if (info.bits_per_sample > 16) {
				swvolume_apply_s32((int32_t *)buffer, samples);
			} else {
				swvolume_apply_s16((short *)buffer, samples);
			}
		}

		snd_pcm_sframes_t written = pcm_write_recover(&pcm_handle, buffer, frames_to_write, info.channels,
													  info.sample_rate, info.bits_per_sample, &period_size);

		if (written == -EINTR) {
			continue; // a command is pending: the top of the loop handles it
		}
		if (written < 0 || !pcm_handle) {
			fprintf(stderr, "Audio: Write failed beyond recovery: %s\n", snd_strerror((int)written));
			if (retry_track_once(filepath, "write failure")) {
				break; // the thread loop picks the queued replay up
			}
			pthread_mutex_lock(&audio_mutex);
			audio_command = AUDIO_CMD_STOP;
			playback_status = AUDIO_STATUS_STOPPED;
			pthread_mutex_unlock(&audio_mutex);
			break;
		}

		bt_seek_probe_report(pcm_handle);
		bt_start_probe_note(pcm_handle, written, info.sample_rate);

		bytes_played += written * frame_bytes;
		host_pace(written, info.sample_rate);
		pthread_mutex_lock(&audio_mutex);
		progress_current_secs = (double)bytes_played / bytes_per_sec;
		pthread_mutex_unlock(&audio_mutex);
	}

	pthread_mutex_lock(&audio_mutex);
	mark_stopped_unless_replaced();
	pthread_mutex_unlock(&audio_mutex);

	// Leaving a track while the device is paused must not drain: snd_pcm_drain()
	// waits for a buffer that a paused device never empties, and the playback
	// thread would never come back for the next track.
	//
	// So the stream is drained only when the track really did reach its end;
	// every other exit is a switch to something else, and the tail of the old
	// track is not wanted.
	if (pcm_handle) {
		// Nothing to un-pause: a paused stream was dropped when the pause
		// began, so it is already stopped and safe to close.
		if (played_to_the_end) {
			// A track shorter than the buffer may never have reached the start
			// threshold: the stream is still PREPARED with the whole track
			// queued. Start it by hand, or the drain has nothing to flush and
			// those last moments are never heard.
			if (snd_pcm_state(pcm_handle) == SND_PCM_STATE_PREPARED) {
				snd_pcm_start(pcm_handle);
			}
			pcm_drain_bounded(pcm_handle);
		} else {
			snd_pcm_drop(pcm_handle);
			}
		snd_pcm_close(pcm_handle);
	}
	pcm_device_open = false;
	free(buffer);
	fclose(f);
}

// Play routine for compressed formats (MP3/FLAC/OGG Vorbis) via the decoder
// abstraction in decode.h. Always decodes to interleaved signed 16-bit PCM.
static void play_decoded_file(const char *filepath, decode_format_t format) {
	decoder_t *dec = decoder_open(filepath, format);
	if (!dec) {
		// A busy card (a cover decoding, the library scanning) can make the
		// first open fail transiently; one more try costs nothing and saves
		// the track.
		usleep(200 * 1000);
		dec = decoder_open(filepath, format);
	}

	// A file still downloading is not a broken file: it is not all there yet.
	//
	// A streamed track starts playing while it arrives, as soon as the cache
	// reports enough. Between that report and this open there are a couple of
	// thread hops, and now and then the decoder arrives a moment too early: the
	// FLAC header is not complete and decoder_open() refuses.
	//
	// So it waits: while the file grows, bytes are arriving and retrying is
	// worthwhile. The wait gives up on any command (another track, a stop, a
	// pause), so it blocks nothing.
	for (int wait = 0; !dec && growfile_is_growing(filepath) && wait < DECODER_GROW_RETRIES; wait++) {
		pthread_mutex_lock(&audio_mutex);
		bool leave = audio_command != AUDIO_CMD_PLAY || play_request || stop_thread;
		pthread_mutex_unlock(&audio_mutex);
		if (leave) {
			break;
		}
		if (wait == 0) {
			fprintf(stderr, "audio[%ld]: '%s' is still downloading, waiting instead of giving up\n", log_ms(),
					filepath);
		}
		usleep(DECODER_GROW_RETRY_MS * 1000);
		dec = decoder_open(filepath, format);
	}

	if (!dec) {
		fprintf(stderr, "Audio: Failed to open decoder for: %s\n", filepath);
		pthread_mutex_lock(&audio_mutex);
		audio_command = AUDIO_CMD_STOP;
		playback_status = AUDIO_STATUS_STOPPED;
		pthread_mutex_unlock(&audio_mutex);
		return;
	}

	int channels = decoder_channels(dec);
	int sample_rate = decoder_sample_rate(dec);
	uint64_t total_frames = decoder_total_pcm_frames(dec);
	int source_bits = decoder_source_bits(dec);

	// New track, clean start: the crossfeed delay line still holds half a
	// millisecond of the previous one, which would be audible over a quiet
	// opening.
	crossfeed_reset();

	// DoP: the frames are DSD bits with a marker byte the DAC watches for, not
	// audio. Nothing in the chain may touch them -- see decoder_passthrough().
	bool passthrough = decoder_passthrough(dec);
	int dsd_multiple = decoder_dsd_multiple(dec);

	// Hi-res sources go to the DAC at their real depth: a 24-bit FLAC plays
	// as S32 (samples left-justified, bit-perfect), the way the stock player
	// flips its stream to "PCM_FORMAT_PCM 32bit". Everything else stays 16.
	int out_bits = source_bits > 16 ? 32 : 16;

	pthread_mutex_lock(&audio_mutex);
	progress_total_secs = (double)total_frames / sample_rate;
	progress_current_secs = 0.0;
	progress_floor_secs = -1.0;
	stream_sample_rate = sample_rate;
	stream_channels = channels;
	stream_bits = source_bits;
	stream_dsd_multiple = dsd_multiple;
	stream_bitrate_kbps = decoder_bitrate_kbps(dec);
	stream_lossy = decoder_is_lossy(dec);
	snprintf(stream_codec, sizeof(stream_codec), "%s", decoder_codec_name(dec));
	// A lossy format that does not declare its bitrate: the player can only
	// print the format name. Worth logging, because on a streaming track there
	// is no other way to work it out -- the cache file is still growing.
	if (stream_lossy && stream_bitrate_kbps <= 0) {
		fprintf(stderr, "audio[%ld]: %s does not declare a bitrate\n", log_ms(), stream_codec);
	}
	// A track that is being restored comes up with PAUSE already queued: say
	// PAUSED right away rather than flashing through PLAYING, which would make
	// the LED change colour for a moment at every boot and the status bar show
	// a play glyph for a track that never started.
	playback_status = (audio_command == AUDIO_CMD_PAUSE) ? AUDIO_STATUS_PAUSED : AUDIO_STATUS_PLAYING;
	pthread_mutex_unlock(&audio_mutex);

	// The DAC has to be told before the stream starts, or the first periods
	// reach it while it is still reading the words as PCM -- which at DoP
	// levels is a bang.
	if (passthrough) {
		// Any held PCM goes first: a DAC about to switch to DSD must not still
		// have the previous track's PCM tail queued.
		gapless_release();
		set_dac_dop(1);
	}

	snd_pcm_uframes_t period_size;
	// The previous track's PCM, if it stayed open and suits this one too: that
	// is the whole of gapless. Never for DoP -- see the comment above
	// gapless_release().
	snd_pcm_t *pcm_handle = passthrough ? NULL : gapless_take(channels, sample_rate, out_bits, &period_size);
	if (pcm_handle) {
		pcm_device_open = true;
	} else {
		pcm_handle = open_pcm_device(channels, sample_rate, out_bits, &period_size);
	}
	if (!pcm_handle && out_bits == 32 && !passthrough) {
		// If this output cannot do S32 for some reason, 16-bit playback still
		// beats silence. Not for DoP: sixteen bits cannot carry a marker byte
		// and a twenty-four bit payload, so half a DoP stream is not quieter
		// DSD, it is noise.
		fprintf(stderr, "Audio: S32 open failed, falling back to 16-bit\n");
		out_bits = 16;
		pcm_handle = open_pcm_device(channels, sample_rate, out_bits, &period_size);
	}
	if (!pcm_handle && passthrough) {
		// DSD256 asks the card for 705.6 kHz, which an output that is not this
		// device's own DAC may simply refuse. There is nowhere to fall back to:
		// filtering the stream down to PCM here costs more than the whole core
		// at DSD256, so the track does not play and the log says why rather
		// than leaving a silent stop to be guessed at.
		fprintf(stderr, "audio: DoP at %d Hz refused by this output; the track cannot play\n", sample_rate);
	}
	if (!pcm_handle) {
		set_dac_dop(0);
		decoder_close(dec);
		pthread_mutex_lock(&audio_mutex);
		audio_command = AUDIO_CMD_STOP;
		playback_status = AUDIO_STATUS_STOPPED;
		pthread_mutex_unlock(&audio_mutex);
		return;
	}

	// From here, how long this track takes to start coming out is timed.
	bt_start_probe_arm(pcm_handle);

	double bytes_per_sec = (double)sample_rate * channels * (out_bits / 8);
	int frame_bytes = channels * (out_bits / 8);
	long paused_at_ms = 0; // when the current pause began (device power-down clock)
	snd_pcm_uframes_t chunk_frames = chunk_for(period_size, sample_rate);
	void *buffer = malloc(chunk_frames * frame_bytes);
	if (!buffer) {
		fprintf(stderr, "Audio: Out of memory for period buffer\n");
		snd_pcm_close(pcm_handle);
		set_dac_dop(0);
		decoder_close(dec);
		pthread_mutex_lock(&audio_mutex);
		audio_command = AUDIO_CMD_STOP;
		playback_status = AUDIO_STATUS_STOPPED;
		pthread_mutex_unlock(&audio_mutex);
		return;
	}

	// int64_t and not long: on the device (MIPS o32) long is 32 bit, and this
	// counts PCM bytes. At 44.1 kHz 16-bit stereo that is 176,400 bytes a
	// second, so a 32-bit sign flips after three and three quarter hours --
	// the middle of a long audiobook, not an edge case. A negative count makes
	// progress_current_secs negative and also decides whether a track finished
	// or was cut short (cut_short, below), so the queue would advance on its
	// own mid-book.
	int64_t bytes_played = 0;
	bool is_paused = false;
	bool played_to_the_end = false;
	int route_check = 0;
	int track_route = alsa_output_key(); // the output auto_set_output just applied

	// The time stretcher. Only on the 16-bit path: what it is for is
	// audiobooks, which are AAC and so always 16-bit, and a hi-res FLAC has no
	// business being played at 1.5x. NULL simply means the feature is not
	// available and playback runs at 1.0.
	speed_t *stretch = (out_bits == 16) ? speed_open(channels, sample_rate) : NULL;
	decoder_fill_t fill_ctx = {dec};

	while (1) {
		loop_turns++;
		pthread_mutex_lock(&audio_mutex);
		if (audio_command == AUDIO_CMD_STOP || play_request) {
			pthread_mutex_unlock(&audio_mutex);
			break;
		}
		if (seek_request) {
			double target = seek_target_secs;
			seek_request = false;
			pthread_mutex_unlock(&audio_mutex);

			uint64_t target_frame = (uint64_t)(target * sample_rate);
			if (target_frame > total_frames) {
				target_frame = total_frames;
			}

			// The requested point may not have downloaded yet. Clamping the
			// request to where the download has reached would mean asking for
			// 1:17 and landing at 0:51; a streaming app does the other thing --
			// it stops on the requested point, downloads up to it, and resumes
			// from there.
			//
			// So when the requested point is past the edge, wait for the edge
			// to reach it -- progress bar parked on the requested point, card
			// silent, and the wait giving up on any command (including another
			// seek: the last touch always wins).
			uint64_t reachable = decoder_seekable_limit(dec);
			if (reachable > 0 && target_frame > reachable && decoder_is_growing(dec)) {
				fprintf(stderr, "audio[%ld]: the requested point has not downloaded yet: waiting for it\n", log_ms());
				// Silence while waiting, when there is a stream to silence: a
				// seek can arrive with the device handed back.
				if (pcm_handle) {
					snd_pcm_drop(pcm_handle);
							}

				// The bar sits on the requested point: that is where this is going.
				pthread_mutex_lock(&audio_mutex);
				progress_current_secs = (double)target_frame / sample_rate;
				pthread_mutex_unlock(&audio_mutex);

				while (true) {
					pthread_mutex_lock(&audio_mutex);
					bool leave = audio_command != AUDIO_CMD_PLAY || play_request || seek_request;
					pthread_mutex_unlock(&audio_mutex);
					if (leave) {
						break; // the top of the loop handles it
					}
					if (!decoder_is_growing(dec)) {
						break; // finished (or died): go as far as possible
					}
					reachable = decoder_seekable_limit(dec);
					if (reachable >= target_frame) {
						fprintf(stderr, "audio[%ld]: the requested point has downloaded, resuming from there\n", log_ms());
						break;
					}
					usleep(100 * 1000);
				}
				reachable = decoder_seekable_limit(dec);
			}
			// As far as is reachable now: if the wait was interrupted (or the
			// download died) this is still the closest point to the requested
			// one that actually exists.
			if (reachable > 0 && target_frame > reachable) {
				target_frame = reachable;
			}

			// If the seek fails -- which happens on a file still downloading
			// when the requested point has not arrived -- go back to where the
			// decoder was rather than leaving it nowhere: the music carries on
			// from where it was, far better than playback dying for good.
			uint64_t was_at = (uint64_t)(bytes_played / frame_bytes);
			if (!decoder_seek_to_frame(dec, target_frame)) {
				if (decoder_seek_to_frame(dec, was_at)) {
					target_frame = was_at;
				}
			}
			// What it is holding belongs to the part of the book just left.
			speed_reset(stretch);
			// Same for the crossfeed delay line: it holds half a millisecond of
			// the point just left, which without this is heard crossing from
			// one ear to the other.
			crossfeed_reset();

			// And the starvation flag recorded before the seek: it belonged to
			// the old position (growfile_seek already clears it; this is the
			// second turn of the key for formats that seek by other routes).
			(void)decoder_take_starved(dec);

			// As in the WAV loop above: no device means no stream to flush, and
			// asking alsa to drop a null one aborts the audio thread.
			if (pcm_handle) {
				snd_pcm_drop(pcm_handle);
						snd_pcm_prepare(pcm_handle);
				is_paused = false;
				bt_seek_probe_arm(pcm_handle);
			}

			bytes_played = (int64_t)(target_frame * frame_bytes);
			pthread_mutex_lock(&audio_mutex);
			progress_current_secs = (double)bytes_played / bytes_per_sec;
			pthread_mutex_unlock(&audio_mutex);
			continue;
		}
		if (audio_command == AUDIO_CMD_PAUSE) {
			bool entering = !is_paused;
			if (entering) {
				is_paused = true;
				playback_status = AUDIO_STATUS_PAUSED;
			}
			pthread_mutex_unlock(&audio_mutex);

			if (entering) {
				int64_t rewind_bytes = pause_stop_stream(pcm_handle, frame_bytes);
				if (rewind_bytes > 0) {
					// The frames still queued were never heard: put the decoder
					// back over them so resuming carries on from where the
					// sound actually stopped instead of skipping a beat.
					uint64_t played_frames = (uint64_t)(bytes_played / frame_bytes);
					uint64_t rewind_frames = (uint64_t)(rewind_bytes / frame_bytes);
					uint64_t resume_frame = played_frames > rewind_frames ? played_frames - rewind_frames : 0;
					if (decoder_seek_to_frame(dec, resume_frame)) {
						bytes_played = (int64_t)(resume_frame * frame_bytes);
						pthread_mutex_lock(&audio_mutex);
						progress_floor_secs = progress_current_secs; // the bar stays put
						progress_current_secs = (double)bytes_played / bytes_per_sec;
						pthread_mutex_unlock(&audio_mutex);
					}
				}
			}

			pthread_mutex_lock(&audio_mutex);
			bool recover = resume_recover_pending;
			resume_recover_pending = false;
			pthread_mutex_unlock(&audio_mutex);
			if (recover && pcm_handle) {
				pcm_restart_after_pause(pcm_handle);
			}

			// Paused long enough: hand the device back so the DAC and the amp
			// power down. The decoder keeps its place; the unpause opens a
			// fresh stream and carries on from the same frame.
			if (entering) {
				paused_at_ms = log_ms();
			}
			if (pcm_handle && (log_ms() - paused_at_ms) >= paused_device_close_ms()) {
				snd_pcm_close(pcm_handle);
				pcm_handle = NULL;
				pcm_device_open = false;
				fprintf(stderr, "audio[%ld]: device released while paused (DAC powered down)\n", log_ms());
			}

			// With the device handed back there is nothing to be quick about:
			// a fifth of a second between checks instead of a twentieth means
			// four fewer wake-ups a second for however many hours the player
			// sits paused in a pocket. The unpause still feels instant.
			usleep(pcm_handle ? 50000 : 200000);
			continue;
		} else {
			// RESUME is one-shot: consumed here, back to PLAY -- see the twin
			// comment in the WAV loop. Leaving it set poisons every "is a
			// command pending?" test and the music comes out stuttering.
			if (audio_command == AUDIO_CMD_RESUME) {
				audio_command = AUDIO_CMD_PLAY;
			}
			if (is_paused) {
				// An unpause always reopens the device from scratch -- see the
				// twin comment in the WAV loop. Drop-at-pause plus
				// prepare-at-resume leaves this driver's stream in a bad state:
				// stuttering audio and a stuttering interface, since the
				// real-time audio thread never yields the CPU. Open-then-write
				// is the path every track start takes.
				if (pcm_handle) {
					snd_pcm_close(pcm_handle);
					pcm_handle = NULL;
					pcm_device_open = false;
					// Breathing room after the close -- like the 120 ms on a
					// track's way out and the 150 ms in pcm_reroute: on this
					// driver an open glued to a close comes back busy or
					// half-broken, and the stream stutters.
					usleep(150 * 1000);
				}
				// Route before stream, as play_file does at every start: during
				// the pause the codec may have gone to rest, and a PCM opened
				// over a half-asleep route stutters. No stream is live here, so
				// poking the codec cannot stall anything.
				auto_set_output();
				{
					snd_pcm_uframes_t new_period = period_size;
					pcm_handle = open_pcm_device(channels, sample_rate, out_bits, &new_period);
					if (!pcm_handle) {
						fprintf(stderr, "Audio: cannot reopen the device after a pause\n");
						audio_command = AUDIO_CMD_STOP;
						playback_status = AUDIO_STATUS_STOPPED;
						pthread_mutex_unlock(&audio_mutex);
						break;
					}
					snd_pcm_uframes_t new_chunk = chunk_for(new_period, sample_rate);
					if (new_chunk > chunk_frames) {
						void *bigger = realloc(buffer, new_chunk * frame_bytes);
						if (!bigger) {
							snd_pcm_close(pcm_handle);
							pcm_handle = NULL;
							pcm_device_open = false;
							audio_command = AUDIO_CMD_STOP;
							playback_status = AUDIO_STATUS_STOPPED;
							pthread_mutex_unlock(&audio_mutex);
							break;
						}
						buffer = bigger;
					}
					chunk_frames = new_chunk;
					period_size = new_period;
					fprintf(stderr, "audio[%ld]: device taken back for the unpause\n", log_ms());
					// A reopen is a fresh start for the sink as much as a new
					// track is, and the clock has to start again with it: left
					// armed from the track, the probe would charge the whole
					// pause to the restart.
					bt_start_probe_arm(pcm_handle);
				}
				is_paused = false;
				playback_status = AUDIO_STATUS_PLAYING;
			}
		}
		pthread_mutex_unlock(&audio_mutex);

		// Every ~32 buffers: has the jack changed?
		if (++route_check >= 32) {
			route_check = 0;
			int now_route = alsa_output_key();
			if (now_route != track_route) {
				track_route = now_route;
				if (!pcm_reroute(&pcm_handle, channels, sample_rate, out_bits, &period_size)) {
					fprintf(stderr, "Audio: reroute failed\n");
					pthread_mutex_lock(&audio_mutex);
					audio_command = AUDIO_CMD_STOP;
					playback_status = AUDIO_STATUS_STOPPED;
					pthread_mutex_unlock(&audio_mutex);
					break;
				}
			}
		}

		// --- the reserve of a track that is still downloading ----------------
		//
		// A streamed track plays while it downloads. Ahead of the play position
		// is a reserve of bytes already received; when the network slows down
		// the reserve is consumed and the decoder asks for data that is not
		// there. Trickling onwards means the read blocks, the card runs its
		// buffer dry and underruns, recovers and underruns again -- audible
		// stutter, and after eight consecutive attempts the write gives up and
		// the track dies.
		//
		// A streaming app instead stops, refills, and resumes. So does this.
		if (decoder_is_growing(dec)) {
			rebuffer_if_starved(dec, sample_rate, bytes_played / (uint64_t)frame_bytes);
		}

		// How many frames came out (what goes to the card) and how many went
		// in (what the position in the track advances by). The two are the
		// same at normal speed, and deliberately not at any other.
		uint64_t frames_read;
		uint64_t input_frames;
		if (out_bits == 32) {
			frames_read = decoder_read_pcm_frames_s32(dec, chunk_frames, (int32_t *)buffer);
			input_frames = frames_read;
			if (frames_read != 0 && !passthrough) {
				// ReplayGain first: it is a correction to the source level, so
				// everything after it works on the level the listener will
				// actually hear.
				replaygain_process_s32((int32_t *)buffer, (int)frames_read, channels);
				eq_process_s32((int32_t *)buffer, (int)frames_read, channels, sample_rate);
				soundfield_process_s32((int32_t *)buffer, (int)frames_read, channels);
				// Crossfeed after soundfield and before balance: one widens the
				// image, this pulls it back inside the head, and balance stays
				// the last word on what reaches each ear.
				crossfeed_process_s32((int32_t *)buffer, (int)frames_read, channels, sample_rate);
				balance_process_s32((int32_t *)buffer, (int)frames_read, channels);
			}
		} else {
			pthread_mutex_lock(&audio_mutex);
			double want_speed = playback_speed;
			pthread_mutex_unlock(&audio_mutex);

			if (stretch) {
				speed_set_factor(stretch, want_speed);
				frames_read = (uint64_t)speed_pull(stretch, (short *)buffer, (int)chunk_frames, decoder_fill, &fill_ctx,
												   &input_frames);
			} else {
				frames_read = decoder_read_pcm_frames_s16(dec, chunk_frames, (short *)buffer);
				input_frames = frames_read;
			}
			if (frames_read != 0) {
				replaygain_process((short *)buffer, (int)frames_read, channels);
				// The graphic EQ, in place on the decoded block. A no-op when off.
				eq_process((short *)buffer, (int)frames_read, channels, sample_rate);
				// ...then the stereo width, at the end of the chain.
				soundfield_process((short *)buffer, (int)frames_read, channels);
				// Crossfeed right after: what soundfield widened, this pulls
				// back inside the head.
				crossfeed_process((short *)buffer, (int)frames_read, channels, sample_rate);
				// Last in the chain, after everything that could have changed
				// the level: the balance is about what reaches each ear.
				balance_process((short *)buffer, (int)frames_read, channels);
			}
		}

		// Has the decoder reached the edge of the download? Two signals:
		//
		//   * the read came back short (the simple case);
		//   * the read came back full but had to wait (decoder_take_starved).
		//     growfile fills the read by waiting for chunks, so from here it
		//     looks normal -- except every read costs a tenth of a second and
		//     every write is an underrun.
		//
		// Either way, do not trickle onwards: stop and refill, like any
		// streaming app. (What the read already produced is still played below:
		// the gap starts after the last good sample.)
		if (decoder_is_growing(dec) && (frames_read < chunk_frames || decoder_take_starved(dec))) {
			rebuffer_after_short_read(dec, sample_rate, (uint64_t)(bytes_played / frame_bytes));
			if (frames_read == 0) {
				// Nothing at all came out: retry after refilling. Falling
				// through would declare "track finished" for a track merely
				// stuck at the edge. If the download did die, the next pass
				// sees a file that no longer grows and the cut-short branch
				// decides the ending.
				continue;
			}
		}

		if (frames_read == 0) {
			// First of all: the read can have come back empty not because the
			// file ended but because a pending command made it give up
			// (growfile's wake-up). Pause, stop, another track, a seek --
			// whichever it is, the top of the loop handles it: a pause must
			// pause, not end the track.
			pthread_mutex_lock(&audio_mutex);
			bool command_pending = audio_command != AUDIO_CMD_PLAY || play_request || seek_request;
			pthread_mutex_unlock(&audio_mutex);
			if (command_pending) {
				continue;
			}

			// Nothing at all came out of this file: that is a failed read, not
			// a track that finished. Reported as a completion, the UI would
			// auto-advance, the next file would read short too, and the queue
			// would walk on in silence. The card can be briefly unreadable
			// after a spell with the screen off (the same reason decoder_open
			// already retries), so the track gets one retry and then stops
			// WITHOUT claiming it played.
			if (bytes_played == 0) {
				fprintf(stderr, "audio[%ld]: '%s' returned no audio at all\n", log_ms(), filepath);
				if (retry_track_once(filepath, "nothing decoded")) {
					break;
				}
				pthread_mutex_lock(&audio_mutex);
				audio_command = AUDIO_CMD_STOP;
				playback_status = AUDIO_STATUS_STOPPED;
				pthread_mutex_unlock(&audio_mutex);
				break;
			}

			// End of track -- if it really is the end.
			//
			// The queue must never walk on by itself. It advances when a track
			// reaches its end and in no other case. Two lies that look like an
			// end are rejected here:
			//
			//   * a stop or another track arrived while blocked in the last
			//     read. That is an interrupted read, not an end: treating it as
			//     one sends the queue on top of what the user just asked for;
			//   * the file ended well short of its duration. This happens to
			//     tracks played while downloading: if the download dies, what is
			//     on disk stops halfway and the read comes back empty. It looks
			//     like the end of the track, but the track is twice as long.
			//
			// Without the second test the player walks itself through the whole
			// list: every truncated track advances the queue, and the next one --
			// not downloaded yet -- is truncated in turn.
			uint64_t declared = decoder_total_pcm_frames(dec);
			uint64_t reached = (uint64_t)(bytes_played / frame_bytes);
			bool cut_short = declared > 0 && sample_rate > 0 && reached + (uint64_t)sample_rate < declared;

			pthread_mutex_lock(&audio_mutex);
			bool natural_end = audio_command != AUDIO_CMD_STOP && !play_request && !cut_short;
			audio_command = AUDIO_CMD_STOP;
			playback_status = AUDIO_STATUS_STOPPED;
			track_completed = natural_end;
			pthread_mutex_unlock(&audio_mutex);
			played_to_the_end = natural_end;

			if (cut_short) {
				fprintf(stderr, "audio[%ld]: '%s' stopped at %.1f s of %.1f: cut short, the queue stays put\n", log_ms(),
						filepath, (double)reached / sample_rate, (double)declared / sample_rate);
			}
			break;
		}

		// The fade is a gain, so it is one more thing a DoP stream cannot have.
		if (fade_enabled && !passthrough) {
			int gain = fade_gain_q10((double)bytes_played / bytes_per_sec, (double)total_frames / sample_rate);
			int samples = (int)frames_read * channels;
			if (out_bits == 32) {
				fade_apply_s32((int32_t *)buffer, samples, gain);
			} else {
				fade_apply_s16((short *)buffer, samples, gain);
			}
		}

		// And the software volume, under the same rule and for the same reason:
		// scaling a DoP stream would stop it being DSD and start being noise.
		if (swvolume_active() && !passthrough) {
			int samples = (int)frames_read * channels;
			if (out_bits == 32) {
				swvolume_apply_s32((int32_t *)buffer, samples);
			} else {
				swvolume_apply_s16((short *)buffer, samples);
			}
		}

		snd_pcm_sframes_t written =
			pcm_write_recover(&pcm_handle, buffer, frames_read, channels, sample_rate, out_bits, &period_size);

		if (written == -EINTR) {
			// Not an error: a command (pause, stop, another track, a seek)
			// arrived while waiting for room in the buffer. The top of the
			// loop handles it; this block of samples is lost, which is right --
			// whoever pressed the key did not want to hear it.
			continue;
		}
		if (written < 0 || !pcm_handle) {
			fprintf(stderr, "Audio: Write failed beyond recovery: %s\n", snd_strerror((int)written));
			if (retry_track_once(filepath, "write failure")) {
				break; // the thread loop picks the queued replay up
			}
			pthread_mutex_lock(&audio_mutex);
			audio_command = AUDIO_CMD_STOP;
			playback_status = AUDIO_STATUS_STOPPED;
			pthread_mutex_unlock(&audio_mutex);
			break;
		}

		bt_seek_probe_report(pcm_handle);
		bt_start_probe_note(pcm_handle, written, sample_rate);

		// The position moves by INPUT frames: at 1.5x, one period written to
		// the card is a period and a half of the book. Scaled by how much of
		// the block the card actually took, for the rare partial write.
		if (frames_read > 0) {
			bytes_played += (int64_t)((double)written * frame_bytes * (double)input_frames / (double)frames_read);
		}
		host_pace(written, sample_rate);
		pthread_mutex_lock(&audio_mutex);
		progress_current_secs = (double)bytes_played / bytes_per_sec;
		pthread_mutex_unlock(&audio_mutex);
	}

	pthread_mutex_lock(&audio_mutex);
	mark_stopped_unless_replaced();
	pthread_mutex_unlock(&audio_mutex);

	// The track ended on its own, gapless is on and nothing special happened:
	// the PCM stays open with its queue inside and the next track appends to
	// it. See the comment above gapless_release() for the conditions and the
	// reason behind each.
	//
	// Never over Bluetooth, for the reason spelled out at paused_device_close_ms():
	// a bluealsa PCM is not a sound card. Its A2DP transport is released as soon
	// as the stream stops being fed, and writes after that are accepted by the
	// plugin and go nowhere -- so the held handle the next track appends to can
	// be a handle to nothing, and the album carries on in silence with the
	// status still reading PLAYING. Over Bluetooth a track change therefore
	// closes and reopens; the gap that costs is inaudible next to the link's
	// own latency.
	//
	// pcm_is_bluetooth(pcm_handle) and not output_is_bluetooth(): the second
	// reads the route installed right now, and the two disagree exactly when it
	// matters. Headphones dropping mid-track flip the route back to the jack
	// straight away (bluetooth.c, apply_output_routing), so a track ending a
	// moment later would ask about the wrong device and put a dead bluealsa
	// handle into the gapless hold.
	bool keep_open = pcm_handle && gapless_enabled && played_to_the_end && !is_paused && !passthrough &&
					 !pcm_is_bluetooth(pcm_handle);
	if (keep_open) {
		gapless_hold(pcm_handle, channels, sample_rate, out_bits, period_size);
		pcm_handle = NULL;
	}

	if (pcm_handle) {
		bool was_paused = is_paused;
		// Asked before the close, because afterwards there is no handle to ask.
		bool was_bluetooth = pcm_is_bluetooth(pcm_handle);
		// Nothing to un-pause: a paused stream was dropped when the pause
		// began, so it is already stopped and safe to close. Only a track that
		// reached its end is drained -- every other exit is a switch to
		// something else, and the tail of the old track is not wanted.
		if (played_to_the_end) {
			// A track shorter than the buffer may never have reached the start
			// threshold: the stream is still PREPARED with the whole track
			// queued. Start it by hand, or the drain has nothing to flush and
			// those last moments are never heard.
			if (snd_pcm_state(pcm_handle) == SND_PCM_STATE_PREPARED) {
				snd_pcm_start(pcm_handle);
			}
			pcm_drain_bounded(pcm_handle);
		} else {
			snd_pcm_drop(pcm_handle);
			}
		snd_pcm_close(pcm_handle);

		// A stream that sat paused (a track left paused while the screen was
		// off, then skipped) leaves the codec idle, and the driver wants a
		// moment before it will hand out the device again. Without this the
		// next track's open can come back busy, and that track never plays.
		if (was_paused) {
			usleep(120 * 1000);
		} else if (was_bluetooth) {
			// The same moment, for the same reason, at the other device. The
			// close above does not end at the plugin: bluealsa still has to
			// hand the A2DP transport back to bluez over D-Bus, and the next
			// track's open has to make it take it again. Reopening into the
			// middle of that is a race nobody wins, and on a card the identical
			// race is already waited out three lines up.
			//
			// A fifth of a second, against a link that already carries the best
			// part of a second of buffer: it does not add a gap anybody hears,
			// and it is not what makes Bluetooth not gapless -- the close and
			// reopen are.
			usleep(BT_REOPEN_SETTLE_MS * 1000);
		}
	}
	pcm_device_open = keep_open;
	pthread_mutex_lock(&audio_mutex);
	stream_dsd_multiple = 0;
	pthread_mutex_unlock(&audio_mutex);
	// Off again with the stream. A DAC left in DSD mode reads the next
	// ordinary track's words as bits and plays noise at full level, so this
	// belongs on the way out of every exit from this function, not only the
	// tidy one.
	if (!keep_open) {
		set_dac_dop(0);
	}
	free(buffer);
	speed_close(stretch);
	decoder_close(dec);
}

static void play_file(const char *filepath) {
	// One line in, one line out: with these, the log answers by itself
	// whether a track that "never started" was never picked up, failed to
	// open, or started and ended at once.
	fprintf(stderr, "audio[%ld]: starting '%s'\n", log_ms(), filepath);

	// And where the crash handler can reach it: a log ends at whatever was
	// flushed, and a process killed rather than faulting writes nothing at all,
	// so the name of the track has to be somewhere a signal handler can read it
	// without going through a stream.
	crumb_set_track(filepath);

	decode_format_t format = decode_detect_format(filepath);
	// The route, applied here: no PCM exists in this thread right now, so
	// the codec switch can never stall a live stream (decompile: the stock
	// remaps po/spdif via its engine's reinit flag, never on a live device).
	//
	// With gapless there can very much be one -- the PCM held open by the
	// previous track, its queue still playing. If the route changed (headphones
	// plugged in between the two tracks) that PCM must be closed before the
	// codec is reprogrammed, not after.
	if (gapless_holding() && alsa_output_key() != held_route) {
		gapless_release();
	}
	// Before the route is written, not only before the PCM is opened: writing
	// the codec route while another stream's PCM is open is the move that
	// wedges this driver (see pcm_reroute), and this is the one place on the
	// way into a track where nothing is holding a lock.
	release_external_output();
	auto_set_output();

	if (format == DECODE_FORMAT_UNKNOWN) {
		// The WAV path takes no part in gapless: it opens the device on its
		// own, and a held PCM would leave it busy.
		gapless_release();
		play_wav_file(filepath);
	} else {
		play_decoded_file(filepath, format);
	}

	pthread_mutex_lock(&audio_mutex);
	bool replaced = play_request;
	pthread_mutex_unlock(&audio_mutex);
	fprintf(stderr, "audio[%ld]: '%s' ended (%s)\n", log_ms(), filepath,
			replaced ? "replaced by a new track" : "stop or end of track");
}

static void *playback_thread_func(void *arg) {
	// The name, and only the name -- not thread_be_background(), which also puts
	// the caller on SCHED_IDLE, and this is the one thread that must never wait
	// behind anything. /proc/self/task/*/comm is what the watchdog's dump
	// prints, and a stall report that says "sonix_player_ho" three times over
	// does not say which thread stopped.
	prctl(PR_SET_NAME, "playback", 0, 0, 0);

	// This thread runs the decoders, so it is the one a malformed file can
	// recurse to the bottom of its stack: give it somewhere for the crash
	// handler to run.
	thread_signal_stack();

	// Real-time round-robin, modestly: the UI decoding a 9 MB cover on the
	// one core must never starve the thread that feeds the DAC. Failing is
	// fine (the host build has no privilege for it).
	struct sched_param rt = {.sched_priority = 10};
	if (pthread_setschedparam(pthread_self(), SCHED_RR, &rt) == 0) {
		fprintf(stderr, "audio: playback thread running at RT priority\n");
	}

	while (1) {
		pthread_mutex_lock(&audio_mutex);
		while (!play_request && !stop_thread) {
			// Suspend-to-RAM is about to start: any held PCM has to be closed
			// now, not in GAPLESS_HOLD_MS. Only this thread may close it, so
			// the request lands here.
			if (hold_flush_request) {
				hold_flush_request = false;
				pthread_mutex_unlock(&audio_mutex);
				gapless_release();
				pthread_mutex_lock(&audio_mutex);
				continue;
			}
			if (!gapless_holding()) {
				pthread_cond_wait(&audio_cond, &audio_mutex);
				continue;
			}

			// With a PCM held open (gapless) the wait is bounded: if the next
			// track does not arrive soon, what the card has queued runs out and
			// the PCM must be closed instead of sitting there holding the DAC.
			struct timespec until;
			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_nsec += (long)GAPLESS_HOLD_MS * 1000000L;
			until.tv_sec += until.tv_nsec / 1000000000L;
			until.tv_nsec %= 1000000000L;

			if (pthread_cond_timedwait(&audio_cond, &audio_mutex, &until) == ETIMEDOUT) {
				// Outside the lock: closing a PCM can mean waiting for the
				// card to finish, and the interface must not queue behind that.
				pthread_mutex_unlock(&audio_mutex);
				gapless_release();
				pthread_mutex_lock(&audio_mutex);
			}
		}
		if (stop_thread) {
			pthread_mutex_unlock(&audio_mutex);
			// A PCM held open by gapless does not go away on its own.
			gapless_release();
			break;
		}

		char filepath[512];
		strncpy(filepath, current_filepath, sizeof(filepath));
		play_request = false;
		play_request_ms = 0;
		playback_context_active = true;
		pthread_mutex_unlock(&audio_mutex);

		play_file(filepath);

		pthread_mutex_lock(&audio_mutex);
		playback_context_active = false;
		pthread_mutex_unlock(&audio_mutex);
	}
	return NULL;
}

// What growfile asks every tenth of a second while the audio thread waits for
// a chunk that is not coming: is there a reason to give up? A stop, a pause,
// another track queued, or a seek. Without it the thread stays in that wait for
// up to twenty seconds after the key was pressed, and from outside the player
// looks dead: prev does nothing and local tracks will not start.
static bool playback_reader_should_abort(void) {
	pthread_mutex_lock(&audio_mutex);
	bool out = audio_command != AUDIO_CMD_PLAY || play_request || stop_thread || seek_request;
	pthread_mutex_unlock(&audio_mutex);
	return out;
}

int audio_init(void) {
	static bool initialized = false;
	if (initialized)
		return 0;

	growfile_set_reader_abort_cb(playback_reader_should_abort);

	set_volume_percent(40); // a sane, audible default on the new dB taper
	auto_set_output();

	pthread_mutex_lock(&audio_mutex);
	stop_thread = false;
	play_request = false;
	play_request_ms = 0;
	audio_command = AUDIO_CMD_STOP;
	playback_status = AUDIO_STATUS_STOPPED;
	pthread_mutex_unlock(&audio_mutex);

	int err = pthread_create(&playback_thread, NULL, playback_thread_func, NULL);
	if (err != 0) {
		fprintf(stderr, "Audio: Failed to create background thread\n");
		return -1;
	}

	initialized = true;
	return 0;
}

int audio_play(const char *filepath) {
	fprintf(stderr, "audio[%ld]: play requested '%s'\n", log_ms(), filepath);

	// NO route write here: this runs on the UI thread while the previous
	// track's PCM may still be open, and poking "Output Port Switch" with a
	// configured stream up stalls the DMA (the after-headphone-plug wedge).
	// The playback thread routes at track start, the stock engine's way.

	pthread_mutex_lock(&audio_mutex);
	restart_fresh_pending = false; // an explicit play beats the armed restart
	strncpy(current_filepath, filepath, sizeof(current_filepath) - 1);
	play_request = true;
	play_request_ms = log_ms();
	write_fail_retry_used = false;
	audio_command = AUDIO_CMD_PLAY;
	playback_status = AUDIO_STATUS_PLAYING;

	// Zero the progress now instead of leaving the outgoing track's numbers up
	// until the thread has opened the new file: the UI polls this at any
	// moment, and stale values make the bar jump back to where the previous
	// track was before snapping to the new one.
	progress_current_secs = 0.0;
	progress_floor_secs = -1.0;
	progress_total_secs = 0.0;
	stream_sample_rate = 0;
	stream_channels = 0;
	seek_request = false;
	seek_target_secs = -1.0;
	stream_bits = 0;
	stream_bitrate_kbps = 0;
	stream_lossy = false;
	stream_codec[0] = '\0';
	track_completed = false; // fresh playback; any prior completion is consumed
	pthread_cond_signal(&audio_cond);
	pthread_mutex_unlock(&audio_mutex);
	return 0;
}

// Play a file starting somewhere other than its beginning: an audiobook picked
// up where it was left. Same shape as audio_play_paused() -- which comes up
// PAUSED at `start_secs` for the boot-time "remember track" restore, consuming
// the queued PAUSE and the queued seek before a frame is written, so nothing
// is heard until play is pressed. Both are functions of their own rather than
// a play followed by a seek because both halves are set under one hold of the
// mutex: otherwise the playback thread could open the file and push its first
// second out of the speakers before the seek reached it.
int audio_play_at(const char *filepath, double start_secs) {
	fprintf(stderr, "audio[%ld]: play requested '%s' from %.1f s\n", log_ms(), filepath, start_secs);

	pthread_mutex_lock(&audio_mutex);
	restart_fresh_pending = false; // an explicit play beats the armed restart
	strncpy(current_filepath, filepath, sizeof(current_filepath) - 1);
	current_filepath[sizeof(current_filepath) - 1] = '\0';
	play_request = true;
	play_request_ms = log_ms();
	write_fail_retry_used = false;
	audio_command = AUDIO_CMD_PLAY;
	playback_status = AUDIO_STATUS_PLAYING;
	if (start_secs > 0) {
		seek_request = true;
		seek_target_secs = start_secs;
	}
	progress_current_secs = start_secs > 0 ? start_secs : 0;
	progress_floor_secs = -1.0;
	progress_total_secs = 0;
	pthread_mutex_unlock(&audio_mutex);

	pthread_cond_signal(&audio_cond);
	return 0;
}

int audio_play_paused(const char *filepath, double start_secs) {
	fprintf(stderr, "audio[%ld]: restore requested '%s' at %.1f s (paused)\n", log_ms(), filepath, start_secs);

	pthread_mutex_lock(&audio_mutex);
	restart_fresh_pending = false; // an explicit play beats the armed restart
	strncpy(current_filepath, filepath, sizeof(current_filepath) - 1);
	current_filepath[sizeof(current_filepath) - 1] = '\0';
	play_request = true;
	play_request_ms = log_ms();
	write_fail_retry_used = false;
	audio_command = AUDIO_CMD_PAUSE;
	playback_status = AUDIO_STATUS_PAUSED;
	if (start_secs > 0) {
		seek_request = true;
		seek_target_secs = start_secs;
	}
	progress_current_secs = start_secs > 0 ? start_secs : 0;
	progress_floor_secs = -1.0;
	progress_total_secs = 0;
	pthread_mutex_unlock(&audio_mutex);

	pthread_cond_signal(&audio_cond);
	return 0;
}

// audio_notify_resume() below is told by the power manager the moment the SoC
// comes back from suspend: the stream a paused track left open is SUSPENDED by
// then. The playback thread does the recovery itself rather than having ALSA
// poked from the interface thread.
bool audio_device_is_open(void) { return pcm_device_open; }

// How long the pending play request has been waiting, or 0 when there is none.
//
// Read without taking audio_mutex on purpose: the caller is the watchdog, and
// the case it exists to report is the playback thread not coming back -- which
// on the unpause path means it is inside snd_pcm_open() with audio_mutex held.
// A watchdog that blocks on the very lock it is investigating reports nothing.
// Both values are a word wide on this target, so the worst a lock-free read can
// see is the state from an instant ago.
long audio_play_request_age_ms(void) {
	if (!play_request || play_request_ms == 0) {
		return 0;
	}
	long age = log_ms() - play_request_ms;
	return age > 0 ? age : 0;
}

void audio_notify_resume(void) {
	pthread_mutex_lock(&audio_mutex);
	resume_recover_pending = true;
	pthread_mutex_unlock(&audio_mutex);
}

// Everything down before mem. Reverse engineering of the stock binary shows
// its standby routine issues STOP, waits for the STOPPED state and only then
// writes `mem`: no ALSA object may exist while the SoC stops, because the AIC
// inside the SoC loses its registers and a handle that outlives it is the
// shortest path to the reboot on the first play. This forces the gapless PCM
// closed and waits -- with a cap -- for the playback thread to release the
// device. Returns true once nothing is open. It does not stop the music: the
// caller must already have decided that (the prototype only suspends when
// stopped).
bool audio_suspend_quiesce(int timeout_ms) {
	pthread_mutex_lock(&audio_mutex);
	hold_flush_request = true;
	pthread_cond_broadcast(&audio_cond);
	pthread_mutex_unlock(&audio_mutex);

	for (int waited = 0; waited < timeout_ms; waited += 20) {
		if (!pcm_device_open && !gapless_holding()) {
			return true;
		}
		usleep(20 * 1000);
	}
	return !pcm_device_open && !gapless_holding();
}

bool audio_playback_context_active(void) { return playback_context_active; }

// The real freeze, for mem. ALSA objects are not enough: a paused track keeps
// play_file() alive with the decoder and a FILE* open on the card, the card
// loses power in mem, and the first play after wake -- an unpause of that
// context -- reboots the device. This does what the stock binary does: a real
// STOP, a wait for the context to be genuinely dead (play_file returned, not
// merely the status changed), and "next play = fresh restart" armed with the
// saved path and position.
//
// The logical state is left at PAUSED when there was a track, so the play
// button still reads as "resume", and that resume -- intercepted in
// audio_resume() -- becomes the fresh start from the right point.
bool audio_suspend_freeze(int timeout_ms) {
	pthread_mutex_lock(&audio_mutex);
	bool was_paused = playback_status == AUDIO_STATUS_PAUSED;
	char path[512];
	snprintf(path, sizeof(path), "%s", current_filepath);
	double pos = progress_current_secs;
	double total = progress_total_secs;

	// STOP, exactly as the user would send it, and a wake-up for any waiter.
	audio_command = AUDIO_CMD_STOP;
	playback_status = AUDIO_STATUS_STOPPED;
	seek_request = false;
	seek_target_secs = -1.0;
	track_completed = false;
	hold_flush_request = true;
	pthread_cond_broadcast(&audio_cond);
	pthread_mutex_unlock(&audio_mutex);

	// Wait for the real teardown: context dead, PCM closed, gapless empty.
	bool clean = false;
	for (int waited = 0; waited <= timeout_ms && !clean; waited += 20) {
		clean = !playback_context_active && !pcm_device_open && !gapless_holding();
		if (!clean) {
			usleep(20 * 1000);
		}
	}
	if (!clean) {
		fprintf(stderr, "audio: freeze failed (context=%d pcm=%d gapless=%d)\n",
				(int)playback_context_active, (int)pcm_device_open, (int)gapless_holding());
		return false;
	}

	if (was_paused && path[0]) {
		pthread_mutex_lock(&audio_mutex);
		restart_fresh_pending = true;
		snprintf(restart_fresh_path, sizeof(restart_fresh_path), "%s", path);
		restart_fresh_pos = pos;
		// The facade stays "paused where you were": the context underneath is
		// dead, but the next play rebuilds it.
		playback_status = AUDIO_STATUS_PAUSED;
		snprintf(current_filepath, sizeof(current_filepath), "%s", path);
		progress_current_secs = pos;
		progress_total_secs = total;
		pthread_mutex_unlock(&audio_mutex);
		fprintf(stderr, "audio: frozen while paused: '%s' at %.1f s, fresh restart armed\n", path, pos);
	} else {
		fprintf(stderr, "audio: frozen while stopped\n");
	}
	return true;
}

void audio_pause(void) {
	printf("pausing\n");
	pthread_mutex_lock(&audio_mutex);
	audio_command = AUDIO_CMD_PAUSE;

	// Report the new state immediately instead of waiting for the playback
	// thread to notice the command. It only looks at it between PCM writes, so
	// for up to a period's worth of audio the status would still say PLAYING --
	// and a second tap in that window would ask for a pause again instead of a
	// resume, leaving the button stuck on the play glyph.
	playback_status = AUDIO_STATUS_PAUSED;
	pthread_mutex_unlock(&audio_mutex);
}

void audio_resume(void) {
	// The fresh restart, when the freeze armed one: the track context no longer
	// exists (killed before mem, deliberately), so "resume" here means
	// rebuilding everything from scratch at the right point -- fresh decoder,
	// fresh route, fresh PCM. That is the same path every track start takes.
	pthread_mutex_lock(&audio_mutex);
	if (restart_fresh_pending && restart_fresh_path[0]) {
		char path[512];
		snprintf(path, sizeof(path), "%s", restart_fresh_path);
		double pos = restart_fresh_pos;
		restart_fresh_pending = false;
		pthread_mutex_unlock(&audio_mutex);
		fprintf(stderr, "audio: resume after mem -> fresh start of '%s' at %.1f s\n", path, pos);
		audio_play_at(path, pos);
		return;
	}
	pthread_mutex_unlock(&audio_mutex);

	printf("resuming\n");

	pthread_mutex_lock(&audio_mutex);
	audio_command = AUDIO_CMD_RESUME;

	// Same reasoning as audio_pause(): the intent is the truth until the
	// playback thread catches up with it.
	if (playback_status == AUDIO_STATUS_PAUSED) {
		playback_status = AUDIO_STATUS_PLAYING;
	}
	pthread_mutex_unlock(&audio_mutex);
}

void audio_stop(void) {
	printf("stopping\n");
	pthread_mutex_lock(&audio_mutex);
	audio_command = AUDIO_CMD_STOP;
	playback_status = AUDIO_STATUS_STOPPED;
	seek_request = false;
	seek_target_secs = -1.0;
	track_completed = false; // deliberate stop is not a natural completion
	pthread_mutex_unlock(&audio_mutex);
}

audio_status_t audio_get_status(void) {
	pthread_mutex_lock(&audio_mutex);
	audio_status_t status = playback_status;
	pthread_mutex_unlock(&audio_mutex);
	return status;
}

bool audio_take_completion(void) {
	pthread_mutex_lock(&audio_mutex);
	bool completed = track_completed;
	track_completed = false;
	pthread_mutex_unlock(&audio_mutex);
	return completed;
}

void audio_get_current_file(char *out, size_t out_size) {
	if (!out || out_size == 0)
		return;
	pthread_mutex_lock(&audio_mutex);
	strncpy(out, current_filepath, out_size - 1);
	out[out_size - 1] = '\0';
	pthread_mutex_unlock(&audio_mutex);
}

void audio_get_progress(double *current_secs, double *total_secs) {
	pthread_mutex_lock(&audio_mutex);
	double current = progress_current_secs;
	if (progress_floor_secs >= 0.0) {
		if (current >= progress_floor_secs) {
			progress_floor_secs = -1.0; // playback has caught up
		} else {
			current = progress_floor_secs;
		}
	}
	if (current_secs)
		*current_secs = current;
	if (total_secs)
		*total_secs = progress_total_secs;
	pthread_mutex_unlock(&audio_mutex);
}

void audio_get_stream_info(int *sample_rate, int *channels) {
	pthread_mutex_lock(&audio_mutex);
	if (sample_rate)
		*sample_rate = stream_sample_rate;
	if (channels)
		*channels = stream_channels;
	pthread_mutex_unlock(&audio_mutex);
}

int audio_get_stream_bits(void) {
	pthread_mutex_lock(&audio_mutex);
	int bits = stream_bits;
	pthread_mutex_unlock(&audio_mutex);
	return bits;
}

int audio_get_stream_bitrate_kbps(void) {
	pthread_mutex_lock(&audio_mutex);
	int kbps = stream_bitrate_kbps;
	pthread_mutex_unlock(&audio_mutex);
	return kbps;
}

bool audio_stream_is_lossy(void) {
	pthread_mutex_lock(&audio_mutex);
	bool lossy = stream_lossy;
	pthread_mutex_unlock(&audio_mutex);
	return lossy;
}

void audio_get_stream_codec(char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	pthread_mutex_lock(&audio_mutex);
	snprintf(out, size, "%s", stream_codec);
	pthread_mutex_unlock(&audio_mutex);
}

int audio_get_dsd_multiple(void) {
	pthread_mutex_lock(&audio_mutex);
	int m = stream_dsd_multiple;
	pthread_mutex_unlock(&audio_mutex);
	return m;
}

// Playback speed, for audiobooks. Applied on the next block, so it can be
// changed while a book is playing without a gap.
void audio_set_speed(double factor) {
	pthread_mutex_lock(&audio_mutex);
	playback_speed = factor;
	pthread_mutex_unlock(&audio_mutex);
}

double audio_get_speed(void) {
	pthread_mutex_lock(&audio_mutex);
	double value = playback_speed;
	pthread_mutex_unlock(&audio_mutex);
	return value;
}

void audio_seek(double seconds) {
	printf("seeking to %.2f seconds\n", seconds);

	pthread_mutex_lock(&audio_mutex);
	progress_current_secs = seconds; // the bar follows the finger, not the decoder
	progress_floor_secs = -1.0;		// and a deliberate jump outranks the pause floor
	seek_target_secs = seconds;
	seek_request = true;
	pthread_mutex_unlock(&audio_mutex);
}

// ---------------------------------------------------------------------------
// where the sound goes
// ---------------------------------------------------------------------------

void audio_set_output_device(const char *pcm) {
	pthread_mutex_lock(&output_lock);
	char before[sizeof(output_pcm)];
	snprintf(before, sizeof(before), "%s", output_pcm);
	if (pcm && pcm[0]) {
		snprintf(output_pcm, sizeof(output_pcm), "%s", pcm);
	} else {
		snprintf(output_pcm, sizeof(output_pcm), "%s", AUDIO_DEFAULT_PCM);
	}
	// Only when it really moved, and on stderr. This line is how a log answers
	// "did the sound stop going to the headphones, or did it stop coming out of
	// them" -- so it must not be one of twenty identical lines, and it must not
	// be sitting in a block buffer when the interesting moment passes.
	char after[sizeof(output_pcm)];
	snprintf(after, sizeof(after), "%s", output_pcm);
	pthread_mutex_unlock(&output_lock);

	if (strcmp(before, after) != 0) {
		fprintf(stderr, "audio[%ld]: the output moves from '%s' to '%s'\n", log_ms(), before, after);
	}
}

void audio_get_output_device(char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	pthread_mutex_lock(&output_lock);
	snprintf(out, out_size, "%s", output_pcm);
	pthread_mutex_unlock(&output_lock);
}

// ---------------------------------------------------------------------------
// Somebody else's audio
//
// AirPlay is the case this exists for. shairport does not open a PCM -- it has
// no ALSA backend at all, its `-o ot` backend is a unix-socket client -- so it
// decodes ALAC and hands the player raw 44100/16/stereo frames to play. The
// stock firmware does exactly the same thing, feeding them into the same output
// chain local playback uses.
//
// This is deliberately NOT part of the playback thread: that thread's whole
// shape is "open a file, decode it, write it", and there is no file here. It is
// the device, opened by the same open_pcm_device() so the routing, the retries
// and the fall back to the jack all behave identically, and written to by
// whoever is holding the other end.
//
// The caller must stop local playback first. Two writers on one PCM is not a
// thing ALSA does.
// ---------------------------------------------------------------------------

static int external_frame_bytes = 4;
static pthread_mutex_t external_lock = PTHREAD_MUTEX_INITIALIZER;

// What the open was made on, so a change can be noticed.
//
// Local playback reads alsa_output_key() every chunk and re-routes when it
// moves. An external source has no decode loop to do that in, and the two
// things the output depends on are only read when a PCM is opened:
//
//   the route ("Output Port Switch"), which is what sends the sound to the
//   4.4 mm socket rather than the 3.5 mm one, and
//
//   the ALSA device name, which is what sends it to a USB-C DAC rather than
//   to the internal one.
//
// Without watching both, plugging anything in while a Bluetooth receiver,
// AirPlay or the emulator is running leaves the stream coming out of whatever
// was in use when it started, and the socket the headphones are actually in
// silent.
static int external_rate;
static int external_channels;
static int external_bits;
static int external_buffer_ms;
static int external_route = -1;			// alsa_output_key() at the open
static char external_device[160];		// the ALSA name at the open
static long external_checked_ms;		// when the two above were last compared

// How often the check below is worth making. The jack is a sysfs read and the
// device name a string compare under a mutex, so it is not expensive -- but at
// 44.1 kHz a receiver hands over a chunk every eleven milliseconds, and doing
// it ninety times a second on a single core buys nothing. A quarter of a
// second is faster than a plug goes in.
#define EXTERNAL_ROUTE_CHECK_MS 250

// Under external_lock. Applies the route, opens, and records both.
static bool external_open_locked(void) {
	// The route first, as play_file() does it. Skipping it would inherit
	// whatever route the last local track left behind -- or, on a player that
	// has not played anything since boot, none at all.
	auto_set_output();

	pcm_forced_buffer_ms = external_buffer_ms;
	snd_pcm_uframes_t period = 0;
	external_pcm = open_pcm_device(external_channels, external_rate, external_bits, &period);
	pcm_forced_buffer_ms = 0;

	if (!external_pcm) {
		return false;
	}
	pcm_device_open = true;
	external_route = alsa_output_key();
	audio_get_output_device(external_device, sizeof(external_device));
	return true;
}

bool audio_external_begin(int sample_rate, int channels, int bits) {
	return audio_external_begin_latency(sample_rate, channels, bits, 0);
}

bool audio_external_begin_latency(int sample_rate, int channels, int bits, int buffer_ms) {
	// A PCM held by gapless occupies the DAC. Callers here want to open one of
	// their own on the same device, and without this they find it busy right
	// after a track has ended on its own.
	gapless_release();

	pthread_mutex_lock(&external_lock);

	if (external_pcm) {
		pthread_mutex_unlock(&external_lock);
		return true; // already open, and on the same terms
	}

	// One frame is one sample per channel. 24-bit is the odd one out -- ALSA's
	// S24_3LE packs it into three bytes, not four -- and this is what a partial
	// write advances by, so getting it wrong would swap the channels for the
	// rest of the stream rather than merely sounding wrong once.
	external_frame_bytes = channels * (bits == 24 ? 3 : bits / 8);

	// Remembered rather than merely used: a re-open after the headphones move
	// has to ask for exactly the same stream. The buffer among them, because
	// the emulator's short one is the whole of how it keeps time.
	external_rate = sample_rate;
	external_channels = channels;
	external_bits = bits;
	external_buffer_ms = buffer_ms;
	external_checked_ms = log_ms();

	bool ok = external_open_locked();

	printf("audio: external source %s (%d Hz, %d ch, %d bits)\n", ok ? "playing" : "COULD NOT OPEN", sample_rate,
		   channels, bits);

	pthread_mutex_unlock(&external_lock);
	return ok;
}

// Under external_lock. Re-opens when the headphones have moved, and says
// whether there is still a PCM to write to.
static bool external_follow_output(void) {
	if (log_ms() - external_checked_ms < EXTERNAL_ROUTE_CHECK_MS) {
		return true;
	}
	external_checked_ms = log_ms();

	char device[sizeof(external_device)];
	audio_get_output_device(device, sizeof(device));
	int route = alsa_output_key();

	if (route == external_route && strcmp(device, external_device) == 0) {
		return true;
	}

	fprintf(stderr, "audio: the output moved under the external source (route %d -> %d, '%s' -> '%s'); re-opening\n",
			external_route, route, external_device, device);

	// Dropped rather than drained: what is still in the buffer belongs to the
	// socket nobody is listening to any more, and draining it would play it
	// there before the change took effect.
	snd_pcm_drop(external_pcm);
	snd_pcm_close(external_pcm);
	external_pcm = NULL;
	pcm_device_open = false;

	if (!external_open_locked()) {
		// The new device would not open. Saying so once is enough: the check
		// runs again in a quarter of a second, and the next plug -- or the
		// same one seated properly -- is another go at it.
		fprintf(stderr, "audio: the new output would not open; the external source has nowhere to write\n");
		return false;
	}
	return true;
}

int audio_external_write(const void *frames, int count) {
	pthread_mutex_lock(&external_lock);

	if (!external_pcm || count <= 0) {
		pthread_mutex_unlock(&external_lock);
		return 0;
	}

	// The headphones may have moved since the last chunk. This is the only
	// place that runs often enough to notice: an external source has no decode
	// loop of its own, and nothing else re-opens a PCM it did not open.
	if (!external_follow_output()) {
		pthread_mutex_unlock(&external_lock);
		return 0;
	}

	int written = 0;
	while (written < count) {
		snd_pcm_sframes_t got = snd_pcm_writei(external_pcm, (const char *)frames + (size_t)written * external_frame_bytes,
											   (snd_pcm_uframes_t)(count - written));
		if (got == -EPIPE) {
			// An underrun: the sender stalled, which over a wireless link is
			// ordinary. Prepare and carry on rather than tearing the stream
			// down over a gap.
			snd_pcm_prepare(external_pcm);
			continue;
		}
		if (got == -ESTRPIPE) {
			while (snd_pcm_resume(external_pcm) == -EAGAIN) {
				usleep(100 * 1000);
			}
			snd_pcm_prepare(external_pcm);
			continue;
		}
		if (got < 0) {
			fprintf(stderr, "audio: external write failed: %s\n", snd_strerror((int)got));
			break;
		}
		written += (int)got;
	}

	pthread_mutex_unlock(&external_lock);
	return written;
}

void audio_external_end(void) {
	pthread_mutex_lock(&external_lock);

	if (external_pcm) {
		snd_pcm_drop(external_pcm);
		snd_pcm_close(external_pcm);
		external_pcm = NULL;
		pcm_device_open = false;
		external_route = -1;
		external_device[0] = '\0';
		printf("audio: external source finished; device handed back\n");
	}

	pthread_mutex_unlock(&external_lock);
}
