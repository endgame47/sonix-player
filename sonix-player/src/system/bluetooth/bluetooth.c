#include "bluetooth.h"

#include "src/system/core/respath.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <strings.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "src/system/bluetooth/airpods.h"
#include "src/system/audio/audio.h"
#include "src/system/bluetooth/btstack.h"
#include "src/system/bluetooth/btvolume.h"
#include "src/system/core/config.h"
#include "src/system/device/power.h"
#include "src/system/device/sysserver.h" // only for the notification socket now
#include "src/system/core/utils.h"

// The pieces of the firmware this player uses, and nothing else. The bluez-tools
// (bt-adapter, bt-device, bt-agent), hciconfig, bluealsa-cli and the
// bt_init/bt_resume/bt_suspend scripts are not used: everything they do is done
// here and in btstack.c, over D-Bus and over the HCI ioctls.
#define BT_TRANSPORT_BIN "/usr/bin/brcm_patchram_plus"
#define BT_TRANSPORT_TTY "/dev/ttyS0"
#define BT_FIRMWARE_DIR "/lib/firmware/bt_bcm"
#define BT_FIRMWARE_AW "BCM4343A1_001.002.009.0122.0538.hcd" // chipvendor 0x81
#define BT_FIRMWARE_AP "BCM4343A1_001.002.009.1010.1030.hcd" // anything else
#define BT_CHIPVENDOR "/sys/devices/platform/md_ingenic,mmc.0/mmc_host/mmc0/mmc0:0001/mmc0:0001:2/chipvendor"
#define BT_RFKILL "/sys/class/rfkill/rfkill0/state"

#define BLUETOOTHD_BIN "/usr/libexec/bluetooth/bluetoothd"
#define BLUEALSA_BIN "/usr/bin/bluealsa"

#define DBUS_DAEMON_BIN "/usr/bin/dbus-daemon"
#define DBUS_SYSTEM_CONF "/usr/share/dbus-1/system.conf"
#define DBUS_UUIDGEN_BIN "/usr/bin/dbus-uuidgen"
#define DBUS_MACHINE_ID "/var/lib/dbus/machine-id"

// The address the firmware generated once and wrote down. It is the device's
// Bluetooth identity: change it and every pair of headphones on the shelf sees
// a device it has never met, and the link keys they hold stop matching.
#define BT_MAC_FILE "/usr/data/bt_macaddr.txt"

// What the address is derived from when that file is not there. The Wi-Fi
// radio has one of its own, given to this unit at the factory, and it is the
// only thing on the board that is already unique to it.
#define BT_WIFI_MAC_FILE "/sys/class/net/wlan0/address"

// Where the two daemons keep their state, and why nothing here has to create
// it. /var is part of the read-only squashfs on this firmware -- no remount in
// inittab, no bind mount anywhere -- so /var/lib/bluetooth cannot work. The
// firmware's bluetoothd is patched to store pairings, link keys and trusted
// flags under /usr/data/bluetooth, on the writable ubifs partition, and its
// bluealsa keeps per-device volume in /usr/data/bluealsa. Both daemons create
// those directories themselves.
//
// The bluealsa built for this player is pointed at the same /usr/data/bluealsa
// (see firmware/build/build-daemon.sh): upstream would compute
// /usr/var/lib/bluealsa, which on a read-only rootfs cannot be created at all.

// The name the firmware announces, when it has been given one.
#define BT_NAME_FILE RESOURCE_DIR "/bt_name"

// The paired-list poll. It is a backstop, not the mechanism: bluez says what
// changed over D-Bus the moment it happens, and btstack rings the change
// callback, so this only catches anything a lost signal would have missed.
#define BT_POLL_MS 15000

// The same with the screen off. Connections and commands do not come through
// here -- they arrive as jobs and wake the thread immediately.
#define BT_POLL_STANDBY_MS 120000

// With Bluetooth off there is nothing to poll at all.
#define BT_IDLE_WAIT_MS 300000

// How long each step of the bring-up is given before it is called failed. Every
// one of them is a wait on a condition, never a sleep: the numbers here are
// deadlines, not delays, and a healthy device passes them in a fraction of the
// time.
#define WAIT_TRANSPORT_MS 12000	 // the chip's firmware over a 3 Mbaud UART
#define WAIT_BLUETOOTHD_MS 8000	 // to claim org.bluez and find the controller
#define WAIT_BLUEALSA_MS 6000	 // to claim org.bluealsa
#define WAIT_STOCK_BRINGUP_MS 20000 // if the firmware's own is still in flight

// How long a daemon is given to take its SIGTERM before it is killed.
#define STOP_GRACE_MS 1500

typedef enum {
	JOB_NONE = 0,
	JOB_POWER, // arg_int: 1 on, 0 park off, -1 deliberate off
	JOB_SCAN,
	JOB_SCAN_STOP,
	JOB_REFRESH,
	JOB_PAIR,
	JOB_CONNECT,
	JOB_DISCONNECT,
	JOB_FORGET,
	JOB_CODECS,		  // re-read what the connected sink offers
	JOB_LDAC_QUALITY, // restart bluealsa so it reads the new --ldac-quality
	JOB_DISCOVERABLE, // arg_int: 1 visible to everyone, 0 not
	// The name the adapter answers with. Carried in g_local_name rather than in
	// the job: a name is longer than `text` holds.
	JOB_SET_NAME,
	JOB_SET_CODEC, // text: the codec to switch to
	JOB_VOLUME,	   // arg_int: percent, for AVRCP absolute volume
	JOB_RX_INFO,   // re-read what a device streaming to this one is sending
	JOB_RX_CODEC,  // text: the codec to move the incoming stream onto
	JOB_MEDIA,	   // text: an AVRCP member to send to that device
} job_type_t;

// `text` is a codec name for JOB_SET_CODEC and an AVRCP member for JOB_MEDIA.
// "Previous" is the longest of the four and fits with room to spare.
typedef struct {
	job_type_t type;
	char mac[BT_MAC_MAX];
	char text[BT_CODEC_MAX];
	int arg_int;
} job_t;

#define JOB_QUEUE_LEN 12

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static bool worker_running;

static job_t queue[JOB_QUEUE_LEN];
static int queue_head, queue_count;

static bool g_enabled;
static bool g_busy;
static bool g_scanning;
static bool g_discoverable; // what the Bluetooth list page asked for, last
static int g_seen_count;   // device objects bluez held at the last refresh
static int g_hidden_count; // ...of which nameless and not audio gear
static bt_state_t g_state = BT_STATE_OFF;
static uint32_t g_serial;

static bt_device_t g_paired[BT_MAX_DEVICES];
static int g_paired_count;
static bt_device_t g_found[BT_MAX_DEVICES];
static int g_found_count;

static bt_op_t g_op = BT_OP_IDLE;
static char g_op_name[BT_NAME_MAX];

static char g_local_name[BT_NAME_MAX] = "HiBy Music";

// A2DP. There is deliberately no on/off switch here, as in the stock player:
// connected headphones are the output, and a switch that could send the music
// back to the jack while they are still worn causes confusion, not choice.
static bool g_volume_sync;	  // the player's volume drives the headphones'
static bool g_a2dp_connected; // a bluealsa PCM exists: the only truthful source
static char g_a2dp_mac[BT_MAC_MAX];

// The last answer bluez gave about the A2DP streams on the connected sink, and
// when. Read on this thread and never on the playback thread: the question
// costs a GetManagedObjects, and a blocking D-Bus call between two writes to
// the card is the very stall this is meant to explain.
static char g_a2dp_streams[192];
static uint32_t g_a2dp_streams_at;
static bool g_a2dp_streams_wanted;
static char g_codecs[BT_MAX_CODECS][BT_CODEC_MAX];
static int g_codec_count;
static char g_codec_selected[BT_CODEC_MAX];
static int g_pending_volume = -1; // coalesces a held volume key into one write

// What bluealsa says the stream arriving from a device on the other side is
// carrying. Filled on the worker, read by the receiver page and by the capture
// loop, neither of which may block on D-Bus.
static bt_stream_t g_rx_stream;
static unsigned g_rx_serial; // moves every time the worker reads the line above

static void post_job(const job_t *job);

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static bool slurp(const char *path, char *buf, size_t size) {
	buf[0] = '\0';
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	size_t got = fread(buf, 1, size - 1, f);
	fclose(f);
	buf[got] = '\0';
	return got > 0;
}

static bool write_file(const char *path, const char *text) {
	int fd = open(path, O_WRONLY);
	if (fd < 0) {
		return false;
	}
	ssize_t written = write(fd, text, strlen(text));
	close(fd);
	return written == (ssize_t)strlen(text);
}

// Copies a fixed-width field into another. By length rather than with %s: the
// compiler cannot see that a char array inside a struct is terminated, and warns
// that the copy could run past it.
static void copy_field(char *dst, size_t dst_size, const char *src, size_t src_size) {
	size_t length = strnlen(src, src_size - 1);
	if (length >= dst_size) {
		length = dst_size - 1;
	}
	memcpy(dst, src, length);
	dst[length] = '\0';
}

static void sleep_ms(int ms) {
	struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

// ---------------------------------------------------------------------------
// processes
// ---------------------------------------------------------------------------

// A process that has exited but not been collected yet keeps its /proc entry,
// name and all, while owning no memory and answering nothing. Counting one as
// running would say the stack is up when it is gone, and would make a daemon
// that took its SIGTERM look like one that refused it.
static bool process_is_zombie(const char *pid) {
	char path[320], line[512] = "";
	snprintf(path, sizeof(path), "/proc/%s/stat", pid);
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	size_t got = fread(line, 1, sizeof(line) - 1, f);
	fclose(f);
	line[got] = '\0';

	// The state is the field after the name, and the name is in brackets and
	// may itself contain one -- so the last bracket, not the first.
	const char *close = strrchr(line, ')');
	if (!close) {
		return false;
	}
	close++;
	while (*close == ' ') {
		close++;
	}
	return *close == 'Z';
}

// Whether /proc/<pid>/comm names this program.
//
// The kernel stores a command name in fifteen characters plus the terminator,
// so anything longer arrives cut: brcm_patchram_plus reads back as
// "brcm_patchram_p". A plain strcmp against the full name therefore never
// matches it, and a kill by that name would silently do nothing.
#define COMM_MAX 15

static bool comm_matches(const char *comm, const char *name) {
	size_t len = strlen(name);
	if (len > COMM_MAX) {
		len = COMM_MAX;
	}
	return strncmp(comm, name, len) == 0 && comm[len] == '\0';
}

// Walks /proc calling `visit` for every live process whose comm is `name`.
static void for_each_process(const char *name, void (*visit)(pid_t pid, void *user), void *user) {
	DIR *proc = opendir("/proc");
	if (!proc) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(proc)) != NULL) {
		if (de->d_name[0] < '0' || de->d_name[0] > '9') {
			continue;
		}
		char path[320], comm[64] = "";
		snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
		FILE *f = fopen(path, "r");
		if (!f) {
			continue;
		}
		if (fgets(comm, sizeof(comm), f)) {
			comm[strcspn(comm, "\r\n")] = '\0';
		}
		fclose(f);
		if (!comm_matches(comm, name) || process_is_zombie(de->d_name)) {
			continue;
		}
		visit((pid_t)strtol(de->d_name, NULL, 10), user);
	}
	closedir(proc);
}

static void count_one(pid_t pid, void *user) {
	(void)pid;
	(*(int *)user)++;
}

static bool process_running(const char *name) {
	int found = 0;
	for_each_process(name, count_one, &found);
	return found > 0;
}

// Kilobytes of resident memory held by the processes with this name. Only for
// the log line: it is what makes the saving checkable on the device instead of
// asserted here.
static void add_rss(pid_t pid, void *user) {
	char path[320];
	snprintf(path, sizeof(path), "/proc/%d/statm", (int)pid);
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	long pages_total = 0, pages_resident = 0;
	if (fscanf(f, "%ld %ld", &pages_total, &pages_resident) == 2) {
		*(long *)user += pages_resident * (sysconf(_SC_PAGESIZE) / 1024);
	}
	fclose(f);
}

static long process_rss_kb(const char *name) {
	long total = 0;
	for_each_process(name, add_rss, &total);
	return total;
}

static void send_signal(pid_t pid, void *user) {
	int sig = *(int *)user;
	if (pid > 1) {
		kill(pid, sig);
	}
}

// SIGTERM every process with this name, wait for it to be gone, then SIGKILL
// whatever is left and wait again. Returns whether the name is gone when it
// finishes -- checked and not assumed, because a daemon left running with the
// interface convinced it had been stopped is exactly how Bluetooth ends up
// refusing to come back on.
//
// By name and not by a stored pid on purpose: bluetoothd re-execs itself and
// dbus can activate it, so a pid recorded at launch is not always the process
// that has to go.
static bool stop_process(const char *name) {
	if (!process_running(name)) {
		return true;
	}

	int sig = SIGTERM;
	for_each_process(name, send_signal, &sig);
	for (int waited = 0; waited < STOP_GRACE_MS && process_running(name); waited += 50) {
		sleep_ms(50);
	}
	if (!process_running(name)) {
		return true;
	}

	fprintf(stderr, "bluetooth: %s ignored SIGTERM, killing it\n", name);
	sig = SIGKILL;
	for_each_process(name, send_signal, &sig);
	// SIGKILL is not refused, but it is not instant either: a process blocked in
	// the kernel -- on the Bluetooth UART, for one -- dies when that call
	// returns and not before.
	for (int waited = 0; waited < 2000 && process_running(name); waited += 50) {
		sleep_ms(50);
	}
	return !process_running(name);
}

