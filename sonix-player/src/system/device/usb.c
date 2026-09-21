#include "usb.h"

#include "src/system/device/adb.h"
#include "src/system/device/power.h"

#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/system/library/audiobookdb.h"
#include "src/system/library/library.h"
#include "src/system/core/logging.h"
#include "src/system/device/sysserver.h"
#include "src/system/audio/usbaudio.h"
#include "src/system/core/utils.h"

#include "lvgl/lvgl.h"

#include "src/gui/library/browser.h"
#include "src/gui/shell/gui.h"
#include "src/system/audio/audio.h"
#include "src/system/playback/device_state.h"


// The ADB init script (S440adb) builds its own gadget under this directory
// and binds it to the device's one USB controller. That matters here: with
// ADB enabled the mass-storage script's own gadget can never bind (the
// controller is busy), which on the stock firmware is resolved by making the
// two exclusive (adbon/adboff swap them). This player does better: when the
// ADB gadget owns the controller, the mass-storage function is added into
// *that* gadget (a composite device, the way Android phones expose adb+MTP
// together), so the PC gets the disk and the adb session both.
#define ADB_GADGET "/sys/kernel/config/usb_gadget/adb_demo"

// VBUS detection. The first node is what the stock binary polls; the second
// is the charger's own (the stock reads that one too). Which of them actually
// flips to 1 on this hardware differs between drivers, so *all* of them are
// polled and any reading 1 means a cable -- the same any-supply logic the
// battery scan uses, which is why the charge icon worked while a single-node
// poll here saw nothing.
static const char *const VBUS_CANDIDATES[] = {
	"/sys/class/power_supply/usb/online",
	"/sys/class/power_supply/mp2731-charger/online",
};

// How often the cable is looked at. Attaching feels immediate at one second;
// the check is two sysfs reads.
#define POLL_SECONDS 1
#define POLL_SECONDS_STANDBY 4

static char sd_device[128];
static char sd_mount[256];

static volatile bool storage_active;

// Which way the gadget was exported, so the restore tears down the same one.
static enum { EXPORT_NONE, EXPORT_OWN_GADGET, EXPORT_ADB_COMPOSITE } export_mode;

// What the PC should see in its "device connected" notification. The
// firmware's mass-storage script hardcodes "HiBy R1" (it shipped on the R1
// and nobody updated the strings); the gadget is pre-created here with the
// right identity before the script runs, and the script keeps it because it
// only writes descriptors into a gadget it created itself.
#define USB_ID_VENDOR "0x32BB"
// The product id the stock firmware gives its storage gadget (config.json calls
// it usb_pid, and keeps 0x0004 for the DAC's dac_pid). They were the same number
// here, so a host that had already met this device as a sound card was being
// shown a card reader wearing the same identity.
#define USB_ID_PRODUCT "0x0101"
#define USB_MANUFACTURER "HiBy"
#ifdef BOARD_R1
	#define USB_PRODUCT "R1"
#else
	#define USB_PRODUCT "R3 Pro II"
#endif

// What the LUN answers to a SCSI INQUIRY: vendor (8), product (16), revision
// (4). Without it the mass-storage function reports "Linux / File-Stor Gadget",
// which is what a bare gadget looks like; the stock writes its own, and some
// hosts -- Android among them -- are fussier about a card reader than a PC is.
#ifdef BOARD_R1
	#define USB_INQUIRY_STRING "HiBy    R1 U-DISK0100"
#else
	#define USB_INQUIRY_STRING "HiBy    R3 Pro II U-DISK0100"
#endif

#define STORAGE_GADGET "/sys/kernel/config/usb_gadget/android0"

bool usb_storage_active(void) { return storage_active; }

// ---------------------------------------------------------------------------
// UI notifications: the worker thread must not touch LVGL, so everything the
// user sees goes through gui_post onto the interface thread.
// ---------------------------------------------------------------------------

static void popup_connected_cb(void *unused) {
	(void)unused;
	gui_notify_popup("usb_storage_shared");
}

static void popup_disconnected_cb(void *unused) {
	(void)unused;
	// Covers the unplug and the PC-side eject alike.
	gui_notify_popup("usb_storage_available_again");
	browser_refresh(); // the PC may have changed the card content
}

static bool vbus_present(void) {
	for (size_t i = 0; i < sizeof(VBUS_CANDIDATES) / sizeof(VBUS_CANDIDATES[0]); i++) {
		char *content = read_file_content(VBUS_CANDIDATES[i]);
		if (!content) {
			continue;
		}
		bool on = content[0] == '1';
		free(content);
		if (on) {
			return true;
		}
	}
	return false;
}

