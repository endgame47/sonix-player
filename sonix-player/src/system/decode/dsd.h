#ifndef DSD_H
#define DSD_H

#include <stdbool.h>
#include <stdint.h>

// DSD: .dsf and .dff, up to DSD256.
//
// DSD is not samples, it is a one-bit stream at a few megahertz -- 2.8224 MHz
// for DSD64, twice that for DSD128, four times for DSD256. It reaches the ears
// one way here: DoP.
//
// DoP (DSD over PCM) packs the bits sixteen at a time into ordinary 24-bit PCM
// frames with a marker byte on top, sent at a sixteenth of the DSD rate, and
// the DAC recognises the marker and switches itself into DSD. It is what this
// hardware is built for: the sound card driver
// (x1600_hiby_r3proii_sound_card.ko) carries a mixer control called DOP_EN
// whose handler calls cs43198_set_dsd_en() straight into the codec, and the
// stock player's ot_devices.json offers DSD by that route and no other.
// Nothing may touch the samples on the way -- a volume change or an EQ band
// would destroy the markers and the DAC would fall back to hearing white
// noise.
//
// Filtering the stream down to PCM here is the other thing a player can do
// with DSD, and this one does not: the filter that does it honestly costs more
// than the whole core at DSD256, which is a stuttering track and an interface
// that will not answer. A device that cannot take 705.6 kHz cannot play a
// DSD256 file, and says so.

typedef struct dsd_file dsd_file_t;

// Opens a .dsf or .dff. NULL when it is neither, when the rate is not one of
// the three, or when the file has more channels than can be played.
dsd_file_t *dsd_open(const char *path);
void dsd_close(dsd_file_t *d);

int dsd_channels(const dsd_file_t *d);
uint32_t dsd_rate(const dsd_file_t *d);	   // the DSD rate itself (2822400, ...)
int dsd_multiple(const dsd_file_t *d);	   // 64, 128 or 256
int dsd_output_rate(const dsd_file_t *d);  // what comes out of dsd_read(): the DSD rate over 16
uint64_t dsd_total_frames(const dsd_file_t *d); // in output frames

// Reads interleaved 32-bit frames: DoP words left-justified in 32 bits.
// Returns frames read, 0 at the end.
uint64_t dsd_read(dsd_file_t *d, uint64_t frames, int32_t *out);

// Seeks to an output frame, landing on an even byte so the halves of every
// word from there on stay the right way round.
bool dsd_seek(dsd_file_t *d, uint64_t frame);

#endif /* DSD_H */
