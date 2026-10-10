#include "sonixlink.h"
#include "sonixlink_bt.h"

#include "src/system/core/respath.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/library/albumart.h"
#include "src/system/image/art_shrink.h"
#include "src/system/core/config.h"
#include "src/system/library/cue.h"
#include "src/system/playback/playlist.h"
// SQLite is vendored: this header, not the system's.
#include "src/system/db/sqlite3.h"
#include "src/system/device/sysinfo.h"
#include "src/system/core/utils.h"
#include "src/system/net/wifi.h"

// ---------------------------------------------------------------------------
// The numbers
// ---------------------------------------------------------------------------

#define BEACON_PORT 7801
#define BEACON_MS 2000
#define TICK_MS 200

// A phone counts as present for this long after its last request. The app
// polls every few seconds for as long as it is connected (its heartbeat), so
// this outlasts several missed polls on a busy network or a phone that has
// briefly held its network back. An app that leaves on purpose says so with
// /api/bye and is forgotten at once.
#define PEER_ALIVE_MS 20000

// The most rows one /api/browse answer carries: the file browser's own ceiling.
#define BROWSE_MAX 5000

#define MDNS_ADDR "224.0.0.251"
#define MDNS_PORT 5353
#define MDNS_TTL 120
#define MDNS_ANNOUNCE_MS 30000

#define SERVICE_TYPE "_sonixlink._tcp.local"

#define NAME_PATH RESOURCE_DIR "/bt_name"
#define DEFAULT_NAME "Sonix Player"

#define CMD_QUEUE_LEN 16
// One more than a phone polling over Wi-Fi needs at once, plus the Bluetooth
// link, which keeps its slot for as long as it stays up.
#define MAX_CLIENTS 6

// A Bluetooth link with no request for this long is closed: the app sends one
// every few seconds for as long as it is connected.
#define PERSISTENT_IDLE_MS 30000
#define PENDING_LINKS 2

// How much of a request is read before it is judged nonsense. These are short.
#define REQUEST_MAX 2048

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static bool worker_running;
static bool enabled;
static bool verbose;

static char peer_text[64];
static char address_text[64];
static uint32_t last_seen_ms;

static char db_path[600];
static char thumbs_path[600];
static char card_root[600];
static char device_name_text[128];

static sonixlink_state_t g_state;
static bool have_state;

static sonixlink_command_t queue[CMD_QUEUE_LEN];
static int queue_head;
static int queue_count;
static char pending_path[SONIXLINK_PATH_MAX];

// The queue window the interface last handed over. One arena of packed strings
// plus an index: two hundred paths cost about sixteen kilobytes rather than the
// hundred a fixed-width array of the same length would take.
// Bluetooth links handed over by sonixlink_bt.c, waiting for the worker to give
// them a client slot.
static int pending_links[PENDING_LINKS];
static int pending_link_count;

static char *queue_arena;
static size_t queue_arena_len;
static int queue_offsets[SONIXLINK_QUEUE_WINDOW];
static int queue_window_count;
static int queue_window_first;
static int queue_total;
static int queue_position;
static unsigned queue_revision;
static int queue_wanted = -1;

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

// ---------------------------------------------------------------------------
// The public face
// ---------------------------------------------------------------------------

static void push_command_list(sonixlink_command_kind_t kind, int arg, const char *path, int list,
							  const char *value) {
	pthread_mutex_lock(&lock);
	if (queue_count < CMD_QUEUE_LEN) {
		sonixlink_command_t *slot = &queue[(queue_head + queue_count) % CMD_QUEUE_LEN];
		memset(slot, 0, sizeof(*slot));
		slot->kind = kind;
		slot->arg = arg;
		if (path && path[0]) {
			snprintf(slot->path, sizeof(slot->path), "%s", path);
			// Kept as well for sonixlink_take_path(), which predates commands
			// carrying their own.
			snprintf(pending_path, sizeof(pending_path), "%s", path);
		}
		slot->list = list;
		if (value && value[0]) {
			snprintf(slot->value, sizeof(slot->value), "%s", value);
		}
		queue_count++;
	}
	pthread_mutex_unlock(&lock);
}

// A command carrying a selection: `paths` (owned from here on, freed when the
// command cannot be queued) and the list or playlist it is about.
static void push_command_paths(sonixlink_command_kind_t kind, int arg, char *paths, int path_count,
							   const char *value) {
	pthread_mutex_lock(&lock);
	if (queue_count < CMD_QUEUE_LEN) {
		sonixlink_command_t *slot = &queue[(queue_head + queue_count) % CMD_QUEUE_LEN];
		memset(slot, 0, sizeof(*slot));
		slot->kind = kind;
		slot->arg = arg;
		if (value && value[0]) {
			snprintf(slot->value, sizeof(slot->value), "%.*s", (int)sizeof(slot->value) - 1, value);
		}
		slot->paths = paths;
		slot->path_count = path_count;
		paths = NULL;
		queue_count++;
	}
	pthread_mutex_unlock(&lock);
	free(paths);
}

void sonixlink_command_free(sonixlink_command_t *cmd) {
	if (!cmd) {
		return;
	}
	free(cmd->paths);
	cmd->paths = NULL;
	cmd->path_count = 0;
}

static void push_command_path(sonixlink_command_kind_t kind, int arg, const char *path) {
	push_command_list(kind, arg, path, SONIXLINK_LIST_ALL, NULL);
}

static void push_command(sonixlink_command_kind_t kind, int arg) { push_command_path(kind, arg, NULL); }

bool sonixlink_take_command(sonixlink_command_t *out) {
	bool got = false;
	pthread_mutex_lock(&lock);
	if (queue_count > 0) {
		*out = queue[queue_head];
		queue_head = (queue_head + 1) % CMD_QUEUE_LEN;
		queue_count--;
		got = true;
	}
	pthread_mutex_unlock(&lock);
	return got;
}

void sonixlink_take_path(char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	pthread_mutex_lock(&lock);
	snprintf(out, out_size, "%s", pending_path);
	pthread_mutex_unlock(&lock);
}

void sonixlink_publish_queue(int total, int position, int first, const char *const *paths, int count,
							 unsigned revision) {
	if (count < 0) {
		count = 0;
	}
	if (count > SONIXLINK_QUEUE_WINDOW) {
		count = SONIXLINK_QUEUE_WINDOW;
	}

	size_t needed = 1;
	for (int i = 0; i < count; i++) {
		needed += (paths && paths[i]) ? strlen(paths[i]) + 1 : 1;
	}

	char *arena = malloc(needed);
	if (!arena) {
		return;
	}
	int offsets[SONIXLINK_QUEUE_WINDOW];
	size_t at = 0;
	for (int i = 0; i < count; i++) {
		const char *p = (paths && paths[i]) ? paths[i] : "";
		offsets[i] = (int)at;
		size_t n = strlen(p) + 1;
		memcpy(arena + at, p, n);
		at += n;
	}

	pthread_mutex_lock(&lock);
	free(queue_arena);
	queue_arena = arena;
	queue_arena_len = at;
	memcpy(queue_offsets, offsets, sizeof(int) * (size_t)count);
	queue_window_count = count;
	queue_window_first = first < 0 ? 0 : first;
	queue_total = total < 0 ? 0 : total;
	queue_position = position;
	queue_revision = revision;
	pthread_mutex_unlock(&lock);
}

int sonixlink_queue_wanted(void) {
	pthread_mutex_lock(&lock);
	int wanted = queue_wanted;
	pthread_mutex_unlock(&lock);
	return wanted;
}

static void art_prepare_for(const char *path);

void sonixlink_publish(const sonixlink_state_t *state) {
	if (!state) {
		return;
	}
	pthread_mutex_lock(&lock);
	bool new_track = strcmp(g_state.path, state->path) != 0;
	g_state = *state;
	have_state = true;
	pthread_mutex_unlock(&lock);
	if (new_track) {
		art_prepare_for(state->path);
	}
}

void sonixlink_adopt_link(int fd) {
	if (fd < 0) {
		return;
	}
	pthread_mutex_lock(&lock);
	bool kept = enabled && pending_link_count < PENDING_LINKS;
	if (kept) {
		pending_links[pending_link_count++] = fd;
	}
	pthread_mutex_unlock(&lock);
	if (!kept) {
		close(fd);
	}
}

bool sonixlink_is_connected(void) {
	pthread_mutex_lock(&lock);
	bool recent = last_seen_ms != 0 && (uint32_t)(now_ms() - last_seen_ms) < PEER_ALIVE_MS;
	pthread_mutex_unlock(&lock);
	return recent;
}

void sonixlink_peer(char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	pthread_mutex_lock(&lock);
	snprintf(out, out_size, "%s", peer_text);
	pthread_mutex_unlock(&lock);
}

void sonixlink_address(char *out, size_t out_size) {
	if (!out || out_size == 0) {
		return;
	}
	pthread_mutex_lock(&lock);
	snprintf(out, out_size, "%s", address_text);
	pthread_mutex_unlock(&lock);
}

void sonixlink_set_db_path(const char *path) {
	pthread_mutex_lock(&lock);
	snprintf(db_path, sizeof(db_path), "%s", path ? path : "");
	pthread_mutex_unlock(&lock);
}

void sonixlink_set_card_root(const char *path) {
	pthread_mutex_lock(&lock);
	snprintf(card_root, sizeof(card_root), "%s", path ? path : "");
	// Without the trailing slash, so "<root>/" and "<root>" compare the same.
	size_t n = strlen(card_root);
	while (n > 1 && card_root[n - 1] == '/') {
		card_root[--n] = '\0';
	}
	pthread_mutex_unlock(&lock);
}

void sonixlink_set_thumbs_path(const char *path) {
	pthread_mutex_lock(&lock);
	snprintf(thumbs_path, sizeof(thumbs_path), "%s", path ? path : "");
	pthread_mutex_unlock(&lock);
}

bool sonixlink_get_enabled(void) {
	pthread_mutex_lock(&lock);
	bool on = enabled;
	pthread_mutex_unlock(&lock);
	return on;
}

void sonixlink_set_enabled(bool on) {
	pthread_mutex_lock(&lock);
	enabled = on;
	pthread_mutex_unlock(&lock);
	config_set_int("wireless", "sonixlink", on ? 1 : 0);
	config_save();
}

// ---------------------------------------------------------------------------
// The device's own details
// ---------------------------------------------------------------------------

static void read_device_name(char *out, size_t out_size) {
	FILE *f = fopen(NAME_PATH, "r");
	if (f) {
		if (fgets(out, (int)out_size, f)) {
			out[strcspn(out, "\r\n")] = '\0';
			fclose(f);
			if (out[0]) {
				return;
			}
		} else {
			fclose(f);
		}
	}
	snprintf(out, out_size, "%s", DEFAULT_NAME);
}

// The address to announce. The Wi-Fi module's own answer first; the interface
// list is the fallback, which is also what makes this work on a desktop where
// there is no wlan0 to ask about.
static bool local_address(char *out, size_t out_size) {
	wifi_status_t status;
	wifi_get_status(&status);
	if (status.ip[0]) {
		snprintf(out, out_size, "%s", status.ip);
		return true;
	}

	struct ifaddrs *list = NULL;
	if (getifaddrs(&list) != 0) {
		return false;
	}
	bool found = false;
	for (struct ifaddrs *it = list; it && !found; it = it->ifa_next) {
		if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) {
			continue;
		}
		if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)) {
			continue;
		}
		struct sockaddr_in *in = (struct sockaddr_in *)it->ifa_addr;
		if (inet_ntop(AF_INET, &in->sin_addr, out, (socklen_t)out_size)) {
			found = true;
		}
	}
	freeifaddrs(list);
	return found;
}

// ---------------------------------------------------------------------------
// A growable byte buffer, for building a reply
// ---------------------------------------------------------------------------

typedef struct {
	char *data;
	size_t len;
	size_t cap;
	bool failed;
} buf_t;

static void buf_free(buf_t *b) {
	free(b->data);
	memset(b, 0, sizeof(*b));
}

static bool buf_room(buf_t *b, size_t extra) {
	if (b->failed) {
		return false;
	}
	if (b->len + extra + 1 <= b->cap) {
		return true;
	}
	size_t want = b->cap ? b->cap : 512;
	while (want < b->len + extra + 1) {
		want *= 2;
	}
	char *grown = realloc(b->data, want);
	if (!grown) {
		b->failed = true;
		return false;
	}
	b->data = grown;
	b->cap = want;
	return true;
}

