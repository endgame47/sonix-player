#include "albumart.h"

#include "src/system/library/cue.h"
#include "src/system/decode/decode.h"
#include "src/system/decode/mp4.h"
#include "src/system/decode/apedec.h"
#include "src/system/library/oggtags.h"
#include "src/system/core/utils.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// APIC / FLAC PICTURE picture type for "front cover". When a file carries
// several pictures (front, back, artist, ...) this is the one to keep.
#define PIC_TYPE_FRONT_COVER 3

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static uint32_t be32(const uint8_t *b) {
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static uint32_t syncsafe32(const uint8_t *b) {
	return ((uint32_t)(b[0] & 0x7F) << 21) | ((uint32_t)(b[1] & 0x7F) << 14) | ((uint32_t)(b[2] & 0x7F) << 7) |
		   (uint32_t)(b[3] & 0x7F);
}

// Only JPEG and PNG reach the GUI: those are the two formats the image decoder
// is built with, and together they cover essentially every tagged file in the
// wild.
static bool is_supported_image(const uint8_t *data, size_t size) {
	if (!data || size < 12)
		return false;

	// JPEG: FF D8 FF
	if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
		return true;

	// PNG: 89 'P' 'N' 'G' 0D 0A 1A 0A
	static const uint8_t png_magic[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
	if (memcmp(data, png_magic, sizeof(png_magic)) == 0)
		return true;

	return false;
}

// Validates and takes a private copy of a picture payload. Returns false (and
// leaves `out` untouched) if the payload is not a usable image.
static bool take_picture(albumart_t *out, const uint8_t *data, size_t size) {
	if (size == 0 || size > ALBUMART_MAX_BYTES || !is_supported_image(data, size))
		return false;

	uint8_t *copy = malloc(size);
	if (!copy)
		return false;

	memcpy(copy, data, size);

	albumart_free(out); // drop any lower-priority picture picked up earlier
	out->data = copy;
	out->size = size;
	return true;
}

// Takes `data`, a malloc'd picture, into `out` when it is a usable image, and
// frees it otherwise.
static bool adopt_picture(albumart_t *out, uint8_t *data, size_t size) {
	if (!data || size > ALBUMART_MAX_BYTES || !is_supported_image(data, size)) {
		free(data);
		return false;
	}
	albumart_free(out);
	out->data = data;
	out->size = size;
	return true;
}

// ---------------------------------------------------------------------------
// Pictures mapped from the file
//
// A picture stored as it is in the file is mapped, not copied: its pages are
// page cache, given back when the picture is freed. Reading a mapped page
// whose card has been pulled out raises SIGBUS. The handler finds the mapping
// the address belongs to, puts zero pages in its place and lets the read carry
// on; the picture is then faulted (albumart_faulted()) and whatever was made
// from it is thrown away. Any other SIGBUS goes to the handler installed
// before this one.
// ---------------------------------------------------------------------------

#define LIVE_MAPS 16

// `gen` is odd while a slot changes, so the handler can tell a consistent
// lo/hi pair from one caught half-written.
static struct {
	volatile uint32_t gen;
	volatile uintptr_t lo; // 0 while the slot is free
	volatile uintptr_t hi;
	volatile sig_atomic_t faulted;
} live_maps[LIVE_MAPS];
static pthread_mutex_t live_maps_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t bus_once = PTHREAD_ONCE_INIT;
static struct sigaction bus_chained;

static void bus_handler(int sig, siginfo_t *info, void *context) {
	uintptr_t addr = (uintptr_t)info->si_addr;
	for (int i = 0; i < LIVE_MAPS; i++) {
		uint32_t gen = live_maps[i].gen;
		__sync_synchronize();
		uintptr_t lo = live_maps[i].lo;
		uintptr_t hi = live_maps[i].hi;
		__sync_synchronize();
		if ((gen & 1) || gen != live_maps[i].gen) {
			continue;
		}
		if (lo && addr >= lo && addr < hi &&
			mmap((void *)lo, hi - lo, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED) {
			live_maps[i].faulted = 1;
			return;
		}
	}
	if (bus_chained.sa_flags & SA_SIGINFO) {
		bus_chained.sa_sigaction(sig, info, context);
	} else if (bus_chained.sa_handler != SIG_DFL && bus_chained.sa_handler != SIG_IGN) {
		bus_chained.sa_handler(sig);
	} else {
		signal(sig, SIG_DFL);
		raise(sig);
	}
}

static void bus_install(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = bus_handler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGBUS, &sa, &bus_chained);
}

// A slot for a new mapping, or -1 when all are taken.
static int live_map_add(void *map, size_t len) {
	pthread_once(&bus_once, bus_install);
	int slot = -1;
	pthread_mutex_lock(&live_maps_lock);
	for (int i = 0; i < LIVE_MAPS; i++) {
		if (!live_maps[i].lo) {
			live_maps[i].gen++;
			__sync_synchronize();
			live_maps[i].faulted = 0;
			live_maps[i].lo = (uintptr_t)map;
			live_maps[i].hi = (uintptr_t)map + len;
			__sync_synchronize();
			live_maps[i].gen++;
			slot = i;
			break;
		}
	}
	pthread_mutex_unlock(&live_maps_lock);
	return slot;
}

static void live_map_remove(int slot) {
	pthread_mutex_lock(&live_maps_lock);
	live_maps[slot].gen++;
	__sync_synchronize();
	live_maps[slot].lo = 0;
	live_maps[slot].hi = 0;
	__sync_synchronize();
	live_maps[slot].gen++;
	pthread_mutex_unlock(&live_maps_lock);
}

bool albumart_faulted(const albumart_t *art) {
	return art && art->map && live_maps[art->map_slot].faulted;
}

void albumart_free(albumart_t *art) {
	if (!art)
		return;
	if (art->map) {
		live_map_remove(art->map_slot);
		munmap(art->map, art->map_len);
		// Read once, for one decode: the pages go back now rather than
		// pushing out what the player and the audio decoder still use.
		posix_fadvise(art->map_fd, (off_t)art->map_at, (off_t)art->map_len, POSIX_FADV_DONTNEED);
		close(art->map_fd);
	} else {
		free(art->data);
	}
	memset(art, 0, sizeof(*art));
}

// The picture stored as it is at [offset, offset + size) of the file, mapped
// (see above). Copied instead, up to ALBUMART_MAX_BYTES, when it cannot be
// mapped. Returns false, leaving `out` untouched, when the bytes are not a
// usable image.
static bool take_file_range(albumart_t *out, const char *path, long long offset, size_t size) {
	if (size < 12 || size > ALBUMART_MAP_MAX_BYTES || offset < 0) {
		return false;
	}
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	struct stat st;
	long page = sysconf(_SC_PAGESIZE);
	if (page <= 0 || fstat(fd, &st) != 0 || offset + (long long)size > (long long)st.st_size) {
		close(fd);
		return false;
	}

	long long at = offset - offset % page;
	size_t len = (size_t)(offset - at) + size;
	void *map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, (off_t)at);
	int slot = map != MAP_FAILED ? live_map_add(map, len) : -1;
	if (slot >= 0) {
		uint8_t *data = (uint8_t *)map + (offset - at);
		if (!is_supported_image(data, size) || live_maps[slot].faulted) {
			live_map_remove(slot);
			munmap(map, len);
			close(fd);
			return false;
		}
		posix_fadvise(fd, (off_t)at, (off_t)len, POSIX_FADV_SEQUENTIAL);
		albumart_free(out);
		out->data = data;
		out->size = size;
		out->map = map;
		out->map_len = len;
		out->map_at = at;
		out->map_fd = fd;
		out->map_slot = slot;
		return true;
	}
	if (map != MAP_FAILED) {
		munmap(map, len);
	}

	uint8_t *buf = size <= ALBUMART_MAX_BYTES ? malloc(size) : NULL;
	bool ok = buf && pread(fd, buf, size, (off_t)offset) == (ssize_t)size;
	close(fd);
	if (!ok) {
		free(buf);
		return false;
	}
	return adopt_picture(out, buf, size);
}

// ---------------------------------------------------------------------------
// MP3: ID3v2 APIC (v2.3/v2.4) and PIC (v2.2)
// ---------------------------------------------------------------------------

// Reverses ID3v2 unsynchronisation in place (every FF 00 pair becomes a plain
// FF) and returns the new length.
static size_t de_unsynchronise(uint8_t *data, size_t size) {
	size_t w = 0;
	for (size_t r = 0; r < size; r++) {
		data[w++] = data[r];
		if (data[r] == 0xFF && r + 1 < size && data[r + 1] == 0x00)
			r++; // swallow the inserted zero byte
	}
	return w;
}

// Skips a text string inside an APIC frame, honouring the frame's text
// encoding (UTF-16 variants are terminated by two zero bytes, not one).
// Returns the offset just past the terminator, or `size` if none was found.
static size_t skip_encoded_string(const uint8_t *data, size_t size, size_t pos, uint8_t encoding) {
	bool utf16 = (encoding == 0x01 || encoding == 0x02);

	if (utf16) {
		while (pos + 1 < size) {
			if (data[pos] == 0 && data[pos + 1] == 0)
				return pos + 2;
			pos += 2;
		}
		return size;
	}

	while (pos < size) {
		if (data[pos] == 0)
			return pos + 1;
		pos++;
	}
	return size;
}

// Where the image starts in an APIC (v2.3/v2.4) or PIC (v2.2) frame body, of
// which `size` bytes are at `body`; 0 when it does not start within them.
// `is_v22` selects the old layout, whose "MIME type" is a fixed 3-character
// format id ("JPG"/"PNG") instead of a null-terminated string. The picture
// type (0-20) goes to `out_type`.
static size_t apic_image_at(const uint8_t *body, size_t size, bool is_v22, uint8_t *out_type) {
	if (size < 4)
		return 0;

	uint8_t encoding = body[0];
	size_t pos = 1;

	if (is_v22) {
		pos += 3; // image format, e.g. "JPG"
	} else {
		pos = skip_encoded_string(body, size, pos, 0x00); // MIME type is always ISO-8859-1
	}
	if (pos >= size)
		return 0;

	*out_type = body[pos];
	pos++;

	pos = skip_encoded_string(body, size, pos, encoding); // description
	return pos < size ? pos : 0;
}

static bool parse_apic_body(const uint8_t *body, size_t size, bool is_v22, albumart_t *out, uint8_t *out_type) {
	size_t at = apic_image_at(body, size, is_v22, out_type);
	return at > 0 && take_picture(out, body + at, size - at);
}

// Whether `pos` is where another frame header begins -- or where the tag ends,
// either by running out or by turning into padding. Frame ids are upper-case
// letters and digits, which is what makes the guess reliable enough to choose
// between two readings of a frame size.
static bool frame_starts_at(const uint8_t *tag, size_t tag_size, size_t pos) {
	if (pos >= tag_size)
		return pos == tag_size;
	if (tag[pos] == 0)
		return true; // padding
	if (pos + 4 > tag_size)
		return false;

	for (size_t i = 0; i < 4; i++) {
		uint8_t c = tag[pos + i];
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
			return false;
	}
	return true;
}

// A v2.3 tag unsynchronised as a whole, read into memory: its frame sizes
// count the bytes after the unsynchronisation is undone, so the frames cannot
// be found in the file itself. `header` is the tag header, already read; the
// file is positioned just past it.
static bool read_id3v2_whole(FILE *f, const uint8_t *header, albumart_t *out) {
	uint8_t major = header[3];
	uint8_t tag_flags = header[5];
	size_t tag_size = syncsafe32(&header[6]);

	// A tag that cannot hold a sane picture (or is absurdly large) is not worth
	// pulling into RAM on a 64 MB device.
	if (tag_size < 16 || tag_size > ALBUMART_MAX_BYTES + (1024 * 1024)) {
		return false;
	}

	uint8_t *tag = malloc(tag_size);
	if (!tag) {
		return false;
	}
	size_t got = fread(tag, 1, tag_size, f);
	tag_size = got;

	// v2.3 applies unsynchronisation to the whole tag, so undo it up front and
	// parse plain bytes from here on. (v2.4 does it per frame, handled below.)
	bool tag_unsync = (tag_flags & 0x80) != 0;
	if (tag_unsync && major < 4)
		tag_size = de_unsynchronise(tag, tag_size);

	size_t pos = 0;

	// Skip the extended header if there is one.
	if (tag_flags & 0x40) {
		if (major >= 3 && pos + 4 <= tag_size) {
			uint32_t ext_size = (major >= 4) ? syncsafe32(tag + pos) : be32(tag + pos);
			pos += (major >= 4) ? ext_size : ext_size + 4;
		}
	}

	size_t header_len = (major < 3) ? 6 : 10;
	bool found = false;
	uint8_t best_type = 0xFF;

	while (pos + header_len <= tag_size) {
		const uint8_t *fh = tag + pos;
		if (fh[0] == 0)
			break; // padding

		char id[5] = {0};
		size_t frame_size;
		uint16_t frame_flags = 0;

		if (major < 3) {
			memcpy(id, fh, 3);
			frame_size = ((size_t)fh[3] << 16) | ((size_t)fh[4] << 8) | (size_t)fh[5];
		} else {
			memcpy(id, fh, 4);
			frame_size = (major >= 4) ? syncsafe32(fh + 4) : be32(fh + 4);
			frame_flags = ((uint16_t)fh[8] << 8) | fh[9];
		}

		pos += header_len;

		// Several widely used taggers write v2.4 frame sizes as plain 32-bit
		// integers rather than the syncsafe ones the version calls for. Read
		// the syncsafe way such a size is far too small, the walk lands inside
		// the picture data, and everything from there on -- the cover
		// included -- is lost. Take the plain reading when it is the one that
		// lands on the next frame.
		if (major >= 4) {
			size_t plain = be32(fh + 4);
			if (plain != frame_size && plain <= tag_size - pos && !frame_starts_at(tag, tag_size, pos + frame_size) &&
				frame_starts_at(tag, tag_size, pos + plain)) {
				frame_size = plain;
			}
		}

		if (frame_size == 0 || frame_size > tag_size - pos)
			break;

		bool is_picture = (major < 3) ? (strcmp(id, "PIC") == 0) : (strcmp(id, "APIC") == 0);
		if (is_picture) {
			uint8_t *body = tag + pos;
			size_t body_size = frame_size;

			// v2.4 per-frame unsynchronisation.
			uint8_t *unsync_copy = NULL;
			if (major >= 4 && (frame_flags & 0x0002)) {
				unsync_copy = malloc(body_size);
				if (unsync_copy) {
					memcpy(unsync_copy, body, body_size);
					body_size = de_unsynchronise(unsync_copy, body_size);
					body = unsync_copy;
				}
			}

			uint8_t type = 0xFF;
			// Keep the first usable picture, but let a front cover replace it.
			if ((!found || best_type != PIC_TYPE_FRONT_COVER) &&
				parse_apic_body(body, body_size, major < 3, out, &type)) {
				found = true;
				best_type = type;
			}

			free(unsync_copy);

			if (found && best_type == PIC_TYPE_FRONT_COVER)
				break; // the front cover: nothing better can follow
		}

		pos += frame_size;
	}

	free(tag);
	return found;
}

// Whether `at` is where another frame header begins in the tag ending at `end`
// -- or where the tag ends, by running out or by turning into padding. The
// file-side twin of frame_starts_at().
static bool frame_starts_in_file(FILE *f, off_t end, off_t at) {
	if (at >= end)
		return at == end;
	uint8_t id[4];
	size_t n = end - at < 4 ? (size_t)(end - at) : 4;
	if (fseeko(f, at, SEEK_SET) != 0 || fread(id, 1, n, f) != n)
		return false;
	if (id[0] == 0)
		return true; // padding
	if (n < 4)
		return false;
	for (size_t i = 0; i < 4; i++) {
		if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9')))
			return false;
	}
	return true;
}

// v2.4 frame flags.
#define ID3_V24_COMPRESSED 0x0008
#define ID3_V24_ENCRYPTED 0x0004
#define ID3_V24_UNSYNC 0x0002
#define ID3_V24_DATA_LENGTH 0x0001
// v2.3 frame flags.
#define ID3_V23_COMPRESSED 0x0080
#define ID3_V23_ENCRYPTED 0x0040

// The few hundred bytes in front of the image: encoding, MIME type, picture
// type, description. A description longer than this leaves the picture out.
#define APIC_HEAD_MAX 1024

// Reads the ID3v2 tag that starts at the file's current position and keeps its
// best picture: the first usable one, or a front cover after it. Separate from
// the callers below because three containers put the very same tag in three
// different places: an MP3 or a raw .aac at byte zero, a .dsf at the offset its
// header points to, an AIFF or a WAV inside an "ID3 " chunk.
//
// The frames are walked in the file and the image is mapped where it lies;
// only a frame unsynchronised on its own is read into memory to be undone.
static bool read_id3v2_picture(FILE *f, const char *path, albumart_t *out) {
	off_t tag_at = ftello(f);
	uint8_t header[10];
	if (tag_at < 0 || fread(header, 1, sizeof(header), f) != sizeof(header) || memcmp(header, "ID3", 3) != 0) {
		return false;
	}

	uint8_t major = header[3];
	uint8_t tag_flags = header[5];
	off_t tag_size = (off_t)syncsafe32(&header[6]);
	if (tag_size < 16) {
		return false;
	}
	if ((tag_flags & 0x80) && major < 4) {
		return read_id3v2_whole(f, header, out);
	}

	off_t end = tag_at + 10 + tag_size;
	off_t pos = tag_at + 10;

	// Skip the extended header if there is one.
	if ((tag_flags & 0x40) && major >= 3) {
		uint8_t b[4];
		if (fread(b, 1, 4, f) != 4) {
			return false;
		}
		pos += (major >= 4) ? (off_t)syncsafe32(b) : (off_t)be32(b) + 4;
	}

	size_t header_len = (major < 3) ? 6 : 10;
	bool found = false;
	uint8_t best_type = 0xFF;

	while (pos + (off_t)header_len <= end && !(found && best_type == PIC_TYPE_FRONT_COVER)) {
		uint8_t fh[10];
		if (fseeko(f, pos, SEEK_SET) != 0 || fread(fh, 1, header_len, f) != header_len || fh[0] == 0)
			break; // out of file, or padding

		char id[5] = {0};
		off_t frame_size;
		uint16_t frame_flags = 0;
		if (major < 3) {
			memcpy(id, fh, 3);
			frame_size = ((off_t)fh[3] << 16) | ((off_t)fh[4] << 8) | (off_t)fh[5];
		} else {
			memcpy(id, fh, 4);
			frame_size = (off_t)((major >= 4) ? syncsafe32(fh + 4) : be32(fh + 4));
			frame_flags = ((uint16_t)fh[8] << 8) | fh[9];
		}
		off_t body = pos + (off_t)header_len;

		// Plain 32-bit sizes where v2.4 calls for syncsafe ones, as several
		// taggers write them: taken when that is the reading that lands on the
		// next frame.
		if (major >= 4) {
			off_t plain = (off_t)be32(fh + 4);
			if (plain != frame_size && plain <= end - body && !frame_starts_in_file(f, end, body + frame_size) &&
				frame_starts_in_file(f, end, body + plain)) {
				frame_size = plain;
			}
		}
		if (frame_size == 0 || frame_size > end - body)
			break;

		bool is_picture = (major < 3) ? (strcmp(id, "PIC") == 0) : (strcmp(id, "APIC") == 0);
		bool opaque = (major == 3 && (frame_flags & (ID3_V23_COMPRESSED | ID3_V23_ENCRYPTED))) ||
					  (major >= 4 && (frame_flags & (ID3_V24_COMPRESSED | ID3_V24_ENCRYPTED)));
		if (is_picture && !opaque) {
			off_t data_at = body;
			off_t data_size = frame_size;
			if (major >= 4 && (frame_flags & ID3_V24_DATA_LENGTH) && data_size > 4) {
				data_at += 4;
				data_size -= 4;
			}

			uint8_t type = 0xFF;
			bool took = false;
			if (major >= 4 && (frame_flags & ID3_V24_UNSYNC)) {
				uint8_t *copy = data_size <= ALBUMART_MAX_BYTES + 65536 ? malloc((size_t)data_size) : NULL;
				if (copy && fseeko(f, data_at, SEEK_SET) == 0 &&
					fread(copy, 1, (size_t)data_size, f) == (size_t)data_size) {
					size_t n = de_unsynchronise(copy, (size_t)data_size);
					took = parse_apic_body(copy, n, false, out, &type);
				}
				free(copy);
			} else {
				uint8_t head[APIC_HEAD_MAX];
				size_t n = data_size < (off_t)sizeof(head) ? (size_t)data_size : sizeof(head);
				size_t at = 0;
				if (fseeko(f, data_at, SEEK_SET) == 0 && fread(head, 1, n, f) == n) {
					at = apic_image_at(head, n, major < 3, &type);
				}
				took = at > 0 && take_file_range(out, path, (long long)(data_at + (off_t)at),
												 (size_t)(data_size - (off_t)at));
			}
			if (took) {
				found = true;
				best_type = type;
			}
		}

		pos = body + frame_size;
	}

	return found;
}

static bool read_mp3_embedded(const char *filepath, albumart_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f)
		return false;

	bool found = read_id3v2_picture(f, filepath, out);
	fclose(f);
	return found;
}