// Starts a daemon and lets go of it. The double fork is what keeps it out of
// this process's children: the player must never have to reap a daemon it is
// not watching, and a daemon must never die because the player restarted.
//
// stdout and stderr are deliberately inherited -- that is how bluetoothd's and
// bluealsa's own complaints reach the player's log, which is the only place
// anybody ever reads them from on this device.
static bool spawn_daemon(const char *path, char *const argv[]) {
	pid_t pid = fork();
	if (pid < 0) {
		fprintf(stderr, "bluetooth: fork for %s failed: %s\n", path, strerror(errno));
		return false;
	}
	if (pid == 0) {
		if (fork() != 0) {
			_exit(0); // the middle process leaves at once; init adopts the daemon
		}
		setsid();
		int devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			close(devnull);
		}
		execv(path, argv);
		_exit(127);
	}

	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		// only reaping the middle process, which exits immediately
	}
	return true;
}

// The one place a helper is still run and waited for. Only ever with a fixed
// argument list of this file's own making -- dbus-uuidgen and modprobe -- never
// with anything that came off the air.
static int run_and_wait(const char *path, char *const argv[], int timeout_ms) {
	pid_t pid = fork();
	if (pid < 0) {
		return -1;
	}
	if (pid == 0) {
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			close(devnull);
		}
		execv(path, argv);
		_exit(127);
	}

	int status = 0;
	for (int waited = 0;; waited += 25) {
		pid_t answered = waitpid(pid, &status, WNOHANG);
		if (answered == pid) {
			return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		}
		if (answered < 0 && errno != EINTR) {
			return -1;
		}
		if (waited >= timeout_ms) {
			kill(pid, SIGKILL);
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
			}
			return -1;
		}
		sleep_ms(25);
	}
}

// ---------------------------------------------------------------------------
// the HCI transport, without hciconfig
//
// `hciconfig hci0 up` is two ioctls on an HCI socket and nothing else, so it is
// done here instead of forked. The constants are kernel UAPI and have not moved
// since the interface existed; they are spelled out rather than pulled from
// <bluetooth/bluetooth.h> because this rootfs has the library but not its
// headers, and the cross build has neither.
// ---------------------------------------------------------------------------

#define BT_AF_BLUETOOTH 31
#define BT_PROTO_HCI 1
#define BT_HCIDEVUP _IOW('H', 201, int)
#define BT_HCIDEVDOWN _IOW('H', 202, int)
#define BT_HCI_DEVICE 0

static int hci_ioctl(unsigned long request) {
	int fd = socket(BT_AF_BLUETOOTH, SOCK_RAW, BT_PROTO_HCI);
	if (fd < 0) {
		return -1; // no Bluetooth in this kernel at all
	}
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	int rc = ioctl(fd, request, BT_HCI_DEVICE);
	int kept = errno;
	close(fd);
	errno = kept;
	return rc;
}

// True when hci0 is up, or was already. EALREADY is the answer on a controller
// that is running, and it is not an error.
static bool hci_up(void) {
	if (hci_ioctl(BT_HCIDEVUP) == 0) {
		return true;
	}
	return errno == EALREADY;
}

static void hci_down(void) { (void)hci_ioctl(BT_HCIDEVDOWN); }

// ---------------------------------------------------------------------------
// uinput
//
// The media keys on a pair of headphones are not audio: bluez receives them over
// AVRCP and re-presents them as an input device, which it creates through
// /dev/uinput. That node does not exist on this rootfs -- nothing here ever
// wanted one -- and without it bluez has nowhere to put the keys, so it drops
// them and the buttons do nothing at all.
//
// Run before bluetoothd starts, so the node is there when the first pair of
// headphones connects.
// ---------------------------------------------------------------------------

static void ensure_uinput(void) {
	if (access("/dev/uinput", F_OK) == 0) {
		return;
	}

	char *argv[3] = {(char *)"modprobe", (char *)"uinput", NULL};
	run_and_wait("/sbin/modprobe", argv, 4000);
	if (access("/dev/uinput", F_OK) == 0) {
		fprintf(stderr, "bluetooth: /dev/uinput arrived with the module\n");
		return;
	}

	// Built in rather than loadable: only the node is missing. /proc/misc names
	// the minor the driver was given, and the major of a misc device is 10.
	FILE *f = fopen("/proc/misc", "r");
	if (!f) {
		return;
	}
	int minor = -1;
	char line[128];
	while (fgets(line, sizeof(line), f)) {
		int n = 0;
		char name[64];
		if (sscanf(line, "%d %63s", &n, name) == 2 && strcmp(name, "uinput") == 0) {
			minor = n;
			break;
		}
	}
	fclose(f);

	if (minor < 0) {
		fprintf(stderr, "bluetooth: no uinput in /proc/misc -- the headphone buttons cannot arrive\n");
		return;
	}
	if (mknod("/dev/uinput", S_IFCHR | 0600, makedev(10, (unsigned)minor)) != 0) {
		fprintf(stderr, "bluetooth: /dev/uinput (10,%d) not created: %s\n", minor, strerror(errno));
		return;
	}
	fprintf(stderr, "bluetooth: /dev/uinput created (10,%d)\n", minor);
}

// ---------------------------------------------------------------------------
// bringing the stack up
//
// Every step tests whether it is already done, waits on the thing that says it
// finished, and says so in the log. Nothing here sleeps for a fixed time and
// nothing here goes through a shell: the device address and the firmware name
// are arguments to execv, not words in a command line.
// ---------------------------------------------------------------------------

static void ensure_dbus(void) {
	if (process_running("dbus-daemon")) {
		return;
	}
	// A stale pid file stops dbus-daemon from starting at all, and it is stale
	// exactly when nothing is running -- which is what was just established.
	unlink("/var/run/messagebus.pid");
	mkdir("/var/lib/dbus", 0755);

	char id[64];
	if (!slurp(DBUS_MACHINE_ID, id, sizeof(id))) {
		// Written once and kept. The firmware's scripts regenerate it on every
		// Bluetooth start, so two runs of the same device look like two different
		// machines to anything that recorded it.
		char *argv[3] = {(char *)"dbus-uuidgen", (char *)"--ensure=" DBUS_MACHINE_ID, NULL};
		run_and_wait(DBUS_UUIDGEN_BIN, argv, 4000);
	}

	char *argv[3] = {(char *)"dbus-daemon", (char *)"--config-file=" DBUS_SYSTEM_CONF, NULL};
	spawn_daemon(DBUS_DAEMON_BIN, argv);
	for (int waited = 0; waited < 3000 && !process_running("dbus-daemon"); waited += 50) {
		sleep_ms(50);
	}
	fprintf(stderr, "bluetooth: dbus-daemon %s\n", process_running("dbus-daemon") ? "started" : "did NOT start");
}

// The firmware's own bring-up, if it is still in flight, is waited for and not
// raced. /etc/init.d/S80_bt_init runs /usr/bin/bt_init in the BACKGROUND at
// every boot, and a player that reaches this point inside that window finds hci0
// not answering yet and would start a SECOND brcm_patchram_plus on /dev/ttyS0.
// Two processes driving one HCI transport is not a state anything recovers from
// by reconnecting.
//
// The delivered firmware disables that script, but a device flashed with the
// stock init, or one where the file was restored, must not be broken by it.
static void wait_out_stock_bringup(void) {
	if (!process_running("brcm_patchram_plus") && !process_running("bt_init")) {
		return;
	}
	fprintf(stderr, "bluetooth: the stock bring-up is running; waiting for it instead of doubling it\n");
	for (int waited = 0; waited < WAIT_STOCK_BRINGUP_MS; waited += 200) {
		if (hci_up()) {
			return;
		}
		sleep_ms(200);
	}
}

static void firmware_name(char *out, size_t size) {
	char vendor[32];
	// 0x81 is the AW-NB372SM; anything else, and a card with no chipvendor node
	// at all, takes the AP6212 blob -- which is the same choice the firmware's
	// script makes.
	if (slurp(BT_CHIPVENDOR, vendor, sizeof(vendor)) && strncmp(vendor, "0x81", 4) != 0) {
		snprintf(out, size, BT_FIRMWARE_DIR "/" BT_FIRMWARE_AP);
	} else {
		snprintf(out, size, BT_FIRMWARE_DIR "/" BT_FIRMWARE_AW);
	}
}

// AA:BB:CC:DD:EE:FF and nothing else. A file of the right length holding
// something else is no more usable than an empty one.
static bool address_is_shaped(const char *text) {
	if (strlen(text) != 17) {
		return false;
	}
	for (int i = 0; i < 17; i++) {
		if (i % 3 == 2) {
			if (text[i] != ':') {
				return false;
			}
		} else if (!isxdigit((unsigned char)text[i])) {
			return false;
		}
	}
	return true;
}

static void format_address(const unsigned char *bytes, char *out, size_t size) {
	snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X", bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
}

// How long the Wi-Fi interface is given to appear before the address is made
// out of nothing instead. It is the difference between an address this device
// will arrive at again after the next factory reset and one it will not, and it
// is paid once in the life of the device.
#define WAIT_WIFI_MAC_MS 3000

// Six bytes taken from the Wi-Fi radio's address, with the last one moved on so
// the two radios of this device are not the same station, and with the
// locally-administered bit set -- which is what says the address was not bought
// from the IEEE and keeps it clear of any real vendor's range.
//
// The Wi-Fi address and not a random one because it is the only number on this
// board that is already unique to this unit and outlives everything: it is not
// in /usr/data, so a factory reset cannot take it. Derived from it, the
// Bluetooth address comes out the same after every reset, and headphones paired
// before one still recognise the player after it.
static bool address_from_wifi(char *out, size_t size) {
	char raw[64];
	// The radios come up on their own threads and this one can win. Worth
	// waiting for: the alternative is not a later Wi-Fi address, it is a
	// permanent random one written down in its place.
	for (int waited = 0; !slurp(BT_WIFI_MAC_FILE, raw, sizeof(raw)); waited += 200) {
		if (waited >= WAIT_WIFI_MAC_MS) {
			fprintf(stderr, "bluetooth: no %s after %d ms\n", BT_WIFI_MAC_FILE, WAIT_WIFI_MAC_MS);
			return false;
		}
		sleep_ms(200);
	}
	raw[strcspn(raw, "\r\n")] = '\0';
	unsigned parts[6];
	if (sscanf(raw, "%2x:%2x:%2x:%2x:%2x:%2x", &parts[0], &parts[1], &parts[2], &parts[3], &parts[4], &parts[5]) != 6) {
		return false;
	}
	unsigned char bytes[6];
	for (int i = 0; i < 6; i++) {
		bytes[i] = (unsigned char)parts[i];
	}
	bytes[0] = (unsigned char)((bytes[0] | 0x02) & 0xFE);
	bytes[5] = (unsigned char)(bytes[5] + 1);
	format_address(bytes, out, size);
	return true;
}

static bool address_from_random(char *out, size_t size) {
	unsigned char bytes[6];
	FILE *f = fopen("/dev/urandom", "rb");
	if (!f) {
		return false;
	}
	size_t got = fread(bytes, 1, sizeof(bytes), f);
	fclose(f);
	if (got != sizeof(bytes)) {
		return false;
	}
	bytes[0] = (unsigned char)((bytes[0] | 0x02) & 0xFE);
	format_address(bytes, out, size);
	return true;
}

// The address to hand brcm_patchram_plus, made and written down when the device
// has none.
//
// /usr/data/bt_macaddr.txt is written once, at the factory or by the stock
// bring-up. This player loads the chip's firmware itself and never goes through
// that bring-up, so on a unit where the file was never written there is no
// address at all and the radio does not come up -- which everything downstream
// reports as a missing bluetoothd.
//
// Written down rather than made afresh at every boot, because an address that
// changes is worse than one that was invented: every pair of headphones the
// user has paired holds a link key against the previous one, and none of them
// would match again.
static bool device_address(char *out, size_t size) {
	char stored[64];
	if (slurp(BT_MAC_FILE, stored, sizeof(stored))) {
		stored[strcspn(stored, "\r\n")] = '\0';
		if (address_is_shaped(stored)) {
			snprintf(out, size, "%s", stored);
			return true;
		}
		fprintf(stderr, "bluetooth: the address in %s is not an address; making one\n", BT_MAC_FILE);
	} else {
		fprintf(stderr, "bluetooth: %s is not there; making an address for this device\n", BT_MAC_FILE);
	}

	if (!address_from_wifi(out, size) && !address_from_random(out, size)) {
		return false;
	}

	int fd = open(BT_MAC_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		// The radio still comes up; it is the next boot that will have to make
		// the address again, and the headphones paired in between will not know
		// this device when it does.
		fprintf(stderr, "bluetooth: %s could not be written; %s will not survive a reboot\n", BT_MAC_FILE, out);
		return true;
	}
	char line[32];
	int len = snprintf(line, sizeof(line), "%s\n", out);
	bool written = write(fd, line, (size_t)len) == len;
	close(fd);
	fprintf(stderr, "bluetooth: this device is %s%s\n", out, written ? ", written down" : " but it was not written down");
	return true;
}

static bool ensure_transport(void) {
	if (hci_up()) {
		return true;
	}

	write_file(BT_RFKILL, "1");
	wait_out_stock_bringup();
	if (hci_up()) {
		return true;
	}

	char address[64];
	if (!device_address(address, sizeof(address))) {
		fprintf(stderr, "bluetooth: no address for this device; the firmware cannot be loaded\n");
		return false;
	}

	char firmware[192];
	firmware_name(firmware, sizeof(firmware));

	char *argv[] = {
		(char *)"brcm_patchram_plus",
		(char *)"--enable_hci",
		(char *)"--baudrate",
		(char *)"3000000",
		(char *)"--no2bytes",
		(char *)"--patchram",
		firmware,
		(char *)BT_TRANSPORT_TTY,
		(char *)"--tosleep=50000",
		(char *)"--use_baudrate_for_download",
		(char *)"--enable_lpm",
		(char *)"--bd_addr",
		address,
		NULL,
	};
	fprintf(stderr, "bluetooth: loading %s over %s\n", firmware, BT_TRANSPORT_TTY);
	if (!spawn_daemon(BT_TRANSPORT_BIN, argv)) {
		return false;
	}

	uint32_t began = now_ms();
	for (int waited = 0; waited < WAIT_TRANSPORT_MS; waited += 100) {
		if (hci_up()) {
			fprintf(stderr, "bluetooth: hci0 up after %u ms\n", now_ms() - began);
			return true;
		}
		sleep_ms(100);
	}
	fprintf(stderr, "bluetooth: hci0 did not arrive in %d ms\n", WAIT_TRANSPORT_MS);
	return false;
}

