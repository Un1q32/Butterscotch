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
    // Cached frame geometry: set by beginFrame, used by beginView / beginGUI
    // to translate game-space port rects into framebuffer pixel rects.
    int32_t frameGameW;
    int32_t frameGameH;
    int32_t frameWindowW;
    int32_t frameWindowH;
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

    if (dataWin->txtr.count > 0) {
        g->slotCount = dataWin->txtr.count;
        g->slots = (BSTexCacheSlot*) calloc(g->slotCount, sizeof(BSTexCacheSlot));
    }
    g->logBudget = 8; 

    g->frameTick = 0;
    g->residentBytes = 0;
    // ~10 MB GPU atlas budget.  Atlases are uploaded as RGBA4 (2 bytes
    // per pixel), so a full 1024x1024 atlas is only 2 MB resident; this
    // budget therefore comfortably holds ~5 of them at once before
    // eviction kicks in.  We deliberately keep ~70 MB of headroom on a
    // 128 MB iPod Touch 2G for data.win metadata + the PNG decode peak
    // (stb_image emits 8888 even though we then convert to 4444).
    g->residentBudget = 10u * 1024u * 1024u;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);

    GLint maxTex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
    if (maxTex <= 0) maxTex = 1024; 
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
// Texture streaming
// ============================================================================

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