// True while the ADB gadget exists and is bound to the USB controller (its
// UDC file holds the controller's name). Bound means the controller is busy
// and the mass-storage script's own gadget could never attach.
static bool adb_gadget_bound(void) {
	char *content = read_file_content(ADB_GADGET "/UDC");
	if (!content) {
		return false;
	}
	content[strcspn(content, "\r\n")] = '\0';
	bool bound = content[0] != '\0';
	free(content);
	return bound;
}

// Set while DAC mode owns the USB controller: nothing in this file may touch
// the gadget then. Without it the watcher thread sees a cable, calls
// ensure_storage_gadget() and rebuilds the mass-storage gadget straight over
// the audio one, taking the stream with it and handing the computer a card it
// should not have. Honoured in the two places that build or bind anything.
static volatile bool dac_owns_usb;

// Entering and leaving DAC mode moves the card back and forth exactly the way
// a cable being plugged in does, which would announce "Memoria di nuovo
// disponibile" or "Collegato al PC" for something the user did not do and
// cannot see. The popups are silenced for the duration of the transition.
static volatile bool suppress_popups;
static void ensure_storage_gadget(void);
static void storage_restore(void);

void usb_set_dac_mode(bool on) {
	// The flag goes up before anything else: setting it afterwards leaves a
	// window in which the watcher thread can wake, see a cable and re-export
	// the card this call is in the middle of taking back.
	suppress_popups = true;
	if (on) {
		dac_owns_usb = true;
		// Take the card back from the computer. In DAC mode the player is a
		// sound card and nothing else: handing over the microSD at the same
		// time is not a bonus, it is the card being pulled out from under the
		// player while it works.
		storage_restore();
	}
	dac_owns_usb = on;
	fprintf(stderr, "usb: controller %s\n", on ? "handed to DAC mode" : "back from DAC mode");
	if (!on) {
		ensure_storage_gadget();
	}
	suppress_popups = false;
}

bool usb_dac_mode(void) {
	return dac_owns_usb;
}

// ---------------------------------------------------------------------------
// The Type-C role
//
// This is a dual-role port: the board carries a FUSB302B on the CC lines
// (module_driver/fusb302b.sh loads it with otg_id_gpio=PB24) and an MP2731 that
// can push 5 V back out, so the socket can be either end of a cable. Left to
// itself the controller advertises both and negotiates, which against a PC is
// no contest -- a PC only ever sources -- and this device lands as the
// peripheral every time.
//
// Against a phone it is a contest, and one this device tends to win. A phone is
// dual-role too and Android implements Try.SNK, meaning it would rather be the
// sink; so the two agree that the DAP is the host, the boost turns on, the DAP
// charges the phone, and the card is never offered to anything. From the user's
// side: "nothing happens". The stock player has exactly one function for this
// (set_usb_working_mode at 0x481f20 in hiby_player): it writes "sink" or "dual"
// to /sys/class/typec/port0/port_type and re-applies the saved choice at every
// boot, which is the "USB working mode: Device / Host" item in its menu.
//
// The host half is in use as well: USB-C headphones and USB DACs live on it
// (usbaudio.c). So the role is not pinned -- it is arbitrated, in
// arbitrate_port() over there, and the rule is that the port is dual while it
// is empty and becomes sink again a few seconds after something attaches that
// does not turn into a sound card. A phone is exactly that case; a port pinned
// dual for good could never connect to one.
//
// This function is the sink half of that rule, kept here because it also runs
// before the gadget is bound: it is the role that decides whether the
// controller has a gadget side at all.
// ---------------------------------------------------------------------------
static void typec_force_sink(void) {
	static bool announced;

	// Not while a DAC is being driven. This one line is the whole of the "the
	// sound stops a few seconds after the screen comes back on" bug: waking the
	// panel makes the loop below rebuild the storage gadget, rebuilding the
	// gadget writes "sink" here, and writing "sink" ends the host session the
	// headphones are living in. The port belongs to usbaudio.c while there is
	// something on it.
	if (usbaudio_active()) {
		return;
	}

	for (int port = 0; port < 4; port++) {
		char path[64];
		snprintf(path, sizeof(path), "/sys/class/typec/port%d/port_type", port);
		if (access(path, W_OK) != 0) {
			continue;
		}

		FILE *f = fopen(path, "w");
		if (!f) {
			continue;
		}
		bool ok = fputs("sink", f) >= 0;
		if (fclose(f) != 0) {
			ok = false;
		}
		if (!announced) {
			announced = true;
			fprintf(stderr, "usb: %s <- sink (%s)\n", path, ok ? "ok" : strerror(errno));
		}
		return;
	}

	if (!announced) {
		announced = true;
		fprintf(stderr, "usb: no writable /sys/class/typec/portN/port_type; leaving the role alone\n");
	}
}

