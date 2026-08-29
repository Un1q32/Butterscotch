#ifndef _BS_GL_RENDERER_H_
#define _BS_GL_RENDERER_H_

#include "common.h"
#include "renderer.h"
#include "runner.h"
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(__SWITCH__)
#include <GLES3/gl3.h>
#elif PLATFORM_VITA
#include <vitaGL.h>
#define GL_BOOL 0x8B56
#else
#include <glad/glad.h>
#endif

typedef enum {
    BATCHTYPE_QUAD,
    BATCHTYPE_TRIANGLE
} BatchType;

// ===[ GLRenderer Struct ]===
typedef struct {
    char* name; // owned
    int32_t location;
    GLenum type;
    uint32_t samplerSlot;
} GLShaderUniform;

typedef struct {
    GLuint shaderId;
    bool compiled;
    uint32_t uniformCount;
    GLShaderUniform* uniforms;

    // cached uniforms
    GLShaderUniform* gmBaseTexture;
    GLShaderUniform* gmMatrices;
    GLShaderUniform* gmFogColour;
    GLShaderUniform* gmAlphaTestEnabled;
    GLShaderUniform* gmAlphaRefValue;
} GMLShader;

typedef struct {
    float x, y, z;
    float u, v;
    uint8_t r, g, b, a;
} Vertex;

// Exposed in the header so platform-specific code (main.c) can access FBO fields for screenshots.
typedef struct {
    Renderer base; // Must be first field for struct embedding

    GMLShader* defaultShaderProgram;
    GMLShader* gmlShaders;
    uint32_t gmlShaderCount;

    bool alphaTestEnable;
    float alphaTestRef;
    bool colorWriteR, colorWriteG, colorWriteB, colorWriteA;
    bool fogEnable;
    uint32_t fogColor; // BGR

    GLuint vao, vbo, ebo;
    Vertex* vertexData; // MAX_QUADS * VERTICES_PER_QUAD vertices

    BatchType batchType;
    int32_t batchCount;
    GLuint currentTextureId;

    // On the desktop/ES path each entry is one extracted sub-region (a TPAG item) rather
    // than a whole TXTR page: we decode the owning page once and upload only the used
    // rectangle, so unused areas never occupy VRAM. textureCount == dataWin->tpag.count.
    // On PLATFORM_VITA (legacy optimized path) entries remain whole pages and textureCount
    // is the page count.
    GLuint* glTextures;       // one GL texture per TPAG item (or per page on Vita)
    int32_t* textureWidths;   // extracted texture pixel dims (TPAG sourceWidth/Height, or page dims on Vita)
    int32_t* textureHeights;
    bool* textureLoaded;      // lazy loading: true once decoded and uploaded
    uint32_t textureCount;

    // Desktop/ES single-entry decoded-page cache: the most recently decoded whole TXTR page
    // is kept in system RAM so that extracting additional TPAG sub-rectangles from the same
    // page reuses the decoded RGBA buffer instead of re-decoding. Evicted whenever a different
    // page is decoded. Not used on PLATFORM_VITA (page-based whole-page uploads).
    uint32_t textureCachePageId; // page whose decoded pixels are cached (UINT32_MAX == none)
    uint8_t* textureCachePixels; // full decoded RGBA buffer of that page (owned by the cache)
    int32_t textureCacheW;
    int32_t textureCacheH;

    GLuint whiteTexture; // 1x1 white pixel for drawing primitives (rectangles, lines, etc.)

    int32_t windowW; // stored from beginFrame for endFrame blit
    int32_t windowH;
    int32_t gameW; // game width (matches the application_surface size)
    int32_t gameH; // game height (matches the application_surface size)

    GLuint hostFramebuffer; // present target for the composited frame, where 0 == the window

    // Original counts from data.win (dynamic slots start at these indices)
    uint32_t originalTexturePageCount;
    uint32_t originalTpagCount;
    uint32_t originalSpriteCount;
    GLuint* surfaces;
    GLuint* surfaceTexture;
    int32_t* surfaceWidth;
    int32_t* surfaceHeight;
    uint32_t surfaceCount;

    // Blending mode + factors
    bool blendEnable;
    int32_t currentBlendMode;
    int32_t currentSFactor;
    int32_t currentDFactor;
    int32_t currentSFactorAlpha;
    int32_t currentDFactorAlpha;

    bool isGL3; // TRUE if running on OpenGL (ES) 3.x+
    bool isGLES;  // TRUE if running on OpenGL ES (GLES)

    // Cached default shader uniforms
    GLShaderUniform* uWorldViewProjection;
    GLShaderUniform* uFogColor;
    GLShaderUniform* uAlphaTestRef;
    GLShaderUniform* uAlphaTestEnabled;
    GLShaderUniform* uTexture;
} GLRenderer;

// Loads (decodes + uploads) a single texture. On the desktop/ES path `index` is a TPAG
// index and only that sub-region of its TXTR page is uploaded; on PLATFORM_VITA it is a
// page index and the whole page is uploaded. Returns true once the texture is ready.
bool GLRenderer_ensureTextureLoaded(GLRenderer* gl, uint32_t index);
Renderer* GLRenderer_create(void);

#endif /* _BS_GL_RENDERER_H_ */