static bool ensure_bluetoothd(void) {
	// The firmware's agent has to go before this player's can be the default
	// one: bluez keeps exactly one, and whichever registered first answers every
	// pairing.
	if (process_running("bt-agent")) {
		fprintf(stderr, "bluetooth: stopping bt-agent; the player is the agent now\n");
		stop_process("bt-agent");
	}

	if (!process_running("bluetoothd")) {
		// -E is the experimental interface and -C the SDP compatibility one. Both
		// are what the firmware starts it with.
		char *argv[4] = {(char *)"bluetoothd", (char *)"-E", (char *)"-C", NULL};
		spawn_daemon(BLUETOOTHD_BIN, argv);
	}

	if (!btstack_wait_service("org.bluez", WAIT_BLUETOOTHD_MS)) {
		fprintf(stderr, "bluetooth: bluetoothd did not claim org.bluez\n");
		return false;
	}
	// The name on the bus is not the same as a controller bluez can drive: the
	// Adapter1 interface appears on /org/bluez/hci0 only once it has one.
	if (!btstack_wait_adapter(WAIT_BLUETOOTHD_MS)) {
		fprintf(stderr, "bluetooth: org.bluez is there but hci0 has no Adapter1\n");
		return false;
	}
	return true;
}

// Whether the bluealsa on this firmware understands an option, read from its own
// help text.
//
// /usr/bin/bluealsa is a file in the firmware, not part of this build, and the
// two that can be there do not take the same arguments: --io-rt-priority arrived
// after 4.1.1 and the stock daemon does not know it. getopt answers an unknown
// long option with a usage message and an exit, so passing one blind would not
// degrade the sound -- it would leave the device with no Bluetooth audio at all.
// One `bluealsa -h`, once per bring-up, buys the right to ask.
static const char *bluealsa_help(void) {
	static char help[8192];
	static bool asked;

	if (!asked) {
		asked = true;
		int fds[2];
		if (pipe(fds) != 0) {
			return false;
		}
		pid_t pid = fork();
		if (pid < 0) {
			close(fds[0]);
			close(fds[1]);
			return false;
		}
		if (pid == 0) {
			dup2(fds[1], STDOUT_FILENO);
			dup2(fds[1], STDERR_FILENO);
			close(fds[0]);
			close(fds[1]);
			char *argv[3] = {(char *)"bluealsa", (char *)"-h", NULL};
			execv(BLUEALSA_BIN, argv);
			_exit(127);
		}
		close(fds[1]);

		size_t used = 0;
		ssize_t got;
		while (used + 1 < sizeof(help) && (got = read(fds[0], help + used, sizeof(help) - 1 - used)) > 0) {
			used += (size_t)got;
		}
		help[used] = '\0';
		close(fds[0]);

		int status = 0;
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		}
	}

	return help;
}

static bool bluealsa_knows_option(const char *option) {
	const char *help = bluealsa_help();
	return help[0] && strstr(help, option) != NULL;
}

// What this bluealsa can encode and what it can decode, read out of its own
// help text once per bring-up.
//
// It is the one fact none of this code can infer: which codecs are in the
// binary is a property of a file that ships with the firmware, not of this
// build. And the two lists need not be the same -- encoding a codec and
// decoding it are different libraries:
//
//   Available BT audio codecs:
//     a2dp-source:	SBC, AAC, aptX, aptX-HD, LDAC
//     a2dp-sink:	SBC, AAC, aptX, aptX-HD, LDAC
//
// What the help does not say is that being in the binary is not the same as
// being offered. See the -c arguments in ensure_bluealsa().
//
// The shape is matched outright -- the header, then a line per profile, "name:"
// and a comma separated list -- because searching for a section by name alone
// lands on the list of PROFILES, which says nothing about codecs.
static char local_source_codecs[BT_MAX_CODECS][BT_CODEC_MAX];
static int local_source_count;
static char local_sink_codecs[BT_MAX_CODECS][BT_CODEC_MAX];
static int local_sink_count;

// "  a2dp-source:\tSBC, AAC, aptX" -> the names, one per row.
static int parse_codec_line(const char *line, int length, char out[][BT_CODEC_MAX], int max) {
	const char *colon = memchr(line, ':', (size_t)length);
	if (!colon) {
		return 0;
	}
	const char *at = colon + 1;
	const char *end = line + length;
	int count = 0;

	while (at < end && count < max) {
		while (at < end && (*at == ' ' || *at == '\t' || *at == ',')) {
			at++;
		}
		int used = 0;
		while (at < end && *at != ',' && *at != ' ' && *at != '\t' && used + 1 < BT_CODEC_MAX) {
			out[count][used++] = *at++;
		}
		out[count][used] = '\0';
		if (used > 0) {
			count++;
		}
		while (at < end && *at != ',') {
			at++;
		}
	}
	return count;
}

static void read_local_codecs(void) {
	const char *help = bluealsa_help();
	if (!help[0]) {
		fprintf(stderr, "bluetooth: bluealsa did not answer -h; its codecs are unknown\n");
		return;
	}

	const char *at = strstr(help, "Available BT audio codecs:");
	if (!at) {
		fprintf(stderr, "bluetooth: this bluealsa does not list its codecs\n");
		return;
	}
	at = strchr(at, '\n');

	while (at) {
		at++;
		const char *end = strchr(at, '\n');
		int length = end ? (int)(end - at) : (int)strlen(at);
		const char *trimmed = at;
		while (length > 0 && (*trimmed == ' ' || *trimmed == '\t')) {
			trimmed++;
			length--;
		}
		if (length <= 0) {
			break; // the blank line that ends the section
		}

		if (strncmp(trimmed, "a2dp-source", 11) == 0) {
			local_source_count = parse_codec_line(trimmed, length, local_source_codecs, BT_MAX_CODECS);
		} else if (strncmp(trimmed, "a2dp-sink", 9) == 0) {
			local_sink_count = parse_codec_line(trimmed, length, local_sink_codecs, BT_MAX_CODECS);
		}
		at = end;
	}

	for (int i = 0; i < 2; i++) {
		bool source = i == 0;
		int count = source ? local_source_count : local_sink_count;
		char line[160] = "";
		size_t used = 0;
		for (int j = 0; j < count; j++) {
			const char *name = source ? local_source_codecs[j] : local_sink_codecs[j];
			int written = snprintf(line + used, sizeof(line) - used, "%s%s", used ? ", " : "", name);
			if (written < 0 || (size_t)written >= sizeof(line) - used) {
				break;
			}
			used += (size_t)written;
		}
		fprintf(stderr, "bluetooth: can %s %s\n", source ? "send" : "receive", used ? line : "nothing");
	}
}

// Being compiled in is not the same as being offered.
//
// bluez-alsa enables SBC and AAC and leaves every other A2DP codec off, and only
// the enabled ones are registered with bluez as endpoints. The other side sees
// the endpoints, never the binary: a phone with LDAC is offered AAC and SBC and
// nothing else while -h happily lists aptX, aptX-HD and LDAC. The help says
// what the binary contains; -c says what to put on the air.
//
// So every codec the help lists is asked for by name, minus the two that are
// already on -- SBC cannot be switched off at all, and asking for it is refused.
// An unknown name makes the daemon exit before it starts, which is why the names
// come from the daemon's own output and are not written here.
//
// The two lists overlap, so a name that is both encoded and decoded is asked for
// once: -c takes a codec, not a direction.
static int codecs_to_ask_for(char out[][BT_CODEC_MAX], int max) {
	int count = 0;
	for (int side = 0; side < 2; side++) {
		int listed = side == 0 ? local_source_count : local_sink_count;
		for (int i = 0; i < listed && count < max; i++) {
			const char *name = side == 0 ? local_source_codecs[i] : local_sink_codecs[i];
			if (strcasecmp(name, "SBC") == 0 || strcasecmp(name, "AAC") == 0) {
				continue;
			}
			bool seen = false;
			for (int j = 0; j < count && !seen; j++) {
				seen = strcasecmp(out[j], name) == 0;
			}
			if (!seen) {
				snprintf(out[count++], BT_CODEC_MAX, "%s", name);
			}
		}
	}
	return count;
}

static bool ensure_bluealsa(void) {
	// Before the daemon, not after: the help text comes from running the binary
	// with -h, not from the bus, and the -c arguments below are built out of it.
	static bool codecs_read;
	if (!codecs_read) {
		codecs_read = true;
		read_local_codecs();
	}

	if (!process_running("bluealsa")) {
		// Both halves of A2DP.
		//
		// As a source the stream is encoded here and the level goes to the
		// headphones over AVRCP. As a sink the player is the one being driven: a
		// phone or a computer encodes, this device decodes and puts the result out
		// of its own DAC, which is Bluetooth receiver mode. The sink profile has
		// to be declared here because bluez builds the local SDP record out of the
		// endpoints bluealsa registers: with only a source endpoint, another
		// source -- a MacBook, say -- finds no profile in common and
		// Device1.Connect() fails. Declaring both costs nothing while nothing is
		// connected: an endpoint no link uses is a record in SDP, not a thread.
		//
		// --keep-alive holds the A2DP transport for a few seconds after the last
		// PCM client closes, instead of releasing it at once. It is off by default
		// because of the silence at a track change: the reopen then finds the
		// Bluetooth socket still valid and reuses it, and a reuse sends no AVDTP
		// START, so nothing tells the sink to start rendering again -- the writes
		// go through, the encoder keeps up, the queue drains at exactly the right
		// rate, and the headphones throw away every packet. Selecting a codec by
		// hand brings the sound back because it forces the configuration to be
		// made again from scratch.
		//
		// At 0 the transport is released and re-acquired around every track
		// change, and the acquire is what sends the START. The cost is a pause
		// while that happens.
		//
		//   [bluetooth]
		//   keep_alive_secs = 5   hold it
		//
		int keep_alive = (int)config_get_int("bluetooth", "keep_alive_secs", 0);
		char keepalive[32];
		snprintf(keepalive, sizeof(keepalive), "--keep-alive=%d", keep_alive < 0 ? 0 : keep_alive);
		//
		// --io-rt-priority puts bluealsa's encoder threads on SCHED_FIFO. The
		// encoding does not happen in this process, it happens in bluealsa, one
		// thread per stream, and by default that thread has the same claim on the
		// one core as LVGL does.
		//
		// It stays a setting because the ordering between encoder and decoder is
		// a question a listen answers better than an argument does:
		//
		//   [bluetooth]
		//   io_rt_priority = 0    no option at all
		//                  = 8    below this player's playback thread (SCHED_RR 10)
		//                  = 12   above it
		//
		// The encoder is the side with the hard deadline -- a packet every few
		// milliseconds or the sink hears a gap -- while the decoder has 750 ms of
		// buffer as slack, which is the argument for 12. A consumer that outranks
		// its producer on a single core is the argument for 8.
		char rtprio[32] = {0};
		int wanted = (int)config_get_int("bluetooth", "io_rt_priority", 12);
		bool rt = wanted > 0 && bluealsa_knows_option("--io-rt-priority");
		if (rt) {
			snprintf(rtprio, sizeof(rtprio), "--io-rt-priority=%d", wanted);
			fprintf(stderr, "bluetooth: encoder threads at SCHED_FIFO %d\n", wanted);
		} else if (wanted > 0) {
			fprintf(stderr, "bluetooth: this bluealsa has no --io-rt-priority; the encoder stays at normal "
							"priority\n");
		}
		// LDAC's bit rate. A daemon option and not a per-link one: it is read
		// here and nowhere else, which is why changing it restarts the daemon.
		// Adaptive is the default because it is the one that survives a bad
		// link -- 990 kbps into a pocket is 990 kbps of dropouts.
		char ldac[48] = {0};
		const char *quality = bluetooth_ldac_quality();
		if (strcmp(quality, "abr") == 0) {
			if (bluealsa_knows_option("--ldac-abr")) {
				snprintf(ldac, sizeof(ldac), "--ldac-abr");
			}
		} else if (bluealsa_knows_option("--ldac-quality")) {
			snprintf(ldac, sizeof(ldac), "--ldac-quality=%s", quality);
		}
		if (ldac[0]) {
			fprintf(stderr, "bluetooth: LDAC %s\n", ldac);
		}

		char codecs[BT_MAX_CODECS][BT_CODEC_MAX];
		int codec_count = codecs_to_ask_for(codecs, BT_MAX_CODECS);

		char *argv[10 + BT_MAX_CODECS * 2];
		int at = 0;
		argv[at++] = (char *)"bluealsa";
		argv[at++] = (char *)"-p";
		argv[at++] = (char *)"a2dp-source";
		argv[at++] = (char *)"-p";
		argv[at++] = (char *)"a2dp-sink";
		argv[at++] = (char *)"--a2dp-volume";
		argv[at++] = keepalive;
		if (rt) {
			argv[at++] = rtprio;
		}
		if (ldac[0]) {
			argv[at++] = ldac;
		}
		for (int i = 0; i < codec_count; i++) {
			argv[at++] = (char *)"-c";
			argv[at++] = codecs[i];
		}
		argv[at] = NULL;
		if (codec_count) {
			char line[160] = "";
			size_t used = 0;
			for (int i = 0; i < codec_count; i++) {
				int written = snprintf(line + used, sizeof(line) - used, "%s%s", used ? ", " : "", codecs[i]);
				if (written < 0 || (size_t)written >= sizeof(line) - used) {
					break;
				}
				used += (size_t)written;
			}
			fprintf(stderr, "bluetooth: offering %s on top of SBC and AAC\n", line);
		}
		fprintf(stderr, "bluetooth: the A2DP transport stays up %d s after the last client\n", keep_alive);
		spawn_daemon(BLUEALSA_BIN, argv);
	}
	if (!btstack_wait_service("org.bluealsa", WAIT_BLUEALSA_MS)) {
		fprintf(stderr, "bluetooth: bluealsa did not claim org.bluealsa\n");
		return false;
	}

	// Which daemon actually answered. /usr/bin/bluealsa is a file in the
	// firmware and not part of this build, so the version is a fact to read,
	// not one to assume: the stock 4.1.1 and the 4.3.1 built for this player
	// behave differently on drain and on poll.
	char version[32];
	if (btstack_bluealsa_version(version, sizeof(version))) {
		fprintf(stderr, "bluetooth: bluealsa v%s on the bus\n", version);
	}
	return true;
}