// Builds the storage gadget -- descriptors with this device's real name, a
// mass-storage function with no medium in it -- and binds it to the USB
// controller. Idempotent: every step checks before it acts, so this both
// creates the gadget at startup and rebuilds it after ADB tore the configfs
// down.
//
// It is bound with an empty medium rather than only while exporting because a
// bound gadget is what lets the controller's state tell a PC from a wall
// charger: a PC enumerates the medium-less card reader and the UDC state reads
// CONFIGURED, a charger only supplies power and never does. The stock player
// makes the same distinction the same way, with its UAC gadget. Exporting is
// then just writing the block device into lun.0/file (the medium clicks in);
// un-exporting writes it empty again. The gadget itself never unbinds.
static void ensure_storage_gadget(void) {
	if (dac_owns_usb || usbaudio_active()) {
		return;
	}

	// The role first, every time: the gadget below is only worth binding to a
	// controller that is going to be the peripheral end of the cable.
	typec_force_sink();

	char cmd[2048];
	snprintf(cmd, sizeof(cmd),
			 "[ -d /sys/kernel/config/usb_gadget ] || mount -t configfs none /sys/kernel/config; "
			 "if [ ! -d %s ]; then "
			 "mkdir %s && cd %s && "
			 "echo 0x00 > bDeviceClass; "
			 "echo 0x0200 > bcdUSB; "
			 "echo 0x0100 > bcdDevice; "
			 "echo %s > idVendor; "
			 "echo %s > idProduct; "
			 "mkdir strings/0x409; "
			 "echo '%s' > strings/0x409/manufacturer; "
			 "echo '%s' > strings/0x409/product; "
			 "echo '%s %s' > strings/0x409/serialnumber; "
			 "mkdir configs/c.1; "
			 "echo 500 > configs/c.1/MaxPower; "
			 "echo 0x80 > configs/c.1/bmAttributes; "
			 "mkdir configs/c.1/strings/0x409; "
			 "echo storage > configs/c.1/strings/0x409/configuration; "
			 "fi; "
			 "cd %s && "
			 "if [ ! -d functions/mass_storage.0 ]; then "
			 "mkdir functions/mass_storage.0; "
			 "echo 0 > functions/mass_storage.0/lun.0/ro; "
			 "echo 1 > functions/mass_storage.0/lun.0/removable; "
			 "echo 1 > functions/mass_storage.0/lun.0/nofua; "
			 "echo 0 > functions/mass_storage.0/lun.0/cdrom; "
			 "echo '' > functions/mass_storage.0/lun.0/file; "
			 "echo '%s' > functions/mass_storage.0/lun.0/inquiry_string 2>/dev/null; "
			 "fi; "
			 "[ -e configs/c.1/mass_storage.0 ] || ln -s functions/mass_storage.0 configs/c.1; "
			 "grep -q '[a-zA-Z0-9]' UDC || echo $(ls /sys/class/udc/ | head -n1) > UDC",
			 STORAGE_GADGET, STORAGE_GADGET, STORAGE_GADGET, USB_ID_VENDOR, USB_ID_PRODUCT, USB_MANUFACTURER,
			 USB_PRODUCT, USB_MANUFACTURER, USB_PRODUCT, STORAGE_GADGET, USB_INQUIRY_STRING);
	int rc = system(cmd);
	(void)rc;
}

// Makes the host enumerate the gadget again, without rebuilding it: the same
// soft-disconnect toggle DAC mode uses (usbdac.c). Silently does nothing on a
// controller with no such node, where the medium simply appears the way it
// always has.
static void storage_reenumerate(void) {
	static const char *const PATHS[] = {
		"/sys/class/usb_gadget/android0/soft_disconnect",
		STORAGE_GADGET "/soft_disconnect",
		NULL,
	};

	for (int i = 0; PATHS[i]; i++) {
		if (access(PATHS[i], W_OK) != 0) {
			continue;
		}
		char cmd[256];
		snprintf(cmd, sizeof(cmd), "echo 1 > %s", PATHS[i]);
		int rc = system(cmd);
		(void)rc;
		usleep(300 * 1000);
		snprintf(cmd, sizeof(cmd), "echo 0 > %s", PATHS[i]);
		rc = system(cmd);
		(void)rc;
		fprintf(stderr, "usb: re-enumerated with the medium in, via %s\n", PATHS[i]);
		return;
	}
}

