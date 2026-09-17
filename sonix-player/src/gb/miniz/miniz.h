#ifndef GB_MINIZ_SHIM_H
#define GB_MINIZ_SHIM_H

// The part of miniz Gearboy reaches for, and nothing else.
//
// Gearboy calls miniz for two things: the CRC32 it identifies the M161 and
// MultiMBC1 cartridges with, and reading a ROM out of a .zip. Only the CRC32
// is real here; the zip reader always answers "not a zip".
//
// Real miniz cannot be linked beside src/system/image/miniz, the inflate
// subset that decodes cover-art PNGs: the two export the same symbols, and the
// link stops at "multiple definition of tinfl_decompressor_free".

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int mz_bool;
typedef unsigned int mz_uint;
typedef unsigned long mz_ulong;

#define MZ_CRC32_INIT (0)

// The real zlib CRC32: Gearboy matches the result against known cartridge
// sums, so an approximation picks the wrong mapper on those two.
mz_ulong mz_crc32(mz_ulong crc, const unsigned char *ptr, size_t buf_len);

// --- the zip reader, which reads nothing ----------------------------------

typedef struct {
	void *unused;
} mz_zip_archive;

typedef struct {
	mz_uint m_file_index;
	unsigned long long m_comp_size;
	unsigned long long m_uncomp_size;
	char m_filename[260];
	char m_comment[256];
} mz_zip_archive_file_stat;

mz_bool mz_zip_reader_init_mem(mz_zip_archive *zip, const void *mem, size_t size, mz_uint flags);
mz_uint mz_zip_reader_get_num_files(mz_zip_archive *zip);
mz_bool mz_zip_reader_file_stat(mz_zip_archive *zip, mz_uint index, mz_zip_archive_file_stat *stat);
void *mz_zip_reader_extract_file_to_heap(mz_zip_archive *zip, const char *name, size_t *size, mz_uint flags);
mz_bool mz_zip_reader_end(mz_zip_archive *zip);

#ifdef __cplusplus
}
#endif

#endif /* GB_MINIZ_SHIM_H */
