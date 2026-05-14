// Single-translation-unit implementations for the stb_* header-only
// libraries that the rest of the Butterscotch runtime references.
//
// We deliberately only enable stb_ds here — the iOS port at this
// milestone does not load PNGs (image_decoder.c is not linked, the
// renderer is mostly a no-op) and does not write images. If/when we
// hook up the GL texture loader, add STB_IMAGE_IMPLEMENTATION here.

#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"
