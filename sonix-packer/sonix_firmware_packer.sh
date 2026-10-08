#!/bin/bash
#
# Builds HiByOS firmware images that boot Sonix Player instead of the stock
# music player -- one per model, from one binary and one set of assets.
#
# Everything it needs sits next to it:
#
#   sonix_firmware_packer.sh   this script
#   r3proii_original.upt       the stock firmware of the R3 Pro II
#   r1_original.upt            the stock firmware of the R1
#   sonix_player               the binary to install, the same one for both
#   sonix_launch               waits for the player and reboots when it exits
#                              (optional: without it the launcher's shell does)
#   kernel/xImage-R3PII        a kernel to install instead of the stock one
#   kernel/xImage-R1           (optional, per model: without it the stock
#                              kernel stays, untouched)
#   assets/
#       R3PII/
#       R1/
#       |
#       v
#   r3proii.upt                one result per stock firmware present
#   r1.upt
#
# A model whose stock firmware is not here is skipped with a warning, so
# somebody who owns one of the two players can still run this and get theirs.
#
# One complete tree per model, and no shared layer: too much of what goes into
# an image belongs to the machine it is going into -- the touch driver is built
# against that kernel, the boot logos are drawn for that panel, and the audio
# pieces answer to that hardware. The files that really are the same in both,
# the language files and some of the images, are cheaper to copy than a rule
# about which layer wins.
#
# What that costs is the two trees drifting apart, so before each image this
# script lists the files the OTHER model has and this one does not. Usually
# that is deliberate; the once it is not, it says so.
#
# No questions asked: run it and it does the lot.

set -euo pipefail
export COLUMNS=1

# --- terminal colours ---
RED='\033[1;31m'
GREEN='\033[1;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Homebrew, for macOS.
export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

say()  { echo -e "$@"; }
step() { echo -e "${BLUE}==>${NC} $*"; }
warn() { echo -e "${YELLOW}!!${NC}  $*"; }
die()  { echo -e "${RED}Error:${NC} $*" >&2; exit 1; }

# ==========================================================================
# Where everything lives
# ==========================================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"

PLAYER_BIN="$SCRIPT_DIR/sonix_player"
LAUNCH_BIN="$SCRIPT_DIR/sonix_launch"

# Kernels to install, one per model folder name: kernel/xImage-R3PII and so on.
# Not under assets/, which is copied into the rootfs.
KERNEL_DIR="$SCRIPT_DIR/kernel"

# The models, one line each: the assets folder, the stock firmware to start
# from, the image to write, and the device-name the player will read out of
# system-info.json. The last one is not decoration -- it is checked against the
# file the overlay actually landed, which is what stops an R1 image being built
# with the R3 Pro II's name in it and offering that model's updates.
MODELS=(
	"R3PII|r3proii_original.upt|r3proii.upt|HiBy R3 Pro II"
	"R1|r1_original.upt|r1.upt|HiBy R1"
)

WORK_DIR="$SCRIPT_DIR/temp"
OTA_DIR="$WORK_DIR/ota_v0"
SQUASH_DIR="$SCRIPT_DIR/squashfs-root"
MERGED_SQUASHFS="$SCRIPT_DIR/rootfs_original.squashfs"

# Paths inside the rootfs.
STOCK_PLAYER="usr/bin/hiby_player"
STOCK_LAUNCHER="usr/bin/hiby_player.sh"
NEW_LAUNCHER="usr/bin/sonix_player.sh"
INIT_SCRIPT="etc/init.d/S92_03_start_music_player"
SYSTEM_INFO="usr/resource/sonix/components/system-info.json"

# The stock interface's own resources. Nothing on the device reads them once
# the player is replaced, and they are several megabytes of an image that has
# to fit in flash.
#
# Note what is NOT in this list: usr/resource/sonix, which is where the assets
# folder lands. The stock fonts and strings live one level above it and go;
# ours stay.
STOCK_UI_DIRS=(usr/resource/litegui usr/resource/layout usr/resource/str usr/resource/fonts)

# The working folders always go, whether the run succeeded or not.
cleanup() { rm -rf "$WORK_DIR" "$SQUASH_DIR" "$MERGED_SQUASHFS" 2>/dev/null || true; }
trap cleanup EXIT