static void buf_add(buf_t *b, const char *text, size_t len) {
	if (!buf_room(b, len)) {
		return;
	}
	memcpy(b->data + b->len, text, len);
	b->len += len;
	b->data[b->len] = '\0';
}

static void buf_str(buf_t *b, const char *text) { buf_add(b, text, strlen(text)); }

static void buf_fmt(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void buf_fmt(buf_t *b, const char *fmt, ...) {
	char line[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n > 0) {
		buf_add(b, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
	}
}

// A JSON string, with the five characters JSON insists on and anything below a
// space written as \u00xx. Bytes above 127 go through untouched: the index
// holds UTF-8 and JSON carries UTF-8.
static void buf_json_string(buf_t *b, const char *text) {
	buf_str(b, "\"");
	for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
		switch (*p) {
		case '"':
			buf_str(b, "\\\"");
			break;
		case '\\':
			buf_str(b, "\\\\");
			break;
		case '\n':
			buf_str(b, "\\n");
			break;
		case '\r':
			buf_str(b, "\\r");
			break;
		case '\t':
			buf_str(b, "\\t");
			break;
		default:
			if (*p < 0x20) {
				buf_fmt(b, "\\u%04x", *p);
			} else {
				buf_add(b, (const char *)p, 1);
			}
			break;
		}
	}
	buf_str(b, "\"");
}

static void buf_json_field(buf_t *b, const char *name, const char *value, bool comma) {
	buf_fmt(b, "\"%s\":", name);
	buf_json_string(b, value);
	if (comma) {
		buf_str(b, ",");
	}
}

// ---------------------------------------------------------------------------
// One client
//
// A request is short and a reply is usually short; the index file is the
// exception and is sent straight from disk in pieces, so a library of any size
// costs one buffer of a few kilobytes rather than its own size in memory.
// ---------------------------------------------------------------------------

typedef struct {
	int fd;
	char request[REQUEST_MAX];
	size_t request_len;

	buf_t out;	 // headers, and the body when it is not a file
	size_t sent; // how much of `out` has gone

	FILE *file;			// the index file, while one is being sent
	long file_left;		// how much of it is left
	uint32_t opened_ms; // when this client arrived (or last asked), so a stalled one can go

	// A Bluetooth link: one connection carries every request, so a finished
	// reply readies it for the next one instead of closing it.
	bool persistent;

	// A request with a body (a selection of tracks): how long the body is,
	// and as much of it as has arrived.
	size_t body_want;
	buf_t body;
} client_t;

static client_t clients[MAX_CLIENTS];

static const char *connection_header(const client_t *c) {
	return c->persistent ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
}

// A reply has gone out whole: a persistent link waits for its next request, any
// other connection is closed.
static void client_done(client_t *c);

static void client_close(client_t *c) {
	if (c->fd >= 0) {
		close(c->fd);
	}
	if (c->file) {
		fclose(c->file);
	}
	buf_free(&c->out);
	buf_free(&c->body);
	memset(c, 0, sizeof(*c));
	c->fd = -1;
}

static void client_done(client_t *c) {
	if (!c->persistent) {
		client_close(c);
		return;
	}
	if (c->file) {
		fclose(c->file);
		c->file = NULL;
	}
	c->file_left = 0;
	buf_free(&c->out);
	buf_free(&c->body);
	c->body_want = 0;
	c->sent = 0;
	c->request_len = 0;
	c->request[0] = '\0';
	c->opened_ms = now_ms();
}

// ---------------------------------------------------------------------------
// Replies
// ---------------------------------------------------------------------------

static void reply_raw(client_t *c, const char *status, const char *type, const char *body, size_t body_len) {
	buf_fmt(&c->out, "HTTP/1.1 %s\r\n", status);
	buf_fmt(&c->out, "Content-Type: %s\r\n", type);
	buf_fmt(&c->out, "Content-Length: %zu\r\n", body_len);
	// The phone is on the same network and nothing here is a secret, but a
	// browser opening this from a page still has to be told it may.
	buf_str(&c->out, "Access-Control-Allow-Origin: *\r\n");
	buf_str(&c->out, connection_header(c));
	if (body && body_len) {
		buf_add(&c->out, body, body_len);
	}
}

static void reply_json(client_t *c, buf_t *json) {
	buf_t head;
	memset(&head, 0, sizeof(head));
	buf_fmt(&head, "HTTP/1.1 200 OK\r\n");
	buf_str(&head, "Content-Type: application/json; charset=utf-8\r\n");
	buf_fmt(&head, "Content-Length: %zu\r\n", json->len);
	buf_str(&head, "Access-Control-Allow-Origin: *\r\n");
	buf_str(&head, connection_header(c));
	buf_add(&head, json->data ? json->data : "", json->len);

	buf_free(&c->out);
	c->out = head;
	buf_free(json);
}

static void reply_status(client_t *c, const char *status, const char *message) {
	reply_raw(c, status, "text/plain; charset=utf-8", message, strlen(message));
}

// ---------------------------------------------------------------------------
// The routes
// ---------------------------------------------------------------------------

static const char *mode_name(int mode) {
	switch (mode) {
	case SONIXLINK_MODE_REPEAT_ALL:
		return "repeat_all";
	case SONIXLINK_MODE_REPEAT_ONE:
		return "repeat_one";
	case SONIXLINK_MODE_SHUFFLE:
		return "shuffle";
	case SONIXLINK_MODE_SHUFFLE_REPEAT:
		return "shuffle_repeat";
	case SONIXLINK_MODE_NORMAL:
	default:
		return "normal";
	}
}

static int mode_from_name(const char *name) {
	if (!name) {
		return -1;
	}
	if (strcmp(name, "repeat_all") == 0) {
		return SONIXLINK_MODE_REPEAT_ALL;
	}
	if (strcmp(name, "repeat_one") == 0) {
		return SONIXLINK_MODE_REPEAT_ONE;
	}
	if (strcmp(name, "shuffle") == 0) {
		return SONIXLINK_MODE_SHUFFLE;
	}
	if (strcmp(name, "shuffle_repeat") == 0) {
		return SONIXLINK_MODE_SHUFFLE_REPEAT;
	}
	if (strcmp(name, "normal") == 0 || strcmp(name, "off") == 0) {
		return SONIXLINK_MODE_NORMAL;
	}
	return -1;
}

static void state_copy(sonixlink_state_t *out) {
	pthread_mutex_lock(&lock);
	*out = g_state;
	pthread_mutex_unlock(&lock);
}

// The size and mtime of the index, so the phone can tell whether the copy it
// already has is still the current one.
static bool db_stat(long *size_out, long *mtime_out) {
	char path[sizeof(db_path)];
	pthread_mutex_lock(&lock);
	snprintf(path, sizeof(path), "%s", db_path);
	pthread_mutex_unlock(&lock);

	struct stat st;
	if (!path[0] || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		return false;
	}
	*size_out = (long)st.st_size;
	*mtime_out = (long)st.st_mtime;
	return true;
}

// ---------------------------------------------------------------------------
// Content fingerprints
//
// The phone keeps its copy of the index and of the thumbnails between one
// connection and the next, and has to tell whether they are still current. The
// files' mtimes cannot say: the index is also where the player keeps where
// playback was left, the saved queue and the playlist counts, so it is written
// on every change of track, and the thumbnails file every time a list draws a
// cover it had not drawn before. Asking by mtime meant a full download at
// nearly every connection.
//
// So what is compared is a hash of what the phone actually reads: the library
// tables and the playlists for the index (the favourites travel live, through
// /api/favourites), the thumbnail keys for the thumbnails (a key names the
// track, its mtime, its size and the box, so the pixels behind a key never
// change without the key changing). Each row is hashed on its own and the row
// hashes are summed, so the order the rows sit in -- which a rescan reshuffles
// -- does not count.
//
// Computing it reads the whole table, about half a second for a large library
// on this processor, so the result is kept against the file's size and mtime
// and computed again only when those move.
// ---------------------------------------------------------------------------

#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

static uint64_t fnv_bytes(uint64_t h, const void *data, size_t len) {
	const uint8_t *p = data;
	for (size_t i = 0; i < len; i++) {
		h ^= p[i];
		h *= FNV_PRIME;
	}
	return h;
}

// Every row of one query folded into the running sum. False when the query
// could not run -- a table that does not exist on this card, for one.
static bool hash_rows(sqlite3 *db, const char *sql, uint64_t *sum, uint64_t *rows) {
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
		sqlite3_finalize(st);
		return false;
	}
	int columns = sqlite3_column_count(st);
	int rc;
	while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
		uint64_t h = FNV_OFFSET;
		for (int i = 0; i < columns; i++) {
			int type = sqlite3_column_type(st, i);
			uint8_t tag = (uint8_t)type;
			h = fnv_bytes(h, &tag, 1);
			if (type == SQLITE_INTEGER) {
				int64_t v = sqlite3_column_int64(st, i);
				h = fnv_bytes(h, &v, sizeof(v));
			} else if (type == SQLITE_FLOAT) {
				double v = sqlite3_column_double(st, i);
				h = fnv_bytes(h, &v, sizeof(v));
			} else if (type == SQLITE_TEXT || type == SQLITE_BLOB) {
				const void *v = sqlite3_column_blob(st, i);
				int n = sqlite3_column_bytes(st, i);
				if (v && n > 0) {
					h = fnv_bytes(h, v, (size_t)n);
				}
			}
		}
		*sum += h;
		(*rows)++;
	}
	sqlite3_finalize(st);
	return rc == SQLITE_DONE;
}

static bool hash_index(const char *path, uint64_t *out) {
	sqlite3 *db = NULL;
	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		sqlite3_close(db);
		return false;
	}
	sqlite3_busy_timeout(db, 1000);

	static const char *const TABLES[] = {
		"MEDIA_TABLE", "ALBUM_TABLE", "ALBUM_GROUP_TABLE", "ARTIST_TABLE",
		"ALBUM_ARTIST_TABLE", "GENRE_TABLE", "ARTIST_LINK", "ALBUM_ARTIST_LINK", "GENRE_LINK",
	};
	uint64_t total = FNV_OFFSET;
	bool ok = true;
	for (size_t t = 0; t < sizeof(TABLES) / sizeof(TABLES[0]) && ok; t++) {
		char sql[96];
		snprintf(sql, sizeof(sql), "SELECT * FROM %s", TABLES[t]);
		uint64_t sum = 0, rows = 0;
		// A table missing from an older card is not an error: it is empty.
		hash_rows(db, sql, &sum, &rows);
		total = fnv_bytes(total, &sum, sizeof(sum));
		total = fnv_bytes(total, &rows, sizeof(rows));
	}

	// The playlists: one table each, found by name. The name goes into the
	// hash too, so renaming a playlist counts.
	sqlite3_stmt *names = NULL;
	if (sqlite3_prepare_v2(db,
						   "SELECT name FROM sqlite_master WHERE type='table' AND substr(name,1,4)='M3U_' ORDER BY name",
						   -1, &names, NULL) == SQLITE_OK) {
		while (sqlite3_step(names) == SQLITE_ROW) {
			const char *name = (const char *)sqlite3_column_text(names, 0);
			if (!name || strchr(name, '"')) {
				continue;
			}
			char sql[400];
			snprintf(sql, sizeof(sql), "SELECT * FROM \"%.300s\"", name);
			uint64_t sum = 0, rows = 0;
			hash_rows(db, sql, &sum, &rows);
			total = fnv_bytes(total, name, strlen(name));
			total = fnv_bytes(total, &sum, sizeof(sum));
			total = fnv_bytes(total, &rows, sizeof(rows));
		}
	} else {
		ok = false;
	}
	sqlite3_finalize(names);
	sqlite3_close(db);
	*out = total;
	return ok;
}

static bool hash_thumbs(const char *path, uint64_t *out) {
	sqlite3 *db = NULL;
	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		sqlite3_close(db);
		return false;
	}
	sqlite3_busy_timeout(db, 1000);
	uint64_t sum = 0, rows = 0;
	bool ok = hash_rows(db, "SELECT key FROM thumbs", &sum, &rows);
	sqlite3_close(db);
	uint64_t total = FNV_OFFSET;
	total = fnv_bytes(total, &sum, sizeof(sum));
	total = fnv_bytes(total, &rows, sizeof(rows));
	*out = total;
	return ok;
}

typedef struct {
	char path[600];
	long size;
	long mtime;
	char text[20];
} fingerprint_t;

// Worker thread only: the cache needs no lock.
static fingerprint_t index_print, thumbs_print;