static void read_local_name(void) {
	// What the user chose wins over the firmware's file, which is on a
	// read-only filesystem and is the factory name.
	const char *chosen = config_get("wireless", "bt_name", "");
	if (chosen && chosen[0]) {
		pthread_mutex_lock(&lock);
		snprintf(g_local_name, sizeof(g_local_name), "%s", chosen);
		pthread_mutex_unlock(&lock);
		return;
	}

	char buf[BT_NAME_MAX];
	if (!slurp(BT_NAME_FILE, buf, sizeof(buf))) {
		return;
	}
	buf[strcspn(buf, "\r\n")] = '\0';
	if (buf[0]) {
		pthread_mutex_lock(&lock);
		snprintf(g_local_name, sizeof(g_local_name), "%s", buf);
		pthread_mutex_unlock(&lock);
	}
}

bool bluetooth_available(void) {
	// A device with Bluetooth has the radio switch, or a controller already
	// attached, or the loader that attaches one. A device without has none.
	return access("/sys/class/rfkill/rfkill0", F_OK) == 0 || access("/sys/class/bluetooth", F_OK) == 0 ||
		   access(BT_TRANSPORT_BIN, F_OK) == 0;
}

// ---------------------------------------------------------------------------
// jobs and state
// ---------------------------------------------------------------------------

static void set_op(bt_op_t op, const char *name) {
	pthread_mutex_lock(&lock);
	g_op = op;
	if (name) {
		snprintf(g_op_name, sizeof(g_op_name), "%s", name);
	}
	pthread_mutex_unlock(&lock);
}

static void do_read_codecs(void);
static void do_auto_codec(const char *mac);

// The device the automatic codec choice has already been made for. Cleared when
// the audio goes, so plugging the same headphones back in asks again.
static char codec_auto_done[BT_MAC_MAX]; // worker only

// Points the playback thread at the right ALSA device: the bluealsa PCM of
// whatever is connected while there is an A2DP sink, the jack otherwise. Called
// whenever either of those two things can have changed. The name is picked up at
// the next PCM open; nothing here interrupts a running stream.
static void apply_output_routing(void) {
	char pcm[128];
	bluetooth_output_pcm(pcm, (int)sizeof(pcm));
	audio_set_output_device(pcm[0] ? pcm : NULL);
}

// True when `name` is only the device's own address dressed up -- bluez uses
// "AA-BB-CC-DD-EE-FF" as the alias of anything that never told it a name.
// Compared with the separators stripped, so ':' and '-' are the same thing.
static bool name_is_just_address(const char *name, const char *mac) {
	const char *a = name;
	const char *b = mac;

	if (!name[0]) {
		return true;
	}

	for (;;) {
		while (*a == ':' || *a == '-' || *a == ' ') {
			a++;
		}
		while (*b == ':' || *b == '-' || *b == ' ') {
			b++;
		}
		if (!*a || !*b) {
			return !*a && !*b;
		}
		if ((*a | 0x20) != (*b | 0x20)) {
			return false;
		}
		a++;
		b++;
	}
}

// The template the firmware keeps at /usr/data/alsa.conf, whose device address
// the stock player rewrites whenever a sink connects. Kept in step here for the
// same reason: it is the file the patched bluealsa reads its LDAC and UAT
// quality settings from.
#define BT_ALSA_CONF "/usr/data/alsa.conf"

static void rewrite_alsa_conf(const char *mac) {
	char before[4096];
	if (!slurp(BT_ALSA_CONF, before, sizeof(before))) {
		return; // no template on this firmware; nothing to keep in step
	}

	char after[4096];
	size_t used = 0;
	char *save = NULL;

	for (char *line = strtok_r(before, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char rewritten[256];
		const char *emit = line;

		if (strncmp(line, "pcm.", 4) == 0) {
			snprintf(rewritten, sizeof(rewritten), "pcm.bluealsa:DEV=%s {", mac);
			emit = rewritten;
		} else {
			const char *device = strstr(line, "device ");
			if (device) {
				int keep = (int)(device - line) + (int)strlen("device ");
				snprintf(rewritten, sizeof(rewritten), "%.*s%s", keep, line, mac);
				emit = rewritten;
			}
		}

		int written = snprintf(after + used, sizeof(after) - used, "%s\n", emit);
		if (written < 0 || (size_t)written >= sizeof(after) - used) {
			return; // would not fit: leave the file exactly as it was
		}
		used += (size_t)written;
	}

	FILE *f = fopen(BT_ALSA_CONF, "w");
	if (!f) {
		return;
	}
	fwrite(after, 1, used, f);
	fclose(f);
}

// The bluealsa PCM object is the truth for "is there anything to write to", and
// it is also allowed to blink. A track change closes the PCM and opens it again;
// without --keep-alive bluealsa releases the A2DP transport in between, and
// headphones that see the stream stop can drop the link and rebuild it a couple
// of seconds later. Following that blink instantly sends the music to the jack
// and back at every track change, which is worse than waiting for it.
//
// So the sink going is believed only when bluez agrees that the device is no
// longer connected, or when it has stayed gone this long.
#define A2DP_GRACE_MS 3000
static uint32_t a2dp_missing_since; // worker only

// Whether bluez still reports an ACL link to this address.
static bool device_still_connected(const char *mac) {
	bool connected = false;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < g_paired_count && !connected; i++) {
		connected = g_paired[i].connected && strcasecmp(g_paired[i].mac, mac) == 0;
	}
	pthread_mutex_unlock(&lock);
	return connected;
}

// Takes the audio state from bluealsa's PCM objects and moves the sound with it.
// The PCM exists exactly while there is something to write to, which is the
// distinction that matters: an ACL link can be up with no audio profile on it
// at all, and that is what "it says connected and plays nothing" is.
static void refresh_audio_state(void) {
	char mac[BT_MAC_MAX];
	bool sink = btstack_audio_sink(mac, sizeof(mac));

	if (sink) {
		a2dp_missing_since = 0;
	} else {
		pthread_mutex_lock(&lock);
		bool had_sink = g_a2dp_connected;
		char last[BT_MAC_MAX];
		copy_field(last, sizeof(last), g_a2dp_mac, sizeof(g_a2dp_mac));
		pthread_mutex_unlock(&lock);

		if (had_sink && last[0] && device_still_connected(last)) {
			if (a2dp_missing_since == 0) {
				a2dp_missing_since = now_ms();
				fprintf(stderr, "bluetooth: the PCM on %s went away but the link is still up; waiting\n", last);
			}
			if ((uint32_t)(now_ms() - a2dp_missing_since) < A2DP_GRACE_MS) {
				return; // nothing has changed yet as far as anything else is concerned
			}
			fprintf(stderr, "bluetooth: the PCM on %s has been gone for %d ms; back to the jack\n", last,
					A2DP_GRACE_MS);
		}
		a2dp_missing_since = 0;
	}

	pthread_mutex_lock(&lock);
	bool changed = sink != g_a2dp_connected || (sink && strcasecmp(g_a2dp_mac, mac) != 0);
	g_a2dp_connected = sink;
	snprintf(g_a2dp_mac, sizeof(g_a2dp_mac), "%s", sink ? mac : "");
	if (sink) {
		g_state = BT_STATE_CONNECTED;
	}
	if (changed) {
		g_serial++;
	}
	pthread_mutex_unlock(&lock);

	if (!changed) {
		return;
	}
	fprintf(stderr, "bluetooth: audio %s%s\n", sink ? "on " : "back on the jack", sink ? mac : "");
	if (sink) {
		// First, because the patched bluealsa reads its LDAC and UAT quality
		// settings out of that file and the choice below can land on LDAC.
		rewrite_alsa_conf(mac);

		// Then the choice, while nothing is playing yet: making it is a
		// teardown and a rebuild of the transport.
		do_auto_codec(mac);

		// And what came of it. Without this line the player only ever logs its
		// own side -- "PCM open 96000 Hz S32_LE" -- and says nothing about the
		// codec, which is the first thing to know when the sound is wrong.
		char path[96];
		if (btstack_audio_info(mac, path, sizeof(path))) {
			fprintf(stderr, "bluetooth: bluealsa is encoding %s\n", path);
		}
	} else {
		codec_auto_done[0] = '\0'; // the next connection chooses again
	}
	apply_output_routing();
	do_read_codecs();
}

// Whether a device bluez has no name for is worth a row anyway.
//
// Nameless devices are normally not shown, and for most of them that is right:
// a scan in a flat picks up tracking tags and anonymous adverts by the dozen.
// Headphones are the case that rule gets wrong. bluez has no name for them
// until a remote name request comes back, and a pair that answers the inquiry
// and then goes quiet never sends one -- so a device that was found is never
// listed. It is worst immediately after forgetting one, because RemoveDevice
// throws away the cached name together with the pairing.
//
// The class of device comes with the inquiry result itself, before any name,
// and the A2DP sink UUID comes with the advert when it comes at all. Either
// says this is something to listen through rather than a tag on a keyring.
#define COD_MAJOR_AUDIO 0x04
static bool anonymous_is_audio(const btstack_device_t *dev) {
	return dev->audio_sink || ((dev->cod >> 8) & 0x1f) == COD_MAJOR_AUDIO;
}

// Re-reads everything bluez knows and splits it into the two lists the
// interface shows.
static void refresh_devices(void) {
	btstack_device_t devices[BT_MAX_DEVICES];
	int count = btstack_devices(devices, BT_MAX_DEVICES);

	bt_device_t paired[BT_MAX_DEVICES];
	bt_device_t found[BT_MAX_DEVICES];
	bt_device_t anonymous[BT_MAX_DEVICES];
	int paired_count = 0, found_count = 0, anonymous_count = 0, hidden = 0;
	bool any_connected = false;

	for (int i = 0; i < count; i++) {
		bt_device_t row;
		memset(&row, 0, sizeof(row));
		copy_field(row.mac, sizeof(row.mac), devices[i].address, sizeof(devices[i].address));
		copy_field(row.name, sizeof(row.name), devices[i].name, sizeof(devices[i].name));
		row.paired = devices[i].paired;
		row.connected = devices[i].connected;
		row.trusted = devices[i].trusted;
		row.rssi = devices[i].rssi;

		// bluez hands back the address as the alias when it has nothing better,
		// sometimes with dashes instead of colons, and that is the same as
		// having no name at all.
		if (name_is_just_address(row.name, row.mac)) {
			row.name[0] = '\0';
		}

		if (row.paired) {
			// A paired device always gets a row, so one that never gave a name
			// falls back to its address -- unlike in the scan list, where a
			// nameless device is simply not shown.
			if (!row.name[0]) {
				snprintf(row.name, sizeof(row.name), "%s", row.mac);
			}
			any_connected = any_connected || row.connected;
			if (paired_count < BT_MAX_DEVICES) {
				paired[paired_count++] = row;
			}
		} else if (row.name[0]) {
			if (found_count < BT_MAX_DEVICES) {
				found[found_count++] = row;
			}
		} else if (anonymous_is_audio(&devices[i])) {
			// By address, which is at least something the user can match
			// against the sticker under the headphones.
			snprintf(row.name, sizeof(row.name), "%s", row.mac);
			if (anonymous_count < BT_MAX_DEVICES) {
				anonymous[anonymous_count++] = row;
			}
		} else {
			hidden++;
		}
	}

	// After the named ones, never among them: a list that opens with hex reads
	// as a list of junk, and the device being looked for is usually named.
	for (int i = 0; i < anonymous_count && found_count < BT_MAX_DEVICES; i++) {
		found[found_count++] = anonymous[i];
	}

	pthread_mutex_lock(&lock);
	memcpy(g_paired, paired, sizeof(bt_device_t) * (size_t)paired_count);
	g_paired_count = paired_count;
	memcpy(g_found, found, sizeof(bt_device_t) * (size_t)found_count);
	g_found_count = found_count;
	g_seen_count = count;
	g_hidden_count = hidden;
	if (g_enabled && !g_a2dp_connected) {
		g_state = any_connected ? BT_STATE_CONNECTED : BT_STATE_ON;
	}
	g_serial++;
	pthread_mutex_unlock(&lock);

	refresh_audio_state();
}

// How long a discovery runs when the user presses search. Ten seconds, as the
// firmware's `bt-adapter -d` does, except that the list fills as devices arrive
// rather than all at once at the end.
#define SCAN_SECONDS 10

static void do_scan(void) {
	if (!btstack_discovery(true)) {
		// Said here as well as at the end, because this is the branch a user with
		// no radio takes: with no line here their log carries nothing about the
		// search, which reads as a search that ran and found nothing.
		fprintf(stderr, "bluetooth: the search could not be started; bluez did not take the discovery\n");
		pthread_mutex_lock(&lock);
		g_scanning = false;
		pthread_mutex_unlock(&lock);
		return;
	}

	for (int second = 0; second < SCAN_SECONDS; second++) {
		sleep_ms(1000);
		pthread_mutex_lock(&lock);
		bool abandon = !g_enabled || !g_scanning;
		pthread_mutex_unlock(&lock);
		if (abandon) {
			break;
		}
		// Every second, so the list grows in front of the user instead of
		// appearing when the sweep is over.
		refresh_devices();
	}

	btstack_discovery(false);
	pthread_mutex_lock(&lock);
	g_scanning = false;
	pthread_mutex_unlock(&lock);
	refresh_devices();

	// One line per search, so that "it does not find my headphones" has an
	// answer: this says whether bluez saw anything at all and how much of it the
	// list decided not to show.
	pthread_mutex_lock(&lock);
	fprintf(stderr, "bluetooth: scan over, bluez holds %d devices, %d paired, %d in the list, %d nameless and dropped\n",
			g_seen_count, g_paired_count, g_found_count, g_hidden_count);
	pthread_mutex_unlock(&lock);
}

