#ifndef OGGTAGS_H
#define OGGTAGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The comment header of an Ogg Vorbis or Ogg Opus file, read straight from the
// pages: "KEY=value" comments, the same in both. A comment not worth reading
// -- an embedded picture, base64 inside the comment -- is skipped with a seek
// per page and never held in memory.

typedef struct oggtags_value oggtags_value_t;

// Walks the comments of the first Vorbis or Opus stream in the file.
//
// A comment of at most `max_text` bytes goes to `small`, whole and
// NUL-terminated. A longer one goes to `large` with its key (not terminated)
// and the length of its value, which `large` may read through `value`; what it
// leaves unread is skipped. A NULL callback skips its comments. Either callback
// returns false to end the walk. False when the file has no such stream.
bool oggtags_walk(const char *path, size_t max_text, bool (*small)(void *user, const char *comment, size_t len),
				  bool (*large)(void *user, const char *key, size_t key_len, uint32_t value_len,
								oggtags_value_t *value),
				  void *user);

// Whether a comment key names an embedded picture: METADATA_BLOCK_PICTURE, or
// the older COVERART.
bool oggtags_key_is_picture(const char *key, size_t key_len);

// Reads up to `n` more bytes of the value handed to `large`. Returns how many
// were read: fewer than asked at the end of the value or on a damaged file.
size_t oggtags_value_read(oggtags_value_t *value, void *buf, size_t n);

#endif
