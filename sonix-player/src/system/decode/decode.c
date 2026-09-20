#include "decode.h"

#include "src/system/library/cue.h"

#include "src/system/decode/growfile.h"

#include "src/system/core/utils.h"

#include "aacdec.h"
#include "alacdec.h"
#include "dsd.h"
#include "mp4.h"
#include "opusdec.h"
#include "sndfile.h"
#include "wavpackdec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#include "stb_vorbis_decl.h"

// ---------------------------------------------------------------------------
// AAC-in-MP4
//
// The container hands over one access unit at a time and the decoder gives
// back one frame of PCM for each, so the only state worth keeping is which
// access unit comes next and what is left over from the last frame the caller
// did not finish reading.
// ---------------------------------------------------------------------------

// Enough for any frame the AAC family produces (4096 for USAC), at up to eight
// channels. One allocation for as long as the file is open.
#define AAC_PCM_CAPACITY (4096 * 8)

typedef struct {
	mp4_file_t *mp4;
	aacdec_t *aac;

	uint32_t frame_index; // the next access unit to decode
	uint32_t frame_total;

	unsigned char *au; // the access unit being handed to the decoder
	uint32_t au_capacity;

	short *pcm;	   // what came out of the last one
	int pcm_frames;
	int pcm_read;

	// After a seek playback restarts a couple of access units early, because
	// the first frame or two out of a decoder that has just been cleared are
	// not yet right, and the PCM before the requested point is then dropped.
	uint64_t skip_frames;
} aac_state_t;

// ---------------------------------------------------------------------------
// ALAC-in-MP4
//
// The same container as AAC and the same shape -- one access unit in, one
// frame of PCM out -- with two differences that matter. The samples are 32-bit
// because ALAC is lossless and often 24-bit; and each frame decodes on its
// own, independent of the one before, so a seek needs no run-up.
// ---------------------------------------------------------------------------

typedef struct {
	mp4_file_t *mp4;
	alacdec_t *alac;

	uint32_t frame_index; // the next access unit to decode
	uint32_t frame_total;

	unsigned char *au;
	uint32_t au_capacity;

	int32_t *pcm; // what came out of the last one
	int pcm_capacity_frames;
	int pcm_frames;
	int pcm_read;

	uint64_t skip_frames; // how much to drop after a seek
} alac_state_t;

// ---------------------------------------------------------------------------
// Raw AAC (ADTS)
//
// A .aac has no container: it is the bare sequence of frames, each preceded by
// seven bytes of ADTS header giving its length, sample rate and channels. Handy
// for a radio stream, awkward for a file: no table says how long it is or where
// the second minute starts.
//
// So on open the file is walked once, header to header and decoding nothing, to
// count the frames (hence the duration) and note where a frame starts every
// ADTS_MARK_STRIDE. With that index a seek is one fseek plus a few frames of
// run-up instead of re-reading the track from the start.
//
// The duration is corrected after the first decoded frame: in HE-AAC the header
// declares the core rate and the decoder returns twice it, so only the decoder
// knows the real samples per frame (aacdec_frame_size).
// ---------------------------------------------------------------------------

#define ADTS_HEADER_BYTES 7
#define ADTS_MARK_STRIDE 32		 // one bookmark every so many frames
#define ADTS_MAX_MARKS 8192		 // ~7 hours at 1024 samples: past that, thin out
#define ADTS_SEEK_RUNUP 2		 // frames of run-up after a seek
#define ADTS_IN_CHUNK 8192		 // how much is fed to the decoder at a time
#define ADTS_PCM_CAPACITY (2048 * 8) // one HE-AAC frame, eight channels

typedef struct {
	FILE *f;
	aacdec_t *aac;

	long first_frame_at; // where the stream starts (past any ID3 tag)
	uint32_t frame_total;
	int header_rate;	 // the rate the header declares
	int header_channels;

	long *marks; // offset of frame number i * mark_stride
	int mark_count;
	int mark_stride;

	unsigned char in[ADTS_IN_CHUNK];
	int in_have;
	int in_used;

	short *pcm;
	int pcm_frames;
	int pcm_read;

	uint64_t skip_frames; // how much to drop after a seek
	bool sized;			  // duration already rescaled to the real frame size
} adts_state_t;

// The rates the ADTS index can name.
static const int ADTS_RATES[16] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
								   16000, 12000, 11025, 8000,  7350,  0,	 0,	    0};

// Returns the frame's total length (header included) if the header is valid,
// otherwise 0.
static int adts_frame_length(const unsigned char *h) {
	if (h[0] != 0xFF || (h[1] & 0xF0) != 0xF0) {
		return 0; // no sync word
	}
	int length = (int)(((uint32_t)(h[3] & 0x03) << 11) | ((uint32_t)h[4] << 3) | ((uint32_t)h[5] >> 5));
	// A frame shorter than its own header does not exist, and neither does an
	// eight-kilobyte one: those are the two ways a random byte pretends to be
	// a header.
	if (length <= ADTS_HEADER_BYTES || length > 8192) {
		return 0;
	}
	return length;
}

static void adts_state_free(adts_state_t *s) {
	if (!s) {
		return;
	}
	aacdec_close(s->aac);
	if (s->f) {
		fclose(s->f);
	}
	free(s->marks);
	free(s->pcm);
	free(s);
}

// Skips the ID3v2 tag nearly every .aac carries in front.
static long adts_skip_id3(FILE *f) {
	unsigned char head[10];
	if (fread(head, 1, sizeof(head), f) != sizeof(head) || memcmp(head, "ID3", 3) != 0) {
		rewind(f);
		return 0;
	}
	long size = (long)(((uint32_t)(head[6] & 0x7F) << 21) | ((uint32_t)(head[7] & 0x7F) << 14) |
					   ((uint32_t)(head[8] & 0x7F) << 7) | (uint32_t)(head[9] & 0x7F));
	long at = 10 + size;
	if (size < 0 || fseek(f, at, SEEK_SET) != 0) {
		rewind(f);
		return 0;
	}
	return at;
}

// The walk over the headers. Reads in blocks and steps through memory: one
// fseek per frame on an SD card would be slower than reading the whole file.
static bool adts_scan(adts_state_t *s) {
	if (fseek(s->f, s->first_frame_at, SEEK_SET) != 0) {
		return false;
	}

	s->mark_stride = ADTS_MARK_STRIDE;
	s->mark_count = 0;
	int mark_capacity = 0;

	unsigned char buf[ADTS_IN_CHUNK];
	int have = 0;
	long frame_at = s->first_frame_at;
	uint32_t count = 0;

	for (;;) {
		if (have < ADTS_HEADER_BYTES) {
			size_t got = fread(buf + have, 1, sizeof(buf) - (size_t)have, s->f);
			have += (int)got;
			if (have < ADTS_HEADER_BYTES) {
				break; // out of file
			}
		}

		int length = adts_frame_length(buf);
		if (length == 0) {
			break; // the chain broke: what has been counted still stands
		}

		if (count == 0) {
			int index = (buf[2] >> 2) & 0x0F;
			s->header_rate = ADTS_RATES[index];
			s->header_channels = (int)(((buf[2] & 0x01) << 2) | ((buf[3] >> 6) & 0x03));
			if (s->header_rate <= 0) {
				return false; // not an AAC stream this can read
			}
		}

		// A bookmark, every so many frames.
		if ((count % (uint32_t)s->mark_stride) == 0) {
			if (s->mark_count == mark_capacity) {
				int grown = mark_capacity ? mark_capacity * 2 : 256;
				long *bigger = realloc(s->marks, (size_t)grown * sizeof(*bigger));
				if (!bigger) {
					break; // without bookmarks it still plays, it just seeks worse
				}
				s->marks = bigger;
				mark_capacity = grown;
			}
			s->marks[s->mark_count++] = frame_at;

			// A very long file must not grow the index without bound: keep
			// every second bookmark and double the stride.
			if (s->mark_count >= ADTS_MAX_MARKS) {
				for (int i = 1; i * 2 < s->mark_count; i++) {
					s->marks[i] = s->marks[i * 2];
				}
				s->mark_count = (s->mark_count + 1) / 2;
				s->mark_stride *= 2;
			}
		}

		count++;
		frame_at += length;

		if (length <= have) {
			memmove(buf, buf + length, (size_t)(have - length));
			have -= length;
		} else {
			// The frame runs past the block: jump to the next one and start
			// reading again from there.
			if (fseek(s->f, frame_at, SEEK_SET) != 0) {
				break;
			}
			have = 0;
		}
	}

	s->frame_total = count;
	return count > 0;
}

// ---------------------------------------------------------------------------
// Files that are still downloading
//
// A Qobuz track starts playing while it arrives. dr_flac and dr_mp3 can read
// through callbacks instead of from a path, and that is all it takes:
// growfile.c answers a read by waiting for the missing piece instead of
// reporting end of file as soon as it reaches the end of what is there.
//
// There are two sets of adapters only because the two libraries have two
// enumerations for the seek origin; they mean the same thing.
// ---------------------------------------------------------------------------

