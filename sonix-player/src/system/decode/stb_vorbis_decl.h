#ifndef STB_VORBIS_DECL_H
#define STB_VORBIS_DECL_H

#include <stdio.h>

// Hand-written declarations for the subset of stb_vorbis.c's public API used by
// decode.c and flappysound.c. stb_vorbis.c is compiled as its own translation
// unit (picked up by the project Makefile's `find src -name '*.c'`), so it
// cannot also be #included here as text without causing duplicate symbols.
// These declarations must stay binary-compatible with the real ones near the
// top of stb_vorbis.c.

typedef struct stb_vorbis stb_vorbis;

typedef struct {
	char *alloc_buffer;
	int alloc_buffer_length_in_bytes;
} stb_vorbis_alloc;

typedef struct {
	unsigned int sample_rate;
	int channels;

	unsigned int setup_memory_required;
	unsigned int setup_temp_memory_required;
	unsigned int temp_memory_required;

	int max_frame_size;
} stb_vorbis_info;

typedef struct {
	char *vendor;

	int comment_list_length;
	char **comment_list;
} stb_vorbis_comment;

extern stb_vorbis_info stb_vorbis_get_info(stb_vorbis *f);
extern stb_vorbis_comment stb_vorbis_get_comment(stb_vorbis *f);
extern void stb_vorbis_close(stb_vorbis *f);
extern stb_vorbis *stb_vorbis_open_filename(const char *filename, int *error, const stb_vorbis_alloc *alloc_buffer);
// The stream from the current position of `file` to its end. With
// close_on_free the file is closed by stb_vorbis_close, or before returning
// NULL.
extern stb_vorbis *stb_vorbis_open_file(FILE *file, int close_on_free, int *error, const stb_vorbis_alloc *alloc_buffer);
extern unsigned int stb_vorbis_stream_length_in_samples(stb_vorbis *f);
extern int stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels, short *buffer, int num_shorts);
extern int stb_vorbis_seek(stb_vorbis *f, unsigned int sample_number);
// The whole file at once: returns frames, fills interleaved samples to free().
extern int stb_vorbis_decode_filename(const char *filename, int *channels, int *sample_rate, short **output);

#endif // STB_VORBIS_DECL_H
