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

// Cache slot for one TXTR entry. We map TXTR index -> one GL texture
// object; sprite/background TPAG rectangles index into the cached
// texture via texcoords.
typedef struct {
    GLuint glHandle;     // 0 if not yet uploaded
    int32_t width;       // populated after first upload
    int32_t height;
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
} GLES1Renderer;

static inline GLES1Renderer* asGLES1(Renderer* r) {
    return (GLES1Renderer*) r;
}

// Forward decl.
static GLuint gles1_ensureTexture(GLES1Renderer* g, int32_t txtrIndex, int32_t* outW, int32_t* outH);

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
}

static void gles1_destroy(Renderer* r) {
    GLES1Renderer* g = asGLES1(r);
    if (g->slots != NULL) {
        for (uint32_t i = 0; i < g->slotCount; i++) {
            if (g->slots[i].glHandle != 0) {
                glDeleteTextures(1, &g->slots[i].glHandle);
            }
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

static void gles1_evictLRU(GLES1Renderer* g, uint32_t bytesToFree) {
    while (g->residentBytes + bytesToFree > g->residentBudget) {
        // Find slot with smallest non-zero lastUsedTick that isn't the
        // current frame's tick (we never evict something we just used).
        uint32_t victimIdx = UINT32_MAX;
        uint32_t victimTick = UINT32_MAX;
        for (uint32_t i = 0; i < g->slotCount; i++) {
            BSTexCacheSlot* s = &g->slots[i];
            if (s->glHandle == 0) continue;
            if (s->lastUsedTick == g->frameTick) continue;
            if (s->lastUsedTick < victimTick) {
                victimTick = s->lastUsedTick;
                victimIdx = i;
            }
        }
        if (victimIdx == UINT32_MAX) break; // nothing to evict

        BSTexCacheSlot* v = &g->slots[victimIdx];
        uint32_t bytes = (uint32_t)(v->width * v->height * 4);
        glDeleteTextures(1, &v->glHandle);
        v->glHandle = 0;
        v->width = 0;
        v->height = 0;
        v->lastUsedTick = 0;
        if (g->residentBytes >= bytes) g->residentBytes -= bytes;
        else g->residentBytes = 0;
    }
}

static GLuint gles1_ensureTexture(GLES1Renderer* g, int32_t txtrIndex, int32_t* outW, int32_t* outH) {
    if (txtrIndex < 0 || (uint32_t) txtrIndex >= g->slotCount) {
        if (g->logBudget > 0) {
            fprintf(stderr, "[gles1] ensureTexture: index %d out of range [0, %u)\n",
                    txtrIndex, g->slotCount);
            g->logBudget--;
        }
        return 0;
    }
    BSTexCacheSlot* slot = &g->slots[txtrIndex];
    slot->lastUsedTick = g->frameTick;

    if (slot->glHandle != 0) {
        if (outW != NULL) *outW = slot->width;
        if (outH != NULL) *outH = slot->height;
        return slot->glHandle;
    }

    if (g->dataWinFile == NULL) {
        if (g->logBudget > 0) {
            fprintf(stderr, "[gles1] ensureTexture[%d]: dataWinFile is NULL\n", txtrIndex);
            g->logBudget--;
        }
        return 0;
    }

    Texture* tex = &g->base.dataWin->txtr.textures[txtrIndex];
    if (g->logBudget > 0) {
        fprintf(stderr, "[gles1] ensureTexture[%d]: blobOffset=%u blobSize=%u\n",
                txtrIndex, (unsigned) tex->blobOffset, (unsigned) tex->blobSize);
    }
    if (tex->blobOffset == 0 || tex->blobSize == 0) {
        if (g->logBudget > 0) {
            fprintf(stderr, "[gles1] ensureTexture[%d]: blob coordinates are zero — external texture?\n", txtrIndex);
            g->logBudget--;
        }
        return 0;
    }

    // Read PNG blob from disk. This is a one-shot per atlas: blob is
    // typically 100-500 KB compressed.
    uint8_t* compressed = (uint8_t*) malloc(tex->blobSize);
    if (compressed == NULL) {
        if (g->logBudget > 0) {
            fprintf(stderr, "[gles1] ensureTexture[%d]: malloc(%u) failed\n",
                    txtrIndex, (unsigned) tex->blobSize);
            g->logBudget--;
        }
        return 0;
    }
    if (fseek(g->dataWinFile, (long) tex->blobOffset, SEEK_SET) != 0) {
        if (g->logBudget > 0) {
            fprintf(stderr, "[gles1] ensureTexture[%d]: fseek(%u) failed\n",
                    txtrIndex, (unsigned) tex->blobOffset);
            g->logBudget--;
        }
        free(compressed);
        return 0;
    }
    size_t got = fread(compressed, 1, tex->blobSize, g->dataWinFile);
    if (got != tex->blobSize) {
        if (g->logBudget > 0) {
            fprintf(stderr, "[gles1] ensureTexture[%d]: fread short: got %zu, wanted %u\n",
                    txtrIndex, got, (unsigned) tex->blobSize);
            g->logBudget--;
        }
        free(compressed);
        return 0;
    }
    if (g->logBudget > 0) {
        fprintf(stderr, "[gles1] ensureTexture[%d]: read %zu bytes, magic=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                txtrIndex, got,
                compressed[0], compressed[1], compressed[2], compressed[3],
                compressed[4], compressed[5], compressed[6], compressed[7]);
    }

    int w = 0, h = 0, ch = 0;
    stbi_uc* pixels = stbi_load_from_memory(compressed, (int) tex->blobSize, &w, &h, &ch, 4);
    free(compressed);
    if (pixels == NULL || w <= 0 || h <= 0) {
        if (g->logBudget > 0) {
            fprintf(stderr, "[gles1] ensureTexture[%d]: stbi decode failed: %s (w=%d h=%d)\n",
                    txtrIndex, stbi_failure_reason() ? stbi_failure_reason() : "(no reason)", w, h);
            g->logBudget--;
        }
        return 0;
    }
    if (g->logBudget > 0) {
        fprintf(stderr, "[gles1] ensureTexture[%d]: decoded %dx%d (ch=%d)\n", txtrIndex, w, h, ch);
        g->logBudget--;
    }

    // Make room.
    uint32_t newBytes = (uint32_t)(w * h * 4);
    if (g->residentBytes + newBytes > g->residentBudget) {
        gles1_evictLRU(g, newBytes);
    }

    GLuint handle = 0;
    glGenTextures(1, &handle);
    glBindTexture(GL_TEXTURE_2D, handle);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);

    slot->glHandle = handle;
    slot->width = w;
    slot->height = h;
    slot->lastUsedTick = g->frameTick;
    g->residentBytes += newBytes;

    if (outW != NULL) *outW = w;
    if (outH != NULL) *outH = h;
    return handle;
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

    int32_t texW = 0, texH = 0;
    GLuint handle = gles1_ensureTexture(g, it->texturePageId, &texW, &texH);
    if (handle == 0 || texW == 0 || texH == 0) return;

    // Determine source rectangle within the atlas.
    int32_t srcX = it->sourceX + (subX >= 0 ? subX : 0);
    int32_t srcY = it->sourceY + (subY >= 0 ? subY : 0);
    int32_t srcW = (subW > 0) ? subW : it->sourceWidth;
    int32_t srcH = (subH > 0) ? subH : it->sourceHeight;

    // targetX/Y/W/H: where this sub-image sits inside the sprite's
    // bounding box, after the atlas has trimmed away transparent edges.
    float ofsX = (float) it->targetX;
    float ofsY = (float) it->targetY;
    float dstW = (float) srcW;
    float dstH = (float) srcH;

    float u0 = (float) srcX / (float) texW;
    float v0 = (float) srcY / (float) texH;
    float u1 = (float)(srcX + srcW) / (float) texW;
    float v1 = (float)(srcY + srcH) / (float) texH;

    GLfloat verts[8];
    GLfloat uvs[8];
    gles1_buildSpriteQuad(
        dstX, dstY, dstW, dstH,
        originX - ofsX, originY - ofsY,
        xscale, yscale, angleDeg,
        u0, v0, u1, v1,
        verts, uvs);

    GLfloat rgba[4];
    unpackBGR(color, alpha, rgba);

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

    int32_t texW = 0, texH = 0;
    GLuint handle = gles1_ensureTexture(g, it->texturePageId, &texW, &texH);
    if (handle == 0 || texW == 0 || texH == 0) return;

    float u0 = (float) it->sourceX / (float) texW;
    float v0 = (float) it->sourceY / (float) texH;
    float u1 = (float)(it->sourceX + it->sourceWidth) / (float) texW;
    float v1 = (float)(it->sourceY + it->sourceHeight) / (float) texH;

    GLfloat verts[8] = { x1, y1, x2, y2, x4, y4, x3, y3 };
    GLfloat uvs[8]   = { u0, v0, u1, v0, u0, v1, u1, v1 };
    GLfloat rgba[4]  = { 1.0f, 1.0f, 1.0f, alpha };

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

static void gles1_drawText(Renderer* r, const char* text, float x, float y, float xs, float ys, float ang) {
    (void) r; (void) text; (void) x; (void) y; (void) xs; (void) ys; (void) ang;
}

static void gles1_drawTextColor(Renderer* r, const char* text, float x, float y, float xs, float ys, float ang, int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha) {
    (void) r; (void) text; (void) x; (void) y; (void) xs; (void) ys; (void) ang;
    (void) c1; (void) c2; (void) c3; (void) c4; (void) alpha;
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