static size_t grow_read_flac(void *user, void *out, size_t bytes) {
	return growfile_read((growfile_t *)user, out, bytes);
}

static drflac_bool32 grow_seek_flac(void *user, int offset, drflac_seek_origin origin) {
	// DRFLAC_SEEK_END never arrives on a growing file -- the end is not
	// written yet -- and refusing it is the right answer: dr_flac falls back
	// on what it already knows.
	if (origin == DRFLAC_SEEK_END) {
		return DRFLAC_FALSE;
	}
	return growfile_seek((growfile_t *)user, (long)offset, origin == DRFLAC_SEEK_CUR ? 1 : 0) ? DRFLAC_TRUE
																							 : DRFLAC_FALSE;
}

static drflac_bool32 grow_tell_flac(void *user, drflac_int64 *cursor) {
	if (!cursor) {
		return DRFLAC_FALSE;
	}
	*cursor = growfile_tell((growfile_t *)user);
	return DRFLAC_TRUE;
}

static size_t grow_read_mp3(void *user, void *out, size_t bytes) {
	return growfile_read((growfile_t *)user, out, bytes);
}

static drmp3_bool32 grow_seek_mp3(void *user, int offset, drmp3_seek_origin origin) {
	if (origin == DRMP3_SEEK_END) {
		return DRMP3_FALSE;
	}
	return growfile_seek((growfile_t *)user, (long)offset, origin == DRMP3_SEEK_CUR ? 1 : 0) ? DRMP3_TRUE
																							: DRMP3_FALSE;
}

static drmp3_bool32 grow_tell_mp3(void *user, drmp3_int64 *cursor) {
	if (!cursor) {
		return DRMP3_FALSE;
	}
	*cursor = growfile_tell((growfile_t *)user);
	return DRMP3_TRUE;
}

// ---------------------------------------------------------------------------
// The MP3 frame index
//
// Without an index, seeking inside an MP3 costs a full decode from the current
// position to the target: MP3 frames have no fixed length written anywhere, so
// dr_mp3 can only find them by reading one after another, and on this processor
// that stalls the audio thread for seconds.
//
// The index takes the decode out of that loop. The file is read once looking at
// headers only, noting where a frame begins every MP3_INDEX_SECONDS of music;
// dr_mp3 then jumps to the nearest noted point and decodes at most that much.
//
// Built on the first seek rather than at open, so playing a track straight
// through costs no extra read before the first note.
// ---------------------------------------------------------------------------

// How much music between points. Two seconds is the compromise: the run-up
// left to dr_mp3 is imperceptible, and a four-minute track costs a hundred and
// twenty entries.
#define MP3_INDEX_SECONDS 2

// The ceiling: past it, points thin out instead of multiplying. 2048 entries
// are about fifty kilobytes, held for as long as the track is open.
#define MP3_INDEX_MAX_POINTS 2048

// A cold restart does not land on the frame one would expect.
//
// An MP3 frame does not necessarily hold its own data: the bit reservoir lets
// the encoder leave part of it in earlier frames, and main_data_begin says how
// many bytes back to go for it. A freshly cleared decoder lacks those bytes and
// returns zero samples, so the caller swallows that frame uncounted. How many
// get swallowed is decidable from the headers alone (main_data_begin against
// what the previous frame leaves behind); mp3_cold_start_t does that arithmetic
// during the scan, so each point is noted on the frame the decoder really
// reaches.
//
// Hence a single frame of run-up: dr_mp3 decodes the last run-up frame into the
// output buffer instead of discarding it, and reads on from there.
#define MP3_INDEX_DISCARD 1

// The reservoir ceiling, as written in dr_mp3.
#define MP3_RESERVOIR_MAX 511

// How many frames a candidate is followed for before giving up. Three or four
// are enough in practice: as soon as one frame gets through, the reservoir is
// full and every frame after it gets through too.
#define MP3_COLD_MAX_FRAMES 32

typedef struct {
	drmp3_seek_point *points;
	drmp3_uint32 count;
	long scanned_bytes; // how much file there was when it was built
	bool failed;		// already tried and impossible: do not retry on every touch
} mp3_index_t;

struct decoder {
	decode_format_t format;
	int channels;
	int sample_rate;
	uint64_t total_pcm_frames;

	// The path, for anything that has to re-read the file on its own without
	// disturbing the decoder's position (the MP3 frame index). Wider than a
	// file path because a CUE track's path is the sheet plus "?track=NN".
	char path[600];

	// The nominal bitrate of the compressed stream, in kbps: what is written
	// in the file, not what comes out of dividing bytes by seconds. 0 when the
	// format has none (the lossless ones) or the file does not say.
	int bitrate_kbps;

	mp3_index_t mp3_index;

	union {
		drmp3 mp3;
		drflac *flac;
		stb_vorbis *vorbis;
		aac_state_t *aac;
		alac_state_t *alac;
		dsd_file_t *dsd;
		sndfile_t *snd;
		opusdec_t *opus;
		wavpackdec_t *wv;
		adts_state_t *adts;
	} impl;

	// Non-NULL when the file was still being written at open. Closed after the
	// decoder, which reads through it right up to the end.
	growfile_t *growing;

	// One track of a CUE sheet: a window onto a file that holds a whole disc.
	// The window is applied at this boundary rather than in each format's
	// branch, so everything above -- duration, progress, seeking, the
	// cut-short guard -- sees an ordinary track that starts at zero.
	// cue_frames is 0 when the file is played whole.
	uint64_t cue_begin;  // first frame of the track inside the file
	uint64_t cue_frames; // how many frames the track lasts
	uint64_t cue_read;   // frames handed out since the window opened
};

// ".alac" is a label, not a container.
//
// Whoever writes it means "there is ALAC in here", which names a codec and not
// a way of packing it: the same samples can sit in an MP4 (as in an ordinary
// .m4a) or in a CAF, and the converters that use this name produce either
// without saying which. So these files are identified from their first bytes
// rather than their name: "ftyp" at byte four is an MP4, "caff" at the start is
// a CAF.
//
// When the file will not even open the answer is MP4, by far the commoner case:
// this function is sometimes handed a bare name with no path (albumart.c uses
// it that way to pick a folder's cover) and there is nothing to sniff.
static decode_format_t alac_container_of(const char *filepath) {
	unsigned char head[8];
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		return DECODE_FORMAT_AAC_MP4;
	}
	size_t got = fread(head, 1, sizeof(head), f);
	fclose(f);

	if (got == sizeof(head) && memcmp(head, "caff", 4) == 0) {
		return sndfile_available() ? DECODE_FORMAT_SNDFILE : DECODE_FORMAT_UNKNOWN;
	}
	return DECODE_FORMAT_AAC_MP4;
}

decode_format_t decode_detect_format(const char *filepath) {
	// A track of a CUE sheet is named by the sheet; what decides the format is
	// the audio file the sheet points at.
	char sheet[512];
	if (cue_split_path(filepath, sheet, sizeof(sheet)) > 0) {
		// Thirty-five kilobytes, and this function calls itself once.
		cue_sheet_t *cue = malloc(sizeof(*cue));
		if (!cue) {
			return DECODE_FORMAT_UNKNOWN;
		}
		if (!cue_parse(sheet, cue)) {
			free(cue);
			return DECODE_FORMAT_UNKNOWN;
		}
		decode_format_t inner = decode_detect_format(cue->audio_path);
		free(cue);
		// A disc ripped to one .wav is the common case, and plain WAV has its
		// own path in audio.c rather than a decoder. That path plays a file
		// whole, which is the opposite of what a sheet asks for, so the track
		// goes to libsndfile instead -- it reads WAV, and it is a decoder, so
		// the window below applies to it like any other.
		if (inner == DECODE_FORMAT_UNKNOWN && sndfile_available()) {
			inner = DECODE_FORMAT_SNDFILE;
		}
		return inner;
	}

	if (has_extension(filepath, ".mp3"))
		return DECODE_FORMAT_MP3;
	if (has_extension(filepath, ".alac"))
		return alac_container_of(filepath);
	if (has_extension(filepath, ".flac"))
		return DECODE_FORMAT_FLAC;
	if (has_extension(filepath, ".ogg"))
		return DECODE_FORMAT_OGG_VORBIS;
	if (has_extension(filepath, ".m4b") || has_extension(filepath, ".m4a") || has_extension(filepath, ".mp4"))
		return DECODE_FORMAT_AAC_MP4;
	if (has_extension(filepath, ".dsf") || has_extension(filepath, ".dff"))
		return DECODE_FORMAT_DSD;
	if (has_extension(filepath, ".opus"))
		return DECODE_FORMAT_OPUS;
	if (has_extension(filepath, ".wv"))
		return DECODE_FORMAT_WAVPACK;
	if (has_extension(filepath, ".aac"))
		return DECODE_FORMAT_AAC_ADTS;
	// Last, and only if the library is really there: an unplayable extension
	// should stay unknown rather than become a file that fails to open.
	if (sndfile_handles(filepath) && sndfile_available())
		return DECODE_FORMAT_SNDFILE;
	return DECODE_FORMAT_UNKNOWN;
}