static bool gles1_decodeAndUploadSlot(GLES1Renderer* g, int32_t txtrIndex, BSTexCacheSlot* slot) {
    if (g->dataWinFile == NULL) return false;

    Texture* tex = &g->base.dataWin->txtr.textures[txtrIndex];
    if (tex->blobOffset == 0 || tex->blobSize == 0) return false;

    uint8_t* compressed = (uint8_t*) malloc(tex->blobSize);
    if (compressed == NULL) return false;
    
    if (fseek(g->dataWinFile, (long) tex->blobOffset, SEEK_SET) != 0) {
        free(compressed);
        return false;
    }
    
    size_t got = fread(compressed, 1, tex->blobSize, g->dataWinFile);
    if (got != tex->blobSize) {
        free(compressed);
        return false;
    }
    
    int w = 0, h = 0, ch = 0;
    // CRITICAL: Requesting 4 channels strictly forces RGBA extraction.
    stbi_uc* pixels = stbi_load_from_memory(compressed, (int) tex->blobSize, &w, &h, &ch, 4);
    free(compressed);
    
    if (pixels == NULL || w <= 0 || h <= 0) return false;

    int origW = w, origH = h;

    int factorX = 1;
    while (w / factorX > g->maxTextureSize) factorX *= 2;
    if (factorX != 1) {
        if (!gles1_downsampleRGBA(&pixels, &w, &h, factorX, 1)) {
            stbi_image_free(pixels);
            return false;
        }
    }

    int32_t bandH = g->maxTextureSize;
    int32_t numBands = (h + bandH - 1) / bandH;
    if (numBands > BS_MAX_BANDS) {
        int factorY = 1;
        while ((h + factorY - 1) / factorY > BS_MAX_BANDS * g->maxTextureSize) factorY *= 2;
        if (factorY > 1) {
            if (!gles1_downsampleRGBA(&pixels, &w, &h, 1, factorY)) {
                stbi_image_free(pixels);
                return false;
            }
        }
        numBands = (h + bandH - 1) / bandH;
    }

    // FIX: Force unpack alignment to 1 byte. OpenGL by default expects 4-byte 
    // row alignment. Even though RGBA (w*4) is usually naturally aligned, 
    // older iOS MBX Lite drivers can stumble on stride mismatches causing 
    // 4x duplication or slanted striping. This forces contiguous reading.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    uint32_t totalBytes = 0;
    for (int32_t b = 0; b < numBands; b++) {
        int32_t rowStart = b * bandH;
        int32_t rowEnd   = rowStart + bandH;
        if (rowEnd > h) rowEnd = h;
        int32_t thisBandH = rowEnd - rowStart;
        if (thisBandH <= 0) { numBands = b; break; }

        // GL storage is GL_UNSIGNED_SHORT_4_4_4_4 = 2 bytes per pixel,
        // half the size of the source 8888.  PowerVR MBX Lite stores
        // textures internally as 16-bit anyway, so we're not losing
        // anything by uploading at the format the GPU is going to use.
        uint32_t bandBytes = (uint32_t)(w * bandH * 2);
        if (g->residentBytes + totalBytes + bandBytes > g->residentBudget) {
            gles1_evictLRU(g, totalBytes + bandBytes);
        }
        
        GLuint handle = 0;
        glGenTextures(1, &handle);
        glBindTexture(GL_TEXTURE_2D, handle);
        
        // GL_NEAREST gives chunky pixel-art look (Paint-style zoom) that
        // matches Undertale's intended aesthetic.  We were on GL_LINEAR
        // briefly to mask scanline artifacts caused by the broken
        // 320x480 → 480x320 renderbuffer in v0.6.1–0.6.6; once the layer
        // / renderbuffer plumbing was fixed in v0.6.7, the GL_LINEAR
        // softening was no longer needed and just made text mushy.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Convert this band from RGBA8888 -> RGBA4444 directly into the
        // upload buffer. Size = w * bandH * 2 bytes (half of the source
        // band's 8888 footprint).  We always allocate a full bandH-sized
        // upload buffer (padded with zeroes for the trailing short
        // band) so the GL storage is a consistent power-of-two-ish
        // height for MBX Lite, which prefers it.
        uint16_t* up = (uint16_t*) calloc((size_t) w * (size_t) bandH, sizeof(uint16_t));
        if (up == NULL) {
            glDeleteTextures(1, &handle);
            for (int32_t bb = 0; bb < b; bb++) {
                glDeleteTextures(1, &slot->glHandles[bb]);
                slot->glHandles[bb] = 0;
            }
            stbi_image_free(pixels);
            return false;
        }
        const uint8_t* src = pixels + (size_t) rowStart * w * 4;
        for (int32_t y = 0; y < thisBandH; y++) {
            const uint8_t* srow = src + (size_t) y * w * 4;
            uint16_t* drow = up + (size_t) y * w;
            for (int32_t x = 0; x < w; x++) {
                uint8_t r = srow[x * 4 + 0];
                uint8_t gC = srow[x * 4 + 1];
                uint8_t bC = srow[x * 4 + 2];
                uint8_t a = srow[x * 4 + 3];
                drow[x] = (uint16_t) ((((uint16_t)(r >> 4)) << 12) |
                                      (((uint16_t)(gC >> 4)) << 8) |
                                      (((uint16_t)(bC >> 4)) << 4) |
                                       ((uint16_t)(a >> 4)));
            }
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, bandH, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, up);
        free(up);
        
        GLenum glErr = glGetError();
        if (glErr != GL_NO_ERROR) {
            fprintf(stderr, "[gles1] ensureSlot[%d] band %d: glTexImage2D failed 0x%04x (size %dx%d)\n",
                    txtrIndex, (int) b, (unsigned) glErr, w, bandH);
            glDeleteTextures(1, &handle);
            for (int32_t bb = 0; bb < b; bb++) {
                glDeleteTextures(1, &slot->glHandles[bb]);
                slot->glHandles[bb] = 0;
            }
            stbi_image_free(pixels);
            return false;
        }
        slot->glHandles[b] = handle;
        totalBytes += bandBytes;
    }
    stbi_image_free(pixels);

    slot->width = origW;
    slot->height = origH;
    slot->bandHeight = bandH;
    slot->numBands = numBands;
    slot->bytesUploaded = totalBytes;
    slot->lastUsedTick = g->frameTick;
    g->residentBytes += totalBytes;

    return true;
}

static BSTexCacheSlot* gles1_ensureSlot(GLES1Renderer* g, int32_t txtrIndex) {
    if (txtrIndex < 0 || (uint32_t) txtrIndex >= g->slotCount) return NULL;
    BSTexCacheSlot* slot = &g->slots[txtrIndex];
    slot->lastUsedTick = g->frameTick;
    if (gles1_slotIsResident(slot)) return slot;
    if (!gles1_decodeAndUploadSlot(g, txtrIndex, slot)) return NULL;
    return slot;
}