// True when a HOST has actually enumerated the bound gadget -- the check that
// tells a PC apart from a wall charger. VBUS alone cannot: a charger raises
// it too, and exporting on VBUS meant every plug-in-to-charge stopped the
// music and unmounted the card mid-listen.
// The name of the one USB device controller, or false when there is none.
static bool first_udc(char *out, size_t out_size) {
	DIR *dir = opendir("/sys/class/udc");
	if (!dir) {
		return false;
	}
	bool found = false;
	struct dirent *de;
	while (!found && (de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.') {
			continue;
		}
		snprintf(out, out_size, "%s", de->d_name);
		found = true;
	}
	closedir(dir);
	return found;
}

static bool udc_configured(void) {
	char *content = read_file_content("/sys/class/udc/13500000.otg_new/state");
	if (!content) {
		// Firmware revision with another controller name: take the first one.
		//
		// By hand, not with popen. This runs once a second for as long as the
		// screen is on and there is no cable, and popen forks the whole player
		// to run a shell -- fifty-odd megabytes of page tables on a device with
		// fifty-six, once a second, next to a thread that has to hand the card
		// a period of audio on time. A directory and a small read cost nothing.
		char name[NAME_MAX + 1];
		if (!first_udc(name, sizeof(name))) {
			return false;
		}
		char path[NAME_MAX + 32];
		snprintf(path, sizeof(path), "/sys/class/udc/%s/state", name);
		content = read_file_content(path);
		if (!content) {
			return false;
		}
		content[strcspn(content, "\r\n")] = '\0';
		bool configured = strcasecmp(content, "configured") == 0;
		free(content);
		return configured;
	}
	content[strcspn(content, "\r\n")] = '\0';
	bool configured = strcasecmp(content, "configured") == 0;
	free(content);
	return configured;
}

// Takes the storage gadget off the USB controller. Used both to hand the
// controller to ADB and to stop paying for the PHY while the device sits in
// standby with no cable.
// Binds the gadget to the controller, or lets go of it. Two writes to one file:
// the controller's name to take it, an empty line to give it back.
//
// Written straight, not through a shell, and for the same reason as first_udc()
// above: this pair runs on the standby and wake path, which is exactly when
// music is playing with the screen dark. A fork here is heard.
static bool gadget_write_udc(const char *text) {
	FILE *f = fopen(STORAGE_GADGET "/UDC", "w");
	if (!f) {
		return false;
	}
	bool ok = fprintf(f, "%s\n", text) >= 0;
	if (fclose(f) != 0) {
		ok = false; // the driver refusing the bind surfaces here, at close
	}
	return ok;
}

static void gadget_unbind(void) { gadget_write_udc(""); }

// Puts the gadget back on the controller. False when there is no gadget to put
// back -- it has never been built, or something removed it -- and the caller
// falls back to building one.
static bool gadget_rebind(void) {
	char udc[NAME_MAX + 1];
	if (access(STORAGE_GADGET "/UDC", W_OK) != 0 || !first_udc(udc, sizeof(udc))) {
		return false;
	}
	return gadget_write_udc(udc);
}

// Called right before ADB's init script starts: two gadgets cannot bind at
// once.
void usb_gadget_yield_to_adb(void) { gadget_unbind(); }

// Called after ADB shuts down. Its stop script also unmounts the whole
// configfs, so the gadget is rebuilt from nothing.
void usb_gadget_reclaim_from_adb(void) { ensure_storage_gadget(); }

// ---------------------------------------------------------------------------
// Around a system suspend
//
// The storage gadget is deliberately kept bound to the USB controller at all
// times (see above: a bound gadget is what lets the controller tell a PC from
// a wall charger). That is fine while the board is awake and fatal when it
// tries to sleep: suspending with a live gadget on the controller makes the
// device answer with a reset instead of a resume, and the player starts from
// scratch.
//
// So the controller is handed nothing at all before the SoC goes down, and the
// gadget is rebuilt on the way back. The stock player does not need this: it
// only builds a gadget while a host is actually attached.
// ---------------------------------------------------------------------------

// The cable, as the USB subsystem itself sees it -- not as the battery poll's
// five-second-old "charging" reading sees it. The suspend rule needs the
// truthful answer: a PC that enumerates the card must never have the gadget
// pulled out from under it.
bool usb_vbus_present(void) { return vbus_present(); }