// Feeds bytes to the decoder until a frame of PCM comes out.
static bool adts_fill(decoder_t *dec) {
	adts_state_t *s = dec->impl.adts;

	for (;;) {
		// First take whatever the decoder is already holding.
		int frames = aacdec_pull(s->aac, s->pcm, ADTS_PCM_CAPACITY);
		if (frames > 0) {
			if (dec->channels <= 0) {
				dec->channels = aacdec_channels(s->aac);
				dec->sample_rate = aacdec_sample_rate(s->aac);
			}
			// The duration, recomputed once from what the decoder really
			// produces: HE-AAC gives 2048 samples per frame, not 1024, at
			// twice the declared rate.
			if (!s->sized) {
				int per_frame = aacdec_frame_size(s->aac);
				if (per_frame > 0) {
					dec->total_pcm_frames = (uint64_t)s->frame_total * (uint64_t)per_frame;
				}
				s->sized = true;
			}

			s->pcm_frames = frames;
			s->pcm_read = 0;

			if (s->skip_frames) {
				int drop = (s->skip_frames > (uint64_t)frames) ? frames : (int)s->skip_frames;
				s->pcm_read = drop;
				s->skip_frames -= (uint64_t)drop;
				if (s->pcm_read >= s->pcm_frames) {
					continue;
				}
			}
			return true;
		}

		// Nothing held: feed it more.
		if (s->in_used >= s->in_have) {
			size_t got = fread(s->in, 1, sizeof(s->in), s->f);
			if (got == 0) {
				return false; // end of file
			}
			s->in_have = (int)got;
			s->in_used = 0;
		}

		int taken = aacdec_fill(s->aac, s->in + s->in_used, s->in_have - s->in_used);
		if (taken <= 0) {
			return false; // the decoder refused: no way forward
		}
		s->in_used += taken;
	}
}

static void aac_state_free(aac_state_t *s) {
	if (!s)
		return;
	aacdec_close(s->aac);
	mp4_close(s->mp4);
	free(s->au);
	free(s->pcm);
	free(s);
}

// Pulls the next access unit out of the container and pushes it through the
// decoder. Returns false at the end of the file (or on a container error, which
// for a book means the same thing: stop).
static bool aac_fill(decoder_t *dec) {
	aac_state_t *s = dec->impl.aac;

	while (s->frame_index < s->frame_total) {
		int len = mp4_read_audio_frame(s->mp4, s->frame_index, s->au, s->au_capacity);
		if (len < 0) {
			// The buffer was too small; the container said exactly how small,
			// so grow once and take the same frame again.
			uint32_t want = (uint32_t)(-len) + 256;
			unsigned char *grown = realloc(s->au, want);
			if (!grown)
				return false;
			s->au = grown;
			s->au_capacity = want;
			continue;
		}
		if (len == 0)
			return false;

		s->frame_index++;
		int frames = aacdec_decode(s->aac, s->au, len, s->pcm, AAC_PCM_CAPACITY);
		if (frames < 0)
			return false;
		if (frames == 0)
			continue; // the decoder is still priming

		if (dec->channels <= 0) {
			dec->channels = aacdec_channels(s->aac);
			dec->sample_rate = aacdec_sample_rate(s->aac);
		}

		s->pcm_frames = frames;
		s->pcm_read = 0;

		if (s->skip_frames) {
			int drop = (s->skip_frames > (uint64_t)frames) ? frames : (int)s->skip_frames;
			s->pcm_read = drop;
			s->skip_frames -= (uint64_t)drop;
			if (s->pcm_read >= s->pcm_frames)
				continue;
		}
		return true;
	}
	return false;
}

static void alac_state_free(alac_state_t *s) {
	if (!s)
		return;
	alacdec_close(s->alac);
	mp4_close(s->mp4);
	free(s->au);
	free(s->pcm);
	free(s);
}

