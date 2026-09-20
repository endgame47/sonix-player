#include "lvgl/lvgl.h"

#include <dirent.h>
#include <malloc.h>
#include <poll.h>
#include <sys/mman.h>
#include <pthread.h>
#include <linux/input.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/gui/nowplaying/cover.h"
#include "src/system/audio/waveform.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/settings/powersettings.h"
#include "src/gui/settings/settings.h"
#include "src/system/device/adb.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/playback/sleeptimer.h"
#include "src/system/audio/usbaudio.h"
#include "src/system/audio/audio.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/bluetooth/bluetooth.h"
#include "src/system/bluetooth/btplayer.h"
#include "src/system/device/clock.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/led.h"
#include "src/system/audio/eq.h"
#include "src/system/audio/headset.h"
#include "src/system/input/keymap.h"
#include "src/system/streaming/qobuz.h"
#include "src/system/streaming/tidal.h"
#include "src/system/remote/dlna.h"
#include "src/system/gearboy/gbdb.h"
#include "src/system/gearboy/gearboy.h"
#include "src/system/remote/sonixlink.h"
#include "src/system/core/panel.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/podcastcache.h"
#include "src/system/streaming/podcastsubs.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/streaming/streamkeys.h"
#include "src/system/library/library.h"
#include "src/system/core/logging.h"
#include "src/system/playback/playlist.h"
#include "src/system/library/playlists.h"
#include "src/system/device/power.h"
#include "src/system/streaming/radio.h"
#include "src/system/device/screenshot.h"
#include "src/system/net/tls.h"
#include "src/system/device/system.h"
#include "src/system/device/usb.h"
#include "src/system/core/utils.h"
#include "src/system/net/webcgi.h"
#include "src/system/net/wifi.h"


static volatile sig_atomic_t running = 1;

#ifdef HOST_BUILD
// Screenshot trigger on the simulator. The device uses volume-up plus power,
// physical keys that do not exist here, so creating /tmp/sonix/shot takes their
// place and runs the same screenshot path (copy, flash, PNG writer).
static void host_shot_poll_cb(lv_timer_t *timer) {
	(void)timer;
	if (access("/tmp/sonix/shot", F_OK) == 0) {
		remove("/tmp/sonix/shot");
		screenshot_request();
	}
}
#endif

// "Remember track", deferred: restoring the remembered track kicks off a cover
// decode and an audio open, which must not run while the very first frame is
// still being put together. It is scheduled a moment after the UI has painted.
static void restore_track_timer_cb(lv_timer_t *timer) {
	lv_timer_delete(timer);

	char saved_track[512];
	double saved_pos = 0;
	if (!library_playback_state_load(saved_track, sizeof(saved_track), &saved_pos) ||
		access(saved_track, R_OK) != 0) {
		return;
	}

	// The queue is saved next to the track, so a library list or a shuffled
	// order comes back as the queue the user left behind rather than as a
	// folder queue built out of the remembered file.
	// A queue that was a library list comes back as the query it was, re-run
	// against whatever the library holds now, so a rescan since the last boot
	// is reflected rather than restored around.
	library_index_spec_t spec;
	int spec_pos = 0;
	bool had_query = library_queue_load_query(&spec, &spec_pos);
	if (had_query) {
		library_index_t *ix = library_index_open(spec.kind, spec.filter, spec.value, spec.order, spec.desc);
		if (library_index_count(ix) > 0) {
			// Whatever was added to that queue by hand rides in the same table
			// the path form uses; here it is a handful of rows, not a library.
			// Each carries the slot it sat at, so they go back where they were
			// instead of in front of whatever is playing.
			char **extra = NULL;
			int *extra_slots = NULL;
			int extra_count = library_queue_load_extra(&extra, &extra_slots);
			player_restore_index(ix, spec_pos, (const char *const *)extra, extra_slots, extra_count, saved_track,
								 saved_pos);
			library_queue_free(extra, extra_count);
			free(extra_slots);
			return;
		}
		library_index_close(ix);
	}

	// The rows form, but only when there was no query: with one, whatever is in
	// that table is the handful of tracks added to the library queue by hand,
	// and restoring those alone comes back as a two-track folder queue.
	char **queue = NULL;
	int queue_index = 0;
	bool queue_custom = false;
	int queue_count = had_query ? 0 : library_queue_load(&queue, &queue_index, &queue_custom);
	if (queue_count > 0) {
		player_restore_list((const char *const *)queue, queue_count, queue_index, queue_custom, saved_track, saved_pos);
		library_queue_free(queue, queue_count);
		return;
	}

	player_restore_track(saved_track, saved_pos);
}

#ifndef HOST_BUILD
// Powering the Bluetooth radio down waits for three daemons to take their
// SIGTERM, so it happens off the interface thread. See the call site.
static void *bluetooth_off_thread(void *arg) {
	(void)arg;
	power_bluetooth_off();
	return NULL;
}
#endif

static void sigint_handler(int sig) {
	(void)sig;
	running = 0;
}

// ---------------------------------------------------------------------------
// staying alive
//
// /usr/bin/hiby_player.sh runs the player and then does `sleep 1; reboot`.
// Whatever the reason -- a clean exit, a failed display init, a segfault, a
// stray SIGPIPE -- the moment this process goes away the device reboots, so a
// bug that would be a crash on a PC is a boot loop here, with no console.
//
// Two rules follow. Nothing may kill the process by default, and no failure
// path may return from main(): it parks instead, which leaves the device up,
// ADB reachable and the log readable.
// ---------------------------------------------------------------------------

// Everything the crash handler writes is also kept here, so the same text can
// be handed to the black box in one go. Static, because a signal handler has
// no business allocating.
static char crash_text[512];
static size_t crash_used;

static void crash_keep(const char *text, size_t len) {
	if (crash_used + len >= sizeof(crash_text)) {
		len = sizeof(crash_text) - 1 - crash_used;
	}
	if (len == 0) {
		return;
	}
	memcpy(crash_text + crash_used, text, len);
	crash_used += len;
}

static void write_raw(const char *text) {
	size_t len = strlen(text);
	ssize_t ignored = write(STDERR_FILENO, text, len);
	(void)ignored;
	crash_keep(text, len);
}

// Writes an unsigned value as hex. Async-signal-safe: no snprintf, no malloc.
static void write_hex(unsigned long value) {
	static const char digits[] = "0123456789abcdef";
	char buf[2 + 2 * sizeof(unsigned long)];
	char *p = buf + sizeof(buf);
	do {
		*--p = digits[value & 0xf];
		value >>= 4;
	} while (value);
	*--p = 'x';
	*--p = '0';
	size_t len = (size_t)(buf + sizeof(buf) - p);
	ssize_t ignored = write(STDERR_FILENO, p, len);
	(void)ignored;
	crash_keep(p, len);
}

// The same as a plain decimal. A pid in hex is a pid nobody can match against
// what `ps` prints, and matching it is the whole point of printing it.
static void write_dec(unsigned long value) {
	char buf[3 * sizeof(unsigned long) + 1];
	char *p = buf + sizeof(buf);
	do {
		*--p = (char)('0' + (value % 10));
		value /= 10;
	} while (value);
	size_t len = (size_t)(buf + sizeof(buf) - p);
	ssize_t ignored = write(STDERR_FILENO, p, len);
	(void)ignored;
	crash_keep(p, len);
}

// ---------------------------------------------------------------------------
// what the machine was holding when it fell over
//
// On this device the kernel reports every user fault with si_code SI_KERNEL
// (0x80) and si_addr zero, so the address that faulted is NOT in the siginfo
// and a logged "fault address: 0x0" is an empty field, not a null pointer. It
// has to be worked out instead: disassemble the instruction at the program
// counter and read the register it used, which means the registers have to be
// in the log.
//
// Same reason for the stack. -O3 with no frame pointer leaves nothing a
// backtrace can walk, but return addresses are still lying in the frames, so
// reading the stack and keeping every word that falls inside the program's own
// code gives the chain of callers back. Both are plain memory reads, which is
// all a signal handler is allowed to do.
// ---------------------------------------------------------------------------

// The program's code, for telling a return address from a number that happens
// to be on the stack. Weak: a toolchain that defines neither simply gets no
// trace rather than a link error.
extern char _ftext[] __attribute__((weak));
extern char __executable_start[] __attribute__((weak));
extern char etext[] __attribute__((weak));

static uintptr_t text_begin(void) {
	if (_ftext) {
		return (uintptr_t)_ftext; // what the MIPS linker calls it
	}
	return (uintptr_t)__executable_start;
}

#if defined(__GLIBC__) && defined(__mips__)
#define CRASH_REGS 32
static const char *const REG_NAMES[CRASH_REGS] = {
	"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",	 "t3", "t4", "t5", "t6", "t7",
	"s0",	"s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0",	 "k1", "gp", "sp", "s8", "ra",
};
static uintptr_t crash_reg(const ucontext_t *uc, int i) { return (uintptr_t)uc->uc_mcontext.gregs[i]; }
static uintptr_t crash_pc(const ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext.pc; }
static uintptr_t crash_sp(const ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext.gregs[29]; }
#elif defined(__GLIBC__) && defined(__x86_64__)
// The host build, so the whole report can be exercised in the simulator and
// not only on a device nobody can attach a debugger to.
//
// The REG_* names live behind _GNU_SOURCE, which this file does not ask for;
// the numbers are fixed by the x86-64 ABI's gregset_t and are only ever used
// here.
#ifndef REG_RSP
#define REG_RSP 15
#define REG_RIP 16
#endif
#define CRASH_REGS 16
static const char *const REG_NAMES[CRASH_REGS] = {
	"r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rdi", "rsi", "rbp", "rbx", "rdx", "rax", "rcx", "rsp",
};
static uintptr_t crash_reg(const ucontext_t *uc, int i) { return (uintptr_t)uc->uc_mcontext.gregs[i]; }
static uintptr_t crash_pc(const ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext.gregs[REG_RIP]; }
static uintptr_t crash_sp(const ucontext_t *uc) { return (uintptr_t)uc->uc_mcontext.gregs[REG_RSP]; }
#endif