// ---------------------------------------------------------------------------
// DSD: the ID3v2 tag a .dsf points at
//
// A .dsf keeps an ordinary ID3v2 tag -- APIC and all -- at the offset written
// in the eight bytes at 20 of its header. A .dff has no picture: its DIIN
// chunks hold text only, so those fall through to the folder.
// ---------------------------------------------------------------------------

static bool read_dsf_embedded(const char *filepath, albumart_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f)
		return false;

	bool found = false;
	uint8_t head[28];
	if (fread(head, 1, sizeof(head), f) == sizeof(head) && memcmp(head, "DSD ", 4) == 0) {
		uint64_t tag_at = 0;
		for (int i = 7; i >= 0; i--) {
			tag_at = (tag_at << 8) | head[20 + i];
		}
		// Zero means no tag, which is common. Anything past the end of the file
		// is a corrupt offset that would otherwise send the read somewhere
		// arbitrary.
		if (fseeko(f, 0, SEEK_END) == 0) {
			off_t size = ftello(f);
			if (tag_at != 0 && size > 0 && tag_at < (uint64_t)size && fseeko(f, (off_t)tag_at, SEEK_SET) == 0) {
				found = read_id3v2_picture(f, filepath, out);
			}
		}
	}

	fclose(f);
	return found;
}