// The fingerprint of a file, as 16 hex digits, or "" when it cannot be had.
static const char *fingerprint(fingerprint_t *cache, const char *path, long size, long mtime,
							   bool (*hash)(const char *, uint64_t *)) {
	if (strcmp(cache->path, path) == 0 && cache->size == size && cache->mtime == mtime && cache->text[0]) {
		return cache->text;
	}
	uint64_t h = 0;
	uint32_t began = now_ms();
	if (!hash(path, &h)) {
		cache->text[0] = '\0';
		return cache->text;
	}
	snprintf(cache->path, sizeof(cache->path), "%s", path);
	cache->size = size;
	cache->mtime = mtime;
	snprintf(cache->text, sizeof(cache->text), "%016llx", (unsigned long long)h);
	if (verbose) {
		printf("sonixlink: fingerprint of %s is %s (%u ms)\n", path, cache->text, now_ms() - began);
	}
	return cache->text;
}

// How the player orders its lists, as a JSON field (with its trailing comma).
static void buf_sort_field(buf_t *j, const sonixlink_state_t *s) {
	buf_fmt(j, "\"sort\":{\"desc\":%u,\"added\":%u,\"artist_by_album\":%s,\"favourites_reversed\":%s},",
			s->sort_desc, s->sort_added, s->artist_by_album ? "true" : "false",
			s->favourites_reversed ? "true" : "false");
}

static void route_info(client_t *c) {
	sonixlink_state_t s;
	state_copy(&s);

	long size = 0, mtime = 0;
	bool have_db = db_stat(&size, &mtime);

	buf_t j;
	memset(&j, 0, sizeof(j));
	buf_str(&j, "{");
	buf_json_field(&j, "name", device_name_text, true);
	buf_json_field(&j, "model", "HiBy R3 Pro II", true);
	buf_json_field(&j, "firmware", sysinfo_os_version(), true);
	buf_json_field(&j, "serial", sysinfo_serial_number(), true);
	buf_fmt(&j, "\"api\":1,\"port\":%d,", SONIXLINK_PORT);
	buf_fmt(&j, "\"tracks\":%u,", s.track_count);
	buf_fmt(&j, "\"scanning\":%s,", s.scanning ? "true" : "false");
	buf_sort_field(&j, &s);
	buf_fmt(&j, "\"db_available\":%s,", have_db ? "true" : "false");
	buf_fmt(&j, "\"db_size\":%ld,\"db_mtime\":%ld,", size, mtime);
	if (have_db && !s.scanning) {
		char path[sizeof(db_path)];
		pthread_mutex_lock(&lock);
		snprintf(path, sizeof(path), "%s", db_path);
		pthread_mutex_unlock(&lock);
		buf_json_field(&j, "db_hash", fingerprint(&index_print, path, size, mtime, hash_index), true);
	}

	char thumbs[sizeof(thumbs_path)];
	pthread_mutex_lock(&lock);
	snprintf(thumbs, sizeof(thumbs), "%s", thumbs_path);
	pthread_mutex_unlock(&lock);

	long covers_size = 0, covers_mtime = 0;
	struct stat cst;
	bool have_covers = thumbs[0] && stat(thumbs, &cst) == 0 && S_ISREG(cst.st_mode) && cst.st_size > 0;
	if (have_covers) {
		covers_size = (long)cst.st_size;
		covers_mtime = (long)cst.st_mtime;
	}
	if (have_covers) {
		buf_json_field(&j, "covers_hash", fingerprint(&thumbs_print, thumbs, covers_size, covers_mtime, hash_thumbs),
					   true);
	}
	buf_json_field(&j, "accent", s.accent[0] ? s.accent : "#3584e4", true);
	buf_fmt(&j, "\"covers_available\":%s,", have_covers ? "true" : "false");
	buf_fmt(&j, "\"covers_size\":%ld,\"covers_mtime\":%ld}", covers_size, covers_mtime);
	reply_json(c, &j);
}

static void route_state(client_t *c) {
	sonixlink_state_t s;
	state_copy(&s);

	buf_t j;
	memset(&j, 0, sizeof(j));
	buf_str(&j, "{");
	buf_fmt(&j, "\"state\":%d,", s.play_state);
	buf_json_field(&j, "mode", mode_name(s.play_mode), true);
	buf_fmt(&j, "\"volume\":%d,", s.volume);
	buf_fmt(&j, "\"position\":%u,\"duration\":%u,", s.progress_secs, s.duration_secs);
	buf_json_field(&j, "title", s.title, true);
	buf_json_field(&j, "artist", s.artist, true);
	buf_json_field(&j, "album", s.album, true);
	buf_json_field(&j, "album_artist", s.album_artist, true);
	buf_json_field(&j, "path", s.path, true);
	buf_fmt(&j, "\"sample_rate\":%u,\"bitrate\":%u,\"bits\":%u,", s.sample_rate, s.bitrate, s.bits);
	buf_fmt(&j, "\"lossless\":%s,", s.lossless ? "true" : "false");
	buf_fmt(&j, "\"battery\":%d,\"charging\":%s,", s.battery_percent, s.charging ? "true" : "false");
	buf_fmt(&j, "\"favourite\":%s,", s.favourite ? "true" : "false");
	buf_json_field(&j, "accent", s.accent[0] ? s.accent : "#3584e4", true);
	buf_sort_field(&j, &s);
	buf_fmt(&j, "\"queue_position\":%d,\"queue_count\":%d,\"queue_revision\":%u,", s.queue_position, s.queue_count,
			s.queue_revision);
	buf_fmt(&j, "\"display_position\":%d,\"display_count\":%d,", s.display_position, s.display_count);
	buf_fmt(&j, "\"favourites_revision\":%u,\"playlists_revision\":%u,", s.favourites_revision,
			s.playlists_revision);
	buf_fmt(&j, "\"scanning\":%s,\"scan_count\":%u,\"tracks\":%u}", s.scanning ? "true" : "false", s.scan_count,
			s.track_count);
	reply_json(c, &j);
}

// The index file, sent from disk. Refused while a scan is running: the file is
// being rewritten, and half a database is worse than none.
static void route_db(client_t *c) {
	sonixlink_state_t s;
	state_copy(&s);
	if (s.scanning) {
		reply_status(c, "503 Service Unavailable", "scanning");
		return;
	}

	long size = 0, mtime = 0;
	if (!db_stat(&size, &mtime)) {
		reply_status(c, "404 Not Found", "no database");
		return;
	}

	char path[sizeof(db_path)];
	pthread_mutex_lock(&lock);
	snprintf(path, sizeof(path), "%s", db_path);
	pthread_mutex_unlock(&lock);

	FILE *f = fopen(path, "rb");
	if (!f) {
		reply_status(c, "500 Internal Server Error", "cannot open the database");
		return;
	}

	buf_fmt(&c->out, "HTTP/1.1 200 OK\r\n");
	buf_str(&c->out, "Content-Type: application/vnd.sqlite3\r\n");
	buf_fmt(&c->out, "Content-Length: %ld\r\n", size);
	buf_fmt(&c->out, "X-Sonix-Db-Mtime: %ld\r\n", mtime);
	buf_str(&c->out, "Access-Control-Allow-Origin: *\r\n");
	buf_str(&c->out, connection_header(c));

	c->file = f;
	c->file_left = size;
	printf("sonixlink: sending the database, %ld bytes\n", size);
}

// One percent-escape, or -1 when the two characters after the % are not hex.
static int hex_pair(const char *p) {
	int hi = -1, lo = -1;
	for (int i = 0; i < 2; i++) {
		char ch = p[i];
		int v = -1;
		if (ch >= '0' && ch <= '9') {
			v = ch - '0';
		} else if (ch >= 'a' && ch <= 'f') {
			v = ch - 'a' + 10;
		} else if (ch >= 'A' && ch <= 'F') {
			v = ch - 'A' + 10;
		} else {
			return -1;
		}
		if (i == 0) {
			hi = v;
		} else {
			lo = v;
		}
	}
	return (hi << 4) | lo;
}

// Copies one query parameter's value out of `query`, undoing the escaping. A
// path arrives this way, so it has to survive spaces, accents and '&'.
static bool query_value(const char *query, const char *key, char *out, size_t out_size) {
	size_t key_len = strlen(key);
	const char *at = query;

	while (at && *at) {
		const char *end = strchr(at, '&');
		size_t pair_len = end ? (size_t)(end - at) : strlen(at);

		if (pair_len > key_len && at[key_len] == '=' && strncmp(at, key, key_len) == 0) {
			const char *value = at + key_len + 1;
			size_t value_len = pair_len - key_len - 1;
			size_t n = 0;
			for (size_t i = 0; i < value_len && n + 1 < out_size; i++) {
				if (value[i] == '%' && i + 2 < value_len + 1 && i + 2 <= value_len) {
					int byte = hex_pair(value + i + 1);
					if (byte >= 0) {
						out[n++] = (char)byte;
						i += 2;
						continue;
					}
				}
				out[n++] = value[i] == '+' ? ' ' : value[i];
			}
			out[n] = '\0';
			return true;
		}
		at = end ? end + 1 : NULL;
	}
	out[0] = '\0';
	return false;
}

static int list_from_name(const char *name) {
	if (!name || !name[0] || strcmp(name, "all") == 0) {
		return SONIXLINK_LIST_ALL;
	}
	if (strcmp(name, "album") == 0) {
		return SONIXLINK_LIST_ALBUM;
	}
	if (strcmp(name, "artist") == 0) {
		return SONIXLINK_LIST_ARTIST;
	}
	if (strcmp(name, "album_artist") == 0) {
		return SONIXLINK_LIST_ALBUM_ARTIST;
	}
	if (strcmp(name, "genre") == 0) {
		return SONIXLINK_LIST_GENRE;
	}
	if (strcmp(name, "favourites") == 0) {
		return SONIXLINK_LIST_FAVOURITES;
	}
	if (strcmp(name, "playlist") == 0) {
		return SONIXLINK_LIST_PLAYLIST;
	}
	if (strcmp(name, "queue") == 0) {
		return SONIXLINK_LIST_QUEUE;
	}
	if (strcmp(name, "albums") == 0) {
		return SONIXLINK_LIST_ALBUMS;
	}
	if (strcmp(name, "folder") == 0) {
		return SONIXLINK_LIST_FOLDER;
	}
	return -1;
}

// The paths in a request's body, one a line, copied into one block of
// NUL-ended strings (the shape sonixlink_command_t carries). Returns how many.
#define BODY_PATHS_MAX 5000

static int body_paths(client_t *c, char **out) {
	*out = NULL;
	if (!c->body.data || c->body.len == 0) {
		return 0;
	}
	char *block = malloc(c->body.len + 1);
	if (!block) {
		return 0;
	}
	int count = 0;
	size_t at = 0;
	const char *line = c->body.data;
	const char *end = c->body.data + c->body.len;
	while (line < end && count < BODY_PATHS_MAX) {
		const char *eol = memchr(line, '\n', (size_t)(end - line));
		size_t len = eol ? (size_t)(eol - line) : (size_t)(end - line);
		if (len > 0 && line[len - 1] == '\r') {
			len--;
		}
		if (len > 0 && len < SONIXLINK_PATH_MAX) {
			memcpy(block + at, line, len);
			at += len;
			block[at++] = '\0';
			count++;
		}
		line = eol ? eol + 1 : end;
	}
	if (count == 0) {
		free(block);
		return 0;
	}
	*out = block;
	return count;
}

