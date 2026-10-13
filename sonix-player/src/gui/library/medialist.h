#ifndef MEDIALIST_H
#define MEDIALIST_H

#include "src/gui/shell/gui.h"
#include "src/system/library/library.h"

#include "lvgl/lvgl.h"

// The library index pages: one screen for name lists (albums, artists, album
// artists, genres), one for one artist's or one genre's records, and one for
// track lists (all tracks, and the tracks of whatever was tapped). A screen each
// rather than one reused, so the back chevron walks artists -> that artist's
// albums -> the album's tracks and back out again the way it should.
//
// Rows are a fixed pool windowed over the scroll position, the same technique
// as the file browser, and the data is windowed with them: the page holds the
// row ids the list is made of (four bytes each) and reads a bandful of actual
// rows back as the viewport moves, so the library has no size ceiling.
extern lv_obj_t *medialist_screen;
extern lv_obj_t *medialist_tracks_screen;
// The middle level: one artist's or one genre's records. Its own screen and
// not the first one reused, because the way back from an album's tracks is
// those albums and the way back from those is the artist or genre list -- three
// lists, and a screen cannot be two of them at once.
extern lv_obj_t *medialist_albums_screen;

void medialist_init(gui_config_t *cfg);

// Whether tapping an artist or a genre opens its records or its tracks. On by
// default; Music > Library turns it off, and an artist or genre page is
// then a flat list of tracks (an artist's with the album grouping on the corner
// button).
bool medialist_album_view(void);
void medialist_set_album_view(bool on);

// "Alternative album view": an album's tracks open under a head with its
// cover, title, artist, genre and year and three buttons -- shuffle, play and a
// track into the queue, circle-play's choices -- which scrolls away with the
// rows. The page takes the sleeve's colour, and the rows carry track numbers
// instead of covers. Off by default; Music > Library. On a record of more
// than one disc there is a "Disc N" heading above the first track of each disc
// either way.
bool medialist_album_head(void);
void medialist_set_album_head(bool on);

// "Go to the current track": All tracks, Albums, Artists, Album artists and
// Genres open scrolled to the row of what is playing -- the track itself in
// All tracks, its album, artist, album artist or genre in the others. Off by
// default; Music > Library.
bool medialist_go_to_current(void);
void medialist_set_go_to_current(bool on);

// "Allow deleting tracks": selection mode on a list of tracks gets a bin, which
// deletes the chosen tracks from the card and from the library after asking.
// Off by default; Music > Library.
bool medialist_allow_delete(void);
void medialist_set_allow_delete(bool on);

// Whether a track row wears a small badge saying what it is -- lossy, CD,
// hi-res or DSD -- under its title. Off by default.
bool medialist_quality_badges(void);
void medialist_set_quality_badges(bool on);

// "Show artist": the artist under the title of each row, on the lists `lists`
// picks out -- all the tracks, the albums, a genre's albums and tracks, the
// favourites, the playlists. The tracks show their own artist, the albums
// their album artist.
#define MEDIALIST_ARTIST_TRACKS 1
#define MEDIALIST_ARTIST_ALBUMS 2
#define MEDIALIST_ARTIST_GENRES 4
#define MEDIALIST_ARTIST_FAVOURITES 8
#define MEDIALIST_ARTIST_PLAYLISTS 16
#define MEDIALIST_ARTIST_ALL                                                                                       \
	(MEDIALIST_ARTIST_TRACKS | MEDIALIST_ARTIST_ALBUMS | MEDIALIST_ARTIST_GENRES | MEDIALIST_ARTIST_FAVOURITES |   \
	 MEDIALIST_ARTIST_PLAYLISTS)
bool medialist_show_artist(void);
int medialist_artist_lists(void);
void medialist_set_show_artist(bool on, int lists);

// How the player's own list of `kind` narrowed by `filter` is ordered right now:
// by name or by date, which way round, and an artist's tracks grouped by
// record or not. What SonixLink follows, so a list on the phone reads in the
// same order and a track tapped there gets the same queue as here.
void medialist_list_order(library_list_t kind, library_filter_t filter, library_order_t *order, bool *desc);

// The same choices as bits, for the phone to order its own lists by: bit
// `kind` of `desc_mask` set is that list reversed, of `added_mask` that list by
// date added.
void medialist_sort_prefs(unsigned *desc_mask, unsigned *added_mask, bool *artist_by_album, bool *favourites_reversed);

// Loads the list and switches to the right screen. For LIBRARY_LIST_TRACKS
// the filter narrows to one album/artist/genre (LIBRARY_FILTER_NONE = all);
// for the name kinds both filter arguments are ignored.
//
// A different list opened on the screen already showing -- "Show album" from
// Favourites, say -- takes the place of the one there, and the chevron brings
// that one back, where it was scrolled to, before it leaves the page.
void medialist_open(const char *title, library_list_t kind, library_filter_t filter, const char *filter_value);

// For the player's menu, right after its medialist_open(): true when the list
// opened on the page already under the player -- in place of another list, or
// over the same one. The step back then reopens what was there with the player
// over it, so the caller must not also ask the switcher to return to the
// player.
bool medialist_step_back_to_player(void);

// Opens one artist the way their disc button was last left: the records, or
// the tracks strung out. What the search opens an artist with.
void medialist_open_artist(const char *name);

// Re-reads what is playing and moves the accent mark to whichever rows now
// carry it. Called from the player whenever the track changes; cheap enough
// to call on a track that has not (it walks two dozen pool rows).
void medialist_notify_now_playing(void);

// The list on screen brought up to date with the index, for a page that comes
// back into view without being loaded again: the player sheet sliding off it.
// Nothing happens when the list has not changed or no list is on screen.
void medialist_refresh_visible(void);

// The same track list, filled from an explicit set of paths rather than from
// the index: what a playlist's contents are. There is no query behind it, so
// this is the one list still held whole in RAM -- a playlist file is a few
// hundred lines, not a library.
//
// `names` may be NULL, or hold NULLs, in which case the file name is used;
// `paths` and `names` are copied. `playlist` is the name of the playlist the
// list came from, or NULL when it came from somewhere else -- it is what makes
// the ellipsis offer to take a track out of the list rather than add it to
// another one.
void medialist_open_paths(const char *title, const char *const *paths, const char *const *names,
						  const char *const *artists, int count, const char *playlist);

// A playlist was renamed while its track list was still open behind the
// playlists page. The list keeps the name it was opened with, and would go on
// writing to a file that no longer exists, so it is told the new one.
void medialist_playlist_renamed(const char *old_name, const char *new_name);

#endif /* MEDIALIST_H */