// ---------------------------------------------------------------------------
// AIFF and WAV: the ID3 chunk
//
// Neither has a place of its own for a picture, so taggers park a whole ID3v2
// tag in an "ID3 " (AIFF) or "id3 " (WAV) chunk, APIC and all. libsndfile plays
// these files but hands over no pictures, so the chunk is walked here. The two
// containers differ only in the byte order of the chunk sizes.
//
// In a WAV the chunk follows the audio, which in a long hi-res recording is
// past two gigabytes: the walk keeps its position in an off_t.
// ---------------------------------------------------------------------------

static bool read_iff_embedded(const char *filepath, albumart_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f)
		return false;

	bool found = false;
	uint8_t form[12];
	bool aiff = false;
	bool wav = false;
	if (fread(form, 1, sizeof(form), f) == sizeof(form)) {
		aiff = memcmp(form, "FORM", 4) == 0;
		wav = memcmp(form, "RIFF", 4) == 0 && memcmp(form + 8, "WAVE", 4) == 0;
	}
	off_t file_end = (aiff || wav) && fseeko(f, 0, SEEK_END) == 0 ? ftello(f) : 0;

	// Chunk bodies are padded to an even length, which the size field does not
	// count. The guard stops a corrupt file from walking for ever.
	off_t pos = 12;
	for (int guard = 0; guard < 64 && !found && pos + 8 <= file_end; guard++) {
		uint8_t ch[8];
		if (fseeko(f, pos, SEEK_SET) != 0 || fread(ch, 1, sizeof(ch), f) != sizeof(ch)) {
			break;
		}
		uint32_t size = aiff ? be32(ch + 4) : ((uint32_t)ch[4] | ((uint32_t)ch[5] << 8) | ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24));
		if (memcmp(ch, "ID3 ", 4) == 0 || memcmp(ch, "id3 ", 4) == 0) {
			found = read_id3v2_picture(f, filepath, out);
			break;
		}
		pos += 8 + (off_t)size + (off_t)(size & 1);
	}

	fclose(f);
	return found;
}

