// The C facade over Gearboy. See gbcore.h.
//
// The only C++ file written here: everything else under src/gb/core is upstream
// Gearboy verbatim, and stays that way so updating it is a copy and not a
// merge.

#include "gbcore.h"

#include <new>
#include <stdio.h>
#include <string.h>
#include <string>

#include "core/Cartridge.h"
#include "core/GearboyCore.h"
#include "core/Memory.h"
#include "core/definitions.h"

// Gearboy expects the host program to define this: its Log_func() checks it to
// decide whether to stay quiet. True here because the player has no console to
// print to, and a printf per core event on the emulation thread would cost more
// than the emulation.
bool g_mcp_stdio_mode = true;

struct gb_core {
	GearboyCore *core;
	char name[24];
	int sample_rate;
	// Key state from the previous frame: the core wants press/release edges
	// while callers pass a full state mask, so the difference is taken here.
	uint16_t keys;
	// The core always needs somewhere to put audio, even when the caller does
	// not want it: passing NULL would still be written through.
	s16 scratch[GB_AUDIO_MAX_SAMPLES];
};

// The order does not affect correctness, only readability: the same eight
// Gameboy_Keys values, listed the way a thumb reaches them.
static const struct {
	uint16_t bit;
	Gameboy_Keys key;
} KEY_MAP[] = {
	{GB_KEY_UP, Up_Key},		 {GB_KEY_DOWN, Down_Key},	  {GB_KEY_LEFT, Left_Key},
	{GB_KEY_RIGHT, Right_Key},   {GB_KEY_A, A_Key},			  {GB_KEY_B, B_Key},
	{GB_KEY_SELECT, Select_Key}, {GB_KEY_START, Start_Key},
};

extern "C" {

gb_core_t *gb_core_create(void) {
	gb_core_t *wrap = new (std::nothrow) gb_core_t;
	if (!wrap) {
		return NULL;
	}
	memset(wrap, 0, sizeof(*wrap));
	wrap->sample_rate = 44100;

	wrap->core = new (std::nothrow) GearboyCore();
	if (!wrap->core) {
		delete wrap;
		return NULL;
	}

	// RGB565 straight from the core: it is the framebuffer format, so no
	// per-pixel conversion is left between the emulator and the panel.
	wrap->core->Init(GB_PIXEL_RGB565);

	// No Super Game Boy. Two calls rather than one, deliberately:
	//
	//   SetSGBEnabled(false)  turns the mode off entirely. This is the one that
	//       matters: without it an SGB-enhanced cartridge (ROM byte 0x146 equal
	//       to 3) enables it by itself, and RenderSGBFrame() starts writing
	//       256x224 into a 160x144 buffer -- 68 KB past the end, every frame.
	//   SetSGBBorder(false)   drops the border. If SGB is ever re-enabled, with
	//       the border off the core draws into its own buffer and copies out
	//       160x144: same path, without the overrun.
	//
	// Both must be set before loading the ROM, where m_bSGB is computed.
	//
	// Nothing is lost as the page stands: the SGB border is 256x224 while the
	// game view is sized for 160x144 at 3x. An SGB game still runs, in plain
	// Game Boy mode, which is how it ran on a Game Boy.
	wrap->core->SetSGBEnabled(false);
	wrap->core->SetSGBBorder(false);

	return wrap;
}

void gb_core_destroy(gb_core_t *wrap) {
	if (!wrap) {
		return;
	}
	delete wrap->core;
	delete wrap;
}

bool gb_core_load_file(gb_core_t *wrap, const char *path, bool force_dmg) {
	if (!wrap || !path || !path[0]) {
		return false;
	}
	if (!wrap->core->LoadROM(path, force_dmg)) {
		return false;
	}

	// After the load, not before: loading a ROM rebuilds the audio unit, so a
	// sample rate set earlier would be lost.
	wrap->core->SetSoundSampleRate(wrap->sample_rate);

	const char *name = wrap->core->GetCartridge() ? wrap->core->GetCartridge()->GetName() : "";
	snprintf(wrap->name, sizeof(wrap->name), "%s", name ? name : "");
	wrap->keys = 0;
	return true;
}

const char *gb_core_rom_name(gb_core_t *wrap) { return wrap ? wrap->name : ""; }

bool gb_core_is_color(gb_core_t *wrap) { return wrap ? wrap->core->IsCGB() : false; }

// Gearboy tells a directory from a full path with a boolean: with fullPath
// false it appends the cartridge's file name to whatever it is given, which is
// the behaviour wanted here. NULL falls back to its default, the file next to
// the ROM.
void gb_core_load_ram(gb_core_t *wrap, const char *dir) {
	if (wrap) {
		wrap->core->LoadRam(dir && dir[0] ? dir : NULL, false);
	}
}

void gb_core_save_ram(gb_core_t *wrap, const char *dir) {
	if (wrap) {
		wrap->core->SaveRam(dir && dir[0] ? dir : NULL, false);
	}
}

// The save-state path. Gearboy can build it itself, but only inside its own
// writing functions; it is needed outside them here because the file is opened
// by this code.
static std::string state_path(GearboyCore *core, const char *dir, int index) {
	std::string path;
	if (dir && dir[0]) {
		path = dir;
		path += "/";
		path += core->GetCartridge()->GetFileName();
	} else {
		path = core->GetCartridge()->GetFilePath();
	}

	std::string::size_type dot = path.rfind('.');
	if (dot != std::string::npos) {
		path.replace(dot + 1, path.length() - dot - 1, "state");
	}

	char suffix[16];
	snprintf(suffix, sizeof(suffix), "%d", index);
	path += suffix;
	return path;
}

// The state is serialised through a memory buffer instead of the std::ofstream
// Gearboy would use on its own.
//
// The reason is the card. Serialisation is hundreds of small stream.write()
// calls -- a register, a byte, a block -- and on an ofstream opened on the
// microSD each one eventually becomes a real write. Serialising into RAM and
// handing it over with a single fwrite turns hundreds of trips to the card into
// one.
//
// Gearboy already supports this: SaveState(buffer, size, screenshot) with a
// NULL buffer computes the size, and with a real buffer fills it. That is two
// serialisations instead of one, but both in memory, which on this device is
// orders of magnitude faster than the card.
//
// The last argument is the embedded thumbnail: false, since it is another 92 KB
// per state and this menu does not show it.
bool gb_core_save_state(gb_core_t *wrap, const char *dir, int index) {
	if (!wrap || !wrap->core->GetCartridge()) {
		return false;
	}

	size_t size = 0;
	if (!wrap->core->SaveState(NULL, size, false) || size == 0) {
		return false;
	}

	u8 *buffer = new (std::nothrow) u8[size];
	if (!buffer) {
		return false;
	}

	bool ok = wrap->core->SaveState(buffer, size, false);
	if (ok) {
		std::string path = state_path(wrap->core, dir, index);
		FILE *f = fopen(path.c_str(), "wb");
		if (f) {
			ok = fwrite(buffer, 1, size, f) == size;
			// The close matters as much as the write: without checking it, a
			// device powered off straight after is left with a half state
			// file.
			if (fclose(f) != 0) {
				ok = false;
			}
		} else {
			ok = false;
		}
	}

	delete[] buffer;
	return ok;
}

// The same in reverse: the file is read in one go and handed to the core,
// rather than letting it read a field at a time off the card.
bool gb_core_load_state(gb_core_t *wrap, const char *dir, int index) {
	if (!wrap || !wrap->core->GetCartridge()) {
		return false;
	}

	std::string path = state_path(wrap->core, dir, index);
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) {
		return false;
	}

	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0) {
		fclose(f);
		return false;
	}

	u8 *buffer = new (std::nothrow) u8[(size_t)size];
	if (!buffer) {
		fclose(f);
		return false;
	}

	bool ok = fread(buffer, 1, (size_t)size, f) == (size_t)size;
	fclose(f);

	if (ok) {
		ok = wrap->core->LoadState(buffer, (size_t)size);
	}

	delete[] buffer;
	return ok;
}