say "${BLUE}###############################################${NC}"
say "${BLUE}###   SONIX PLAYER -- FIRMWARE PACKER       ###${NC}"
say "${BLUE}###############################################${NC}"
say ""

# ==========================================================================
# Portability: md5, file size, build stamp
# ==========================================================================
if command -v md5sum >/dev/null 2>&1; then
	get_md5() { md5sum "$1" | awk '{print $1}'; }
else
	get_md5() { md5 -q "$1"; }
fi

if stat -c%s . >/dev/null 2>&1; then
	get_size()  { stat -c%s "$1"; }
	# When the file was last written, as DDMMYYYYHHMM. GNU date takes the file
	# itself here; the BSD one takes seconds, which is why this is not one line
	# for both.
	get_stamp() { date -r "$1" +%d%m%Y%H%M; }
else
	get_size()  { stat -f%z "$1"; }
	get_stamp() { stat -f%Sm -t %d%m%Y%H%M "$1"; }
fi

# sed -i is spelled differently on the two systems, so nothing here uses it:
# the file is rewritten through a temporary and moved into place.
sed_file() {
	local expression="$1" file="$2"
	sed -E "$expression" "$file" > "$file.packer.tmp"
	mv "$file.packer.tmp" "$file"
}

# How many times a string appears in a file -- every occurrence, not the lines
# that hold one. A single line can kill both hiby_player.sh and hiby_player,
# and counting lines would report that as one.
count_in() { tr -c '[:alnum:]_./' '\n' < "$2" | grep -c -- "$1" || true; }

# Replaces every hiby_player with sonix_player, and proves it: the count before,
# every resulting line printed, and a hard stop if anything survived.
#
# One substitution covers both spellings -- a reference to `hiby_player` becomes
# `sonix_player`, and one to `hiby_player.sh` becomes `sonix_player.sh`, which
# is the name the launcher has just been given.
rename_player_in() {
	local file="$1" label="$2"
	local before
	before="$(count_in "hiby_player" "$file")"

	sed_file "s/hiby_player/sonix_player/g" "$file"

	if grep -q "hiby_player" "$file"; then
		die "$label still mentions hiby_player after the rewrite."
	fi
	say "    $label: $before occurrence(s) of hiby_player replaced"
	grep -n "sonix_player" "$file" | sed 's/^/        /'
}

# Copies FS_data, the HBC3000's FPGA configuration, out of the stock
# sa_sound_hbc3000.ko into a file: the array named by the module's symbol table,
# in its own section. perl because macOS and Linux both have it.
hbc3000_firmware() {
	perl -e '
		local $/;
		open(my $f, "<:raw", $ARGV[0]) or die "$ARGV[0]: $!\n";
		my $e = <$f>;
		substr($e, 0, 6) eq "\x7fELF\x01\x01" or die "$ARGV[0]: not a 32-bit little-endian ELF\n";
		my $shoff = unpack("V", substr($e, 0x20, 4));
		my ($shentsize, $shnum) = unpack("vv", substr($e, 0x2e, 4));
		my @sh = map { [unpack("V10", substr($e, $shoff + $_ * $shentsize, 40))] } 0 .. $shnum - 1;
		for my $s (grep { $_->[1] == 2 } @sh) {
			my $strtab = $sh[$s->[6]]->[4];
			for (my $o = 0; $o < $s->[5]; $o += 16) {
				my ($name, $value, $size, $info, $other, $shndx) =
					unpack("VVVCCv", substr($e, $s->[4] + $o, 16));
				next unless unpack("Z*", substr($e, $strtab + $name)) eq "FS_data";
				open(my $out, ">:raw", $ARGV[1]) or die "$ARGV[1]: $!\n";
				print $out substr($e, $sh[$shndx]->[4] + $value, $size);
				exit 0;
			}
		}
		die "$ARGV[0]: no FS_data\n";
	' "$1" "$2"
}