// ---------------------------------------------------------------------------
// FLAC: PICTURE metadata block
// ---------------------------------------------------------------------------

// The metadata blocks are walked here rather than through dr_flac, which reads
// a whole PICTURE block into memory before the callback sees it: this reads
// the few fields in front of the picture and leaves the bytes where they are,
// for take_file_range() to copy or map. The front cover wins; failing that,
// the last usable picture.
#define FLAC_MAX_BLOCKS 1024

static bool read_flac_embedded(const char *filepath, albumart_t *out) {
	FILE *f = fopen(filepath, "rb");
	if (!f)
		return false;

	// An ID3v2 tag in front of the stream, which some taggers write and the
	// decoder skips.
	long long pos = 0;
	uint8_t head[10];
	if (fread(head, 1, sizeof(head), f) == sizeof(head) && memcmp(head, "ID3", 3) == 0) {
		pos = 10 + (long long)syncsafe32(head + 6) + ((head[5] & 0x10) ? 10 : 0);
	}
	uint8_t magic[4];
	if (fseeko(f, (off_t)pos, SEEK_SET) != 0 || fread(magic, 1, 4, f) != 4 || memcmp(magic, "fLaC", 4) != 0) {
		fclose(f);
		return false;
	}
	pos += 4;

	bool found = false;
	uint32_t best_type = 0;
	for (int n = 0; n < FLAC_MAX_BLOCKS; n++) {
		uint8_t bh[4];
		if (fseeko(f, (off_t)pos, SEEK_SET) != 0 || fread(bh, 1, 4, f) != 4)
			break;
		bool last = (bh[0] & 0x80) != 0;
		int type = bh[0] & 0x7F;
		uint32_t length = ((uint32_t)bh[1] << 16) | ((uint32_t)bh[2] << 8) | (uint32_t)bh[3];
		long long body = pos + 4;

		// PICTURE: type, MIME, description, four 32-bit dimensions, length.
		if (type == 6 && !(found && best_type == PIC_TYPE_FRONT_COVER) && length >= 32) {
			uint8_t b[8];
			long long at = body;
			uint32_t ptype = 0, mime_len = 0, desc_len = 0, data_len = 0;
			bool ok = fread(b, 1, 8, f) == 8;
			if (ok) {
				ptype = be32(b);
				mime_len = be32(b + 4);
				at += 8 + (long long)mime_len;
				ok = mime_len <= length && fseeko(f, (off_t)at, SEEK_SET) == 0 && fread(b, 1, 4, f) == 4;
			}
			if (ok) {
				desc_len = be32(b);
				at += 4 + (long long)desc_len + 16;
				ok = desc_len <= length && fseeko(f, (off_t)at, SEEK_SET) == 0 && fread(b, 1, 4, f) == 4;
			}
			if (ok) {
				data_len = be32(b);
				at += 4;
				ok = at + (long long)data_len <= body + (long long)length;
			}
			if (ok && take_file_range(out, filepath, at, data_len)) {
				found = true;
				best_type = ptype;
			}
		}

		pos = body + (long long)length;
		if (last)
			break;
	}

	fclose(f);
	return found;
}

