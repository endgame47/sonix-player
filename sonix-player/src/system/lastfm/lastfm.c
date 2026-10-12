#define _GNU_SOURCE 1

#include "lastfm.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/config.h"
#include "src/system/core/md5.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/net/http.h"
#include "src/system/net/wifi.h"
#include "src/system/streaming/streamkeys.h"

#define API_URL "https://ws.audioscrobbler.com/2.0/"
#define API_TIMEOUT_SECS 15
#define BODY_LIMIT (64 * 1024)

// track.scrobble takes at most 50 plays per request.
#define BATCH_MAX 50
#define QUEUE_MAX 5000
#define PENDING_MAX 256
#define FIELD_MAX 256

// 2020-01-01 UTC. A clock before it has not been set since the last power-off.
#define CLOCK_VALID_AFTER 1577836800LL

#define RETRY_FIRST_SECS 30.0
#define RETRY_MAX_SECS 600.0
// How often the worker looks at the Wi-Fi and the clock while something is
// waiting on them.
#define WAIT_POLL_SECS 10

// Last.fm's "invalid session key" this many times in a row, with the usual
// retry pauses between, before the session counts as lost. One refusal can
// come from Last.fm having a bad moment rather than from the key.
#define SESSION_REFUSALS_MAX 3

// Last.fm's rule: a track of at least 30 seconds, listened to for half its
// length or for four minutes.
#define SCROBBLE_MIN_LENGTH 30.0
#define SCROBBLE_ENOUGH 240.0

typedef struct {
	long long when;	   // UTC seconds of the start of the play, 0 while not known
	double boot_start; // CLOCK_BOOTTIME of the start, to work `when` out later
	int duration;
	char artist[FIELD_MAX];
	char title[FIELD_MAX];
	char album[FIELD_MAX];
} play_t;

// ---------------------------------------------------------------------------
// state shared with the worker, under `lock`
// ---------------------------------------------------------------------------

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake;
static bool started;

static const char *api_key;
static const char *api_secret;

static bool enabled;
static char session_key[64];
static char user[128];
static bool session_expired;
static int session_refusals;
static bool refused;
static char detail[160];

static bool login_requested;
static bool logout_requested;
static bool logging_in;
static char login_user[128];
static char login_password[256];
static unsigned login_serial;
static lastfm_login_result_t login_result;

// Plays that have qualified and are not in the queue file yet: the file waits
// for a card that can be written, and a play waits for a clock that is set.
static play_t pending[PENDING_MAX];
static int pending_count;
static int file_count; // plays in the queue file; 0 while there is no card

static bool now_playing_waiting;
static play_t now_playing;
static unsigned now_playing_generation;
static unsigned current_generation;
static bool current_playing;

static int requests_in_flight;

static char session_path[512];

// Worker thread only. `card_ready` is true once the queue on the card now in
// the slot has been checked and counted.
static bool card_ready;
static char queue_path[600];
static char queue_tmp_path[610];

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static double boot_seconds(void) {
	struct timespec ts;
#ifdef CLOCK_BOOTTIME
	if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
#endif
		clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool clock_valid(void) { return (long long)time(NULL) >= CLOCK_VALID_AFTER; }

static void wipe(void *p, size_t n) {
	volatile unsigned char *b = p;
	while (n--) {
		*b++ = 0;
	}
}

static void copy_field(char *dst, const char *src) { snprintf(dst, FIELD_MAX, "%s", src ? src : ""); }

// Under `lock`. Plays are counted for an account that is signed in, and for
// one whose session has to be renewed: they wait in the queue on the card and
// go out once the same account signs in again.
static bool counting_locked(void) { return enabled && user[0] && (session_key[0] || session_expired); }

static bool online(void) {
	wifi_status_t st;
	wifi_get_status(&st);
	return st.state == WIFI_STATE_CONNECTED && st.ip[0];
}

// The session lives in the folder that holds device_config.ini: /usr/data on
// the device, there with or without a card.
static void build_paths(void) {
	char dir[400] = "/tmp";
	const char *cfg = config_path();
	if (cfg && cfg[0]) {
		const char *slash = strrchr(cfg, '/');
		if (slash == cfg) {
			snprintf(dir, sizeof(dir), "/");
		} else if (slash) {
			snprintf(dir, sizeof(dir), "%.*s", (int)(slash - cfg), cfg);
		} else {
			snprintf(dir, sizeof(dir), ".");
		}
	}
	snprintf(session_path, sizeof(session_path), "%s/lastfm.session", dir);
}

// ---------------------------------------------------------------------------
// the session file: the key on the first line, the user on the second
// ---------------------------------------------------------------------------

static void load_session(void) {
	FILE *f = fopen(session_path, "r");
	if (!f) {
		return;
	}
	char line[256];
	if (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		snprintf(session_key, sizeof(session_key), "%.*s", (int)sizeof(session_key) - 1, line);
	}
	if (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		snprintf(user, sizeof(user), "%.*s", (int)sizeof(user) - 1, line);
	}
	fclose(f);
	wipe(line, sizeof(line));
}

// Worker thread. An empty key with a user is a session that expired.
static void save_session(const char *key, const char *name) {
	if (!key[0] && !name[0]) {
		unlink(session_path);
		return;
	}
	char tmp[530];
	snprintf(tmp, sizeof(tmp), "%s.tmp", session_path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		fprintf(stderr, "lastfm: cannot write %s: %s\n", tmp, strerror(errno));
		return;
	}
	chmod(tmp, 0600);
	fprintf(f, "%s\n%s\n", key, name);
	bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, session_path) != 0) {
		fprintf(stderr, "lastfm: cannot write %s\n", session_path);
		unlink(tmp);
	}
}