#ifdef CRASH_REGS
// Four to a line, named, because a column of bare numbers is unreadable and the
// name is what says which one the faulting instruction was using.
static void write_registers(const ucontext_t *uc) {
	for (int i = 0; i < CRASH_REGS; i++) {
		write_raw((i % 4) == 0 ? "*** " : "  ");
		write_raw(REG_NAMES[i]);
		write_raw("=");
		write_hex(crash_reg(uc, i));
		if ((i % 4) == 3) {
			write_raw("\n");
		}
	}
}

// Where the stack pointer sits between the two ends of the stack. A fault on a
// store to sp with a handful of bytes left below it is a stack overflow and
// needs no further explanation; a fault with megabytes to spare is something
// else entirely, and knowing which is which is the difference between looking
// for the right bug and looking for the wrong one.
static void write_stack_room(uintptr_t sp) {
	uintptr_t low = 0, high = 0;
	write_raw("*** sp: ");
	write_hex(sp);
	if (!thread_stack_bounds(&low, &high)) {
		write_raw("  (stack bounds unknown on this thread)\n");
		return;
	}
	write_raw("  stack ");
	write_hex(low);
	write_raw("..");
	write_hex(high);
	if (sp < low || sp >= high) {
		write_raw("  OUTSIDE ITS STACK\n");
		return;
	}
	write_raw("  room left ");
	write_dec((unsigned long)(sp - low));
	write_raw(" bytes, used ");
	write_dec((unsigned long)(high - sp));
	write_raw("\n");
}

// The first CRASH_TRACE_MAX words still on the stack that point into the
// program's own code. Not a backtrace -- there is no unwinding here and dead
// frames leave stale addresses behind -- but running them through addr2line
// names the functions that were on the way in.
#define CRASH_TRACE_MAX 24

static void write_stack_trace(uintptr_t sp) {
	uintptr_t low = 0, high = 0;
	uintptr_t code_begin = text_begin();
	uintptr_t code_end = (uintptr_t)etext;

	// Only within bounds that are known: reading past the end of a stack would
	// fault a second time, inside the handler, and the log would say nothing at
	// all -- which is the one failure this whole function exists to prevent.
	if (!code_begin || !code_end || code_begin >= code_end) {
		return;
	}
	if (!thread_stack_bounds(&low, &high) || sp < low || sp >= high) {
		return;
	}

	write_raw("*** on the stack, addr2line these:\n*** ");
	int shown = 0;
	for (uintptr_t at = sp; at + sizeof(uintptr_t) <= high && shown < CRASH_TRACE_MAX;
		 at += sizeof(uintptr_t)) {
		uintptr_t word;
		memcpy(&word, (const void *)at, sizeof(word));
		if (word < code_begin || word >= code_end) {
			continue;
		}
		write_hex(word);
		shown++;
		write_raw((shown % 6) == 0 ? "\n*** " : " ");
	}
	write_raw("\n");
}
#endif

// The three breadcrumbs, whichever of them has something to say. They cost
// nothing when empty, and together they are what says which file the player
// had in hand when it fell over.
static void write_crumbs(void) {
	static const struct {
		const char *label;
		const char *(*read)(void);
	} CRUMBS[] = {
		{"*** last file the player touched: ", crumb_get},
		{"*** track: ", crumb_track},
		{"*** decoding the artwork of: ", crumb_artwork},
	};

	for (size_t i = 0; i < sizeof(CRUMBS) / sizeof(CRUMBS[0]); i++) {
		const char *what = CRUMBS[i].read();
		if (what && what[0]) {
			write_raw(CRUMBS[i].label);
			write_raw(what);
			write_raw("\n");
		}
	}
}

// Runs on SIGSEGV and friends. Only async-signal-safe calls here: write(2) to
// the log, then park. Deliberately does not re-raise, because dying is what
// causes the reboot.
//
// With the program counter in the log,
//   mipsel-rockbox-linux-gnu-addr2line -f -e sonix_player 0x...
// names the function and the line.
static void crash_handler(int sig, siginfo_t *info, void *context) {
	const char *name = "signal";
	switch (sig) {
	case SIGSEGV:
		name = "SIGSEGV (bad memory access)";
		break;
	case SIGBUS:
		name = "SIGBUS (misaligned or bad address)";
		break;
	case SIGILL:
		name = "SIGILL (illegal instruction)";
		break;
	case SIGFPE:
		name = "SIGFPE (arithmetic error)";
		break;
	case SIGABRT:
		name = "SIGABRT (abort)";
		break;
	default:
		break;
	}

	write_raw("\n*** sonix_player crashed: ");
	write_raw(name);
	write_raw(" ***\n");

	if (info) {
		write_raw("*** fault address: ");
		write_hex((unsigned long)info->si_addr);
		write_raw("  code: ");
		write_hex((unsigned long)info->si_code);
		write_raw("\n");
	}

#ifdef CRASH_REGS
	if (context) {
		const ucontext_t *uc = (const ucontext_t *)context;
		uintptr_t sp = crash_sp(uc);

		write_raw("*** pc: ");
		write_hex(crash_pc(uc));
#if defined(__mips__)
		write_raw("  ra: ");
		write_hex(crash_reg(uc, 31));
#endif
		write_raw("\n*** addr2line -f -C -i -e sonix_player <pc> <ra>\n");

		// The stack first: it is the one question with a yes-or-no answer, and
		// it decides whether the registers below are even worth reading.
		write_stack_room(sp);
		write_registers(uc);
		write_stack_trace(sp);
	}
#else
	(void)context;
#endif

	write_crumbs();

	// Ask the scan to stop. Two stores to a bool, which is all this call is, so
	// it is safe from here -- and without it the scan thread keeps naming files
	// and the report ends up buried in the middle of the log.
	library_scan_stop();

	write_raw("*** staying alive so the device does not reboot; kill it over ADB to exit ***\n");

	// The log lives on the microSD and the card is never unmounted on the way
	// out, so a device whose power is pulled while frozen loses whatever the
	// kernel had not written back. fsync + sync put the crash block on the
	// card before anything else happens. Both are async-signal-safe.
	fsync(STDERR_FILENO);
	sync();

	// The charger back on. The player is about to sit here doing nothing, and
	// if it went down holding the charge limit the battery would never fill
	// again -- the charger driver keeps that bit until the next boot. One write
	// to one sysfs node, which is safe enough to do from here.
	power_charging_release();

	for (;;) {
		pause();
	}
}

#ifndef HOST_BUILD
// Called instead of returning from main() when something has gone wrong.
static void park(const char *reason) {
	fprintf(stderr, "*** %s\n", reason);
	fprintf(stderr, "*** staying alive so the device does not reboot; kill it over ADB to exit\n");
	fflush(stderr);

	while (running) {
		sleep(3600);
	}
}
#endif

#ifndef HOST_BUILD
// A couple of lines of process and system memory state. The same ones the
// watchdog prints when the interface locks up, but said once at boot so two
// boots can be compared.
static void report_memory(const char *when) {
	// For the process: VmSize is how much is mapped, VmRSS how much is really
	// in RAM, VmLck how much is pinned there and cannot be freed. VmLck is the
	// line that counts: it should be the code and nothing else.
	FILE *st = fopen("/proc/self/status", "r");
	if (st) {
		char line[128];
		while (fgets(line, sizeof(line), st)) {
			if (strncmp(line, "VmSize:", 7) == 0 || strncmp(line, "VmRSS:", 6) == 0 ||
				strncmp(line, "VmLck:", 6) == 0) {
				fprintf(stderr, "memory (%s): %s", when, line);
			}
		}
		fclose(st);
	}

	FILE *mi = fopen("/proc/meminfo", "r");
	if (mi) {
		char line[128];
		while (fgets(line, sizeof(line), mi)) {
			if (strncmp(line, "MemTotal:", 9) == 0 || strncmp(line, "MemFree:", 8) == 0 ||
				strncmp(line, "MemAvailable:", 13) == 0) {
				fprintf(stderr, "memory (%s): %s", when, line);
			}
		}
		fclose(mi);
	}
}


// Pins the executable pages -- the player's and the libraries' -- in RAM, and
// nothing else.
//
// Under memory pressure the kernel evicts clean code pages; the playback
// thread then page-faults in the middle of a FLAC frame with the card busy on
// a cover, stalls for seconds, and the starved audio stream dies with EIO. A
// fault in an icon only delays a redraw, so everything that is not code is
// left to the kernel: mlockall(MCL_CURRENT) would hold the untouched .bss and
// megabytes of icons in RAM at full price, on a machine with fifty-five.
//
// "Holds code" means executable AND NOT WRITABLE. The x alone is not enough:
// an ELF carrying no PT_GNU_STACK segment, or one marked executable, gets
// READ_IMPLIES_EXEC, and from then on the data segment, the .bss, the heap and
// the stack all come out of /proc/self/maps as "rwxp". Code is never writable,
// so the test is "r-x". The writable-and-executable bytes are counted and
// reported rather than silently dropped, because their being anything other
// than zero is what says this process runs under that flag.
//
// The map cannot tell code from read-only data either: this binary uses the
// classic two-segment layout, and the R E LOAD carries
//
//     .init .text .MIPS.stubs .fini .rodata .eh_frame_hdr .eh_frame
//
// in a single mapping. The linker places `etext` between .fini and .rodata, so
// the one mapping that holds this very function is locked only as far as
// etext and the read-only tail behind it is left to the kernel. Every other
// mapping, libraries included, is pinned whole: reading the marker wrong must
// cost protection, never correctness.
//
// If /proc/self/maps cannot be read, mlockall(MCL_CURRENT) is the fallback --
// pinning too much costs memory, never correctness.