static void route_command(client_t *c, const char *query) {
	char what[32] = "";
	char value[SONIXLINK_PATH_MAX] = "";
	query_value(query, "do", what, sizeof(what));
	query_value(query, "value", value, sizeof(value));

	int number = value[0] ? atoi(value) : 0;
	bool known = true;

	if (strcmp(what, "play") == 0) {
		push_command(SONIXLINK_CMD_PLAY, 0);
	} else if (strcmp(what, "pause") == 0) {
		push_command(SONIXLINK_CMD_PAUSE, 0);
	} else if (strcmp(what, "toggle") == 0) {
		push_command(SONIXLINK_CMD_TOGGLE, 0);
	} else if (strcmp(what, "stop") == 0) {
		push_command(SONIXLINK_CMD_STOP, 0);
	} else if (strcmp(what, "next") == 0) {
		push_command(SONIXLINK_CMD_NEXT, 0);
	} else if (strcmp(what, "prev") == 0) {
		push_command(SONIXLINK_CMD_PREV, 0);
	} else if (strcmp(what, "seek") == 0) {
		push_command(SONIXLINK_CMD_SEEK, number);
	} else if (strcmp(what, "volume") == 0) {
		push_command(SONIXLINK_CMD_VOLUME, number < 0 ? 0 : (number > 100 ? 100 : number));
	} else if (strcmp(what, "mode") == 0) {
		int mode = mode_from_name(value);
		if (mode < 0) {
			reply_status(c, "400 Bad Request", "unknown mode");
			return;
		}
		push_command(SONIXLINK_CMD_MODE, mode);
	} else if (strcmp(what, "play_path") == 0) {
		char path[SONIXLINK_PATH_MAX] = "";
		query_value(query, "path", path, sizeof(path));
		if (!path[0]) {
			reply_status(c, "400 Bad Request", "no path");
			return;
		}
		// Where the track was picked from. Without it every track played from
		// the app would put the whole library in the queue.
		char list_name[32] = "";
		char list_value[SONIXLINK_TEXT_MAX] = "";
		query_value(query, "list", list_name, sizeof(list_name));
		query_value(query, "value", list_value, sizeof(list_value));
		int list = list_from_name(list_name);
		if (list < 0) {
			reply_status(c, "400 Bad Request", "unknown list");
			return;
		}
		push_command_list(SONIXLINK_CMD_PLAY_PATH, 0, path, list, list_value);
	} else if (strcmp(what, "favourite") == 0) {
		char path[SONIXLINK_PATH_MAX] = "";
		query_value(query, "path", path, sizeof(path));
		if (!path[0]) {
			reply_status(c, "400 Bad Request", "no path");
			return;
		}
		// Nothing said means toggle, which is what a star being tapped means.
		int want = -1;
		if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0) {
			want = 1;
		} else if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0) {
			want = 0;
		}
		push_command_path(SONIXLINK_CMD_FAVOURITE, want, path);
	} else if (strcmp(what, "queue_index") == 0) {
		push_command(SONIXLINK_CMD_QUEUE_INDEX, number);
	} else if (strcmp(what, "play_all") == 0) {
		char list_name[32] = "";
		char how[16] = "";
		query_value(query, "list", list_name, sizeof(list_name));
		query_value(query, "how", how, sizeof(how));
		int list = list_from_name(list_name);
		if (list < 0 || list == SONIXLINK_LIST_QUEUE || list == SONIXLINK_LIST_FOLDER) {
			reply_status(c, "400 Bad Request", "unknown list");
			return;
		}
		int arg = strcmp(how, "shuffle") == 0  ? SONIXLINK_PLAY_SHUFFLE
				  : strcmp(how, "random") == 0 ? SONIXLINK_PLAY_RANDOM_TO_QUEUE
											   : SONIXLINK_PLAY_SEQUENCE;
		push_command_list(SONIXLINK_CMD_PLAY_ALL, arg, NULL, list, value);
	} else if (strcmp(what, "queue_next") == 0 || strcmp(what, "favourites_add") == 0 ||
			   strcmp(what, "favourites_remove") == 0 || strcmp(what, "playlist_add") == 0 ||
			   strcmp(what, "playlist_remove") == 0) {
		bool to_playlist = strncmp(what, "playlist_", 9) == 0;
		if (to_playlist && !value[0]) {
			reply_status(c, "400 Bad Request", "no playlist");
			return;
		}
		// The command keeps SONIXLINK_TEXT_MAX bytes of it: a longer name
		// would reach the playlist code cut, as a different playlist.
		if (to_playlist && strlen(value) >= SONIXLINK_TEXT_MAX) {
			reply_status(c, "400 Bad Request", "playlist name too long");
			return;
		}
		char *paths = NULL;
		int count = body_paths(c, &paths);
		if (count == 0) {
			reply_status(c, "400 Bad Request", "no paths");
			return;
		}
		sonixlink_command_kind_t kind = strcmp(what, "queue_next") == 0		  ? SONIXLINK_CMD_QUEUE_NEXT
										: strcmp(what, "favourites_add") == 0	  ? SONIXLINK_CMD_FAVOURITES_ADD
										: strcmp(what, "favourites_remove") == 0 ? SONIXLINK_CMD_FAVOURITES_REMOVE
										: strcmp(what, "playlist_add") == 0	  ? SONIXLINK_CMD_PLAYLIST_ADD
																				  : SONIXLINK_CMD_PLAYLIST_REMOVE;
		push_command_paths(kind, 0, paths, count, value);
	} else if (strcmp(what, "scan") == 0) {
		push_command(SONIXLINK_CMD_SCAN, 0);
	} else {
		known = false;
	}

	if (!known) {
		reply_status(c, "400 Bad Request", "unknown command");
		return;
	}
	if (verbose) {
		printf("sonixlink: command %s %s\n", what, value);
	}
	reply_status(c, "200 OK", "ok");
}

// The artwork the scan wrote down beside a track, when there is one. Nothing is
// decoded here: it is a file on the card, sent as it is.
static void route_cover(client_t *c, const char *query) {
	char path[SONIXLINK_PATH_MAX] = "";
	if (!query_value(query, "path", path, sizeof(path)) || !path[0]) {
		reply_status(c, "400 Bad Request", "no path");
		return;
	}

	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 8 * 1024 * 1024) {
		reply_status(c, "404 Not Found", "no cover");
		return;
	}
	FILE *f = fopen(path, "rb");
	if (!f) {
		reply_status(c, "404 Not Found", "no cover");
		return;
	}

	const char *type = "image/jpeg";
	size_t len = strlen(path);
	if (len > 4 && strcasecmp(path + len - 4, ".png") == 0) {
		type = "image/png";
	}

	buf_fmt(&c->out, "HTTP/1.1 200 OK\r\n");
	buf_fmt(&c->out, "Content-Type: %s\r\n", type);
	buf_fmt(&c->out, "Content-Length: %ld\r\n", (long)st.st_size);
	buf_str(&c->out, "Access-Control-Allow-Origin: *\r\n");
	buf_str(&c->out, connection_header(c));
	c->file = f;
	c->file_left = (long)st.st_size;
}


// The queue as the interface last handed it over, with the paths only. The
// phone already has the index, so it looks up title and artist there instead of
// being sent them again.
//
// Without `from` the answer is the window as it stands, around the track
// playing now. With it, the phone wants the stretch starting there: the
// interface is asked for it, and until its next pump has published it the
// answer says "pending" with no paths, and the phone asks again.
static void route_queue(client_t *c, const char *query) {
	char from_text[16] = "";
	int from = -1;
	if (query_value(query, "from", from_text, sizeof(from_text)) && from_text[0]) {
		from = atoi(from_text);
		if (from < 0) {
			from = 0;
		}
	}

	buf_t j;
	memset(&j, 0, sizeof(j));

	pthread_mutex_lock(&lock);
	int total = queue_total;
	int position = queue_position;
	int first = queue_window_first;
	int count = queue_window_count;
	unsigned revision = queue_revision;
	bool pending = false;
	if (from >= 0) {
		queue_wanted = from;
		pending = from < total && first != from;
	}

	buf_str(&j, "{");
	buf_fmt(&j, "\"count\":%d,\"position\":%d,\"revision\":%u,\"pending\":%s,", total, position, revision,
			pending ? "true" : "false");
	if (pending) {
		count = 0;
		first = from;
	}
	buf_fmt(&j, "\"first\":%d,\"paths\":[", first);
	for (int i = 0; i < count; i++) {
		size_t at = (size_t)queue_offsets[i];
		const char *path = (queue_arena && at < queue_arena_len) ? queue_arena + at : "";
		if (i) {
			buf_str(&j, ",");
		}
		buf_json_string(&j, path);
	}
	buf_str(&j, "]}");
	pthread_mutex_unlock(&lock);

	reply_json(c, &j);
}

// The starred tracks, read straight from the index with a connection of this
// thread's own. It is a small table, and reading it here rather than through
// the interface means the phone sees a star it has just set without waiting for
// the next index download.
static void route_favourites(client_t *c) {
	char path[sizeof(db_path)];
	pthread_mutex_lock(&lock);
	snprintf(path, sizeof(path), "%s", db_path);
	pthread_mutex_unlock(&lock);

	buf_t j;
	memset(&j, 0, sizeof(j));
	buf_str(&j, "{\"favourites\":[");

	sqlite3 *db = NULL;
	if (path[0] && sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
		sqlite3_stmt *st = NULL;
		if (sqlite3_prepare_v2(db, "SELECT path,name,artist FROM FAVOURITES ORDER BY added_at DESC", -1, &st, NULL) ==
			SQLITE_OK) {
			bool first = true;
			while (sqlite3_step(st) == SQLITE_ROW) {
				const char *p = (const char *)sqlite3_column_text(st, 0);
				if (!p || !p[0]) {
					continue;
				}
				const char *name = (const char *)sqlite3_column_text(st, 1);
				const char *artist = (const char *)sqlite3_column_text(st, 2);
				buf_str(&j, first ? "{" : ",{");
				first = false;
				buf_json_field(&j, "path", p, true);
				buf_json_field(&j, "name", name ? name : "", true);
				buf_json_field(&j, "artist", artist ? artist : "", false);
				buf_str(&j, "}");
			}
		}
		sqlite3_finalize(st);
	}
	sqlite3_close(db);

	buf_str(&j, "]}");
	reply_json(c, &j);
}

// The index's path, or false when there is none yet.
static bool index_path(char *out, size_t out_size) {
	pthread_mutex_lock(&lock);
	snprintf(out, out_size, "%s", db_path);
	pthread_mutex_unlock(&lock);
	return out[0] != '\0';
}

// The playlists, read live like the favourites: the copy of the index on the
// phone is only as fresh as its last download, and a playlist made a moment ago
// on the player or from the phone belongs in the list now. Names with their
// track counts; the order is the app's to apply.
static void route_playlists(client_t *c) {
	char path[sizeof(db_path)];
	sonixlink_state_t s;
	state_copy(&s);

	buf_t j;
	memset(&j, 0, sizeof(j));
	buf_fmt(&j, "{\"revision\":%u,\"playlists\":[", s.playlists_revision);

	sqlite3 *db = NULL;
	if (index_path(path, sizeof(path)) && sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
		sqlite3_busy_timeout(db, 1000);
		sqlite3_stmt *names = NULL;
		if (sqlite3_prepare_v2(db,
							   "SELECT name FROM sqlite_master WHERE type='table'"
							   " AND name LIKE 'M3U\\_%' ESCAPE '\\' ORDER BY name",
							   -1, &names, NULL) == SQLITE_OK) {
			bool first = true;
			while (sqlite3_step(names) == SQLITE_ROW) {
				const char *table = (const char *)sqlite3_column_text(names, 0);
				if (!table || strlen(table) <= 4 || strchr(table, '"')) {
					continue;
				}
				char sql[400];
				snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM \"%.300s\" WHERE present<>0", table);
				int count = 0;
				sqlite3_stmt *st = NULL;
				if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
					count = sqlite3_column_int(st, 0);
				}
				sqlite3_finalize(st);
				buf_str(&j, first ? "{" : ",{");
				first = false;
				buf_json_field(&j, "name", table + 4, true);
				buf_fmt(&j, "\"count\":%d}", count);
			}
		}
		sqlite3_finalize(names);
	}
	sqlite3_close(db);

	buf_str(&j, "]}");
	reply_json(c, &j);
}

// One playlist's tracks in the order it was built, as the player's playlist
// page lists them (only those on the card): path, title and artist, which is
// all a row needs; the rest the phone finds in its index by path.
static void route_playlist(client_t *c, const char *query) {
	char name[300];
	char path[sizeof(db_path)];
	if (!query_value(query, "name", name, sizeof(name)) || !name[0] || strchr(name, '"')) {
		reply_status(c, "400 Bad Request", "which playlist?");
		return;
	}

	buf_t j;
	memset(&j, 0, sizeof(j));
	buf_str(&j, "{");
	buf_json_field(&j, "name", name, true);
	buf_str(&j, "\"tracks\":[");

	sqlite3 *db = NULL;
	if (index_path(path, sizeof(path)) && sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
		sqlite3_busy_timeout(db, 1000);
		char sql[420];
		snprintf(sql, sizeof(sql), "SELECT path,title,artist FROM \"M3U_%s\" WHERE present<>0 ORDER BY idx", name);
		sqlite3_stmt *st = NULL;
		if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
			bool first = true;
			while (sqlite3_step(st) == SQLITE_ROW) {
				const char *p = (const char *)sqlite3_column_text(st, 0);
				const char *title = (const char *)sqlite3_column_text(st, 1);
				const char *artist = (const char *)sqlite3_column_text(st, 2);
				if (!p || !p[0]) {
					continue;
				}
				buf_str(&j, first ? "{" : ",{");
				first = false;
				buf_json_field(&j, "path", p, true);
				buf_json_field(&j, "title", title ? title : "", true);
				buf_json_field(&j, "artist", artist ? artist : "", false);
				buf_str(&j, "}");
			}
		}
		sqlite3_finalize(st);
	}
	sqlite3_close(db);

	buf_str(&j, "]}");
	reply_json(c, &j);
}