// ---------------------------------------------------------------------------
// OGG Vorbis: METADATA_BLOCK_PICTURE / COVERART comments
// ---------------------------------------------------------------------------

static int base64_value(char c) {
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1; // padding, whitespace or garbage
}

// A base64 value read through oggtags and decoded as it comes, so neither the
// text nor more than the image itself is ever in memory.
typedef struct {
	oggtags_value_t *value;
	char in[4096];
	size_t have, used;
	uint32_t accum;
	int bits;
	bool end;
} b64_stream_t;

// Decodes up to `n` bytes into `dst`, or skips them when `dst` is NULL.
static size_t b64_read(b64_stream_t *b, uint8_t *dst, size_t n) {
	size_t done = 0;
	while (done < n) {
		if (b->bits >= 8) {
			b->bits -= 8;
			if (dst)
				dst[done] = (uint8_t)(b->accum >> b->bits);
			done++;
			continue;
		}
		if (b->used == b->have) {
			b->have = b->end ? 0 : oggtags_value_read(b->value, b->in, sizeof(b->in));
			b->used = 0;
			if (b->have == 0) {
				b->end = true;
				break;
			}
		}
		int v = base64_value(b->in[b->used++]);
		if (v < 0)
			continue;
		b->accum = ((b->accum << 6) | (uint32_t)v) & 0xFFFFFF;
		b->bits += 6;
	}
	return done;
}