static void do_scan_stop(void) {
	pthread_mutex_lock(&lock);
	g_scanning = false;
	pthread_mutex_unlock(&lock);
	btstack_discovery(false);
}

// ---------------------------------------------------------------------------
// coming back to the headphones
// ---------------------------------------------------------------------------

#define BT_LAST_DEVICE_KEY "bt_last_device"

static void remember_last_device(const char *mac) {
	if (!mac || !mac[0]) {
		return;
	}
	const char *known = config_get("wireless", BT_LAST_DEVICE_KEY, "");
	if (strcasecmp(known, mac) == 0) {
		return; // already what it says: no write, no card wear
	}
	config_set("wireless", BT_LAST_DEVICE_KEY, mac);
	config_save();
}

// Forgetting a device has to forget it here too. Without this the address stays
// written down after the pairing is gone, and every power-on spends the five
// seconds of the paired-list wait -- ten rounds of GetManagedObjects -- to
// conclude what the RemoveDevice already knew.
static void forget_last_device(const char *mac) {
	if (!mac || !mac[0]) {
		return;
	}
	if (strcasecmp(config_get("wireless", BT_LAST_DEVICE_KEY, ""), mac) != 0) {
		return;
	}
	config_set("wireless", BT_LAST_DEVICE_KEY, "");
	config_save();
	fprintf(stderr, "bluetooth: %s was the last device; nothing to go back to now\n", mac);
}

// How long the audio profile is given to come up after Connect has returned. On
// a pair of headphones that works, the bluealsa PCM appears well inside a
// second; six is long enough for one that negotiates slowly without leaving the
// user in front of a spinner.
#define A2DP_CONFIRM_MS 6000

// And how long the headphones are given to come back by themselves, at the end
// of the bring-up, before anything is asked of them.
#define SELF_RECONNECT_GRACE_MS 8000

// Waits for the bluealsa PCM of this device to exist, or for the deadline.
static bool wait_for_a2dp(const char *mac, int timeout_ms) {
	for (int waited = 0;; waited += 100) {
		char sink[BT_MAC_MAX];
		if (btstack_audio_sink(sink, sizeof(sink)) && strcasecmp(sink, mac) == 0) {
			return true;
		}
		pthread_mutex_lock(&lock);
		bool abandon = !g_enabled;
		pthread_mutex_unlock(&lock);
		if (abandon || waited >= timeout_ms) {
			return false;
		}
		sleep_ms(100);
	}
}