// The player's own thumbnail cache, sent as it stands.
//
// Not a second database built for the phone: this is the file cover.c already
// fills while the player's lists are drawn, keyed by a hash of the track's path,
// its box size, its mtime and its size -- all of which the phone can work out
// from the index it has. What is inside is already decoded and scaled, so the
// phone neither decodes nor resizes anything.
static void route_covers(client_t *c) {
	char path[sizeof(thumbs_path)];
	pthread_mutex_lock(&lock);
	snprintf(path, sizeof(path), "%s", thumbs_path);
	pthread_mutex_unlock(&lock);

	struct stat st;
	if (!path[0] || stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
		reply_status(c, "404 Not Found", "no thumbnails yet");
		return;
	}
	FILE *f = fopen(path, "rb");
	if (!f) {
		reply_status(c, "500 Internal Server Error", "cannot open the thumbnails");
		return;
	}

	buf_fmt(&c->out, "HTTP/1.1 200 OK\r\n");
	buf_str(&c->out, "Content-Type: application/vnd.sqlite3\r\n");
	buf_fmt(&c->out, "Content-Length: %ld\r\n", (long)st.st_size);
	buf_fmt(&c->out, "X-Sonix-Covers-Mtime: %ld\r\n", (long)st.st_mtime);
	buf_str(&c->out, "Access-Control-Allow-Origin: *\r\n");
	buf_str(&c->out, connection_header(c));
	c->file = f;
	c->file_left = (long)st.st_size;
	printf("sonixlink: sending the thumbnails, %ld bytes\n", (long)st.st_size);
}

// ---------------------------------------------------------------------------
// Thumbnails one at a time
//
// The whole thumbnails file can be a hundred megabytes: every cover the player
// has drawn at every size it draws. Over Bluetooth that is most of an hour. So
// the phone asks for the thumbnails it is about to show, a screenful at a time,
// and keeps what it receives: the pixels behind a key never change (the key
// names the track's path, mtime, size and the box), so nothing is asked twice.
//
//   /api/thumbkeys          every key the file holds, 8 bytes each (the key's
//                           64 bits, little-endian): how the phone knows which
//                           tracks it can ask about without asking for every
//                           one. All of them, those without a picture too: the
//                           keys alone come out of the primary key's index,
//                           while telling which have a picture means reading
//                           every row, which in a file this size is most of it.
//   /api/thumbs?keys=k,k,.. up to THUMBS_PER_ASK keys of 16 hex digits. For
//                           each one the file holds: the 16 digits, width and
//                           height (u16 LE), the pixel bytes' length (u32 LE)
//                           and the RGB565 pixels. Width and height at zero
//                           is "looked, no artwork". A key the file does not
//                           hold is left out.
//
// The connection to the file stays open between requests: a screenful of rows
// scrolled past is several requests a second.
// ---------------------------------------------------------------------------

#define THUMBS_PER_ASK 64

// Worker thread only.
static sqlite3 *thumbs_db;
static char thumbs_db_path[600];

static sqlite3 *thumbs_open(void) {
	char path[sizeof(thumbs_path)];
	pthread_mutex_lock(&lock);
	snprintf(path, sizeof(path), "%s", thumbs_path);
	pthread_mutex_unlock(&lock);
	if (!path[0]) {
		return NULL;
	}
	if (thumbs_db && strcmp(thumbs_db_path, path) == 0) {
		return thumbs_db;
	}
	if (thumbs_db) {
		sqlite3_close(thumbs_db);
		thumbs_db = NULL;
	}
	if (sqlite3_open_v2(path, &thumbs_db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		sqlite3_close(thumbs_db);
		thumbs_db = NULL;
		return NULL;
	}
	// The interface writes this file while lists draw; a read waits its turn.
	sqlite3_busy_timeout(thumbs_db, 1000);
	// Only what one request reads: the pages are not worth keeping.
	sqlite3_exec(thumbs_db, "PRAGMA cache_size=-64", NULL, NULL, NULL);
	snprintf(thumbs_db_path, sizeof(thumbs_db_path), "%s", path);
	return thumbs_db;
}

// A statement failed: the file may have been replaced under the connection.
static void thumbs_forget(void) {
	if (thumbs_db) {
		sqlite3_close(thumbs_db);
		thumbs_db = NULL;
	}
}

static int hex_digit(char ch) {
	if (ch >= '0' && ch <= '9') {
		return ch - '0';
	}
	if (ch >= 'a' && ch <= 'f') {
		return ch - 'a' + 10;
	}
	if (ch >= 'A' && ch <= 'F') {
		return ch - 'A' + 10;
	}
	return -1;
}

static void buf_u16(buf_t *b, unsigned v) {
	char bytes[2] = {(char)(v & 0xff), (char)((v >> 8) & 0xff)};
	buf_add(b, bytes, 2);
}

static void buf_u32(buf_t *b, uint32_t v) {
	char bytes[4] = {(char)(v & 0xff), (char)((v >> 8) & 0xff), (char)((v >> 16) & 0xff), (char)((v >> 24) & 0xff)};
	buf_add(b, bytes, 4);
}

static void route_thumbkeys(client_t *c) {
	sqlite3 *db = thumbs_open();
	if (!db) {
		reply_status(c, "404 Not Found", "no thumbnails yet");
		return;
	}
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(db, "SELECT key FROM thumbs", -1, &st, NULL) != SQLITE_OK) {
		sqlite3_finalize(st);
		thumbs_forget();
		reply_status(c, "500 Internal Server Error", "cannot read the thumbnails");
		return;
	}
	buf_t body;
	memset(&body, 0, sizeof(body));
	while (sqlite3_step(st) == SQLITE_ROW) {
		const char *key = (const char *)sqlite3_column_text(st, 0);
		if (!key || strlen(key) != 16) {
			continue;
		}
		uint64_t v = 0;
		bool ok = true;
		for (int i = 0; i < 16 && ok; i++) {
			int d = hex_digit(key[i]);
			ok = d >= 0;
			v = (v << 4) | (uint64_t)(d < 0 ? 0 : d);
		}
		if (!ok) {
			continue;
		}
		char bytes[8];
		for (int i = 0; i < 8; i++) {
			bytes[i] = (char)((v >> (8 * i)) & 0xff);
		}
		buf_add(&body, bytes, 8);
	}
	sqlite3_finalize(st);
	if (body.failed) {
		buf_free(&body);
		reply_status(c, "500 Internal Server Error", "out of memory");
		return;
	}
	reply_raw(c, "200 OK", "application/octet-stream", body.data, body.len);
	buf_free(&body);
}

static void route_thumbs(client_t *c, const char *query) {
	char list[THUMBS_PER_ASK * 17 + 1] = "";
	if (!query_value(query, "keys", list, sizeof(list)) || !list[0]) {
		reply_status(c, "400 Bad Request", "no keys");
		return;
	}
	sqlite3 *db = thumbs_open();
	if (!db) {
		reply_status(c, "404 Not Found", "no thumbnails yet");
		return;
	}
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(db, "SELECT w,h,pixels FROM thumbs WHERE key=?", -1, &st, NULL) != SQLITE_OK) {
		sqlite3_finalize(st);
		thumbs_forget();
		reply_status(c, "500 Internal Server Error", "cannot read the thumbnails");
		return;
	}

	buf_t body;
	memset(&body, 0, sizeof(body));
	int asked = 0;
	for (char *key = strtok(list, ","); key && asked < THUMBS_PER_ASK; key = strtok(NULL, ",")) {
		if (strlen(key) != 16) {
			continue;
		}
		asked++;
		sqlite3_reset(st);
		sqlite3_bind_text(st, 1, key, 16, SQLITE_TRANSIENT);
		if (sqlite3_step(st) != SQLITE_ROW) {
			continue;
		}
		int w = sqlite3_column_int(st, 0);
		int h = sqlite3_column_int(st, 1);
		const void *pixels = sqlite3_column_blob(st, 2);
		int n = sqlite3_column_bytes(st, 2);
		bool picture = w > 0 && h > 0 && w <= 1024 && h <= 1024 && pixels && n >= w * h * 2;
		buf_add(&body, key, 16);
		buf_u16(&body, picture ? (unsigned)w : 0);
		buf_u16(&body, picture ? (unsigned)h : 0);
		uint32_t len = picture ? (uint32_t)(w * h * 2) : 0;
		buf_u32(&body, len);
		if (len) {
			buf_add(&body, pixels, len);
		}
	}
	sqlite3_finalize(st);
	if (body.failed) {
		buf_free(&body);
		reply_status(c, "500 Internal Server Error", "out of memory");
		return;
	}
	reply_raw(c, "200 OK", "application/octet-stream", body.data ? body.data : "", body.len);
	buf_free(&body);
}

// The artwork of a track, whole and undecoded: the picture inside the tags, or
// the cover file beside it. The thumbnails file answers the lists; this answers
// the one screen that wants the real thing, and the phone decodes it.
//
// With `max=<pixels>` the picture is made to fit that size and sent as a JPEG
// of a few tens of kilobytes (art_shrink.h). Making it is a decode of a cover
// that can be several megabytes, a second or more on this processor, and it is
// not done here: this thread serves every phone and every request, and over
// Bluetooth one request at a time -- a state read waiting a second behind a
// cover is the progress bar standing still at 0:00. So the picture is made on
// a thread of its own (art_worker), and until it is ready the answer is
// "202 Accepted" at once; the phone asks again a moment later, and the link is
// free in between. The thread also starts on its own when the track changes,
// at the size a phone last asked for, so the cover is usually ready before
// anyone asks.
// ---------------------------------------------------------------------------

#define ART_MAX_SIDE_LIMIT 1280
#define ART_JPEG_QUALITY 85
#define ART_KEPT 3

// A phone that asked for a fitted cover this recently gets the next track's
// made ahead of time.
#define ART_PREPARE_FOR_MS (10 * 60 * 1000)

typedef struct {
	char path[SONIXLINK_PATH_MAX];
	int side;
	bool missing;  // the track has no artwork
	uint8_t *data; // what to send: the fitted JPEG, or the original when it is small
	size_t len;
	const char *type;
	uint32_t used_ms;
} art_entry_t;

// Under art_lock.
static pthread_mutex_t art_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t art_wake = PTHREAD_COND_INITIALIZER;
static art_entry_t art_kept[ART_KEPT];
static char art_job_path[SONIXLINK_PATH_MAX];
static int art_job_side;
static bool art_job_queued;
static char art_busy_path[SONIXLINK_PATH_MAX];
static int art_busy_side;
static int art_last_side;
static uint32_t art_last_asked_ms;
static bool art_thread_started;
static pthread_t art_thread;

static const char *art_type_of(const uint8_t *data, size_t size) {
	if (size > 3 && data[0] == 0xFF && data[1] == 0xD8) {
		return "image/jpeg";
	}
	if (size > 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
		return "image/png";
	}
	return "application/octet-stream";
}

// Under art_lock.
static art_entry_t *art_find(const char *path, int side) {
	for (int i = 0; i < ART_KEPT; i++) {
		if ((art_kept[i].data || art_kept[i].missing) && art_kept[i].side == side &&
			strcmp(art_kept[i].path, path) == 0) {
			return &art_kept[i];
		}
	}
	return NULL;
}

// Under art_lock: the entry that has gone longest unused, emptied.
static art_entry_t *art_slot(void) {
	art_entry_t *oldest = &art_kept[0];
	for (int i = 0; i < ART_KEPT; i++) {
		if (!art_kept[i].data && !art_kept[i].missing) {
			oldest = &art_kept[i];
			break;
		}
		if (art_kept[i].used_ms < oldest->used_ms) {
			oldest = &art_kept[i];
		}
	}
	free(oldest->data);
	memset(oldest, 0, sizeof(*oldest));
	return oldest;
}

