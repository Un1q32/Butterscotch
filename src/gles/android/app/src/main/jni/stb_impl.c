// Single-translation-unit implementations for the stb_* header-only
// libraries the Butterscotch runtime references. Mirrors
// src/gles/ios/stb_impl.c — only the PNG decoder is needed (TXTR blobs
// are PNG), so we drop the other image formats to keep the .so small.

#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO         // we feed byte buffers, never FILE*
#define STBI_NO_JPEG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb_image.h"
