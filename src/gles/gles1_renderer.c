#include "gles1_renderer.h"
#include "../utils.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdbool.h>

// Platform GLES 1.1 headers. iOS uses <OpenGLES/ES1/gl.h>, Android NDK
// uses <GLES/gl.h>. Pick based on what the platform main.* defines.
#if defined(__APPLE__)
    #include <OpenGLES/ES1/gl.h>
    #include <OpenGLES/ES1/glext.h>
#else
    #include <GLES/gl.h>
    #include <GLES/glext.h>
#endif

#include "stb_image.h"

// Renderer.h already pulls in data_win.h. text_utils.h is header-only
// inlines for glyph lookup / UTF-8 decode / line metrics used by drawText.
#include "../text_utils.h"

// ============================================================================
// Helpers
// ============================================================================

static inline void unpackBGR(uint32_t bgr, float a, GLfloat out[4]) {
    out[0] = ((bgr      ) & 0xFF) / 255.0f;
    out[1] = ((bgr >>  8) & 0xFF) / 255.0f;
    out[2] = ((bgr >> 16) & 0xFF) / 255.0f;
    out[3] = a;
}

// ============================================================================
// GLES1Renderer — extends the base Renderer struct with our
// data-streaming texture cache. The vtable functions receive Renderer*
// but we always allocate the larger struct so a downcast is safe.
// ============================================================================

// Cache slot for one TXTR entry. We map TXTR index -> up to BS_MAX_BANDS
// GL texture objects, splitting the atlas vertically into 1024-row
// bands so PowerVR MBX Lite's 1024x1024 GL_MAX_TEXTURE_SIZE doesn't
// force us to downsample the source pixels. UNDERTALE ships atlases
// at 1024x2048 (2 bands); other GMS:Studio games may ship 1024x1024
// (1 band) or up to 1024x4096 (4 bands).
//
// width  = original atlas width in source pixels.
// height = original atlas height in source pixels.
// bandHeight = uniform per-band height (= maxTextureSize); the last
//              band may be partially-populated if height isn't a multiple.
// numBands = ceil(height / bandHeight).
//
// Sprite / glyph rectangles index into this slot using the original
// (sourceX, sourceY) pixel coords. The renderer code does:
//   band     = sourceY / bandHeight
//   localY   = sourceY - band*bandHeight
//   v        = localY / bandHeight
// to pick the right band texture and remap V into [0,1].
#define BS_MAX_BANDS 4

typedef struct {
    GLuint glHandles[BS_MAX_BANDS];  // 0 entries = not uploaded
    int32_t width;                   // original atlas width  (source pixels)
    int32_t height;                  // original atlas height (source pixels)
    int32_t bandHeight;              // per-band texture height
    int32_t numBands;
    uint32_t bytesUploaded;          // sum of band texture sizes in bytes
    uint32_t lastUsedTick;
} BSTexCacheSlot;

typedef struct {
    Renderer base; // MUST be first — vtable functions cast Renderer* -> GLES1Renderer*
    FILE* dataWinFile;          // owned, fopen'd from dataWinPath; falls back to dataWin->lazyLoadFile if path was not set
    char* dataWinPath;          // strdup, owned
    bool ownsFile;              // true if we fopen'd it, false if we borrowed from lazyLoadFile
    BSTexCacheSlot* slots;      // count = dataWin->txtr.count
    uint32_t slotCount;
    uint32_t frameTick;
    uint32_t residentBytes;
    uint32_t residentBudget;
    uint32_t logBudget;         // diagnostic prints we still owe; decremented as we log
    int32_t maxTextureSize;     // GL_MAX_TEXTURE_SIZE (1024 on MBX Lite, 2048/4096 on later iOS)
} GLES1Renderer;

static inline GLES1Renderer* asGLES1(Renderer* r) {
    return (GLES1Renderer*) r;
}

// Forward decls.
static BSTexCacheSlot* gles1_ensureSlot(GLES1Renderer* g, int32_t txtrIndex);

// Pick which band a given source-Y row belongs to. Clamps to the
// valid range so out-of-bounds lookups don't index past the array.
static inline int32_t gles1_bandOfRow(const BSTexCacheSlot* slot, int32_t srcY) {
    if (slot->bandHeight <= 0) return 0;
    int32_t b = srcY / slot->bandHeight;
    if (b < 0) return 0;
    if (b >= slot->numBands) return slot->numBands - 1;
    return b;
}

// ============================================================================
// Renderer vtable: lifecycle
// ============================================================================

static void gles1_init(Renderer* r, DataWin* dataWin) {
    GLES1Renderer* g = asGLES1(r);
    r->dataWin = dataWin;
    r->drawColor = 0xFFFFFF;
    r->drawAlpha = 1.0f;
    r->drawFont = -1;
    r->drawHalign = 0;
    r->drawValign = 0;
    r->circlePrecision = 24;

    // Prefer our own FILE* (set via GLES1Renderer_setDataWinPath) so we
    // don't race with the parser's room-payload lazy loader on a shared
    // file position. Fall back to dataWin->lazyLoadFile if the embedder
    // didn't tell us a path.
    if (g->dataWinPath != NULL) {
        g->dataWinFile = fopen(g->dataWinPath, "rb");
        g->ownsFile = (g->dataWinFile != NULL);
        if (g->dataWinFile == NULL) {
            fprintf(stderr, "[gles1] WARN: fopen('%s') failed (errno keeps you guessing on iOS 3); will fall back to lazyLoadFile\n", g->dataWinPath);
        }
    }
    if (g->dataWinFile == NULL && dataWin->lazyLoadFile != NULL) {
        g->dataWinFile = dataWin->lazyLoadFile;
        g->ownsFile = false;
    }
    if (g->dataWinFile == NULL) {
        fprintf(stderr, "[gles1] WARN: no data.win FILE* for texture streaming — sprites will be white\n");
    } else {
        fprintf(stderr, "[gles1] data.win FILE* ready (owned=%d), TXTR count=%u\n",
                (int) g->ownsFile, (unsigned) dataWin->txtr.count);
    }

    // Allocate one cache slot per TXTR entry.
    if (dataWin->txtr.count > 0) {
        g->slotCount = dataWin->txtr.count;
        g->slots = (BSTexCacheSlot*) calloc(g->slotCount, sizeof(BSTexCacheSlot));
    }
    g->logBudget = 8; // first 8 ensureTexture calls get fully logged

    g->frameTick = 0;
    g->residentBytes = 0;
    // ~12 MB conservative VRAM budget for MBX Lite (16 MB shared with FB).
    // 12 MB is enough for 3 RGBA 1024x1024 atlases, which covers a typical
    // Undertale frame (1 sprite atlas + 1 bg atlas + 1 font atlas).
    g->residentBudget = 12u * 1024u * 1024u;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);

    // Find out what the GPU actually accepts. PowerVR MBX Lite caps at
    // 1024x1024 even though GMS:Studio ships atlases up to 2048x2048
    // (and 4096 on later devices). Anything larger silently rejects
    // from glTexImage2D with GL_INVALID_VALUE — we have to downsample.
    GLint maxTex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
    if (maxTex <= 0) maxTex = 1024; // sensible MBX Lite default if the query fails
    g->maxTextureSize = maxTex;
    fprintf(stderr, "[gles1] GL_MAX_TEXTURE_SIZE = %d\n", (int) maxTex);
    const GLubyte* vendor = glGetString(GL_VENDOR);
    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* version = glGetString(GL_VERSION);
    fprintf(stderr, "[gles1] GL_VENDOR=%s RENDERER=%s VERSION=%s\n",
            vendor ? (const char*) vendor : "(null)",
            renderer ? (const char*) renderer : "(null)",
            version ? (const char*) version : "(null)");
}

