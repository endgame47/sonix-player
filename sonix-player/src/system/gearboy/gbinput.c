#include "gbinput.h"

#ifndef GB_CORE
#define GB_CORE 0
#endif

#if !GB_CORE || defined(HOST_BUILD)

// On the simulator the glass is a mouse in an SDL window: there is no evdev node
// to take over, so input stays whatever LVGL sends to the buttons -- one finger
// at a time, which is enough to exercise the page.
bool gbinput_start(const gbinput_zone_t *zones, int count, void (*on_menu)(void)) {
	(void)zones;
	(void)count;
	(void)on_menu;
	return false;
}
void gbinput_stop(void) {}
bool gbinput_active(void) { return false; }

#else

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "src/system/gearboy/gearboy.h"
#include "src/system/core/panel.h"
#include "src/system/device/power.h"

#define MAX_ZONES 16

// How long to wait for a packet before rechecking whether it is time to stop.
// 100 ms reads as instant to a human finger, and the thread sleeps the rest of
// the time.
#define POLL_TIMEOUT_MS 100

static pthread_t reader;
static bool thread_live;
static volatile bool stop_flag;
static volatile bool active_flag;

static gbinput_zone_t zones[MAX_ZONES];
static int zone_count;
static void (*menu_cb)(void);

// Glass size, used to map raw coordinates onto screen ones. Same as the
// display's, and constant here because this device has exactly one panel.
//
// Named GLASS_ rather than PANEL_ because PANEL_H is already the include guard
// of panel.h, which this file includes.
#ifdef BOARD_R1
	#define GLASS_W 480
	#define GLASS_H 800
#else
	#define GLASS_W 480
	#define GLASS_H 720
#endif

static int abs_min_x, abs_max_x, abs_min_y, abs_max_y;

bool gbinput_active(void) { return active_flag; }

// Axis ranges, asked of the driver rather than guessed: on this panel they
// nearly match the pixel grid, and "nearly" is not a basis for placing the edge
// of a button.
static void read_abs_range(int fd) {
	struct input_absinfo info;

	abs_min_x = 0;
	abs_max_x = GLASS_W - 1;
	abs_min_y = 0;
	abs_max_y = GLASS_H - 1;

	if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &info) == 0 && info.maximum > info.minimum) {
		abs_min_x = info.minimum;
		abs_max_x = info.maximum;
	}
	if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &info) == 0 && info.maximum > info.minimum) {
		abs_min_y = info.minimum;
		abs_max_y = info.maximum;
	}
}

static int scale(int value, int lo, int hi, int size) {
	if (hi <= lo) {
		return 0;
	}
	long long v = (long long)(value - lo) * (size - 1) / (hi - lo);
	if (v < 0) {
		v = 0;
	}
	if (v > size - 1) {
		v = size - 1;
	}
	return (int)v;
}

// Glass coordinates to screen coordinates, rotation included. The rotation has
// to be applied by hand because lv_evdev_set_calibration() only does it for
// LVGL, which is not in this path: without it, on a flipped screen the D-pad
// would answer to the A/B buttons.
static void to_screen(int raw_x, int raw_y, int *out_x, int *out_y) {
	int x = scale(raw_x, abs_min_x, abs_max_x, GLASS_W);
	int y = scale(raw_y, abs_min_y, abs_max_y, GLASS_H);

	if (display_get_rotated()) {
		x = GLASS_W - 1 - x;
		y = GLASS_H - 1 - y;
	}

	*out_x = x;
	*out_y = y;
}

static uint16_t keys_at(int x, int y) {
	for (int i = 0; i < zone_count; i++) {
		const gbinput_zone_t *z = &zones[i];
		if (x >= z->x && x < z->x + z->w && y >= z->y && y < z->y + z->h) {
			return z->keys;
		}
	}
	return 0;
}