// Provided by the linker: the first address past the code.
extern char etext[];

static void lock_code_pages(void) {
	FILE *maps = fopen("/proc/self/maps", "r");
	if (!maps) {
		if (mlockall(MCL_CURRENT) != 0) {
			perror("mlockall");
		}
		return;
	}

	long page_size = sysconf(_SC_PAGESIZE);
	unsigned long page = page_size > 0 ? (unsigned long)page_size : 4096u;
	unsigned long code_end = (unsigned long)etext;
	unsigned long here = (unsigned long)&lock_code_pages;

	char line[256];
	unsigned long locked = 0;
	unsigned long writable = 0;
	unsigned long readonly = 0;
	int regions = 0;
	int refused = 0;

	while (fgets(line, sizeof(line), maps)) {
		unsigned long start = 0;
		unsigned long end = 0;
		char perms[8] = "";
		if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3 || end <= start) {
			continue;
		}
		if (perms[2] != 'x') {
			continue;
		}
		// Executable and writable: data wearing the x this process gives to
		// everything readable. See the note above.
		if (perms[1] == 'w') {
			writable += end - start;
			continue;
		}

		unsigned long stop = end;
		// The player's own mapping, and the marker inside it: everything past
		// etext is .rodata and the unwind tables.
		if (here >= start && here < end && code_end > start && code_end < end) {
			stop = (code_end + page - 1) & ~(page - 1);
			if (stop > end) {
				stop = end;
			}
			readonly += end - stop;
		}

		if (mlock((void *)start, (size_t)(stop - start)) == 0) {
			locked += stop - start;
			regions++;
		} else {
			// The kernel keeps a few mappings of its own that cannot be
			// locked; they are not the player's code.
			refused++;
		}
	}
	fclose(maps);

	fprintf(stderr,
			"memory: %d code regions locked in RAM (%lu kB), %d refused, "
			"left %lu kB writable and %lu kB read-only\n",
			regions, locked / 1024, refused, writable / 1024, readonly / 1024);
}

// Tells the kernel who to kill when memory runs out. The player is the last
// thing that should go, because killing it reboots the device. The flip side
// of being this protected is that the player must never ask for more memory
// than the device can give -- see the decode budget in cover.c, which keeps a
// big cover from turning this protection into a thrashing kernel.
static void set_oom_priority(const char *value) {
	FILE *f = fopen("/proc/self/oom_score_adj", "w");
	if (!f) {
		return;
	}
	fputs(value, f);
	fclose(f);
}
#endif

// ---------------------------------------------------------------------------
// the interface watchdog
//
// If the interface thread ever stops turning the event loop, the device looks
// frozen and the log says nothing. This thread watches a heartbeat the main
// loop increments; when it stalls for a few seconds, every thread's name,
// state and kernel wait channel goes to the log, which names the stuck thread
// and the exact kernel call it is stuck in.
// ---------------------------------------------------------------------------

static volatile uint32_t ui_heartbeat;

#define WATCHDOG_STALL_SECONDS 5

// And how long a play request may sit unclaimed before the same dump is taken
// for the playback thread. Generous: opening a device can legitimately take a
// couple of seconds against a card that is still tearing the last stream down,
// and a track that is starting has already cleared the request by then.
#define WATCHDOG_PLAY_STALL_MS 10000

static void watchdog_dump_threads(void) {
	// Memory first: a stall with MemAvailable on the floor is the kernel
	// thrashing, not a bug in any one thread. Dirty and Writeback are pages
	// waiting to reach the card, so when those are most of what is missing the
	// card is the bottleneck and nobody has allocated anything. VmRSS answers
	// the other half -- whether the player itself has grown.
	FILE *mi = fopen("/proc/meminfo", "r");
	if (mi) {
		char line[128];
		while (fgets(line, sizeof(line), mi)) {
			if (strncmp(line, "MemFree:", 8) == 0 || strncmp(line, "MemAvailable:", 13) == 0 ||
				strncmp(line, "Dirty:", 6) == 0 || strncmp(line, "Writeback:", 10) == 0) {
				fprintf(stderr, "watchdog:   %s", line);
			}
		}
		fclose(mi);
	}

	FILE *st = fopen("/proc/self/status", "r");
	if (st) {
		char line[128];
		while (fgets(line, sizeof(line), st)) {
			if (strncmp(line, "VmRSS:", 6) == 0 || strncmp(line, "Threads:", 8) == 0) {
				fprintf(stderr, "watchdog:   %s", line);
			}
		}
		fclose(st);
	}

	const char *doing = crumb_get();
	if (doing && doing[0]) {
		fprintf(stderr, "watchdog:   last file: %s\n", doing);
	}
	if (crumb_track()[0]) {
		fprintf(stderr, "watchdog:   track: %s\n", crumb_track());
	}
	if (crumb_artwork()[0]) {
		fprintf(stderr, "watchdog:   decoding artwork of: %s\n", crumb_artwork());
	}

	DIR *tasks = opendir("/proc/self/task");
	if (!tasks) {
		return;
	}

	struct dirent *de;
	while ((de = readdir(tasks)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}

		char path[96], comm[32] = "?", state = '?', wchan[128] = "?";

		// Task ids are small integers, so the explicit precision only bounds a
		// case that cannot happen, and keeps the compiler quiet about it.
		snprintf(path, sizeof(path), "/proc/self/task/%.32s/comm", de->d_name);
		FILE *f = fopen(path, "r");
		if (f) {
			if (fgets(comm, sizeof(comm), f)) {
				comm[strcspn(comm, "\n")] = '\0';
			}
			fclose(f);
		}

		snprintf(path, sizeof(path), "/proc/self/task/%.32s/stat", de->d_name);
		f = fopen(path, "r");
		if (f) {
			// pid (comm) state ... -- the comm can hold spaces, so find the
			// closing parenthesis rather than trusting scanf.
			char line[256];
			if (fgets(line, sizeof(line), f)) {
				char *close = strrchr(line, ')');
				if (close && close[1] == ' ') {
					state = close[2];
				}
			}
			fclose(f);
		}

		snprintf(path, sizeof(path), "/proc/self/task/%.32s/wchan", de->d_name);
		f = fopen(path, "r");
		if (f) {
			size_t n = fread(wchan, 1, sizeof(wchan) - 1, f);
			wchan[n] = '\0';
			fclose(f);
		}

		fprintf(stderr, "watchdog:   tid %s (%s) state=%c wchan=%s\n", de->d_name, comm, state, wchan);
	}
	closedir(tasks);
	fflush(stderr);
}

static void *watchdog_thread(void *arg) {
	(void)arg;

	// Above the playback thread, which runs SCHED_RR at 10.
	//
	// The whole point of this thread is to report a device that has stopped
	// answering, and the way it stops answering on one core is a real-time
	// thread that never blocks. At normal priority the watchdog is exactly as
	// starved as the interface it is meant to report on -- which is why a freeze
	// can leave a log with nothing in it at all. It sleeps a second at a time
	// and then reads a few files, so it cannot take anything from the audio.
	thread_be_realtime("watchdog", 20);

	uint32_t last = ui_heartbeat;
	unsigned last_turns = audio_loop_turns();
	int stalled_for = 0;
	bool dumped = false;

	// The other thread worth watching. A stalled interface is obvious to the
	// person holding the device; a stalled playback thread is not -- the
	// interface answers, the track is logged as requested, and no sound comes
	// out, from any output. wchan in the dump names the syscall it went into
	// and did not come back from.
	bool play_dumped = false;

	for (;;) {
		sleep(1);

		unsigned turns = audio_loop_turns();
		unsigned turns_this_second = turns - last_turns;
		last_turns = turns;

		long waiting_ms = audio_play_request_age_ms();
		if (waiting_ms >= WATCHDOG_PLAY_STALL_MS) {
			if (!play_dumped) {
				play_dumped = true;
				fprintf(stderr, "watchdog: the playback thread has not taken a play request for %ld ms -- thread dump:\n",
						waiting_ms);
				watchdog_dump_threads();
			}
		} else {
			play_dumped = false;
		}

		uint32_t now = ui_heartbeat;
		if (now != last) {
			if (dumped) {
				fprintf(stderr, "watchdog: UI recovered after ~%d s\n", stalled_for);
			}
			last = now;
			stalled_for = 0;
			dumped = false;
			continue;
		}

		stalled_for++;
		if (stalled_for >= WATCHDOG_STALL_SECONDS && !dumped) {
			fprintf(stderr, "watchdog: UI thread has not run for %d s (playback loop %u turns in the last second) "
							"-- thread dump:\n",
					stalled_for, turns_this_second);
			watchdog_dump_threads();
			dumped = true; // once per stall, not once per second
		}
	}

	return NULL;
}