void usb_suspend_prepare(void) {
	if (!sd_device[0]) {
		return; // USB mode was never started (host build, or no card)
	}
	// Every gadget, not just ours: with ADB on it is adbd's gadget that holds
	// the controller.
	int rc = system("for u in /sys/kernel/config/usb_gadget/*/UDC; do echo '' > $u 2>/dev/null; done");
	(void)rc;
	fprintf(stderr, "usb: gadgets unbound for suspend\n");
}

void usb_resume_restore(void) {
	if (!sd_device[0]) {
		return;
	}
	ensure_storage_gadget();
	fprintf(stderr, "usb: storage gadget rebound after resume\n");
}

// Adds the mass-storage function to the ADB gadget and rebinds it, giving the
// host a composite adb+storage device. Shell, because this is precisely the
// firmware's own scripting style and the sequence is standard configfs work:
// unbind, add function, link into the config, rebind. The lun settings match
// what the stock binary writes (ro=0 removable=1 nofua=1 cdrom=0). The adb
// connection drops for a moment at the rebind and comes back on its own.
static void composite_export(void) {
	char cmd[1024];
	snprintf(cmd, sizeof(cmd),
			 "cd %s && "
			 "echo '' > UDC; "
			 "echo '" USB_MANUFACTURER "' > strings/0x409/manufacturer 2>/dev/null; "
			 "echo '" USB_PRODUCT "' > strings/0x409/product 2>/dev/null; "
			 "mkdir -p functions/mass_storage.0 && "
			 "echo 0 > functions/mass_storage.0/lun.0/ro; "
			 "echo 1 > functions/mass_storage.0/lun.0/removable; "
			 "echo 1 > functions/mass_storage.0/lun.0/nofua; "
			 "echo 0 > functions/mass_storage.0/lun.0/cdrom; "
			 "echo %s > functions/mass_storage.0/lun.0/file; "
			 "[ -e configs/c.1/mass_storage.0 ] || ln -s functions/mass_storage.0 configs/c.1; "
			 "echo $(ls /sys/class/udc/ | head -n1) > UDC",
			 ADB_GADGET, sd_device);
	fprintf(stderr, "usb: adding storage to the adb gadget\n");
	int rc = system(cmd);
	fprintf(stderr, "usb: composite export exit code %d\n", rc);
}

static void composite_restore(void) {
	char cmd[1024];
	snprintf(cmd, sizeof(cmd),
			 "cd %s && "
			 "echo '' > UDC; "
			 "echo '' > functions/mass_storage.0/lun.0/file; "
			 "rm -f configs/c.1/mass_storage.0; "
			 "rmdir functions/mass_storage.0; "
			 "echo $(ls /sys/class/udc/ | head -n1) > UDC",
			 ADB_GADGET);
	int rc = system(cmd);
	fprintf(stderr, "usb: composite restore exit code %d\n", rc);
}

// True while something is mounted on sd_mount (paths resolved, because the
// firmware reaches the card through symlinks and /proc/mounts lists the
// canonical one).
static bool mount_still_up(void) {
	char wanted[512];
	if (!realpath(sd_mount, wanted)) {
		return false;
	}

	FILE *f = fopen("/proc/mounts", "r");
	if (!f) {
		return false;
	}

	char device[256], point[256];
	bool found = false;
	while (fscanf(f, "%255s %255s %*[^\n]", device, point) == 2) {
		if (strcmp(point, wanted) == 0) {
			found = true;
			break;
		}
	}

	fclose(f);
	return found;
}

// True when the host has ejected the medium: on a removable lun the "safely
// remove" on the PC closes the gadget's backing file, so lun.0/file reads
// empty. That is the signal to take the card back -- otherwise the player
// sits saying "no card" until the cable is physically pulled.
static bool host_ejected(void) {
	const char *lun_file = (export_mode == EXPORT_ADB_COMPOSITE)
							   ? ADB_GADGET "/functions/mass_storage.0/lun.0/file"
							   : STORAGE_GADGET "/functions/mass_storage.0/lun.0/file";

	char *content = read_file_content(lun_file);
	if (!content) {
		return false; // node unreadable: assume still exported, do nothing rash
	}
	content[strcspn(content, "\r\n")] = '\0';
	bool ejected = content[0] == '\0';
	free(content);
	return ejected;
}

