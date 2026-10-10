#ifndef ALBUMART_H
#define ALBUMART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A cover image exactly as it is stored on disk / inside the tag: still
// compressed (JPEG or PNG). Decoding it into pixels is the GUI's job (see
// src/gui/cover.h), this module only finds the bytes.
typedef struct {
	uint8_t *data; // owned by the caller, read-only -- free with albumart_free()
	size_t size;

	// Set when `data` points into a mapping of the file rather than at a copy.
	// Zero, as from memset, means a copy.
	void *map;
	size_t map_len;
	long long map_at;
	int map_fd;
	int map_slot;
} albumart_t;

// A picture stored as it is in the file -- a FLAC PICTURE block, an ID3 APIC
// frame, an MP4 `covr` atom, an APEv2 binary item, a cover file -- is mapped
// from the file rather than copied into memory, up to this size.
#define ALBUMART_MAP_MAX_BYTES (32 * 1024 * 1024)

// The largest picture copied into memory: one that has to be decoded on the
// way (base64 in Ogg comments, an unsynchronised ID3 frame) or that cannot be
// mapped.
#define ALBUMART_MAX_BYTES (8 * 1024 * 1024)

// How many places a cover can come from, for the candidate loaders below.
#define ALBUMART_CANDIDATES 2

// Finds the cover art for an audio file. Tries the picture embedded in the
// file's tags first (ID3v2 APIC for MP3, .dsf, AIFF and WAV, PICTURE block for FLAC,
// METADATA_BLOCK_PICTURE / COVERART for OGG and Opus, `covr` for MP4/ALAC, the
// APEv2 cover item for WavPack and APE),
// then falls back to a cover image file sitting in the same folder (cover.jpg,
// folder.jpg, front.png, ...). Returns true and fills `out` on success; `out`
// is left zeroed otherwise.
bool albumart_load_for_file(const char *filepath, albumart_t *out);

// Same, but only looks for a cover image file inside `dirpath`. Used for
// folder rows in the file browser, where there is no single track to read.
bool albumart_load_for_dir(const char *dirpath, albumart_t *out);

// The same two sources, one at a time and best first (0 = the embedded
// picture, 1 = a cover file in the folder; for a directory, 0 = the cover file,
// 1 = the first track's embedded picture). The loaders above simply walk them
// in order and stop at the first that yields bytes.
//
// A caller that decodes what it gets wants the loop instead: a picture whose
// bytes exist but cannot be decoded -- a progressive JPEG larger than the
// device's decode budget, a truncated APIC -- must not hide the perfectly good
// cover.jpg lying next to the track. Returns false when there is nothing at
// that index.
bool albumart_load_candidate(const char *filepath, int index, albumart_t *out);
bool albumart_load_dir_candidate(const char *dirpath, int index, albumart_t *out);

// True when the card went away while a mapped picture was being read: the
// bytes read since are zeros, and anything made from them is to be thrown
// away. Asked after the picture has been used, before albumart_free().
bool albumart_faulted(const albumart_t *art);

// Frees the image buffer or the mapping and zeroes the struct. Safe to call on
// a zeroed one.
void albumart_free(albumart_t *art);

#endif // ALBUMART_H