static void *art_worker(void *unused) {
	(void)unused;
	thread_be_background("sonixlink art");
	for (;;) {
		char path[SONIXLINK_PATH_MAX];
		int side;
		pthread_mutex_lock(&art_lock);
		while (!art_job_queued) {
			pthread_cond_wait(&art_wake, &art_lock);
		}
		snprintf(path, sizeof(path), "%s", art_job_path);
		side = art_job_side;
		art_job_queued = false;
		snprintf(art_busy_path, sizeof(art_busy_path), "%s", path);
		art_busy_side = side;
		bool done_already = art_find(path, side) != NULL;
		pthread_mutex_unlock(&art_lock);

		uint8_t *data = NULL;
		size_t len = 0;
		const char *type = "image/jpeg";
		bool missing = false;
		if (!done_already) {
			uint32_t began = now_ms();
			albumart_t art;
			memset(&art, 0, sizeof(art));
			if (!albumart_load_for_file(path, &art) || !art.data || art.size == 0) {
				missing = true;
			} else {
				data = art_shrink_to_jpeg(art.data, art.size, side, ART_JPEG_QUALITY, &len);
				if (data) {
					if (verbose) {
						printf("sonixlink: artwork %zu -> %zu bytes at %d px in %u ms\n", art.size, len, side,
							   now_ms() - began);
					}
				} else {
					// Already small, or not a picture the shrink can read: sent
					// as it is, so it is kept as it is.
					data = malloc(art.size);
					if (data) {
						memcpy(data, art.data, art.size);
						len = art.size;
						type = art_type_of(art.data, art.size);
					} else {
						missing = true;
					}
				}
			}
			// A card pulled out under the read: zeros, not artwork.
			if (data && albumart_faulted(&art)) {
				free(data);
				data = NULL;
				missing = true;
			}
			albumart_free(&art);
		}

		pthread_mutex_lock(&art_lock);
		if (!done_already) {
			art_entry_t *e = art_slot();
			snprintf(e->path, sizeof(e->path), "%s", path);
			e->side = side;
			e->missing = missing || !data;
			e->data = data;
			e->len = len;
			e->type = type;
			e->used_ms = now_ms();
		}
		art_busy_path[0] = '\0';
		pthread_mutex_unlock(&art_lock);
	}
	return NULL;
}

// Under art_lock: makes `path` at `side` the next job (replacing one not yet
// started: only the newest track matters).
static void art_queue_locked(const char *path, int side) {
	if (art_find(path, side)) {
		return;
	}
	if (art_busy_path[0] && art_busy_side == side && strcmp(art_busy_path, path) == 0) {
		return;
	}
	snprintf(art_job_path, sizeof(art_job_path), "%s", path);
	art_job_side = side;
	art_job_queued = true;
	if (!art_thread_started) {
		if (pthread_create(&art_thread, NULL, art_worker, NULL) == 0) {
			pthread_detach(art_thread);
			art_thread_started = true;
		}
	}
	pthread_cond_signal(&art_wake);
}

// The track has changed: its cover is made now, at the size the phone last
// asked for, if a phone is asking for fitted covers at all.
static void art_prepare_for(const char *path) {
	if (!path || !path[0]) {
		return;
	}
	pthread_mutex_lock(&art_lock);
	if (art_last_side > 0 && (uint32_t)(now_ms() - art_last_asked_ms) < ART_PREPARE_FOR_MS) {
		art_queue_locked(path, art_last_side);
	}
	pthread_mutex_unlock(&art_lock);
}

static void reply_art_bytes(client_t *c, const char *type, const uint8_t *data, size_t len) {
	buf_fmt(&c->out, "HTTP/1.1 200 OK\r\n");
	buf_fmt(&c->out, "Content-Type: %s\r\n", type);
	buf_fmt(&c->out, "Content-Length: %zu\r\n", len);
	buf_str(&c->out, "Access-Control-Allow-Origin: *\r\n");
	buf_str(&c->out, connection_header(c));
	buf_add(&c->out, (const char *)data, len);
}

static void route_art(client_t *c, const char *query) {
	char path[SONIXLINK_PATH_MAX] = "";
	if (!query_value(query, "path", path, sizeof(path)) || !path[0]) {
		reply_status(c, "400 Bad Request", "no path");
		return;
	}
	char max_text[16] = "";
	int max_side = 0;
	if (query_value(query, "max", max_text, sizeof(max_text)) && max_text[0]) {
		max_side = atoi(max_text);
		if (max_side < 64 || max_side > ART_MAX_SIDE_LIMIT) {
			max_side = 0;
		}
	}

	if (max_side) {
		pthread_mutex_lock(&art_lock);
		art_last_side = max_side;
		art_last_asked_ms = now_ms();
		art_entry_t *e = art_find(path, max_side);
		if (e) {
			e->used_ms = now_ms();
			if (e->missing) {
				pthread_mutex_unlock(&art_lock);
				reply_status(c, "404 Not Found", "no artwork");
				return;
			}
			reply_art_bytes(c, e->type, e->data, e->len);
			pthread_mutex_unlock(&art_lock);
			return;
		}
		art_queue_locked(path, max_side);
		pthread_mutex_unlock(&art_lock);
		// Not ready: asked again shortly. Retry-After in milliseconds is not
		// HTTP, so the app's own pace decides.
		reply_status(c, "202 Accepted", "preparing");
		return;
	}

	albumart_t art;
	memset(&art, 0, sizeof(art));
	if (!albumart_load_for_file(path, &art) || !art.data || art.size == 0) {
		albumart_free(&art);
		reply_status(c, "404 Not Found", "no artwork");
		return;
	}
	// What it is, from the bytes rather than from a file name: an embedded
	// picture has no name to read an extension off.
	size_t before = c->out.len;
	reply_art_bytes(c, art_type_of(art.data, art.size), art.data, art.size);
	if (albumart_faulted(&art)) {
		// A card pulled out under the copy: zeros, not artwork.
		c->out.len = before;
		albumart_free(&art);
		reply_status(c, "404 Not Found", "no artwork");
		return;
	}
	albumart_free(&art);
}

// The app has let go of the player: the status bar stops showing it now rather
// than when PEER_ALIVE_MS runs out.
static void route_bye(client_t *c) {
	pthread_mutex_lock(&lock);
	last_seen_ms = 0;
	peer_text[0] = '\0';
	pthread_mutex_unlock(&lock);
	reply_status(c, "200 OK", "bye");
}

// ---------------------------------------------------------------------------
// /api/browse: one folder of the card, as the file browser lists it
//
// Folders first, then what can be played, each alphabetically without regard
// to case; hidden entries and what a desktop leaves behind are skipped. A cue
// sheet is listed track by track ("3. Title", played through the virtual path
// "album.cue?track=3") and the file it cuts up is left out, as in browser.c.
// ---------------------------------------------------------------------------

typedef struct {
	char *label; // what the row says
	char *file;	 // the name inside the folder: the real one, or the sheet's virtual one
	bool dir;
} browse_entry_t;

typedef struct {
	browse_entry_t *items;
	int count;
	int cap;
} browse_list_t;

static bool browse_playable(const char *name) {
	static const char *const exts[] = {".wav", ".mp3", ".flac", ".ogg", ".m4b", ".m4a", ".alac", ".aac", ".dsf",
									   ".dff", ".aif", ".aiff", ".aifc", ".caf", ".opus", ".wv",	".ape"};
	if (playlist_is_junk_name(name)) {
		return false;
	}
	for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
		if (has_extension(name, exts[i])) {
			return true;
		}
	}
	return false;
}

static bool browse_add(browse_list_t *l, const char *label, const char *file, bool dir) {
	if (l->count >= BROWSE_MAX) {
		return false;
	}
	if (l->count == l->cap) {
		int cap = l->cap ? l->cap * 2 : 64;
		browse_entry_t *grown = realloc(l->items, sizeof(*grown) * (size_t)cap);
		if (!grown) {
			return false;
		}
		l->items = grown;
		l->cap = cap;
	}
	browse_entry_t *e = &l->items[l->count];
	e->label = strdup(label);
	e->file = strdup(file);
	e->dir = dir;
	if (!e->label || !e->file) {
		free(e->label);
		free(e->file);
		return false;
	}
	l->count++;
	return true;
}

static void browse_free(browse_list_t *l) {
	for (int i = 0; i < l->count; i++) {
		free(l->items[i].label);
		free(l->items[i].file);
	}
	free(l->items);
	memset(l, 0, sizeof(*l));
}

static int browse_cmp(const void *a, const void *b) {
	const browse_entry_t *ea = a;
	const browse_entry_t *eb = b;
	if (ea->dir != eb->dir) {
		return ea->dir ? -1 : 1;
	}
	int by_track = 0;
	if (cue_order_tracks(ea->file, eb->file, &by_track)) {
		return by_track;
	}
	return strcasecmp(ea->file, eb->file);
}

#define BROWSE_CLAIMS 32

static void browse_read(const char *path, browse_list_t *out) {
	DIR *dir = opendir(path);
	if (!dir) {
		return;
	}

	// The sheets first: what they cut up is not listed a second time.
	char (*claims)[256] = calloc(BROWSE_CLAIMS, 256);
	int claim_count = 0;
	cue_sheet_t *sheet = malloc(sizeof(*sheet));
	struct dirent *de;
	while (sheet && claims && (de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.' || !cue_is_sheet(de->d_name)) {
			continue;
		}
		char sheet_path[PATH_MAX + 260];
		snprintf(sheet_path, sizeof(sheet_path), "%s/%s", path, de->d_name);
		if (!cue_parse(sheet_path, sheet) || sheet->split) {
			continue;
		}
		for (int t = 0; t < sheet->track_count; t++) {
			char file[320];
			char label[220];
			cue_virtual_path(de->d_name, sheet->tracks[t].number, file, sizeof(file));
			if (sheet->tracks[t].title[0]) {
				snprintf(label, sizeof(label), "%d. %s", sheet->tracks[t].number, sheet->tracks[t].title);
			} else {
				snprintf(label, sizeof(label), "%d", sheet->tracks[t].number);
			}
			browse_add(out, label, file, false);
		}
		const char *slash = strrchr(sheet->audio_path, '/');
		if (claim_count < BROWSE_CLAIMS) {
			snprintf(claims[claim_count++], 256, "%.255s", slash ? slash + 1 : sheet->audio_path);
		}
	}
	free(sheet);
	rewinddir(dir);

	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.' || playlist_is_junk_name(de->d_name)) {
			continue;
		}
		bool is_dir;
		if (de->d_type == DT_DIR) {
			is_dir = true;
		} else if (de->d_type == DT_REG || de->d_type == DT_LNK || browse_playable(de->d_name)) {
			is_dir = false;
		} else {
			char entry[PATH_MAX + 260];
			snprintf(entry, sizeof(entry), "%s/%s", path, de->d_name);
			struct stat st;
			if (stat(entry, &st) != 0) {
				continue;
			}
			is_dir = S_ISDIR(st.st_mode);
		}
		if (!is_dir && !browse_playable(de->d_name)) {
			continue;
		}
		bool claimed = false;
		for (int i = 0; !is_dir && claims && i < claim_count && !claimed; i++) {
			claimed = strcmp(claims[i], de->d_name) == 0;
		}
		if (!claimed && !browse_add(out, de->d_name, de->d_name, is_dir)) {
			break;
		}
	}
	free(claims);
	closedir(dir);
	qsort(out->items, (size_t)out->count, sizeof(*out->items), browse_cmp);
}

