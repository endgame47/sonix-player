# Makefile for sonix_player

# Board selector -- `make target BOARD=r1` (default r3proii if unset). 
# Gates BOARD_DEFINE (-DBOARD_R3PROII / -DBOARD_R1
BOARD ?= r3proii
ifeq ($(filter $(BOARD),r3proii r1),)
$(error Unknown BOARD '$(BOARD)' -- expected r3proii or r1)
endif
BOARD_DEFINE = -DBOARD_$(shell echo $(BOARD) | tr a-z A-Z)

# Target executables
HOST_BIN = sonix_player_host
TARGET_BIN = sonix_player

LVGL_DIR = $(CURDIR)/lvgl
ROCKBOX_TOOLCHAIN_DIR = $(CURDIR)/rockbox-toolchain

CC = gcc
CXX = g++
CROSS_CC ?= $(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-gcc
CROSS_CXX ?= $(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-g++

# The interface includes three of LVGL's private headers
# (lv_timer_private.h, lv_event_private.h, lv_roller_private.h) and calls
# lv_span_get_style() and lv_spangroup_refresh(). None of them exist before 9.5.
LVGL_VER = 9.5.0

# Self-bootstrap: clone LVGL if it doesn't exist yet before evaluating variables
ifeq ($(wildcard $(LVGL_DIR)),)
$(info Cloning LVGL v$(LVGL_VER)...)
$(shell git clone --depth 1 -b v$(LVGL_VER) https://github.com/lvgl/lvgl.git)
endif

# Read out of the headers, so a checkout with no git history still answers.
# Both names are tried: the macros sit in lvgl.h up to 9.1 and in lv_version.h
# from 9.2.
LVGL_VER_FOUND := $(shell sed -n 's/^\#define LVGL_VERSION_\(MAJOR\|MINOR\|PATCH\) *\([0-9]*\).*/\2/p' \
					$(LVGL_DIR)/lv_version.h $(LVGL_DIR)/lvgl.h 2>/dev/null | head -3 | paste -sd. -)
ifneq ($(LVGL_VER_FOUND),)
ifneq ($(LVGL_VER_FOUND),$(LVGL_VER))
$(warning lvgl/ holds $(LVGL_VER_FOUND) and this player needs $(LVGL_VER).)
$(warning Remove the lvgl directory and run make again to fetch the right one.)
$(error wrong LVGL version)
endif
endif

# Compile flags.
#
#   -DLV_CONF_INCLUDE_SIMPLE=1  lets lv_conf.h be included as "lv_conf.h".
#   -MMD -MP                    write a .d per object, pulled back in at the
#                               bottom of this file so a header edit rebuilds.
#   -D_FILE_OFFSET_BITS=64      required on the 32-bit target: without it
#                               readdir()/stat() fail with EOVERFLOW on an inode
#                               or offset wider than 32 bits, which an exFAT
#                               card produces routinely.
CFLAGS = -O3 -g -Wall -MMD -MP -D_FILE_OFFSET_BITS=64 -I. -I$(LVGL_DIR) -DLV_CONF_INCLUDE_SIMPLE=1 -DGB_CORE=1
# On the host the audio libraries are the system ones (libopusfile-dev,
# libwavpack-dev); on the target they are the ones this Makefile builds below.
# opusfile.h includes <opus_multistream.h> unprefixed, so the opus/ directory
# has to be among the -I in both cases.
HOST_CFLAGS = $(CFLAGS) -DHOST_BUILD=1 $(BOARD_DEFINE) $(shell sdl2-config --cflags) $(shell pkg-config --cflags freetype2) \
			  $(shell pkg-config --cflags opusfile wavpack)
# -include glibc_compat.h must enter EVERY translation unit of the target
# build: it carries the .symver lines that keep five maths functions on the
# names the device's glibc 2.22 knows, and a .symver applies only to its own
# unit. Target only.
TARGET_CFLAGS = $(CFLAGS) $(BOARD_DEFINE) -I$(LVGL_DIR)/src -I$(FREETYPE_PREFIX)/include/freetype2 \
				-I$(AUDIO_PREFIX)/include -I$(AUDIO_PREFIX)/include/opus \
				-include src/system/core/glibc_compat.h

# The C++ in this project is the Game Boy core (src/gb, Gearboy) plus the facade
# that makes it callable from the player, which is C.
#
#   -DGEARBOY_DISABLE_DISASSEMBLER  drops the disassembler and the breakpoints:
#       they are for Gearboy's own debugger, which is not here.
#   -fno-exceptions -fno-rtti       the core uses neither, and without them a
#       few tables and some dead code go away.
#   The -I are needed because Gearboy's sources include each other by short name
#       ("Video.h"), as if they were in their own directory -- which is how they
#       sit upstream, and they are not touched so that the next update does not
#       have to be merged by hand.
#   -Wno-attributes                 Gearboy declares InvalidOPCode() noinline
#       first and inline afterwards. Upstream code, left alone on purpose (see
#       src/gb/README.md).
GB_CFLAGS = -DGEARBOY_DISABLE_DISASSEMBLER -fno-exceptions -fno-rtti -Wno-attributes \
			-Isrc/gb/core -Isrc/gb/miniz
HOST_CXXFLAGS = $(HOST_CFLAGS) $(GB_CFLAGS)
TARGET_CXXFLAGS = $(TARGET_CFLAGS) $(GB_CFLAGS)

# Link flags
#
# -static-libstdc++ -static-libgcc are not optional on the target: the device
# has libstdc++.so.6.0.21, that is GLIBCXX_3.4.21 (the GCC 5 ABI), and the
# toolchain is GCC 9, which emits references up to 3.4.26. Linked dynamically
# the binary does not start at all.
#
# Target only. On the host they ask for a static libstdc++ that most
# distributions do not ship, and the link fails with "cannot find -lstdc++".
GB_LDFLAGS = -static-libstdc++ -static-libgcc
HOST_LDFLAGS = $(shell sdl2-config --libs) -lpthread -lm -ldl -lasound $(shell pkg-config --libs freetype2) \
			   $(shell pkg-config --libs opusfile wavpack)
TARGET_LDFLAGS = $(FREETYPE_PREFIX)/lib/libfreetype.a $(AUDIO_LIBS) -lpthread -lm -ldl -lasound \
				 $(GB_LDFLAGS)

# FreeType for the target, built static: the rootfs has no libfreetype.so. The
# host uses the system one. The device's fonts are plain OTF/TTF, so zlib,
# bzip2, png, harfbuzz and brotli are all off.
FREETYPE_VER = 2.13.3
FREETYPE_TARBALL = freetype-$(FREETYPE_VER).tar.xz
FREETYPE_URL = https://download.savannah.gnu.org/releases/freetype/$(FREETYPE_TARBALL)
FREETYPE_URL_MIRROR = https://downloads.sourceforge.net/project/freetype/freetype2/$(FREETYPE_VER)/$(FREETYPE_TARBALL)
FREETYPE_SRC = $(CURDIR)/freetype-$(FREETYPE_VER)
FREETYPE_PREFIX = $(CURDIR)/freetype-target

# The audio libraries carried inside the binary, static like FreeType. The
# firmware's /usr/lib holds a libopus.so, but it is the raw codec with no Ogg
# demuxer, so linking against it is not an option.
#
#   libogg + libopus + opusfile -> .opus (src/system/decode/opusdec.c)
#   libwavpack                  -> .wv   (src/system/decode/wavpackdec.c)
#
# The configure options below were settled against these versions.
# ALAC is not here: its decoder is a single file under src/system/decode/alac/.
AUDIO_PREFIX = $(CURDIR)/audio-target

OGG_VER = 1.3.5
OGG_TARBALL = libogg-$(OGG_VER).tar.gz
OGG_URL = https://downloads.xiph.org/releases/ogg/$(OGG_TARBALL)
OGG_URL_MIRROR = https://ftp.osuosl.org/pub/xiph/releases/ogg/$(OGG_TARBALL)
OGG_SRC = $(CURDIR)/libogg-$(OGG_VER)
OGG_LIB = $(AUDIO_PREFIX)/lib/libogg.a

OPUS_VER = 1.4
OPUS_TARBALL = opus-$(OPUS_VER).tar.gz
OPUS_URL = https://downloads.xiph.org/releases/opus/$(OPUS_TARBALL)
OPUS_URL_MIRROR = https://ftp.osuosl.org/pub/xiph/releases/opus/$(OPUS_TARBALL)
OPUS_SRC = $(CURDIR)/opus-$(OPUS_VER)
OPUS_LIB = $(AUDIO_PREFIX)/lib/libopus.a

OPUSFILE_VER = 0.12
OPUSFILE_TARBALL = opusfile-$(OPUSFILE_VER).tar.gz
OPUSFILE_URL = https://downloads.xiph.org/releases/opus/$(OPUSFILE_TARBALL)
OPUSFILE_URL_MIRROR = https://ftp.osuosl.org/pub/xiph/releases/opus/$(OPUSFILE_TARBALL)
OPUSFILE_SRC = $(CURDIR)/opusfile-$(OPUSFILE_VER)
OPUSFILE_LIB = $(AUDIO_PREFIX)/lib/libopusfile.a

WAVPACK_VER = 5.6.0
WAVPACK_TARBALL = wavpack-$(WAVPACK_VER).tar.bz2
WAVPACK_URL = https://www.wavpack.com/$(WAVPACK_TARBALL)
WAVPACK_URL_MIRROR = https://github.com/dbry/WavPack/releases/download/$(WAVPACK_VER)/$(WAVPACK_TARBALL)
WAVPACK_SRC = $(CURDIR)/wavpack-$(WAVPACK_VER)
WAVPACK_LIB = $(AUDIO_PREFIX)/lib/libwavpack.a

# The order matters: with static libraries the linker resolves left to right,
# and opusfile calls opus which calls ogg.
AUDIO_LIBS = $(OPUSFILE_LIB) $(OPUS_LIB) $(OGG_LIB) $(WAVPACK_LIB)


# Source files. src/gb holds a .c as well (the miniz shell) alongside the C++
# core, so both searches reach into it.
APP_SRCS := $(shell find src -name '*.c')
LVGL_SRCS = $(shell find $(LVGL_DIR)/src -type f -name '*.c')

# The C++ here is the Game Boy core under src/gb/core plus its facade. Separate
# list because it takes different flags, and the link goes through the C++
# compiler so that libstdc++ comes along.
APP_CXX_SRCS := $(shell find src/gb -name '*.cpp')
HOST_LD = $(CXX)
TARGET_LD = $(CROSS_CXX)

# Object files
HOST_OBJS = $(APP_SRCS:src/%.c=build_host/%.o) $(APP_CXX_SRCS:src/%.cpp=build_host/%.opp) \
			$(LVGL_SRCS:$(LVGL_DIR)/%.c=build_host/lvgl/%.o)
TARGET_OBJS = $(APP_SRCS:src/%.c=build_target/%.o) $(APP_CXX_SRCS:src/%.cpp=build_target/%.opp) \
			  $(LVGL_SRCS:$(LVGL_DIR)/%.c=build_target/lvgl/%.o)

.PHONY: all host target clean compile_commands.json

all: host compile_commands.json

host: $(HOST_BIN) compile_commands.json

$(HOST_BIN): $(HOST_OBJS)
	$(HOST_LD) -o $@ $(HOST_OBJS) $(HOST_LDFLAGS)
	@echo "Host build complete: Run './$(HOST_BIN)' to start the simulator."

build_host/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

build_host/%.opp: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(HOST_CXXFLAGS) -c $< -o $@

build_host/lvgl/%.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

target: $(TARGET_BIN) compile_commands.json

$(TARGET_BIN): $(TARGET_OBJS)
	$(TARGET_LD) -o $@ $(TARGET_OBJS) $(TARGET_LDFLAGS)
	@$(MAKE) --no-print-directory check-abi
	@echo "Target build complete: File ready at '$(TARGET_BIN)'"

# ---------------------------------------------------------------------------
# The check the linker does not do
#
# The toolchain compiles against glibc 2.27, the device has 2.22. A binary
# asking for a newer versioned symbol links without a word and then refuses to
# start, and on this device that means a boot loop: sonix_player.sh runs
# `sleep 1; reboot` as soon as the player exits.
#
# glibc_compat.h holds the known functions down; this catches the next ones.
# ---------------------------------------------------------------------------

CROSS_READELF ?= $(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-readelf

# The device's glibc, as major.minor.
DEVICE_GLIBC_MAJOR = 2
DEVICE_GLIBC_MINOR = 22

.PHONY: check-abi
check-abi:
	@if [ ! -x "$(CROSS_READELF)" ]; then \
		echo "check-abi: $(CROSS_READELF) is not there, check skipped"; \
		exit 0; \
	fi; \
	bad=$$($(CROSS_READELF) --dyn-syms -W $(TARGET_BIN) 2>/dev/null \
		| awk '{ for (i = 1; i <= NF; i++) if (match($$i, /@GLIBC_[0-9.]+$$/)) { \
			ver = substr($$i, RSTART + 7); n = split(ver, v, "."); \
			if (v[1] > $(DEVICE_GLIBC_MAJOR) || (v[1] == $(DEVICE_GLIBC_MAJOR) && v[2] > $(DEVICE_GLIBC_MINOR))) print "  " $$i; \
		} }' | sort -u); \
	if [ -n "$$bad" ]; then \
		echo ""; \
		echo "*** ERROR: the binary asks for symbols the device does not have."; \
		echo "*** The device has glibc $(DEVICE_GLIBC_MAJOR).$(DEVICE_GLIBC_MINOR); these want more:"; \
		echo "$$bad"; \
		echo ""; \
		echo "*** As it stands the binary DOES NOT START, and the device boot loops."; \
		echo "*** Add a .symver in src/system/core/glibc_compat.h, which explains how."; \
		echo ""; \
		rm -f $(TARGET_BIN); \
		exit 1; \
	fi; \
	echo "check-abi: no symbol beyond glibc $(DEVICE_GLIBC_MAJOR).$(DEVICE_GLIBC_MINOR)."

build_target/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(TARGET_CFLAGS) -c $< -o $@

build_target/%.opp: src/%.cpp
	@mkdir -p $(dir $@)
	$(CROSS_CXX) $(TARGET_CXXFLAGS) -c $< -o $@

build_target/lvgl/%.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CROSS_CC) $(TARGET_CFLAGS) -c $< -o $@

# For the Zed/clangd LSP: autofill and hover popups.
compile_commands.json:
	@python3 generate_compile_commands.py

clean:
	rm -rf build_host build_target $(HOST_BIN) $(TARGET_BIN) compile_commands.json compile_flags.txt

# Make sure the cross compiler is built before trying to make any of the target
# objects with it.
$(TARGET_OBJS): $(CROSS_CC) $(FREETYPE_PREFIX)/lib/libfreetype.a $(AUDIO_LIBS)
$(TARGET_BIN): $(FREETYPE_PREFIX)/lib/libfreetype.a $(AUDIO_LIBS)

$(CROSS_CC):
	@echo "Building Rockbox toolchain..."
	./rockboxdev/rockboxdev.sh

# Cross-build FreeType, once. configure needs the toolchain on PATH for
# ranlib/ar to be found under their host-triplet names.
$(FREETYPE_PREFIX)/lib/libfreetype.a: $(CROSS_CC)
	@echo "Building static FreeType $(FREETYPE_VER) for the target..."
	@if [ ! -f $(FREETYPE_TARBALL) ]; then \
		wget -O $(FREETYPE_TARBALL) $(FREETYPE_URL) || \
		wget -O $(FREETYPE_TARBALL) $(FREETYPE_URL_MIRROR); \
	fi
	@if [ ! -d $(FREETYPE_SRC) ]; then tar xf $(FREETYPE_TARBALL); fi
	cd $(FREETYPE_SRC) && PATH="$(ROCKBOX_TOOLCHAIN_DIR)/bin:$$PATH" ./configure \
		--host=mipsel-rockbox-linux-gnu \
		--prefix=$(FREETYPE_PREFIX) \
		--disable-shared --enable-static \
		--with-zlib=no --with-bzip2=no --with-png=no \
		--with-harfbuzz=no --with-brotli=no \
		CC=$(CROSS_CC) CFLAGS="-O2" \
		AR=$(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-ar \
		RANLIB=$(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-ranlib
	PATH="$(ROCKBOX_TOOLCHAIN_DIR)/bin:$$PATH" $(MAKE) -C $(FREETYPE_SRC)
	PATH="$(ROCKBOX_TOOLCHAIN_DIR)/bin:$$PATH" $(MAKE) -C $(FREETYPE_SRC) install

# ---------------------------------------------------------------------------
# Downloaded, configured with the toolchain on PATH (configure looks for ar and
# ranlib under the triplet name) and installed into AUDIO_PREFIX. The .a is the
# make target, so each is built once.
# ---------------------------------------------------------------------------

CROSS_PATH = PATH="$(ROCKBOX_TOOLCHAIN_DIR)/bin:$$PATH"
CROSS_TOOLS = CC=$(CROSS_CC) CFLAGS="-O2" \
			  AR=$(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-ar \
			  RANLIB=$(ROCKBOX_TOOLCHAIN_DIR)/bin/mipsel-rockbox-linux-gnu-ranlib
CROSS_COMMON = --host=mipsel-rockbox-linux-gnu --prefix=$(AUDIO_PREFIX) --disable-shared --enable-static

$(OGG_LIB): $(CROSS_CC)
	@echo "Building static libogg $(OGG_VER) for the target..."
	@if [ ! -f $(OGG_TARBALL) ]; then \
		wget -O $(OGG_TARBALL) $(OGG_URL) || wget -O $(OGG_TARBALL) $(OGG_URL_MIRROR); \
	fi
	@if [ ! -d $(OGG_SRC) ]; then tar xf $(OGG_TARBALL); fi
	cd $(OGG_SRC) && $(CROSS_PATH) ./configure $(CROSS_COMMON) $(CROSS_TOOLS)
	$(CROSS_PATH) $(MAKE) -C $(OGG_SRC)
	$(CROSS_PATH) $(MAKE) -C $(OGG_SRC) install

# --enable-fixed-point: integer decoder, bit-exact across platforms. The float
# API stays compiled in -- removing it would take --disable-float-api.
$(OPUS_LIB): $(CROSS_CC)
	@echo "Building static libopus $(OPUS_VER) for the target..."
	@if [ ! -f $(OPUS_TARBALL) ]; then \
		wget -O $(OPUS_TARBALL) $(OPUS_URL) || wget -O $(OPUS_TARBALL) $(OPUS_URL_MIRROR); \
	fi
	@if [ ! -d $(OPUS_SRC) ]; then tar xf $(OPUS_TARBALL); fi
	cd $(OPUS_SRC) && $(CROSS_PATH) ./configure $(CROSS_COMMON) \
		--enable-fixed-point --disable-doc --disable-extra-programs \
		$(CROSS_TOOLS)
	$(CROSS_PATH) $(MAKE) -C $(OPUS_SRC)
	$(CROSS_PATH) $(MAKE) -C $(OPUS_SRC) install

# DEPS_CFLAGS/DEPS_LIBS by hand: opusfile's configure finds ogg and opus with
# PKG_CHECK_MODULES, and no pkg-config here knows the cross sysroot. Passing
# them skips the search. --disable-http drops the stream support that would
# pull in OpenSSL.
$(OPUSFILE_LIB): $(CROSS_CC) $(OGG_LIB) $(OPUS_LIB)
	@echo "Building static opusfile $(OPUSFILE_VER) for the target..."
	@if [ ! -f $(OPUSFILE_TARBALL) ]; then \
		wget -O $(OPUSFILE_TARBALL) $(OPUSFILE_URL) || wget -O $(OPUSFILE_TARBALL) $(OPUSFILE_URL_MIRROR); \
	fi
	@if [ ! -d $(OPUSFILE_SRC) ]; then tar xf $(OPUSFILE_TARBALL); fi
	cd $(OPUSFILE_SRC) && $(CROSS_PATH) ./configure $(CROSS_COMMON) \
		--disable-http --disable-examples --disable-doc \
		DEPS_CFLAGS="-I$(AUDIO_PREFIX)/include -I$(AUDIO_PREFIX)/include/opus" \
		DEPS_LIBS="-L$(AUDIO_PREFIX)/lib -lopus -logg" \
		$(CROSS_TOOLS)
	$(CROSS_PATH) $(MAKE) -C $(OPUSFILE_SRC)
	$(CROSS_PATH) $(MAKE) -C $(OPUSFILE_SRC) install

# --enable-legacy: also reads the .wv written by WavPack 4.0 and earlier, which
# costs a few kilobytes. --enable-dsd keeps DSD inside WavPack, which the
# wrapper opens with OPEN_DSD_AS_PCM. --disable-asm because the assembly wavpack
# ships is for x86 and ARM.
$(WAVPACK_LIB): $(CROSS_CC)
	@echo "Building static libwavpack $(WAVPACK_VER) for the target..."
	@if [ ! -f $(WAVPACK_TARBALL) ]; then \
		wget -O $(WAVPACK_TARBALL) $(WAVPACK_URL) || wget -O $(WAVPACK_TARBALL) $(WAVPACK_URL_MIRROR); \
	fi
	@if [ ! -d $(WAVPACK_SRC) ]; then tar xf $(WAVPACK_TARBALL); fi
	cd $(WAVPACK_SRC) && $(CROSS_PATH) ./configure $(CROSS_COMMON) \
		--disable-apps --disable-man --disable-asm \
		--enable-dsd --enable-legacy --without-libiconv-prefix \
		$(CROSS_TOOLS)
	$(CROSS_PATH) $(MAKE) -C $(WAVPACK_SRC)
	$(CROSS_PATH) $(MAKE) -C $(WAVPACK_SRC) install

# Header dependencies recorded by -MMD (see CFLAGS). The dashes keep make quiet
# on the first build, when no .d files exist yet. The C++ objects end in .opp
# so the two compile rules cannot overlap, which is why both suffixes are
# substituted -- otherwise make reads an object file as a makefile.
-include $(patsubst %.opp,%.d,$(patsubst %.o,%.d,$(HOST_OBJS)))
-include $(patsubst %.opp,%.d,$(patsubst %.o,%.d,$(TARGET_OBJS)))