static void watchdog_start(void) {
	pthread_t thread;
	if (pthread_create(&thread, NULL, watchdog_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}

// Runs on SIGTERM and the other "stop now" signals.
//
// These are not faults, so the crash handler above never sees them. The
// interesting part is not the signal but the sender: SA_SIGINFO carries the pid
// of whoever asked, so a supervisor script killing the player and the user
// holding the power button can be told apart afterwards.
//
// It reports and then dies the way it would have died anyway: default action,
// re-raise. Nothing about the shutdown changes.
static void terminate_handler(int sig, siginfo_t *info, void *context) {
	(void)context;

	const char *name = "signal";
	switch (sig) {
	case SIGTERM:
		name = "SIGTERM (asked to stop)";
		break;
	case SIGINT:
		name = "SIGINT";
		break;
	case SIGQUIT:
		name = "SIGQUIT";
		break;
	case SIGHUP:
		name = "SIGHUP";
		break;
	default:
		break;
	}

	write_raw("\n*** sonix_player is being stopped: ");
	write_raw(name);
	write_raw(" ***\n");

	// si_pid is 0 when the kernel sent it rather than a process.
	if (info) {
		write_raw("*** sent by pid ");
		write_dec((unsigned long)info->si_pid);
		write_raw(" uid ");
		write_dec((unsigned long)info->si_uid);
		write_raw("\n");
	}

	write_crumbs();

	library_scan_stop();

	fsync(STDERR_FILENO);
	sync();

	signal(sig, SIG_DFL);
	raise(sig);
}

static void install_signal_guards(void) {
	// A closed socket or pipe must never be fatal: a failed sys_server
	// handshake would otherwise be a boot loop.
	signal(SIGPIPE, SIG_IGN);

	// A stack overflow raises SIGSEGV with no room left to run the handler in,
	// and the handler then faults again and the process dies with nothing in
	// the log. SA_ONSTACK sends it to the separate stack instead; the kernel
	// ignores the flag on a thread that has none.
	thread_signal_stack();

	// sigaction, not signal(): SA_SIGINFO is what carries the faulting
	// address and the register state the log needs.
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = crash_handler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);

	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);

	// The other way a process ends: someone asks it to. SIGKILL is missing
	// from this list because it cannot be caught -- what catches that one is
	// the kernel's own log, read back at the next start (see
	// logging_report_previous_run).
	struct sigaction term;
	memset(&term, 0, sizeof(term));
	term.sa_sigaction = terminate_handler;
	term.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&term.sa_mask);

	sigaction(SIGTERM, &term, NULL);
	sigaction(SIGINT, &term, NULL);
	sigaction(SIGQUIT, &term, NULL);
	sigaction(SIGHUP, &term, NULL);
}

#ifdef HOST_BUILD
#include "src/drivers/sdl/lv_sdl_keyboard.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/drivers/sdl/lv_sdl_window.h"
#include <SDL2/SDL.h>

#define GET_MUSIC_DIR() (snprintf((char[256]){0}, 256, "%s/Music", getenv("HOME") ? getenv("HOME") : ""))
#else
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#include "src/drivers/evdev/lv_evdev.h"
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif

// The longest the main loop will ever sleep between passes of
// lv_timer_handler(). See the loop itself for why this cap exists.
#define MAIN_LOOP_MAX_SLEEP_MS 250

// ---------------------------------------------------------------------------
// Kinetic scroll tuning.
//
// LVGL has kinetic scrolling of its own -- the list glides and brakes when the
// finger lifts -- but with the factory values it is barely noticeable on this
// screen. Two knobs, both per-indev and with no public setter in 9.1, hence
// the private header include:
//
//   * scroll_throw: the percentage the velocity drops on each frame of the
//     throw animation. The default 10 kills the glide in half a second; at 5
//     it lasts about twice as long and travels about twice as far.
//   * scroll_limit: how many pixels the finger must move before LVGL calls it
//     a scroll rather than a tap. The default 10 is a lot on a 480x720 panel
//     and the scroll starts late; at 4 the list latches on immediately.
//
// Applied to both the device touch panel and the simulator mouse, so the two
// feel the same.
// ---------------------------------------------------------------------------
#include "lvgl/src/indev/lv_indev_private.h"

#define SCROLL_THROW_DECAY_PCT 5
#define SCROLL_START_LIMIT_PX 4

static void tune_kinetic_scroll(lv_indev_t *indev) {
	if (!indev) {
		return;
	}
	indev->scroll_throw = SCROLL_THROW_DECAY_PCT;
	indev->scroll_limit = SCROLL_START_LIMIT_PX;
}

#if defined(BOARD_R1)
	#define SCREEN_WIDTH 480
	#define SCREEN_HEIGHT 800
#else
	#define SCREEN_WIDTH 480
	#define SCREEN_HEIGHT 720
#endif

static uint32_t custom_tick_get(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

static void post_gui_popup(const char *message, void *user_data) {
	(void)user_data;
	gui_notify_popup(message);
}

#ifdef HOST_BUILD
static lv_display_t *host_display;

// The keyboard stands in for the buttons on the case.
//
// The two skip keys arrive from the kernel swapped -- NEXTSONG is the upper
// button and PREVIOUSSONG the lower one (see keymap_button_for_code) -- so the
// codes below are crossed to match what the device really sends.
static const struct {
	SDL_Keycode key;
	int code;
} HOST_BUTTONS[] = {
	{SDLK_p, KEY_POWER},		{SDLK_u, KEY_VOLUMEUP},		{SDLK_i, KEY_VOLUMEDOWN},
	{SDLK_b, KEY_NEXTSONG},		{SDLK_n, KEY_PLAYPAUSE},	{SDLK_m, KEY_PREVIOUSSONG},
};

static int host_key_watch(void *userdata, SDL_Event *event) {
	(void)userdata;
	if (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP) {
		return 1;
	}
	if (event->key.repeat) {
		return 1;
	}
	for (size_t i = 0; i < sizeof(HOST_BUTTONS) / sizeof(HOST_BUTTONS[0]); i++) {
		if (HOST_BUTTONS[i].key == event->key.keysym.sym) {
			input_host_button(HOST_BUTTONS[i].code, event->type == SDL_KEYDOWN);
			break;
		}
	}
	return 1;
}

static lv_display_t *init_host_display(void) {
	lv_display_t *disp = lv_sdl_window_create(SCREEN_WIDTH, SCREEN_HEIGHT);
	if (!disp) {
		fprintf(stderr, "Error: Failed to create SDL2 window\n");
		return NULL;
	}

	// Non-resizeable, which also makes tiling window managers float it.
	lv_sdl_window_set_resizeable(disp, false);

	// Flushing the backbuffers once here avoids the initial viewport shift.
	SDL_Renderer *renderer = (SDL_Renderer *)lv_sdl_window_get_renderer(disp);
	if (renderer) {
		SDL_RenderClear(renderer);	 // Clear backbuffer state
		SDL_RenderPresent(renderer); // Flush context out to Wayland surface
		SDL_Delay(50);				 // Brief pause to allow the compositor coordinates to settle
	}

	lv_indev_t *mouse = lv_sdl_mouse_create();
	if (mouse) {
		lv_indev_set_display(mouse, disp);
		tune_kinetic_scroll(mouse);
	}

	host_display = disp;

	lv_indev_t *kbd = lv_sdl_keyboard_create();
	if (kbd) {
		lv_indev_set_display(kbd, disp);
	}

	SDL_AddEventWatch(host_key_watch, NULL);
	fprintf(stderr, "input: keyboard buttons -- p power, u vol+, i vol-, b prev, n play/pause, m next\n");

	return disp;
}

// No panel to re-arm on the host: the SDL window never blanks.
void display_wake_begin(lv_display_t *disp) { (void)disp; }
void display_wake_end(lv_display_t *disp) { (void)disp; }

// The folder that stands in for the memory card: SONIX_SD_ROOT, else the
// documents folder named by XDG, else ~/Documents or ~/Documenti.
static void host_card_root(char *out, size_t size) {
	const char *forced = getenv("SONIX_SD_ROOT");
	if (forced && forced[0]) {
		snprintf(out, size, "%s", forced);
		return;
	}

	const char *home = getenv("HOME");
	if (!home || !home[0]) {
		home = ".";
	}

	char conf[1024];
	snprintf(conf, sizeof(conf), "%s/.config/user-dirs.dirs", home);
	FILE *f = fopen(conf, "r");
	if (f) {
		char line[1024];
		while (fgets(line, sizeof(line), f)) {
			const char *eq = strchr(line, '=');
			if (!eq || strncmp(line, "XDG_DOCUMENTS_DIR", 17) != 0) {
				continue;
			}
			char value[1024];
			snprintf(value, sizeof(value), "%s", eq + 1);
			value[strcspn(value, "\r\n")] = '\0';

			char *v = value;
			size_t len = strlen(v);
			if (len >= 2 && v[0] == '"' && v[len - 1] == '"') {
				v[len - 1] = '\0';
				v++;
			}
			if (strncmp(v, "$HOME", 5) == 0) {
				snprintf(out, size, "%s%s", home, v + 5);
			} else {
				snprintf(out, size, "%s", v);
			}
			fclose(f);
			if (out[0]) {
				return;
			}
			f = NULL;
			break;
		}
		if (f) {
			fclose(f);
		}
	}

	snprintf(out, size, "%s/Documents", home);
	if (access(out, F_OK) == 0) {
		return;
	}
	char italian[1024];
	snprintf(italian, sizeof(italian), "%s/Documenti", home);
	if (access(italian, F_OK) == 0) {
		snprintf(out, size, "%s", italian);
	}
}

// The simulator draws through SDL, which owns its own texture: there is no
// framebuffer page to rotate into. The setting is still remembered so the
// switch behaves the same way, it just has nothing to turn upside down.
static bool host_rotated;
bool display_rotation_supported(void) { return false; }
bool display_get_rotated(void) { return host_rotated; }
void display_set_rotated(bool rotated) { host_rotated = rotated; }

// The touch panel on the simulator is the mouse inside the SDL window: there
// is no evdev node to hand over, and nothing to switch off.
void panel_touch_enable(bool enabled) { (void)enabled; }
const char *panel_touch_device(void) { return NULL; }

// There is no framebuffer to read on the simulator, but the SDL renderer can
// be read back. The rest of the path is identical (same copy, same flash, same
// PNG writer); only the source of the pixels differs.
uint16_t *display_capture_frame(int *out_w, int *out_h) {
	SDL_Renderer *renderer = host_display ? (SDL_Renderer *)lv_sdl_window_get_renderer(host_display) : NULL;
	if (!renderer) {
		return NULL;
	}

	int w = 0, h = 0;
	if (SDL_GetRendererOutputSize(renderer, &w, &h) != 0 || w <= 0 || h <= 0) {
		return NULL;
	}

	// A single reused block, the same rule as the framebuffer version below,
	// so callers behave identically in both builds.
	static uint16_t *copy;
	static size_t copy_size;
	size_t want = (size_t)w * (size_t)h * sizeof(uint16_t);
	if (!copy || copy_size != want) {
		free(copy);
		copy = malloc(want);
		copy_size = copy ? want : 0;
	}
	if (!copy) {
		return NULL;
	}
	if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_RGB565, copy, w * (int)sizeof(uint16_t)) != 0) {
		fprintf(stderr, "fb: SDL_RenderReadPixels: %s\n", SDL_GetError());
		return NULL;
	}

	if (out_w) {
		*out_w = w;
	}
	if (out_h) {
		*out_h = h;
	}
	return copy;
}
#else
// ---------------------------------------------------------------------------
// Double-buffered framebuffer with page flipping.
//
// LVGL's stock fbdev driver memcpy()s every dirty area straight into the one
// visible buffer, so the panel scans out a half-updated frame and the
// interface tears whenever it moves. Here LVGL draws directly into the
// off-screen half of a double-height framebuffer, refr_sync_areas() keeps the
// two pages coherent, and the flush of the last area pans with
// FBIOPAN_DISPLAY, which the display driver latches on vsync. A framebuffer
// that cannot give two pages falls back to LVGL's plain driver.
// ---------------------------------------------------------------------------