// Downsample an RGBA8 buffer in place by a power-of-two factor in each
// axis. Used to fit Undertale's 1024x2048 atlases onto MBX Lite's
// 1024x1024 cap. Box-filter average of factorX*factorY source pixels
// into one destination pixel. Mutates *pPixels (frees old, allocates new)
// and updates *pW / *pH.
static bool gles1_downsampleRGBA(stbi_uc** pPixels, int* pW, int* pH, int factorX, int factorY) {
    if (factorX < 1 || factorY < 1) return false;
    if (factorX == 1 && factorY == 1) return true;
    int srcW = *pW;
    int srcH = *pH;
    int dstW = srcW / factorX;
    int dstH = srcH / factorY;
    if (dstW <= 0 || dstH <= 0) return false;
    stbi_uc* dst = (stbi_uc*) malloc((size_t) dstW * dstH * 4);
    if (dst == NULL) return false;
    stbi_uc* src = *pPixels;
    int blockArea = factorX * factorY;
    for (int y = 0; y < dstH; y++) {
        for (int x = 0; x < dstW; x++) {
            int sumR = 0, sumG = 0, sumB = 0, sumA = 0;
            int srcY0 = y * factorY;
            int srcX0 = x * factorX;
            for (int dy = 0; dy < factorY; dy++) {
                const stbi_uc* row = src + ((srcY0 + dy) * srcW + srcX0) * 4;
                for (int dx = 0; dx < factorX; dx++) {
                    sumR += row[0]; sumG += row[1]; sumB += row[2]; sumA += row[3];
                    row += 4;
                }
            }
            stbi_uc* outPx = dst + (y * dstW + x) * 4;
            outPx[0] = (stbi_uc)(sumR / blockArea);
            outPx[1] = (stbi_uc)(sumG / blockArea);
            outPx[2] = (stbi_uc)(sumB / blockArea);
            outPx[3] = (stbi_uc)(sumA / blockArea);
        }
    }
    stbi_image_free(src);
    *pPixels = dst;
    *pW = dstW;
    *pH = dstH;
    return true;
}

static void gles1_freeSlotGL(BSTexCacheSlot* s) {
    for (int b = 0; b < BS_MAX_BANDS; b++) {
        if (s->glHandles[b] != 0) {
            glDeleteTextures(1, &s->glHandles[b]);
            s->glHandles[b] = 0;
        }
    }
    s->bytesUploaded = 0;
    s->lastUsedTick = 0;
}

static void gles1_destroy(Renderer* r) {
    GLES1Renderer* g = asGLES1(r);
    if (g->slots != NULL) {
        for (uint32_t i = 0; i < g->slotCount; i++) {
            gles1_freeSlotGL(&g->slots[i]);
        }
        free(g->slots);
        g->slots = NULL;
    }
    if (g->ownsFile && g->dataWinFile != NULL) {
        fclose(g->dataWinFile);
        g->dataWinFile = NULL;
    }
    if (g->dataWinPath != NULL) {
        free(g->dataWinPath);
        g->dataWinPath = NULL;
    }
}

// ============================================================================
// Texture streaming — read PNG blob from data.win lazily, decode via
// stb_image, upload to a GL texture. Cached for subsequent frames.
// LRU eviction kicks in when residentBytes > residentBudget.
// ============================================================================

// Is this slot resident in VRAM right now?
static inline bool gles1_slotIsResident(const BSTexCacheSlot* s) {
    for (int b = 0; b < BS_MAX_BANDS; b++) {
        if (s->glHandles[b] != 0) return true;
    }
    return false;
}

static void gles1_evictLRU(GLES1Renderer* g, uint32_t bytesToFree) {
    while (g->residentBytes + bytesToFree > g->residentBudget) {
        uint32_t victimIdx = UINT32_MAX;
        uint32_t victimTick = UINT32_MAX;
        for (uint32_t i = 0; i < g->slotCount; i++) {
            BSTexCacheSlot* s = &g->slots[i];
            if (!gles1_slotIsResident(s)) continue;
            if (s->lastUsedTick == g->frameTick) continue;
            if (s->lastUsedTick < victimTick) {
                victimTick = s->lastUsedTick;
                victimIdx = i;
            }
        }
        if (victimIdx == UINT32_MAX) break;

        BSTexCacheSlot* v = &g->slots[victimIdx];
        uint32_t bytes = v->bytesUploaded;
        gles1_freeSlotGL(v);
        if (g->residentBytes >= bytes) g->residentBytes -= bytes;
        else g->residentBytes = 0;
    }
}

