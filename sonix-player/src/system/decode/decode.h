#ifndef DECODE_H
#define DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Compressed formats supported through the unified decoder below. WAV is
// uncompressed PCM and is handled separately in audio.c.
typedef enum {
	DECODE_FORMAT_UNKNOWN,
	DECODE_FORMAT_MP3,
	DECODE_FORMAT_FLAC,
	DECODE_FORMAT_OGG_VORBIS,
	// AAC inside an MP4 container: .m4a, .m4b, .mp4. Audiobooks on this device
	// are .m4b, and this is the only way one can be played at all. The decoder
	// itself is the firmware's own libfdk-aac, opened at run time -- see
	// aacdec.h -- so this format is unavailable on a device without it.
	DECODE_FORMAT_AAC_MP4,
	// DSD: .dsf and .dff, up to DSD256. Either handed to the DAC untouched as
	// DoP or filtered down to 176.4 kHz PCM here -- see dsd.h, and
	// decoder_passthrough() below for what the difference costs the caller.
	DECODE_FORMAT_DSD,
	// What the firmware's libsndfile provides: AIFF/AIFF-C and Core Audio (ALAC
	// included). See sndfile.h -- it is borrowed at run time, so these formats
	// are unavailable on a device without that library.
	DECODE_FORMAT_SNDFILE,
	// ALAC inside an MP4: the same .m4a files as AAC. The file name does not say
	// so, the sample entry does, so decode_detect_format() never returns this
	// value -- decoder_open() sets it when it opens the container and finds out
	// what is inside. See alacdec.h.
	DECODE_FORMAT_ALAC_MP4,
	// Opus inside Ogg: .opus files. libogg, libopus and opusfile are compiled
	// statically into the binary rather than borrowed from the firmware; see
	// opusdec.h for why.
	DECODE_FORMAT_OPUS,
	// WavPack: .wv files, including hybrid mode with the .wvc correction file
	// alongside, and DSD inside WavPack. See wavpackdec.h.
	DECODE_FORMAT_WAVPACK,
	// Raw AAC: .aac files, where frames follow one another behind their ADTS
	// header with no container around them. Same decoder as .m4a -- the
	// firmware's libfdk-aac -- but opened in ADTS mode, where it finds the frame
	// boundaries itself. Duration and seeking have to be built here by walking
	// the headers, because a raw stream carries no index.
	DECODE_FORMAT_AAC_ADTS,
} decode_format_t;

typedef struct decoder decoder_t;

// Determine which decoder (if any) handles this file, based on its extension.
decode_format_t decode_detect_format(const char *filepath);

// Opens a decoder for the given file/format. Returns NULL on failure.
decoder_t *decoder_open(const char *filepath, decode_format_t format);

int decoder_channels(const decoder_t *dec);
int decoder_source_bits(const decoder_t *dec); // 16 for mp3/ogg, the real depth for flac
int decoder_sample_rate(const decoder_t *dec);

// True for lossy formats (MP3, AAC, Vorbis, Opus).
//
// For whoever writes the format line. A lossy format has no bit depth of its
// own: "16/44.1 MP3" describes the PCM coming out of the decoder, not the file.
// The figure that characterises a lossy file is its bitrate.
bool decoder_is_lossy(const decoder_t *dec);

// The nominal bitrate in kbps: the one written in the file (the first frame's
// header for an MP3, the esds for an AAC), not the average from bytes divided
// by seconds.
//
// 0 when unknown: a lossless format, a variable-bitrate MP3 (where the first
// header's number would be a lie), an AAC that declares nothing. Whoever
// displays the number needs an answer for zero too.
//
// On Qobuz and Tidal tracks it is the only usable number: the cache file is
// still growing while it plays, and bytes-over-seconds would give a bitrate
// that visibly climbs.
int decoder_bitrate_kbps(const decoder_t *dec);

// "MP3", "AAC", "FLAC", "ALAC", "Opus", "Vorbis", "WavPack". Empty string when
// the right name is the container's (the libsndfile formats) or when someone
// else writes the line (DSD).
//
// Not the extension: an .m4a can be AAC or ALAC, and those read as two
// different things under a cover.
const char *decoder_codec_name(const decoder_t *dec);

// Total PCM frames in the stream, or 0 if unknown.
uint64_t decoder_total_pcm_frames(const decoder_t *dec);

// Reads up to frame_count PCM frames, interleaved signed 16-bit, into pBuffer.
// Returns the number of frames actually read; 0 means end of stream.
// Buffer type is `short` (not int16_t) to exactly match the underlying
// decoder libraries' signatures across translation units.
uint64_t decoder_read_pcm_frames_s16(decoder_t *dec, uint64_t frame_count, short *pBuffer);

// Reads up to frame_count PCM frames, interleaved signed 32-bit (the source
// samples left-justified, so a 24-bit FLAC arrives bit-perfect in the top 24
// bits). This feeds the DAC's S32 mode for hi-res tracks, the same way the
// stock player switches its tinyalsa stream to "PCM_FORMAT_PCM 32bit".
// MP3/Vorbis fall back to their 16-bit decode widened in place.
uint64_t decoder_read_pcm_frames_s32(decoder_t *dec, uint64_t frame_count, int32_t *pBuffer);

// True when what comes out of decoder_read_pcm_frames_s32() is not audio the
// caller may touch. Only DoP does this: its frames are DSD bits with a marker
// byte the DAC looks for, and a gain change or an EQ band would scramble the
// marker and leave the DAC decoding noise. Everything in the chain --
// ReplayGain, EQ, soundfield, balance, the fade at the end of a track -- has
// to be skipped for a stream that says yes.
bool decoder_passthrough(const decoder_t *dec);

// 64, 128 or 256 for a DSD file, 0 for anything else.
int decoder_dsd_multiple(const decoder_t *dec);

// Seek decoder to the specified frame index. Returns nonzero on success, 0 on failure.
int decoder_seek_to_frame(decoder_t *dec, uint64_t frame_index);

// The furthest frame that can be seeked to right now. For a normal file that is
// the end; for one still downloading it is how far it has got, minus a margin
// (the byte-to-time ratio of a FLAC is not exact).
//
// Callers must consult this first and clamp their request, so the position they
// show and the position the music resumes at are the same.
uint64_t decoder_seekable_limit(decoder_t *dec);

// True if this file is still growing while being read.
//
// Playback needs to know. Ahead of the read head sits a reserve of bytes
// downloaded but not yet played, and it can run out when the network slows
// down. Playback must then stop and let the reserve refill rather than let the
// sound card run dry.
bool decoder_is_growing(const decoder_t *dec);

// How many bytes have been downloaded and how many there will be in total, for
// a growing file. False for a normal file or once the download has finished.
//
// Bytes, not frames: the frame estimate (decoder_seekable_limit) is good enough
// for the progress bar but it is a proportion, and a proportion can claim there
// is margin while the decoder is already at the edge. To know how much is
// really left, look at the bytes.
bool decoder_grow_span(decoder_t *dec, long *done, long *total);

// True (and cleared by reading) if since the last call a read had to wait for
// bytes not yet downloaded: the decoder reached the edge of the download. Reads
// still complete after waiting, so it is invisible from outside; this is the
// only way to know, and it is the signal on which playback pauses to rebuild
// its reserve.
bool decoder_take_starved(decoder_t *dec);

void decoder_close(decoder_t *dec);

#endif // DECODE_H
