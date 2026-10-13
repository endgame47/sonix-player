#ifndef STB_IMAGE_DECL_H
#define STB_IMAGE_DECL_H

/*
 * Declaration-only view of stb_image, mirroring how stb_vorbis_decl.h is used
 * for the audio decoder: the implementation lives in exactly one translation
 * unit (stb_image_impl.c) and everyone else includes this.
 *
 * The STBI_ONLY_* / STBI_NO_* defines must match stb_image_impl.c so the
 * prototypes line up.
 */

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
// A radio station's icon is a GIF often enough that leaving GIF out leaves
// those stations without one. The first frame is what is drawn.
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR

#include "src/system/image/stb_image.h"

#endif // STB_IMAGE_DECL_H