typedef struct {
	albumart_t *out;
	bool found;
	uint32_t best_type;
} ogg_art_t;

// METADATA_BLOCK_PICTURE holds a FLAC PICTURE block (big endian: type, MIME,
// description, four dimensions, length, image); COVERART the image alone. The
// front cover ends the walk; otherwise a later picture replaces an earlier
// one.
static bool ogg_art_comment(void *user, const char *key, size_t key_len, uint32_t value_len,
							oggtags_value_t *value) {
	ogg_art_t *a = user;
	if (!oggtags_key_is_picture(key, key_len))
		return true;
	bool block = key_len == 22;

	b64_stream_t b;
	memset(&b, 0, sizeof(b));
	b.value = value;
	uint32_t type = 0xFF;
	size_t size = (size_t)value_len / 4 * 3; // COVERART: the most it can decode to
	if (block) {
		uint8_t f[8];
		if (b64_read(&b, f, 8) != 8)
			return true;
		type = be32(f);
		uint32_t mime_len = be32(f + 4);
		if (b64_read(&b, NULL, mime_len) != mime_len || b64_read(&b, f, 4) != 4)
			return true;
		uint32_t desc_len = be32(f);
		if (b64_read(&b, NULL, desc_len) != desc_len || b64_read(&b, NULL, 16) != 16 || b64_read(&b, f, 4) != 4)
			return true;
		size = be32(f);
	}
	if (size == 0 || size > ALBUMART_MAX_BYTES)
		return true;

	uint8_t *data = malloc(size);
	if (!data)
		return true;
	size_t got = b64_read(&b, data, size);
	if (block && got != size) {
		free(data);
		return true;
	}
	if (adopt_picture(a->out, data, got)) {
		a->found = true;
		a->best_type = type;
	}
	return !(a->found && a->best_type == PIC_TYPE_FRONT_COVER);
}

// Ogg Vorbis and Opus: the same Vorbis comments, walked in the file (see
// oggtags.h). A picture there is base64 text and has to be decoded into
// memory, up to ALBUMART_MAX_BYTES.
static bool read_ogg_embedded(const char *filepath, albumart_t *out) {
	ogg_art_t a = {.out = out, .found = false, .best_type = 0xFF};
	oggtags_walk(filepath, 0, NULL, ogg_art_comment, &a);
	return a.found;
}

// WavPack and Monkey's Audio: the raw image inside the APEv2 binary item, past
// the file name that starts it.
static bool read_apev2_embedded(const char *filepath, albumart_t *out) {
	int64_t at = 0;
	uint32_t size = 0;
	return apedec_cover_at(filepath, &at, &size) && take_file_range(out, filepath, (long long)at, size);
}

// ---------------------------------------------------------------------------
// Folder fallback: cover.jpg & friends sitting next to the music
// ---------------------------------------------------------------------------

// Preferred cover file names, best first. Matched case-insensitively, so
// "Folder.jpg" and "folder.JPG" both work.
static const char *const cover_basenames[] = {
	"cover", "folder", "front", "albumart", "albumartsmall", "album", "artwork", "thumb",
};

