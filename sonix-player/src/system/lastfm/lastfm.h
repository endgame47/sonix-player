#ifndef LASTFM_H
#define LASTFM_H

#include <stdbool.h>

// Scrobbling to Last.fm.
//
// The GUI thread reports what is playing once a second with
// lastfm_note_playback(), and this module decides what counts as a play and
// when it has been listened to long enough to be a scrobble: half its length
// or four minutes, whichever comes first, for a track of at least 30 seconds.
//
// A worker thread does the network and the files. "Now playing" goes out when
// there is Wi-Fi at the start of a track. Scrobbles go into a queue in .local
// on the card and are sent in batches whenever the Wi-Fi is up. Nothing
// here turns the Wi-Fi on, and the radio is held up only while a request is on
// the wire.
//
// The API account comes from the streaming keys ([lastfm]); without it the
// service is unavailable. Signing in trades the username and password for a
// session key (auth.getMobileSession), which is what is kept, beside
// device_config.ini; the password is not.

typedef enum {
	LASTFM_STATE_UNAVAILABLE, // no API keys in the streaming keys
	LASTFM_STATE_OFF,
	LASTFM_STATE_SIGNED_OUT,
	LASTFM_STATE_SIGNING_IN,
	LASTFM_STATE_CONNECTED,
	LASTFM_STATE_SESSION_EXPIRED, // Last.fm refused the session: sign in again
	LASTFM_STATE_REFUSED,		  // Last.fm refused a request; `detail` says why
} lastfm_state_t;

typedef enum {
	LASTFM_LOGIN_OK,
	LASTFM_LOGIN_WRONG_CREDENTIALS,
	LASTFM_LOGIN_NO_REPLY,
	LASTFM_LOGIN_REFUSED, // `detail` says why
} lastfm_login_result_t;

typedef struct {
	lastfm_state_t state;
	bool enabled;
	char user[128];
	char detail[160]; // Last.fm's own message or the transport error, in English
	int queued;		  // scrobbles not sent yet

	// Moves each time a sign-in attempt finishes; `login_result` is how it did.
	unsigned login_serial;
	lastfm_login_result_t login_result;
} lastfm_status_t;

// Reads the session and the queue and starts the worker. Call once, after
// config_init() and streamkeys_init().
void lastfm_init(void);

bool lastfm_available(void);

// Stored in the configuration. GUI thread.
void lastfm_set_enabled(bool on);

// Switched on, with keys and an account, signed in or with a session waiting
// to be renewed: plays are being counted.
bool lastfm_active(void);

// Both return at once; the worker does the work. The password is wiped as soon
// as the request has been built.
void lastfm_login(const char *user, const char *password);
void lastfm_logout(void);

void lastfm_get_status(lastfm_status_t *out);

typedef struct {
	bool playing;
	const char *file; // the loaded file, "" for none
	double position;  // seconds
	double duration;  // seconds, 0 when unknown
	const char *artist;
	const char *title;
	const char *album;
	bool skip; // radio, audiobooks and podcasts are never scrobbled
} lastfm_playback_t;

// GUI thread, about once a second. Tags may arrive empty on the first calls
// for a new file and be filled in later.
void lastfm_note_playback(const lastfm_playback_t *now);

// Last.fm's rule for a play that counts: a track of at least 30 seconds,
// listened to for half its length or for four minutes, whichever comes first.
// `listened` is time actually played, in seconds. The player's own play counts
// (playcount.h) go by the same rule, whether Last.fm is in use or not.
bool lastfm_play_counts(double length, double listened);

// True while a request is on the wire, so the Wi-Fi is not parked under it.
bool lastfm_network_wanted(void);

#endif /* LASTFM_H */