static void gles1_buildSpriteQuad(
        float dstX, float dstY, float dstW, float dstH,
        float originX, float originY, float xscale, float yscale, float angleDeg,
        float u0, float v0, float u1, float v1,
        GLfloat out_verts[8], GLfloat out_uvs[8]) {
    float c = cosf(angleDeg * 0.01745329252f);
    float s = sinf(angleDeg * 0.01745329252f);
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

static void gles1_emitQuad(GLuint handle,
                           float dstX, float dstY, float dstW, float dstH,
                           float originX, float originY,
                           float xscale, float yscale, float angleDeg,
                           float u0, float v0, float u1, float v1,
                           const GLfloat rgba[4]) {
    GLfloat verts[8];
    GLfloat uvs[8];
    gles1_buildSpriteQuad(dstX, dstY, dstW, dstH, originX, originY,
                          xscale, yscale, angleDeg, u0, v0, u1, v1, verts, uvs);

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
                               int32_t subX, int32_t subY, int32_t subW, int32_t subH,
                               uint32_t color, float alpha) {
    DataWin* dw = g->base.dataWin;
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dw->tpag.count) return;
    TexturePageItem* it = &dw->tpag.items[tpagIndex];

    BSTexCacheSlot* slot = gles1_ensureSlot(g, it->texturePageId);
    if (slot == NULL || slot->bandHeight <= 0) return;

    int32_t srcX = it->sourceX + (subX >= 0 ? subX : 0);
    int32_t srcY = it->sourceY + (subY >= 0 ? subY : 0);
    int32_t srcW = (subW > 0) ? subW : it->sourceWidth;
    int32_t srcH = (subH > 0) ? subH : it->sourceHeight;

    if (srcW <= 0 || srcH <= 0) return;

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

    if (firstBand == lastBand) {
        int32_t bandTop = firstBand * bandH;
        float v0 = (float)(srcY - bandTop) / (float) bandH;
        float v1 = (float)(srcY + srcH - bandTop) / (float) bandH;
        gles1_emitQuad(slot->glHandles[firstBand],
                       dstX, dstY, fullDstW, (float) srcH,
                       originX - ofsX, originY - ofsY,
                       xscale, yscale, angleDeg,
                       u0, v0, u1, v1, rgba);
        return;
    }

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
                       u0, v0, u1, v1, rgba);
        dstStartY += segH * yscale;
        srcCursor += segH;
        srcRemaining -= segH;
    }
    (void) totalSrcH;
}

// ============================================================================
// Renderer vtable: frame + view + GUI passes
// ============================================================================