static int fb_fd = -1;
static uint8_t *fb_mem;
static size_t fb_page_size;
static struct fb_var_screeninfo fb_var;

// ---------------------------------------------------------------------------
// 180 degree rotation ("Rotate screen")
//
// LVGL 9.1 has no display rotation of its own (it arrives in 9.2), and the
// panel cannot be told to scan out upside down. So when the option is on the
// rendering moves off the framebuffer: LVGL draws into ONE full-screen RAM
// page, and what it draws is copied into the framebuffer turned through 180
// degrees. Unrotated, LVGL renders straight into the mapped pages, with no
// copy at all.
//
// One RAM page, not two, deliberately. With two, LVGL's direct mode keeps
// them coherent by re-drawing the previous frame's areas into the buffer it
// is rendering into, but it does not flush those areas, because for LVGL the
// buffer is the framebuffer -- and with the copy in between, the framebuffer
// page would never receive them. A single RAM page always holds the whole
// current frame, and the code below keeps its own record of what each
// framebuffer page is still missing.
//
// The touch panel is turned with it, by inverting the evdev calibration.
// ---------------------------------------------------------------------------

static lv_display_t *fb_display;   // the page-flipping display, NULL on the fallback
static lv_indev_t *fb_touch;	   // so the calibration can be flipped with the screen
static bool fb_rotated;
static uint8_t *rot_page;		   // the RAM page LVGL draws into while rotated

// What this frame touched, and what each framebuffer page has not been given
// yet (a bounding box each -- cheap, and a frame's damage is one region in
// practice). Empty is x1 > x2.
static lv_area_t rot_frame_dirty;
static lv_area_t rot_page_pending[2];

static void area_clear(lv_area_t *a) {
	a->x1 = 1;
	a->y1 = 1;
	a->x2 = 0;
	a->y2 = 0;
}

static bool area_is_empty(const lv_area_t *a) { return a->x1 > a->x2 || a->y1 > a->y2; }

static void area_merge(lv_area_t *into, const lv_area_t *add) {
	if (area_is_empty(add)) {
		return;
	}
	if (area_is_empty(into)) {
		*into = *add;
		return;
	}
	if (add->x1 < into->x1) {
		into->x1 = add->x1;
	}
	if (add->y1 < into->y1) {
		into->y1 = add->y1;
	}
	if (add->x2 > into->x2) {
		into->x2 = add->x2;
	}
	if (add->y2 > into->y2) {
		into->y2 = add->y2;
	}
}

static void area_set_full(lv_area_t *a) {
	a->x1 = 0;
	a->y1 = 0;
	a->x2 = (int32_t)fb_var.xres - 1;
	a->y2 = (int32_t)fb_var.yres - 1;
}

// Copies one dirty rectangle into `dst` (a whole framebuffer page) rotated by
// 180 degrees: the pixel at (x, y) lands at (W-1-x, H-1-y).
static void blit_rotated_180(uint16_t *dst, const uint16_t *src, const lv_area_t *area, int32_t width,
							 int32_t height) {
	int32_t x1 = area->x1 < 0 ? 0 : area->x1;
	int32_t y1 = area->y1 < 0 ? 0 : area->y1;
	int32_t x2 = area->x2 >= width ? width - 1 : area->x2;
	int32_t y2 = area->y2 >= height ? height - 1 : area->y2;

	for (int32_t y = y1; y <= y2; y++) {
		const uint16_t *src_row = src + (size_t)y * width;
		uint16_t *dst_row = dst + (size_t)(height - 1 - y) * width;
		for (int32_t x = x1; x <= x2; x++) {
			dst_row[width - 1 - x] = src_row[x];
		}
	}
}

static void fb_pan_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
	bool second_page;

	if (fb_rotated) {
		// Collect this frame's damage; the copy happens once, on the last
		// area, into the page that is NOT on screen.
		area_merge(&rot_frame_dirty, area);
		if (!lv_display_flush_is_last(disp)) {
			lv_display_flush_ready(disp);
			return;
		}

		second_page = (fb_var.yoffset == 0); // flip to the other page
		int target = second_page ? 1 : 0;

		// Everything this page still owes, plus what just changed.
		lv_area_t redraw = rot_page_pending[target];
		area_merge(&redraw, &rot_frame_dirty);

		if (!area_is_empty(&redraw)) {
			blit_rotated_180((uint16_t *)(fb_mem + (second_page ? fb_page_size : 0)), (const uint16_t *)px, &redraw,
							 (int32_t)fb_var.xres, (int32_t)fb_var.yres);
		}

		area_clear(&rot_page_pending[target]);
		area_merge(&rot_page_pending[target ? 0 : 1], &rot_frame_dirty); // the other page still owes it
		area_clear(&rot_frame_dirty);
	} else {
		second_page = (px >= fb_mem + fb_page_size);
	}

	if (lv_display_flush_is_last(disp)) {
		fb_var.yoffset = second_page ? fb_var.yres : 0;
		if (ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var) < 0) {
			// One warning, not one per frame.
			static bool warned;
			if (!warned) {
				perror("fb: FBIOPAN_DISPLAY");
				warned = true;
			}
		}
	}
	lv_display_flush_ready(disp);
}

static lv_display_t *try_panned_display(void) {
	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0) {
		return NULL;
	}

	struct fb_fix_screeninfo fix;
	if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var) < 0 || ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		close(fb_fd);
		fb_fd = -1;
		return NULL;
	}

	// LVGL renders with a stride of xres * bytespp; a padded framebuffer line
	// would shear the picture, so refuse and fall back.
	uint32_t bytespp = fb_var.bits_per_pixel / 8;
	if (bytespp != 2 || fix.line_length != fb_var.xres * bytespp) {
		close(fb_fd);
		fb_fd = -1;
		return NULL;
	}

	// Ask for two pages.
	fb_var.yres_virtual = fb_var.yres * 2;
	fb_var.xres_virtual = fb_var.xres;
	fb_var.yoffset = 0;
	if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &fb_var) < 0) {
		close(fb_fd);
		fb_fd = -1;
		return NULL;
	}
	ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix); // smem_len may have been updated

	fb_page_size = (size_t)fix.line_length * fb_var.yres;
	if ((size_t)fix.smem_len < fb_page_size * 2) {
		close(fb_fd);
		fb_fd = -1;
		return NULL;
	}

	fb_mem = mmap(NULL, fb_page_size * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_mem == MAP_FAILED) {
		close(fb_fd);
		fb_fd = -1;
		return NULL;
	}

	lv_display_t *disp = lv_display_create((int32_t)fb_var.xres, (int32_t)fb_var.yres);
	if (!disp) {
		munmap(fb_mem, fb_page_size * 2);
		close(fb_fd);
		fb_fd = -1;
		return NULL;
	}

	lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(disp, fb_mem, fb_mem + fb_page_size, (uint32_t)fb_page_size,
						   LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(disp, fb_pan_flush_cb);
	fb_display = disp;

	printf("fb: double-buffered, page flip on FBIOPAN_DISPLAY (%ux%u)\n", fb_var.xres, fb_var.yres);
	return disp;
}