// Internal: decode the TXTR blob and upload all bands. Mutates *slot.
// On failure leaves slot in non-resident state and returns false.
static bool gles1_decodeAndUploadSlot(GLES1Renderer* g, int32_t txtrIndex, BSTexCacheSlot* slot) {
    if (g->dataWinFile == NULL) {
        if (g->logBudget > 0) {
            fprintf(stderr, "[gles1] ensureSlot[%d]: dataWinFile is NULL\n", txtrIndex);
            g->logBudget--;
        }
        return false;
    }

    Texture* tex = &g->base.dataWin->txtr.textures[txtrIndex];
    if (g->logBudget > 0) {
        fprintf(stderr, "[gles1] ensureSlot[%d]: blobOffset=%u blobSize=%u\n",
                txtrIndex, (unsigned) tex->blobOffset, (unsigned) tex->blobSize);
    }
    if (tex->blobOffset == 0 || tex->blobSize == 0) {
        if (g->logBudget > 0) {
            fprintf(stderr, "[gles1] ensureSlot[%d]: blob coordinates are zero — external texture?\n", txtrIndex);
            g->logBudget--;
        }
        return false;
    }

    uint8_t* compressed = (uint8_t*) malloc(tex->blobSize);
    if (compressed == NULL) {
        if (g->logBudget > 0) {
            fprintf(stderr, "[gles1] ensureSlot[%d]: malloc(%u) failed\n",
                    txtrIndex, (unsigned) tex->blobSize);
            g->logBudget--;
        }
        return false;
    }
    if (fseek(g->dataWinFile, (long) tex->blobOffset, SEEK_SET) != 0) {
        if (g->logBudget > 0) {
            fprintf(stderr, "[gles1] ensureSlot[%d]: fseek(%u) failed\n",
                    txtrIndex, (unsigned) tex->blobOffset);
            g->logBudget--;
        }
        free(compressed);
        return false;
    }
    size_t got = fread(compressed, 1, tex->blobSize, g->dataWinFile);
    if (got != tex->blobSize) {
        if (g->logBudget > 0) {
            fprintf(stderr, "[gles1] ensureSlot[%d]: fread short: got %zu, wanted %u\n",
                    txtrIndex, got, (unsigned) tex->blobSize);
            g->logBudget--;
        }
        free(compressed);
        return false;
    }
    int w = 0, h = 0, ch = 0;
    stbi_uc* pixels = stbi_load_from_memory(compressed, (int) tex->blobSize, &w, &h, &ch, 4);
    free(compressed);
    if (pixels == NULL || w <= 0 || h <= 0) {
        if (g->logBudget > 0) {
            fprintf(stderr, "[gles1] ensureSlot[%d]: stbi decode failed: %s (w=%d h=%d)\n",
                    txtrIndex, stbi_failure_reason() ? stbi_failure_reason() : "(no reason)", w, h);
            g->logBudget--;
        }
        return false;
    }
    int origW = w, origH = h;
    if (g->logBudget > 0) {
        fprintf(stderr, "[gles1] ensureSlot[%d]: decoded %dx%d (ch=%d)\n", txtrIndex, w, h, ch);
    }

    // Width handling: if the atlas is wider than GL_MAX_TEXTURE_SIZE
    // (rare — Undertale is 1024 wide and MBX Lite caps at 1024) we
    // fall back to box-filter downsampling on the X axis only, so
    // banding can still handle the Y axis at full resolution.
    int factorX = 1;
    while (w / factorX > g->maxTextureSize) factorX *= 2;
    if (factorX != 1) {
        if (!gles1_downsampleRGBA(&pixels, &w, &h, factorX, 1)) {
            fprintf(stderr, "[gles1] ensureSlot[%d]: X-downsample %dx failed; dropping atlas\n",
                    txtrIndex, factorX);
            stbi_image_free(pixels);
            return false;
        }
        if (g->logBudget > 0) {
            fprintf(stderr, "[gles1] ensureSlot[%d]: X-downsampled %d -> %d (factor %d)\n",
                    txtrIndex, origW, w, factorX);
        }
    }

    // Choose band height. Standard case: bands of maxTextureSize rows.
    int32_t bandH = g->maxTextureSize;
    int32_t numBands = (h + bandH - 1) / bandH;
    if (numBands > BS_MAX_BANDS) {
        // Atlas way too tall — fall back to Y-downsampling.
        int factorY = 1;
        while ((h + factorY - 1) / factorY > BS_MAX_BANDS * g->maxTextureSize) factorY *= 2;
        if (factorY > 1) {
            if (!gles1_downsampleRGBA(&pixels, &w, &h, 1, factorY)) {
                fprintf(stderr, "[gles1] ensureSlot[%d]: Y-downsample %dx failed; dropping atlas\n",
                        txtrIndex, factorY);
                stbi_image_free(pixels);
                return false;
            }
            if (g->logBudget > 0) {
                fprintf(stderr, "[gles1] ensureSlot[%d]: Y-downsampled %d -> %d (factor %d) — atlas exceeded %d-band cap\n",
                        txtrIndex, origH, h, factorY, BS_MAX_BANDS);
            }
        }
        numBands = (h + bandH - 1) / bandH;
    }

    // Upload one texture per band. The slot's UV math will pick the
    // right band based on the source-Y coord of each drawn rect.
    uint32_t totalBytes = 0;
    int32_t lastBandH = bandH;
    for (int32_t b = 0; b < numBands; b++) {
        int32_t rowStart = b * bandH;
        int32_t rowEnd   = rowStart + bandH;
        if (rowEnd > h) rowEnd = h;
        int32_t thisBandH = rowEnd - rowStart;
        if (thisBandH <= 0) { numBands = b; break; }
        // We always allocate a power-of-two-friendly bandH; the last
        // band's unused tail rows stay uninitialized (and aren't
        // sampled because TPAG rects don't go past the atlas height).
        // We still upload at full bandH to keep UV math uniform.
        uint32_t bandBytes = (uint32_t)(w * bandH * 4);
        if (g->residentBytes + totalBytes + bandBytes > g->residentBudget) {
            gles1_evictLRU(g, totalBytes + bandBytes);
        }
        GLuint handle = 0;
        glGenTextures(1, &handle);
        glBindTexture(GL_TEXTURE_2D, handle);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (thisBandH == bandH) {
            // Whole band — upload directly.
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, bandH, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         pixels + (size_t) rowStart * w * 4);
        } else {
            // Short band (atlas height not multiple of bandH). Upload
            // a full bandH texture with the source rows + black tail.
            uint8_t* padded = (uint8_t*) calloc((size_t) w * bandH * 4, 1);
            if (padded != NULL) {
                memcpy(padded, pixels + (size_t) rowStart * w * 4, (size_t) w * thisBandH * 4);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, bandH, 0, GL_RGBA, GL_UNSIGNED_BYTE, padded);
                free(padded);
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, bandH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            }
        }
        GLenum glErr = glGetError();
        if (glErr != GL_NO_ERROR) {
            fprintf(stderr, "[gles1] ensureSlot[%d] band %d: glTexImage2D failed 0x%04x (size %dx%d)\n",
                    txtrIndex, (int) b, (unsigned) glErr, w, bandH);
            glDeleteTextures(1, &handle);
            // Free already-uploaded bands.
            for (int32_t bb = 0; bb < b; bb++) {
                glDeleteTextures(1, &slot->glHandles[bb]);
                slot->glHandles[bb] = 0;
            }
            stbi_image_free(pixels);
            return false;
        }
        slot->glHandles[b] = handle;
        totalBytes += bandBytes;
        lastBandH = thisBandH;
    }
    stbi_image_free(pixels);

    slot->width = origW;
    slot->height = origH;
    slot->bandHeight = bandH;
    slot->numBands = numBands;
    slot->bytesUploaded = totalBytes;
    slot->lastUsedTick = g->frameTick;
    g->residentBytes += totalBytes;

    if (g->logBudget > 0) {
        fprintf(stderr, "[gles1] ensureSlot[%d]: uploaded %dx%d in %d band(s) of %d rows (last=%d), %u KB\n",
                txtrIndex, w, h, (int) numBands, (int) bandH, (int) lastBandH,
                (unsigned)(totalBytes / 1024));
        g->logBudget--;
    }
    return true;
}