// ---------------------------------------------------------------------------
// the queue file
//
// .local/lastfm.queue on the card. The first line names the account the plays
// belong to, `#lastfm \t user`; a file for another account, or without that
// line, is thrown away. Then one play per line:
//
//     when \t duration \t artist \t title \t album
//
// In the text fields '%', tab, CR and LF are written as %25, %09, %0D and %0A;
// the album may be empty. Only the worker touches the file.
// ---------------------------------------------------------------------------

#define QUEUE_HEADER "#lastfm\t"

static bool card_writable(void) { return storage_sd_root() && storage_sd_device() && storage_card_attached() && storage_sd_writable() && !usb_storage_active(); }

static void queue_escape(FILE *f, const char *s) {
	for (; *s; s++) {
		if (*s == '%' || *s == '\t' || *s == '\r' || *s == '\n') {
			fprintf(f, "%%%02X", (unsigned char)*s);
		} else {
			fputc(*s, f);
		}
	}
}

static int hex_digit(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

// Unescapes [from, to) into a FIELD_MAX buffer, cutting what does not fit.
static void queue_unescape(const char *from, const char *to, char *out) {
	size_t n = 0;
	while (from < to && n + 1 < FIELD_MAX) {
		int hi, lo;
		if (*from == '%' && to - from >= 3 && (hi = hex_digit(from[1])) >= 0 && (lo = hex_digit(from[2])) >= 0) {
			out[n++] = (char)(hi * 16 + lo);
			from += 3;
		} else {
			out[n++] = *from++;
		}
	}
	out[n] = '\0';
}

static bool queue_parse(const char *line, play_t *out) {
	const char *field[5];
	const char *end[5];
	const char *p = line;
	for (int i = 0; i < 5; i++) {
		field[i] = p;
		const char *stop = i < 4 ? strchr(p, '\t') : p + strcspn(p, "\r\n");
		if (!stop) {
			return false;
		}
		end[i] = stop;
		p = stop + 1;
	}

	memset(out, 0, sizeof(*out));
	char *num_end = NULL;
	out->when = strtoll(field[0], &num_end, 10);
	if (num_end != end[0] || out->when < CLOCK_VALID_AFTER) {
		return false;
	}
	out->duration = (int)strtol(field[1], &num_end, 10);
	if (num_end != end[1] || out->duration < 0) {
		out->duration = 0;
	}
	queue_unescape(field[2], end[2], out->artist);
	queue_unescape(field[3], end[3], out->title);
	queue_unescape(field[4], end[4], out->album);
	return out->artist[0] && out->title[0];
}

// Whether `line` is the header for `name`.
static bool queue_header_is(const char *line, const char *name) {
	size_t len = strlen(QUEUE_HEADER);
	if (strncmp(line, QUEUE_HEADER, len) != 0) {
		return false;
	}
	const char *who = line + len;
	size_t who_len = strcspn(who, "\r\n");
	return who_len == strlen(name) && strncasecmp(who, name, who_len) == 0;
}

// Checks the queue on the card in the slot against the account signed in and
// counts its plays. False when there is no card to write to.
static bool queue_attach(const char *name) {
	if (!card_writable()) {
		return false;
	}
	char dir[520];
	snprintf(dir, sizeof(dir), "%s/.local", storage_sd_root());
	mkdir(dir, 0755);
	snprintf(queue_path, sizeof(queue_path), "%s/lastfm.queue", dir);
	snprintf(queue_tmp_path, sizeof(queue_tmp_path), "%s.tmp", queue_path);

	int plays = 0;
	FILE *f = fopen(queue_path, "r");
	if (f) {
		char *line = NULL;
		size_t cap = 0;
		bool mine = getline(&line, &cap, f) >= 0 && queue_header_is(line, name);
		while (mine && getline(&line, &cap, f) >= 0) {
			plays++;
		}
		free(line);
		fclose(f);
		if (!mine) {
			printf("lastfm: %s is not %s's; discarding it\n", queue_path, name);
			unlink(queue_path);
			plays = 0;
		}
	}
	pthread_mutex_lock(&lock);
	file_count = plays;
	pthread_mutex_unlock(&lock);
	return true;
}

static bool queue_append(const play_t *plays, int count, const char *name) {
	FILE *f = fopen(queue_path, "a");
	if (!f) {
		fprintf(stderr, "lastfm: cannot open %s: %s\n", queue_path, strerror(errno));
		return false;
	}
	if (ftell(f) == 0) {
		fprintf(f, QUEUE_HEADER "%s\n", name);
	}
	for (int i = 0; i < count; i++) {
		fprintf(f, "%lld\t%d\t", plays[i].when, plays[i].duration);
		queue_escape(f, plays[i].artist);
		fputc('\t', f);
		queue_escape(f, plays[i].title);
		fputc('\t', f);
		queue_escape(f, plays[i].album);
		fputc('\n', f);
	}
	bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	return ok;
}

// Up to `max` plays from the head of the queue. `*lines_used` counts every play
// line read, unreadable ones included, so that they go with the batch.
static int queue_read_head(play_t *out, int max, int *lines_used) {
	*lines_used = 0;
	FILE *f = fopen(queue_path, "r");
	if (!f) {
		return 0;
	}
	char *line = NULL;
	size_t cap = 0;
	int count = 0;
	if (getline(&line, &cap, f) >= 0) { // the header
		while (count < max && getline(&line, &cap, f) >= 0) {
			(*lines_used)++;
			if (queue_parse(line, &out[count])) {
				count++;
			}
		}
	}
	free(line);
	fclose(f);
	return count;
}

// Removes the first `lines` plays, keeping the header; the file goes when no
// play is left.
static void queue_drop_head(int lines) {
	FILE *in = fopen(queue_path, "r");
	if (!in) {
		return;
	}
	FILE *out = fopen(queue_tmp_path, "w");
	if (!out) {
		fclose(in);
		return;
	}
	char *line = NULL;
	size_t cap = 0;
	int n = -1; // the header is line -1
	bool any = false;
	while (getline(&line, &cap, in) >= 0) {
		if (n < 0) {
			fputs(line, out);
		} else if (n >= lines) {
			fputs(line, out);
			any = true;
		}
		n++;
	}
	free(line);
	fclose(in);
	bool ok = fflush(out) == 0 && fsync(fileno(out)) == 0;
	ok = fclose(out) == 0 && ok;
	if (!ok || !any) {
		unlink(queue_tmp_path);
		if (ok) {
			unlink(queue_path);
		}
		return;
	}
	if (rename(queue_tmp_path, queue_path) != 0) {
		unlink(queue_tmp_path);
	}
}

// ---------------------------------------------------------------------------
// a signed request
//
// api_sig is the MD5 of every other parameter, sorted by name and written as
// name then value with nothing between, followed by the secret.
// ---------------------------------------------------------------------------

typedef struct {
	char name[24]; // "timestamp[49]" is the longest
	const char *value;
} param_t;

typedef struct {
	char *buf;
	size_t len;
	size_t cap;
	bool failed;
} text_t;

static void text_add(text_t *t, const char *s, size_t n) {
	if (t->failed) {
		return;
	}
	if (t->len + n + 1 > t->cap) {
		size_t cap = t->cap ? t->cap : 1024;
		while (cap < t->len + n + 1) {
			cap *= 2;
		}
		char *grown = malloc(cap);
		if (!grown) {
			t->failed = true;
			return;
		}
		// Copied and wiped rather than realloc'd: the text holds the password
		// and the secret.
		if (t->buf) {
			memcpy(grown, t->buf, t->len);
			wipe(t->buf, t->cap);
			free(t->buf);
		}
		t->buf = grown;
		t->cap = cap;
	}
	memcpy(t->buf + t->len, s, n);
	t->len += n;
	t->buf[t->len] = '\0';
}

static void text_str(text_t *t, const char *s) { text_add(t, s, strlen(s)); }

static void text_encoded(text_t *t, const char *s) {
	static const char hex[] = "0123456789ABCDEF";
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
			text_add(t, (const char *)p, 1);
		} else {
			char esc[3] = {'%', hex[*p >> 4], hex[*p & 15]};
			text_add(t, esc, 3);
		}
	}
}

