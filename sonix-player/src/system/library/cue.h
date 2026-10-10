#ifndef CUE_H
#define CUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A CD ripped as one long file plus a .cue sheet: the sheet says where each
// track starts inside that file. Without this the library shows one hour-long
// entry per album, so the scanner reads the sheet and the decoder seeks to the
// offsets it reports.
//
//   REM GENRE Rock
//   REM DATE 1973
//   PERFORMER "Some Band"
//   TITLE "Some Album"
//   FILE "album.flac" WAVE
//     TRACK 01 AUDIO
//       TITLE "First Song"
//       INDEX 01 00:00:00
//     TRACK 02 AUDIO
//       TITLE "Second Song"
//       INDEX 01 05:23:40
//
// The parser never allocates: everything lands in the cue_sheet_t the caller
// owns. Sheets come off a card anyone can write to, so nothing in here trusts
// the file -- oversized fields truncate, absurd counts and timestamps are
// dropped, and an unreadable sheet is simply "not a sheet".

#define CUE_MAX_TRACKS 99

typedef struct {
	int number;			// the TRACK number as written
	char title[160];
	char performer[160];
	uint64_t begin_ms;	// from INDEX 01 (INDEX 00 is the pre-gap: ignore it)
	uint64_t end_ms;	// where the next track starts; 0 on the last track
} cue_track_t;

typedef struct {
	char audio_path[512];	// absolute path of the file FILE points at
	char album[160];		// the sheet's TITLE
	char performer[160];	// the sheet's PERFORMER
	char genre[96];			// from REM GENRE, empty if absent
	int year;				// from REM DATE, 0 if absent
	int disc;				// from REM DISCNUMBER, 0 if absent
	int track_count;
	// A sheet over a disc already split one file per track: only the first
	// file's track is here. The library and the browser take the files as
	// they are and leave such a sheet alone; a track path that names it still
	// plays, as it did when the library indexed it.
	bool split;
	cue_track_t tracks[CUE_MAX_TRACKS];
} cue_sheet_t;

// Parses the sheet at `cue_path`. False when it is not a usable sheet: no
// FILE, no track with an INDEX, or the audio file it names is not there.
bool cue_parse(const char *cue_path, cue_sheet_t *out);

// True when the file name ends in ".cue" (case-insensitive).
bool cue_is_sheet(const char *path);

// True when the name is a WAV, which may carry its own track list in its RIFF
// markers ("cue " points with "adtl" labels) and can then be handed to
// cue_parse() exactly like a sheet -- the file standing in for the sheet. Only
// the name is looked at; whether there really are markers is cue_parse's
// answer, and it is false for a WAV that is one plain track.
bool cue_wav_has_markers(const char *path);

// The virtual path this player uses for one track of a sheet, 1-based:
//
//     /mnt/sd/Music/Album/album.cue?track=3
//
// Everything downstream carries paths, so a track of a sheet has to be
// expressible as one.
void cue_virtual_path(const char *cue_path, int track_number, char *out, size_t size);

// Splits a virtual path. Returns the 1-based track number and copies the sheet
// path into `cue_out`; returns 0 for an ordinary path (still copied).
int cue_split_path(const char *path, char *cue_out, size_t size);

// Orders two paths that are both tracks of the same sheet, by track number.
//
// It exists because the alternative is alphabetical, and alphabetically track
// ten comes before track two. Returns false when the two are not tracks of one
// sheet, which leaves the caller to order them however it orders everything
// else; `out` is only written when it returns true.
bool cue_order_tracks(const char *a, const char *b, int *out);

#endif // CUE_H