# Writes an image into the OTA folder the way the updater reads it: 512 KiB
# chunks named NAME.NNNN.<md5 of the previous chunk>, the first one named after
# the md5 of the whole image, and ota_md5_NAME.<md5 of the whole image> listing
# the md5 of every chunk in order. The image itself is consumed.
write_chunks() {
	local name="$1" image="$2"
	local whole_sum current_sum md5_file count suffix chunk f

	whole_sum="$(get_md5 "$image")"
	split -b 524288 -a 4 "$image" "$OTA_DIR/temp_chunk_"
	rm -f "$image"

	md5_file="$OTA_DIR/ota_md5_$name.$whole_sum"
	: > "$md5_file"

	count=0
	current_sum="$whole_sum"
	for f in "$OTA_DIR/temp_chunk_"*; do
		[ -e "$f" ] || continue
		suffix="$(printf "%04d" $count)"
		chunk="$OTA_DIR/$name.$suffix.$current_sum"
		mv "$f" "$chunk"
		current_sum="$(get_md5 "$chunk")"
		echo "$current_sum" >> "$md5_file"
		count=$((count + 1))
	done
	say "    $name: $count chunks"
}

# ==========================================================================
# What has to be there before anything is touched
# ==========================================================================
step "Checking what is needed"

# A string and not an array: bash 3.2, which is the one macOS ships, treats
# ${#array[@]} on an empty array as an unbound variable under `set -u`.
MISSING=""
for tool in 7z unsquashfs mksquashfs split tar; do
	command -v "$tool" >/dev/null 2>&1 || MISSING="$MISSING $tool"
done
# The ISO writer answers to more than one name depending on the system.
ISO_TOOL=""
for tool in mkisofs genisoimage xorrisofs; do
	if command -v "$tool" >/dev/null 2>&1; then
		ISO_TOOL="$tool"
		break
	fi
done
[ -n "$ISO_TOOL" ] || MISSING="$MISSING mkisofs"

if [ -n "$MISSING" ]; then
	die "missing:$MISSING
  Debian/Ubuntu:  sudo apt install p7zip-full squashfs-tools genisoimage
  macOS:          brew install p7zip squashfs cdrtools"
fi

[ -f "$PLAYER_BIN" ] || die "sonix_player is not next to this script."

ASSETS_DIR="$SCRIPT_DIR/assets"
[ -d "$ASSETS_DIR" ] || die "there is no assets folder next to this script."

# Which models can be built at all. A stock firmware that is not here is not an
# error -- it is a model this person does not own.
BUILDABLE=""
for entry in "${MODELS[@]}"; do
	IFS='|' read -r m_dir m_src m_out m_name <<< "$entry"
	if [ ! -d "$ASSETS_DIR/$m_dir" ]; then
		warn "assets/$m_dir is missing: $m_name will be skipped."
		continue
	fi
	if [ ! -f "$SCRIPT_DIR/$m_src" ]; then
		warn "$m_src is not next to this script: $m_name will be skipped."
		continue
	fi
	BUILDABLE="$BUILDABLE$entry
"
	say "    $m_name: $m_src -> $m_out"
done

[ -n "$BUILDABLE" ] || die "no stock firmware to start from. Put at least one of
  r3proii_original.upt / r1_original.upt next to this script."

BUILD_STAMP="$(get_stamp "$PLAYER_BIN")"
TODAY="$(date +%d%m%Y)"
say "    binary:  sonix_player, $(get_size "$PLAYER_BIN") bytes, built $BUILD_STAMP"
if [ "${BUILD_STAMP:0:8}" != "$TODAY" ]; then
	warn "sonix_player was not built today; build_version will still say when it was."
fi

