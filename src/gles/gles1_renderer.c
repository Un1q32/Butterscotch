#include "gles1_renderer.h"
#include "../utils.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Platform GLES 1.1 headers. iOS uses <OpenGLES/ES1/gl.h>, Android NDK
// uses <GLES/gl.h>. Pick based on what the platform main.* defines.
#if defined(__APPLE__)
    #include <OpenGLES/ES1/gl.h>
    #include <OpenGLES/ES1/glext.h>
#else
    #include <GLES/gl.h>
    #include <GLES/glext.h>
#endif

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
// Renderer vtable: lifecycle
// ============================================================================

static void gles1_init(Renderer* r, DataWin* dataWin) {
    r->dataWin = dataWin;
    r->drawColor = 0xFFFFFF;
    r->drawAlpha = 1.0f;
    r->drawFont = -1;
    r->drawHalign = 0;
    r->drawValign = 0;
    r->circlePrecision = 24;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
}

static void gles1_destroy(Renderer* r) {
    (void) r;
}

// ============================================================================
// Renderer vtable: frame + view + GUI passes
// ============================================================================

static void gles1_beginFrame(Renderer* r, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    (void) r; (void) gameW; (void) gameH;
    glViewport(0, 0, windowW, windowH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0.0f, (GLfloat) windowW, (GLfloat) windowH, 0.0f, -1.0f, 1.0f);
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
    (void) r; (void) tpagIndex; (void) x; (void) y; (void) ox; (void) oy; (void) xs; (void) ys; (void) ang; (void) color; (void) alpha;
}

static void gles1_drawSpritePart(Renderer* r, int32_t tpagIndex, int32_t sox, int32_t soy, int32_t sw, int32_t sh, float x, float y, float xs, float ys, float ang, float px, float py, uint32_t color, float alpha) {
    (void) r; (void) tpagIndex; (void) sox; (void) soy; (void) sw; (void) sh; (void) x; (void) y; (void) xs; (void) ys; (void) ang; (void) px; (void) py; (void) color; (void) alpha;
}

static void gles1_drawSpritePos(Renderer* r, int32_t tpagIndex, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float alpha) {
    (void) r; (void) tpagIndex; (void) x1; (void) y1; (void) x2; (void) y2; (void) x3; (void) y3; (void) x4; (void) y4; (void) alpha;
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
    Renderer* r = (Renderer*) calloc(1, sizeof(Renderer));
    if (!r) return NULL;
    r->vtable = &kGles1Vtable;
    return r;
}
