#ifndef LRCLIB_H
#define LRCLIB_H

#include <stdbool.h>

// Lyrics from LRCLIB (lrclib.net), the free database of timed lyrics: a search
// by the tags of a track, and the one picked saved as an .lrc file beside it,
// which is where lyrics_load() looks first.
//
// Both block on the network or the card: call them on a thread of their own,
// never on the interface thread.

typedef enum {
	LRCLIB_OK,			 // at least one record with words
	LRCLIB_NOT_FOUND,	 // nothing for these tags
	LRCLIB_INSTRUMENTAL, // only records that say the track has no words
	LRCLIB_NO_TAGS,		 // no title or no artist to search with
	LRCLIB_NETWORK,		 // no answer, or not one that could be read
} lrclib_result_t;

typedef struct {
	char track[160];
	char artist[160];
	char album[160];
	int duration; // seconds, 0 when the record does not say
	bool synced;  // `text` is timed, LRC lines
	char *text;	  // the timed lyrics when there are some, the plain ones otherwise
} lrclib_record_t;

// Searches by title and artist, then by the two as free text when that finds
// nothing. On LRCLIB_OK *records holds *count records, the likeliest first:
// within two seconds of `duration_secs` (0 when unknown), timed, from `album`.
// Free them with lrclib_free().
lrclib_result_t lrclib_search(const char *title, const char *artist, const char *album, int duration_secs, lrclib_record_t **records, int *count);

void lrclib_free(lrclib_record_t *records, int count);

// Writes `text` as "<the track's file name>.lrc" beside `path`. False when the
// card would not take it.
bool lrclib_save(const char *path, const char *text);

#endif /* LRCLIB_H */