// A copy of what the panel is showing right now.
//
// fb_var.yoffset says which of the two pages is on screen, and the flush
// updates it at every swap: zero is the first page, yres the second. Nothing
// else is needed, not even whether rotation is on, because while rotated the
// framebuffer already holds the turned image, which is exactly what is
// visible.
uint16_t *display_capture_frame(int *out_w, int *out_h) {
	if (!fb_mem || fb_page_size == 0) {
		return NULL; // plain fbdev fallback, no second page
	}

	// One block, kept for the life of the process instead of allocating one
	// per shot: nearly 700 KB contiguous, which is exactly the request malloc
	// refuses on this device while Qobuz is downloading and a decoder is open.
	//
	// The caller must not free it (see screenshot.c): the block stays here.
	static uint16_t *copy;
	static size_t copy_size;
	if (!copy || copy_size != fb_page_size) {
		free(copy);
		copy = malloc(fb_page_size);
		copy_size = copy ? fb_page_size : 0;
	}
	if (!copy) {
		fprintf(stderr, "fb: no memory for the screen copy (%zu bytes)\n", fb_page_size);
		return NULL;
	}

	const uint8_t *page = fb_mem + (fb_var.yoffset ? fb_page_size : 0);
	memcpy(copy, page, fb_page_size);

	if (out_w) {
		*out_w = (int)fb_var.xres;
	}
	if (out_h) {
		*out_h = (int)fb_var.yres;
	}
	return copy;
}

// ---------------------------------------------------------------------------
// Wake-from-blank kick (called by power.c around the wake repaint).
//
// The sysfs blank/unblank cycle tears down the panel's scan-out, and some
// drivers also reset the video mode -- which for the page-flipping display
// means losing the double-height virtual resolution, after which every
// FBIOPAN_DISPLAY to the second page fails and the panel stays black. So on
// wake the mode is put back explicitly (ACTIVATE_NOW | FORCE re-arms scan-out
// even when nothing changed) and the current page is re-panned. The plain
// fbdev fallback has its own mechanism: force_refresh makes its next flush
// issue the same re-arming FBIOPUT_VSCREENINFO.
//
// lv_linux_fbdev_set_force_refresh() must only be called on that fallback: on
// the page-flipping display it dereferences fbdev driver data the display does
// not have, and the SIGSEGV parks the process with the panel dark.
// ---------------------------------------------------------------------------

void display_wake_begin(lv_display_t *disp) {
	if (fb_fd >= 0) {
		fb_var.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
		if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &fb_var) < 0) {
			perror("fb: FBIOPUT_VSCREENINFO (wake)");
		}
		if (ioctl(fb_fd, FBIOPAN_DISPLAY, &fb_var) < 0) {
			perror("fb: FBIOPAN_DISPLAY (wake)");
		}
		return;
	}
	lv_linux_fbdev_set_force_refresh(disp, true);
}

void display_wake_end(lv_display_t *disp) {
	if (fb_fd >= 0) {
		return; // the wake repaint's own flush pans to the freshly drawn page
	}
	lv_linux_fbdev_set_force_refresh(disp, false);
}

// Frees the RAM pages once the refresh that may still be reading them is
// over: an async call runs at the top of the next lv_timer_handler pass.
static void free_rot_pages_cb(void *unused) {
	(void)unused;
	if (fb_rotated) {
		return; // turned back on in the meantime
	}
	free(rot_page);
	rot_page = NULL;
}

bool display_rotation_supported(void) { return fb_display != NULL; }

// The node lv_evdev_create() actually opened. Remembered because the emulator
// must open exactly that one instead of guessing: touch is event1 on this
// device, but the event0 fallback exists because that is not guaranteed.
static char fb_touch_node[32];

const char *panel_touch_device(void) { return fb_touch_node[0] ? fb_touch_node : NULL; }

// Hands the touch panel over and takes it back. While disabled the indev is
// not polled and stops reading the node, so events pile up unread in the
// kernel queue. Whatever re-enables it must wait a moment before trusting the
// first touch (see gearboyplay.c).
void panel_touch_enable(bool enabled) {
	if (fb_touch) {
		lv_indev_enable(fb_touch, enabled);
	}
}

bool display_get_rotated(void) { return fb_rotated; }

void display_set_rotated(bool rotated) {
	if (!fb_display || rotated == fb_rotated) {
		return;
	}

	if (rotated) {
		if (!rot_page) {
			rot_page = malloc(fb_page_size);
		}
		if (!rot_page) {
			fprintf(stderr, "fb: no memory for the rotated page (%zu bytes)\n", fb_page_size);
			return;
		}
		memset(rot_page, 0, fb_page_size);

		// Both framebuffer pages still hold the picture the right way up, so
		// they each owe a full frame.
		area_clear(&rot_frame_dirty);
		area_set_full(&rot_page_pending[0]);
		area_set_full(&rot_page_pending[1]);

		lv_display_set_buffers(fb_display, rot_page, NULL, (uint32_t)fb_page_size, LV_DISPLAY_RENDER_MODE_DIRECT);
		fb_rotated = true;
	} else {
		lv_display_set_buffers(fb_display, fb_mem, fb_mem + fb_page_size, (uint32_t)fb_page_size,
							   LV_DISPLAY_RENDER_MODE_DIRECT);
		fb_rotated = false;
		lv_async_call(free_rot_pages_cb, NULL);
	}

	// The touch panel turns with the picture: an inverted calibration maps
	// the top-left corner of the glass to the bottom-right of the screen.
	if (fb_touch) {
		if (fb_rotated) {
			lv_evdev_set_calibration(fb_touch, (int)fb_var.xres - 1, (int)fb_var.yres - 1, 0, 0);
		} else {
			lv_evdev_set_calibration(fb_touch, 0, 0, (int)fb_var.xres - 1, (int)fb_var.yres - 1);
		}
	}

	// Both framebuffer pages still hold the picture the other way up. One
	// invalidation is enough: the page bookkeeping above already has each
	// page owing a full frame, so the next two flips repaint them both. No
	// lv_refr_now() here, because this runs inside a settings event handler
	// and rendering from one is unsafe.
	lv_obj_invalidate(lv_screen_active());
	lv_obj_invalidate(lv_layer_top());
	lv_obj_invalidate(lv_layer_sys());

	printf("fb: rotation %s\n", fb_rotated ? "on (180 degrees)" : "off");
}

static lv_display_t *init_target_display(void) {
	// Tear-free page flipping when the framebuffer supports it...
	lv_display_t *disp = try_panned_display();

	// ...and LVGL's plain single-buffer driver when it does not.
	if (!disp) {
		fprintf(stderr, "fb: page flipping unavailable, falling back to the plain fbdev driver\n");
		disp = lv_linux_fbdev_create();
		if (!disp) {
			fprintf(stderr, "Error: Failed to create Linux framebuffer display\n");
			return NULL;
		}
		lv_linux_fbdev_set_file(disp, "/dev/fb0");
	}

	lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event1");
	if (touch) {
		snprintf(fb_touch_node, sizeof(fb_touch_node), "%s", "/dev/input/event1");
	} else {
		fprintf(stderr, "Warning: Failed to open /dev/input/event1. Trying event0...\n");
		touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event0");
		if (touch) {
			snprintf(fb_touch_node, sizeof(fb_touch_node), "%s", "/dev/input/event0");
		}
	}

	if (touch) {
		lv_indev_set_display(touch, disp);
		tune_kinetic_scroll(touch);
		fb_touch = touch;
		printf("Touch screen input driver successfully registered.\n");
	} else {
		fprintf(stderr, "Warning: No touch input device found.\n");
	}

	return disp;
}
#endif