// Hands the card to the host. Ordering is what the stock player does: stop
// touching the filesystem, unmount, then export the raw device.
static void storage_export(void) {
	fprintf(stderr, "usb: host attached, exporting %s\n", sd_device);

	// Said first, not last.
	//
	// The card supervisor stands down while this flag is up (see
	// storage_recheck_card): a card handed to a PC on purpose must not be taken
	// back from under it. Everything below -- stopping playback, closing the
	// databases, up to three unmount attempts a second apart, the
	// re-enumeration -- takes longer than the supervisor's four-second poll, and
	// all the supervisor needs to see is the moment after the unmount: no mount,
	// nobody claiming the card, so it mounts the card again. From then on the PC
	// and the player write to the same medium, and the player reads a stale
	// snapshot of the filesystem.
	storage_active = true;

	// Playback reads from the card; the library and the audiobook index hold
	// open databases on it; the log usually *is* on it -- and an open
	// descriptor makes the unmount fail. Release everything before trying.
	//
	// Where the track was, first of all: the stop below is what the next press
	// of play would otherwise read as "the track ended, start it again".
	device_state_note_storage_gone();
	audio_stop();
	library_close();
	audiobookdb_close();
	logging_suspend_for_usb();

	if (sysserver_available()) {
		sysserver_umount(sd_mount);
	} else {
		char cmd[512];
		snprintf(cmd, sizeof(cmd), "umount %s", sd_mount);
		int urc = system(cmd);
		(void)urc;
	}

	// Handing the raw device to a PC while the card is still mounted here would
	// corrupt the filesystem. If the first unmount did not take (a straggler
	// still holding a file), escalate: plain, forced, then lazy.
	//
	// The loop also gives up if DAC mode takes the controller meanwhile:
	// carrying on would insert the medium into what is by then the audio
	// gadget, and the card would appear on the PC in DAC mode.
	for (int attempt = 0; attempt < 3 && mount_still_up() && !dac_owns_usb; attempt++) {
		const char *flags = (attempt == 0) ? "" : (attempt == 1) ? "-f " : "-l ";
		char cmd[512];
		snprintf(cmd, sizeof(cmd), "umount %s%s", flags, sd_mount);
		fprintf(stderr, "usb: card still mounted, retrying: %s\n", cmd);
		int urc = system(cmd);
		(void)urc;
		sleep(1);
	}
	if (dac_owns_usb) {
		fprintf(stderr, "usb: DAC mode took the controller; export abandoned\n");
		storage_active = false; // the supervisor is wanted again
		return;
	}
	if (mount_still_up()) {
		fprintf(stderr, "usb: could not unmount %s; exporting anyway (read-mostly card)\n", sd_mount);
	}

	// With ADB holding the USB controller its gadget must carry the storage
	// too -- a second gadget can never bind. Otherwise the player's own,
	// already-bound gadget just gets the medium inserted: one write into
	// lun.0/file.
	if (adb_gadget_bound()) {
		composite_export();
		export_mode = EXPORT_ADB_COMPOSITE;
	} else {
		char cmd[512];
		snprintf(cmd, sizeof(cmd), "echo %s > " STORAGE_GADGET "/functions/mass_storage.0/lun.0/file", sd_device);
		int rc = system(cmd);
		fprintf(stderr, "usb: medium inserted (%s), exit code %d\n", sd_device, rc);
		export_mode = EXPORT_OWN_GADGET;

		// And then the host is asked to look again. It enumerated a card reader
		// with no card in it -- that emptiness is what tells a PC apart from a
		// wall charger -- and a PC will poll until the medium turns up, but a
		// phone reads the LUN once, at attach, and never comes back to it. A
		// short soft-disconnect is a re-plug as far as the host is concerned,
		// and this time there is a card in the slot.
		storage_reenumerate();
	}

	// What the controller thinks now, for the log: "CONFIGURED" means the PC
	// accepted the device.
	int lrc = system("for u in /sys/class/udc/*/state; do echo \"usb: udc $u: $(cat $u)\" >&2; done");
	(void)lrc;

	if (!suppress_popups) {
		gui_post(popup_connected_cb, NULL);
	}
}