// Test pattern: draws four coloured quads + a white border directly into the
// visible framebuffer.  Bypasses everything in the game / projection pipeline
// so we can see what the GL/iOS layer presents.
//
// Layout (in NDC, ortho top-left = (0,0)):
//   TL = red    (0,0)..(0.5,0.5)
//   TR = green  (0.5,0)..(1,0.5)
//   BL = blue   (0,0.5)..(0.5,1)
//   BR = yellow (0.5,0.5)..(1,1)
// Plus a 4-NDC-pixel-wide WHITE border around the whole frame so we can see
// the actual edges of the renderbuffer as displayed by iOS.  And a CYAN
// horizontal line at y=0.25 and y=0.75 to detect horizontal tiling/wrap.
static void gles1_drawColoredQuad(float x0, float y0, float x1, float y1,
                                  float r, float g, float b) {
    GLfloat v[8] = { x0, y0,  x1, y0,  x0, y1,  x1, y1 };
    glVertexPointer(2, GL_FLOAT, 0, v);
    glColor4f(r, g, b, 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void gles1_drawDiagnosticTestPattern(int32_t windowW, int32_t windowH) {
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    glViewport(0, 0, windowW, windowH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0.0f, 1.0f, 1.0f, 0.0f, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // Black clear first so any region NOT covered by quads stays black.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnableClientState(GL_VERTEX_ARRAY);

    // Four quadrant colour blocks, drawn as separate quads (no scissor).
    gles1_drawColoredQuad(0.00f, 0.00f, 0.50f, 0.50f, 1.0f, 0.0f, 0.0f); // TL red
    gles1_drawColoredQuad(0.50f, 0.00f, 1.00f, 0.50f, 0.0f, 1.0f, 0.0f); // TR green
    gles1_drawColoredQuad(0.00f, 0.50f, 0.50f, 1.00f, 0.0f, 0.0f, 1.0f); // BL blue
    gles1_drawColoredQuad(0.50f, 0.50f, 1.00f, 1.00f, 1.0f, 1.0f, 0.0f); // BR yellow

    // Vertical CYAN markers at NDC x = 0.25, 0.5, 0.75 (full height).
    // Each ~1.5 pixels wide.  These let us detect horizontal tiling: if
    // the screen shows 4 copies of the pattern, we'll see 4 cyan stripes
    // each per quadrant instead of just one between quadrants.
    float vMarker = 1.5f / (float) (windowW > 0 ? windowW : 1);
    gles1_drawColoredQuad(0.25f - vMarker, 0.0f, 0.25f + vMarker, 1.0f, 0.0f, 1.0f, 1.0f);
    gles1_drawColoredQuad(0.50f - vMarker, 0.0f, 0.50f + vMarker, 1.0f, 1.0f, 1.0f, 1.0f);
    gles1_drawColoredQuad(0.75f - vMarker, 0.0f, 0.75f + vMarker, 1.0f, 0.0f, 1.0f, 1.0f);

    // Horizontal MAGENTA markers at NDC y = 0.25, 0.75 (full width). For
    // detecting vertical tiling.
    float hMarker = 1.5f / (float) (windowH > 0 ? windowH : 1);
    gles1_drawColoredQuad(0.0f, 0.25f - hMarker, 1.0f, 0.25f + hMarker, 1.0f, 0.0f, 1.0f);
    gles1_drawColoredQuad(0.0f, 0.75f - hMarker, 1.0f, 0.75f + hMarker, 1.0f, 0.0f, 1.0f);

    // WHITE outer border, 4 NDC pixels thick — shows the actual visible
    // edge of the renderbuffer.
    float bx = 4.0f / (float) (windowW > 0 ? windowW : 1);
    float by = 4.0f / (float) (windowH > 0 ? windowH : 1);
    gles1_drawColoredQuad(0.0f, 0.0f, 1.0f, by, 1.0f, 1.0f, 1.0f);    // top
    gles1_drawColoredQuad(0.0f, 1.0f - by, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f); // bottom
    gles1_drawColoredQuad(0.0f, 0.0f, bx, 1.0f, 1.0f, 1.0f, 1.0f);    // left
    gles1_drawColoredQuad(1.0f - bx, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f); // right

    glDisableClientState(GL_VERTEX_ARRAY);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

// Frames over which the diagnostic test pattern overrides the normal game
// render. After this many frames the renderer reverts to drawing the game.
// We make this short so the user can quickly see whether the pipeline is OK.
#define BS_DIAG_PATTERN_FRAMES 240

// Map a port rect (in game-screen coordinates, 0..gameW × 0..gameH) into
// physical framebuffer pixels (0..windowW × 0..windowH). Uses a uniform
// scale = min(sx, sy) so the game's aspect ratio is preserved, with
// letterboxing centered on the framebuffer (black bars on the longer
// axis). Game-screen Y runs top-down (origin top-left) but GL viewport
// Y runs bottom-up, so we flip the y axis.
//
// For Undertale on an iPod Touch 2G: game = 640x480 (4:3), window =
// 480x320 (3:2). Uniform scale = min(480/640, 320/480) = 0.6667. Game
// fits as 426x320 centered with ~27px black bars on the left and right.
static void gles1_portToViewport(GLES1Renderer* g,
                                 int32_t portX, int32_t portY, int32_t portW, int32_t portH,
                                 GLint* vx, GLint* vy, GLsizei* vw, GLsizei* vh) {
    int32_t gameW = g->frameGameW > 0 ? g->frameGameW : g->frameWindowW;
    int32_t gameH = g->frameGameH > 0 ? g->frameGameH : g->frameWindowH;
    if (gameW <= 0) gameW = 1;
    if (gameH <= 0) gameH = 1;
    float sxFull = (float) g->frameWindowW / (float) gameW;
    float syFull = (float) g->frameWindowH / (float) gameH;
    float s = sxFull < syFull ? sxFull : syFull;
    float fitW = (float) gameW * s;
    float fitH = (float) gameH * s;
    float offX = ((float) g->frameWindowW - fitW) * 0.5f;
    float offY = ((float) g->frameWindowH - fitH) * 0.5f;
    int32_t pxX = (int32_t) (offX + (float) portX * s + 0.5f);
    int32_t pxY = (int32_t) (offY + (float) portY * s + 0.5f);
    int32_t pxW = (int32_t) ((float) portW * s + 0.5f);
    int32_t pxH = (int32_t) ((float) portH * s + 0.5f);
    // Flip Y: game-screen top-left origin → GL bottom-left origin.
    *vx = (GLint) pxX;
    *vy = (GLint) (g->frameWindowH - (pxY + pxH));
    *vw = (GLsizei) pxW;
    *vh = (GLsizei) pxH;
}

static void gles1_beginFrame(Renderer* r, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    GLES1Renderer* g = asGLES1(r);
    g->frameTick++;
    if (gameW <= 0) gameW = windowW;
    if (gameH <= 0) gameH = windowH;
    g->frameGameW = gameW;
    g->frameGameH = gameH;
    g->frameWindowW = windowW;
    g->frameWindowH = windowH;

    if (g->frameTick <= 4 || g->frameTick == 60 || g->frameTick == 180) {
        fprintf(stderr, "[gles1] beginFrame tick=%u game=%dx%d window=%dx%d\n",
                g->frameTick, gameW, gameH, windowW, windowH);
    }

    glViewport(0, 0, windowW, windowH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0.0f, (GLfloat) gameW, (GLfloat) gameH, 0.0f, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    
    // FIX: Ensure no residual state messes up the start of the frame.
    glDisable(GL_TEXTURE_2D);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}

static void gles1_endFrame(Renderer* r) {
    (void) r;
    glFlush();
}

// view = rect in room coordinates the game wants to display
// port = rect in game-screen coordinates (0..gameW, 0..gameH) where the
//        view should land
// We translate the port rect into physical framebuffer pixels and set up
// the projection so that drawing in view coords lands in the port rect.
static void gles1_beginView(Renderer* r, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH,
                            int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle) {
    GLES1Renderer* g = asGLES1(r);
    (void) viewAngle;
    if (viewW <= 0 || viewH <= 0 || portW <= 0 || portH <= 0) return;

    GLint vx, vy; GLsizei vw, vh;
    gles1_portToViewport(g, portX, portY, portW, portH, &vx, &vy, &vw, &vh);

    if (g->frameTick <= 2 || g->frameTick == 60) {
        fprintf(stderr, "[gles1] beginView tick=%u view=(%d,%d %dx%d) port=(%d,%d %dx%d) -> vp=(%d,%d %dx%d)\n",
                g->frameTick, viewX, viewY, viewW, viewH, portX, portY, portW, portH,
                (int) vx, (int) vy, (int) vw, (int) vh);
    }

    glViewport(vx, vy, vw, vh);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof((GLfloat) viewX, (GLfloat) (viewX + viewW),
             (GLfloat) (viewY + viewH), (GLfloat) viewY,
             -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void gles1_endView(Renderer* r) { (void) r; }

// GUI is drawn in (0..guiW, 0..guiH) coordinates and landed in the given
// port rect on screen.
static void gles1_beginGUI(Renderer* r, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY, int32_t portW, int32_t portH) {
    GLES1Renderer* g = asGLES1(r);
    if (guiW <= 0 || guiH <= 0 || portW <= 0 || portH <= 0) return;

    GLint vx, vy; GLsizei vw, vh;
    gles1_portToViewport(g, portX, portY, portW, portH, &vx, &vy, &vw, &vh);

    if (g->frameTick <= 2 || g->frameTick == 60) {
        fprintf(stderr, "[gles1] beginGUI tick=%u gui=%dx%d port=(%d,%d %dx%d) -> vp=(%d,%d %dx%d)\n",
                g->frameTick, guiW, guiH, portX, portY, portW, portH,
                (int) vx, (int) vy, (int) vw, (int) vh);
    }

    glViewport(vx, vy, vw, vh);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0.0f, (GLfloat) guiW, (GLfloat) guiH, 0.0f, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
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
// Text drawing
// ============================================================================

typedef struct {
    Font* font;
    TexturePageItem* atlasTpag;
    int32_t atlasTpagIndex;
    BSTexCacheSlot* slot;
    Sprite* spriteFontSprite;
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
    (void) c2; (void) c3; (void) c4; 
    uint32_t bgr = (c1 >= 0) ? (uint32_t) c1 : r->drawColor;
    gles1_drawTextInternal(asGLES1(r), text, x, y, xs, ys, ang, bgr, alpha);
}

static int32_t gles1_createSpriteFromSurface(Renderer* r, int32_t sid, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xo, int32_t yo) { return -1; }
static void gles1_deleteSprite(Renderer* r, int32_t s) { }

// ============================================================================
// Renderer vtable: GPU state
// ============================================================================

static void gles1_gpuSetBlendMode(Renderer* r, int32_t mode) {
    (void) r; (void) mode;
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void gles1_gpuSetBlendModeExt(Renderer* r, int32_t s, int32_t d) { }

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

static void gles1_gpuSetFog(Renderer* r, bool e, uint32_t color) { }

// ============================================================================
// Renderer vtable: surfaces
// ============================================================================

static int32_t gles1_createSurface(Renderer* r, int32_t w, int32_t h) { return -1; }
static bool gles1_surfaceExists(Renderer* r, int32_t s) { return false; }
static bool gles1_setRenderTarget(Renderer* r, int32_t s) { return true; }
static float gles1_getSurfaceWidth(Renderer* r, int32_t s) { return 0.0f; }
static float gles1_getSurfaceHeight(Renderer* r, int32_t s) { return 0.0f; }
static void gles1_drawSurface(Renderer* r, int32_t s, int32_t sl, int32_t st, int32_t sw, int32_t sh, float x, float y, float xs, float ys, float ang, uint32_t color, float alpha) { }
static void gles1_surfaceResize(Renderer* r, int32_t s, int32_t w, int32_t h) { }
static void gles1_surfaceFree(Renderer* r, int32_t s) { }
static void gles1_surfaceCopy(Renderer* r, int32_t ds, int32_t dx, int32_t dy, int32_t ss, int32_t sx, int32_t sy, int32_t sw, int32_t sh, bool part) { }
static bool gles1_surfaceGetPixels(Renderer* r, int32_t s, uint8_t* out) { return false; }

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
    .drawTile                 = NULL,  
    .drawTiled                = NULL,  
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

void GLES1Renderer_handleMemoryWarning(Renderer* r) {
    if (r == NULL) return;
    GLES1Renderer* g = asGLES1(r);
    if (g->slots == NULL) return;
    uint32_t freedSlots = 0;
    uint32_t freedBytes = 0;
    for (uint32_t i = 0; i < g->slotCount; i++) {
        BSTexCacheSlot* s = &g->slots[i];
        if (!gles1_slotIsResident(s)) continue;
        // Keep atlases that were used this frame — they're almost
        // certainly going to be sampled again next frame.
        if (s->lastUsedTick == g->frameTick) continue;
        uint32_t bytes = s->bytesUploaded;
        gles1_freeSlotGL(s);
        if (g->residentBytes >= bytes) g->residentBytes -= bytes;
        else g->residentBytes = 0;
        freedSlots++;
        freedBytes += bytes;
    }
    fprintf(stderr, "[gles1] memory warning: evicted %u atlases (%u bytes), now resident=%u/%u\n",
            (unsigned) freedSlots, (unsigned) freedBytes,
            (unsigned) g->residentBytes, (unsigned) g->residentBudget);
}