// ---------------------------------------------------------------------------
// the reader thread
// ---------------------------------------------------------------------------
//
// Two protocols, because which one the driver speaks is not guaranteed.
//
// Type A (what the patched GT967 uses) sends contacts one after another,
// separated by SYN_MT_REPORT, and closes the packet with SYN_REPORT: there is
// no identity, only how many there are and where. That is exactly what a
// button pad needs, since a finger on a button need not be tracked as the same
// finger as before.
//
// Type B numbers the slots (ABS_MT_SLOT) and uses ABS_MT_TRACKING_ID of -1 to
// say a contact is gone. Supporting it costs twenty lines and removes the
// question.

typedef struct {
	int x, y;
	bool used;
} contact_t;

static void *reader_thread(void *arg) {
	int fd = (int)(intptr_t)arg;

	contact_t slots[GBINPUT_MAX_CONTACTS];
	memset(slots, 0, sizeof(slots));

	// Type A: the contacts of the packet being assembled.
	contact_t pending[GBINPUT_MAX_CONTACTS];
	int pending_count = 0;
	int pending_x = -1, pending_y = -1;

	// Type B: the current slot.
	int cur_slot = 0;
	bool protocol_b = false;
	uint16_t last_mask = 0;

	// True once the first SYN_MT_REPORT has been seen, that is once the driver
	// is known to speak type A. From then on every SYN_REPORT closes a packet,
	// and a packet with no contacts means the glass is clear. The gt9xx marks
	// the last finger leaving with a bare SYN_REPORT and no SYN_MT_REPORT, so
	// without this flag such a packet would carry no information and the keys
	// would stay pressed.
	bool protocol_a = false;

	// The driver reports this separately and it is the most direct answer of
	// all: at zero there is nothing on the glass. Not every driver sends it, so
	// it cannot be relied on alone, but when it arrives it outranks any
	// reconstruction.
	bool btn_touch_down = true;
	bool saw_btn_touch = false;

	memset(pending, 0, sizeof(pending));

	while (!stop_flag) {
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		int ready = poll(&pfd, 1, POLL_TIMEOUT_MS);
		if (ready <= 0) {
			continue; // timeout or a signal: recheck stop_flag
		}

		struct input_event ev[64];
		ssize_t got = read(fd, ev, sizeof(ev));
		if (got < (ssize_t)sizeof(ev[0])) {
			if (got < 0 && (errno == EINTR || errno == EAGAIN)) {
				continue;
			}
			break; // the node disappeared under the reader
		}

		int count = (int)(got / (ssize_t)sizeof(ev[0]));
		for (int i = 0; i < count; i++) {
			const struct input_event *e = &ev[i];

			if (e->type == EV_ABS) {
				switch (e->code) {
				case ABS_MT_SLOT:
					protocol_b = true;
					cur_slot = e->value;
					if (cur_slot < 0 || cur_slot >= GBINPUT_MAX_CONTACTS) {
						cur_slot = 0;
					}
					break;
				case ABS_MT_TRACKING_ID:
					if (protocol_b) {
						slots[cur_slot].used = e->value >= 0;
					}
					break;
				case ABS_MT_POSITION_X:
					if (protocol_b) {
						slots[cur_slot].x = e->value;
						slots[cur_slot].used = true;
					} else {
						pending_x = e->value;
					}
					break;
				case ABS_MT_POSITION_Y:
					if (protocol_b) {
						slots[cur_slot].y = e->value;
						slots[cur_slot].used = true;
					} else {
						pending_y = e->value;
					}
					break;
				default:
					break;
				}
				continue;
			}

			if (e->type == EV_KEY && e->code == BTN_TOUCH) {
				saw_btn_touch = true;
				btn_touch_down = e->value != 0;
				continue;
			}

			if (e->type != EV_SYN) {
				continue;
			}

			// End of one contact (type A).
			if (e->code == SYN_MT_REPORT) {
				protocol_a = true;
				if (pending_x >= 0 && pending_y >= 0 && pending_count < GBINPUT_MAX_CONTACTS) {
					pending[pending_count].x = pending_x;
					pending[pending_count].y = pending_y;
					pending[pending_count].used = true;
					pending_count++;
				}
				pending_x = pending_y = -1;
				continue;
			}

			if (e->code != SYN_REPORT) {
				continue;
			}

			// End of packet: the full state of the glass is known, and the full
			// key state follows from it. Recomputed from scratch every time,
			// which is what makes a finger sliding from one button to another
			// without lifting behave correctly.
			uint16_t mask = 0;

			if (protocol_b) {
				for (int s = 0; s < GBINPUT_MAX_CONTACTS; s++) {
					if (!slots[s].used) {
						continue;
					}
					int sx, sy;
					to_screen(slots[s].x, slots[s].y, &sx, &sy);
					mask |= keys_at(sx, sy);
				}
			} else if (protocol_a) {
				// Type A. A `pending_count` of zero means the glass is clear,
				// reached by both driver conventions: an empty SYN_MT_REPORT
				// followed by SYN_REPORT, or a bare SYN_REPORT. The gt9xx sends
				// the latter.
				for (int s = 0; s < pending_count; s++) {
					int sx, sy;
					to_screen(pending[s].x, pending[s].y, &sx, &sy);
					mask |= keys_at(sx, sy);
				}
			} else {
				// The protocol is still unknown: no slot and no SYN_MT_REPORT
				// seen yet. There is nothing to derive a state from, so the
				// previous state stands.
				continue;
			}

			// The last word: if the driver says the glass is clear, it is clear,
			// whatever the contacts say.
			if (saw_btn_touch && !btn_touch_down) {
				mask = 0;
			}

			// Menu fires on the rising edge: a finger resting there counts once,
			// not thirty times a second while it stays down.
			if ((mask & GBINPUT_KEY_MENU) && !(last_mask & GBINPUT_KEY_MENU) && menu_cb) {
				menu_cb();
			}
			last_mask = mask;

			gearboy_set_keys((uint16_t)(mask & ~GBINPUT_KEY_MENU));

			pending_count = 0;
			pending_x = pending_y = -1;
		}
	}

	close(fd);
	gearboy_set_keys(0);
	active_flag = false;
	return NULL;
}