# sonix_launch has to be a 32-bit little-endian MIPS executable: the launcher
# execs it, and a busybox shell that cannot exec it exits without starting the
# player. The first 20 bytes of the ELF header say so.
if [ -f "$LAUNCH_BIN" ]; then
	ELF_HEAD="$(od -An -tx1 -N20 "$LAUNCH_BIN" | tr -d ' \n')"
	[ "${ELF_HEAD:0:12}" = "7f454c460101" ] && [ "${ELF_HEAD:32:8}" = "02000800" ] ||
		die "sonix_launch is not a 32-bit little-endian MIPS executable.
	  Build it with \`make target\` (or \`make sonix_launch\`) in sonix-player."
	say "    launch:  sonix_launch, $(get_size "$LAUNCH_BIN") bytes"
else
	warn "sonix_launch is not next to this script: the launcher keeps its shell for the whole session."
fi
say ""

# ==========================================================================
# One image, start to finish
# ==========================================================================
# Called once per model. Everything it touches -- the work folder, the unpacked
# rootfs -- is torn down by cleanup() before the next model starts, so the two
# builds cannot see each other's files.
build_one() {
	local MODEL_DIR="$1" MODEL_NAME="$2"
	local SOURCE_UPT="$SCRIPT_DIR/$3" OUTPUT_UPT="$SCRIPT_DIR/$4"

	say "${YELLOW}###############################################${NC}"
	say "${YELLOW}###   $MODEL_NAME${NC}"
	say "${YELLOW}###############################################${NC}"
	say ""

	# ==========================================================================
	# 1. Unpack the stock firmware
	# ==========================================================================
	step "[$MODEL_NAME] unpacking $(basename "$SOURCE_UPT")"

	rm -rf "$WORK_DIR" "$SQUASH_DIR" "$MERGED_SQUASHFS"
	mkdir -p "$WORK_DIR"

	7z x "$SOURCE_UPT" -o"$WORK_DIR" -y > /dev/null

	[ -d "$OTA_DIR" ] || die "there is no ota_v0 folder inside the .upt: not a firmware of this kind."

	say "    Joining the rootfs chunks..."
	( cd "$OTA_DIR" && cat rootfs.squashfs.* ) > "$MERGED_SQUASHFS"

	say "    Extracting the filesystem..."
	unsquashfs -f -d "$SQUASH_DIR" "$MERGED_SQUASHFS" > /dev/null
	rm -f "$MERGED_SQUASHFS"

	# The old chunks go now: what gets written back must not sit next to what it
	# replaces, or the image ends up carrying both.
	rm -f "$OTA_DIR/ota_md5_rootfs.squashfs."* "$OTA_DIR/rootfs."*

	[ -f "$SQUASH_DIR/$STOCK_LAUNCHER" ] || die "the rootfs has no $STOCK_LAUNCHER: unexpected firmware."
	[ -f "$SQUASH_DIR/$INIT_SCRIPT" ]    || die "the rootfs has no $INIT_SCRIPT: unexpected firmware."
	say ""

	# ==========================================================================
	# 2. The binary
	# ==========================================================================
	step "[$MODEL_NAME] installing sonix_player"

	chmod 777 "$PLAYER_BIN"

	if [ -e "$SQUASH_DIR/$STOCK_PLAYER" ]; then
		rm -f "$SQUASH_DIR/$STOCK_PLAYER"
		say "    $STOCK_PLAYER removed"
	else
		warn "$STOCK_PLAYER was not there to begin with"
	fi

	cp -f "$PLAYER_BIN" "$SQUASH_DIR/usr/bin/sonix_player"
	chmod 777 "$SQUASH_DIR/usr/bin/sonix_player"
	say "    usr/bin/sonix_player installed (777)"
	say ""

	# ==========================================================================
	# 3. The launcher script
	# ==========================================================================
	step "[$MODEL_NAME] renaming the launcher"

	mv "$SQUASH_DIR/$STOCK_LAUNCHER" "$SQUASH_DIR/$NEW_LAUNCHER"
	say "    $STOCK_LAUNCHER -> $NEW_LAUNCHER"
	rename_player_in "$SQUASH_DIR/$NEW_LAUNCHER" "$NEW_LAUNCHER"
	# The mode is set after the overlay, not here: the overlay may carry its own
	# copy of this file and would bring its own mode with it. See step 6.
	say ""

	# ==========================================================================
	# 4. The init script
	# ==========================================================================
	step "[$MODEL_NAME] updating $INIT_SCRIPT"

	rename_player_in "$SQUASH_DIR/$INIT_SCRIPT" "$INIT_SCRIPT"
	say ""

	# ==========================================================================
	# 4b. The HBC3000's configuration
	# ==========================================================================
	#
	# The open sa_sound_hbc3000.ko loads the FPGA's configuration from
	# lib/firmware/hbc3000.fw. The configuration is HiBy's and is not in the
	# assets: it is taken here from the stock module, before the overlay puts
	# the open one in its place.
	if [ -f "$ASSETS_DIR/$MODEL_DIR/module_driver/sa_sound_hbc3000.ko" ]; then
		step "[$MODEL_NAME] taking the HBC3000 configuration from the stock module"

		command -v perl >/dev/null 2>&1 || die "perl is needed to read the stock sa_sound_hbc3000.ko."
		STOCK_HBC="$SQUASH_DIR/module_driver/sa_sound_hbc3000.ko"
		[ -f "$STOCK_HBC" ] || die "the stock firmware has no module_driver/sa_sound_hbc3000.ko.
	  The open one would have no configuration to load: the outputs would stay silent."
		mkdir -p "$SQUASH_DIR/lib/firmware"
		hbc3000_firmware "$STOCK_HBC" "$SQUASH_DIR/lib/firmware/hbc3000.fw" ||
			die "no configuration in the stock sa_sound_hbc3000.ko."
		say "    lib/firmware/hbc3000.fw: $(wc -c < "$SQUASH_DIR/lib/firmware/hbc3000.fw" | tr -d ' ') bytes"
		say ""
	fi

	# ==========================================================================
	# 5. The overlay
	# ==========================================================================
	step "[$MODEL_NAME] copying the overlay onto the root of the rootfs"

	# tar rather than cp: it merges into directories that already exist,
	# overwrites the files that clash, carries the hidden ones, and behaves the
	# same way on macOS and on Linux -- none of which is true of `cp -a` on both.
	( cd "$ASSETS_DIR/$MODEL_DIR" && tar cf - . ) | ( cd "$SQUASH_DIR" && tar xf - )

	MODEL_N="$(cd "$ASSETS_DIR/$MODEL_DIR" && find . -type f | wc -l | tr -d ' ')"
	say "    $MODEL_DIR/: $MODEL_N files"

	# The streaming keys go in sealed, as streaming-keys.bin. One left in the
	# clear in the assets is not shipped: an image is unpacked by anyone who
	# downloads it. See tools/seal_streamkeys.py in sonix-player.
	PLAIN_KEYS="usr/resource/sonix/components/streaming-keys.ini"
	if [ -f "$SQUASH_DIR/$PLAIN_KEYS" ]; then
		rm -f "$SQUASH_DIR/$PLAIN_KEYS"
		warn "$MODEL_DIR/$PLAIN_KEYS is in the clear and was left out; seal it with tools/seal_streamkeys.py"
	fi
	if [ ! -f "$SQUASH_DIR/usr/resource/sonix/components/streaming-keys.bin" ]; then
		warn "no streaming-keys.bin in $MODEL_DIR: Tidal, Qobuz and podcasts will be off"
	fi

	# The stock Wi-Fi/Bluetooth driver goes once bcm_wlbt_power and brcmfmac
	# have taken its place: only when the module list no longer loads it.
	DRIVER_LIST="$SQUASH_DIR/module_driver/driver_default_init_script.sh"
	if [ -f "$ASSETS_DIR/$MODEL_DIR/module_driver/bcm_wlbt_power.ko" ] &&
		grep -q '^sh bcm_wlbt_power\.sh' "$DRIVER_LIST" 2>/dev/null &&
		! grep -q 'cywdhd' "$DRIVER_LIST"; then
		for rel in module_driver/cywdhd.ko module_driver/cywdhd.sh; do
			if [ -f "$SQUASH_DIR/$rel" ]; then
				rm -f "$SQUASH_DIR/$rel"
				say "    $rel removed (bcm_wlbt_power and brcmfmac replace it)"
			fi
		done
	fi

	# What the other model's tree carries and this one does not. Nothing is
	# copied across and nothing fails: whatever the stock firmware had at that
	# path is left alone, which for a boot logo is this model's own at its own
	# size. It is listed because the other reason for the difference is having
	# added a file to one tree and forgotten the other.
	for other in "${MODELS[@]}"; do
		IFS='|' read -r o_dir o_src o_out o_name <<< "$other"
		[ "$o_dir" != "$MODEL_DIR" ] || continue
		[ -d "$ASSETS_DIR/$o_dir" ] || continue
		while IFS= read -r rel; do
			[ -n "$rel" ] || continue
			say "    only in $o_dir/: ${rel#./} -- the stock firmware's own is kept here"
		done < <(comm -23 \
			<(cd "$ASSETS_DIR/$o_dir" && find . -type f | sort) \
			<(cd "$ASSETS_DIR/$MODEL_DIR" && find . -type f | sort))
	done
	say ""

	# ==========================================================================
	# 5b. The modes that decide whether any of this runs
	# ==========================================================================
	#
	# After the overlay, because the overlay wins: it may carry its own copy of
	# the launcher, and tar brings the mode along with the file.
	#
	# This is not housekeeping. The init script is
	#
	#     PL01=/usr/bin/sonix_player.sh
	#     [ -f $PL01 ] && ( $PL01 & ) || echo "file <$PL01> don't exist."
	#
	# and `-f` asks whether the file EXISTS, not whether it can be run. A
	# launcher that arrives without its execute bit passes that test, fails to
	# start, takes the `&&` branch so the `||` message is never printed, and
	# leaves a device sitting on its boot logo with nothing in any log and no
	# reboot to hint at it. An assets folder that has been through a zip is
	# enough to cause it, which is why this is checked rather than assumed.
	step "[$MODEL_NAME] making the boot path executable"

	for rel in "$NEW_LAUNCHER" "$INIT_SCRIPT" usr/bin/bluealsa usr/bin/shairport_on.sh etc/init.d/S80_bt_init; do
		if [ -f "$SQUASH_DIR/$rel" ]; then
			chmod 755 "$SQUASH_DIR/$rel"
			say "    $rel is 755"
		fi
	done

	# Anything else the overlay put in a directory of executables, the
	# kernel-module scripts and the init scripts with it.
	for rel in $(cd "$ASSETS_DIR/$MODEL_DIR" && find usr/bin module_driver etc/init.d -type f 2>/dev/null); do
		[ -f "$SQUASH_DIR/$rel" ] || continue
		chmod 755 "$SQUASH_DIR/$rel"
	done

	# The two that decide whether the player is ever started at all.
	for rel in "$NEW_LAUNCHER" "$INIT_SCRIPT"; do
		[ -x "$SQUASH_DIR/$rel" ] || die "$rel is not executable in the image.
	  Nothing would start the player and the device would sit on its boot logo."
	done
	say ""

	# ==========================================================================
	# 5c. sonix_launch
	# ==========================================================================
	#
	# The launcher execs sonix_launch just before the line that starts the
	# player, so the shell is replaced by a process of a few kB that does what
	# the rest of the script does: runs the player, then `sleep 1; reboot`. The
	# original lines stay under it and still run on a rootfs without the binary.
	if [ -f "$LAUNCH_BIN" ]; then
		step "[$MODEL_NAME] starting the player through sonix_launch"

		cp -f "$LAUNCH_BIN" "$SQUASH_DIR/usr/bin/sonix_launch"
		chmod 755 "$SQUASH_DIR/usr/bin/sonix_launch"
		say "    usr/bin/sonix_launch installed (755)"

		LAUNCHER_FILE="$SQUASH_DIR/$NEW_LAUNCHER"
		if grep -q "sonix_launch" "$LAUNCHER_FILE"; then
			say "    $NEW_LAUNCHER already calls it"
		else
			PLAYER_LINES="$(grep -c '^/usr/bin/sonix_player$' "$LAUNCHER_FILE" || true)"
			if [ "$PLAYER_LINES" = "1" ]; then
				awk '$0 == "/usr/bin/sonix_player" { print "[ -x /usr/bin/sonix_launch ] && exec /usr/bin/sonix_launch /usr/bin/sonix_player" } { print }' \
					"$LAUNCHER_FILE" > "$LAUNCHER_FILE.new"
				mv -f "$LAUNCHER_FILE.new" "$LAUNCHER_FILE"
				chmod 755 "$LAUNCHER_FILE"
				grep -q "exec /usr/bin/sonix_launch" "$LAUNCHER_FILE" || die "$NEW_LAUNCHER was not rewritten."
				say "    $NEW_LAUNCHER execs sonix_launch"
			else
				rm -f "$SQUASH_DIR/usr/bin/sonix_launch"
				warn "$NEW_LAUNCHER has $PLAYER_LINES lines that are exactly /usr/bin/sonix_player, not one:
	  left as it is, and sonix_launch not installed."
			fi
		fi
		say ""
	fi

	# ==========================================================================
	# 6. The stock interface's resources
	# ==========================================================================
	step "[$MODEL_NAME] removing the stock interface's folders"

	for dir in "${STOCK_UI_DIRS[@]}"; do
		if [ -e "$SQUASH_DIR/$dir" ]; then
			rm -rf "$SQUASH_DIR/$dir"
			[ -e "$SQUASH_DIR/$dir" ] && die "could not remove $dir."
			say "    $dir removed"
		else
			say "    $dir was not there"
		fi
	done
	say ""

	# ==========================================================================
	# 7. The build stamp
	# ==========================================================================
	step "[$MODEL_NAME] system-info.json"

	JSON="$SQUASH_DIR/$SYSTEM_INFO"
	[ -f "$JSON" ] || die "$SYSTEM_INFO is not in the rootfs.
	  It should come from assets/$MODEL_DIR/$SYSTEM_INFO"

	# The name in the file against the model being built. Everything the player
	# decides by model -- the panel it opens the simulator at, the .upt it will
	# accept, the converter and the serial it prints -- hangs off this one string,
	# and an image carrying the other model's name would offer that model's update
	# to a device that cannot survive it.
	FOUND_NAME="$(tr -d '\r' < "$JSON" | tr ',' '\n' | grep -i '"device-name"' | head -1 | sed -E 's/.*"device-name"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/')"
	[ -n "$FOUND_NAME" ] || die "$SYSTEM_INFO has no device-name."
	[ "$FOUND_NAME" = "$MODEL_NAME" ] || die "$SYSTEM_INFO says device-name \"$FOUND_NAME\",
	  but this is the $MODEL_NAME image. Check assets/$MODEL_DIR/$SYSTEM_INFO."
	say "    device-name = $FOUND_NAME"

	# Whatever case the key was written in. The player reads it case-insensitively
	# (find_nocase in src/system/device/sysinfo.c), so "Build_version" and
	# "build_version" are equally correct in the file and this has to accept the
	# same. The spelling is read out first and then used literally in the
	# substitution, because sed's case-insensitive flag is a GNU extension and this
	# has to run on macOS too.
	KEY="$(grep -oE '"[Bb][Uu][Ii][Ll][Dd]_[Vv][Ee][Rr][Ss][Ii][Oo][Nn]"' "$JSON" | head -1 || true)"
	[ -n "$KEY" ] || die "$SYSTEM_INFO has no build_version key, in any case."

	sed_file "s/($KEY[[:space:]]*:[[:space:]]*\")[^\"]*(\")/\\1$BUILD_STAMP\\2/" "$JSON"

	grep -q "\"$BUILD_STAMP\"" "$JSON" || die "$KEY was not rewritten."
	say "    $KEY = $BUILD_STAMP"
	say ""

	# ==========================================================================
	# 8. Repack
	# ==========================================================================
	say "${YELLOW}###############################${NC}"
	say "${YELLOW}###   NEW FILESYSTEM        ###${NC}"
	say "${YELLOW}###############################${NC}"
	say ""

	step "[$MODEL_NAME] clearing macOS clutter"
	find "$SQUASH_DIR" -name '.DS_Store' -type f -delete 2>/dev/null || true
	find "$SQUASH_DIR" -name '._*' -type f -delete 2>/dev/null || true

	# Nothing this script writes belongs in the image.
	find "$SQUASH_DIR" -name '*.packer.tmp' -type f -delete 2>/dev/null || true

	ROOTFS_NEW="$OTA_DIR/rootfs.squashfs"
	KERNEL_NEW="$KERNEL_DIR/xImage-$MODEL_DIR"

	step "[$MODEL_NAME] building the filesystem"
	mksquashfs "$SQUASH_DIR" "$ROOTFS_NEW" -comp lzo -all-root > /dev/null

	ORIGINAL_SUM="$(get_md5 "$ROOTFS_NEW")"
	SIZE="$(get_size "$ROOTFS_NEW")"
	say "    rootfs.squashfs: $SIZE bytes, md5 $ORIGINAL_SUM"

	step "[$MODEL_NAME] updating ota_update.in"
	if [ -f "$KERNEL_NEW" ]; then
		# A kernel of our own: it replaces the stock one, chunks and md5 chain
		# alike. It may not be bigger than the stock one -- that is all the
		# room the kernel partition is known to have.
		STOCK_X_SIZE="$(grep -A 3 'img_name=xImage' "$OTA_DIR/ota_update.in" | grep 'img_size' | cut -d= -f2 | tr -d '\r ')"
		X_SIZE="$(get_size "$KERNEL_NEW")"
		X_MD5="$(get_md5 "$KERNEL_NEW")"
		[ -n "$STOCK_X_SIZE" ] || die "cannot read the kernel entry from ota_update.in."
		[ "$X_SIZE" -le "$STOCK_X_SIZE" ] ||
			die "kernel/xImage-$MODEL_DIR is $X_SIZE bytes, the stock kernel $STOCK_X_SIZE: it may not fit."
		say "    xImage: kernel/xImage-$MODEL_DIR, $X_SIZE bytes, md5 $X_MD5 (stock: $STOCK_X_SIZE bytes)"
	else
		# The stock kernel stays, so its two lines are carried over exactly as
		# they were: recomputing them from a file this script never writes
		# would be a way of getting them wrong.
		X_SIZE="$(grep -A 3 'img_name=xImage' "$OTA_DIR/ota_update.in" | grep 'img_size' | cut -d= -f2 | tr -d '\r ')"
		X_MD5="$(grep -A 3 'img_name=xImage' "$OTA_DIR/ota_update.in" | grep 'img_md5' | cut -d= -f2 | tr -d '\r ')"
		[ -n "$X_SIZE" ] && [ -n "$X_MD5" ] || die "cannot read the kernel entry from ota_update.in."
		say "    xImage: the stock kernel, unchanged"
	fi

	cat > "$OTA_DIR/ota_update.in" <<-EOF
	ota_version=0

	img_type=kernel
	img_name=xImage
	img_size=$X_SIZE
	img_md5=$X_MD5

	img_type=rootfs
	img_name=rootfs.squashfs
	img_size=$SIZE
	img_md5=$ORIGINAL_SUM
	EOF

	step "[$MODEL_NAME] splitting into chunks and building the md5 chains"
	write_chunks rootfs.squashfs "$ROOTFS_NEW"
	if [ -f "$KERNEL_NEW" ]; then
		rm -f "$OTA_DIR/ota_md5_xImage."* "$OTA_DIR/xImage."*
		cp "$KERNEL_NEW" "$OTA_DIR/xImage"
		write_chunks xImage "$OTA_DIR/xImage"
	fi
	say ""

	# ==========================================================================
	# 9. The image
	# ==========================================================================
	say "${YELLOW}###############################${NC}"
	say "${YELLOW}###   FIRMWARE IMAGE        ###${NC}"
	say "${YELLOW}###############################${NC}"
	say ""

	step "[$MODEL_NAME] writing $(basename "$OUTPUT_UPT")"
	rm -f "$OUTPUT_UPT"
	"$ISO_TOOL" -o "$OUTPUT_UPT" -J -r "$WORK_DIR" > /dev/null 2>&1

	[ -f "$OUTPUT_UPT" ] || die "the image was not written."

	cleanup

	say "  $OUTPUT_UPT"
	say "  $(get_size "$OUTPUT_UPT") bytes, build_version $BUILD_STAMP"
	say ""
}

# ==========================================================================
# Every model that can be built
# ==========================================================================
while IFS='|' read -r m_dir m_src m_out m_name; do
	[ -n "$m_dir" ] || continue
	build_one "$m_dir" "$m_name" "$m_src" "$m_out"
done <<< "$BUILDABLE"

say "${GREEN}#############################${NC}"
say "${GREEN}###   DONE                ###${NC}"
say "${GREEN}#############################${NC}"
say ""
say "${GREEN}  Ready to be copied to the SD card.${NC}"
say ""