static BSTexCacheSlot* gles1_ensureSlot(GLES1Renderer* g, int32_t txtrIndex) {
    if (txtrIndex < 0 || (uint32_t) txtrIndex >= g->slotCount) {
        if (g->logBudget > 0) {
            fprintf(stderr, "[gles1] ensureSlot: index %d out of range [0, %u)\n",
                    txtrIndex, g->slotCount);
            g->logBudget--;
        }
        return NULL;
    }
    BSTexCacheSlot* slot = &g->slots[txtrIndex];
    slot->lastUsedTick = g->frameTick;
    if (gles1_slotIsResident(slot)) return slot;
    if (!gles1_decodeAndUploadSlot(g, txtrIndex, slot)) return NULL;
    return slot;
}

// Bake the quad geometry for a sprite/tpag draw into local vertex
// + texcoord arrays. Used by drawSprite and drawSpritePart.
static void gles1_buildSpriteQuad(
        float dstX, float dstY, float dstW, float dstH,
        float originX, float originY, float xscale, float yscale, float angleDeg,
        float u0, float v0, float u1, float v1,
        GLfloat out_verts[8], GLfloat out_uvs[8]) {
    float c = cosf(angleDeg * 0.01745329252f);
    float s = sinf(angleDeg * 0.01745329252f);
    // Local corners relative to the pivot.
    float lx[4] = { -originX,            dstW - originX,       -originX,             dstW - originX };
    float ly[4] = { -originY,            -originY,             dstH - originY,       dstH - originY };
    for (int i = 0; i < 4; i++) {
        float x = lx[i] * xscale;
        float y = ly[i] * yscale;
        out_verts[i*2+0] = dstX + (x * c - y * s);
        out_verts[i*2+1] = dstY + (x * s + y * c);
    }
    out_uvs[0] = u0; out_uvs[1] = v0;
    out_uvs[2] = u1; out_uvs[3] = v0;
    out_uvs[4] = u0; out_uvs[5] = v1;
    out_uvs[6] = u1; out_uvs[7] = v1;
}

// Submit one textured quad. Used by both the band-spanning and
// single-band paths below. Local helper — caller fills in dstRect
// + UVs.
static void gles1_emitQuad(GLuint handle,
                           float dstX, float dstY, float dstW, float dstH,
                           float originX, float originY,
                           float xscale, float yscale, float angleDeg,
                           float u0, float v0, float u1, float v1,
                           const GLfloat rgba[4]) {
    GLfloat verts[8];
    GLfloat uvs[8];
    gles1_buildSpriteQuad(
        dstX, dstY, dstW, dstH,
        originX, originY,
        xscale, yscale, angleDeg,
        u0, v0, u1, v1,
        verts, uvs);

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, handle);
    glColor4f(rgba[0], rgba[1], rgba[2], rgba[3]);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glTexCoordPointer(2, GL_FLOAT, 0, uvs);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
}

static void gles1_drawTpagQuad(GLES1Renderer* g, int32_t tpagIndex,
                               float dstX, float dstY,
                               float originX, float originY,
                               float xscale, float yscale, float angleDeg,
                               // optional sub-rect clipping inside the tpag (or -1 to use full):
                               int32_t subX, int32_t subY, int32_t subW, int32_t subH,
                               uint32_t color, float alpha) {
    DataWin* dw = g->base.dataWin;
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dw->tpag.count) return;
    TexturePageItem* it = &dw->tpag.items[tpagIndex];

    BSTexCacheSlot* slot = gles1_ensureSlot(g, it->texturePageId);
    if (slot == NULL || slot->bandHeight <= 0) return;

    // Determine source rectangle within the atlas (in original pixels).
    int32_t srcX = it->sourceX + (subX >= 0 ? subX : 0);
    int32_t srcY = it->sourceY + (subY >= 0 ? subY : 0);
    int32_t srcW = (subW > 0) ? subW : it->sourceWidth;
    int32_t srcH = (subH > 0) ? subH : it->sourceHeight;

    if (srcW <= 0 || srcH <= 0) return;

    // targetX/Y/W/H: where this sub-image sits inside the sprite's
    // bounding box, after the atlas has trimmed away transparent edges.
    float ofsX = (float) it->targetX;
    float ofsY = (float) it->targetY;

    GLfloat rgba[4];
    unpackBGR(color, alpha, rgba);

    int32_t bandH = slot->bandHeight;
    int32_t firstBand = gles1_bandOfRow(slot, srcY);
    int32_t lastBand  = gles1_bandOfRow(slot, srcY + srcH - 1);
    float uScale = (float) slot->width;
    float u0 = (float) srcX / uScale;
    float u1 = (float)(srcX + srcW) / uScale;
    float fullDstW = (float) srcW;

    // Fast path: sub-rect lives entirely in one band (true for almost
    // every sprite + every glyph).
    if (firstBand == lastBand) {
        int32_t bandTop = firstBand * bandH;
        float v0 = (float)(srcY - bandTop) / (float) bandH;
        float v1 = (float)(srcY + srcH - bandTop) / (float) bandH;
        gles1_emitQuad(slot->glHandles[firstBand],
                       dstX, dstY, fullDstW, (float) srcH,
                       originX - ofsX, originY - ofsY,
                       xscale, yscale, angleDeg,
                       u0, v0, u1, v1,
                       rgba);
        return;
    }

    // Slow path: rect spans 2 or more bands. Emit one quad per band
    // with the dest height proportionally split. Rotation around this
    // path is unusual (rotated sprites are typically small), so we
    // just scale Y-segments by the modelview — the rotation is applied
    // in buildSpriteQuad to the whole "dst" rect of each segment, not
    // to the originally-intended single rect; for axis-aligned draws
    // (angleDeg=0) this produces a pixel-identical result.
    int32_t totalSrcH = srcH;
    float dstStartY = dstY;
    int32_t srcCursor = srcY;
    int32_t srcRemaining = srcH;
    float originY_adj = originY - ofsY;
    for (int32_t b = firstBand; b <= lastBand && srcRemaining > 0; b++) {
        int32_t bandTop = b * bandH;
        int32_t bandBottom = bandTop + bandH;
        int32_t segStart = srcCursor;
        int32_t segEnd   = (srcCursor + srcRemaining < bandBottom) ? srcCursor + srcRemaining : bandBottom;
        int32_t segH = segEnd - segStart;
        if (segH <= 0) continue;
        float v0 = (float)(segStart - bandTop) / (float) bandH;
        float v1 = (float)(segEnd   - bandTop) / (float) bandH;
        float segOriginY = originY_adj - (float)(segStart - srcY);
        gles1_emitQuad(slot->glHandles[b],
                       dstX, dstStartY, fullDstW, (float) segH,
                       originX - ofsX, segOriginY,
                       xscale, yscale, angleDeg,
                       u0, v0, u1, v1,
                       rgba);
        dstStartY += segH * yscale;
        srcCursor += segH;
        srcRemaining -= segH;
    }
    (void) totalSrcH;
}