static void text_free(text_t *t) {
	if (t->buf) {
		wipe(t->buf, t->cap);
	}
	free(t->buf);
	memset(t, 0, sizeof(*t));
}

static int param_cmp(const void *a, const void *b) { return strcmp(((const param_t *)a)->name, ((const param_t *)b)->name); }

typedef enum {
	RESULT_OK,
	RESULT_RETRY,		// no answer, or Last.fm busy: try again later
	RESULT_SESSION,		// the session key is no longer valid
	RESULT_BAD_REQUEST, // the parameters were refused
	RESULT_REFUSED,		// anything else Last.fm said no to
} result_t;

// The text of an entity-encoded XML fragment [from, to), into out.
static void xml_text(const char *from, const char *to, char *out, size_t size) {
	static const struct {
		const char *entity;
		char c;
	} ENTITIES[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
	size_t n = 0;
	while (from < to && n + 1 < size) {
		bool done = false;
		for (size_t i = 0; i < sizeof(ENTITIES) / sizeof(ENTITIES[0]) && !done; i++) {
			size_t len = strlen(ENTITIES[i].entity);
			if ((size_t)(to - from) >= len && strncmp(from, ENTITIES[i].entity, len) == 0) {
				out[n++] = ENTITIES[i].c;
				from += len;
				done = true;
			}
		}
		if (!done) {
			out[n++] = *from++;
		}
	}
	out[n] = '\0';
}

// The text of the first <tag>...</tag> in body.
static bool xml_tag(const char *body, const char *tag, char *out, size_t size) {
	char open[32], close[32];
	snprintf(open, sizeof(open), "<%s>", tag);
	snprintf(close, sizeof(close), "</%s>", tag);
	const char *start = strstr(body, open);
	if (!start) {
		return false;
	}
	start += strlen(open);
	const char *end = strstr(start, close);
	if (!end) {
		return false;
	}
	xml_text(start, end, out, size);
	return out[0] != '\0';
}

// Sends the parameters, signed, and sorts the answer. api_key and api_sig are
// added to `params`, which needs two free slots past `count`. `why` gets
// Last.fm's message or the transport error, `code_out` Last.fm's error code
// (0 when there is none). On success the body is handed back in *body_out
// when that is not NULL.
static result_t call(param_t *params, int count, char *why, size_t why_size, int *code_out, char **body_out) {
	why[0] = '\0';
	if (code_out) {
		*code_out = 0;
	}
	snprintf(params[count].name, sizeof(params[count].name), "api_key");
	params[count].value = api_key;
	count++;
	qsort(params, (size_t)count, sizeof(params[0]), param_cmp);

	text_t sig = {0};
	for (int i = 0; i < count; i++) {
		text_str(&sig, params[i].name);
		text_str(&sig, params[i].value);
	}
	text_str(&sig, api_secret);
	char api_sig[MD5_HEX_LEN] = "";
	bool sig_failed = sig.failed;
	if (!sig_failed) {
		md5_hex(sig.buf, sig.len, api_sig);
	}
	text_free(&sig);

	snprintf(params[count].name, sizeof(params[count].name), "api_sig");
	params[count].value = api_sig;
	count++;

	text_t form = {0};
	for (int i = 0; i < count; i++) {
		if (i) {
			text_str(&form, "&");
		}
		text_str(&form, params[i].name);
		text_str(&form, "=");
		text_encoded(&form, params[i].value);
	}
	if (form.failed || sig_failed) {
		text_free(&form);
		snprintf(why, why_size, "out of memory");
		return RESULT_RETRY;
	}

	http_req_t req = {
		.method = "POST",
		.body = form.buf,
		.body_len = form.len,
		.content_type = "application/x-www-form-urlencoded",
		.want_error_body = true,
		.allow_empty_body = true,
	};

	pthread_mutex_lock(&lock);
	requests_in_flight++;
	pthread_mutex_unlock(&lock);

	char *body = NULL;
	size_t body_len = 0;
	int status = 0;
	http_request(API_URL, &req, &body, &body_len, BODY_LIMIT, API_TIMEOUT_SECS, &status);

	pthread_mutex_lock(&lock);
	requests_in_flight--;
	pthread_mutex_unlock(&lock);
	text_free(&form);

	result_t result;
	const char *lfm = body ? strstr(body, "<lfm") : NULL;
	if (!lfm) {
		const char *err = http_last_error();
		if (err && err[0]) {
			snprintf(why, why_size, "%s", err);
		} else {
			snprintf(why, why_size, "HTTP %d", status);
		}
		result = RESULT_RETRY;
	} else if (strstr(lfm, "status=\"ok\"")) {
		result = RESULT_OK;
	} else {
		int code = 0;
		const char *err = strstr(lfm, "<error");
		if (err) {
			const char *attr = strstr(err, "code=\"");
			code = attr ? atoi(attr + 6) : 0;
			const char *text = strchr(err, '>');
			const char *text_end = text ? strchr(text + 1, '<') : NULL;
			if (text && text_end) {
				xml_text(text + 1, text_end, why, why_size);
			}
		}
		if (!why[0]) {
			snprintf(why, why_size, "error %d", code);
		}
		if (code_out) {
			*code_out = code;
		}
		switch (code) {
		case 9: // invalid session key
			result = RESULT_SESSION;
			break;
		case 8:	 // operation failed
		case 11: // service offline
		case 16: // temporarily unavailable
		case 29: // rate limit exceeded
			result = RESULT_RETRY;
			break;
		case 6: // invalid parameters
		case 7: // invalid resource
			result = RESULT_BAD_REQUEST;
			break;
		default:
			result = RESULT_REFUSED;
			break;
		}
	}

	if (result == RESULT_OK && body_out) {
		*body_out = body;
	} else {
		free(body);
	}
	return result;
}

// ---------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------

static void do_login(const char *name, char *password) {
	param_t params[5] = {
		{"method", "auth.getMobileSession"},
		{"username", name},
		{"password", password},
	};
	char why[160];
	int code = 0;
	char *body = NULL;
	result_t result = call(params, 3, why, sizeof(why), &code, &body);
	wipe(password, strlen(password));

	char key[64] = "";
	char name_out[128] = "";
	if (result == RESULT_OK && (!xml_tag(body, "key", key, sizeof(key)) || !xml_tag(body, "name", name_out, sizeof(name_out)))) {
		snprintf(why, sizeof(why), "no session in the answer");
		result = RESULT_REFUSED;
	}
	free(body);

	lastfm_login_result_t outcome;
	if (result == RESULT_OK) {
		outcome = LASTFM_LOGIN_OK;
	} else if (result == RESULT_RETRY) {
		outcome = LASTFM_LOGIN_NO_REPLY;
	} else if (code == 4) { // authentication failed
		outcome = LASTFM_LOGIN_WRONG_CREDENTIALS;
	} else {
		outcome = LASTFM_LOGIN_REFUSED;
	}

	if (outcome == LASTFM_LOGIN_OK) {
		save_session(key, name_out);
		printf("lastfm: signed in as %s\n", name_out);
	} else {
		fprintf(stderr, "lastfm: sign-in failed: %s\n", why);
	}

	pthread_mutex_lock(&lock);
	logging_in = false;
	login_result = outcome;
	login_serial++;
	if (outcome == LASTFM_LOGIN_OK) {
		snprintf(session_key, sizeof(session_key), "%s", key);
		snprintf(user, sizeof(user), "%s", name_out);
		session_expired = false;
		session_refusals = 0;
		refused = false;
		detail[0] = '\0';
	} else {
		snprintf(detail, sizeof(detail), "%s", why);
	}
	pthread_mutex_unlock(&lock);
	wipe(key, sizeof(key));
}

static void do_logout(void) {
	save_session("", "");
	if (card_ready) {
		unlink(queue_path);
	}
	card_ready = false;
	pthread_mutex_lock(&lock);
	session_key[0] = '\0';
	user[0] = '\0';
	session_expired = false;
	refused = false;
	detail[0] = '\0';
	file_count = 0;
	pending_count = 0;
	now_playing_waiting = false;
	pthread_mutex_unlock(&lock);
	printf("lastfm: signed out\n");
}

static void session_lost(const char *why) {
	fprintf(stderr, "lastfm: the session was refused (%s); sign in again\n", why);
	pthread_mutex_lock(&lock);
	session_key[0] = '\0';
	session_expired = true;
	session_refusals = 0;
	char name[128];
	snprintf(name, sizeof(name), "%s", user);
	pthread_mutex_unlock(&lock);
	save_session("", name);
}

// An "invalid session key" answer. True when the session is now lost; until
// then the key stays and the caller tries again later.
static bool session_refused(const char *why) {
	pthread_mutex_lock(&lock);
	int count = ++session_refusals;
	pthread_mutex_unlock(&lock);
	if (count >= SESSION_REFUSALS_MAX) {
		session_lost(why);
		return true;
	}
	fprintf(stderr, "lastfm: the session was refused (%s), %d of %d; keeping it for now\n", why, count, SESSION_REFUSALS_MAX);
	return false;
}

static void session_accepted(void) {
	pthread_mutex_lock(&lock);
	session_refusals = 0;
	pthread_mutex_unlock(&lock);
}

// Puts the plays whose time is known into the file on the card. A play that
// started before the clock was set gets its time once the clock is right,
// counted back on CLOCK_BOOTTIME, which keeps running through suspend. Plays
// stay here while there is no card to write them to.
static void flush_pending(const char *name) {
	bool valid = clock_valid();
	long long now_real = (long long)time(NULL);
	double now_boot = boot_seconds();

	pthread_mutex_lock(&lock);
	for (int i = 0; i < pending_count; i++) {
		if (!pending[i].when && valid) {
			pending[i].when = now_real - (long long)(now_boot - pending[i].boot_start);
		}
	}
	pthread_mutex_unlock(&lock);
	if (!card_ready) {
		return;
	}

	play_t *ready = malloc(sizeof(play_t) * PENDING_MAX);
	if (!ready) {
		return;
	}
	pthread_mutex_lock(&lock);
	int ready_count = 0;
	for (int i = 0; i < pending_count; i++) {
		if (pending[i].when) {
			ready[ready_count++] = pending[i];
		}
	}
	int room = QUEUE_MAX - file_count;
	pthread_mutex_unlock(&lock);
	if (!ready_count) {
		free(ready);
		return;
	}

	if (room < 0) {
		room = 0;
	}
	int kept = ready_count > room ? room : ready_count;
	if (kept < ready_count) {
		fprintf(stderr, "lastfm: the queue is full; %d plays not kept\n", ready_count - kept);
	}
	bool written = kept == 0 || queue_append(ready, kept, name);
	free(ready);
	if (!written) {
		return; // still in `pending`, for the next pass
	}

	// Out of `pending` go the plays just handled; the GUI thread may have added
	// more behind them in the meantime.
	pthread_mutex_lock(&lock);
	file_count += kept;
	int keep = 0;
	for (int i = 0; i < pending_count; i++) {
		if (!pending[i].when) {
			pending[keep++] = pending[i];
		}
	}
	pending_count = keep;
	pthread_mutex_unlock(&lock);
}

static void send_now_playing(const play_t *track, const char *sk) {
	param_t params[8];
	int n = 0;
	char duration[16];
	params[n++] = (param_t){"method", "track.updateNowPlaying"};
	params[n++] = (param_t){"sk", sk};
	params[n++] = (param_t){"artist", track->artist};
	params[n++] = (param_t){"track", track->title};
	if (track->album[0]) {
		params[n++] = (param_t){"album", track->album};
	}
	if (track->duration > 0) {
		snprintf(duration, sizeof(duration), "%d", track->duration);
		params[n++] = (param_t){"duration", duration};
	}
	char why[160];
	result_t result = call(params, n, why, sizeof(why), NULL, NULL);
	// A refused session is left to the scrobbles to count: they are retried,
	// "now playing" is not.
	if (result == RESULT_OK) {
		session_accepted();
	} else {
		fprintf(stderr, "lastfm: now playing not sent: %s\n", why);
	}
}

// Sends up to `max` plays from the head of the queue. `*lines` is how many
// lines of the file the batch took up.
static result_t send_batch(int max, const char *sk, char *why, size_t why_size, int *lines) {
	*lines = 0;
	why[0] = '\0';
	play_t *plays = malloc(sizeof(play_t) * (size_t)max);
	// Five per play at most, plus method and sk, plus the two call() adds.
	param_t *params = malloc(sizeof(param_t) * (size_t)(max * 5 + 4));
	char(*numbers)[2][16] = malloc(sizeof(*numbers) * (size_t)max);
	if (!plays || !params || !numbers) {
		free(plays);
		free(params);
		free(numbers);
		snprintf(why, why_size, "out of memory");
		return RESULT_RETRY;
	}

	int count = queue_read_head(plays, max, lines);
	result_t result = RESULT_OK;
	if (count > 0) {
		int n = 0;
		params[n++] = (param_t){"method", "track.scrobble"};
		params[n++] = (param_t){"sk", sk};
		for (int i = 0; i < count; i++) {
			snprintf(numbers[i][0], sizeof(numbers[i][0]), "%lld", plays[i].when);
			snprintf(numbers[i][1], sizeof(numbers[i][1]), "%d", plays[i].duration);
			snprintf(params[n].name, sizeof(params[n].name), "artist[%d]", i);
			params[n++].value = plays[i].artist;
			snprintf(params[n].name, sizeof(params[n].name), "track[%d]", i);
			params[n++].value = plays[i].title;
			snprintf(params[n].name, sizeof(params[n].name), "timestamp[%d]", i);
			params[n++].value = numbers[i][0];
			if (plays[i].album[0]) {
				snprintf(params[n].name, sizeof(params[n].name), "album[%d]", i);
				params[n++].value = plays[i].album;
			}
			if (plays[i].duration > 0) {
				snprintf(params[n].name, sizeof(params[n].name), "duration[%d]", i);
				params[n++].value = numbers[i][1];
			}
		}
		result = call(params, n, why, why_size, NULL, NULL);
		if (result == RESULT_OK) {
			printf("lastfm: %d scrobbles sent\n", count);
		}
	}

	free(plays);
	free(params);
	free(numbers);
	return result;
}

typedef struct {
	int batch;
	double backoff;
	double retry_at; // boot seconds
	bool was_online;
} sender_t;

// One batch off the queue, and what to do after it.
static void send_queue(sender_t *s, const char *sk) {
	char why[160];
	int lines = 0;
	result_t result = send_batch(s->batch, sk, why, sizeof(why), &lines);
	bool drop = false;

	switch (result) {
	case RESULT_OK:
		drop = true;
		session_accepted();
		s->backoff = RETRY_FIRST_SECS;
		pthread_mutex_lock(&lock);
		refused = false;
		detail[0] = '\0';
		pthread_mutex_unlock(&lock);
		break;
	case RESULT_BAD_REQUEST:
		// One play of the batch is the problem: they go one at a time until
		// it is the one being sent, and then it is let go.
		if (s->batch > 1) {
			s->batch = 1;
		} else {
			fprintf(stderr, "lastfm: a queued play was refused (%s); dropping it\n", why);
			drop = true;
		}
		break;
	case RESULT_SESSION:
		if (!session_refused(why)) {
			s->retry_at = boot_seconds() + s->backoff;
			s->backoff = s->backoff * 2 > RETRY_MAX_SECS ? RETRY_MAX_SECS : s->backoff * 2;
		}
		break;
	case RESULT_REFUSED:
		fprintf(stderr, "lastfm: scrobbles refused: %s\n", why);
		pthread_mutex_lock(&lock);
		refused = true;
		snprintf(detail, sizeof(detail), "%s", why);
		pthread_mutex_unlock(&lock);
		s->retry_at = boot_seconds() + RETRY_MAX_SECS;
		break;
	case RESULT_RETRY:
		fprintf(stderr, "lastfm: scrobbles not sent (%s); again in %.0f s\n", why, s->backoff);
		s->retry_at = boot_seconds() + s->backoff;
		s->backoff = s->backoff * 2 > RETRY_MAX_SECS ? RETRY_MAX_SECS : s->backoff * 2;
		break;
	}

	pthread_mutex_lock(&lock);
	if (lines == 0) {
		// Nothing readable where the count says there are lines: the file is
		// gone or empty.
		file_count = 0;
	} else if (drop) {
		file_count = file_count > lines ? file_count - lines : 0;
	}
	// One at a time lasts until the queue is empty.
	if (file_count == 0) {
		s->batch = BATCH_MAX;
	}
	pthread_mutex_unlock(&lock);
	if (drop && lines > 0) {
		queue_drop_head(lines);
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	sender_t sender = {.batch = BATCH_MAX, .backoff = RETRY_FIRST_SECS};

	for (;;) {
		pthread_mutex_lock(&lock);
		bool out = logout_requested;
		bool in = login_requested;
		char name[128] = "";
		char password[256] = "";
		if (in) {
			snprintf(name, sizeof(name), "%s", login_user);
			snprintf(password, sizeof(password), "%s", login_password);
			wipe(login_password, sizeof(login_password));
		}
		logout_requested = false;
		login_requested = false;
		pthread_mutex_unlock(&lock);

		if (out) {
			do_logout();
		}
		if (in) {
			do_login(name, password);
			wipe(password, sizeof(password));
			card_ready = false; // checked again against the account now signed in
			sender.retry_at = 0;
			sender.backoff = RETRY_FIRST_SECS;
		}

		pthread_mutex_lock(&lock);
		bool active = enabled && session_key[0];
		bool counting = counting_locked();
		char sk[64];
		char account[128];
		snprintf(sk, sizeof(sk), "%s", session_key);
		snprintf(account, sizeof(account), "%s", user);
		pthread_mutex_unlock(&lock);

		// Plays go to the card while there is an account to keep them for;
		// they go to Last.fm only with a session.
		bool up = false;
		if (counting) {
			// The card comes and goes: pulled out, shared over USB, read-only.
			if (!card_writable()) {
				if (card_ready) {
					card_ready = false;
					pthread_mutex_lock(&lock);
					file_count = 0;
					pthread_mutex_unlock(&lock);
				}
			} else if (!card_ready) {
				card_ready = queue_attach(account);
			}
			flush_pending(account);
		}
		if (active) {
			up = online();
			if (up && !sender.was_online) {
				sender.retry_at = 0;
				sender.backoff = RETRY_FIRST_SECS;
			}
			sender.was_online = up;

			if (up && boot_seconds() >= sender.retry_at) {
				// "Now playing" once per track, for the track still playing,
				// and not retried.
				pthread_mutex_lock(&lock);
				bool announce = false;
				play_t track;
				if (now_playing_waiting && (now_playing_generation != current_generation || !current_playing)) {
					now_playing_waiting = false;
				} else if (now_playing_waiting) {
					now_playing_waiting = false;
					announce = true;
					track = now_playing;
				}
				bool queued = file_count > 0;
				pthread_mutex_unlock(&lock);

				if (announce) {
					send_now_playing(&track, sk);
				}
				if (queued) {
					send_queue(&sender, sk);
				}
			}
		}

		// Asleep until there is something to do. While plays wait for the
		// network, the clock or a card, awake now and then to look again; with
		// more to send and nothing in the way, straight on.
		pthread_mutex_lock(&lock);
		active = enabled && session_key[0];
		counting = counting_locked();
		bool more_now = active && up && file_count > 0 && boot_seconds() >= sender.retry_at;
		bool waiting = (counting && (!card_ready || pending_count > 0)) || (active && (file_count > 0 || now_playing_waiting));
		if (!logout_requested && !login_requested && !more_now) {
			if (waiting) {
				struct timespec ts;
				clock_gettime(CLOCK_MONOTONIC, &ts);
				ts.tv_sec += WAIT_POLL_SECS;
				pthread_cond_timedwait(&wake, &lock, &ts);
			} else {
				pthread_cond_wait(&wake, &lock);
			}
		}
		pthread_mutex_unlock(&lock);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// the public side
// ---------------------------------------------------------------------------

void lastfm_init(void) {
	if (started) {
		return;
	}
	api_key = streamkeys_lastfm_key();
	api_secret = streamkeys_lastfm_secret();
	build_paths();

	pthread_condattr_t attr;
	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&wake, &attr);
	pthread_condattr_destroy(&attr);

	pthread_mutex_lock(&lock);
	enabled = config_get_bool("lastfm", "enabled", false);
	load_session();
	session_expired = !session_key[0] && user[0];
	pthread_mutex_unlock(&lock);

	if (!api_key || !api_secret) {
		printf("lastfm: no API keys; Last.fm stays off\n");
		return;
	}

	pthread_t thread;
	if (pthread_create(&thread, NULL, worker_main, NULL) != 0) {
		fprintf(stderr, "lastfm: cannot start the worker\n");
		return;
	}
	pthread_detach(thread);
	started = true;
	printf("lastfm: %s, %s\n", enabled ? "on" : "off", session_key[0] ? "signed in" : "signed out");
}

bool lastfm_available(void) { return started; }

void lastfm_set_enabled(bool on) {
	config_set_bool("lastfm", "enabled", on);
	config_save();
	pthread_mutex_lock(&lock);
	enabled = on;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
}

bool lastfm_active(void) {
	if (!started) {
		return false;
	}
	pthread_mutex_lock(&lock);
	bool counting = counting_locked();
	pthread_mutex_unlock(&lock);
	return counting;
}

void lastfm_login(const char *name, const char *password) {
	if (!started || !name || !name[0] || !password || !password[0]) {
		return;
	}
	pthread_mutex_lock(&lock);
	if (!logging_in) {
		snprintf(login_user, sizeof(login_user), "%s", name);
		snprintf(login_password, sizeof(login_password), "%s", password);
		logging_in = true;
		login_requested = true;
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);
}

void lastfm_logout(void) {
	if (!started) {
		return;
	}
	pthread_mutex_lock(&lock);
	logout_requested = true;
	session_key[0] = '\0';
	session_expired = false;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
}

void lastfm_get_status(lastfm_status_t *out) {
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&lock);
	out->enabled = enabled;
	snprintf(out->user, sizeof(out->user), "%s", session_key[0] ? user : "");
	snprintf(out->detail, sizeof(out->detail), "%s", detail);
	out->queued = file_count + pending_count;
	out->login_serial = login_serial;
	out->login_result = login_result;
	if (!started) {
		out->state = LASTFM_STATE_UNAVAILABLE;
	} else if (logging_in) {
		out->state = LASTFM_STATE_SIGNING_IN;
	} else if (!enabled) {
		out->state = LASTFM_STATE_OFF;
	} else if (session_expired) {
		out->state = LASTFM_STATE_SESSION_EXPIRED;
	} else if (!session_key[0]) {
		out->state = LASTFM_STATE_SIGNED_OUT;
	} else if (refused) {
		out->state = LASTFM_STATE_REFUSED;
	} else {
		out->state = LASTFM_STATE_CONNECTED;
	}
	pthread_mutex_unlock(&lock);
}

bool lastfm_network_wanted(void) {
	pthread_mutex_lock(&lock);
	bool wanted = requests_in_flight > 0;
	pthread_mutex_unlock(&lock);
	return wanted;
}

// ---------------------------------------------------------------------------
// counting plays -- GUI thread only
// ---------------------------------------------------------------------------

static struct {
	char file[512];
	unsigned generation;
	bool skip;
	bool scrobbled;
	bool was_playing;
	double listened;
	double last_tick;
	double last_position;
	double boot_start;
	long long real_start;
	play_t track;
} play;

static void take_tags(const lastfm_playback_t *now) {
	copy_field(play.track.artist, now->artist);
	copy_field(play.track.title, now->title);
	copy_field(play.track.album, now->album);
}

static void start_play(const lastfm_playback_t *now, double t) {
	snprintf(play.file, sizeof(play.file), "%s", now->file);
	play.generation++;
	play.skip = now->skip;
	play.scrobbled = false;
	play.was_playing = true;
	play.listened = 0;
	play.last_tick = t;
	play.last_position = now->position;
	play.boot_start = t;
	play.real_start = clock_valid() ? (long long)time(NULL) : 0;
	memset(&play.track, 0, sizeof(play.track));
	take_tags(now);
}

bool lastfm_play_counts(double length, double listened) {
	if (length < SCROBBLE_MIN_LENGTH) {
		return false;
	}
	double enough = length / 2 < SCROBBLE_ENOUGH ? length / 2 : SCROBBLE_ENOUGH;
	return listened >= enough;
}

static void qualify(void) {
	if (play.scrobbled || play.skip || !play.track.artist[0] || !play.track.title[0] ||
		!lastfm_play_counts(play.track.duration, play.listened)) {
		return;
	}
	play.scrobbled = true;

	play_t p = play.track;
	p.when = play.real_start;
	p.boot_start = play.boot_start;
	pthread_mutex_lock(&lock);
	if (pending_count < PENDING_MAX) {
		pending[pending_count++] = p;
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);
}

void lastfm_note_playback(const lastfm_playback_t *now) {
	if (!lastfm_active()) {
		play.file[0] = '\0';
		return;
	}

	double t = boot_seconds();
	const char *file = now->file ? now->file : "";
	bool same = play.file[0] && strcmp(play.file, file) == 0;
	// Back at the start of the same file after being well into it: played
	// again, by repeat-one or by the user.
	bool again = same && now->position < 5.0 && now->position + 10.0 < play.last_position;

	bool announce = false;
	if (now->playing && file[0] && (!same || again)) {
		start_play(now, t);
		announce = true;
	} else if (!same) {
		play.file[0] = '\0';
		return;
	} else if (!play.track.title[0] && now->title && now->title[0]) {
		// The tags came in after the file started.
		take_tags(now);
		announce = true;
	}

	if (now->playing && play.was_playing) {
		double step = t - play.last_tick;
		// A longer gap is the device having been asleep, not listening.
		if (step > 0 && step < 5.0) {
			play.listened += step;
		}
	}
	play.was_playing = now->playing;
	play.last_tick = t;
	play.last_position = now->position;
	if (now->duration > 0) {
		play.track.duration = (int)(now->duration + 0.5);
	}

	pthread_mutex_lock(&lock);
	current_generation = play.generation;
	current_playing = now->playing;
	if (announce && session_key[0] && !play.skip && play.track.artist[0] && play.track.title[0]) {
		now_playing = play.track;
		now_playing_generation = play.generation;
		now_playing_waiting = true;
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);

	qualify();
}