// Takes the card back from the host and puts everything the export tore down
// back in place.
static void storage_restore(void) {
	fprintf(stderr, "usb: host detached, reclaiming %s\n", sd_device);

	if (export_mode == EXPORT_ADB_COMPOSITE) {
		composite_restore();
	} else {
		// Take the medium out; the gadget stays bound (the PC now sees a card
		// reader with no card, which is exactly the truth).
		int rc = system("echo '' > " STORAGE_GADGET "/functions/mass_storage.0/lun.0/file");
		fprintf(stderr, "usb: medium removed, exit code %d\n", rc);
	}
	export_mode = EXPORT_NONE;

	// Whatever is mounted now is a stale view of the card and has to go.
	//
	// The kernel cached that filesystem's metadata before handing the raw
	// device to the host; everything the PC wrote since is invisible through
	// it. Usually there is nothing left mounted, because the export unmounted
	// it -- but not always, since the export gives up after three attempts and
	// exports anyway. The loop below would then find the mount point "already
	// up" and leave it alone, and the player would carry on reading a directory
	// that no longer describes the card.
	//
	// So: drop it first, then mount. Unmounting a mount point that reads fine
	// costs a syscall; reading a card through a stale one costs the user their
	// firmware update.
	if (mount_still_up()) {
		fprintf(stderr, "usb: dropping the pre-export mount before reading the card again\n");
		if (sysserver_available()) {
			sysserver_umount(sd_mount);
		}
		if (mount_still_up()) {
			char cmd[2 * sizeof(sd_mount) + 64];
			snprintf(cmd, sizeof(cmd), "umount %s 2>/dev/null || umount -l %s", sd_mount, sd_mount);
			int urc = system(cmd);
			(void)urc;
		}
	}

	// Mount it back, and check rather than hope: a remount that fails silently
	// leaves the player with no card until a reboot. Try the daemon, then the
	// syscall, then busybox, until the mount point actually reads.
	for (int attempt = 0; attempt < 6 && !mount_still_up(); attempt++) {
		if (attempt) {
			sleep(1);
		}
		if (sysserver_available() && sysserver_mount(sd_device, sd_mount) == 0) {
			continue;
		}
		// ntfs-3g first, for the same reason as at startup (see mount_ntfs_rw in
		// system.c): the kernel's ntfs module mounts read-only and would win,
		// leaving the card browsable and nothing else after every USB session.
		char remount[1600];
		snprintf(remount, sizeof(remount),
				 "mount %s %s 2>/dev/null || ntfs-3g %s %s 2>/dev/null || mount -t vfat,exfat,ntfs3,ntfs %s %s",
				 sd_device, sd_mount, sd_device, sd_mount, sd_device, sd_mount);
		int mrc = system(remount);
		(void)mrc;
	}
	fprintf(stderr, "usb: card remounted on %s: %s\n", sd_mount, mount_still_up() ? "yes" : "NO");

	// The databases and the log live on the card that just came back.
	char db_path[512];
	snprintf(db_path, sizeof(db_path), "%s/.local/library.db", sd_mount);
	library_open(db_path);
	snprintf(db_path, sizeof(db_path), "%s/.local/audiobooks.db", sd_mount);
	audiobookdb_open(db_path);
	logging_resume_after_usb();

	storage_active = false;
	if (!suppress_popups) {
		gui_post(popup_disconnected_cb, NULL);
	}
}