static void route_browse(client_t *c, const char *query) {
	char root[sizeof(card_root)];
	pthread_mutex_lock(&lock);
	snprintf(root, sizeof(root), "%s", card_root);
	pthread_mutex_unlock(&lock);

	char asked[SONIXLINK_PATH_MAX] = "";
	query_value(query, "path", asked, sizeof(asked));

	// Only inside the card: the path is resolved first, so neither ".." nor a
	// link leads out of it.
	char real_root[PATH_MAX];
	char real[PATH_MAX];
	if (!root[0] || !realpath(root, real_root)) {
		reply_status(c, "404 Not Found", "no card");
		return;
	}
	if (!realpath(asked[0] ? asked : root, real)) {
		reply_status(c, "404 Not Found", "no such folder");
		return;
	}
	size_t root_len = strlen(real_root);
	if (strncmp(real, real_root, root_len) != 0 || (real[root_len] != '\0' && real[root_len] != '/')) {
		reply_status(c, "403 Forbidden", "outside the card");
		return;
	}
	struct stat st;
	if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) {
		reply_status(c, "404 Not Found", "not a folder");
		return;
	}

	// The paths go out under the root as the player spells it, which is how the
	// index spells them too, so the app can match them against it.
	char shown[PATH_MAX + sizeof(root)];
	snprintf(shown, sizeof(shown), "%s%s", root, real + root_len);
	char parent[sizeof(shown)] = "";
	if (real[root_len] != '\0') {
		snprintf(parent, sizeof(parent), "%s", shown);
		char *slash = strrchr(parent, '/');
		if (slash) {
			*slash = '\0';
		}
	}

	browse_list_t list;
	memset(&list, 0, sizeof(list));
	browse_read(real, &list);

	buf_t j;
	memset(&j, 0, sizeof(j));
	buf_str(&j, "{");
	buf_json_field(&j, "path", shown, true);
	buf_json_field(&j, "root", root, true);
	buf_json_field(&j, "parent", parent, true);
	buf_str(&j, "\"entries\":[");
	for (int i = 0; i < list.count; i++) {
		char full[sizeof(shown) + 330];
		snprintf(full, sizeof(full), "%s/%s", shown, list.items[i].file);
		buf_str(&j, i ? ",{" : "{");
		buf_json_field(&j, "name", list.items[i].label, true);
		buf_json_field(&j, "path", full, true);
		buf_fmt(&j, "\"dir\":%s}", list.items[i].dir ? "true" : "false");
	}
	buf_str(&j, "]}");
	browse_free(&list);
	reply_json(c, &j);
}

// A page for a browser, so the player can be checked without the app at all.
static void route_root(client_t *c) {
	sonixlink_state_t s;
	state_copy(&s);

	buf_t b;
	memset(&b, 0, sizeof(b));
	buf_str(&b, "<!doctype html><meta charset=\"utf-8\">");
	buf_str(&b, "<title>SonixLink</title>");
	buf_str(&b, "<style>body{font:16px system-ui;margin:2rem;max-width:34rem}"
				"a{display:inline-block;margin:.2rem .4rem .2rem 0}</style>");
	buf_fmt(&b, "<h1>%s</h1>", device_name_text);
	buf_fmt(&b, "<p>%u tracks in the index.</p>", s.track_count);
	buf_str(&b, "<p><a href=\"/api/info\">/api/info</a> <a href=\"/api/state\">/api/state</a> "
				"<a href=\"/api/db\">/api/db</a></p>");
	buf_str(&b, "<p>Commands: <code>/api/command?do=play</code>, <code>?do=pause</code>, "
				"<code>?do=next</code>, <code>?do=prev</code>, <code>?do=volume&amp;value=50</code>, "
				"<code>?do=mode&amp;value=shuffle</code>.</p>");
	reply_raw(c, "200 OK", "text/html; charset=utf-8", b.data ? b.data : "", b.len);
	buf_free(&b);
}

// ---------------------------------------------------------------------------
// The request
// ---------------------------------------------------------------------------

static void serve_request(client_t *c) {
	// "METHOD /path?query HTTP/1.1". The method is not looked at: nothing here
	// is destructive enough to need protecting from a browser, and every
	// argument travels in the query.
	char line[REQUEST_MAX];
	snprintf(line, sizeof(line), "%s", c->request);
	char *first_eol = strpbrk(line, "\r\n");
	if (first_eol) {
		*first_eol = '\0';
	}

	char *target = strchr(line, ' ');
	if (!target) {
		reply_status(c, "400 Bad Request", "malformed request");
		return;
	}
	target++;
	char *after = strchr(target, ' ');
	if (after) {
		*after = '\0';
	}

	char *query = strchr(target, '?');
	if (query) {
		*query++ = '\0';
	} else {
		query = (char *)"";
	}

	if (verbose) {
		printf("sonixlink: %s?%s\n", target, query);
	}

	if (strcmp(target, "/api/info") == 0) {
		route_info(c);
	} else if (strcmp(target, "/api/state") == 0) {
		route_state(c);
	} else if (strcmp(target, "/api/db") == 0) {
		route_db(c);
	} else if (strcmp(target, "/api/command") == 0) {
		route_command(c, query);
	} else if (strcmp(target, "/api/cover") == 0) {
		route_cover(c, query);
	} else if (strcmp(target, "/api/art") == 0) {
		route_art(c, query);
	} else if (strcmp(target, "/api/covers") == 0) {
		route_covers(c);
	} else if (strcmp(target, "/api/thumbs") == 0) {
		route_thumbs(c, query);
	} else if (strcmp(target, "/api/thumbkeys") == 0) {
		route_thumbkeys(c);
	} else if (strcmp(target, "/api/queue") == 0) {
		route_queue(c, query);
	} else if (strcmp(target, "/api/browse") == 0) {
		route_browse(c, query);
	} else if (strcmp(target, "/api/ping") == 0) {
		reply_status(c, "200 OK", "ok");
	} else if (strcmp(target, "/api/bye") == 0) {
		route_bye(c);
	} else if (strcmp(target, "/api/favourites") == 0) {
		route_favourites(c);
	} else if (strcmp(target, "/api/playlists") == 0) {
		route_playlists(c);
	} else if (strcmp(target, "/api/playlist") == 0) {
		route_playlist(c, query);
	} else if (strcmp(target, "/") == 0 || strcmp(target, "/index.html") == 0) {
		route_root(c);
	} else {
		reply_status(c, "404 Not Found", "no such thing");
	}
}

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------

static int listen_on(int port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0) {
		fprintf(stderr, "sonixlink: port %d unavailable: %s\n", port, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static void accept_client(int listener) {
	struct sockaddr_in from;
	socklen_t len = sizeof(from);
	int fd = accept(listener, (struct sockaddr *)&from, &len);
	if (fd < 0) {
		return;
	}

	client_t *slot = NULL;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].fd < 0) {
			slot = &clients[i];
			break;
		}
	}
	if (!slot) {
		// Every slot busy. Refusing is better than queueing: the phone retries
		// its next poll a moment later.
		close(fd);
		return;
	}

	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

	slot->fd = fd;
	slot->opened_ms = now_ms();

	char who[INET_ADDRSTRLEN] = "";
	inet_ntop(AF_INET, &from.sin_addr, who, sizeof(who));
	pthread_mutex_lock(&lock);
	snprintf(peer_text, sizeof(peer_text), "%s", who);
	last_seen_ms = now_ms();
	pthread_mutex_unlock(&lock);
}

// Reads until the blank line that ends the headers, then answers. A request
// bigger than the buffer is refused rather than grown into.
// The Content-Length a request's headers declare, 0 when they declare none.
static long request_content_length(const char *request, size_t head_len) {
	const char *at = request;
	const char *end = request + head_len;
	while (at < end) {
		const char *eol = memchr(at, '\n', (size_t)(end - at));
		if (!eol) {
			break;
		}
		if ((size_t)(eol - at) > 15 && strncasecmp(at, "Content-Length:", 15) == 0) {
			return strtol(at + 15, NULL, 10);
		}
		at = eol + 1;
	}
	return 0;
}

// The most a body may be: a selection of a few thousand tracks.
#define BODY_MAX (1024 * 1024)

static void read_client(client_t *c) {
	if (c->body_want) {
		char chunk[4096];
		ssize_t n = recv(c->fd, chunk, sizeof(chunk), 0);
		if (n <= 0) {
			if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
				return;
			}
			client_close(c);
			return;
		}
		buf_add(&c->body, chunk, (size_t)n);
		if (c->body.failed) {
			c->body_want = 0;
			reply_status(c, "500 Internal Server Error", "out of memory");
			return;
		}
		if (c->body.len >= c->body_want) {
			c->body_want = 0;
			serve_request(c);
		}
		return;
	}

	ssize_t n = recv(c->fd, c->request + c->request_len, sizeof(c->request) - c->request_len - 1, 0);
	if (n <= 0) {
		if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
			return;
		}
		client_close(c);
		return;
	}
	c->request_len += (size_t)n;
	c->request[c->request_len] = '\0';

	char *end = strstr(c->request, "\r\n\r\n");
	size_t head_len = end ? (size_t)(end - c->request) + 4 : 0;
	if (!end) {
		end = strstr(c->request, "\n\n");
		head_len = end ? (size_t)(end - c->request) + 2 : 0;
	}
	if (!end) {
		if (c->request_len + 1 >= sizeof(c->request)) {
			reply_status(c, "431 Request Header Fields Too Large", "too long");
		}
		return;
	}

	// A body, when the request has one: what arrived with the headers first,
	// the rest as it comes.
	long length = request_content_length(c->request, head_len);
	if (length > 0) {
		if (length > BODY_MAX) {
			reply_status(c, "413 Payload Too Large", "too long");
			return;
		}
		buf_free(&c->body);
		size_t already = c->request_len - head_len;
		if (already > (size_t)length) {
			already = (size_t)length;
		}
		buf_add(&c->body, c->request + head_len, already);
		c->request[head_len] = '\0';
		c->request_len = head_len;
		if (c->body.len < (size_t)length) {
			c->body_want = (size_t)length;
			return;
		}
	}

	serve_request(c);
}

// Writes what is ready, then the file if there is one. Never blocks: whatever
// the socket would not take now goes on the next turn of the loop.
static void write_client(client_t *c) {
	while (c->sent < c->out.len) {
		ssize_t n = send(c->fd, c->out.data + c->sent, c->out.len - c->sent, MSG_NOSIGNAL);
		if (n > 0) {
			c->sent += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EINTR)) {
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			return;
		}
		client_close(c);
		return;
	}

	if (!c->file) {
		client_done(c); // everything has gone
		return;
	}

	char chunk[8192];
	size_t want = sizeof(chunk);
	if ((long)want > c->file_left) {
		want = (size_t)c->file_left;
	}
	size_t got = want ? fread(chunk, 1, want, c->file) : 0;
	if (got == 0) {
		// A file shorter than it said: the reply is broken, so is the link.
		client_close(c);
		return;
	}

	size_t at = 0;
	while (at < got) {
		ssize_t n = send(c->fd, chunk + at, got - at, MSG_NOSIGNAL);
		if (n > 0) {
			at += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR) {
			continue;
		}
		client_close(c);
		return;
	}
	c->file_left -= (long)got;
	if (c->file_left <= 0) {
		client_done(c);
	}
}

// ---------------------------------------------------------------------------
// Being found
//
// Two ways, because neither has to carry the whole job. The DNS-SD record is
// what Android resolves by itself; the plain announcement is what anything else
// can listen for with six lines of socket code.
// ---------------------------------------------------------------------------

static int open_beacon(void) {
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return -1;
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	return fd;
}

static void send_beacon(int fd, const char *ip) {
	char message[256];
	int n = snprintf(message, sizeof(message), "SONIXLINK1 %s %d %s", ip, SONIXLINK_PORT, device_name_text);
	if (n <= 0) {
		return;
	}

	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(BEACON_PORT);
	to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	sendto(fd, message, (size_t)n, 0, (struct sockaddr *)&to, sizeof(to));
}

// --- mDNS ---
//
// Enough of it to be found, and no more: the record set for one service, sent
// when asked for and a few times unprompted. No probing and no conflict
// resolution -- there is one of these on the network, and if there are two the
// worst case is a phone offering a choice of two players with the same name.

static int open_mdns(void) {
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return -1;
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(MDNS_PORT);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}

	struct ip_mreq mreq;
	memset(&mreq, 0, sizeof(mreq));
	mreq.imr_multiaddr.s_addr = inet_addr(MDNS_ADDR);
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);
	setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

	unsigned char ttl = 255; // what the specification asks for
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
	return fd;
}

// A DNS name, written as a run of length-prefixed labels ending in a zero. No
// compression pointers are produced: the packet is small and a reader that
// handles pointers also handles their absence.
static size_t put_name(uint8_t *out, size_t at, size_t max, const char *name) {
	const char *p = name;
	while (*p) {
		const char *dot = strchr(p, '.');
		size_t len = dot ? (size_t)(dot - p) : strlen(p);
		if (len == 0 || len > 63 || at + len + 1 >= max) {
			return at;
		}
		out[at++] = (uint8_t)len;
		memcpy(out + at, p, len);
		at += len;
		p = dot ? dot + 1 : p + len;
	}
	if (at < max) {
		out[at++] = 0;
	}
	return at;
}

static size_t put_u16(uint8_t *out, size_t at, size_t max, uint16_t v) {
	if (at + 2 > max) {
		return at;
	}
	out[at++] = (uint8_t)(v >> 8);
	out[at++] = (uint8_t)(v & 0xff);
	return at;
}