// Like aac_fill(), without the priming: an ALAC frame does not depend on the
// one before, so the first frame out is already good.
static bool alac_fill(decoder_t *dec) {
	alac_state_t *s = dec->impl.alac;

	while (s->frame_index < s->frame_total) {
		int len = mp4_read_audio_frame(s->mp4, s->frame_index, s->au, s->au_capacity);
		if (len < 0) {
			uint32_t want = (uint32_t)(-len) + 256;
			unsigned char *grown = realloc(s->au, want);
			if (!grown)
				return false;
			s->au = grown;
			s->au_capacity = want;
			continue;
		}
		if (len == 0)
			return false;

		s->frame_index++;
		int frames = alacdec_decode(s->alac, s->au, len, s->pcm, s->pcm_capacity_frames);
		if (frames <= 0)
			continue; // empty or unreadable frame: carry on

		s->pcm_frames = frames;
		s->pcm_read = 0;

		if (s->skip_frames) {
			int drop = (s->skip_frames > (uint64_t)frames) ? frames : (int)s->skip_frames;
			s->pcm_read = drop;
			s->skip_frames -= (uint64_t)drop;
			if (s->pcm_read >= s->pcm_frames)
				continue;
		}
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// The MP3 frame index (see the comment above mp3_index_t)
// ---------------------------------------------------------------------------

// How many bytes of the file can be read right now. For an ordinary file its
// size; for one still downloading, how far it has got -- past that there is
// nothing, and reading it would mean waiting.
static long mp3_available_bytes(decoder_t *dec) {
	if (dec->growing) {
		long done = 0, total = 0;
		if (growfile_span(dec->growing, &done, &total) && total > 0) {
			return done;
		}
	}
	FILE *f = fopen(dec->path, "rb");
	if (!f) {
		return 0;
	}
	long size = 0;
	if (fseek(f, 0, SEEK_END) == 0) {
		size = ftell(f);
	}
	fclose(f);
	return size > 0 ? size : 0;
}

// A sliding window over the file, to avoid one fseek+fread of four bytes per
// frame: that is nine thousand pairs of calls on a four-minute track, and the
// library has no way to know the next jump nearly always lands inside the
// buffer it already holds.
typedef struct {
	FILE *f;
	unsigned char buf[64 * 1024];
	long at;	// file position of the buffer's first byte
	size_t len; // how many valid bytes it holds
} mp3_window_t;

// Returns a pointer to the `want` bytes starting at `pos`, or NULL when they
// are not there (end of file, or end of what has been downloaded).
static const unsigned char *mp3_window_at(mp3_window_t *w, long pos, size_t want, long avail) {
	if (want > sizeof(w->buf) || pos < 0 || pos + (long)want > avail) {
		return NULL;
	}
	if (pos < w->at || pos + (long)want > w->at + (long)w->len) {
		if (fseek(w->f, pos, SEEK_SET) != 0) {
			return NULL;
		}
		size_t room = sizeof(w->buf);
		if ((long)room > avail - pos) {
			room = (size_t)(avail - pos);
		}
		w->len = fread(w->buf, 1, room, w->f);
		w->at = pos;
		if (w->len < want) {
			return NULL;
		}
	}
	return w->buf + (pos - w->at);
}

// What this frame asks of the reservoir (main_data_begin) and what it leaves
// behind. All of it is in the header and the side info that follows -- no
// decoding.
//
// main_data_begin is the first bits of the side info, which starts right after
// the header (plus two CRC bytes when present): nine bits on MPEG1, eight on
// the other versions.
static void mp3_reservoir_needs(const unsigned char *h, int frame_bytes, int *wants, int *leaves) {
	*wants = 0;
	*leaves = 0;

	// The reservoir exists only in Layer III. The layer bits read 3 for I,
	// 2 for II, 1 for III: the other two layers are self-contained.
	if (DRMP3_HDR_GET_LAYER(h) != 1) {
		*leaves = MP3_RESERVOIR_MAX;
		return;
	}

	int crc = DRMP3_HDR_IS_CRC(h) ? 2 : 0;
	int side = DRMP3_HDR_TEST_MPEG1(h) ? (DRMP3_HDR_IS_MONO(h) ? 17 : 32) : (DRMP3_HDR_IS_MONO(h) ? 9 : 17);
	int off = 4 + crc;

	*wants = DRMP3_HDR_TEST_MPEG1(h) ? ((h[off] << 1) | (h[off + 1] >> 7)) : h[off];
	*leaves = frame_bytes - 4 - crc - side;
	if (*leaves < 0) {
		*leaves = 0;
	}
}

// The arithmetic of a cold restart from a given frame.
typedef struct {
	bool pending;  // a candidate is being followed
	long byte_pos; // where the candidate starts
	int reserv;	   // what is in the reservoir, following minimp3's own rules
	int walked;	   // how many frames have been looked at
} mp3_cold_start_t;

static void mp3_index_free(mp3_index_t *ix) {
	free(ix->points);
	ix->points = NULL;
	ix->count = 0;
	ix->scanned_bytes = 0;
}

// Reads the file once looking only at headers and notes a point every
// MP3_INDEX_SECONDS of music. False when that is impossible, which leaves
// dr_mp3's brute-force search in place: it works, it is only slow.
static bool mp3_index_build(decoder_t *dec) {
	mp3_index_t *ix = &dec->mp3_index;
	if (ix->failed || !dec->path[0] || dec->sample_rate <= 0) {
		return false;
	}

	long avail = mp3_available_bytes(dec);
	long start = (long)dec->impl.mp3.streamStartOffset;
	if (avail <= start + 4) {
		return false;
	}

	mp3_window_t *w = calloc(1, sizeof(*w));
	if (!w) {
		return false;
	}
	w->at = -1;
	w->f = fopen(dec->path, "rb");
	if (!w->f) {
		free(w);
		ix->failed = true;
		return false;
	}

	drmp3_seek_point *pts = malloc(sizeof(*pts) * MP3_INDEX_MAX_POINTS);
	if (!pts) {
		fclose(w->f);
		free(w);
		return false;
	}

	// The first point is the real start of the audio -- past the ID3 and past
	// the Xing/Info frame, exactly where dr_mp3 puts zero. Without it, a seek
	// before the second point falls into dr_mp3's "no valid point" branch,
	// which restarts from byte zero of the file: inside the tag, not the audio.
	pts[0].seekPosInBytes = (drmp3_uint64)start;
	pts[0].pcmFrameIndex = 0;
	pts[0].mp3FramesToDiscard = 0;
	pts[0].pcmFramesToDiscard = 0;
	drmp3_uint32 count = 1;

	uint64_t spacing = (uint64_t)dec->sample_rate * MP3_INDEX_SECONDS;
	if (dec->total_pcm_frames > 0) {
		uint64_t widest = dec->total_pcm_frames / MP3_INDEX_MAX_POINTS + 1;
		if (widest > spacing) {
			spacing = widest; // a very long track thins out instead of overflowing
		}
	}

	long pos = start;
	uint64_t pcm = 0; // samples since the start of the audio
	uint64_t delay = dec->impl.mp3.delayInPCMFrames;
	uint64_t next_target = spacing;
	unsigned char first[4] = {0};
	mp3_cold_start_t cold = {0};

	while (count < MP3_INDEX_MAX_POINTS) {
		// Eight bytes, not four: besides the header this needs the start of the
		// side info, where main_data_begin decides whether a restart from this
		// frame is possible.
		const unsigned char *h = mp3_window_at(w, pos, 8, avail);
		if (!h) {
			break; // out of what is there
		}

		if (!drmp3_hdr_valid(h) || (first[0] && !drmp3_hdr_compare(first, h))) {
			// A header that does not match nearly always means a tag wedged in
			// the middle (APE, a trailing ID3v1). Try to resynchronise a little
			// further on; failing that, stop and keep what has been indexed so
			// far -- it is still valid.
			long limit = pos + 8192;
			long back = pos;
			bool found = false;
			for (pos++; pos < limit; pos++) {
				const unsigned char *p = mp3_window_at(w, pos, 8, avail);
				if (!p) {
					break;
				}
				if (drmp3_hdr_valid(p) && (!first[0] || drmp3_hdr_compare(first, p))) {
					found = true;
					break;
				}
			}
			if (!found) {
				pos = back;
				break;
			}
			cold.pending = false; // the earlier candidate no longer holds
			continue;
		}

		if (!first[0]) {
			memcpy(first, h, 4);
		}

		int bytes = drmp3_hdr_frame_bytes(h, 0) + drmp3_hdr_padding(h);
		unsigned samples = drmp3_hdr_frame_samples(h);
		if (bytes <= 4 || samples == 0) {
			break; // free format: the length is not in the header
		}

		int wants = 0, leaves = 0;
		mp3_reservoir_needs(h, bytes, &wants, &leaves);

		// Enough music has passed since the last point: take this frame as the
		// candidate to restart the decoder from. Where it will really land is
		// what the loop below works out.
		if (!cold.pending && pcm >= next_target && pcm > delay) {
			cold.pending = true;
			cold.byte_pos = pos;
			cold.reserv = 0;
			cold.walked = 0;
		}

		if (cold.pending) {
			if (cold.reserv >= wants) {
				// The first frame a decoder restarted cold from cold.byte_pos
				// can really decode: that is the one it will end up holding.
				//
				// The `- delay`: dr_mp3 counts the encoder's priming samples in
				// currentPCMFrame, but a read never returns them -- it skips
				// them at the head of the stream silently. A brute-force seek
				// to sample T gets there by reading T output samples, and so
				// ends up `delay` samples further on in the raw count. For the
				// index to land exactly where brute force landed, the point has
				// to be declared on the same scale.
				drmp3_seek_point *p = &pts[count++];
				p->seekPosInBytes = (drmp3_uint64)cold.byte_pos;
				p->pcmFrameIndex = pcm - delay;
				p->mp3FramesToDiscard = MP3_INDEX_DISCARD;
				p->pcmFramesToDiscard = 0;
				next_target = pcm + spacing;
				cold.pending = false;
			} else {
				// It did not make it: minimp3 swallows the frame and moves on,
				// and the reservoir fills with what was there plus what this
				// frame carries (with the decode skipped, all of it stays).
				int have = cold.reserv < wants ? cold.reserv : wants;
				cold.reserv = have + leaves;
				if (cold.reserv > MP3_RESERVOIR_MAX) {
					cold.reserv = MP3_RESERVOIR_MAX;
				}
				if (++cold.walked >= MP3_COLD_MAX_FRAMES) {
					cold.pending = false; // no way out: try again further on
				}
			}
		}

		pcm += samples;
		pos += bytes;
	}

	fclose(w->f);
	free(w);

	if (count < 2) {
		// Not one point past the start: useless, and retrying on every touch
		// would cost a read of the file for nothing.
		free(pts);
		ix->failed = true;
		return false;
	}

	drmp3_seek_point *shrunk = realloc(pts, sizeof(*pts) * count);
	if (shrunk) {
		pts = shrunk;
	}

	// Unbind the old table before freeing it: dr_mp3 holds the pointer without
	// owning it.
	drmp3_bind_seek_table(&dec->impl.mp3, 0, NULL);
	mp3_index_free(ix);

	ix->points = pts;
	ix->count = count;
	ix->scanned_bytes = pos;
	drmp3_bind_seek_table(&dec->impl.mp3, count, pts);
	return true;
}

// The bitrate of the first header, plain -- on a VBR file too, where it is not
// "the" bitrate but is still the only figure available without reading
// everything.
static int mp3_first_frame_kbps(decoder_t *dec) {
	FILE *f = fopen(dec->path, "rb");
	if (!f) {
		return 0;
	}
	unsigned char h[4] = {0};
	int kbps = 0;
	if (fseek(f, (long)dec->impl.mp3.streamStartOffset, SEEK_SET) == 0 && fread(h, 1, 4, f) == 4 &&
		drmp3_hdr_valid(h)) {
		kbps = (int)drmp3_hdr_bitrate_kbps(h);
	}
	fclose(f);
	return kbps;
}

// The average bitrate of the first frames, walking header to header inside the
// part already on disk. On a CBR it is the bitrate, identical to the first
// frame's; on a VBR the first frame alone lies -- encoders nearly always start
// on a quiet passage, hence at the bottom of the scale -- and a hundred frames
// are enough not to be badly wrong. Costs one read of 256 KB already
// downloaded.
#define MP3_HEAD_FRAMES 200
#define MP3_HEAD_BYTES (256 * 1024)

static int mp3_head_average_kbps(decoder_t *dec) {
	FILE *f = fopen(dec->path, "rb");
	if (!f) {
		return 0;
	}

	// Borrowed for the length of the scan and given straight back, rather than
	// held in a static: the buffer is a quarter of a megabyte and this runs once
	// per streaming track. MP3_HEAD_BYTES covers the worst case (two hundred
	// frames at 320 kbps is 192 kB).
	unsigned char *head = malloc(MP3_HEAD_BYTES);
	if (!head) {
		fclose(f);
		return 0; // the caller falls back to the first frame's bitrate
	}

	long start = (long)dec->impl.mp3.streamStartOffset;
	size_t got = 0;
	if (fseek(f, start, SEEK_SET) == 0) {
		got = fread(head, 1, MP3_HEAD_BYTES, f);
	}
	fclose(f);

	long total_kbps = 0;
	int frames = 0;
	size_t at = 0;
	while (at + 4 <= got && frames < MP3_HEAD_FRAMES) {
		if (!drmp3_hdr_valid(head + at)) {
			at++; // out of sync: look for the next header
			continue;
		}
		int bytes = drmp3_hdr_frame_bytes(head + at, 0) + drmp3_hdr_padding(head + at);
		if (bytes <= 0) {
			break;
		}
		total_kbps += (long)drmp3_hdr_bitrate_kbps(head + at);
		frames++;
		at += (size_t)bytes;
	}

	free(head);

	if (frames == 0) {
		return 0;
	}
	return (int)(total_kbps / frames);
}

// The bitrate written in the first header. On a constant-bitrate stream -- the
// 320 kbps MP3s the services sell, and nearly everything on a card -- it is the
// bitrate, and does not change from one end of the track to the other. On a VBR
// it is not, so this returns 0: whoever shows the number falls back on the real
// average (bytes over seconds), the only honest thing to say about a VBR.
static int mp3_probe_bitrate(decoder_t *dec) {
	if (dec->impl.mp3.isVBR) {
		return 0;
	}
	return mp3_first_frame_kbps(dec);
}

// How long a still-downloading MP3 is, without reading all of it.
//
// drmp3_get_pcm_frame_count() has two routes. With a Xing/Info header the total
// is written inside and the answer is immediate and exact. Without one, dr_mp3
// reads the stream end to end, frame by frame -- and on a downloading file
// those reads wait for the network, so counting means waiting for the whole
// download, minutes on an hour-long podcast episode (feeds very often carry no
// Xing header). A stalled download makes it worse: the count gives up halfway
// and reports a duration proportional to what had arrived.
//
// So while the file is growing and the total is written nowhere, it is
// estimated: total bytes (the server's Content-Length) divided by the bitrate
// of the first header. Exact on a CBR, an approximation on a VBR without Xing.
static uint64_t mp3_growing_estimate(decoder_t *dec) {
	long done = 0;
	long total = 0;
	long bytes = 0;
	if (growfile_span(dec->growing, &done, &total) && total > 0) {
		bytes = total; // how much there will be, not how much has arrived
	} else {
		bytes = done;
	}

	// The head average, with the first frame as a safety net.
	int kbps = mp3_head_average_kbps(dec);
	if (kbps <= 0) {
		kbps = mp3_first_frame_kbps(dec);
	}
	long start = (long)dec->impl.mp3.streamStartOffset;
	if (kbps <= 0 || dec->sample_rate <= 0 || bytes <= start) {
		return 0; // unknown: better no duration than an invented one
	}

	double seconds = (double)(bytes - start) * 8.0 / ((double)kbps * 1000.0);
	return (uint64_t)(seconds * (double)dec->sample_rate);
}

static decoder_t *decoder_open_file(const char *filepath, decode_format_t format);

// A track of a CUE sheet: open the file the sheet points at, then narrow the
// decoder to that track's stretch of it.
static decoder_t *decoder_open_cue(const char *sheet_path, int track, decode_format_t format) {
	// Thirty-five kilobytes, on the playback thread's stack and below a decoder
	// open that has its own buffers: the sheet lives on the heap, and only
	// long enough to read the three things wanted from it.
	cue_sheet_t *cue = malloc(sizeof(*cue));
	if (!cue) {
		return NULL;
	}
	if (!cue_parse(sheet_path, cue) || track < 1 || track > cue->track_count) {
		free(cue);
		return NULL;
	}
	uint64_t begin_ms = cue->tracks[track - 1].begin_ms;
	uint64_t end_ms = cue->tracks[track - 1].end_ms;
	char audio_path[sizeof(cue->audio_path)];
	snprintf(audio_path, sizeof(audio_path), "%s", cue->audio_path);
	free(cue);

	decoder_t *dec = decoder_open_file(audio_path, format);
	if (!dec) {
		return NULL;
	}

	int rate = dec->sample_rate;
	if (rate <= 0) {
		decoder_close(dec);
		return NULL;
	}

	uint64_t whole = dec->total_pcm_frames;
	uint64_t begin = (uint64_t)((begin_ms * (uint64_t)rate) / 1000);
	uint64_t end = end_ms ? (uint64_t)((end_ms * (uint64_t)rate) / 1000) : whole;

	if (whole > 0 && begin >= whole) {
		decoder_close(dec); // the sheet points past the end of the file
		return NULL;
	}
	if (whole > 0 && (end == 0 || end > whole)) {
		end = whole;
	}
	if (end <= begin) {
		decoder_close(dec);
		return NULL;
	}

	dec->cue_begin = begin;
	dec->cue_frames = end - begin;
	dec->cue_read = 0;
	// The path stays the virtual one: it is what the caller asked for, and what
	// it will hand back for the next seek.
	snprintf(dec->path, sizeof(dec->path), "%s?track=%d", sheet_path, track);

	if (begin > 0) {
		decoder_seek_to_frame(dec, 0); // seeks to cue_begin, see below
	}
	return dec;
}

decoder_t *decoder_open(const char *filepath, decode_format_t format) {
	char sheet[512];
	int track = cue_split_path(filepath, sheet, sizeof(sheet));
	if (track > 0) {
		return decoder_open_cue(sheet, track, format);
	}
	return decoder_open_file(filepath, format);
}

static decoder_t *decoder_open_file(const char *filepath, decode_format_t format) {
	decoder_t *dec = calloc(1, sizeof(decoder_t));
	if (!dec)
		return NULL;
	dec->format = format;
	snprintf(dec->path, sizeof(dec->path), "%s", filepath ? filepath : "");

	switch (format) {
	case DECODE_FORMAT_MP3:
		if (growfile_is_growing(filepath)) {
			dec->growing = growfile_open(filepath);
			if (!dec->growing || !drmp3_init(&dec->impl.mp3, grow_read_mp3, grow_seek_mp3, grow_tell_mp3, NULL, dec->growing, NULL)) {
				growfile_close(dec->growing);
				free(dec);
				return NULL;
			}
		} else if (!drmp3_init_file(&dec->impl.mp3, filepath, NULL)) {
			free(dec);
			return NULL;
		}
		dec->channels = (int)dec->impl.mp3.channels;
		dec->sample_rate = (int)dec->impl.mp3.sampleRate;
		// A totalPCMFrameCount other than UINT64_MAX means one thing: there was
		// a Xing/Info header and the total is already in hand. Only when that
		// is missing and the file is still downloading does this estimate
		// instead of counting -- see mp3_growing_estimate().
		if (dec->growing && dec->impl.mp3.totalPCMFrameCount == DRMP3_UINT64_MAX) {
			dec->total_pcm_frames = mp3_growing_estimate(dec);
		} else {
			dec->total_pcm_frames = drmp3_get_pcm_frame_count(&dec->impl.mp3);
		}
		dec->bitrate_kbps = mp3_probe_bitrate(dec);
		break;

	case DECODE_FORMAT_FLAC:
		if (growfile_is_growing(filepath)) {
			dec->growing = growfile_open(filepath);
			dec->impl.flac =
				dec->growing ? drflac_open(grow_read_flac, grow_seek_flac, grow_tell_flac, dec->growing, NULL) : NULL;
		} else {
			dec->impl.flac = drflac_open_file(filepath, NULL);
		}
		if (!dec->impl.flac) {
			growfile_close(dec->growing);
			free(dec);
			return NULL;
		}
		dec->channels = dec->impl.flac->channels;
		dec->sample_rate = (int)dec->impl.flac->sampleRate;
		dec->total_pcm_frames = dec->impl.flac->totalPCMFrameCount;
		break;

	case DECODE_FORMAT_OGG_VORBIS: {
		int error = 0;
		dec->impl.vorbis = stb_vorbis_open_filename(filepath, &error, NULL);
		if (!dec->impl.vorbis) {
			free(dec);
			return NULL;
		}
		stb_vorbis_info info = stb_vorbis_get_info(dec->impl.vorbis);
		dec->channels = info.channels;
		dec->sample_rate = (int)info.sample_rate;
		dec->total_pcm_frames = stb_vorbis_stream_length_in_samples(dec->impl.vorbis);
		break;
	}

	case DECODE_FORMAT_AAC_ADTS: {
		adts_state_t *s = calloc(1, sizeof(*s));
		if (!s) {
			free(dec);
			return NULL;
		}
		dec->impl.adts = s;

		s->f = fopen(filepath, "rb");
		s->pcm = malloc(ADTS_PCM_CAPACITY * sizeof(short));
		if (!s->f || !s->pcm) {
			adts_state_free(s);
			free(dec);
			return NULL;
		}

		s->first_frame_at = adts_skip_id3(s->f);
		if (!adts_scan(s)) {
			adts_state_free(s);
			free(dec);
			return NULL; // no ADTS header: not a real .aac
		}

		s->aac = aacdec_open_adts();
		if (!s->aac) {
			adts_state_free(s);
			free(dec);
			return NULL; // no libfdk on this device
		}

		// What the header says, pending the first decoded frame: callers look
		// at duration and rate as soon as the file opens, before asking for a
		// single sample.
		dec->sample_rate = s->header_rate;
		dec->channels = s->header_channels > 0 ? s->header_channels : 2;
		dec->total_pcm_frames = (uint64_t)s->frame_total * 1024;

		if (fseek(s->f, s->first_frame_at, SEEK_SET) != 0) {
			adts_state_free(s);
			free(dec);
			return NULL;
		}

		// The nominal bitrate: the stream's bytes over its duration. ADTS
		// declares it nowhere, and this is the number a listener expects to
		// read.
		if (fseek(s->f, 0, SEEK_END) == 0) {
			long end = ftell(s->f);
			double seconds = dec->sample_rate > 0
								 ? (double)dec->total_pcm_frames / (double)dec->sample_rate
								 : 0;
			if (seconds > 0.5 && end > s->first_frame_at) {
				dec->bitrate_kbps = (int)(((double)(end - s->first_frame_at) * 8.0) / seconds / 1000.0);
			}
		}
		if (fseek(s->f, s->first_frame_at, SEEK_SET) != 0) {
			adts_state_free(s);
			free(dec);
			return NULL;
		}
		break;
	}

	case DECODE_FORMAT_SNDFILE:
		dec->impl.snd = sndfile_open(filepath);
		if (!dec->impl.snd) {
			free(dec);
			return NULL;
		}
		dec->channels = sndfile_channels(dec->impl.snd);
		dec->sample_rate = sndfile_sample_rate(dec->impl.snd);
		dec->total_pcm_frames = sndfile_total_frames(dec->impl.snd);
		break;

	case DECODE_FORMAT_DSD: {
		dec->impl.dsd = dsd_open(filepath);
		if (!dec->impl.dsd) {
			free(dec);
			return NULL;
		}
		dec->channels = dsd_channels(dec->impl.dsd);
		dec->sample_rate = dsd_output_rate(dec->impl.dsd);
		dec->total_pcm_frames = dsd_total_frames(dec->impl.dsd);
		break;
	}

	case DECODE_FORMAT_OPUS:
		dec->impl.opus = opusdec_open(filepath);
		if (!dec->impl.opus) {
			free(dec);
			return NULL;
		}
		dec->channels = opusdec_channels(dec->impl.opus);
		dec->sample_rate = opusdec_sample_rate(dec->impl.opus);
		dec->total_pcm_frames = opusdec_total_frames(dec->impl.opus);
		break;

	case DECODE_FORMAT_WAVPACK:
		dec->impl.wv = wavpackdec_open(filepath);
		if (!dec->impl.wv) {
			free(dec);
			return NULL;
		}
		dec->channels = wavpackdec_channels(dec->impl.wv);
		dec->sample_rate = wavpackdec_sample_rate(dec->impl.wv);
		dec->total_pcm_frames = wavpackdec_total_frames(dec->impl.wv);
		break;

	case DECODE_FORMAT_AAC_MP4: {
		// The container is opened before deciding which decoder to build: .m4a
		// and .m4b can hold either AAC or ALAC, and the sample entry is the
		// only place that says which.
		mp4_file_t *mp4 = mp4_open(filepath);
		if (!mp4) {
			fprintf(stderr, "decode: %s is not an MP4 with audio we know how to play\n", filepath);
			free(dec);
			return NULL;
		}

		int cfg_len = 0;
		const unsigned char *cfg = mp4_audio_config(mp4, &cfg_len);

		if (mp4_audio_codec(mp4) == MP4_CODEC_ALAC) {
			alac_state_t *s = calloc(1, sizeof(*s));
			if (!s) {
				mp4_close(mp4);
				free(dec);
				return NULL;
			}
			s->mp4 = mp4;
			dec->impl.alac = s;
			// From here on this is no longer an AAC file, and the rest of the
			// module has to know it.
			dec->format = DECODE_FORMAT_ALAC_MP4;

			s->alac = alacdec_open(cfg, cfg_len);
			if (!s->alac) {
				alac_state_free(s);
				free(dec);
				return NULL;
			}

			dec->channels = alacdec_channels(s->alac);
			dec->sample_rate = alacdec_sample_rate(s->alac);

			s->frame_total = mp4_audio_frame_count(s->mp4);
			s->pcm_capacity_frames = alacdec_frame_samples(s->alac);
			s->au_capacity = 8192;
			s->au = malloc(s->au_capacity);
			s->pcm = malloc((size_t)s->pcm_capacity_frames * (size_t)dec->channels * sizeof(int32_t));
			if (!s->au || !s->pcm) {
				alac_state_free(s);
				free(dec);
				return NULL;
			}

			// ALAC does not misreport its rate the way HE-AAC does, so the
			// duration comes from the container with nothing decoded first.
			dec->total_pcm_frames = (uint64_t)(mp4_duration_seconds(s->mp4) * (double)dec->sample_rate);
			break;
		}

		aac_state_t *s = calloc(1, sizeof(*s));
		if (!s) {
			mp4_close(mp4);
			free(dec);
			return NULL;
		}
		s->mp4 = mp4;
		dec->impl.aac = s;

		int asc_len = cfg_len;
		const unsigned char *asc = cfg;
		s->aac = aacdec_open(asc, asc_len);
		if (!s->aac) {
			fprintf(stderr, "decode: AAC not available (%s)\n", aacdec_last_error());
			aac_state_free(s);
			free(dec);
			return NULL;
		}

		s->frame_total = mp4_audio_frame_count(s->mp4);
		s->au_capacity = 8192;
		s->au = malloc(s->au_capacity);
		s->pcm = malloc(AAC_PCM_CAPACITY * sizeof(short));
		if (!s->au || !s->pcm) {
			aac_state_free(s);
			free(dec);
			return NULL;
		}

		// Decode the first frame now rather than trusting the sample
		// description: HE-AAC declares half its real output rate there, and
		// parametric stereo turns a mono core into two channels. What comes
		// back is kept, not thrown away -- it is the start of the book.
		if (!aac_fill(dec)) {
			fprintf(stderr, "decode: no decodable audio in %s\n", filepath);
			aac_state_free(s);
			free(dec);
			return NULL;
		}

		if (dec->sample_rate <= 0)
			dec->sample_rate = mp4_audio_sample_rate(s->mp4);
		if (dec->channels <= 0)
			dec->channels = mp4_audio_channels(s->mp4);

		// The length has to be counted in the decoder's own output rate, which
		// is why it is worked out from seconds instead of from the container's
		// frame count.
		dec->total_pcm_frames = (uint64_t)(mp4_duration_seconds(s->mp4) * (double)dec->sample_rate);
		dec->bitrate_kbps = mp4_audio_bitrate_kbps(s->mp4);
		break;
	}

	default:
		free(dec);
		return NULL;
	}

	if (dec->channels <= 0 || dec->sample_rate <= 0) {
		decoder_close(dec);
		return NULL;
	}

	return dec;
}

int decoder_channels(const decoder_t *dec) { return dec->channels; }

int decoder_source_bits(const decoder_t *dec) {
	// MP3 and Vorbis have no fixed source depth; 16 is what everything
	// treats them as. FLAC carries its real one.
	if (dec->format == DECODE_FORMAT_FLAC && dec->impl.flac) {
		return (int)dec->impl.flac->bitsPerSample;
	}
	// DSD goes out as 24-bit words in a 32-bit stream: DoP needs exactly 24,
	// sixteen bits of stream plus the marker byte.
	if (dec->format == DECODE_FORMAT_DSD) {
		return 24;
	}
	// A 24-bit AIFF is a hi-res track and has to reach the DAC as one.
	if (dec->format == DECODE_FORMAT_SNDFILE) {
		return sndfile_bits(dec->impl.snd);
	}
	// The same goes for the two newer lossless formats: a 24-bit ALAC or
	// WavPack is a hi-res track and has to be treated as one.
	if (dec->format == DECODE_FORMAT_ALAC_MP4) {
		return alacdec_bits(dec->impl.alac->alac);
	}
	if (dec->format == DECODE_FORMAT_WAVPACK) {
		return wavpackdec_bits(dec->impl.wv);
	}
	return 16;
}

bool decoder_is_lossy(const decoder_t *dec) {
	if (!dec) {
		return false;
	}
	switch (dec->format) {
	case DECODE_FORMAT_MP3:
	case DECODE_FORMAT_OGG_VORBIS:
	case DECODE_FORMAT_AAC_MP4:
	case DECODE_FORMAT_AAC_ADTS:
	case DECODE_FORMAT_OPUS:
		return true;
	default:
		return false;
	}
}

int decoder_bitrate_kbps(const decoder_t *dec) { return dec ? dec->bitrate_kbps : 0; }

const char *decoder_codec_name(const decoder_t *dec) {
	if (!dec) {
		return "";
	}
	switch (dec->format) {
	case DECODE_FORMAT_MP3:
		return "MP3";
	case DECODE_FORMAT_FLAC:
		return "FLAC";
	case DECODE_FORMAT_OGG_VORBIS:
		return "Vorbis";
	case DECODE_FORMAT_AAC_MP4:
	case DECODE_FORMAT_AAC_ADTS:
		return "AAC";
	case DECODE_FORMAT_ALAC_MP4:
		return "ALAC";
	case DECODE_FORMAT_OPUS:
		return "Opus";
	case DECODE_FORMAT_WAVPACK:
		return "WavPack";
	default:
		// DSD has a line of its own (DSD64/DoP), and for the libsndfile
		// formats the right name is the container's, i.e. the extension: say
		// nothing here and let the caller fall back on it.
		return "";
	}
}

bool decoder_passthrough(const decoder_t *dec) { return dec && dec->format == DECODE_FORMAT_DSD; }

int decoder_dsd_multiple(const decoder_t *dec) {
	return (dec && dec->format == DECODE_FORMAT_DSD) ? dsd_multiple(dec->impl.dsd) : 0;
}

int decoder_sample_rate(const decoder_t *dec) { return dec->sample_rate; }

uint64_t decoder_total_pcm_frames(const decoder_t *dec) {
	return dec->cue_frames ? dec->cue_frames : dec->total_pcm_frames;
}

static uint64_t decoder_read_s16_inner(decoder_t *dec, uint64_t frame_count, short *pBuffer);
static uint64_t decoder_read_s32_inner(decoder_t *dec, uint64_t frame_count, int32_t *pBuffer);

// How many frames the window still owes, and the bookkeeping after a read.
// Outside a CUE track both are transparent.
static uint64_t cue_clamp(const decoder_t *dec, uint64_t frame_count) {
	if (!dec->cue_frames) {
		return frame_count;
	}
	uint64_t left = dec->cue_read < dec->cue_frames ? dec->cue_frames - dec->cue_read : 0;
	return frame_count > left ? left : frame_count;
}

static uint64_t cue_took(decoder_t *dec, uint64_t got) {
	if (dec->cue_frames) {
		dec->cue_read += got;
	}
	return got;
}

uint64_t decoder_read_pcm_frames_s16(decoder_t *dec, uint64_t frame_count, short *pBuffer) {
	frame_count = cue_clamp(dec, frame_count);
	if (frame_count == 0) {
		return 0; // the track ends here, even though the file goes on
	}
	return cue_took(dec, decoder_read_s16_inner(dec, frame_count, pBuffer));
}

static uint64_t decoder_read_s16_inner(decoder_t *dec, uint64_t frame_count, short *pBuffer) {
	switch (dec->format) {
	case DECODE_FORMAT_MP3:
		return drmp3_read_pcm_frames_s16(&dec->impl.mp3, frame_count, pBuffer);

	case DECODE_FORMAT_FLAC:
		return drflac_read_pcm_frames_s16(dec->impl.flac, frame_count, pBuffer);

	case DECODE_FORMAT_OGG_VORBIS: {
		int frames_read = stb_vorbis_get_samples_short_interleaved(
			dec->impl.vorbis,
			dec->channels,
			pBuffer,
			(int)(frame_count * (uint64_t)dec->channels));
		return frames_read > 0 ? (uint64_t)frames_read : 0;
	}

	case DECODE_FORMAT_SNDFILE:
		return sndfile_read_s16(dec->impl.snd, frame_count, pBuffer);

	case DECODE_FORMAT_OPUS:
		return opusdec_read_s16(dec->impl.opus, frame_count, pBuffer);

	case DECODE_FORMAT_WAVPACK:
		return wavpackdec_read_s16(dec->impl.wv, frame_count, pBuffer);

	case DECODE_FORMAT_ALAC_MP4: {
		// The samples are already left-justified in 32 bits: they are narrowed
		// here, while the S32 route takes them as they are.
		alac_state_t *s = dec->impl.alac;
		uint64_t written = 0;
		while (written < frame_count) {
			if (s->pcm_read >= s->pcm_frames && !alac_fill(dec))
				break;
			uint64_t available = (uint64_t)(s->pcm_frames - s->pcm_read);
			uint64_t take = frame_count - written;
			if (take > available)
				take = available;
			const int32_t *src = s->pcm + (size_t)s->pcm_read * dec->channels;
			short *dst = pBuffer + written * (uint64_t)dec->channels;
			uint64_t samples = take * (uint64_t)dec->channels;
			for (uint64_t i = 0; i < samples; i++) {
				dst[i] = (short)(src[i] >> 16);
			}
			s->pcm_read += (int)take;
			written += take;
		}
		return written;
	}

	case DECODE_FORMAT_AAC_ADTS: {
		adts_state_t *s = dec->impl.adts;
		uint64_t written = 0;
		while (written < frame_count) {
			if (s->pcm_read >= s->pcm_frames && !adts_fill(dec))
				break;
			uint64_t available = (uint64_t)(s->pcm_frames - s->pcm_read);
			uint64_t take = frame_count - written;
			if (take > available)
				take = available;
			memcpy(pBuffer + written * (uint64_t)dec->channels, s->pcm + (size_t)s->pcm_read * dec->channels,
				   (size_t)take * dec->channels * sizeof(short));
			s->pcm_read += (int)take;
			written += take;
		}
		return written;
	}

	case DECODE_FORMAT_AAC_MP4: {
		aac_state_t *s = dec->impl.aac;
		uint64_t written = 0;
		while (written < frame_count) {
			if (s->pcm_read >= s->pcm_frames && !aac_fill(dec))
				break;
			uint64_t available = (uint64_t)(s->pcm_frames - s->pcm_read);
			uint64_t take = frame_count - written;
			if (take > available)
				take = available;
			memcpy(pBuffer + written * (uint64_t)dec->channels, s->pcm + (size_t)s->pcm_read * dec->channels,
				   (size_t)take * dec->channels * sizeof(short));
			s->pcm_read += (int)take;
			written += take;
		}
		return written;
	}

	default:
		return 0;
	}
}

uint64_t decoder_read_pcm_frames_s32(decoder_t *dec, uint64_t frame_count, int32_t *pBuffer) {
	frame_count = cue_clamp(dec, frame_count);
	if (frame_count == 0) {
		return 0;
	}
	return cue_took(dec, decoder_read_s32_inner(dec, frame_count, pBuffer));
}

static uint64_t decoder_read_s32_inner(decoder_t *dec, uint64_t frame_count, int32_t *pBuffer) {
	switch (dec->format) {
	case DECODE_FORMAT_DSD:
		return dsd_read(dec->impl.dsd, frame_count, pBuffer);

	case DECODE_FORMAT_SNDFILE:
		// libsndfile left-justifies into the int, which is the same contract
		// dr_flac keeps and the same one the S32 output path expects.
		return sndfile_read_s32(dec->impl.snd, frame_count, pBuffer);

	case DECODE_FORMAT_FLAC:
		// dr_flac hands the source samples left-justified in 32 bits: a
		// 24-bit stream keeps all 24 bits, exactly what S32 playback wants.
		return drflac_read_pcm_frames_s32(dec->impl.flac, frame_count, pBuffer);

	case DECODE_FORMAT_WAVPACK:
		// Same contract: left-justified by the wrapper.
		return wavpackdec_read_s32(dec->impl.wv, frame_count, pBuffer);

	case DECODE_FORMAT_ALAC_MP4: {
		// Here the decoder already produces what is wanted, so this is a plain
		// copy -- no widening and no loss of the low bits.
		alac_state_t *s = dec->impl.alac;
		uint64_t written = 0;
		while (written < frame_count) {
			if (s->pcm_read >= s->pcm_frames && !alac_fill(dec))
				break;
			uint64_t available = (uint64_t)(s->pcm_frames - s->pcm_read);
			uint64_t take = frame_count - written;
			if (take > available)
				take = available;
			memcpy(pBuffer + written * (uint64_t)dec->channels, s->pcm + (size_t)s->pcm_read * dec->channels,
				   (size_t)take * dec->channels * sizeof(int32_t));
			s->pcm_read += (int)take;
			written += take;
		}
		return written;
	}

	default: {
		// 16-bit sources: decode into the front of the same buffer, then widen
		// in place from the end backwards -- position i's write only ever lands
		// on narrow slots 2i/2i+1, both at or past everything still unread.
		short *narrow = (short *)pBuffer;
		uint64_t frames_read = decoder_read_pcm_frames_s16(dec, frame_count, narrow);
		uint64_t got = frames_read * (uint64_t)dec->channels;
		for (uint64_t i = got; i-- > 0;) {
			int32_t wide = (int32_t)narrow[i] << 16;
			pBuffer[i] = wide;
		}
		return frames_read;
	}
	}
}

uint64_t decoder_seekable_limit(decoder_t *dec) {
	if (!dec || dec->total_pcm_frames == 0) {
		return 0;
	}
	if (!dec->growing) {
		return dec->total_pcm_frames;
	}

	long done = 0, total = 0;
	if (!growfile_span(dec->growing, &done, &total) || total <= 0) {
		return dec->total_pcm_frames; // no longer growing: it is all there
	}

	// The ratio of bytes to time on a FLAC is not exact -- blocks compress
	// differently -- so a margin is kept: better to stop a little short and
	// succeed than to aim at the limit and fail.
	double share = (double)done / (double)total;
	return (uint64_t)((double)dec->total_pcm_frames * share * 0.90);
}

bool decoder_is_growing(const decoder_t *dec) {
	if (!dec || !dec->growing) {
		return false;
	}
	// `growing` stays attached even once the download has finished: what
	// matters is whether it is still growing now.
	long done = 0, total = 0;
	return growfile_span((growfile_t *)dec->growing, &done, &total) && total > 0;
}

bool decoder_grow_span(decoder_t *dec, long *done, long *total) {
	if (!dec || !dec->growing) {
		return false;
	}
	return growfile_span(dec->growing, done, total);
}

bool decoder_take_starved(decoder_t *dec) {
	if (!dec || !dec->growing) {
		return false;
	}
	return growfile_take_starved(dec->growing);
}

int decoder_seek_to_frame(decoder_t *dec, uint64_t frame_index) {
	if (!dec)
		return 0;

	// Inside a CUE track the caller counts from the start of the TRACK; the
	// decoder counts from the start of the FILE.
	if (dec->cue_frames) {
		if (frame_index > dec->cue_frames) {
			frame_index = dec->cue_frames;
		}
		dec->cue_read = frame_index;
		frame_index += dec->cue_begin;
	}

	// A still-downloading file cannot be seeked past where it has got to, so
	// the request is clamped to the furthest point that can really be served.
	//
	// Needed because the decoders' binary search asks for positions scattered
	// across the whole file, and an unclamped one parks the audio thread waiting
	// for bytes minutes away.
	//
	// Callers should have clamped already (decoder_seekable_limit); this is the
	// second turn of the key, for the ones that forget.
	uint64_t limit = decoder_seekable_limit(dec);
	if (limit > 0 && frame_index > limit) {
		frame_index = limit;
	}

	switch (dec->format) {
	case DECODE_FORMAT_MP3: {
		// The index, built on the first seek. Without it dr_mp3 decodes from
		// where it is to where it was asked, stalling the audio thread for
		// seconds; with it, at most MP3_INDEX_SECONDS are left to decode.
		mp3_index_t *ix = &dec->mp3_index;
		bool stale = ix->count > 0 && dec->growing && frame_index > ix->points[ix->count - 1].pcmFrameIndex &&
					 mp3_available_bytes(dec) > ix->scanned_bytes;
		if (ix->count == 0 || stale) {
			mp3_index_build(dec);
		}
		return (int)drmp3_seek_to_pcm_frame(&dec->impl.mp3, frame_index);
	}

	case DECODE_FORMAT_FLAC:
		return (int)drflac_seek_to_pcm_frame(dec->impl.flac, frame_index);

	case DECODE_FORMAT_OGG_VORBIS:
		return stb_vorbis_seek(dec->impl.vorbis, (unsigned int)frame_index);

	case DECODE_FORMAT_DSD:
		return dsd_seek(dec->impl.dsd, frame_index) ? 1 : 0;

	case DECODE_FORMAT_SNDFILE:
		return sndfile_seek(dec->impl.snd, frame_index) ? 1 : 0;

	case DECODE_FORMAT_OPUS:
		return opusdec_seek(dec->impl.opus, frame_index) ? 1 : 0;

	case DECODE_FORMAT_WAVPACK:
		return wavpackdec_seek(dec->impl.wv, frame_index) ? 1 : 0;

	case DECODE_FORMAT_ALAC_MP4: {
		alac_state_t *s = dec->impl.alac;
		if (dec->sample_rate <= 0)
			return 0;

		// No run-up, unlike AAC: every ALAC access unit decodes on its own, so
		// this jumps straight to the right one and drops the samples that sit
		// before the requested point.
		double target = (double)frame_index / (double)dec->sample_rate;
		uint32_t at = mp4_frame_at_time(s->mp4, target);

		s->frame_index = at;
		s->pcm_frames = 0;
		s->pcm_read = 0;

		double from_time = mp4_frame_time(s->mp4, at);
		double ahead = target - from_time;
		s->skip_frames = ahead > 0 ? (uint64_t)(ahead * (double)dec->sample_rate) : 0;
		return 1;
	}

	case DECODE_FORMAT_AAC_ADTS: {
		adts_state_t *s = dec->impl.adts;
		if (dec->sample_rate <= 0 || s->mark_count == 0) {
			return 0;
		}

		// Samples per frame: the real figure once the decoder has spoken,
		// otherwise the textbook one.
		int per_frame = s->sized ? aacdec_frame_size(s->aac) : 1024;
		if (per_frame <= 0) {
			per_frame = 1024;
		}

		// The frame covering the requested point, and the bookmark just before
		// it: the restart happens there, with a few frames of run-up because a
		// freshly flushed decoder takes a moment to produce correct output.
		uint32_t want = (uint32_t)(frame_index / (uint64_t)per_frame);
		uint32_t run_up = want > ADTS_SEEK_RUNUP ? ADTS_SEEK_RUNUP : want;
		uint32_t from = want - run_up;
		int mark = (int)(from / (uint32_t)s->mark_stride);
		if (mark >= s->mark_count) {
			mark = s->mark_count - 1;
		}
		uint32_t at_frame = (uint32_t)mark * (uint32_t)s->mark_stride;

		if (fseek(s->f, s->marks[mark], SEEK_SET) != 0) {
			return 0;
		}

		aacdec_flush(s->aac);
		s->in_have = 0;
		s->in_used = 0;
		s->pcm_frames = 0;
		s->pcm_read = 0;

		// Samples remain between the bookmark and the requested point: they are
		// decoded and thrown away, as on the MP4 route.
		uint64_t from_sample = (uint64_t)at_frame * (uint64_t)per_frame;
		s->skip_frames = frame_index > from_sample ? frame_index - from_sample : 0;
		return 1;
	}

	case DECODE_FORMAT_AAC_MP4: {
		aac_state_t *s = dec->impl.aac;
		if (dec->sample_rate <= 0)
			return 0;

		double target = (double)frame_index / (double)dec->sample_rate;
		uint32_t at = mp4_frame_at_time(s->mp4, target);
		// Two access units of run-up: a decoder that has just been cleared
		// needs a frame or two before its output is right, and the overlap
		// window of the one before matters as well.
		uint32_t from = at > 2 ? at - 2 : 0;

		aacdec_flush(s->aac);
		s->frame_index = from;
		s->pcm_frames = 0;
		s->pcm_read = 0;

		double from_time = mp4_frame_time(s->mp4, from);
		double ahead = target - from_time;
		s->skip_frames = ahead > 0 ? (uint64_t)(ahead * (double)dec->sample_rate) : 0;
		return 1;
	}

	default:
		return 0;
	}
}

void decoder_close(decoder_t *dec) {
	if (!dec)
		return;

	// The decoder first, the file handle after: while the decoder is alive it
	// keeps reading through it.
	growfile_t *growing = dec->growing;

	switch (dec->format) {
	case DECODE_FORMAT_MP3:
		// The seek table belongs to this module: dr_mp3 holds the pointer and
		// never frees it. Unbind before closing, then free.
		drmp3_bind_seek_table(&dec->impl.mp3, 0, NULL);
		drmp3_uninit(&dec->impl.mp3);
		mp3_index_free(&dec->mp3_index);
		break;
	case DECODE_FORMAT_FLAC:
		drflac_close(dec->impl.flac);
		break;
	case DECODE_FORMAT_OGG_VORBIS:
		stb_vorbis_close(dec->impl.vorbis);
		break;
	case DECODE_FORMAT_AAC_MP4:
		aac_state_free(dec->impl.aac);
		break;
	case DECODE_FORMAT_AAC_ADTS:
		adts_state_free(dec->impl.adts);
		break;
	case DECODE_FORMAT_ALAC_MP4:
		alac_state_free(dec->impl.alac);
		break;
	case DECODE_FORMAT_OPUS:
		opusdec_close(dec->impl.opus);
		break;
	case DECODE_FORMAT_WAVPACK:
		wavpackdec_close(dec->impl.wv);
		break;
	case DECODE_FORMAT_DSD:
		dsd_close(dec->impl.dsd);
		break;
	case DECODE_FORMAT_SNDFILE:
		sndfile_close(dec->impl.snd);
		break;
	default:
		break;
	}

	growfile_close(growing);
	free(dec);
}