static bool is_image_name(const char *name) {
	return has_extension(name, ".jpg") || has_extension(name, ".jpeg") || has_extension(name, ".png");
}

// Ranks a file name against `cover_basenames`. Lower is better; -1 means the
// name isn't a recognised cover file at all.
static int cover_name_rank(const char *name) {
	if (!is_image_name(name))
		return -1;

	const char *dot = strrchr(name, '.');
	size_t stem_len = dot ? (size_t)(dot - name) : strlen(name);

	for (size_t i = 0; i < sizeof(cover_basenames) / sizeof(cover_basenames[0]); i++) {
		size_t len = strlen(cover_basenames[i]);
		if (stem_len == len && strncasecmp(name, cover_basenames[i], len) == 0)
			return (int)i;
	}
	return -1;
}

static bool read_whole_file(const char *path, albumart_t *out) {
	long size = get_file_size(path);
	if (size <= 0)
		return false;
	return take_file_range(out, path, 0, (size_t)size);
}

// Whether an image file is named after `stem` -- the same name, a different
// extension.
static bool image_named_after(const char *name, const char *stem) {
	if (!stem || !stem[0])
		return false;

	const char *dot = strrchr(name, '.');
	size_t stem_len = dot ? (size_t)(dot - name) : strlen(name);
	return stem_len == strlen(stem) && strncasecmp(name, stem, stem_len) == 0;
}

// Looks for a cover image inside `dirpath`. `audio_stem` (may be NULL) is the
// file name of the track without its extension, so "01 - Song.mp3" can pick up
// a matching "01 - Song.jpg".
//
// Ranked, best first: the track's own name, then the folder's own name (rippers
// commonly write "Artist - Album.jpg" beside the tracks), then the generic
// names, and last the single image of a folder that holds exactly one.
#define COVER_RANK_TRACK_NAME 0
#define COVER_RANK_DIR_NAME 1
#define COVER_RANK_GENERIC 2
#define COVER_RANK_NONE 1000

static bool find_cover_file(const char *dirpath, const char *audio_stem, char *out_path, size_t out_size) {
	DIR *dir = opendir(dirpath);
	if (!dir)
		return false;

	// The folder's own name, without its path.
	const char *dir_slash = strrchr(dirpath, '/');
	const char *dir_stem = dir_slash ? dir_slash + 1 : dirpath;

	char best[256] = {0};
	int best_rank = COVER_RANK_NONE;

	char only_image[256] = {0};
	int image_count = 0;

	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		if (!is_image_name(de->d_name))
			continue;

		image_count++;
		if (image_count == 1) {
			snprintf(only_image, sizeof(only_image), "%s", de->d_name);
		}

		int rank = COVER_RANK_NONE;
		if (image_named_after(de->d_name, audio_stem)) {
			rank = COVER_RANK_TRACK_NAME;
		} else if (image_named_after(de->d_name, dir_stem)) {
			rank = COVER_RANK_DIR_NAME;
		} else {
			int generic = cover_name_rank(de->d_name);
			if (generic >= 0) {
				rank = COVER_RANK_GENERIC + generic;
			}
		}

		if (rank < best_rank) {
			best_rank = rank;
			snprintf(best, sizeof(best), "%s", de->d_name);
			if (rank == COVER_RANK_TRACK_NAME) {
				break; // nothing can beat it
			}
		}
	}
	closedir(dir);

	// Nothing matched by name, but if the folder holds exactly one image it's
	// almost certainly the cover.
	if (best[0] == '\0' && image_count == 1) {
		snprintf(best, sizeof(best), "%s", only_image);
	}

	if (best[0] == '\0')
		return false;

	snprintf(out_path, out_size, "%s/%s", dirpath, best);
	return true;
}

// ---------------------------------------------------------------------------
// MP4 / M4A / M4B: the `covr` atom
// ---------------------------------------------------------------------------

static bool read_mp4_embedded(const char *filepath, albumart_t *out) {
	mp4_file_t *m = mp4_open(filepath);
	if (!m) {
		return false;
	}

	uint64_t offset = 0;
	uint32_t size = 0;
	bool is_png = false;
	bool found = mp4_cover_art(m, &offset, &size, &is_png);
	mp4_close(m);

	if (!found) {
		return false;
	}

	// Checked like every other format: a `covr` atom may hold a BMP, or the
	// atom may simply be mis-sized, and handing those bytes on as artwork
	// costs a failed decode and, worse, a cached "no cover here".
	return take_file_range(out, filepath, (long long)offset, size);
}

// Reads the picture stored inside the file's own tags, if the format has a
// place for one.
static bool read_embedded(const char *filepath, albumart_t *out) {
	switch (decode_detect_format(filepath)) {
	case DECODE_FORMAT_MP3:
	case DECODE_FORMAT_AAC_ADTS:
		// Same reason as the tags: an .aac file's cover sits in an ID3v2 APIC
		// frame, exactly as in an mp3.
		return read_mp3_embedded(filepath, out);
	case DECODE_FORMAT_FLAC:
		return read_flac_embedded(filepath, out);
	case DECODE_FORMAT_OGG_VORBIS:
	case DECODE_FORMAT_OPUS:
		return read_ogg_embedded(filepath, out);
	case DECODE_FORMAT_AAC_MP4:
	case DECODE_FORMAT_ALAC_MP4:
		// The cover lives in the container, not the audio stream, so the codec
		// is irrelevant. ALAC is listed explicitly because a file named .alac
		// is detected as its own format and would otherwise never be asked.
		return read_mp4_embedded(filepath, out);
	case DECODE_FORMAT_WAVPACK:
	case DECODE_FORMAT_APE:
		return read_apev2_embedded(filepath, out);
	case DECODE_FORMAT_DSD:
		return has_extension(filepath, ".dsf") && read_dsf_embedded(filepath, out);
	case DECODE_FORMAT_SNDFILE:
		// Of what libsndfile plays here, only AIFF has somewhere to put a
		// picture.
		return (has_extension(filepath, ".aif") || has_extension(filepath, ".aiff") || has_extension(filepath, ".aifc")) && read_iff_embedded(filepath, out);
	default:
		// A plain WAV is no decoder's format -- audio.c plays it itself -- but
		// it can carry the same ID3 chunk.
		return has_extension(filepath, ".wav") && read_iff_embedded(filepath, out);
	}
}