int main(int argc, char **argv) {
	// Before anything at all -- before the signal guards, before the allocator
	// is tuned, before a display is opened. thttpd runs this same binary as the
	// transfer page's directory listing (see src/system/webcgi.h), and in that
	// role it is a small program that prints JSON and exits. None of the player
	// belongs on that path.
	if (webcgi_should_run(argc, argv)) {
		return webcgi_main();
	}

	// On this device dying means rebooting (see above).
	install_signal_guards();

	// How the allocator behaves matters more here than anywhere else. glibc
	// raises its mmap threshold on its own once it sees large blocks being
	// freed, after which a decoded cover comes out of the arena instead of its
	// own mapping -- and the arena never shrinks. Pinning both thresholds
	// keeps every large, short-lived allocation in a mapping that is handed
	// straight back to the kernel when it is freed.
	mallopt(M_MMAP_THRESHOLD, 96 * 1024);
	mallopt(M_TRIM_THRESHOLD, 128 * 1024);
	signal(SIGINT, sigint_handler);

	logging_init();

	printf("Starting sonix_player...\n");

	// After logging_init and not before it: both of these say what they did,
	// and anything printed before the log is holding output goes to whatever
	// the launcher left on stdout, which on this device is nothing.
	//
	// Nothing between here and there maps executable pages, so the set this
	// walks is the same one it would find a few lines earlier.
#ifndef HOST_BUILD
	set_oom_priority("-500");

	lock_code_pages();
#endif

#ifndef HOST_BUILD
	// Straight away, before anything is opened: how much RAM the binary takes
	// and how much is left for the rest of the device. Here and not up at
	// lock_code_pages() because nothing collects stderr before logging_init().
	//
	// Printed every time, because the player sets oom_score_adj to -500: when
	// RAM runs out the kernel kills something else, and if that is a system
	// service the device restarts with nothing in the player's log to say so.
	report_memory("startup");
#endif

	// Settings live on the UBIFS partition, the one writable place that is
	// always present: the rootfs is read-only and the card may be missing.
	// SONIX_CONFIG points it somewhere else for the host build and for testing.
	const char *config_file = getenv("SONIX_CONFIG");
#ifndef HOST_BUILD
	if (!config_file) {
		config_file = "/usr/data/device_config.ini";
	}
#endif
	config_init(config_file);

	// The reader's own file, beside the settings. SONIX_EBOOK_CONFIG moves it for
	// the host build the same way SONIX_CONFIG moves the other one; with neither
	// set on the host both stay unset and the reader keeps its state in memory
	// for the session.
	const char *ebook_file = getenv("SONIX_EBOOK_CONFIG");
#ifndef HOST_BUILD
	if (!ebook_file) {
		ebook_file = "/usr/data/ebook_config.ini";
	}
#endif
	config_store_init(config_ebook_store(), ebook_file);

	// A player updated in place has its reading positions in the main settings
	// file. They are moved once, here, and both files are written: leaving them
	// behind would reset every book on the shelf to its first page.
	if (config_store_move_section(config_main_store(), config_ebook_store(), "ebook") > 0) {
		config_store_save(config_ebook_store());
		config_save();
	}

	// The interface's own text, before any of it is built. SONIX_LANG_DIR points
	// it somewhere else for the host build, the same way SONIX_CONFIG does.
	const char *lang_dir = getenv("SONIX_LANG_DIR");
	lang_init(lang_dir && lang_dir[0] ? lang_dir : LANG_DIR);
	// The playback mode (repeat/shuffle) as it was left. Restored before the
	// GUI builds, so the repeat button is born showing the right icon.
	playlist_set_mode((playback_mode_t)config_get_int("player", "playback_mode", 0));
	// The equalizer's bands and switch, before any playback can start.
	eq_init();
	// Gapless playback, before anything can start: the engine reads this at
	// the end of every track.
	audio_set_gapless(config_get_int("audio", "gapless", 0) != 0);
	clock_init();
	adb_apply_saved_state();
	// The buttons on the headset cable: the kernel module starts disabled and
	// only remembers what was last written to it, so the configured state has
	// to be written back on every boot, on or off.
	headset_init();
	// What the three side keys do: the table is read once here, and the key
	// threads consult it on every press.
	keymap_init();
	// The streaming services' application keys, and Qobuz with them. Before
	// the GUI builds its pages, because the Qobuz page is drawn one way or
	// the other depending on whether the keys are there.
	streamkeys_init();
	qobuz_init();
	tidal_init();

	lv_init();

	// lv_init() seeds its generator with a literal (0x1234ABCD), so without
	// this the sequence is the same on every boot -- and lv_rand() picks the
	// track a shuffle starts on. The queue's own deal seeds from the clock
	// itself (playlist.c seed_once); only the starting point comes from here.
	//
	// After lv_init(), never before: it would overwrite this.
	lv_rand_set_seed((uint32_t)time(NULL) ^ (uint32_t)getpid());

	// The fonts, before anything can create a widget: every lv_font_t the
	// interface points at is filled in here, from /usr/resource/sonix/fonts through
	// FreeType -- the stock player's own mechanism. Without default.otf there
	// is no drawing so much as an error message, so that is fatal.
	if (!fonts_init()) {
#ifdef HOST_BUILD
		fprintf(stderr, "fonts: no default.otf in usr/resource/sonix/fonts or assets/fonts --\n"
						"       start the simulator from the folder that holds them\n");
		return 1;
#else
		park("fonts: /usr/resource/sonix/fonts/default.otf is missing or unreadable");
		return 1;
#endif
	}

	lv_tick_set_cb(custom_tick_get);

#ifdef HOST_BUILD
	printf("Initializing Host Build (SDL2 Simulation at %dx%d)...\n", SCREEN_WIDTH, SCREEN_HEIGHT);
	lv_display_t *disp = init_host_display();
	if (!disp) {
		return 1; // on a PC, exiting is the right thing to do
	}

	char music_path[1024];
	host_card_root(music_path, sizeof(music_path));
	fprintf(stderr, "card: %s\n", music_path);
#else
	printf("Initializing Target Build (Linux Framebuffer and EVDEV touch)...\n");
	lv_display_t *disp = init_target_display();
	if (!disp) {
		park("no framebuffer: cannot start the UI");
		return 1;
	}
#endif
	(void)disp;

	// Nothing in HiBy OS mounts the microSD on its own: mdev has no rule for
	// mmcblk, and sys_server -- the daemon that does the mounting -- only acts
	// on requests over its socket. The stock hiby_player is what sends those
	// requests, because it doubles as the hotplug manager. Replacing it means
	// taking over that job, which is what manage_mount does; the player still
	// stays out of the way if it finds the card already mounted.
	storage_config_t storage_cfg = {
		// Both NULL: the player probes the block devices and the mount-point
		// names the stock firmware uses and reports what it found in the log.
		.device = NULL,
		.mount_point = NULL,
#ifdef HOST_BUILD
		.manage_mount = false, // never touch the developer's own filesystem
#else
		.manage_mount = true,
#endif
	};

#ifdef HOST_BUILD
	// A PC has no battery here, so the status bar would always show an empty
	// shell. Point these at any two files to fake one while working on the UI:
	//   echo 80 > /tmp/cap; echo Charging > /tmp/stat
	//   SONIX_BATTERY_CAPACITY=/tmp/cap SONIX_BATTERY_STATUS=/tmp/stat ./sonix_player_host
	battery_config_t battery_cfg = {
		.battery_capacity_file = getenv("SONIX_BATTERY_CAPACITY"),
		.battery_status_file = getenv("SONIX_BATTERY_STATUS"),
	};
#else
	// Both NULL: the player enumerates /sys/class/power_supply and picks the
	// battery and the chargers out by their `type`, the same way the stock
	// firmware does. Hardcoding "battery" and "mp2731-charger" would work today
	// and break on the next revision.
	battery_config_t battery_cfg = {
		.battery_capacity_file = NULL,
		.battery_status_file = NULL,
	};
#endif

	system_config_t system_cfg = {
		.battery_cfg = &battery_cfg,
		.storage_cfg = &storage_cfg,
	};

	system_start_services(&system_cfg, post_gui_popup, NULL);

#ifndef HOST_BUILD
	logging_attach_sd(storage_sd_root());
	// Now that the log is on the card: whatever the kernel said about the
	// previous run, which is the only place a SIGKILL leaves a trace.
	logging_report_previous_run();
#endif

	gui_config_t gui_cfg = {
		.screen_width = SCREEN_WIDTH,
		.screen_height = SCREEN_HEIGHT,
		.top_bar_height = 44,
		.padding = 15,
#ifdef HOST_BUILD
		.sd_root_path = music_path,
#else
		// Resolved by the mount code above rather than hardcoded: /mnt/sd_0,
		// /data/mnt/sd_0 and /usr/data/mnt/sd_0 are the same directory reached
		// through different symlinks, and which one is real varies by firmware.
		.sd_root_path = storage_sd_root(),
#endif
	};

	// Artwork thumbnails are kept on the card, next to the music, so that
	// browsing a library only ever decodes each cover once -- the same trick
	// the stock player uses with its own .hiby/.../cache folder.
	cover_set_cache_dir(gui_cfg.sd_root_path);

	// The shapes of tracks, for the player's alternative layout. The worker
	// sleeps until a page asks for one, so starting it costs a thread and
	// nothing else.
	waveform_set_cache_dir(gui_cfg.sd_root_path);
	waveform_start();

	// User playlists: plain .m3u files in <card>/Playlist.
	playlists_init(gui_cfg.sd_root_path);

	// And the tree "play by folder" is allowed to walk: the same card, so the
	// walk cannot wander off it.
	playlist_set_card_root(gui_cfg.sd_root_path);

	// MSEB presets: one small file each in <card>/.local/MSEB.
	mseb_presets_set_dir(gui_cfg.sd_root_path);
	eq_presets_set_dir(gui_cfg.sd_root_path);
	peq_presets_set_dir(gui_cfg.sd_root_path);

	// The music index, beside the thumbnails. Same schema as the stock
	// player's, so the two can read each other's database.
	if (gui_cfg.sd_root_path && gui_cfg.sd_root_path[0]) {
		// A card that cannot be written to keeps its databases in /tmp instead.
		// They do not survive a reboot, so the library has to be scanned again
		// each time -- but with no library.db at all, library_scan_start()
		// gives up at its first line and the scan button does nothing, for any
		// database and with no message. The thumbnail cache falls back the
		// same way.
		const char *db_root = gui_cfg.sd_root_path;
		if (!storage_sd_writable()) {
			db_root = "/tmp";
			mkdir("/tmp/.local", 0755);
			fprintf(stderr, "main: the card is read-only, so the databases live in /tmp for this session\n");
		}

		// Before the index opens, because the collation it registers reads it.
		library_set_skip_articles(config_get_int("player", "skip_articles", 1) != 0);

		char db_path[600];
		snprintf(db_path, sizeof(db_path), "%s/.local/library.db", db_root);
		library_open(db_path);
		// The same file the app downloads: one index, served as it is.
		sonixlink_set_db_path(db_path);

		// The thumbnails the player has already made for its own lists: the app
		// downloads that same file rather than having a second one built for it.
		char thumbs_path[600];
		snprintf(thumbs_path, sizeof(thumbs_path), "%s/.local/thumbnails", db_root);
		sonixlink_set_thumbs_path(thumbs_path);

		// The audiobook index lives beside it, in its own file.
		snprintf(db_path, sizeof(db_path), "%s/.local/audiobooks.db", db_root);
		audiobookdb_open(db_path);

		// And the radio's own: the starred stations and the last few played.
		radio_store_open(db_root);
	}

	// TLS, so an https:// station can be opened at all. Warmed up here rather
	// than on first use for two reasons: the first call parses a ~200 KB
	// certificate bundle, which on this CPU is long enough to be felt if it
	// happens while a list is being drawn, and the log line it prints says
	// which bundle was found -- which is the first thing to look at when
	// https stations fail.
	tls_set_card_root(gui_cfg.sd_root_path);
	// Where downloaded stream tracks go. The real card is used -- the same
	// root the Qobuz page and the card watcher use -- and not
	// gui_cfg.sd_root_path, which on the simulator is a different folder: the
	// cache and the code that cleans it must agree on one root.
	const char *card_root = storage_sd_root();
	qobuzcache_set_root(card_root && card_root[0] ? card_root : gui_cfg.sd_root_path);
	tidalcache_set_root(card_root && card_root[0] ? card_root : gui_cfg.sd_root_path);
	podcastcache_set_root(card_root && card_root[0] ? card_root : gui_cfg.sd_root_path);
	podcastsubs_set_root(card_root && card_root[0] ? card_root : gui_cfg.sd_root_path);
	// And where a track arriving over DLNA lands: same card, same reason.
	dlna_set_root(card_root && card_root[0] ? card_root : gui_cfg.sd_root_path);
	// And where the emulator looks for Games/GB and Games/GBC. Unlike the
	// lines above this uses gui_cfg.sd_root_path and not storage_sd_root():
	// those write a cache, which belongs on the real card, while games are
	// the user's own files and live where the user's music lives. On the
	// device the two are the same (see how gui_cfg is filled in); on the
	// simulator they are not, which is what makes the page testable without
	// the device.
	gbdb_set_root(gui_cfg.sd_root_path);

	// Rows pointing into the stream caches are dropped once at boot: a cache
	// file may be gone, and starring a streamed track goes to the account
	// rather than to the local database.
	library_forget_under(qobuzcache_dir());
	library_forget_under(tidalcache_dir());
	if (!tls_available()) {
		fprintf(stderr, "tls: unavailable; https radio stations cannot be opened\n");
	}

	// The radios, before the interface: both read their switch out of the
	// config here, and the Wireless pages are built from those switches, so a
	// Wi-Fi left on is drawn as on from the first frame. Each starts a worker
	// and returns at once; if a switch was on, the bring-up happens on that
	// worker while the interface carries on drawing.
	wifi_init();
	bluetooth_init();
	// Its worker starts here but stays idle: no socket is opened until the
	// switch is on, which the same config read decides.
	sonixlink_init();

	gui_init(&gui_cfg);

	// "Rotate screen", as it was left. After gui_init so there are screens to
	// redraw, and before the first frame the user sees.
	if (config_get_int("screen", "rotate_180", 0)) {
		display_set_rotated(true);
	}

	// The USB-C port, allowed to take a peripheral. Before the volume restore
	// below, so that a DAC already plugged in at boot is found and the level
	// lands on it rather than on a CS43198 nobody is listening to.
	sleeptimer_init();
	usbaudio_init();
	usbaudio_poll();

	// "Remember volume": the saved levels replace audio_init's default. Every
	// output's, not only the one in use, because which output is in force at
	// boot depends on what is plugged in or connected: loading one level would
	// start another output at the wrong level and then write that level over
	// the right one.
	//
	// The two sockets fall back to the 3.5 mm level and USB-C to whatever the
	// jack was left at: a player updating from a build that kept one level for
	// all of them comes up exactly where it was, and the levels part company
	// from the first time each output is used.
	if (config_get_int("player", "remember_volume", 0)) {
		int wired = (int)config_get_int("player", "volume", get_volume_percent());
		int balanced = (int)config_get_int("player", "volume_balanced", wired);
		int usb = (int)config_get_int("player", "volume_usb", wired);
		int bt = (int)config_get_int("wireless", "bt_volume", wired);
		volume_profile_init(wired, bt);
		volume_profile_set_level(VOLUME_OUTPUT_BALANCED, balanced);
		volume_profile_set_level(VOLUME_OUTPUT_USB, usb);

		// Which one the player is actually on right now. The routing watcher
		// would settle it within a second anyway, but a second of the wrong
		// loudness in a pair of headphones is exactly the second that matters.
		int level = wired;
		if (bluetooth_audio_active()) {
			level = bt;
			volume_profile_set_output(VOLUME_OUTPUT_BLUETOOTH);
		} else if (usbaudio_active()) {
			level = usb;
			volume_profile_set_output(VOLUME_OUTPUT_USB);
		} else if (headphone_jack_state() == JACK_BALANCED) {
			level = balanced;
			volume_profile_set_output(VOLUME_OUTPUT_BALANCED);
		}
		set_volume_percent(level);
	}

	// "Remember track", once the first frames are on screen (see the timer).
	if (config_get_int("player", "remember_track", 0)) {
		lv_timer_create(restore_track_timer_cb, 600, NULL);
	}


#ifdef HOST_BUILD
	lv_timer_create(host_shot_poll_cb, 200, NULL); // see host_shot_poll_cb
#endif

	// The media player bluez shows to the headphones. After gui_init, because
	// the transport commands it receives are sent onto the GUI thread; it does
	// nothing at all until Bluetooth is switched on.
	btplayer_init();

	// The status LED: aqua from power-on, exactly like the stock firmware.
	led_init();

#ifndef HOST_BUILD
	// USB mass storage: hand the card to a PC when one is attached, exactly
	// like the stock firmware (see usb.h).
	usb_start(storage_sd_device(), storage_sd_root());
#endif

	// power / display idle management (backlight, screen-off, SoC suspend).
	// On the host build every device path is left NULL so nothing touches the
	// developer's own backlight/framebuffer/suspend.
	power_config_t power_cfg = {
#ifdef HOST_BUILD
		.brightness_path = NULL,
		.max_brightness_path = NULL,
		.blank_path = NULL,
#else
		.brightness_path = "/sys/class/backlight/backlight_pwm0/brightness",
		.max_brightness_path = "/sys/class/backlight/backlight_pwm0/max_brightness",
		// Screen on/off via the framebuffer blank node -- the same mechanism
		// Rockbox's HiBy port uses. Writing here powers the backlight, panel,
		// and touch controller together, and is reliably reversible.
		.blank_path = "/sys/class/graphics/fb0/blank",
		// The suspend-to-RAM prototype writes "mem" here. NULL on the host
		// build like every other device path, so nothing touches the
		// developer's own machine.
		.power_state_path = "/sys/power/state",
#endif
		.brightness = -1, // adopt the panel's current level as the on-brightness
		.screen_off_enabled = false,
		// Off unless asked for in Settings. Blanking this panel powers the
		// touch controller down with it, so the screen cannot be woken by
		// touching it -- only by the power key. A timer that traps the user
		// like that is not something to have on by default.
		.screen_off_timeout_ms = 60000,
	};
	// Whatever was chosen in Settings last time.
	power_cfg.screen_off_timeout_ms = (uint32_t)config_get_int("power", "screen_off_seconds", 0) * 1000;
	power_cfg.screen_off_enabled = (power_cfg.screen_off_timeout_ms > 0);
	// The brightness picked in Settings > Screen; -1 (not set yet) keeps
	// the adopt-the-panel's-level behaviour above.
	power_cfg.brightness = config_get_int("screen", "brightness", -1);
	// Double-tap wake as it was left: the touch controller's gesture switch
	// must be told again on every boot.
	power_set_double_tap_wake(config_get_int("screen", "double_tap_wake", 0) != 0);

	power_init(&power_cfg, disp);

	// Charge limit, automatic shutdown and standby, as they were left. The
	// screen timeout is the Screen page's setting and comes from there.
	powersettings_apply();
	settings_apply_screen_off();

#ifndef HOST_BUILD
	// Bluetooth off. A firmware whose /etc/init.d/S80_bt_init has not been
	// replaced powers the radio up at every boot -- rfkill on, patchram,
	// bluetoothd, bluealsa -- and with the switch off nothing in this player
	// talks to it, so it is current spent on nothing. Done on a thread of its
	// own because it waits for the daemons to go, and the interface must not
	// stall for it.
	//
	// Not when Bluetooth is switched on in Wireless: the stack that is already
	// up is the one bluetooth_init() is about to want.
	if (config_get_int("power", "bluetooth_off", 1) && !config_get_int("wireless", "bluetooth", 0)) {
		pthread_t bt_thread;
		if (pthread_create(&bt_thread, NULL, bluetooth_off_thread, NULL) == 0) {
			pthread_detach(bt_thread);
		}
	}
#endif

	// The main event loop: run whatever LVGL timers are due, then sleep until
	// the next one is.
	//
	// The sleep is capped, and that cap is not a nicety. lv_timer_handler()
	// returns LV_NO_TIMER_READY (0xFFFFFFFF) when NO timer is scheduled at
	// all -- every one of them paused, which is exactly what the standby path
	// arranges. Multiplied by 1000 in 32-bit arithmetic that wraps to about
	// 2295 seconds, so uncapped the player would go deaf and blind for
	// thirty-eight minutes: the panel frozen on whatever it last drew, touches
	// unread (the input device is polled by a timer too), the clock stopped.
	//
	// A quarter of a second is the ceiling because that is the standby budget
	// this player runs to: four wakeups a second with nothing to do.
	//
	// The wait is a poll() on the gui_post wake descriptor rather than a
	// usleep() (see gui.c), so a thread that posts wakes the loop at once and
	// nothing wakes it while the queue is empty. With no descriptor (eventfd
	// not opened) poll() on a negative fd is exactly a timed sleep, and the
	// bridge timer polls fast in that case.
	watchdog_start();

	printf("Entering main event loop...\n");
	while (running) {
		ui_heartbeat++;
		uint32_t time_till_next = lv_timer_handler();
		if (time_till_next > MAIN_LOOP_MAX_SLEEP_MS) {
			time_till_next = MAIN_LOOP_MAX_SLEEP_MS; // also catches LV_NO_TIMER_READY
		}

		struct pollfd wake = {.fd = gui_post_wake_fd(), .events = POLLIN};
		if (poll(&wake, 1, (int)time_till_next) > 0) {
			// Something was posted: drain the queue now instead of waiting
			// for the bridge's own timer to come round.
			gui_post_service();
		}
	}

	printf("\n");

	return 0;
}