// ============================================================================
// Renderer vtable: frame + view + GUI passes
// ============================================================================

static void gles1_beginFrame(Renderer* r, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    GLES1Renderer* g = asGLES1(r);
    g->frameTick++;
    glViewport(0, 0, windowW, windowH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    // We draw in game-space coordinates (0,0) at top-left, (gameW,gameH)
    // at bottom-right. The renderbuffer is sized to the physical screen
    // (e.g. 320x480 on iPod Touch 2G); ortho stretches the game's
    // virtual canvas to fill it.
    if (gameW <= 0) gameW = windowW;
    if (gameH <= 0) gameH = windowH;
    glOrthof(0.0f, (GLfloat) gameW, (GLfloat) gameH, 0.0f, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void gles1_endFrame(Renderer* r) {
    (void) r;
    // The EAGLContext owner (UIView on iOS) is responsible for
    // presenting the renderbuffer. Just flush here.
    glFlush();
}

static void gles1_beginView(Renderer* r, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH,
                            int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    (void) r; (void) viewX; (void) viewY; (void) viewW; (void) viewH;
    (void) portX; (void) portY; (void) portW; (void) portH; (void) viewAngle;
}

static void gles1_endView(Renderer* r) { (void) r; }

static void gles1_beginGUI(Renderer* r, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY, int32_t portW, int32_t portH) {
    (void) r; (void) guiW; (void) guiH; (void) portX; (void) portY; (void) portW; (void) portH;
}

static void gles1_endGUI(Renderer* r) { (void) r; }

// ============================================================================
// Renderer vtable: clear + flush
// ============================================================================

static void gles1_clearScreen(Renderer* r, uint32_t color, float alpha) {
    (void) r;
    GLfloat rgba[4];
    unpackBGR(color, alpha, rgba);
    glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
    glClear(GL_COLOR_BUFFER_BIT);
}

static void gles1_flush(Renderer* r) {
    (void) r;
    glFlush();
}

// ============================================================================
// Renderer vtable: draw stubs
//
// These are intentional no-ops for the first on-device milestone. They
// will be replaced with real GLES 1.1 fixed-function draws once we have
// confirmation that the framework + context + buffer present pipeline
// works on the iPod Touch 2G. Filling them in incrementally lets us
// keep iterating from real-device logs instead of guessing.
// ============================================================================

static void gles1_drawSprite(Renderer* r, int32_t tpagIndex, float x, float y, float ox, float oy, float xs, float ys, float ang, uint32_t color, float alpha) {
    gles1_drawTpagQuad(asGLES1(r), tpagIndex, x, y, ox, oy, xs, ys, ang, -1, -1, -1, -1, color, alpha);
}

static void gles1_drawSpritePart(Renderer* r, int32_t tpagIndex, int32_t sox, int32_t soy, int32_t sw, int32_t sh, float x, float y, float xs, float ys, float ang, float px, float py, uint32_t color, float alpha) {
    gles1_drawTpagQuad(asGLES1(r), tpagIndex, x, y, px, py, xs, ys, ang, sox, soy, sw, sh, color, alpha);
}

static void gles1_drawSpritePos(Renderer* r, int32_t tpagIndex, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float alpha) {
    GLES1Renderer* g = asGLES1(r);
    DataWin* dw = g->base.dataWin;
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dw->tpag.count) return;
    TexturePageItem* it = &dw->tpag.items[tpagIndex];

    BSTexCacheSlot* slot = gles1_ensureSlot(g, it->texturePageId);
    if (slot == NULL || slot->bandHeight <= 0) return;
    int32_t band = gles1_bandOfRow(slot, it->sourceY);
    int32_t bandTop = band * slot->bandHeight;

    float u0 = (float) it->sourceX / (float) slot->width;
    float v0 = (float) (it->sourceY - bandTop) / (float) slot->bandHeight;
    float u1 = (float)(it->sourceX + it->sourceWidth) / (float) slot->width;
    float v1 = (float)(it->sourceY + it->sourceHeight - bandTop) / (float) slot->bandHeight;

    GLfloat verts[8] = { x1, y1, x2, y2, x4, y4, x3, y3 };
    GLfloat uvs[8]   = { u0, v0, u1, v0, u0, v1, u1, v1 };
    GLfloat rgba[4]  = { 1.0f, 1.0f, 1.0f, alpha };

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, slot->glHandles[band]);
    glColor4f(rgba[0], rgba[1], rgba[2], rgba[3]);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glTexCoordPointer(2, GL_FLOAT, 0, uvs);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
}

static void gles1_drawRectangle(Renderer* r, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    (void) r;
    GLfloat rgba[4];
    unpackBGR(color, alpha, rgba);
    GLfloat verts[8] = { x1, y1, x2, y1, x1, y2, x2, y2 };
    glDisable(GL_TEXTURE_2D);
    glColor4f(rgba[0], rgba[1], rgba[2], rgba[3]);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    if (outline) {
        GLfloat line[10] = { x1, y1, x2, y1, x2, y2, x1, y2, x1, y1 };
        glVertexPointer(2, GL_FLOAT, 0, line);
        glDrawArrays(GL_LINE_STRIP, 0, 5);
    } else {
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glDisableClientState(GL_VERTEX_ARRAY);
}

static void gles1_drawRectangleColor(Renderer* r, float x1, float y1, float x2, float y2, uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4, float alpha, bool outline) {
    (void) c2; (void) c3; (void) c4;
    gles1_drawRectangle(r, x1, y1, x2, y2, c1, alpha, outline);
}

static void gles1_drawLine(Renderer* r, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    (void) r; (void) width;
    GLfloat rgba[4];
    unpackBGR(color, alpha, rgba);
    GLfloat verts[4] = { x1, y1, x2, y2 };
    glDisable(GL_TEXTURE_2D);
    glColor4f(rgba[0], rgba[1], rgba[2], rgba[3]);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glDrawArrays(GL_LINES, 0, 2);
    glDisableClientState(GL_VERTEX_ARRAY);
}

static void gles1_drawTriangle(Renderer* r, float x1, float y1, float x2, float y2, float x3, float y3, bool outline) {
    (void) r;
    GLfloat verts[6] = { x1, y1, x2, y2, x3, y3 };
    glDisable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glDrawArrays(outline ? GL_LINE_LOOP : GL_TRIANGLES, 0, 3);
    glDisableClientState(GL_VERTEX_ARRAY);
}

static void gles1_drawLineColor(Renderer* r, float x1, float y1, float x2, float y2, float width, uint32_t c1, uint32_t c2, float alpha) {
    (void) c2;
    gles1_drawLine(r, x1, y1, x2, y2, width, c1, alpha);
}

// ============================================================================
// Text drawing — bitmap-font path
// ============================================================================
//
// GMS:Studio fonts are baked into a single TPAG (or for sprite fonts,
// one TPAG per glyph) at build time. To render text we:
//   1. Resolve the Font from dataWin->font.fonts[drawFont].
//   2. Resolve its atlas TPAG (or per-glyph TPAGs for sprite fonts).
//   3. Walk each line of UTF-8 text, looking up each glyph in the font,
//      computing UV from glyph sourceX/Y/W/H + atlas TPAG offset, and
//      emitting a 2-triangle strip per glyph.
//
// We bind the atlas texture once per glyph (the same atlas is usually
// reused across all glyphs in a line) — GLES 1.1 doesn't have batched
// glDrawArrays with multiple textures and a per-glyph rebind is cheap
// when the bind is to the already-bound id.

typedef struct {
    Font* font;
    TexturePageItem* atlasTpag; // single TPAG for regular fonts (NULL for sprite fonts)
    int32_t atlasTpagIndex;
    BSTexCacheSlot* slot;       // band-aware slot for regular fonts (NULL for sprite fonts)
    Sprite* spriteFontSprite;   // source sprite for sprite fonts (NULL otherwise)
} FontState;

static bool gles1_resolveFontState(GLES1Renderer* g, Font* font, FontState* st) {
    DataWin* dw = g->base.dataWin;
    st->font = font;
    st->atlasTpag = NULL;
    st->atlasTpagIndex = -1;
    st->slot = NULL;
    st->spriteFontSprite = NULL;

    if (!font->isSpriteFont) {
        int32_t tpi = font->tpagIndex;
        if (tpi < 0 || (uint32_t) tpi >= dw->tpag.count) return false;
        st->atlasTpagIndex = tpi;
        st->atlasTpag = &dw->tpag.items[tpi];
        int16_t pageId = st->atlasTpag->texturePageId;
        if (pageId < 0) return false;
        BSTexCacheSlot* slot = gles1_ensureSlot(g, pageId);
        if (slot == NULL || slot->bandHeight <= 0) return false;
        st->slot = slot;
        return true;
    }

    if (font->spriteIndex >= 0 && (uint32_t) font->spriteIndex < dw->sprt.count) {
        st->spriteFontSprite = &dw->sprt.sprites[font->spriteIndex];
        return true;
    }
    return false;
}

// Look up the atlas + UV coords + on-line glyph origin for a single
// glyph. Returns false if the glyph's atlas isn't resolvable (so the
// caller advances the cursor without drawing).
//
// Glyphs almost always fit inside a single band (they're ≤30px tall and
// the band granularity is 1024). For the rare case a glyph straddles a
// band boundary we just clamp to the lower band; the visible cropping
// is one frame of one glyph and almost imperceptible for tiny text.
static bool gles1_resolveGlyph(GLES1Renderer* g, FontState* st, FontGlyph* glyph,
                               float cursorX, float cursorY,
                               GLuint* outTex, float* outU0, float* outV0, float* outU1, float* outV1,
                               float* outLocalX0, float* outLocalY0, float* outDstW, float* outDstH) {
    DataWin* dw = g->base.dataWin;
    Font* font = st->font;
    if (font->isSpriteFont && st->spriteFontSprite != NULL) {
        Sprite* sprite = st->spriteFontSprite;
        int32_t glyphIdx = (int32_t) (glyph - font->glyphs);
        if (glyphIdx < 0 || (uint32_t) glyphIdx >= sprite->textureCount) return false;
        int32_t tpi = sprite->tpagIndices[glyphIdx];
        if (tpi < 0 || (uint32_t) tpi >= dw->tpag.count) return false;
        TexturePageItem* tp = &dw->tpag.items[tpi];
        int16_t pid = tp->texturePageId;
        if (pid < 0) return false;
        BSTexCacheSlot* slot = gles1_ensureSlot(g, pid);
        if (slot == NULL || slot->bandHeight <= 0) return false;
        int32_t band = gles1_bandOfRow(slot, tp->sourceY);
        int32_t bandTop = band * slot->bandHeight;
        *outTex = slot->glHandles[band];
        *outU0 = (float) tp->sourceX / (float) slot->width;
        *outV0 = (float) (tp->sourceY - bandTop) / (float) slot->bandHeight;
        *outU1 = (float) (tp->sourceX + tp->sourceWidth) / (float) slot->width;
        *outV1 = (float) (tp->sourceY + tp->sourceHeight - bandTop) / (float) slot->bandHeight;
        *outLocalX0 = cursorX + (float) glyph->offset;
        *outLocalY0 = cursorY + (float) ((int32_t) tp->targetY - sprite->originY);
        *outDstW = (float) tp->sourceWidth;
        *outDstH = (float) tp->sourceHeight;
        return true;
    }

    // Regular font: glyph sourceX/Y are coords inside the atlas TPAG,
    // so add atlas TPAG origin to land on the actual atlas texel.
    BSTexCacheSlot* slot = st->slot;
    if (slot == NULL || slot->bandHeight <= 0) return false;
    int32_t absX = st->atlasTpag->sourceX + glyph->sourceX;
    int32_t absY = st->atlasTpag->sourceY + glyph->sourceY;
    int32_t band = gles1_bandOfRow(slot, absY);
    int32_t bandTop = band * slot->bandHeight;
    *outTex = slot->glHandles[band];
    *outU0 = (float) absX / (float) slot->width;
    *outV0 = (float) (absY - bandTop) / (float) slot->bandHeight;
    *outU1 = (float) (absX + glyph->sourceWidth)  / (float) slot->width;
    *outV1 = (float) (absY + glyph->sourceHeight - bandTop) / (float) slot->bandHeight;
    *outLocalX0 = cursorX + (float) glyph->offset;
    *outLocalY0 = cursorY;
    *outDstW = (float) glyph->sourceWidth;
    *outDstH = (float) glyph->sourceHeight;
    return true;
}

// Common body of drawText / drawTextColor. The two only differ in how
// they sample color: drawText uses renderer->drawColor as a flat color,
// drawTextColor takes 4 corner colors but for now we just use c1 as
// a flat color (the runtime hardly uses the gradient variant for
// Undertale).
static void gles1_drawTextInternal(GLES1Renderer* g, const char* text,
                                   float x, float y, float xscale, float yscale, float angleDeg,
                                   uint32_t color, float alpha) {
    DataWin* dw = g->base.dataWin;
    int32_t fontIndex = g->base.drawFont;
    if (fontIndex < 0 || (uint32_t) fontIndex >= dw->font.count) return;
    Font* font = &dw->font.fonts[fontIndex];

    FontState st;
    if (!gles1_resolveFontState(g, font, &st)) return;

    GLfloat rgba[4];
    unpackBGR(color, alpha, rgba);

    int32_t textLen = (int32_t) strlen(text);
    int32_t lineCount = TextUtils_countLines(text, textLen);
    if (lineCount <= 0) return;
    float lineStride = TextUtils_lineStride(font);

    float totalHeight = (float) lineCount * lineStride;
    float valignOffset = 0.0f;
    if (g->base.drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (g->base.drawValign == 2) valignOffset = -totalHeight;

    float c = cosf(-angleDeg * 0.01745329252f);
    float s = sinf(-angleDeg * 0.01745329252f);
    float sx = xscale * font->scaleX;
    float sy = yscale * font->scaleY;

    glEnable(GL_TEXTURE_2D);
    glColor4f(rgba[0], rgba[1], rgba[2], rgba[3]);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    GLuint lastBoundTex = 0;

    float cursorY = valignOffset - (float) font->ascenderOffset;
    int32_t lineStart = 0;

    for (int32_t li = 0; li < lineCount; li++) {
        int32_t lineEnd = lineStart;
        while (lineEnd < textLen && !TextUtils_isNewlineChar(text[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;

        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0.0f;
        if (g->base.drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (g->base.drawHalign == 2) halignOffset = -lineWidth;
        float cursorX = halignOffset;

        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = false;
        if (lineLen > pos) {
            ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            hasCh = true;
        }
        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            uint16_t nextCh = 0;
            bool hasNext = lineLen > pos;
            if (hasNext) nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

            if (glyph != NULL) {
                bool drewSuccessfully = false;
                if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                    GLuint glyphTex;
                    float u0, v0, u1, v1;
                    float lx0, ly0, dstW, dstH;
                    if (gles1_resolveGlyph(g, &st, glyph, cursorX, cursorY,
                                           &glyphTex, &u0, &v0, &u1, &v1, &lx0, &ly0, &dstW, &dstH)) {
                        if (glyphTex != lastBoundTex) {
                            glBindTexture(GL_TEXTURE_2D, glyphTex);
                            lastBoundTex = glyphTex;
                        }
                        // Build the four corners with local-then-transform geometry
                        // matching the matrix `[xscale*c, -yscale*s; xscale*s, yscale*c]` then translate by (x,y).
                        float lxs[4] = { lx0,       lx0 + dstW, lx0,        lx0 + dstW };
                        float lys[4] = { ly0,       ly0,        ly0 + dstH, ly0 + dstH };
                        GLfloat verts[8];
                        for (int k = 0; k < 4; k++) {
                            float lxScaled = lxs[k] * sx;
                            float lyScaled = lys[k] * sy;
                            verts[k*2+0] = x + (lxScaled * c - lyScaled * s);
                            verts[k*2+1] = y + (lxScaled * s + lyScaled * c);
                        }
                        GLfloat uvs[8] = { u0, v0,  u1, v0,  u0, v1,  u1, v1 };
                        glVertexPointer(2, GL_FLOAT, 0, verts);
                        glTexCoordPointer(2, GL_FLOAT, 0, uvs);
                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                        drewSuccessfully = true;
                    }
                }
                cursorX += (float) glyph->shift;
                if (drewSuccessfully && hasNext) {
                    cursorX += TextUtils_getKerningOffset(glyph, nextCh);
                }
            }
            ch = nextCh;
            hasCh = hasNext;
        }
        cursorY += lineStride;
        lineStart = (lineEnd < textLen) ? TextUtils_skipNewline(text, lineEnd, textLen) : lineEnd;
    }

    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
}

static void gles1_drawText(Renderer* r, const char* text, float x, float y, float xs, float ys, float ang) {
    gles1_drawTextInternal(asGLES1(r), text, x, y, xs, ys, ang, r->drawColor, r->drawAlpha);
}

static void gles1_drawTextColor(Renderer* r, const char* text, float x, float y, float xs, float ys, float ang,
                                int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha) {
    (void) c2; (void) c3; (void) c4; // GLES 1.1 fixed pipeline path: flatten to top-left color
    uint32_t bgr = (c1 >= 0) ? (uint32_t) c1 : r->drawColor;
    gles1_drawTextInternal(asGLES1(r), text, x, y, xs, ys, ang, bgr, alpha);
}

static int32_t gles1_createSpriteFromSurface(Renderer* r, int32_t sid, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xo, int32_t yo) {
    (void) r; (void) sid; (void) x; (void) y; (void) w; (void) h; (void) removeback; (void) smooth; (void) xo; (void) yo;
    return -1;
}

static void gles1_deleteSprite(Renderer* r, int32_t s) { (void) r; (void) s; }

// ============================================================================
// Renderer vtable: GPU state
// ============================================================================

static void gles1_gpuSetBlendMode(Renderer* r, int32_t mode) {
    (void) r; (void) mode;
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void gles1_gpuSetBlendModeExt(Renderer* r, int32_t s, int32_t d) {
    (void) r; (void) s; (void) d;
}

static void gles1_gpuSetBlendEnable(Renderer* r, bool e) {
    (void) r;
    if (e) glEnable(GL_BLEND); else glDisable(GL_BLEND);
}

static void gles1_gpuSetAlphaTestEnable(Renderer* r, bool e) {
    (void) r;
    if (e) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST);
}

static void gles1_gpuSetAlphaTestRef(Renderer* r, uint8_t ref) {
    (void) r;
    glAlphaFunc(GL_GREATER, (GLclampf) (ref / 255.0f));
}

static void gles1_gpuSetColorWriteEnable(Renderer* r, bool rd, bool g, bool b, bool a) {
    (void) r;
    glColorMask(rd ? GL_TRUE : GL_FALSE, g ? GL_TRUE : GL_FALSE, b ? GL_TRUE : GL_FALSE, a ? GL_TRUE : GL_FALSE);
}

static void gles1_gpuGetColorWriteEnable(Renderer* r, bool* rd, bool* g, bool* b, bool* a) {
    (void) r;
    if (rd) *rd = true;
    if (g)  *g  = true;
    if (b)  *b  = true;
    if (a)  *a  = true;
}

static bool gles1_gpuGetBlendEnable(Renderer* r) {
    (void) r;
    return glIsEnabled(GL_BLEND) == GL_TRUE;
}

static void gles1_gpuSetFog(Renderer* r, bool e, uint32_t color) {
    (void) r; (void) e; (void) color;
}

// ============================================================================
// Renderer vtable: surfaces
// ============================================================================

static int32_t gles1_createSurface(Renderer* r, int32_t w, int32_t h) {
    (void) r; (void) w; (void) h;
    return -1;
}

static bool gles1_surfaceExists(Renderer* r, int32_t s) {
    (void) r; (void) s;
    return false;
}

static bool gles1_setRenderTarget(Renderer* r, int32_t s) {
    (void) r; (void) s;
    return true;
}

static float gles1_getSurfaceWidth(Renderer* r, int32_t s) {
    (void) r; (void) s;
    return 0.0f;
}

static float gles1_getSurfaceHeight(Renderer* r, int32_t s) {
    (void) r; (void) s;
    return 0.0f;
}

static void gles1_drawSurface(Renderer* r, int32_t s, int32_t sl, int32_t st, int32_t sw, int32_t sh, float x, float y, float xs, float ys, float ang, uint32_t color, float alpha) {
    (void) r; (void) s; (void) sl; (void) st; (void) sw; (void) sh;
    (void) x; (void) y; (void) xs; (void) ys; (void) ang; (void) color; (void) alpha;
}

static void gles1_surfaceResize(Renderer* r, int32_t s, int32_t w, int32_t h) {
    (void) r; (void) s; (void) w; (void) h;
}

static void gles1_surfaceFree(Renderer* r, int32_t s) {
    (void) r; (void) s;
}

static void gles1_surfaceCopy(Renderer* r, int32_t ds, int32_t dx, int32_t dy, int32_t ss, int32_t sx, int32_t sy, int32_t sw, int32_t sh, bool part) {
    (void) r; (void) ds; (void) dx; (void) dy; (void) ss; (void) sx; (void) sy; (void) sw; (void) sh; (void) part;
}

static bool gles1_surfaceGetPixels(Renderer* r, int32_t s, uint8_t* out) {
    (void) r; (void) s; (void) out;
    return false;
}

// ============================================================================
// Constructor + vtable instance
// ============================================================================

static RendererVtable kGles1Vtable = {
    .init                     = gles1_init,
    .destroy                  = gles1_destroy,
    .beginFrame               = gles1_beginFrame,
    .endFrame                 = gles1_endFrame,
    .beginView                = gles1_beginView,
    .endView                  = gles1_endView,
    .beginGUI                 = gles1_beginGUI,
    .endGUI                   = gles1_endGUI,
    .drawSprite               = gles1_drawSprite,
    .drawSpritePart           = gles1_drawSpritePart,
    .drawSpritePos            = gles1_drawSpritePos,
    .drawRectangle            = gles1_drawRectangle,
    .drawRectangleColor       = gles1_drawRectangleColor,
    .drawLine                 = gles1_drawLine,
    .drawTriangle             = gles1_drawTriangle,
    .drawLineColor            = gles1_drawLineColor,
    .drawText                 = gles1_drawText,
    .drawTextColor            = gles1_drawTextColor,
    .flush                    = gles1_flush,
    .clearScreen              = gles1_clearScreen,
    .createSpriteFromSurface  = gles1_createSpriteFromSurface,
    .deleteSprite             = gles1_deleteSprite,
    .gpuSetBlendMode          = gles1_gpuSetBlendMode,
    .gpuSetBlendModeExt       = gles1_gpuSetBlendModeExt,
    .gpuSetBlendEnable        = gles1_gpuSetBlendEnable,
    .gpuSetAlphaTestEnable    = gles1_gpuSetAlphaTestEnable,
    .gpuSetAlphaTestRef       = gles1_gpuSetAlphaTestRef,
    .gpuSetColorWriteEnable   = gles1_gpuSetColorWriteEnable,
    .gpuGetColorWriteEnable   = gles1_gpuGetColorWriteEnable,
    .gpuGetBlendEnable        = gles1_gpuGetBlendEnable,
    .gpuSetFog                = gles1_gpuSetFog,
    .drawTile                 = NULL,  // fall back to drawSpritePart
    .drawTiled                = NULL,  // fall back to per-tile loop
    .createSurface            = gles1_createSurface,
    .surfaceExists            = gles1_surfaceExists,
    .setRenderTarget          = gles1_setRenderTarget,
    .getSurfaceWidth          = gles1_getSurfaceWidth,
    .getSurfaceHeight         = gles1_getSurfaceHeight,
    .drawSurface              = gles1_drawSurface,
    .surfaceResize            = gles1_surfaceResize,
    .surfaceFree              = gles1_surfaceFree,
    .surfaceCopy              = gles1_surfaceCopy,
    .surfaceGetPixels         = gles1_surfaceGetPixels,
    .drawTiledPart            = NULL,
};

Renderer* GLES1Renderer_create(void) {
    GLES1Renderer* g = (GLES1Renderer*) calloc(1, sizeof(GLES1Renderer));
    if (g == NULL) return NULL;
    g->base.vtable = &kGles1Vtable;
    return &g->base;
}

void GLES1Renderer_setDataWinPath(Renderer* r, const char* path) {
    if (r == NULL || path == NULL) return;
    GLES1Renderer* g = asGLES1(r);
    if (g->dataWinPath != NULL) {
        free(g->dataWinPath);
        g->dataWinPath = NULL;
    }
    size_t n = strlen(path);
    g->dataWinPath = (char*) malloc(n + 1);
    if (g->dataWinPath != NULL) {
        memcpy(g->dataWinPath, path, n + 1);
    }
}
