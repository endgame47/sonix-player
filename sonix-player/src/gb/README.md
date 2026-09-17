# Game Boy core

The emulator itself, reached from Other → Gearboy. Only the core lives here; everything that drives it sits elsewhere in the tree.

| file | what it does |
|---|---|
| `src/system/gearboy/gearboy.c` | the emulation thread: frames, audio, saves |
| `src/system/gearboy/gbinput.c` | the panel read in multitouch while a game runs |
| `src/system/gearboy/gbdb.c` | which ROMs are there and what the cartridges are called |
| `src/gui/gearboy/gearboypage.c` | the game list |
| `src/gui/gearboy/gearboyplay.c` | the game screen and the on-screen pad |
| `src/gui/gearboy/gearboysettings.c` | palette, shader, color correction, boot ROMs |

## Contents

| path | what | licence |
|---|---|---|
| `core/` | [Gearboy](https://github.com/drhelius/Gearboy) by Ignacio Sanchez, its `src/` verbatim | GPL-3.0-or-later |
| `core/audio/` | Blargg's `Gb_Snd_Emu`, carried by Gearboy | LGPL-2.1-or-later |
| `miniz/` | **not** miniz: a stand-in, see below | public domain (the original) |
| `gbcore.h` / `gbcore.cpp` | the gate between the player and the core (C++) | as the rest of the project |

Taken from `drhelius/Gearboy`, commit `542f60b7`. Nothing under `core/` is
modified.

## The gate

`gbcore.h` is the only header the player includes. It covers loading a ROM, running a frame, the key state, battery RAM, save states, the DMG palette, Game Boy Color correction and the boot ROMs.

The core produces RGB565, which is the framebuffer format of this device, so between the emulator and the panel there is only the 3x upscale.

`GB_FRAME_MAX_PIXELS` is 256x224 and not 160x144. Gearboy also does Super Game Boy, and an SGB-enhanced cartridge switches it on from byte 0x146 of the ROM; `RenderSGBFrame()` then draws 256x224 — the picture plus its border — into the caller's buffer. `gb_core_create()` disables SGB, and the buffer is allocated at the worst case regardless.

## The miniz stand-in

Gearboy pulls in miniz for two things: the CRC32 it identifies the M161 and MultiMBC1 multi-game cartridges with, and reading ROMs out of a `.zip`. Only the first is used here - ROMs arrive from a file path.

Real miniz also cannot sit beside what the player already carries. `src/system/image/miniz` is the inflate subset that decodes cover-art PNGs, and the two export the same symbols; the link stops at `multiple definition of tinfl_decompressor_free`.

So `miniz/` here is 130 lines: the real zlib CRC32, and a zip reader that always answers "not a zip". Gearboy's own sources stay byte-identical to upstream. If an update starts calling a miniz symbol that is not in the stand-in, the link says so.

## Building

With the rest of the player: `make target`, or `make host`. The Makefile keeps a separate source list and rule for C++ (`%.opp` objects).

```make
GB_CFLAGS = -DGEARBOY_DISABLE_DISASSEMBLER -fno-exceptions -fno-rtti \
            -Wno-attributes -Isrc/gb/core -Isrc/gb/miniz
```

* `-DGEARBOY_DISABLE_DISASSEMBLER` drops the disassembler and the breakpoints, which belong to Gearboy's own debugger.
* `-fno-exceptions -fno-rtti` - the core uses neither.
* `-Wno-attributes` - `InvalidOPCode()` is declared noinline and then inline.
* The `-I` are needed because Gearboy's sources include each other by short name (`"Video.h"`), as they sit upstream.

`-DPERFORMANCE` is Gearboy's own switch: the CPU runs 75 machine cycles before the PPU, APU and timer are brought up to date instead of stepping one at a time. It is off. Mid-scanline effects break with it on.

The target link carries `-static-libstdc++ -static-libgcc`. The device has `libstdc++.so.6.0.21`, which is `GLIBCXX_3.4.21` (the GCC 5 ABI), and the toolchain is GCC 9, which emits references up to `3.4.26`. Linked dynamically the binary does not start.

`common.h` corrects color gamma with `powf()`.
`src/system/core/glibc_compat.h`, force-included into every target unit, keeps that call on the symbol version the device's glibc 2.22 carries, and `make check-abi` fails the build if anything in the binary asks for a version that is not on the device.


## What the player hands over

**Audio.** `audio_external_begin_latency()` opens the PCM at 46. Writing one frame's samples to a blocking PCM is also the metronome: the DAC's crystal sets the pace, so picture and sound cannot drift apart. The panel runs at 60 Hz and the game at 59.73, and frames that fall between two vsyncs are not seen.

**Input.** LVGL's indev is a pointer device - one contact, one position. While a game runs, `panel_touch_enable(false)` parks it and `gbinput.c` reads the evdev node itself in the multitouch protocol, tracking up to five contacts. This needs the touch driver patch in [PATCHES.md](../../PATCHES.md); without it the emulator says so and falls back to one finger at a time.
On the host build the LVGL buttons stay.

## Updating Gearboy

A copy, not a merge:

```sh
git clone --depth 1 https://github.com/drhelius/Gearboy /tmp/gearboy
cp /tmp/gearboy/src/*.cpp /tmp/gearboy/src/*.h              src/gb/core/
cp /tmp/gearboy/src/audio/*.cpp /tmp/gearboy/src/audio/*.h  src/gb/core/audio/
cp /tmp/gearboy/LICENSE                                     src/gb/core/LICENSE
```
