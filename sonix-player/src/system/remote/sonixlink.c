#include "sonixlink.h"

#include "src/system/core/respath.h"

#include <arpa/inet.h>
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/system/library/albumart.h"
#include "src/system/core/config.h"
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

// A phone counts as present for this long after its last request. It polls the
// state a few times a second while its screen is on, so this is generous.
#define PEER_ALIVE_MS 8000

#define MDNS_ADDR "224.0.0.251"
#define MDNS_PORT 5353
#define MDNS_TTL 120
#define MDNS_ANNOUNCE_MS 30000

#define SERVICE_TYPE "_sonixlink._tcp.local"

#define NAME_PATH RESOURCE_DIR "/bt_name"
#define DEFAULT_NAME "Sonix Player"

#define CMD_QUEUE_LEN 16
#define MAX_CLIENTS 4

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
static char *queue_arena;
static size_t queue_arena_len;
static int queue_offsets[SONIXLINK_QUEUE_WINDOW];
static int queue_window_count;
static int queue_window_first;
static int queue_total;
static int queue_position;

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

void sonixlink_publish_queue(int total, int position, int first, const char *const *paths, int count) {
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
	pthread_mutex_unlock(&lock);
}

void sonixlink_publish(const sonixlink_state_t *state) {
	if (!state) {
		return;
	}
	pthread_mutex_lock(&lock);
	g_state = *state;
	have_state = true;
	pthread_mutex_unlock(&lock);
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
	uint32_t opened_ms; // when this client arrived, so a stalled one can go
} client_t;

static client_t clients[MAX_CLIENTS];

static void client_close(client_t *c) {
	if (c->fd >= 0) {
		close(c->fd);
	}
	if (c->file) {
		fclose(c->file);
	}
	buf_free(&c->out);
	memset(c, 0, sizeof(*c));
	c->fd = -1;
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
	buf_str(&c->out, "Connection: close\r\n\r\n");
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
	buf_str(&head, "Connection: close\r\n\r\n");
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

static void route_info(client_t *c) {
	sonixlink_state_t s;
	state_copy(&s);

	long size = 0, mtime = 0;
	bool have_db = db_stat(&size, &mtime);

	buf_t j;
	memset(&j, 0, sizeof(j));
	buf_str(&j, "{");
	buf_json_field(&j, "name", device_name_text, true);
	#ifdef BOARD_R1
		buf_json_field(&j, "model", "HiBy R1", true);
	#else
		buf_json_field(&j, "model", "HiBy R3 Pro II", true);
	#endif
	buf_json_field(&j, "firmware", sysinfo_os_version(), true);
	buf_json_field(&j, "serial", sysinfo_serial_number(), true);
	buf_fmt(&j, "\"api\":1,\"port\":%d,", SONIXLINK_PORT);
	buf_fmt(&j, "\"tracks\":%u,", s.track_count);
	buf_fmt(&j, "\"scanning\":%s,", s.scanning ? "true" : "false");
	buf_fmt(&j, "\"db_available\":%s,", have_db ? "true" : "false");
	buf_fmt(&j, "\"db_size\":%ld,\"db_mtime\":%ld,", size, mtime);

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
	buf_json_field(&j, "path", s.path, true);
	buf_fmt(&j, "\"sample_rate\":%u,\"bitrate\":%u,\"bits\":%u,", s.sample_rate, s.bitrate, s.bits);
	buf_fmt(&j, "\"lossless\":%s,", s.lossless ? "true" : "false");
	buf_fmt(&j, "\"battery\":%d,\"charging\":%s,", s.battery_percent, s.charging ? "true" : "false");
	buf_fmt(&j, "\"favourite\":%s,", s.favourite ? "true" : "false");
	buf_json_field(&j, "accent", s.accent[0] ? s.accent : "#3584e4", true);
	buf_fmt(&j, "\"queue_position\":%d,\"queue_count\":%d,", s.queue_position, s.queue_count);
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
	buf_str(&c->out, "Connection: close\r\n\r\n");

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
	return -1;
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
	buf_str(&c->out, "Connection: close\r\n\r\n");
	c->file = f;
	c->file_left = (long)st.st_size;
}


// The queue as the interface last handed it over: a window around the track
// playing now, with the paths only. The phone already has the index, so it
// looks up title and artist there instead of being sent them again.
static void route_queue(client_t *c) {
	buf_t j;
	memset(&j, 0, sizeof(j));

	pthread_mutex_lock(&lock);
	int total = queue_total;
	int position = queue_position;
	int first = queue_window_first;
	int count = queue_window_count;

	buf_str(&j, "{");
	buf_fmt(&j, "\"count\":%d,\"position\":%d,\"first\":%d,\"paths\":[", total, position, first);
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
	buf_str(&c->out, "Connection: close\r\n\r\n");
	c->file = f;
	c->file_left = (long)st.st_size;
	printf("sonixlink: sending the thumbnails, %ld bytes\n", (long)st.st_size);
}

// The artwork of a track, whole and undecoded: the picture inside the tags, or
// the cover file beside it. The thumbnails file answers the lists; this answers
// the one screen that wants the real thing, and the phone decodes it.
static void route_art(client_t *c, const char *query) {
	char path[SONIXLINK_PATH_MAX] = "";
	if (!query_value(query, "path", path, sizeof(path)) || !path[0]) {
		reply_status(c, "400 Bad Request", "no path");
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
	const char *type = "application/octet-stream";
	if (art.size > 3 && art.data[0] == 0xFF && art.data[1] == 0xD8) {
		type = "image/jpeg";
	} else if (art.size > 8 && art.data[0] == 0x89 && art.data[1] == 'P' && art.data[2] == 'N' && art.data[3] == 'G') {
		type = "image/png";
	}

	buf_fmt(&c->out, "HTTP/1.1 200 OK\r\n");
	buf_fmt(&c->out, "Content-Type: %s\r\n", type);
	buf_fmt(&c->out, "Content-Length: %zu\r\n", art.size);
	buf_str(&c->out, "Access-Control-Allow-Origin: *\r\n");
	buf_str(&c->out, "Connection: close\r\n\r\n");
	buf_add(&c->out, (const char *)art.data, art.size);
	albumart_free(&art);
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
	} else if (strcmp(target, "/api/queue") == 0) {
		route_queue(c);
	} else if (strcmp(target, "/api/favourites") == 0) {
		route_favourites(c);
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
static void read_client(client_t *c) {
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

	if (!strstr(c->request, "\r\n\r\n") && !strstr(c->request, "\n\n")) {
		if (c->request_len + 1 >= sizeof(c->request)) {
			reply_status(c, "431 Request Header Fields Too Large", "too long");
		}
		return;
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
		client_close(c); // Connection: close, and everything has gone
		return;
	}

	char chunk[8192];
	size_t want = sizeof(chunk);
	if ((long)want > c->file_left) {
		want = (size_t)c->file_left;
	}
	size_t got = want ? fread(chunk, 1, want, c->file) : 0;
	if (got == 0) {
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
		client_close(c);
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
	char ip[64] = "";
	char instance[192] = "";
	char host[160] = "";

	for (int i = 0; i < MAX_CLIENTS; i++) {
		clients[i].fd = -1;
	}

	for (;;) {
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
					read_client(c);
				} else if (revents & POLLOUT) {
					write_client(c);
				}
			}
		}

		// A connection that arrived and then said nothing holds a slot. Ten
		// seconds is longer than any request of this protocol takes to arrive.
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd >= 0 && (uint32_t)(now_ms() - clients[i].opened_ms) > 10000) {
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
}