// Picks the alphabetically first track in the folder, so an album row always
// shows the same picture whatever order the filesystem returns entries in.
static bool find_first_track(const char *dirpath, char *out_name, size_t out_size) {
	DIR *dir = opendir(dirpath);
	if (!dir)
		return false;

	bool found = false;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		// Only the formats that can carry a picture. A plain WAV is detected as
		// no decoder's format (audio.c plays it itself) but carries one in its
		// ID3 chunk, see read_embedded().
		if (decode_detect_format(de->d_name) == DECODE_FORMAT_UNKNOWN && !has_extension(de->d_name, ".wav"))
			continue;

		if (!found || strcasecmp(de->d_name, out_name) < 0) {
			snprintf(out_name, out_size, "%s", de->d_name);
			found = true;
		}
	}

	closedir(dir);
	return found;
}

bool albumart_load_for_dir(const char *dirpath, albumart_t *out) {
	for (int i = 0; i < ALBUMART_CANDIDATES; i++) {
		if (albumart_load_dir_candidate(dirpath, i, out))
			return true;
	}
	return false;
}

bool albumart_load_dir_candidate(const char *dirpath, int index, albumart_t *out) {
	memset(out, 0, sizeof(*out));

	if (index == 0) {
		char cover_path[1300];
		return find_cover_file(dirpath, NULL, cover_path, sizeof(cover_path)) && read_whole_file(cover_path, out);
	}
	if (index != 1)
		return false;

	// An album folder with no cover file usually still carries the artwork
	// inside its tracks -- borrow it from the first one.
	char track_name[256] = {0};
	if (!find_first_track(dirpath, track_name, sizeof(track_name)))
		return false;

	char track_path[1300];
	snprintf(track_path, sizeof(track_path), "%s/%s", dirpath, track_name);
	return read_embedded(track_path, out);
}

bool albumart_load_for_file(const char *filepath, albumart_t *out) {
	for (int i = 0; i < ALBUMART_CANDIDATES; i++) {
		if (albumart_load_candidate(filepath, i, out))
			return true;
	}
	return false;
}

bool albumart_load_candidate(const char *filepath, int index, albumart_t *out) {
	memset(out, 0, sizeof(*out));

	if (index < 0 || index >= ALBUMART_CANDIDATES)
		return false;

	// A track of a CUE sheet has no artwork of its own: the picture belongs to
	// the file the sheet cuts up, or sits in the folder beside it.
	//
	// The sheet is on the heap, not the stack: cue_sheet_t is 35 KB, the
	// compiler would reserve it on entry whether or not this branch is taken,
	// and this function recurses -- 70 KB of loader-thread stack for a case
	// that fires only on a `.cue?track=` path.
	char sheet_path[512];
	if (cue_split_path(filepath, sheet_path, sizeof(sheet_path)) > 0) {
		cue_sheet_t *cue = malloc(sizeof(*cue));
		if (!cue) {
			return false;
		}
		bool ok = cue_parse(sheet_path, cue) && albumart_load_candidate(cue->audio_path, index, out);
		free(cue);
		return ok;
	}

	// The path IS a picture. There is no track here to read tags from and no
	// folder to search: a radio station's downloaded icon is simply the
	// artwork, and the player asks for it by path like any other cover.
	if (has_extension(filepath, ".jpg") || has_extension(filepath, ".jpeg") || has_extension(filepath, ".png")) {
		return index == 0 && read_whole_file(filepath, out);
	}

	if (index == 0) {
		return read_embedded(filepath, out);
	}

	// A cover image file sitting next to the track.
	char dirpath[1024];
	strncpy(dirpath, filepath, sizeof(dirpath) - 1);
	dirpath[sizeof(dirpath) - 1] = '\0';

	char *slash = strrchr(dirpath, '/');
	const char *filename = slash ? slash + 1 : dirpath;

	char stem[256] = {0};
	const char *dot = strrchr(filename, '.');
	size_t stem_len = dot ? (size_t)(dot - filename) : strlen(filename);
	if (stem_len < sizeof(stem)) {
		memcpy(stem, filename, stem_len);
		stem[stem_len] = '\0';
	}

	if (slash) {
		*slash = '\0';
	} else {
		strncpy(dirpath, ".", sizeof(dirpath) - 1);
	}
	if (dirpath[0] == '\0')
		strncpy(dirpath, "/", sizeof(dirpath) - 1);

	char cover_path[1300];
	if (find_cover_file(dirpath, stem, cover_path, sizeof(cover_path)))
		return read_whole_file(cover_path, out);

	return false;
}