void gb_core_set_keys(gb_core_t *wrap, uint16_t mask) {
	if (!wrap) {
		return;
	}

	uint16_t changed = (uint16_t)(mask ^ wrap->keys);
	if (!changed) {
		return;
	}

	for (size_t i = 0; i < sizeof(KEY_MAP) / sizeof(KEY_MAP[0]); i++) {
		if (!(changed & KEY_MAP[i].bit)) {
			continue;
		}
		if (mask & KEY_MAP[i].bit) {
			wrap->core->KeyPressed(KEY_MAP[i].key);
		} else {
			wrap->core->KeyReleased(KEY_MAP[i].key);
		}
	}

	wrap->keys = mask;
}

void gb_core_set_sample_rate(gb_core_t *wrap, int rate) {
	if (wrap) {
		wrap->sample_rate = rate;
		wrap->core->SetSoundSampleRate(rate);
	}
}

void gb_core_run_frame(gb_core_t *wrap, uint16_t *pixels, int16_t *audio, int *audio_samples) {
	if (!wrap) {
		return;
	}

	int count = 0;
	s16 *sink = audio ? reinterpret_cast<s16 *>(audio) : wrap->scratch;
	wrap->core->RunToVBlank(reinterpret_cast<u16 *>(pixels), sink, &count, false, NULL, pixels != NULL);

	if (audio_samples) {
		// How many the core produced, not how many were copied out: the APU
		// runs inside the CPU loop regardless, and this count is what paces
		// time (738 per channel at 59.7 frames per second).
		*audio_samples = count;
	}
}

void gb_core_set_dmg_palette(gb_core_t *wrap, const uint8_t colors[4][3]) {
	if (!wrap || !colors) {
		return;
	}
	GB_Color c[4];
	for (int i = 0; i < 4; i++) {
		c[i].red = colors[i][0];
		c[i].green = colors[i][1];
		c[i].blue = colors[i][2];
	}
	wrap->core->SetDMGPalette(c[0], c[1], c[2], c[3]);
}

void gb_core_set_color_correction(gb_core_t *wrap, bool enabled) {
	if (wrap) {
		wrap->core->EnableColorCorrection(enabled);
	}
}

bool gb_core_set_bootroms(gb_core_t *wrap, const char *dmg_path, const char *cgb_path) {
	if (!wrap || !dmg_path || !cgb_path) {
		return false;
	}
	// The files are checked here because Memory::LoadBootroom only logs when a
	// file is missing, and enabling a boot ROM that never loaded gives a white
	// screen when the game starts.
	FILE *a = fopen(dmg_path, "rb");
	FILE *b = fopen(cgb_path, "rb");
	bool ok = a != NULL && b != NULL;
	if (a) {
		fclose(a);
	}
	if (b) {
		fclose(b);
	}
	if (!ok) {
		return false;
	}
	Memory *mem = wrap->core->GetMemory();
	if (!mem) {
		return false;
	}
	mem->LoadBootromDMG(dmg_path);
	mem->LoadBootromGBC(cgb_path);
	mem->EnableBootromDMG(true);
	mem->EnableBootromGBC(true);
	return true;
}

const char *gb_core_build(void) {
#ifdef PERFORMANCE
	return "Gearboy PERFORMANCE (75 machine cycles between updates)";
#else
	return "Gearboy accurate (one machine cycle at a time)";
#endif
}

} // extern "C"