// The address of the connected sink other than `except`, if there is one. Only
// one pair of headphones can hold the A2DP link, and the stock player
// disconnects the incumbent before connecting the new one.
static bool other_connected_sink(const char *except, char *out, size_t size) {
	bool found = false;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < g_paired_count; i++) {
		if (g_paired[i].connected && strcasecmp(g_paired[i].mac, except) != 0) {
			copy_field(out, size, g_paired[i].mac, sizeof(g_paired[i].mac));
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	return found;
}

// org.bluez.Device1.Connect: all profiles, and its reply is the answer. Sent
// exactly once -- calling Connect again while the first is still coming up
// makes bluez tear the link down.
#define CONNECT_TIMEOUT_MS 25000
#define PAIR_TIMEOUT_MS 40000

static bool connect_device(const char *mac) {
	// Never on top of a link that is already up.
	//
	// Headphones often come back by themselves the instant the adapter is
	// powered, and a Connect sent on top of that makes bluez pull the profile
	// down; from there every attempt is refused. Read here, immediately before
	// the command, which is the only place late enough.
	char sink[BT_MAC_MAX];
	if (btstack_audio_sink(sink, sizeof(sink)) && strcasecmp(sink, mac) == 0) {
		fprintf(stderr, "bluetooth: %s is already connected with audio; leaving it alone\n", mac);
		remember_last_device(mac);
		return true;
	}

	char incumbent[BT_MAC_MAX];
	if (other_connected_sink(mac, incumbent, sizeof(incumbent))) {
		fprintf(stderr, "bluetooth: disconnecting %s first (one sink at a time)\n", incumbent);
		btstack_disconnect(incumbent);
		sleep_ms(400);
	}

	bool ok = btstack_connect(mac, CONNECT_TIMEOUT_MS);
	fprintf(stderr, "bluetooth: connect %s -> %s\n", mac, ok ? "ok" : "FAILED");
	if (ok) {
		remember_last_device(mac);
		// Device1.Connect returning says the ACL link is up. It does not say
		// that the AUDIO profile came up, which is a separate and later event --
		// bluealsa publishing the PCM. Reporting the first as success puts
		// "connected" on screen over headphones that play nothing.
		//
		// A profile that does not arrive is NOT reported as a failed connection:
		// the link is there, and answering false puts "make sure the headphones
		// are in pairing mode" on the screen, which is advice for a problem the
		// user does not have.
		bool audio = wait_for_a2dp(mac, A2DP_CONFIRM_MS);
		fprintf(stderr, "bluetooth: audio profile on %s %s\n", mac, audio ? "came up" : "did NOT come up");
	}
	refresh_devices();
	return ok;
}

static bool pair_and_connect(const char *mac) {
	// bluez must currently know this address. It ages a merely-seen device out
	// after thirty seconds, and between the sweep finishing, the list being
	// drawn and the user tapping, half a minute goes by very easily -- which is
	// what makes Pair answer "Device not found". A short discovery puts it back
	// in bluez's hands, and stops as soon as the device is there.
	for (int waited = 0; waited < 8000; waited += 500) {
		btstack_device_t devices[BT_MAX_DEVICES];
		int count = btstack_devices(devices, BT_MAX_DEVICES);
		bool known = false;
		for (int i = 0; i < count && !known; i++) {
			known = strcasecmp(devices[i].address, mac) == 0;
		}
		if (known) {
			break;
		}
		if (waited == 0) {
			btstack_discovery(true);
		}
		sleep_ms(500);
	}
	btstack_discovery(false);

	fprintf(stderr, "bluetooth: pairing %s\n", mac);
	if (!btstack_pair(mac, PAIR_TIMEOUT_MS)) {
		fprintf(stderr, "bluetooth: %s would not pair\n", mac);
		return false;
	}

	// Trusted only for a device the user chose explicitly, which is exactly what
	// has just happened. Without it bluez asks the agent again on every later
	// connection, and a pair of headphones that reconnects on its own has nobody
	// to ask.
	btstack_trust(mac, true);

	fprintf(stderr, "bluetooth: %s paired, connecting\n", mac);
	return connect_device(mac);
}

// Whether going back to the last device should be given up, and why.
//
// The reconnect runs on the worker, inline in the power-on job, and can hold
// that thread for the best part of a minute: eight seconds of grace plus three
// connect attempts against a device that may be in a drawer. Nothing the user
// asks for in the meantime runs until it returns, so a queued job from the user
// ends it.
//
// A sink that belongs to somebody else ends it too: connect_device() disconnects
// whatever is connected first ("one sink at a time"), which would push off
// headphones the user has only just connected.
static bool reconnect_should_stop(const char *mac) {
	char sink[BT_MAC_MAX];
	if (btstack_audio_sink(sink, sizeof(sink)) && strcasecmp(sink, mac) != 0) {
		fprintf(stderr, "bluetooth: %s is playing now; not going back to %s\n", sink, mac);
		return true;
	}

	pthread_mutex_lock(&lock);
	bool off = !g_enabled;
	bool user_waiting = false;
	for (int i = 0; i < queue_count && !user_waiting; i++) {
		switch (queue[(queue_head + i) % JOB_QUEUE_LEN].type) {
		case JOB_SCAN:
		case JOB_PAIR:
		case JOB_CONNECT:
		case JOB_DISCONNECT:
		case JOB_FORGET:
			user_waiting = true;
			break;
		default:
			break;
		}
	}
	pthread_mutex_unlock(&lock);

	if (off) {
		return true;
	}
	if (user_waiting) {
		fprintf(stderr, "bluetooth: the user asked for something else; dropping the reconnect to %s\n", mac);
	}
	return user_waiting;
}

static void reconnect_last(void) {
	char mac[BT_MAC_MAX];
	snprintf(mac, sizeof(mac), "%s", config_get("wireless", BT_LAST_DEVICE_KEY, ""));
	if (!mac[0]) {
		// Said, not passed over in silence: "it does not reconnect by itself"
		// and "there is nothing written down to reconnect to" look identical
		// from the outside and are fixed in completely different places.
		fprintf(stderr, "bluetooth: no last device on file; nothing to go back to\n");
		return;
	}

	char sink[BT_MAC_MAX];
	if (btstack_audio_sink(sink, sizeof(sink))) {
		fprintf(stderr, "bluetooth: %s came back on its own\n", sink);
		return;
	}

	// Only if it is still a paired device: an address that has been forgotten
	// would be a discovery for nothing. Asked more than once, because at boot
	// bluez publishes its list a moment after it claims the bus, and a single
	// look would read as "forgotten" and skip the reconnection.
	bool paired = false;
	for (int attempt = 0; attempt < 10 && !paired; attempt++) {
		pthread_mutex_lock(&lock);
		for (int i = 0; i < g_paired_count && !paired; i++) {
			paired = strcasecmp(g_paired[i].mac, mac) == 0;
		}
		pthread_mutex_unlock(&lock);
		if (paired || attempt == 9) {
			break;
		}
		if (reconnect_should_stop(mac)) {
			return;
		}
		sleep_ms(500);
		refresh_devices();
	}
	if (!paired) {
		fprintf(stderr, "bluetooth: %s is not in the paired list; not going back to it\n", mac);
		return;
	}

	// First, a moment to come back by themselves -- which is what headphones do
	// the instant the adapter is powered. Waiting costs nothing when they do not
	// come back, and when they do it is the difference between a working link
	// and a link torn down by a command from this side.
	for (int waited = 0; waited < SELF_RECONNECT_GRACE_MS; waited += 250) {
		if (btstack_audio_sink(sink, sizeof(sink)) && strcasecmp(sink, mac) == 0) {
			fprintf(stderr, "bluetooth: %s came back on its own after %d ms\n", mac, waited);
			return;
		}
		if (reconnect_should_stop(mac)) {
			return;
		}
		sleep_ms(250);
	}

	// Up to three goes. The first can land while bluealsa has a process but has
	// not registered its endpoint with bluez yet, and then the link comes up with
	// no audio profile on it.
	for (int attempt = 1; attempt <= 3; attempt++) {
		// Checked again here and not only between the attempts: the previous
		// connect_device() took seconds, and this is the last moment before a
		// command goes out that could tear down somebody else's link.
		if (reconnect_should_stop(mac)) {
			return;
		}
		fprintf(stderr, "bluetooth: going back to %s (attempt %d)\n", mac, attempt);
		connect_device(mac);

		if (btstack_audio_sink(sink, sizeof(sink)) && strcasecmp(sink, mac) == 0) {
			fprintf(stderr, "bluetooth: %s is back with audio\n", mac);
			return;
		}
		if (reconnect_should_stop(mac)) {
			return;
		}
		sleep_ms(1500);
	}
	fprintf(stderr, "bluetooth: %s did not come back with an audio profile after three tries\n", mac);
}

// ---------------------------------------------------------------------------
// power
// ---------------------------------------------------------------------------

static void on_bus_change(void);

// The whole sequence, in order, with a wait on each step rather than a sleep.
static bool bring_stack_up(void) {
	uint32_t began = now_ms();

	ensure_dbus();

	// Before bluetoothd, not after: it opens the node when a pair of headphones
	// connects, and a node that appears later is a node the connection missed.
	ensure_uinput();

	if (!ensure_transport()) {
		return false;
	}

	// The bus connection has to exist before bluetoothd is waited for: the wait
	// is a question asked on it.
	if (!btstack_open()) {
		fprintf(stderr, "bluetooth: cannot open the system bus\n");
		return false;
	}
	btstack_set_change_cb(on_bus_change);

	if (!ensure_bluetoothd()) {
		return false;
	}
	btstack_register_agent();

	if (ensure_bluealsa()) {
		// The PCM objects that already existed were never announced to a
		// connection that had not opened yet.
		btstack_refresh_audio();
	} else {
		// Not a failed bring-up. The radio, bluez and the device list are all
		// up, so the settings page works and headphones can still be paired --
		// what cannot work is sound, because bluealsa is what carries it, and
		// bluez refuses Device1.Connect on a sink with no audio endpoint
		// registered. Said once and plainly, because from the outside it looks
		// like "it connects for three seconds and drops" and the cause is a
		// daemon that never started.
		fprintf(stderr, "bluetooth: NO AUDIO -- bluealsa is not on the bus, so no A2DP endpoint is "
						"registered and every headset connection will be refused\n");
	}

	btstack_set_powered(true);
	// Pairable always; visible only while the Bluetooth list page is open, which
	// the page asks for itself. A player permanently visible to every phone in
	// the room is not what anyone asked for.
	//
	// The timeout goes to 0 first. bluez counts 180 seconds from the moment
	// Discoverable turns true and then turns it off again on its own, which from
	// the other side looks like the player vanishing halfway through a search.
	btstack_set_pairable(true);
	btstack_set_discoverable_timeout(0);
	// Whatever the page last asked for, and not a flat false: turning Bluetooth
	// on from that page brings the stack up while the page is open, and a false
	// here would leave it invisible on the one screen where it should not be.
	pthread_mutex_lock(&lock);
	bool visible = g_discoverable;
	pthread_mutex_unlock(&lock);
	btstack_set_discoverable(visible);

	read_local_name();
	pthread_mutex_lock(&lock);
	char alias[BT_NAME_MAX];
	snprintf(alias, sizeof(alias), "%s", g_local_name);
	pthread_mutex_unlock(&lock);
	btstack_set_alias(alias);

	fprintf(stderr, "bluetooth: stack up in %u ms\n", now_ms() - began);
	return true;
}

// And down, in the order the stack is actually layered: the audio first, then
// the adapter, then the daemon under it, then the transport. The firmware's
// bt_suspend does the exact opposite -- hci0 down, kill bluetoothd, kill
// patchram, rfkill off, and only then kill bluealsa -- so bluealsa is asked to
// let go of a transport that has already gone.
static void bring_stack_down(bool deliberate) {
	btstack_discovery(false);

	// Whatever is connected is disconnected properly rather than having the
	// radio pulled out from under it: a link torn down by the transport going
	// away is one the headphones spend the next minute trying to rebuild.
	char sink[BT_MAC_MAX];
	if (btstack_audio_sink(sink, sizeof(sink))) {
		btstack_disconnect(sink);
		for (int waited = 0; waited < 1500 && btstack_audio_sink(NULL, 0); waited += 100) {
			sleep_ms(100);
		}
	}

	// Nothing may be holding a PCM on bluealsa when bluealsa goes away. This
	// closes the one the gapless path keeps open and waits for the playback
	// thread to let go of a live one; the routing is put back to the jack first,
	// so whatever it opens next is the jack.
	char was_playing_on[160];
	audio_get_output_device(was_playing_on, sizeof(was_playing_on));
	bool was_on_bluealsa = strncmp(was_playing_on, "bluealsa", 8) == 0;

	pthread_mutex_lock(&lock);
	g_state = BT_STATE_OFF;
	g_paired_count = 0;
	g_found_count = 0;
	g_codec_count = 0;
	g_codec_selected[0] = '\0';
	g_a2dp_connected = false;
	g_a2dp_mac[0] = '\0';
	g_serial++;
	pthread_mutex_unlock(&lock);

	apply_output_routing();

	if (was_on_bluealsa && !audio_suspend_quiesce(1500)) {
		fprintf(stderr, "bluetooth: the device was still busy; stopping bluealsa under it\n");
	}

	long freed = process_rss_kb("bluealsa") + process_rss_kb("bluetoothd") + process_rss_kb("bt-agent");

	stop_process("bluealsa");

	// The adapter is powered down while bluetoothd is still there to do it --
	// which is what tells the controller to stop advertising and to close its
	// links cleanly.
	btstack_set_powered(false);

	// The agent registration and every match go with the connection. Dropped
	// before bluetoothd is stopped, so dbus-daemon is not left activating it
	// again to answer a call from this side that was still in flight, which
	// leaves bluetoothd alive after Bluetooth has been switched off.
	btstack_close();

	stop_process("bluetoothd");
	stop_process("bt-agent");

	if (freed > 0) {
		fprintf(stderr, "bluetooth: daemons stopped, about %ld KB back\n", freed);
	}

	// The transport only for a deliberate switch-off. A park has to come back
	// in a hurry and re-uploading the chip's firmware would put a second and a
	// half in front of every wake with headphones connected.
	if (deliberate) {
		hci_down();
		if (stop_process("brcm_patchram_plus")) {
			write_file(BT_RFKILL, "0");
			fprintf(stderr, "bluetooth: radio off and the UART released\n");
		} else {
			fprintf(stderr, "bluetooth: brcm_patchram_plus is still running\n");
		}
	}
}

void bluetooth_power_down_stack(void) {
	if (!process_running("bluetoothd") && !process_running("bluealsa") &&
		!process_running("brcm_patchram_plus")) {
		return; // nothing was started: nothing to put down
	}
	long freed = process_rss_kb("bluealsa") + process_rss_kb("bluetoothd") + process_rss_kb("bt-agent");

	// The same order as bring_stack_down(), minus everything that needs the bus:
	// with the switch off there is no connection open and nothing connected.
	stop_process("bluealsa");
	stop_process("bluetoothd");
	stop_process("bt-agent");
	hci_down();
	if (stop_process("brcm_patchram_plus")) {
		write_file(BT_RFKILL, "0");
	}
	fprintf(stderr, "bluetooth: the boot-time stack is down, about %ld KB back\n", freed);
}

static void do_power(bool on, bool deliberate) {
	if (on) {
		// The bring-up runs every time, not only when a daemon is missing: after
		// a suspend to RAM all three processes still exist while the radio under
		// them has been through a power cycle and answers nothing. Every step
		// guards itself, so there is almost nothing to do when there is nothing
		// to do.
		if (!bring_stack_up()) {
			pthread_mutex_lock(&lock);
			g_state = BT_STATE_OFF;
			pthread_mutex_unlock(&lock);
			return;
		}

		pthread_mutex_lock(&lock);
		g_state = BT_STATE_ON;
		pthread_mutex_unlock(&lock);

		refresh_devices();

		// The radio is up and the list on screen is real, so the page stops
		// saying "turning on" here rather than at the end.
		//
		// What follows is going back to the last device, which is a different
		// thing with its own feedback on the row it concerns: a grace period
		// for headphones that reconnect by themselves, then up to three
		// attempts. Ten seconds on a bad day, and a device list sitting under a
		// "turning on" line for all of it reads as a radio that never finished
		// starting.
		pthread_mutex_lock(&lock);
		g_busy = false;
		pthread_mutex_unlock(&lock);

		reconnect_last();
		return;
	}

	bring_stack_down(deliberate);
}

// ---------------------------------------------------------------------------
// A2DP codecs
// ---------------------------------------------------------------------------

static bool connected_mac(char *out, size_t size) {
	if (btstack_audio_sink(out, size)) {
		return true;
	}
	bool found = false;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < g_paired_count; i++) {
		if (g_paired[i].connected) {
			copy_field(out, size, g_paired[i].mac, sizeof(g_paired[i].mac));
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	return found;
}

// ---------------------------------------------------------------------------
// choosing the codec
//
// A2DP does not negotiate by trying and falling back: the two ends exchange
// what they can do and ONE of them picks a single configuration out of the
// overlap. Which one picks depends on the direction. Driving a pair of
// headphones the player is the source and the choice is its own; as a receiver
// the phone is the source and the choice is the phone's -- all this device can
// do there is publish everything it can decode and let the phone take its pick.
//
// So this is the source half. bluealsa picks a configuration when the link comes
// up and that choice stands until something asks for another one, which can
// leave a pair of headphones that speaks LDAC on SBC. GetCodecs already answers
// with the overlap, so "start at the best and go down" is one ordering of that
// list plus a SelectCodec for each step until one is accepted.
//
// Once per connection and never again, because SelectCodec tears the transport
// down and builds it again -- which is the right thing to do before the music
// starts and the wrong thing to do while it is playing.
// ---------------------------------------------------------------------------

// Only what is listed here is ever chosen automatically. An unknown name is not
// guessed at: "better than SBC" is not something to assume about a codec this
// code has never heard of, and some of them (FastStream, the low latency ones)
// are deliberately worse.
static const struct {
	const char *name;
	int rank;
} CODEC_RANKS[] = {
	{"ldac", 50}, {"aptxhd", 40}, {"aptx", 30}, {"aac", 20}, {"sbc", 10},
};

// bluealsa spells it "aptX-HD"; a config file might say "aptx_hd". Compared on
// letters and digits only, in lower case.
static void codec_normalise(const char *in, char *out, size_t size) {
	size_t used = 0;
	for (const char *p = in; *p && used + 1 < size; p++) {
		if (isalnum((unsigned char)*p)) {
			out[used++] = (char)tolower((unsigned char)*p);
		}
	}
	out[used] = '\0';
}

static int codec_rank(const char *name) {
	char key[BT_CODEC_MAX];
	codec_normalise(name, key, sizeof(key));
	for (size_t i = 0; i < sizeof(CODEC_RANKS) / sizeof(CODEC_RANKS[0]); i++) {
		if (strcmp(key, CODEC_RANKS[i].name) == 0) {
			return CODEC_RANKS[i].rank;
		}
	}
	return 0; // unknown: left alone
}

// The best the two ends have in common, once per connection, with no setting in
// front of it: choosing the codec is the player's job and not a question to ask.
// A choice made by hand on the codec page changes the link that is up and is not
// written down, so the next connection starts from the top again.
static void do_auto_codec(const char *mac) {
	if (!mac || !mac[0]) {
		return;
	}
	if (strcasecmp(codec_auto_done, mac) == 0) {
		return;
	}
	snprintf(codec_auto_done, sizeof(codec_auto_done), "%s", mac);

	char codecs[BT_MAX_CODECS][BT_CODEC_MAX];
	char selected[BT_CODEC_MAX] = "";
	int count = btstack_codecs(mac, codecs, BT_MAX_CODECS, selected, sizeof(selected));
	if (count <= 0) {
		return;
	}

	// The floor is what is already in force -- there is no sense asking for
	// something no better than that -- and the ceiling drops to whatever was
	// just refused, so each step of the descent looks strictly below the last.
	int floor_rank = codec_rank(selected);
	int ceiling = 1000;

	for (;;) {
		int best = -1;
		for (int i = 0; i < count; i++) {
			int rank = codec_rank(codecs[i]);
			if (rank <= floor_rank || rank >= ceiling) {
				continue;
			}
			if (best < 0 || rank > codec_rank(codecs[best])) {
				best = i;
			}
		}
		if (best < 0) {
			return; // nothing better left on offer: what bluealsa chose stands
		}

		fprintf(stderr, "bluetooth: %s offers %s; asking for it instead of %s\n", mac, codecs[best],
				selected[0] ? selected : "what it chose");
		if (btstack_select_codec(mac, codecs[best])) {
			do_read_codecs();
			return;
		}

		// Refused: one step down and ask again. This is the only place A2DP
		// really does descend, and it is an error path rather than the normal
		// course of a negotiation.
		fprintf(stderr, "bluetooth: %s refused %s; trying the next one down\n", mac, codecs[best]);
		ceiling = codec_rank(codecs[best]);
	}
}

static void do_read_codecs(void) {
	char mac[BT_MAC_MAX];
	if (!connected_mac(mac, sizeof(mac))) {
		pthread_mutex_lock(&lock);
		g_codec_count = 0;
		g_codec_selected[0] = '\0';
		g_serial++;
		pthread_mutex_unlock(&lock);
		return;
	}

	char codecs[BT_MAX_CODECS][BT_CODEC_MAX];
	char selected[BT_CODEC_MAX] = "";
	int count = btstack_codecs(mac, codecs, BT_MAX_CODECS, selected, sizeof(selected));

	pthread_mutex_lock(&lock);
	memcpy(g_codecs, codecs, sizeof(char) * (size_t)count * BT_CODEC_MAX);
	g_codec_count = count;
	snprintf(g_codec_selected, sizeof(g_codec_selected), "%s", selected);
	g_serial++;
	pthread_mutex_unlock(&lock);
}

static void do_set_codec(const char *codec) {
	char mac[BT_MAC_MAX];
	if (!codec || !codec[0] || !connected_mac(mac, sizeof(mac))) {
		set_op(BT_OP_FAILED, NULL);
		return;
	}

	bool done = btstack_select_codec(mac, codec);
	// Renegotiating tears the stream down and builds it again; the PCM comes
	// back a moment later and the codec with it.
	sleep_ms(600);
	do_read_codecs();
	set_op(done ? BT_OP_OK : BT_OP_FAILED, codec);
}

// ---------------------------------------------------------------------------
// the receiving side
// ---------------------------------------------------------------------------

bool bluetooth_receiver_device(char *mac_out, int mac_size, char *name_out, int name_size) {
	if (mac_out && mac_size) {
		mac_out[0] = '\0';
	}
	if (name_out && name_size) {
		name_out[0] = '\0';
	}

	char mac[BT_MAC_MAX];
	if (!btstack_audio_source(mac, sizeof(mac))) {
		return false;
	}
	if (mac_out && mac_size) {
		snprintf(mac_out, (size_t)mac_size, "%s", mac);
	}

	// The name is whatever the paired list already knows; the address is the
	// only thing that is always there, so it stands in when it does not.
	if (name_out && name_size) {
		pthread_mutex_lock(&lock);
		for (int i = 0; i < g_paired_count; i++) {
			if (strcasecmp(g_paired[i].mac, mac) == 0 && g_paired[i].name[0]) {
				snprintf(name_out, (size_t)name_size, "%s", g_paired[i].name);
				break;
			}
		}
		pthread_mutex_unlock(&lock);
		if (!name_out[0]) {
			snprintf(name_out, (size_t)name_size, "%s", mac);
		}
	}
	return true;
}

// Renegotiating on the sink side tears the incoming stream down and builds it
// again, exactly as it does for headphones, so the page is told to read the new
// state once bluealsa has settled.
static void do_read_receiver(void);

static void do_set_receiver_codec(const char *codec) {
	char mac[BT_MAC_MAX];
	if (!codec || !codec[0] || !btstack_audio_source(mac, sizeof(mac))) {
		set_op(BT_OP_FAILED, NULL);
		return;
	}

	// The level the sending device had set, carried over by hand: the PCM the
	// renegotiation builds is a new object at bluealsa's default volume, and the
	// phone does not send its level again, so without this its volume keys stop
	// reaching the player after a codec change.
	int volume = -1;
	if (!btstack_pcm_volume_get(mac, true, &volume)) {
		volume = -1;
	}

	bool done = btstack_select_codec_dir(mac, true, codec);
	sleep_ms(600);

	if (done && volume >= 0) {
		// The PCM comes back a moment after the transport does, so one retry
		// covers the case where it is not there yet on the first try.
		if (!btstack_pcm_volume_set(mac, true, volume)) {
			sleep_ms(400);
			btstack_pcm_volume_set(mac, true, volume);
		}
	}

	do_read_receiver();
	set_op(done ? BT_OP_OK : BT_OP_FAILED, codec);
}

static void do_read_receiver(void) {
	char mac[BT_MAC_MAX];
	btstack_stream_t info;
	bool sending = btstack_audio_source(mac, sizeof(mac));
	bool have = sending && btstack_stream_info(mac, true, &info);

	// What the sender is doing, read outright: bluez announces every later
	// change, but nothing has been announced yet at the moment the link comes
	// up, and that is exactly when the first key can arrive.
	//
	// Asked of the device rather than of the stream: a transport that is
	// momentarily unreadable is not a device that stopped playing.
	if (sending) {
		btstack_refresh_media_status(mac);
	} else {
		btstack_forget_media();
	}

	pthread_mutex_lock(&lock);
	if (have) {
		snprintf(g_rx_stream.codec, sizeof(g_rx_stream.codec), "%s", info.codec);
		g_rx_stream.rate = info.rate;
		g_rx_stream.channels = info.channels;
		g_rx_stream.bits = info.bits;
	} else {
		memset(&g_rx_stream, 0, sizeof(g_rx_stream));
	}
	g_rx_serial++;
	g_serial++;
	pthread_mutex_unlock(&lock);
}

static void do_media_command(const char *member) {
	char mac[BT_MAC_MAX];
	if (!member || !member[0] || !btstack_audio_source(mac, sizeof(mac))) {
		return;
	}
	if (!btstack_media_command(mac, member)) {
		fprintf(stderr, "bluetooth: %s was refused by %s\n", member, mac);
		// The command did not land, so the assumption made when it was posted
		// did not happen: ask what is really true rather than leave the guess
		// standing.
		btstack_refresh_media_status(mac);
	}
}

bool bluetooth_receiver_playing(void) {
	char status[32];
	if (!btstack_media_status(status, sizeof(status))) {
		return false;
	}
	return strcasecmp(status, "playing") == 0;
}

void bluetooth_receiver_note_playing(bool playing) { btstack_note_media_status(playing ? "playing" : "paused"); }

bool bluetooth_receiver_stream(bt_stream_t *out) {
	if (!out) {
		return false;
	}
	pthread_mutex_lock(&lock);
	*out = g_rx_stream;
	pthread_mutex_unlock(&lock);
	return out->codec[0] != '\0';
}

// What the phone offered on the link that is up, and which of them it is using.
// Unlike the headphone direction this is read on the spot rather than cached:
// the list is only ever wanted while the receiver page is open, and a poll for
// it would be a D-Bus round trip every half second for nothing.
int bluetooth_receiver_codecs(char out[][BT_CODEC_MAX], int max, char *selected, int selected_size) {
	if (selected && selected_size) {
		selected[0] = '\0';
	}
	if (!out || max <= 0) {
		return 0;
	}

	char mac[BT_MAC_MAX];
	if (!btstack_audio_source(mac, sizeof(mac))) {
		return 0;
	}
	return btstack_codecs_dir(mac, true, out, max, selected, selected_size ? (size_t)selected_size : 0);
}

bool bluetooth_receiver_track(bt_track_t *out) {
	if (!out) {
		return false;
	}
	btstack_track_t track;
	bool have = btstack_media_track(&track);
	snprintf(out->title, sizeof(out->title), "%s", track.title);
	snprintf(out->artist, sizeof(out->artist), "%s", track.artist);
	snprintf(out->album, sizeof(out->album), "%s", track.album);
	out->duration_ms = track.duration_ms;
	return have;
}

void bluetooth_receiver_set_codec(const char *codec) {
	if (!codec || !codec[0]) {
		return;
	}
	job_t job = {.type = JOB_RX_CODEC};
	snprintf(job.text, sizeof(job.text), "%s", codec);
	post_job(&job);
}

unsigned bluetooth_receiver_stream_serial(void) {
	pthread_mutex_lock(&lock);
	unsigned now = g_rx_serial;
	pthread_mutex_unlock(&lock);
	// The metadata moves on its own, announced by the sender rather than
	// noticed here, so it carries its own counter into this one.
	return now + btstack_media_serial();
}

void bluetooth_refresh_receiver(void) {
	job_t job = {.type = JOB_RX_INFO};
	post_job(&job);
}

void bluetooth_receiver_command(const char *member) {
	if (!member || !member[0]) {
		return;
	}
	job_t job = {.type = JOB_MEDIA};
	snprintf(job.text, sizeof(job.text), "%s", member);
	post_job(&job);
}

static void do_set_volume(int percent) {
	// btvolume owns the level: it writes it on org.bluealsa.PCM1 and follows what
	// the headphones send back, both over the bus. There is no command-line
	// fallback any more, and none is needed -- the bus path exists exactly when
	// bluealsa does, which is exactly when there is a stream to set a level on.
	if (btvolume_available()) {
		btvolume_notify_local(percent);
	}
}

// The name of a known device, for the toast. Falls back to the address.
static void name_for(const char *mac, char *out, size_t size) {
	snprintf(out, size, "%s", mac);

	pthread_mutex_lock(&lock);
	for (int i = 0; i < g_paired_count; i++) {
		if (strcasecmp(g_paired[i].mac, mac) == 0) {
			snprintf(out, size, "%s", g_paired[i].name);
			pthread_mutex_unlock(&lock);
			return;
		}
	}
	for (int i = 0; i < g_found_count; i++) {
		if (strcasecmp(g_found[i].mac, mac) == 0) {
			snprintf(out, size, "%s", g_found[i].name);
			pthread_mutex_unlock(&lock);
			return;
		}
	}
	pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// what the bus says
// ---------------------------------------------------------------------------

// Called on the D-Bus reader thread. It must not make a call of its own -- the
// reply would have to be read by the thread that is still inside the handler --
// so it only wakes the worker, which re-reads properly.
static void on_bus_change(void) {
	job_t job = {.type = JOB_REFRESH};
	post_job(&job);
}

// The firmware's patched bluetoothd also pushes "BT:CHANGE <mac> AudioSink
// connected" at whoever holds /var/run/sys_client. Nothing is decided from it
// any more -- bluealsa's PCM objects are the truth, and they arrive on the bus
// -- but it is a free hint that something moved, so it is used as one.
static void notification_cb(const char *message) {
	if (strncmp(message, "BT:", 3) != 0) {
		return;
	}
	if (strncmp(message, "BT:CHANGE ", 10) == 0 || strncmp(message, "BT:CODEC ", 9) == 0) {
		job_t job = {.type = JOB_REFRESH};
		post_job(&job);
	}
}

// ---------------------------------------------------------------------------
// worker
// ---------------------------------------------------------------------------

static bool take_job(job_t *out) {
	if (queue_count == 0) {
		return false;
	}
	*out = queue[queue_head];
	queue_head = (queue_head + 1) % JOB_QUEUE_LEN;
	queue_count--;
	return true;
}

// A refresh already waiting is a refresh that will read whatever the next
// signal would have made it read. Without this, a device that connects fires
// several PropertiesChanged in a row and fills the queue with the same job,
// pushing out the connection the user actually asked for.
static bool queue_has(job_type_t type) {
	for (int i = 0; i < queue_count; i++) {
		if (queue[(queue_head + i) % JOB_QUEUE_LEN].type == type) {
			return true;
		}
	}
	return false;
}

static void post_job(const job_t *job) {
	pthread_mutex_lock(&lock);
	bool duplicate = job->type == JOB_REFRESH && queue_has(JOB_REFRESH);
	bool queued = duplicate || queue_count < JOB_QUEUE_LEN;
	if (!duplicate && queued) {
		queue[(queue_head + queue_count) % JOB_QUEUE_LEN] = *job;
		queue_count++;
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);

	if (!queued) {
		fprintf(stderr, "bluetooth: job queue full (%d); dropping a job of type %d\n", JOB_QUEUE_LEN, (int)job->type);
	}
}

static void *bluetooth_worker(void *arg) {
	(void)arg;
	thread_be_background("bluetooth");

	uint32_t last_poll = 0;

	for (;;) {
		job_t job;
		bool have_job;

		pthread_mutex_lock(&lock);
		have_job = take_job(&job);
		if (!have_job) {
			// How long to sleep before the next round. A job wakes the thread
			// anyway, so a longer wait slows nothing the user can ask for; it
			// only drops the wake-ups spent finding nothing to do.
			//
			// The deadline goes through deadline_in_ms(): computing tv_nsec by
			// hand overflows on this 32-bit platform, which makes the wait return
			// EINVAL at once and spins this thread.
			uint32_t wait_ms =
				!g_enabled ? BT_IDLE_WAIT_MS : (power_screen_is_on() ? BT_POLL_MS : BT_POLL_STANDBY_MS);
			// While a sink is missing but its link is still up, the answer is
			// due within the grace above: the usual poll is far too slow to
			// give it.
			if (a2dp_missing_since != 0 && wait_ms > 500) {
				wait_ms = 500;
			}
			struct timespec deadline;
			deadline_in_ms(&deadline, wait_ms);
			pthread_cond_timedwait(&wake, &lock, &deadline);
			have_job = take_job(&job);
		}
		bool enabled = g_enabled;
		bool audio_active = enabled && g_a2dp_connected;
		char active_mac[BT_MAC_MAX];
		snprintf(active_mac, sizeof(active_mac), "%s", g_a2dp_mac);
		pthread_mutex_unlock(&lock);

		// The stream btvolume bridges, and the device airpods talks to. Connect,
		// disconnect and reconnect all reach them as this address changing, so
		// nothing else has to tell them.
		bool have_active = audio_active && active_mac[0];
		btvolume_set_device(have_active ? active_mac : NULL);
		if (have_active) {
			char active_name[BT_NAME_MAX];
			name_for(active_mac, active_name, sizeof(active_name));
			airpods_set_device(active_mac, active_name);
		} else {
			airpods_set_device(NULL, NULL);
		}

		// Asked for by the track start, answered here, read by the line two
		// seconds later.
		pthread_mutex_lock(&lock);
		bool want_streams = g_a2dp_streams_wanted;
		g_a2dp_streams_wanted = false;
		pthread_mutex_unlock(&lock);
		if (want_streams && have_active) {
			char streams[sizeof(g_a2dp_streams)];
			if (btstack_a2dp_streams(active_mac, streams, sizeof(streams))) {
				pthread_mutex_lock(&lock);
				snprintf(g_a2dp_streams, sizeof(g_a2dp_streams), "%s", streams);
				g_a2dp_streams_at = now_ms();
				pthread_mutex_unlock(&lock);
			}
		}

		if (have_job) {
			char name[BT_NAME_MAX];

			switch (job.type) {
			case JOB_POWER:
				do_power(job.arg_int > 0, job.arg_int < 0);
				pthread_mutex_lock(&lock);
				g_busy = false;
				pthread_mutex_unlock(&lock);
				break;

			case JOB_SCAN:
				do_scan();
				break;

			case JOB_SCAN_STOP:
				do_scan_stop();
				break;

			case JOB_REFRESH:
				refresh_devices();
				break;

			case JOB_PAIR:
				name_for(job.mac, name, sizeof(name));
				// Pairing and connecting are one step from the user's side: a
				// device that has just been paired and does nothing is not what
				// anyone meant by tapping their headphones.
				set_op(pair_and_connect(job.mac) ? BT_OP_OK : BT_OP_FAILED, name);
				refresh_devices();
				break;

			case JOB_CONNECT:
				name_for(job.mac, name, sizeof(name));
				set_op(connect_device(job.mac) ? BT_OP_OK : BT_OP_FAILED, name);
				refresh_devices();
				break;

			case JOB_DISCONNECT:
				name_for(job.mac, name, sizeof(name));
				btstack_disconnect(job.mac);
				sleep_ms(400);
				refresh_devices();
				set_op(BT_OP_OK, name);
				break;

			case JOB_FORGET:
				name_for(job.mac, name, sizeof(name));
				btstack_disconnect(job.mac);
				set_op(btstack_remove(job.mac) ? BT_OP_OK : BT_OP_FAILED, name);
				forget_last_device(job.mac);
				refresh_devices();
				break;

			case JOB_CODECS:
				do_read_codecs();
				break;

			case JOB_LDAC_QUALITY:
				if (process_running("bluealsa")) {
					fprintf(stderr, "bluetooth: restarting bluealsa for LDAC %s\n", bluetooth_ldac_quality());
					stop_process("bluealsa");
					sleep_ms(300);
					codec_auto_done[0] = '\0'; // the new daemon negotiates again
					ensure_bluealsa();
				}
				break;

			case JOB_DISCOVERABLE:
				btstack_set_discoverable(job.arg_int != 0);
				break;

			case JOB_SET_NAME: {
				pthread_mutex_lock(&lock);
				char alias[BT_NAME_MAX];
				snprintf(alias, sizeof(alias), "%s", g_local_name);
				pthread_mutex_unlock(&lock);
				btstack_set_alias(alias);
				break;
			}

			case JOB_SET_CODEC:
				do_set_codec(job.text);
				break;

			case JOB_VOLUME: {
				// Whatever the last value posted was: a held volume key must not
				// turn into fifty writes.
				int percent;
				pthread_mutex_lock(&lock);
				percent = g_pending_volume;
				g_pending_volume = -1;
				pthread_mutex_unlock(&lock);
				if (percent >= 0) {
					do_set_volume(percent);
				}
				break;
			}

			case JOB_RX_INFO:
				do_read_receiver();
				break;

			case JOB_RX_CODEC:
				do_set_receiver_codec(job.text);
				break;

			case JOB_MEDIA:
				do_media_command(job.text);
				break;

			case JOB_NONE:
			default:
				break;
			}
			continue;
		}

		// The backstop. bluez says what changed the moment it happens, so this
		// only catches what a lost signal or a dropped connection would have
		// hidden -- including the connection itself having gone.
		uint32_t poll_period = power_screen_is_on() ? BT_POLL_MS : BT_POLL_STANDBY_MS;
		if (enabled && (uint32_t)(now_ms() - last_poll) >= poll_period) {
			last_poll = now_ms();
			if (!btstack_alive()) {
				fprintf(stderr, "bluetooth: the bus connection dropped; opening it again\n");
				if (btstack_open()) {
					btstack_set_change_cb(on_bus_change);
					btstack_register_agent();
					btstack_refresh_audio();
				}
			}
			refresh_devices();
		}
	}

	return NULL;
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

void bluetooth_init(void) {
	if (worker_running) {
		return;
	}

	g_enabled = config_get_int("wireless", "bluetooth", 0) != 0;
	g_volume_sync = config_get_int("wireless", "bt_volume_sync", 1) != 0;

	// Before anything asks for it. Bring-up reads it too, but the settings page
	// shows the name with the radio off, and until this the answer was the
	// built-in fallback rather than what the firmware's file says.
	read_local_name();

	btvolume_init();
	btvolume_set_sync(g_volume_sync);
	airpods_init();

	if (pthread_create(&worker, NULL, bluetooth_worker, NULL) != 0) {
		fprintf(stderr, "bluetooth: could not start the worker thread\n");
		g_enabled = false;
		return;
	}
	pthread_detach(worker);
	worker_running = true;

	sysserver_listen(notification_cb);

	if (g_enabled && bluetooth_available()) {
		job_t job = {.type = JOB_POWER, .arg_int = 1};
		g_busy = true;
		post_job(&job);
	} else if (g_enabled) {
		g_enabled = false;
	}
}

bool bluetooth_get_enabled(void) {
	pthread_mutex_lock(&lock);
	bool on = g_enabled;
	pthread_mutex_unlock(&lock);
	return on;
}

static void set_enabled(bool on, bool remember) {
	if (!worker_running) {
		return;
	}

	pthread_mutex_lock(&lock);
	if (g_enabled == on) {
		pthread_mutex_unlock(&lock);
		return;
	}
	g_enabled = on;
	g_busy = true;
	if (!on) {
		g_state = BT_STATE_OFF;
		g_found_count = 0;
		g_scanning = false;
		g_serial++;

		// Everything queued concerns a radio that is going off, and whatever the
		// worker is waiting on no longer matters. Dropping the queue keeps the
		// "turning off" notice from sitting there for as long as a pairing
		// attempt takes.
		queue_head = 0;
		queue_count = 0;
	}
	pthread_mutex_unlock(&lock);

	if (remember) {
		config_set_int("wireless", "bluetooth", on ? 1 : 0);
		config_save();
	}

	// The sign carries which kind of "off" this is: see do_power().
	job_t job = {.type = JOB_POWER, .arg_int = on ? 1 : (remember ? -1 : 0)};
	post_job(&job);
}

void bluetooth_set_enabled(bool on) { set_enabled(on, true); }

// The idle park. Same radio, but the setting is left alone: what the user chose
// must survive a battery pulled while the device was asleep in a pocket, and a
// park written to the config comes back as "the user switched Bluetooth off".
void bluetooth_set_enabled_transient(bool on) { set_enabled(on, false); }

void bluetooth_notify_resume(void) {
	if (!worker_running) {
		return;
	}
	pthread_mutex_lock(&lock);
	bool on = g_enabled;
	pthread_mutex_unlock(&lock);
	if (!on) {
		return;
	}
	// The same job the switch posts, on a radio that is already meant to be on:
	// the bring-up runs whatever it finds, which is what a resume needs, and
	// then goes back to the headphones.
	job_t job = {.type = JOB_POWER, .arg_int = 1};
	post_job(&job);
}

bool bluetooth_busy(void) {
	pthread_mutex_lock(&lock);
	bool busy = g_busy;
	pthread_mutex_unlock(&lock);
	return busy;
}

bt_state_t bluetooth_get_state(void) {
	pthread_mutex_lock(&lock);
	bt_state_t state = g_enabled ? g_state : BT_STATE_OFF;
	pthread_mutex_unlock(&lock);
	return state;
}

const char *bluetooth_local_name(void) { return g_local_name; }

bool bluetooth_set_local_name(const char *name) {
	if (!name) {
		return false;
	}

	char wanted[BT_NAME_MAX];
	snprintf(wanted, sizeof(wanted), "%s", name);

	size_t length = strlen(wanted);
	while (length > 0 && (wanted[length - 1] == ' ' || wanted[length - 1] == '\t')) {
		wanted[--length] = '\0';
	}
	const char *start = wanted;
	while (*start == ' ' || *start == '\t') {
		start++;
	}
	if (!*start) {
		return false;
	}

	pthread_mutex_lock(&lock);
	snprintf(g_local_name, sizeof(g_local_name), "%s", start);
	bool enabled = g_enabled;
	pthread_mutex_unlock(&lock);

	config_set("wireless", "bt_name", start);
	config_save();

	// A radio that is off has nothing to tell: bring-up reads the name itself.
	if (enabled) {
		job_t job = {.type = JOB_SET_NAME};
		post_job(&job);
	}
	return true;
}

uint32_t bluetooth_devices_serial(void) {
	pthread_mutex_lock(&lock);
	uint32_t serial = g_serial;
	pthread_mutex_unlock(&lock);
	return serial;
}

int bluetooth_get_paired(bt_device_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	pthread_mutex_lock(&lock);
	int count = g_paired_count < max ? g_paired_count : max;
	memcpy(out, g_paired, sizeof(bt_device_t) * (size_t)count);
	pthread_mutex_unlock(&lock);
	return count;
}

int bluetooth_get_found(bt_device_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	pthread_mutex_lock(&lock);
	int count = g_found_count < max ? g_found_count : max;
	memcpy(out, g_found, sizeof(bt_device_t) * (size_t)count);
	pthread_mutex_unlock(&lock);
	return count;
}

void bluetooth_scan_start(void) {
	pthread_mutex_lock(&lock);
	bool skip = g_scanning || !g_enabled || g_busy;
	if (!skip) {
		g_scanning = true;
	}
	pthread_mutex_unlock(&lock);

	if (skip) {
		return;
	}

	job_t job = {.type = JOB_SCAN};
	post_job(&job);
}

void bluetooth_scan_stop(void) {
	job_t job = {.type = JOB_SCAN_STOP};
	post_job(&job);
}

bool bluetooth_scan_running(void) {
	pthread_mutex_lock(&lock);
	bool running = g_scanning;
	pthread_mutex_unlock(&lock);
	return running;
}

void bluetooth_refresh(void) {
	pthread_mutex_lock(&lock);
	bool skip = !g_enabled;
	pthread_mutex_unlock(&lock);
	if (skip) {
		return;
	}
	job_t job = {.type = JOB_REFRESH};
	post_job(&job);
}

static void post_device_job(job_type_t type, const char *mac) {
	if (!mac || !mac[0]) {
		return;
	}
	set_op(BT_OP_BUSY, NULL);

	job_t job = {.type = type};
	snprintf(job.mac, sizeof(job.mac), "%s", mac);
	post_job(&job);
}

void bluetooth_pair(const char *mac) { post_device_job(JOB_PAIR, mac); }
void bluetooth_connect(const char *mac) { post_device_job(JOB_CONNECT, mac); }
void bluetooth_disconnect(const char *mac) { post_device_job(JOB_DISCONNECT, mac); }
void bluetooth_forget(const char *mac) { post_device_job(JOB_FORGET, mac); }

bool bluetooth_connected_device(bt_device_t *out) {
	bool found = false;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < g_paired_count; i++) {
		if (g_paired[i].connected) {
			if (out) {
				*out = g_paired[i];
			}
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	return found;
}

// ---------------------------------------------------------------------------
// A2DP
// ---------------------------------------------------------------------------

bool bluetooth_audio_active(void) {
	pthread_mutex_lock(&lock);
	bool active = g_enabled && g_a2dp_connected;
	pthread_mutex_unlock(&lock);
	return active;
}

void bluetooth_request_a2dp_streams(void) {
	pthread_mutex_lock(&lock);
	g_a2dp_streams_wanted = true;
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
}

bool bluetooth_a2dp_streams(char *out, int size, int *age_ms) {
	if (!out || size <= 0) {
		return false;
	}
	pthread_mutex_lock(&lock);
	bool have = g_a2dp_streams[0] != '\0';
	snprintf(out, (size_t)size, "%s", g_a2dp_streams);
	if (age_ms) {
		*age_ms = have ? (int)(now_ms() - g_a2dp_streams_at) : -1;
	}
	pthread_mutex_unlock(&lock);
	return have;
}

void bluetooth_output_pcm(char *out, int size) {
	if (!out || size <= 0) {
		return;
	}
	out[0] = '\0';

	char mac[BT_MAC_MAX];
	pthread_mutex_lock(&lock);
	mac[0] = '\0';
	// The bluealsa PCM, and nothing else. It exists exactly when there is a sink
	// to write to; an ACL link can be up with no audio profile on it at all.
	if (g_enabled && g_a2dp_connected) {
		snprintf(mac, sizeof(mac), "%s", g_a2dp_mac);
	}
	pthread_mutex_unlock(&lock);

	if (!mac[0]) {
		return;
	}

	// alsa-lib already knows what this means: /etc/alsa/conf.d/20-bluealsa.conf
	// defines pcm.bluealsa as a `plug` around the bluealsa plugin, so the rate
	// and format conversion the headphones need comes for free.
	snprintf(out, (size_t)size, "bluealsa:DEV=%s,PROFILE=a2dp", mac);
}

int bluetooth_get_codecs(char out[][BT_CODEC_MAX], int max, char *selected, int selected_size) {
	pthread_mutex_lock(&lock);
	int count = g_codec_count < max ? g_codec_count : max;
	for (int i = 0; i < count; i++) {
		// Copied by length rather than with %s: the compiler cannot see that
		// each row of the table is terminated, and warns that a name could run
		// on into the next one.
		size_t length = strnlen(g_codecs[i], BT_CODEC_MAX - 1);
		memcpy(out[i], g_codecs[i], length);
		out[i][length] = '\0';
	}
	if (selected && selected_size > 0) {
		snprintf(selected, (size_t)selected_size, "%s", g_codec_selected);
	}
	pthread_mutex_unlock(&lock);
	return count;
}

void bluetooth_refresh_codecs(void) {
	job_t job = {.type = JOB_CODECS};
	post_job(&job);
}

int bluetooth_local_codecs(char out[][BT_CODEC_MAX], int max, bool receiving) {
	int have = receiving ? local_sink_count : local_source_count;
	int count = have < max ? have : max;
	for (int i = 0; i < count; i++) {
		const char *name = receiving ? local_sink_codecs[i] : local_source_codecs[i];
		size_t length = strnlen(name, BT_CODEC_MAX - 1);
		memcpy(out[i], name, length);
		out[i][length] = '\0';
	}
	return count;
}

static bool ldac_quality_known(const char *mode) {
	return strcmp(mode, "abr") == 0 || strcmp(mode, "high") == 0 || strcmp(mode, "standard") == 0 ||
		   strcmp(mode, "mobile") == 0;
}

const char *bluetooth_ldac_quality(void) {
	const char *mode = config_get("wireless", "bt_ldac_quality", "abr");
	return ldac_quality_known(mode) ? mode : "abr";
}

void bluetooth_set_ldac_quality(const char *mode) {
	if (!mode || !ldac_quality_known(mode) || strcmp(mode, bluetooth_ldac_quality()) == 0) {
		return;
	}
	config_set("wireless", "bt_ldac_quality", mode);
	config_save();

	// The option is read when the daemon starts and never again, so a running
	// one has to be replaced for the choice to mean anything now. Queued on the
	// worker like everything else that touches the stack.
	job_t job = {.type = JOB_LDAC_QUALITY};
	post_job(&job);
}

void bluetooth_set_codec(const char *codec) {
	if (!codec || !codec[0]) {
		return;
	}
	// Sent, not written down. The player picks the best one on its own at every
	// connection; this is the one link that is up being told to use something
	// else, and it lasts as long as that link does.
	set_op(BT_OP_BUSY, NULL);

	job_t job = {.type = JOB_SET_CODEC};
	snprintf(job.text, sizeof(job.text), "%s", codec);
	post_job(&job);
}

void bluetooth_set_discoverable(bool on) {
	pthread_mutex_lock(&lock);
	bool enabled = g_enabled;
	g_discoverable = on;
	pthread_mutex_unlock(&lock);
	if (!enabled) {
		return; // nothing to tell bluez; bring-up reads the flag
	}
	job_t job = {.type = JOB_DISCOVERABLE, .arg_int = on ? 1 : 0};
	post_job(&job);
}

bool bluetooth_volume_sync(void) {
	pthread_mutex_lock(&lock);
	bool on = g_volume_sync;
	pthread_mutex_unlock(&lock);
	return on;
}

void bluetooth_set_volume_sync(bool on) {
	pthread_mutex_lock(&lock);
	g_volume_sync = on;
	pthread_mutex_unlock(&lock);

	btvolume_set_sync(on);
	config_set_int("wireless", "bt_volume_sync", on ? 1 : 0);
	config_save();
}

// The player's volume has moved. It is handed to the stream whichever way the
// setting is: synchronised, bluealsa passes it on to the headphones as an AVRCP
// absolute volume; separate, bluealsa attenuates the stream itself and the
// headphones keep their own level.
void bluetooth_notify_volume(int percent) {
	pthread_mutex_lock(&lock);
	bool wanted = g_enabled && g_a2dp_connected;
	bool already_queued = g_pending_volume >= 0;
	if (wanted) {
		g_pending_volume = percent;
	}
	pthread_mutex_unlock(&lock);

	// One job in flight is enough: it picks up whatever the latest value is when
	// it runs, so holding the volume key costs one AVRCP write, not one per
	// step.
	if (wanted && !already_queued) {
		job_t job = {.type = JOB_VOLUME};
		post_job(&job);
	}
}

bt_op_t bluetooth_take_op_result(char *name_out, int name_size) {
	pthread_mutex_lock(&lock);
	bt_op_t op = g_op;
	if (name_out && name_size > 0) {
		snprintf(name_out, (size_t)name_size, "%s", g_op_name);
	}
	if (op == BT_OP_OK || op == BT_OP_FAILED) {
		g_op = BT_OP_IDLE;
	}
	pthread_mutex_unlock(&lock);
	return op;
}
