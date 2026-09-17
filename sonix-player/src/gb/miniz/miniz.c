// See miniz.h for what this file stands in for.

#include "miniz.h"

// The zlib CRC32, polynomial 0xEDB88320. The table is built on first use.
mz_ulong mz_crc32(mz_ulong crc, const unsigned char *ptr, size_t buf_len) {
	static unsigned long table[256];
	static int built;

	if (!built) {
		for (unsigned i = 0; i < 256; i++) {
			unsigned long c = i;
			for (int k = 0; k < 8; k++) {
				c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
			}
			table[i] = c;
		}
		built = 1;
	}

	if (!ptr) {
		return 0;
	}

	unsigned long c = (unsigned long)crc ^ 0xFFFFFFFFUL;
	for (size_t i = 0; i < buf_len; i++) {
		c = table[(c ^ ptr[i]) & 0xFF] ^ (c >> 8);
	}
	return (mz_ulong)((c ^ 0xFFFFFFFFUL) & 0xFFFFFFFFUL);
}

// --- the zip that is not there ----------------------------------------------
//
// Cartridge::LoadFromZipFile is reached only when the file name ends in .zip.

mz_bool mz_zip_reader_init_mem(mz_zip_archive *zip, const void *mem, size_t size, mz_uint flags) {
	(void)zip;
	(void)mem;
	(void)size;
	(void)flags;
	return 0;
}

mz_uint mz_zip_reader_get_num_files(mz_zip_archive *zip) {
	(void)zip;
	return 0;
}

mz_bool mz_zip_reader_file_stat(mz_zip_archive *zip, mz_uint index, mz_zip_archive_file_stat *stat) {
	(void)zip;
	(void)index;
	(void)stat;
	return 0;
}

void *mz_zip_reader_extract_file_to_heap(mz_zip_archive *zip, const char *name, size_t *size, mz_uint flags) {
	(void)zip;
	(void)name;
	(void)flags;
	if (size) {
		*size = 0;
	}
	return 0;
}

mz_bool mz_zip_reader_end(mz_zip_archive *zip) {
	(void)zip;
	return 1;
}
