// Single-translation-unit implementations for the stb_* header-only
// libraries that the rest of the Butterscotch runtime references.
//
// stb_ds:    used by the runtime in many places (hashmaps, dyn arrays).
// stb_image: used by gles1_renderer.c to decode the TXTR PNG blobs we
//            stream out of data.win on demand and upload via
//            glTexImage2D. Only the PNG decoder is needed (sprites and
//            backgrounds), so we drop the other formats to keep the
//            armv6 binary small.

#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO         // we feed it byte buffers, never FILE*
#define STBI_NO_THREAD_LOCALS // iOS 3 has no thread-locals at this ABI
#define STBI_NO_JPEG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"