static size_t put_u32be(uint8_t *out, size_t at, size_t max, uint32_t v) {
	if (at + 4 > max) {
		return at;
	}
	out[at++] = (uint8_t)(v >> 24);
	out[at++] = (uint8_t)((v >> 16) & 0xff);
	out[at++] = (uint8_t)((v >> 8) & 0xff);
	out[at++] = (uint8_t)(v & 0xff);
	return at;
}

// The whole answer: PTR for the service type, SRV and TXT for the instance, and
// A for the host. Sent as one packet so a resolver needs no second round trip.
static size_t build_mdns_answer(uint8_t *out, size_t max, const char *instance, const char *host, const char *ip) {
	size_t at = 0;
	at = put_u16(out, at, max, 0);		// id: zero in a response
	at = put_u16(out, at, max, 0x8400); // response, authoritative
	at = put_u16(out, at, max, 0);		// no questions
	at = put_u16(out, at, max, 4);		// four answers
	at = put_u16(out, at, max, 0);
	at = put_u16(out, at, max, 0);

	// PTR: the service type points at this instance.
	at = put_name(out, at, max, SERVICE_TYPE);
	at = put_u16(out, at, max, 12); // PTR
	at = put_u16(out, at, max, 1);	// IN
	at = put_u32be(out, at, max, MDNS_TTL);
	size_t len_at = at;
	at = put_u16(out, at, max, 0);
	size_t start = at;
	at = put_name(out, at, max, instance);
	put_u16(out, len_at, max, (uint16_t)(at - start));

	// SRV: where the instance actually is.
	at = put_name(out, at, max, instance);
	at = put_u16(out, at, max, 33); // SRV
	at = put_u16(out, at, max, 0x8001);
	at = put_u32be(out, at, max, MDNS_TTL);
	len_at = at;
	at = put_u16(out, at, max, 0);
	start = at;
	at = put_u16(out, at, max, 0); // priority
	at = put_u16(out, at, max, 0); // weight
	at = put_u16(out, at, max, SONIXLINK_PORT);
	at = put_name(out, at, max, host);
	put_u16(out, len_at, max, (uint16_t)(at - start));

	// TXT: one key, so the record is never empty.
	at = put_name(out, at, max, instance);
	at = put_u16(out, at, max, 16); // TXT
	at = put_u16(out, at, max, 0x8001);
	at = put_u32be(out, at, max, MDNS_TTL);
	len_at = at;
	at = put_u16(out, at, max, 0);
	start = at;
	{
		const char *txt = "api=1";
		size_t txt_len = strlen(txt);
		if (at + txt_len + 1 < max) {
			out[at++] = (uint8_t)txt_len;
			memcpy(out + at, txt, txt_len);
			at += txt_len;
		}
	}
	put_u16(out, len_at, max, (uint16_t)(at - start));

	// A: the host's address.
	at = put_name(out, at, max, host);
	at = put_u16(out, at, max, 1); // A
	at = put_u16(out, at, max, 0x8001);
	at = put_u32be(out, at, max, MDNS_TTL);
	at = put_u16(out, at, max, 4);
	struct in_addr parsed;
	if (inet_aton(ip, &parsed) && at + 4 <= max) {
		memcpy(out + at, &parsed.s_addr, 4);
		at += 4;
	}
	return at;
}

static void send_mdns(int fd, const char *instance, const char *host, const char *ip) {
	uint8_t packet[512];
	size_t len = build_mdns_answer(packet, sizeof(packet), instance, host, ip);
	if (len == 0) {
		return;
	}
	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(MDNS_PORT);
	to.sin_addr.s_addr = inet_addr(MDNS_ADDR);
	sendto(fd, packet, len, 0, (struct sockaddr *)&to, sizeof(to));
}

// Whether a query packet asks about this service. The question name is walked
// label by label and compared with the service type; a compression pointer in a
// question is not something a querier sends, so meeting one ends the walk.
static bool mdns_asks_for_us(const uint8_t *packet, size_t len) {
	if (len < 12) {
		return false;
	}
	if (packet[2] & 0x80) {
		return false; // a response, not a question
	}
	uint16_t questions = (uint16_t)((packet[4] << 8) | packet[5]);

	size_t at = 12;
	for (uint16_t q = 0; q < questions && at < len; q++) {
		char name[256];
		size_t n = 0;
		while (at < len && packet[at]) {
			size_t label = packet[at];
			if (label & 0xc0) {
				return false;
			}
			at++;
			if (at + label > len || n + label + 2 > sizeof(name)) {
				return false;
			}
			if (n) {
				name[n++] = '.';
			}
			memcpy(name + n, packet + at, label);
			n += label;
			at += label;
		}
		at++; // the zero that ends the name
		name[n] = '\0';
		at += 4; // type and class

		if (strcasecmp(name, SERVICE_TYPE) == 0 || strcasecmp(name, "_services._dns-sd._udp.local") == 0) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------

static void *sonixlink_worker(void *unused) {
	(void)unused;
	thread_be_background("sonixlink");

	int listener = -1, beacon = -1, mdns = -1;
	uint32_t last_beacon = 0, last_mdns = 0;
	char ip[INET6_ADDRSTRLEN] = "";
	char instance[192] = "";
	char host[160] = "";

	for (int i = 0; i < MAX_CLIENTS; i++) {
		clients[i].fd = -1;
	}

	for (;;) {
		// Bluetooth links handed over since the last pass get a slot of their
		// own, kept until the link goes quiet.
		int links[PENDING_LINKS];
		int link_count = 0;
		pthread_mutex_lock(&lock);
		link_count = pending_link_count;
		memcpy(links, pending_links, sizeof(int) * (size_t)link_count);
		pending_link_count = 0;
		pthread_mutex_unlock(&lock);
		for (int l = 0; l < link_count; l++) {
			client_t *slot = NULL;
			for (int i = 0; i < MAX_CLIENTS && !slot; i++) {
				if (clients[i].fd < 0) {
					slot = &clients[i];
				}
			}
			if (!slot) {
				close(links[l]);
				continue;
			}
			struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
			setsockopt(links[l], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
			slot->fd = links[l];
			slot->persistent = true;
			slot->opened_ms = now_ms();
			pthread_mutex_lock(&lock);
			snprintf(peer_text, sizeof(peer_text), "Bluetooth");
			last_seen_ms = now_ms();
			pthread_mutex_unlock(&lock);
			printf("sonixlink: a phone connected over Bluetooth\n");
		}

		if (!sonixlink_get_enabled()) {
			if (listener >= 0) {
				for (int i = 0; i < MAX_CLIENTS; i++) {
					if (clients[i].fd >= 0) {
						client_close(&clients[i]);
					}
				}
				close(listener);
				listener = -1;
				if (beacon >= 0) {
					close(beacon);
					beacon = -1;
				}
				if (mdns >= 0) {
					close(mdns);
					mdns = -1;
				}
				pthread_mutex_lock(&lock);
				address_text[0] = '\0';
				pthread_mutex_unlock(&lock);
				printf("sonixlink: off\n");
			}
			usleep(300 * 1000);
			continue;
		}

		if (listener < 0) {
			listener = listen_on(SONIXLINK_PORT);
			if (listener < 0) {
				sleep(1);
				continue;
			}
			beacon = open_beacon();
			mdns = open_mdns();
			read_device_name(device_name_text, sizeof(device_name_text));

			// The instance name is what a phone shows in its list. A dot in the
			// device's name would split the label, so it becomes a space.
			char clean[128];
			snprintf(clean, sizeof(clean), "%s", device_name_text);
			for (char *p = clean; *p; p++) {
				if (*p == '.') {
					*p = ' ';
				}
			}
			snprintf(instance, sizeof(instance), "%s.%s", clean, SERVICE_TYPE);
			snprintf(host, sizeof(host), "%s.local", clean);
			printf("sonixlink: listening on port %d as \"%s\"%s\n", SONIXLINK_PORT, device_name_text,
				   mdns >= 0 ? "" : " (without mDNS)");
			last_mdns = 0;
		}

		bool have_ip = local_address(ip, sizeof(ip));
		pthread_mutex_lock(&lock);
		if (have_ip) {
			snprintf(address_text, sizeof(address_text), "%s:%d", ip, SONIXLINK_PORT);
		} else {
			address_text[0] = '\0';
		}
		pthread_mutex_unlock(&lock);

		if (have_ip && beacon >= 0 && (uint32_t)(now_ms() - last_beacon) >= BEACON_MS) {
			last_beacon = now_ms();
			send_beacon(beacon, ip);
		}
		if (have_ip && mdns >= 0 && (uint32_t)(now_ms() - last_mdns) >= MDNS_ANNOUNCE_MS) {
			last_mdns = now_ms();
			send_mdns(mdns, instance, host, ip);
		}

		struct pollfd fds[2 + MAX_CLIENTS];
		int n = 0;
		fds[n].fd = listener;
		fds[n].events = POLLIN;
		n++;
		int mdns_slot = -1;
		if (mdns >= 0) {
			mdns_slot = n;
			fds[n].fd = mdns;
			fds[n].events = POLLIN;
			n++;
		}
		// Which client each descriptor belongs to. Kept explicitly rather than
		// worked out again afterwards: a client accepted further down this same
		// pass would shift a positional guess by one, and every client after it
		// would then be handed somebody else's events.
		int owner[MAX_CLIENTS];
		int watched = 0;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd < 0) {
				continue;
			}
			fds[n].fd = clients[i].fd;
			// A client with something to send is waiting on the socket, not on
			// the phone.
			fds[n].events = (clients[i].out.len > clients[i].sent || clients[i].file) ? POLLOUT : POLLIN;
			owner[watched++] = i;
			n++;
		}
		int first_client = n - watched;
		for (int i = 0; i < n; i++) {
			fds[i].revents = 0;
		}

		int ready = poll(fds, (nfds_t)n, TICK_MS);
		if (ready < 0 && errno != EINTR) {
			sleep(1);
			continue;
		}

		if (ready > 0) {
			if (fds[0].revents & POLLIN) {
				accept_client(listener);
			}
			if (mdns_slot >= 0 && (fds[mdns_slot].revents & POLLIN)) {
				uint8_t packet[1500];
				struct sockaddr_in from;
				socklen_t from_len = sizeof(from);
				ssize_t got = recvfrom(mdns, packet, sizeof(packet), 0, (struct sockaddr *)&from, &from_len);
				if (got > 0 && have_ip && mdns_asks_for_us(packet, (size_t)got)) {
					send_mdns(mdns, instance, host, ip);
				}
			}

			// Only the clients that were actually in the poll set, each with
			// its own events. A client accepted a moment ago is served on the
			// next pass, which is the very next tick.
			for (int w = 0; w < watched; w++) {
				client_t *c = &clients[owner[w]];
				if (c->fd < 0) {
					continue;
				}
				short revents = fds[first_client + w].revents;
				if (revents & (POLLHUP | POLLERR)) {
					client_close(c);
					continue;
				}
				if (revents & POLLIN) {
					pthread_mutex_lock(&lock);
					last_seen_ms = now_ms();
					pthread_mutex_unlock(&lock);
					if (c->persistent) {
						c->opened_ms = now_ms();
					}
					read_client(c);
				} else if (revents & POLLOUT) {
					write_client(c);
				}
			}
		}

		// A connection that arrived and then said nothing holds a slot. Ten
		// seconds is longer than any request of this protocol takes to arrive;
		// a Bluetooth link is given longer, between one request and the next.
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd < 0) {
				continue;
			}
			uint32_t idle = now_ms() - clients[i].opened_ms;
			bool busy = clients[i].out.len > clients[i].sent || clients[i].file;
			if (clients[i].persistent ? (!busy && idle > PERSISTENT_IDLE_MS) : idle > 10000) {
				client_close(&clients[i]);
			}
		}
	}

	return NULL;
}

void sonixlink_init(void) {
	if (worker_running) {
		return;
	}
	for (int i = 0; i < MAX_CLIENTS; i++) {
		clients[i].fd = -1;
	}

	pthread_mutex_lock(&lock);
	enabled = config_get_int("wireless", "sonixlink", 0) != 0;
	pthread_mutex_unlock(&lock);
	verbose = config_get_int("wireless", "sonixlink_log", 0) != 0;

	if (pthread_create(&worker, NULL, sonixlink_worker, NULL) != 0) {
		fprintf(stderr, "sonixlink: cannot start the thread\n");
		return;
	}
	pthread_detach(worker);
	worker_running = true;

	sonixlink_bt_init();
}