bool gbinput_start(const gbinput_zone_t *list, int count, void (*on_menu)(void)) {
	if (active_flag) {
		return true;
	}
	if (thread_live) {
		pthread_join(reader, NULL);
		thread_live = false;
	}

	const char *node = panel_touch_device();
	if (!node) {
		fprintf(stderr, "gbinput: no touch node to take\n");
		return false;
	}

	if (count < 0) {
		count = 0;
	}
	if (count > MAX_ZONES) {
		count = MAX_ZONES;
	}
	memcpy(zones, list, (size_t)count * sizeof(zones[0]));
	zone_count = count;
	menu_cb = on_menu;

	int fd = open(node, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "gbinput: %s does not open: %s\n", node, strerror(errno));
		return false;
	}
	read_abs_range(fd);

	// LVGL lets go of the glass before the thread starts reading: the other way
	// round leaves a window where both interpret it, and the game's first touch
	// would also press the button underneath.
	panel_touch_enable(false);

	stop_flag = false;
	active_flag = true;
	if (pthread_create(&reader, NULL, reader_thread, (void *)(intptr_t)fd) != 0) {
		fprintf(stderr, "gbinput: the thread does not start\n");
		panel_touch_enable(true);
		active_flag = false;
		close(fd);
		return false;
	}
	thread_live = true;
	return true;
}

void gbinput_stop(void) {
	if (thread_live) {
		stop_flag = true;
		pthread_join(reader, NULL);
		thread_live = false;
	}
	active_flag = false;
	menu_cb = NULL;
	gearboy_set_keys(0);
	panel_touch_enable(true);
}

#endif /* GB_CORE && !HOST_BUILD */