static void *usb_thread(void *arg) {
	(void)arg;
	thread_be_background("usb watcher");

	bool exported = false;
	// Set after the PC ejects the medium: the card is back but the cable is
	// still in, and re-exporting now would undo the eject the user just asked
	// for. Cleared when VBUS finally drops.
	bool await_unplug = false;

	// Said once, not once a second.
	bool said_adb = false;

	// The gadget is unbound from the controller while the device sits in
	// standby with no cable (see below).
	bool parked = false;

	for (;;) {
		// DAC mode owns the controller: stand completely down. Not just "do
		// not rebuild" -- do not look, do not export, do not park. Everything
		// this loop does ends in a write to the gadget, and every one of those
		// writes would land on the audio gadget instead.
		if (dac_owns_usb) {
			sleep(1);
			continue;
		}

		// And the same for a DAC or a pair of headphones on the port: the
		// controller is the host's then, and everything below ends in a write
		// to a gadget that has no business existing while it is.
		if (usbaudio_active()) {
			sleep(1);
			continue;
		}

		// Slower while the panel is dark. A cable being plugged in is
		// something the user just did, so a second's resolution is worth
		// having with the screen on; in standby it is one more thread waking
		// the CPU every second for the whole night.
		sleep(power_screen_is_on() ? POLL_SECONDS : POLL_SECONDS_STANDBY);

		// A host that has enumerated this device is a cable, whatever the
		// charger says. On a phone the two can disagree: the power role and the
		// data role are separate on Type-C, so a phone can act as the host
		// while the charger node reads nothing at all -- and treating that as
		// "no cable" is how the gadget ended up being unbound from under a host
		// that was talking to it.
		bool vbus = vbus_present() || udc_configured();

		if (!vbus) {
			if (exported) {
				storage_restore();
				exported = false;
			}
			await_unplug = false;

			// No cable, panel dark: take the gadget off the controller. A
			// bound gadget keeps the USB PHY and the OTG block clocked --
			// /sys/kernel/debug/clk/clk_summary shows gate_usbphy and
			// gate_otg alive a minute into standby with nothing plugged in --
			// and that is current spent waiting for a cable that is not there.
			// The stock player only ever builds a gadget once a host has
			// turned up; this is the same idea, but only in standby, so
			// nothing about plugging a cable into a device in use changes.
			//
			// Never while ADB is running: the controller is ADB's then, and
			// reclaiming it would cut the connection.
			//
			// And not while audio is open either. Both of these are
			// housekeeping for a cable that is not attached: nothing is
			// waiting for the gadget, so there is no reason to touch the
			// controller in the middle of a track -- a rebind runs the whole
			// gadget script, shell and all, and costs a hole in the sound.
			// Both happen at the next poll after playback stops, and a cable
			// turning up is handled below whatever this decided.
			if (audio_device_is_open()) {
				continue;
			}

			if (!parked && !power_screen_is_on() && !adb_is_running()) {
				gadget_unbind();
				parked = true;
				fprintf(stderr, "usb: no cable and the screen is dark; gadget unbound\n");
			} else if (parked && power_screen_is_on()) {
				if (!gadget_rebind()) {
					ensure_storage_gadget();
				}
				parked = false;
				fprintf(stderr, "usb: awake again; gadget rebound\n");
			}
			continue;
		}

		// A cable turned up while the gadget was parked: put it back before
		// anything else, or the host will never see this device.
		if (parked) {
			// Here there is a host waiting, so this happens whatever playback
			// is doing -- but by the cheap route first: the gadget was only
			// unbound, not taken apart, so putting it back is one write.
			typec_force_sink();
			if (!gadget_rebind()) {
				ensure_storage_gadget();
			}
			parked = false;
			fprintf(stderr, "usb: cable in; gadget rebound\n");
			continue; // give the controller a poll to enumerate
		}

		if (exported) {
			if (host_ejected()) {
				fprintf(stderr, "usb: host ejected the medium, reclaiming the card\n");
				storage_restore();
				exported = false;
				await_unplug = true;
			}
			continue;
		}

		if (await_unplug) {
			continue;
		}

		// ADB is on: the cable is here to carry a shell, not to be a card
		// reader. Exporting the card now unmounts it from under that shell --
		// which is why /mnt/sd_0 came back "Read-only file system" over ADB --
		// and the stock firmware does not do it either. Leave the card where
		// it is.
		if (adb_is_running()) {
			if (!said_adb) {
				fprintf(stderr, "usb: ADB is running; the card stays mounted (no mass storage)\n");
				said_adb = true;
			}
			continue;
		}
		said_adb = false;

		// VBUS alone is not a PC -- a wall charger raises it too, and taking
		// the card away (stopping the music with it) for a power brick was
		// exactly the "track would not start while charging" bug. Only a host
		// that has enumerated the gadget counts.
		if (!udc_configured()) {
			continue;
		}

		storage_export();
		exported = true;
	}

	return NULL;
}

void usb_start(const char *device, const char *mount_point) {
	if (!device || !device[0] || !mount_point || !mount_point[0]) {
		fprintf(stderr, "usb: no card device/mount known; USB mode disabled\n");
		return; // host build, or no card: nothing to export
	}

	bool any_vbus = false;
	for (size_t i = 0; i < sizeof(VBUS_CANDIDATES) / sizeof(VBUS_CANDIDATES[0]); i++) {
		bool have = access(VBUS_CANDIDATES[i], R_OK) == 0;
		fprintf(stderr, "usb: vbus node %s: %s\n", VBUS_CANDIDATES[i], have ? "present" : "missing");
		any_vbus |= have;
	}
	if (!any_vbus) {
		fprintf(stderr, "usb: no VBUS node found; USB mode disabled\n");
		return;
	}
	if (access("/sys/kernel/config", F_OK) != 0) {
		fprintf(stderr, "usb: no configfs on this kernel; USB mode disabled\n");
		return;
	}

	// The port is a peripheral from here on, whatever is at the other end of
	// the cable. The stock player re-applies its own saved choice at every boot
	// for the same reason: the role is not remembered across one.
	typec_force_sink();

	snprintf(sd_device, sizeof(sd_device), "%s", device);
	snprintf(sd_mount, sizeof(sd_mount), "%s", mount_point);

	// The empty-medium gadget goes up right away (unless ADB already owns the
	// controller -- then its gadget provides the enumeration signal instead).
	if (!adb_gadget_bound()) {
		ensure_storage_gadget();
	}

	pthread_t thread;
	if (pthread_create(&thread, NULL, usb_thread, NULL) == 0) {
		pthread_detach(thread);
		fprintf(stderr, "usb: watching for a USB host (exporting %s)\n", sd_device);
	}
}
