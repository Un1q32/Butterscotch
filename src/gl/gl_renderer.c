#include "gl_renderer.h"
#include "matrix_math.h"
#include "text_utils.h"

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(__SWITCH__)
#include <GLES3/gl3.h>
#elif PLATFORM_VITA
#include <vitaGL.h>
#include "vita_textures.h"
#else
#include <glad/glad.h>
#endif
#include <ctype.h>
#include "stdio_compat.h"
#include <stdlib.h>
#include "string_compat.h"
#include "math_compat.h"

#include "stb_image.h"
#include "stb_ds.h"
#include "utils.h"
#include "image_decoder.h"
#include "gl_common.h"
#include "gl_wrappers.h"

// ===[ Constants ]===
#define MAX_QUADS 4096
#define VERTICES_PER_TRIANGLE 3
#define VERTICES_PER_QUAD 4
#define INDICES_PER_QUAD 6

// ===[ Shader Sources ]===

static const char* baseVertexShader =
    "uniform mat4 uWorldViewProjection;\n"
    "void main() {\n"
    "    gl_Position = uWorldViewProjection * vec4(aPos, 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
    "    vColor = aColor;\n"
    "}\n";

static const char* baseFragmentShader =
    "uniform sampler2D uTexture;\n"
    "uniform float uAlphaTestRef;\n"
    "uniform bool uAlphaTestEnabled;\n"
    "uniform vec4 uFogColor;\n"
    "void main() {\n"
    "    vec4 c = TEXTURE_2D(uTexture, vTexCoord) * vColor;\n"
    "    if (uAlphaTestEnabled) {\n"
    "        if (uAlphaTestRef >= c.a) discard;\n"
    "    }\n"
    "    c.rgb = mix(c.rgb, uFogColor.rgb, uFogColor.a);\n"
    "    FRAG_COLOR = c;\n"
    "}\n";

// ===[ Runtime OpenGL extension checks ]===

static bool hasFBO() {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(__VITA__) && !defined(__SWITCH__)
    return glGenFramebuffers;
#else
    return true;
#endif
}

static bool hasVAO() {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(__VITA__) && !defined(__SWITCH__)
    return glGenVertexArrays;
#else
    return true;
#endif
}

static inline uint8_t floatToUnormByte(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    return (uint8_t)(v * 255.0f + 0.5f);
}

// ===[ Shader Compilation ]===

#ifdef PLATFORM_VITA
// replaces every instance of (thing *= blahblah) with (thing = thing * (blahblah).)
// vitaGL doesn't support the operator *= so...
// this assumes that every line ends with ; so its a bit finicky
char *fixShaderForVita(const char *src) {
    size_t src_len = strlen(src);
    size_t cap = src_len * 2 + 64;
    char *out = malloc(cap);
    size_t out_len = 0;
    if (!out) return NULL;

    #define ENSURE(n) do { \
    if (out_len + (size_t)(n) + 1 > cap) { \
        cap = (out_len + (size_t)(n) + 1) * 2; \
        char *tmp = realloc(out, cap); \
        if (!tmp) { free(out); return NULL; } \
            out = tmp; \
    } \
    } while (0)

    #define APPEND(s, n) do { ENSURE(n); memcpy(out + out_len, (s), (n)); out_len += (n); } while (0)
    #define APPEND_STR(s) APPEND((s), strlen(s))
    #define APPEND_CH(c) do { ENSURE(1); out[out_len++] = (char)(c); } while (0)

    size_t i = 0;
    while (i < src_len) {
        char c = src[i];
        if (c == '/' && i + 1 < src_len && src[i + 1] == '/') {
            size_t start = i;
            while (i < src_len && src[i] != '\n') i++;
            APPEND(src + start, i - start);
            continue;
        }

        if (c == '/' && i + 1 < src_len && src[i + 1] == '*') {
            size_t start = i;
            i += 2;
            while (i + 1 < src_len && !(src[i] == '*' && src[i + 1] == '/')) i++;
            i = (i + 1 < src_len) ? i + 2 : src_len;
            APPEND(src + start, i - start);
            continue;
        }

        if (isalpha((unsigned char)c) || c == '_') {
            size_t id_start = i;
            size_t j = i;

            while (j < src_len && (isalnum((unsigned char)src[j]) || src[j] == '_')) j++;

            size_t k = j;
            for (;;) {
                size_t save = k;
                while (k < src_len && isspace((unsigned char)src[k])) k++;

                if (k < src_len && src[k] == '.') {
                    size_t after_dot = k + 1;
                    while (after_dot < src_len && isspace((unsigned char)src[after_dot])) after_dot++;
                    if (after_dot < src_len && (isalpha((unsigned char)src[after_dot]) || src[after_dot] == '_')) {
                        k = after_dot;
                        while (k < src_len && (isalnum((unsigned char)src[k]) || src[k] == '_')) k++;
                        continue;
                    }
                    k = save;
                    break;
                } else if (k < src_len && src[k] == '[') {
                    int depth = 1;
                    size_t m = k + 1;
                    while (m < src_len && depth > 0) {
                        if (src[m] == '[') depth++;
                        else if (src[m] == ']') depth--;
                        m++;
                    }
                    if (depth == 0) { k = m; continue; }
                    k = save;
                    break;
                } else {
                    k = save;
                    break;
                }
            }

            size_t m = k;
            while (m < src_len && isspace((unsigned char)src[m])) m++;

            if (m + 1 < src_len && src[m] == '*' && src[m + 1] == '=' &&
                !(m + 2 < src_len && src[m + 2] == '=')) {

                size_t rhs_start = m + 2;
            size_t p = rhs_start;
            int depth = 0;
            while (p < src_len) {
                char rc = src[p];
                if (rc == '(' || rc == '[') depth++;
                else if (rc == ')' || rc == ']') depth--;
                else if (rc == ';' && depth == 0) break;
                p++;
            }

            if (p < src_len && src[p] == ';') {
                size_t rhs_end = p;
                while (rhs_end > rhs_start && isspace((unsigned char)src[rhs_end - 1])) rhs_end--;
                size_t rhs_s = rhs_start;
                while (rhs_s < rhs_end && isspace((unsigned char)src[rhs_s])) rhs_s++;

                APPEND(src + id_start, k - id_start);   /* lvalue */
                APPEND_STR(" = ");
                APPEND(src + id_start, k - id_start);   /* lvalue again */
                APPEND_STR(" * (");
                APPEND(src + rhs_s, rhs_end - rhs_s);   /* expr */
                APPEND_CH(')');
                APPEND_CH(';');

                i = p + 1;
                continue;
            }
                }
        }

        APPEND_CH(c);
        i++;
    }

    ENSURE(0);
    out[out_len] = '\0';
    return out;

    #undef ENSURE
    #undef APPEND
    #undef APPEND_STR
    #undef APPEND_CH
}
#endif

static GLuint compileShader(GLenum type, const char* source, bool* ok) {
    #ifdef PLATFORM_VITA
    const char* actualSource = fixShaderForVita(source);
    #else
    const char* actualSource = source;
    #endif
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &actualSource, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
        logError("GL: Shader compilation failed: %s\n", infoLog);
        *ok = false;
        return 0;
    }
    *ok = true;
    #ifdef PLATFORM_VITA
    free((char*)actualSource);
    #endif
    return shader;
}

static GLuint linkProgram(const char* name, uint32_t vertexAttributeCount, const char** vertexAttributes, GLuint vertShader, GLuint fragShader, bool *success2) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vertShader);
    glAttachShader(program, fragShader);

    repeat(vertexAttributeCount, i) {
        glBindAttribLocation(program, i, vertexAttributes[i]);
    }

    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
        logError("GL: Shader %s linking failed: %s\n", name, infoLog);
        *success2 = false;
    } else {
        *success2 = true;
        logInfo("GL: Shader %s succesfully linked!\n", name);
    }
    return program;
}

static GLShaderUniform* getShaderUniform(GMLShader* shader, const char* name, GLenum type) {
    GLint location = glGetUniformLocation(shader->shaderId, name);
    if (location < 0) return NULL;

    GLShaderUniform* uniform = (GLShaderUniform*) safeCalloc(1, sizeof(GLShaderUniform));
    uniform->location = location;
    uniform->type = type;
    return uniform;
}

// ===[ Batch Flush ]===

static void flushBatch(GLRenderer* gl) {
    if (gl->batchCount == 0) return;

    if (gl->base.currentShader != -1) {
        GMLShader* shader = &gl->gmlShaders[gl->base.currentShader];

        GLShaderUniform* uniform = shader->gmBaseTexture;
        if (uniform != nullptr)
            glActiveTexture(GL_TEXTURE0 + uniform->samplerSlot);
        glBindTexture(GL_TEXTURE_2D, gl->currentTextureId);
    } else {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, gl->currentTextureId);
    }

    int32_t indexCount = gl->batchCount * INDICES_PER_QUAD;

    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    int32_t totalVboSize = MAX_QUADS * VERTICES_PER_QUAD * sizeof(Vertex);
#ifdef PLATFORM_VITA
    vglBufferData(GL_ARRAY_BUFFER, (void*)gl->vertexData);
    gl->vertexData = (Vertex*)vglAllocFromScratch((size_t)totalVboSize);
    //glBufferData(GL_ARRAY_BUFFER, totalVboSize, (void*)gl->vertexData, GL_DYNAMIC_DRAW);
#else
    int32_t singleVertexCount = (gl->batchType == BATCHTYPE_QUAD) ? VERTICES_PER_QUAD : VERTICES_PER_TRIANGLE;
    int32_t vertexCount = gl->batchCount * singleVertexCount;
    glBufferData(GL_ARRAY_BUFFER, totalVboSize, nullptr, GL_DYNAMIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vertexCount * sizeof(Vertex), gl->vertexData);
#endif


    if (hasVAO()) {
        glBindVertexArray(gl->vao);
    } else {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->ebo);

        int32_t stride = sizeof(Vertex);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*) offsetof(Vertex, x));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*) offsetof(Vertex, r));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*) offsetof(Vertex, u));
        glEnableVertexAttribArray(2);
    }

    if (gl->batchType == BATCHTYPE_QUAD) {
        glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT, nullptr);
    } else if (gl->batchType == BATCHTYPE_TRIANGLE) {
        glDrawArrays(GL_TRIANGLES, 0, gl->batchCount * VERTICES_PER_TRIANGLE);
    }

    if (!hasVAO()) {
        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
        glDisableVertexAttribArray(2);
    }

    gl->batchCount = 0;
}

static void flushIfNeededAndSetActiveState(GLRenderer* gl, BatchType batchType, GLuint textureId) {
    if (gl->batchCount != 0) {
        // TODO: This should be changed down the road from MAX_QUADS to MAX_WHATEVER_BATCH_TYPE_ARE_WE_USING
        if (gl->batchType != batchType || gl->currentTextureId != textureId || gl->batchCount == MAX_QUADS) {
            flushBatch(gl);
        }
    }

    gl->batchType = batchType;
    gl->currentTextureId = textureId;
}

// ===[ Vtable Implementations ]===

static bool compileProgram(GMLShader* gmlShader, const char* name, const char* vertexShaderSource, const char* fragmentShaderSource, uint32_t vertexAttributeCount, const char** vertexAttributes) {
    logInfo("GL: Compiling %s vertex shader\n", name);
    bool vertexShaderOK = false;
    bool fragmentShaderOK = false;
    GLuint vertShaderT = compileShader(GL_VERTEX_SHADER, vertexShaderSource, &vertexShaderOK);
    if (!vertexShaderOK) {
        logError("GL: Failed to compile %s vertex shader!\n", name);
        return false;
    }
    logInfo("GL: Compiling %s fragment shader\n", name);
    GLuint fragShaderT = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSource, &fragmentShaderOK);
    if (!fragmentShaderOK) {
        logError("GL: Failed to compile %s fragment shader!\n", name);
        return false;
    }

    bool success;
    GLuint shaderId = linkProgram(name, vertexAttributeCount, vertexAttributes, vertShaderT, fragShaderT, &success);
    glDeleteShader(vertShaderT);
    glDeleteShader(fragShaderT);
    //Texture Set Stage BS has to be done bruh :(
    int32_t samplerIndex = 0;
    GLint uniformCount;
    glGetProgramiv(shaderId, GL_ACTIVE_UNIFORMS, &uniformCount);

    gmlShader->uniformCount = uniformCount;
    gmlShader->uniforms = (GLShaderUniform *)safeCalloc(uniformCount, sizeof(GLShaderUniform));

    // We can only get the length of a specific uniform in OpenGL 4.3+...
    GLint maxUniformNameLength = 0;
    glGetProgramiv(shaderId, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxUniformNameLength);

    repeat(uniformCount, b) {
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;

        char* uniformName = (char *)safeMalloc(maxUniformNameLength + 1);

        glGetActiveUniform(shaderId, b, maxUniformNameLength, &length, &size, &type, uniformName);

        gmlShader->uniforms[b].location = glGetUniformLocation(shaderId, uniformName);
        gmlShader->uniforms[b].name = uniformName;
        gmlShader->uniforms[b].type = type;

        if (type == GL_SAMPLER_2D) {
            glUseProgram(shaderId);
            glUniform1i(gmlShader->uniforms[b].location, samplerIndex);
            gmlShader->uniforms[b].samplerSlot = samplerIndex;
            samplerIndex += 1;
        }

        if (strcmp(uniformName, "gm_BaseTexture") == 0)
            gmlShader->gmBaseTexture = &gmlShader->uniforms[b];
#ifdef PLATFORM_VITA
        if (strcmp(uniformName, "gm_Matrices") == 0)
#else
        if (strcmp(uniformName, "gm_Matrices[0]") == 0)
#endif
            gmlShader->gmMatrices = &gmlShader->uniforms[b];
        if (strcmp(uniformName, "gm_FogColour") == 0)
            gmlShader->gmFogColour = &gmlShader->uniforms[b];
        if (strcmp(uniformName, "gm_AlphaTestEnabled") == 0)
            gmlShader->gmAlphaTestEnabled = &gmlShader->uniforms[b];
        if (strcmp(uniformName, "gm_AlphaRefValue") == 0)
            gmlShader->gmAlphaRefValue = &gmlShader->uniforms[b];
    }

    gmlShader->shaderId = shaderId;
    gmlShader->compiled = true;
    return true;
}

static void glInit(Renderer* renderer, DataWin* dataWin) {
    GLRenderer* gl = (GLRenderer*) renderer;
    renderer->dataWin = dataWin;

    Matrix4f world;
    Matrix4f_identity(&world);
    renderer->gmlMatrices[MATRIX_WORLD] = world;

    GMLShader* defaultShader = (GMLShader*)safeCalloc(1, sizeof(GMLShader));
    GLVer ver = GLCommon_getGLVersion();
    if (ver.major < 2) {
        logError("GL: The modern-gl renderer requires OpenGL 2.0 or newer\n");
        abort();
    }
    gl->isGL3 = (ver.major >= 3);
    gl->isGLES = ver.isGLES;

#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(PLATFORM_VITA) && !defined(__SWITCH__)
    gl_init_wrappers();
#endif

    if (!hasFBO()) {
        logError("GL: The modern-gl renderer requires FBO support\n");
        abort();
    }

    // ===[ CPU-side decoded-page cache ]===
    // When enabled, recently decoded TXTR pages are kept in system RAM so that other
    // TPAG items on the same page (pageless mode) or re-loads after GPU eviction
    // (paged mode) don't have to re-decode the PNG data.  The cache is a flat array;
    // on a miss the oldest entry is evicted.  A size of 0 disables caching entirely.
    if (gl->pageCacheSize > 0) {
        gl->txtrPageCache = (PageCacheEntry *)safeMalloc(gl->pageCacheSize * sizeof(PageCacheEntry));
        repeat(gl->pageCacheSize, i) {
            gl->txtrPageCache[i].pageId  = UINT32_MAX; // empty slot
            gl->txtrPageCache[i].width   = 0;
            gl->txtrPageCache[i].height  = 0;
            gl->txtrPageCache[i].pixels  = nullptr;
        }
        logInfo("GL: Page cache enabled (%zu entries)\n", gl->pageCacheSize);
    }

    char vertSrc[1024];
    char fragSrc[1024];
    const char* vertHeader = "";
    const char* fragHeader = "";

    if (gl->isGL3) {
        if (gl->isGLES) {
            vertHeader = "#version 300 es\nprecision highp float;\n";
            fragHeader = "#version 300 es\nprecision mediump float;\n";

            snprintf(vertSrc, sizeof(vertSrc),
                "%s"
                "layout(location = 0) in vec2 aPos;\nlayout(location = 1) in vec4 aColor;\nlayout(location = 2) in vec2 aTexCoord;\n"
                "out vec2 vTexCoord;\nout vec4 vColor;\n%s",
                vertHeader, baseVertexShader);
        } else {
            vertHeader = "#version 130\n";
            fragHeader = "#version 130\n";

            snprintf(vertSrc, sizeof(vertSrc),
                "%s"
                "in vec2 aPos;\nin vec4 aColor;\nin vec2 aTexCoord;\n"
                "out vec2 vTexCoord;\nout vec4 vColor;\n%s",
                vertHeader, baseVertexShader);
        }

        snprintf(fragSrc, sizeof(fragSrc),
            "%s"
            "in vec2 vTexCoord;\nin vec4 vColor;\nout vec4 fragColor;\n"
            "#define TEXTURE_2D texture\n#define FRAG_COLOR fragColor\n%s",
            fragHeader, baseFragmentShader);
    } else {
        if (gl->isGLES) {
            vertHeader = "#version 100\nprecision highp float;\n";
            fragHeader = "#version 100\nprecision mediump float;\n";
        } else {
            vertHeader = "#version 110\n";
            fragHeader = "#version 110\n";
        }

        snprintf(vertSrc, sizeof(vertSrc),
            "%s"
            "attribute vec2 aPos;\nattribute vec4 aColor;\nattribute vec2 aTexCoord;\n"
            "varying vec2 vTexCoord;\nvarying vec4 vColor;\n%s",
            vertHeader, baseVertexShader);

        snprintf(fragSrc, sizeof(fragSrc),
            "%s"
            "varying vec2 vTexCoord;\nvarying vec4 vColor;\n"
            "#define TEXTURE_2D texture2D\n#define FRAG_COLOR gl_FragColor\n%s",
            fragHeader, baseFragmentShader);
    }

    const char* defaultAttributes[] = { "aPos", "aColor", "aTexCoord" };
    bool success = compileProgram(defaultShader, "default", vertSrc, fragSrc, 3, defaultAttributes);
    if (!success) {
        logError("GL: Failed to compile default shaders! Bailing...\n");
        abort();
    }

    gl->defaultShaderProgram = defaultShader;

    gl->uWorldViewProjection = getShaderUniform(defaultShader, "uWorldViewProjection", GL_FLOAT_MAT4);
    gl->uFogColor            = getShaderUniform(defaultShader, "uFogColor",            GL_FLOAT_VEC4);
    gl->uAlphaTestRef        = getShaderUniform(defaultShader, "uAlphaTestRef",        GL_FLOAT);
    gl->uAlphaTestEnabled    = getShaderUniform(defaultShader, "uAlphaTestEnabled",    GL_BOOL);
    gl->uTexture             = getShaderUniform(defaultShader, "uTexture",             GL_SAMPLER_2D);

    gl->gmlShaders = (GMLShader *)safeCalloc(dataWin->shdr.count, sizeof(GMLShader));
    logInfo("GL: %u Shaders Found\n", dataWin->shdr.count);

    repeat(dataWin->shdr.count, i) {
        Shader* shdr = &dataWin->shdr.shaders[i];
        GMLShader* gmlShader = &gl->gmlShaders[i];

        if (!shdr->present) {
            gl->gmlShaderCount++;
            logWarn("GL: Skipping shader %d because it isn't present!\n", (int)i);
            continue;
        }

        logInfo("GL: Compiling %s\n", shdr->name);

        const char* vertexShaderSource = gl->isGLES ? shdr->glslES_Vertex : shdr->glsl_Vertex;
        const char* fragmentShaderSource = gl->isGLES ? shdr->glslES_Fragment : shdr->glsl_Fragment;

        char* patchedVertexSource = nullptr;
        char* patchedFragmentSource = nullptr;

        if (!gl->isGLES && ver.major == 2 && ver.minor == 0) { // super opengl 2.0 fuckery go go
            if (vertexShaderSource && strstr(vertexShaderSource, "#version 120")) {
                patchedVertexSource = safeStrdup(vertexShaderSource);
                char* loc = strstr(patchedVertexSource, "#version 120");
                if (loc) loc[10] = '1';
                vertexShaderSource = patchedVertexSource;
            }
            if (fragmentShaderSource && strstr(fragmentShaderSource, "#version 120")) {
                patchedFragmentSource = safeStrdup(fragmentShaderSource);
                char* loc = strstr(patchedFragmentSource, "#version 120");
                if (loc) loc[10] = '1';
                fragmentShaderSource = patchedFragmentSource;
            }
        }

        compileProgram(
            gmlShader,
            shdr->name,
            vertexShaderSource,
            fragmentShaderSource,
            shdr->vertexAttributeCount,
            shdr->vertexAttributes
        );

        if (patchedVertexSource) free(patchedVertexSource);
        if (patchedFragmentSource) free(patchedFragmentSource);

        gl->gmlShaderCount++;
    }
    GLShaderUniform* uAlphaTestRef = getShaderUniform(gl->defaultShaderProgram, "uAlphaTestRef", GL_FLOAT);
    GLShaderUniform* uFogColor     = getShaderUniform(gl->defaultShaderProgram, "uFogColor",     GL_FLOAT_VEC4);

    gl->alphaTestEnable = false;
    gl->alphaTestRef = 0.0f;
    gl->colorWriteR = true;
    gl->colorWriteG = true;
    gl->colorWriteB = true;
    gl->colorWriteA = true;
    gl->fogEnable = false;
    gl->fogColor = 0;
    glUseProgram(gl->defaultShaderProgram->shaderId);
    glUniform1f(uAlphaTestRef->location, -1.0f);
    glUniform4f(uFogColor->location, 0.0f, 0.0f, 0.0f, 0.0f);
    free(uAlphaTestRef);
    free(uFogColor);

    // Create VAO/VBO/EBO
    if (hasVAO()) {
        glGenVertexArrays(1, &gl->vao);
        glBindVertexArray(gl->vao);
    }
    glGenBuffers(1, &gl->vbo);
    glGenBuffers(1, &gl->ebo);

    // VBO: sized for max quads
    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
#ifndef PLATFORM_VITA // We don't really need to warm up the buffer since we have scratch memory on VitaGL...
    int32_t vboSize = MAX_QUADS * VERTICES_PER_QUAD * sizeof(Vertex);
    glBufferData(GL_ARRAY_BUFFER, vboSize, nullptr, GL_DYNAMIC_DRAW);
#endif

    int32_t eboSize = MAX_QUADS * INDICES_PER_QUAD * sizeof(uint16_t);
    uint16_t* indices = (uint16_t*)safeMalloc(eboSize);
    for (int32_t i = 0; MAX_QUADS > i; i++) {
        uint16_t base = i * 4;
        indices[i*6+0] = base+0; indices[i*6+1] = base+1; indices[i*6+2] = base+2;
        indices[i*6+3] = base+2; indices[i*6+4] = base+3; indices[i*6+5] = base+0;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, eboSize, indices, GL_STATIC_DRAW);
    free(indices);

    if (hasVAO()) {
        // Vertex attributes: pos(2f), texcoord(2f), color(4f)
        int32_t stride = sizeof(Vertex);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*) offsetof(Vertex, x));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void*) offsetof(Vertex, r));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*) offsetof(Vertex, u));
        glEnableVertexAttribArray(2);
        glBindVertexArray(0);
    }

    // Allocate CPU-side vertex buffer
#if PLATFORM_VITA
    gl->vertexData = (Vertex *)vglAllocFromScratch(MAX_QUADS * VERTICES_PER_QUAD * sizeof(Vertex));
#else
    gl->vertexData = (Vertex *)safeMalloc(MAX_QUADS * VERTICES_PER_QUAD * sizeof(Vertex));
#endif

    // ===[ Allocate texture arrays ]===
    // The size depends on the current texture loading mode:
    //   - VitaTextures active: use VitaTextures page count (external compressed format)
    //   - pagelessTextures == true (pageless mode): tpag.count – one GL texture per TPAG
    //   - pagelessTextures == false (paged mode): txtr.count – one GL texture per TXTR page
#if defined(PLATFORM_VITA)
    if (VitaTextures_Active()) {
        gl->textureCount = VitaTextures_GetPageCount();
    } else
#endif
    if (gl->pagelessTextures) {
        // Pageless: one GL texture per TPAG item (the extracted sub-rectangle).
        gl->textureCount = dataWin->tpag.count;
    } else {
        // Paged: one GL texture per TXTR page (the whole decoded page).
        gl->textureCount = dataWin->txtr.count;
    }
    gl->glTextures = (GLuint *)safeMalloc(gl->textureCount * sizeof(GLuint));
    gl->textureWidths = (int32_t *)safeMalloc(gl->textureCount * sizeof(int32_t));
    gl->textureHeights = (int32_t *)safeMalloc(gl->textureCount * sizeof(int32_t));
    gl->textureLoaded = (bool *)safeMalloc(gl->textureCount * sizeof(bool));

    glGenTextures((GLsizei) gl->textureCount, gl->glTextures);

    for (uint32_t i = 0; gl->textureCount > i; i++) {
        gl->textureWidths[i] = 0;
        gl->textureHeights[i] = 0;
        gl->textureLoaded[i] = false;
    }

    // Create 1x1 white pixel texture for primitive drawing (rectangles, lines, etc.)
    glGenTextures(1, &gl->whiteTexture);
    glBindTexture(GL_TEXTURE_2D, gl->whiteTexture);
    uint8_t whitePixel[4] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, whitePixel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); //I believe the old way this was done was wrong
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Enable blending
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    gl->batchCount = 0;
    gl->currentTextureId = 0;

    // Save original counts so we know which slots are from data.win vs dynamic.
    // Dynamic sprites created at runtime get slots beyond these boundaries.
    gl->originalTexturePageCount = gl->textureCount;
    gl->originalTpagCount = dataWin->tpag.count;
    gl->originalSpriteCount = dataWin->sprt.count;

    logInfo("GL: Renderer initialized (%u %s)\n", gl->textureCount,
            gl->pagelessTextures ? "textures (pageless)" : "texture pages");
}

static void glGpuSetShader(Renderer* renderer, int32_t shaderIndex) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);
    GMLShader* gmlShader = &gl->gmlShaders[shaderIndex];

    glUseProgram(gmlShader->shaderId);
    //Gotta set those built-ins! they ain't gonna set themselves
    GLShaderUniform* gmMatricesUniform = gmlShader->gmMatrices;
    GLShaderUniform* gmFogColourUniform = gmlShader->gmFogColour;

    //Lights are for another time

    GLShaderUniform* gmAlphaTestEnabledUniform = gmlShader->gmAlphaTestEnabled;
    GLShaderUniform* gmAlphaRefValue = gmlShader->gmAlphaRefValue;

    Matrix4f flippedClip[MATRICES_MAX];
    memcpy(flippedClip, renderer->gmlMatrices, sizeof(flippedClip));

    Matrix4f_flipClipY(&flippedClip[MATRIX_PROJECTION]);
    Matrix4f_flipClipY(&flippedClip[MATRIX_WORLD_VIEW_PROJECTION]);

    if (gmMatricesUniform != nullptr) {
        glUniformMatrix4fv(gmMatricesUniform->location, 5, GL_FALSE, flippedClip[0].m);
    }
    if (gmFogColourUniform != nullptr) {
        glUniform1i(gmFogColourUniform->location, gl->fogColor);
    }
    if (gmAlphaTestEnabledUniform != nullptr) {
        glUniform1i(gmAlphaTestEnabledUniform->location, gl->alphaTestEnable);
    }
    if (gmAlphaRefValue != nullptr) {
        glUniform1f(gmAlphaRefValue->location, gl->alphaTestRef);
    }

    renderer->currentShader = shaderIndex;
}

static void glShaderSettingsRefresh(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);
    if (renderer->currentShader != -1) {
        glGpuSetShader(renderer, (int32_t) renderer->currentShader);
    } else {
        float fogR = (float) BGR_R(gl->fogColor) / 255.0f;
        float fogG = (float) BGR_G(gl->fogColor) / 255.0f;
        float fogB = (float) BGR_B(gl->fogColor) / 255.0f;

        glUseProgram(gl->defaultShaderProgram->shaderId);

        Matrix4f flippedClip[MATRICES_MAX];
        memcpy(flippedClip, renderer->gmlMatrices, sizeof(flippedClip));
        //I was making the Legacy OpenGL renderer work with the projections, then I realized I think I only need to flip the Projection(s) and not the other ones
        Matrix4f_flipClipY(&flippedClip[MATRIX_PROJECTION]);
        Matrix4f_flipClipY(&flippedClip[MATRIX_WORLD_VIEW_PROJECTION]);

        glUniformMatrix4fv(gl->uWorldViewProjection->location, 1, GL_FALSE, flippedClip[MATRIX_WORLD_VIEW_PROJECTION].m);
        glUniform4f(gl->uFogColor->location, fogR, fogG, fogB, gl->fogEnable ? 1.0f : 0.0f);
        glUniform1f(gl->uAlphaTestRef->location, gl->alphaTestRef);
        glUniform1i(gl->uAlphaTestEnabled->location, gl->alphaTestEnable);
        glUniform1i(gl->uTexture->location, 1);
    }
}

// camera_apply: swap the active world->clip projection on the current target without touching its viewport.
static void glApplyProjection(Renderer* renderer, const Matrix4f* viewMatrix,const Matrix4f* projectionMatrix) {
    GLRenderer* gl = (GLRenderer*) renderer;

    // Flush first so pending quads draw under the projection they were issued with.
    flushBatch(gl);

    Matrix4f world = renderer->gmlMatrices[MATRIX_WORLD];
    Matrix4f view = *viewMatrix;
    Matrix4f projection = *projectionMatrix;

    Matrix4f worldView;
    Matrix4f_multiply(&worldView, &view, &world);

    Matrix4f worldViewProjection;
    Matrix4f_multiply(&worldViewProjection, &projection, &worldView);

    renderer->gmlMatrices[MATRIX_VIEW] = view;
    renderer->gmlMatrices[MATRIX_PROJECTION] = projection;
    renderer->gmlMatrices[MATRIX_WORLD_VIEW] = worldView;
    renderer->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION] = worldViewProjection;
    //oh my I hope it's good enough.
    glShaderSettingsRefresh(renderer);
}

static void glGpuResetShader(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);
    glUseProgram(gl->defaultShaderProgram->shaderId);
    renderer->currentShader = -1;
    glShaderSettingsRefresh(renderer);
}

static void freeShader(GMLShader* shader) {
    if (shader->compiled)
        glDeleteProgram(shader->shaderId);

    repeat(shader->uniformCount, i) {
        free(shader->uniforms[i].name);
    }
    free(shader->uniforms);
}

static void glDestroy(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;

    glDeleteTextures(1, &gl->whiteTexture);

    repeat(gl->gmlShaderCount, i) {
        freeShader(&gl->gmlShaders[i]);
    }

    free(gl->gmlShaders);

    repeat(gl->surfaceCount, i) {
        if (gl->surfaceTexture[i] != 0) glDeleteTextures(1, &gl->surfaceTexture[i]);
        if (gl->surfaces[i] != 0) glDeleteFramebuffers(1, &gl->surfaces[i]);
    }
    free(gl->surfaces);
    free(gl->surfaceTexture);
    free(gl->surfaceWidth);
    free(gl->surfaceHeight);

    freeShader(gl->defaultShaderProgram);
    free(gl->defaultShaderProgram);
    glDeleteTextures((GLsizei) gl->textureCount, gl->glTextures);
    if (hasVAO()) glDeleteVertexArrays(1, &gl->vao);
    glDeleteBuffers(1, &gl->vbo);
    glDeleteBuffers(1, &gl->ebo);

    free(gl->glTextures);
    free(gl->textureWidths);
    free(gl->textureHeights);
    free(gl->textureLoaded);

    // Free the CPU-side decoded-page cache
    if (gl->txtrPageCache != nullptr) {
        repeat(gl->pageCacheSize, i) {
            free(gl->txtrPageCache[i].pixels);
        }
        free(gl->txtrPageCache);
    }

    free(gl->uWorldViewProjection);
    free(gl->uFogColor);
    free(gl->uAlphaTestRef);
    free(gl->uAlphaTestEnabled);
    free(gl->uTexture);
#ifndef PLATFORM_VITA
    free(gl->vertexData);
#endif
    free(gl);
}

static void glBeginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH) {
    GLRenderer* gl = (GLRenderer*) renderer;

    gl->batchCount = 0;
    gl->currentTextureId = 0;
    gl->windowW = windowW;
    gl->windowH = windowH;
    gl->gameW = gameW;
    gl->gameH = gameH;

    // Bind the application_surface
    int32_t appId = gl->base.runner->applicationSurfaceId;
    glBindFramebuffer(GL_FRAMEBUFFER, gl->surfaces[appId]);
    glViewport(0, 0, gameW, gameH);
    gl->base.CPortX = 0;
    gl->base.CPortY = 0;
    gl->base.CPortW = gameW;
    gl->base.CPortH = gameH;
}

static void glBeginView(Renderer* renderer, MAYBE_UNUSED int32_t viewX, MAYBE_UNUSED int32_t viewY, MAYBE_UNUSED int32_t viewW, MAYBE_UNUSED int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH, MAYBE_UNUSED float viewAngle) {
    GLRenderer* gl = (GLRenderer*) renderer;
    gl->batchCount = 0;
    gl->currentTextureId = 0;

    // Set viewport and scissor to the port rectangle within the FBO
    // FBO uses game resolution, port coordinates are in game space
    // OpenGL viewport Y is bottom-up, game Y is top-down

    glViewport(portX, portY, portW, portH);

    gl->base.CPortX = portX;
    gl->base.CPortY = portY;
    gl->base.CPortW = portW;
    gl->base.CPortH = portH;

    glEnable(GL_SCISSOR_TEST);
    glScissor(portX, portY, portW, portH);

    int32_t viewCurrent = 0;
    if (renderer->runner->viewsEnabled) {
    viewCurrent = renderer->runner->viewCurrent;
    }
    RuntimeView* view = &renderer->runner->views[viewCurrent];
    gl->base.cameraCurrent = view->cameraId;
    GMLCamera* camera = Runner_getCameraById(renderer->runner, gl->base.cameraCurrent);
    glApplyProjection(renderer,&camera->viewMatrix,&camera->projectionMatrix);
    glActiveTexture(GL_TEXTURE1);

    if (hasVAO()) glBindVertexArray(gl->vao);

}

static void glEndView(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);
    glDisable(GL_SCISSOR_TEST);
}

static void glBeginGUI(Renderer* renderer, MAYBE_UNUSED int32_t guiW, MAYBE_UNUSED int32_t guiH, int32_t portX, int32_t portY, int32_t portW, MAYBE_UNUSED int32_t portH, int32_t targetSurfaceId) {
    GLRenderer* gl = (GLRenderer*) renderer;

    gl->batchCount = 0;
    gl->currentTextureId = 0;

    if (targetSurfaceId == RENDER_TARGET_HOST_FRAMEBUFFER) {
        glBindFramebuffer(GL_FRAMEBUFFER, gl->hostFramebuffer);
        glViewport(0, 0, portW, portH);
        glScissor(0, 0, portW, portH);
    } else {
        require(targetSurfaceId >= 0 && (uint32_t) targetSurfaceId < gl->surfaceCount);
        require(gl->surfaces[targetSurfaceId] != 0);
        glBindFramebuffer(GL_FRAMEBUFFER, gl->surfaces[targetSurfaceId]);
        int32_t glPortY = gl->gameH - portY - portH;
        glViewport(portX, glPortY, portW, portH);
        glScissor(portX, glPortY, portW, portH);
    }

    glEnable(GL_SCISSOR_TEST);
    //I dunno hopefully this is at least somewhat correct...
    gl->base.cameraCurrent = GUI_CAMERA;
    GMLCamera* camera = &renderer->runner->guiCamera;
    camera->allocated = true;
    camera->viewX = 0.0;
    camera->viewY = 0.0;
    camera->viewWidth = guiW;
    camera->viewHeight = guiH;
    camera->borderX = 0;
    camera->borderY = 0;
    camera->speedX = 0;
    camera->speedY = 0;
    camera->objectId = -1;
    camera->viewAngle = 0;

    Matrix4f projectionMatrix;
    Matrix4f_Orthographic(&projectionMatrix, (float) guiW, (float) guiH, 32000.0, 0.0);

    Matrix4f viewMatrix;
    float x = (float) guiW * 0.5f;
    float y = (float) guiH * 0.5f;
    Matrix4f_identity(&viewMatrix);
    Matrix4f_LookAt(&viewMatrix, x, y, -16000.0, x, y, 16000.0, 0.0, 1.0, 0.0);
    camera->viewMatrix = viewMatrix;
    camera->projectionMatrix = projectionMatrix;
    glApplyProjection(renderer,&camera->viewMatrix,&camera->projectionMatrix);


    glActiveTexture(GL_TEXTURE1);

    if (hasVAO()) glBindVertexArray(gl->vao);
}

static void glSetGuiProjection(Renderer* renderer, int32_t guiW, int32_t guiH, MAYBE_UNUSED int32_t portW, MAYBE_UNUSED int32_t portH, MAYBE_UNUSED bool renderingToUserSurface) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);

    // GL surfaces are stored bottom-up and draw_surface samples them with vertical flip.
    gl->base.cameraCurrent = GUI_CAMERA;
    GMLCamera* camera = &renderer->runner->guiCamera;
    camera->allocated = true;
    camera->viewX = 0.0;
    camera->viewY = 0.0;
    camera->viewWidth = guiW;
    camera->viewHeight = guiH;
    camera->borderX = 0;
    camera->borderY = 0;
    camera->speedX = 0;
    camera->speedY = 0;
    camera->objectId = -1;
    camera->viewAngle = 0;

    //yeah no I have no idea how to do the GUI
    Matrix4f projectionMatrix;
    Matrix4f_Orthographic(&projectionMatrix, (float) guiW, (float) guiH, 32000.0, 0.0);
    if (renderingToUserSurface) Matrix4f_flipClipY(&projectionMatrix);
    Matrix4f viewMatrix;
    float x = (float) guiW * 0.5f;
    float y = (float) guiH * 0.5f;
    Matrix4f_identity(&viewMatrix);
    Matrix4f_LookAt(&viewMatrix, x, y, -16000.0, x, y, 16000.0, 0.0, 1.0, 0.0);
    camera->viewMatrix = viewMatrix;
    camera->projectionMatrix = projectionMatrix;
    glApplyProjection(renderer,&camera->viewMatrix,&camera->projectionMatrix);
}

static void glEndGUI(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);
    glDisable(GL_SCISSOR_TEST);
}

static void glEndFrameInit(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (hasVAO()) glBindVertexArray(0);

    if (renderer->runner->usingAppSurface && !renderer->runner->appSurfaceAutoDraw) {
        glBindFramebuffer(GL_FRAMEBUFFER, gl->hostFramebuffer);
        return;
    }
}

static void glEndFrameEnd(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;

    if (renderer->runner->usingAppSurface && !renderer->runner->appSurfaceAutoDraw) {
        return;
    }
    int32_t appId = gl->base.runner->applicationSurfaceId;

    if (gl->isGL3) {
        GLCommon_beginLetterboxBlit(gl->surfaces[appId], gl->hostFramebuffer);
        GLCommon_endLetterboxBlit(gl->surfaceWidth[appId], gl->surfaceHeight[appId], gl->gameW, gl->gameH, gl->windowW, gl->windowH, gl->hostFramebuffer);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, gl->hostFramebuffer);
        GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);
        if (scissorWasEnabled) glDisable(GL_SCISSOR_TEST);

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glViewport(0, 0, gl->windowW, gl->windowH);

        renderer->vtable->setGuiProjection(renderer, gl->windowW, gl->windowH, gl->windowW, gl->windowH, false);
        glDisable(GL_BLEND);

        int32_t sx, sy, ex, ey;
        GLCommon_computeLetterbox(gl->gameW, gl->gameH, gl->windowW, gl->windowH, &sx, &sy, &ex, &ey);
        float scaleX = (float)(ex - sx) / (float)gl->gameW;
        float scaleY = (float)(ey - sy) / (float)gl->gameH;

        renderer->vtable->drawSurface(renderer, appId, 0, 0, gl->gameW, gl->gameH, (float)sx, (float)sy, scaleX, scaleY, 0.0f, 0xFFFFFF, 1.0f);
        flushBatch(gl);

        glEnable(GL_BLEND);
        if (scissorWasEnabled) glEnable(GL_SCISSOR_TEST);
    }
}

static void glRendererFlush(Renderer* renderer) {
    flushBatch((GLRenderer*) renderer);
}

static void glClearScreen(Renderer* renderer, uint32_t color, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    // GML draw_clear ignores the active scissor and clears the whole target. Disable scissor for the clear and restore it after.
    //No it doesn't?
    glClearColor(r, g, b, alpha);
    glClear(GL_COLOR_BUFFER_BIT);
}

// ===========================================================================
// ==[ Texture Decoding & Cache ]=============================================
// ===========================================================================

// Look up a decoded TXTR page in the CPU-side cache.  Returns a pointer to the cached
// RGBA pixels and sets *outW/*outH, or returns nullptr on a cache miss.
// On a hit the entry is promoted to the most-recently-used position so that the entry
// the store path evicts (index 0) is always the least-recently-used one.
static uint8_t* pageCacheLookup(GLRenderer* gl, uint32_t pageId, int32_t* outW, int32_t* outH) {
    if (gl->txtrPageCache == nullptr) return nullptr;
    repeat(gl->pageCacheSize, i) {
        if (gl->txtrPageCache[i].pageId == pageId) {
            PageCacheEntry hit = gl->txtrPageCache[i];
            *outW = (int32_t) hit.width;
            *outH = (int32_t) hit.height;
            // Promote the hit entry to the MRU slot: shift everything after it left
            // by one, then place it at the end (index pageCacheSize-1).
            if (i != gl->pageCacheSize - 1) {
                for (size_t k = i; k + 1 < gl->pageCacheSize; k++) {
                    gl->txtrPageCache[k] = gl->txtrPageCache[k + 1];
                }
                gl->txtrPageCache[gl->pageCacheSize - 1] = hit;
            }
            return hit.pixels;
        }
    }
    return nullptr;
}

// Store a decoded TXTR page in the CPU-side cache.  Takes ownership of `pixels`
// (the cache will free it on eviction).  Evicts the least-recently-used entry when full.
static void pageCacheStore(GLRenderer* gl, uint32_t pageId, int32_t w, int32_t h, uint8_t* pixels) {
    if (gl->txtrPageCache == nullptr) { free(pixels); return; }

    // The array is kept ordered from least- (index 0) to most-recently-used (end).
    // An insertion is just: drop the LRU if the cache is full, then append the new
    // page at the MRU position.  (pageCacheLookup maintains order on hits.)
    size_t count = 0;
    repeat(gl->pageCacheSize, i) {
        if (gl->txtrPageCache[i].pageId != UINT32_MAX) count++;
    }

    // If the cache is full we must give up one slot: evict the LRU at index 0.  Otherwise
    // the new entry goes into the first free slot, but we must keep the LRU ordering, so
    // compact the live entries toward the front first.
    size_t insertAt;
    if (count == gl->pageCacheSize) {
        // Full: evict index 0 (LRU) and shift everything left.
        free(gl->txtrPageCache[0].pixels);
        for (size_t i = 0; i + 1 < gl->pageCacheSize; i++) {
            gl->txtrPageCache[i] = gl->txtrPageCache[i + 1];
        }
        insertAt = gl->pageCacheSize - 1;
    } else {
        // Not full: compact live entries toward the front so the new entry lands at the
        // end (MRU), preserving the existing least-to-most-recent ordering.
        size_t write = 0;
        repeat(gl->pageCacheSize, i) {
            if (gl->txtrPageCache[i].pageId != UINT32_MAX) {
                if (write != i) gl->txtrPageCache[write] = gl->txtrPageCache[i];
                write++;
            }
        }
        insertAt = write;
    }

    gl->txtrPageCache[insertAt].pageId  = pageId;
    gl->txtrPageCache[insertAt].width   = (uint32_t) w;
    gl->txtrPageCache[insertAt].height  = (uint32_t) h;
    gl->txtrPageCache[insertAt].pixels  = pixels;
}

// Decode a TXTR page to RGBA pixels, using the cache when possible.
// On success returns pixels and sets *outW/*outH, and sets *outFromCache to indicate
// whether the returned pointer belongs to the cache (do NOT free) or is freshly decoded
// (the caller owns it and MUST free).  Returns nullptr on failure (and *outFromCache=false).
static uint8_t* decodeTxtrPage(GLRenderer* gl, DataWin* dw, uint32_t pageId, int32_t* outW, int32_t* outH, bool* outFromCache) {
    // Try the cache first – a cache hit returns a pointer the caller must NOT free.
    uint8_t* cached = pageCacheLookup(gl, pageId, outW, outH);
    if (cached != nullptr) {
        *outFromCache = true;
        return cached;
    }

    *outFromCache = false;

    // Cache miss – decode from the blob
    Texture* txtr = &dw->txtr.textures[pageId];
    DataWin_loadTxtrIfNeeded(dw, pageId);
    bool gm2022_5 = DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);
    uint8_t* pixels = ImageDecoder_decodeToRgba(txtr->blobData, (size_t) txtr->blobSize, gm2022_5, outW, outH);
    if (pixels == nullptr) {
        logWarn("GL: Failed to decode TXTR page %u\n", pageId);
        return nullptr;
    }
    // Free the compressed blob now that we've decoded it (unless it's memory-mapped)
    if (!txtr->mapped) {
        free(txtr->blobData);
        txtr->blobData = nullptr;
    }

    // Store a copy in the cache (the cache owns its copy; we return a separate pointer
    // so the caller can extract a sub-rectangle without corrupting the cached page).
    if (gl->txtrPageCache != nullptr) {
        uint8_t* cacheCopy = (uint8_t *)safeMalloc((size_t) (*outW) * (size_t) (*outH) * 4);
        memcpy(cacheCopy, pixels, (size_t) (*outW) * (size_t) (*outH) * 4);
        pageCacheStore(gl, pageId, *outW, *outH, cacheCopy);
    }

    return pixels;
}

// ===========================================================================
// ==[ Texture Upload ]======================================================
// ===========================================================================

// Lazily decodes and uploads a texture on first access.
//
// In **pageless mode** (`gl->pagelessTextures == true`), `index` is a TPAG index:
//   1. Look up which TXTR page the TPAG belongs to via tpag->texturePageId.
//   2. Decode that page (using the cache when possible).
//   3. Copy out the exact sub-rectangle this TPAG uses and upload it to its own
//      GL texture.  Unused areas of the page occupy no VRAM.
//
// In **paged mode** (`gl->pagelessTextures == false`), `index` is a TXTR page id:
//   1. Decode the whole page and upload it as a single GL texture.
//   2. TPAGs sample sub-regions of this texture via UV coordinates at draw time.
//
// On PLATFORM_VITA with VitaTextures active, the texture is loaded from the external
// compressed file regardless of the mode setting.
//
// Returns true once the texture is ready, false if it failed to decode.
bool GLRenderer_ensureTextureLoaded(GLRenderer* gl, uint32_t index) {
    if (gl->textureLoaded[index]) return (gl->textureWidths[index] != 0);
    gl->textureLoaded[index] = true;

    DataWin* dw = gl->base.dataWin;

#if defined(PLATFORM_VITA)
    // ============================================================
    // VitaTextures: external compressed texture file (always page-based)
    // ============================================================
    if (VitaTextures_Active()) {
        glBindTexture(GL_TEXTURE_2D, gl->glTextures[index]);
        if (!VitaTextures_LoadPage(index, &gl->textureWidths[index], &gl->textureHeights[index])) {
            logError("GL: Failed to load Vita TXTR page %u", index);
            return false;
        }
        logInfo("GL: Loaded Vita TXTR page %u (%dx%d)\n", index, gl->textureWidths[index], gl->textureHeights[index]);
        return true;
    }
#endif

    if (gl->pagelessTextures) {
        // ============================================================
        // PAGELESS MODE: index is a TPAG index.
        // Decode the owning TXTR page, extract the sub-rectangle for
        // this TPAG, and upload only that sub-rectangle.
        // ============================================================
        if (dw->tpag.count <= index) return false;
        TexturePageItem* tpag = &dw->tpag.items[index];
        int16_t pageId = tpag->texturePageId;
        if (0 > pageId || dw->txtr.count <= (uint32_t) pageId) return false;

        int pageW, pageH;
        bool fromCache;
        uint8_t* pagePixels = decodeTxtrPage(gl, dw, (uint32_t) pageId, &pageW, &pageH, &fromCache);
        if (pagePixels == nullptr) return false;

        // Source rectangle within the decoded page
        int su = tpag->sourceX, sv = tpag->sourceY;
        int rw = tpag->sourceWidth, rh = tpag->sourceHeight;

        // Clamp to decoded page bounds (safety)
        if (su + rw > pageW) rw = pageW - su;
        if (sv + rh > pageH) rh = pageH - sv;
        if (rw < 1) rw = 1;
        if (rh < 1) rh = 1;

        gl->textureWidths[index]  = rw;
        gl->textureHeights[index] = rh;

        // Upload the sub-rectangle.  If the TPAG covers the entire page we can
        // upload straight from the decode buffer; otherwise we must copy it out.
        if (su == 0 && sv == 0 && rw == pageW && rh == pageH) {
            glBindTexture(GL_TEXTURE_2D, gl->glTextures[index]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, pagePixels);
        } else {
            uint8_t* outPx = (uint8_t *)safeMalloc((size_t) rw * (size_t) rh * 4);
            for (int32_t row = 0; row < rh; row++) {
                memcpy(outPx + (size_t) row * rw * 4,
                       pagePixels + ((size_t) (sv + row) * pageW + su) * 4,
                       (size_t) rw * 4);
            }
            glBindTexture(GL_TEXTURE_2D, gl->glTextures[index]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, outPx);
            free(outPx);
        }

        // Free the freshly decoded page buffer unless it came from the cache
        // (the cache keeps its own copy).
        if (!fromCache) free(pagePixels);

        bool isPOT = (rw & (rw - 1)) == 0 && (rh & (rh - 1)) == 0;
        GLint wrapMode = isPOT ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode);

        logInfo("GL: Loaded texture %u (TPAG %u, sub-rect %dx%d)\n", index, pageId, rw, rh);

    } else {
        // ============================================================
        // PAGED MODE: index is a TXTR page id.
        // Decode the whole page and upload it as one GL texture.
        // TPAGs will sample sub-regions via UV coordinates at draw time.
        // ============================================================
        if (dw->txtr.count <= index) return false;

        int pageW, pageH;
        bool fromCache;
        uint8_t* pagePixels = decodeTxtrPage(gl, dw, index, &pageW, &pageH, &fromCache);
        if (pagePixels == nullptr) return false;

        gl->textureWidths[index]  = pageW;
        gl->textureHeights[index] = pageH;

        glBindTexture(GL_TEXTURE_2D, gl->glTextures[index]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pageW, pageH, 0, GL_RGBA, GL_UNSIGNED_BYTE, pagePixels);

        // Free the freshly decoded page buffer unless it came from the cache
        if (!fromCache) free(pagePixels);

        bool isPOT = (pageW & (pageW - 1)) == 0 && (pageH & (pageH - 1)) == 0;
        GLint wrapMode = isPOT ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode);

        logInfo("GL: Loaded TXTR page %u (%dx%d)\n", index, pageW, pageH);
    }

    return true;
}

// ===========================================================================
// ==[ Sprite Texture Resolution ]===========================================
// ===========================================================================

// Resolves a TPAG index to a loaded GL texture, the pixel dimensions of that texture,
// and the UV rectangle that selects the TPAG's region within the texture.
//
// In pageless mode the GL texture IS the TPAG's sub-rectangle, so UVs are (0,0)-(1,1).
// In paged mode the GL texture is the whole TXTR page, so UVs map the TPAG's source
// coordinates into the page's normalised UV space.
//
// Returns false if the TPAG index is out of range or the texture failed to load.
static bool resolveSpriteTexture(GLRenderer* gl, int32_t tpagIndex, TexturePageItem** outTpag, GLuint* outTexId, int32_t* outTexW, int32_t* outTexH, float* outU0, float* outV0, float* outU1, float* outV1) {
    DataWin* dw = gl->base.dataWin;
    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return false;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];

    if (gl->pagelessTextures) {
        // Pageless: the texture array is keyed by TPAG index, and the GL texture is
        // exactly the sub-rect for this TPAG.  (We only need pageId to be valid here,
        // but the array access is by tpagIndex.)
        int16_t pageId = tpag->texturePageId;
        if (0 > pageId) return false;
        if ((uint32_t) tpagIndex >= gl->textureCount) return false;
        if (!GLRenderer_ensureTextureLoaded(gl, (uint32_t) tpagIndex)) return false;
        *outTpag  = tpag;
        *outTexId = gl->glTextures[tpagIndex];
        *outTexW  = gl->textureWidths[tpagIndex];
        *outTexH  = gl->textureHeights[tpagIndex];
        // UVs cover the entire sub-rect texture
        if (outU0) { *outU0 = 0.0f; *outV0 = 0.0f; *outU1 = 1.0f; *outV1 = 1.0f; }
    } else {
        // Paged: look up via the page – the GL texture is the whole TXTR page
        int16_t pageId = tpag->texturePageId;
        if (0 > pageId || gl->textureCount <= (uint32_t) pageId) return false;
        if (!GLRenderer_ensureTextureLoaded(gl, (uint32_t) pageId)) return false;
        *outTpag  = tpag;
        *outTexId = gl->glTextures[pageId];
        *outTexW  = gl->textureWidths[pageId];
        *outTexH  = gl->textureHeights[pageId];
        // UVs map the TPAG's source rect into the page's normalised UV space
        if (outU0) {
            float w = (float) *outTexW, h = (float) *outTexH;
            *outU0 = (float) tpag->sourceX / w;
            *outV0 = (float) tpag->sourceY / h;
            *outU1 = (float) (tpag->sourceX + tpag->sourceWidth) / w;
            *outV1 = (float) (tpag->sourceY + tpag->sourceHeight) / h;
        }
    }
    return true;
}

// Emits a single textured quad into the batch given 4 final screen-space corners (TL, TR, BR, BL), 4 UVs forming a rect (u0,v0)-(u1,v1), and a flat color/alpha.
// Handles texture rebinding and batch flushing.
static void emitTexturedQuad(
    GLRenderer* gl, GLuint texId,
    float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
    float u0, float v0, float u1, float v1,
    uint8_t r0, uint8_t g0, uint8_t b0,
    uint8_t r1, uint8_t g1, uint8_t b1,
    uint8_t r2, uint8_t g2, uint8_t b2,
    uint8_t r3, uint8_t g3, uint8_t b3,
    float alpha
) {
    flushIfNeededAndSetActiveState(gl, BATCHTYPE_QUAD, texId);

    Vertex* verts = gl->vertexData + gl->batchCount * VERTICES_PER_QUAD;
    uint8_t ca = floatToUnormByte(alpha);

    verts[0].x = x0; verts[0].y = y0; verts[0].u = u0; verts[0].v = v0; verts[0].r = r0; verts[0].g = g0; verts[0].b = b0; verts[0].a = ca;
    verts[1].x = x1; verts[1].y = y1; verts[1].u = u1; verts[1].v = v0; verts[1].r = r1; verts[1].g = g1; verts[1].b = b1; verts[1].a = ca;
    verts[2].x = x2; verts[2].y = y2; verts[2].u = u1; verts[2].v = v1; verts[2].r = r2; verts[2].g = g2; verts[2].b = b2; verts[2].a = ca;
    verts[3].x = x3; verts[3].y = y3; verts[3].u = u0; verts[3].v = v1; verts[3].r = r3; verts[3].g = g3; verts[3].b = b3; verts[3].a = ca;

    gl->batchCount++;
}

static void drawMultiColoredTextureWithTransform(
    GLRenderer* renderer, GLuint textureId, Matrix4f transform,
    float localX0, float localY0, float localX1, float localY1,
    float u0, float v0, float u1, float v1,
    uint8_t r0, uint8_t g0, uint8_t b0,
    uint8_t r1, uint8_t g1, uint8_t b1,
    uint8_t r2, uint8_t g2, uint8_t b2,
    uint8_t r3, uint8_t g3, uint8_t b3,
    float alpha
) {
    float x0, y0, x1, y1, x2, y2, x3, y3;
    Matrix4f_transformPoint(&transform, localX0, localY0, &x0, &y0);
    Matrix4f_transformPoint(&transform, localX1, localY0, &x1, &y1);
    Matrix4f_transformPoint(&transform, localX1, localY1, &x2, &y2);
    Matrix4f_transformPoint(&transform, localX0, localY1, &x3, &y3);

    emitTexturedQuad(
        renderer, textureId,
        x0, y0, x1, y1, x2, y2, x3, y3,
        u0, v0, u1, v1,
        r0, g0, b0,
        r1, g1, b1,
        r2, g2, b2,
        r3, g3, b3,
        alpha
    );
}

static void drawTextureWithTransform(
    GLRenderer* renderer, GLuint textureId, Matrix4f transform,
    float localX0, float localY0, float localX1, float localY1,
    float u0, float v0, float u1, float v1,
    uint32_t color, float alpha
) {
    uint8_t r = (uint8_t) BGR_R(color);
    uint8_t g = (uint8_t) BGR_G(color);
    uint8_t b = (uint8_t) BGR_B(color);

    drawMultiColoredTextureWithTransform(
        renderer, textureId, transform,
        localX0, localY0, localX1, localY1,
        u0, v0, u1, v1,
        r, g, b,
        r, g, b,
        r, g, b,
        r, g, b,
        alpha
    );
}

static void drawTexture(
    GLRenderer* renderer,
    GLuint textureId,
    float x,
    float y,
    // Locals = the coordinate in relation to the sprite "frame" itself, includes originX/originY and trimmed transparency
    float localX0,
    float localY0,
    float localX1,
    float localY1,
    float u0,
    float v0,
    float u1,
    float v1,
    float xscale,
    float yscale,
    float angleDeg,
    uint32_t color,
    float alpha
) {
    // Build 2D transform: T(x,y) * R(-angleDeg) * S(xscale, yscale)
    // GML rotation is counter-clockwise, OpenGL rotation is counter-clockwise, but
    // since we have Y-down, we negate the angle to get the correct visual rotation
    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    drawTextureWithTransform(
        renderer,
        textureId,
        transform,
        localX0,
        localY0,
        localX1,
        localY1,
        u0,
        v0,
        u1,
        v1,
        color,
        alpha
    );
}

static void glDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    TexturePageItem* tpag;
    GLuint texId;
    int32_t texW, texH;
    float u0, v0, u1, v1;
    if (!resolveSpriteTexture(gl, tpagIndex, &tpag, &texId, &texW, &texH, &u0, &v0, &u1, &v1)) return;

    // Use targetWidth/Height (draw size in bounding rect), not sourceWidth/Height (texture sample size).
    // They differ when the texture was auto-downscaled by GMS to fit a texture page.
    float localX0 = (float) tpag->targetX - originX;
    float localY0 = (float) tpag->targetY - originY;
    float localX1 = localX0 + (float) tpag->targetWidth;
    float localY1 = localY0 + (float) tpag->targetHeight;

    drawTexture(
        gl,
        texId,
        x,
        y,
        localX0,
        localY0,
        localX1,
        localY1,
        u0,
        v0,
        u1,
        v1,
        xscale,
        yscale,
        angleDeg,
        color,
        alpha
    );
}

static void drawTiled(
    GLRenderer* gl,
    GLuint texId,
    float gridX,
    float gridY,
    float tileW,
    float tileH,
    bool tileX,
    bool tileY,
    float roomW,
    float roomH,
    float quadOffsetX0,
    float quadW,
    float quadOffsetY0,
    float quadH,
    float u0,
    float v0,
    float u1,
    float v1,
    uint32_t color,
    float alpha
) {
    if (0 >= tileW || 0 >= tileH) return;

    float startX, endX, startY, endY;
    if (tileX) {
        startX = fmodf(gridX, tileW);
        if (startX > 0) startX -= tileW;
        endX = roomW;
    } else {
        startX = gridX;
        endX = startX + tileW;
    }
    if (tileY) {
        startY = fmodf(gridY, tileH);
        if (startY > 0) startY -= tileH;
        endY = roomH;
    } else {
        startY = gridY;
        endY = startY + tileH;
    }

    // Optimization for 2D affine projects: Clip the tiled extent to the world-space AABB of what the active projection can see.
    const Matrix4f* projection = &gl->base.gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION];
    Matrix4f invProjection;
    if (Matrix4f_isAffine2D(projection) && Matrix4f_inverse(&invProjection, projection)) {
        // The borders of the screen
        static const float ndcCorners[4][2] = {{-1.0f, -1.0f}, {1.0f, -1.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f}};
        float visMinX = 0.0f, visMinY = 0.0f, visMaxX = 0.0f, visMaxY = 0.0f;
        // For each point, get the borders of it in world-space.
        repeat(4, i) {
            float wx, wy;
            Matrix4f_transformPoint(&invProjection, ndcCorners[i][0], ndcCorners[i][1], &wx, &wy);
            if (i == 0 || wx < visMinX) visMinX = wx;
            if (i == 0 || wx > visMaxX) visMaxX = wx;
            if (i == 0 || wy < visMinY) visMinY = wy;
            if (i == 0 || wy > visMaxY) visMaxY = wy;
        }
        if (tileX) {
            // Snap start forward by whole tiles so the tile containing visMinX is kept
            if (visMinX > startX) startX += floorf((visMinX - startX) / tileW) * tileW;
            if (visMaxX < endX) endX = visMaxX;
        }
        if (tileY) {
            if (visMinY > startY) startY += floorf((visMinY - startY) / tileH) * tileH;
            if (visMaxY < endY) endY = visMaxY;
        }
    }
    if (startX >= endX || startY >= endY) return;

    uint8_t r = (uint8_t) BGR_R(color), g = (uint8_t) BGR_G(color), b = (uint8_t) BGR_B(color);

    // Integer tile counts avoid FP-comparison drift; the inner break handles overshoot at the boundary
    int32_t tilesX = (int32_t) ((endX - startX) / tileW) + 1;
    int32_t tilesY = (int32_t) ((endY - startY) / tileH) + 1;
    if (0 >= tilesX || 0 >= tilesY) return;

    repeat(tilesY, iy) {
        float dy = startY + (float) iy * tileH;
        if (dy >= endY) break;
        float vy0 = dy + quadOffsetY0;
        float vy1 = vy0 + quadH;
        repeat(tilesX, ix) {
            float dx = startX + (float) ix * tileW;
            if (dx >= endX) break;
            float vx0 = dx + quadOffsetX0;
            float vx1 = vx0 + quadW;
            emitTexturedQuad(gl, texId, vx0, vy0, vx1, vy0, vx1, vy1, vx0, vy1, u0, v0, u1, v1, r, g, b, r, g, b, r, g, b, r, g, b, alpha);
        }
    }
}

static void glDrawSpriteTiled(Renderer* renderer, int32_t tpagIndex, float originX, float originY, float x, float y, float xscale, float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    TexturePageItem* tpag;
    GLuint texId;
    int32_t texW, texH;
    float u0, v0, u1, v1;
    if (!resolveSpriteTexture(gl, tpagIndex, &tpag, &texId, &texW, &texH, &u0, &v0, &u1, &v1)) return;

    float axScale = fabsf(xscale);
    float ayScale = fabsf(yscale);
    float tileW = (float) tpag->boundingWidth * axScale;
    float tileH = (float) tpag->boundingHeight * ayScale;

    // Use targetWidth/Height (draw size in bounding rect), not sourceWidth/Height (texture sample size).
    // They differ when the texture was auto-downscaled by GMS to fit a texture page.
    float localX0 = (float) tpag->targetX - originX;
    float localY0 = (float) tpag->targetY - originY;
    // Per-tile quad origin = grid cell (dx) + originX*axScale (cancels the grid anchor) + xscale*localX0
    float quadOffX0 = originX * axScale + xscale * localX0;
    float quadOffY0 = originY * ayScale + yscale * localY0;
    float quadW = xscale * (float) tpag->targetWidth;
    float quadH = yscale * (float) tpag->targetHeight;

    drawTiled(
        gl,
        texId,
        x - originX * axScale,
        y - originY * ayScale,
        tileW,
        tileH,
        tileX,
        tileY,
        roomW,
        roomH,
        quadOffX0,
        quadW,
        quadOffY0,
        quadH,
        u0,
        v0,
        u1,
        v1,
        color,
        alpha
    );
}

static void glDrawSpritePartColor(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    TexturePageItem* tpag;
    GLuint texId;
    int32_t texW, texH;
    float u0b, v0b, u1b, v1b;
    if (!resolveSpriteTexture(gl, tpagIndex, &tpag, &texId, &texW, &texH, &u0b, &v0b, &u1b, &v1b)) return;

    // Compute UVs for the sub-region. u0b/v0b is the origin of the TPAG region in the resolved
    // texture's UV space (0 on the desktop/ES per-tpag path, page-relative on Vita), so the
    // sub-region is that base plus the offset scaled by this texture's pixel dimensions.
    float u0 = u0b + (float) srcOffX / (float) texW;
    float v0 = v0b + (float) srcOffY / (float) texH;
    float u1 = u0b + (float) (srcOffX + srcW) / (float) texW;
    float v1 = v0b + (float) (srcOffY + srcH) / (float) texH;

    // Convert BGR colors to RGB bytes
    uint8_t r1 = (uint8_t) BGR_R(color1), g1 = (uint8_t) BGR_G(color1), b1 = (uint8_t) BGR_B(color1);
    uint8_t r2 = (uint8_t) BGR_R(color2), g2 = (uint8_t) BGR_G(color2), b2 = (uint8_t) BGR_B(color2);
    uint8_t r3 = (uint8_t) BGR_R(color3), g3 = (uint8_t) BGR_G(color3), b3 = (uint8_t) BGR_B(color3);
    uint8_t r4 = (uint8_t) BGR_R(color4), g4 = (uint8_t) BGR_G(color4), b4 = (uint8_t) BGR_B(color4);

    // Quad corners (no origin offset - draw_sprite_part ignores sprite origin)
    float cx0, cy0, cx1, cy1, cx2, cy2, cx3, cy3;
    if (angleDeg == 0.0f) {
        cx0 = x;                         cy0 = y;
        cx1 = x + (float) srcW * xscale; cy1 = y;
        cx2 = x + (float) srcW * xscale; cy2 = y + (float) srcH * yscale;
        cx3 = x;                         cy3 = y + (float) srcH * yscale;
    } else {
        float angleRad = -angleDeg * ((float) M_PI / 180.0f);
        float cosA = cosf(angleRad);
        float sinA = sinf(angleRad);
        float qx0 = x,                         qy0 = y;
        float qx1 = x + (float) srcW * xscale, qy1 = y;
        float qx2 = x + (float) srcW * xscale, qy2 = y + (float) srcH * yscale;
        float qx3 = x,                         qy3 = y + (float) srcH * yscale;
        float dx, dy;
        dx = qx0 - pivotX; dy = qy0 - pivotY; cx0 = cosA * dx - sinA * dy + pivotX; cy0 = sinA * dx + cosA * dy + pivotY;
        dx = qx1 - pivotX; dy = qy1 - pivotY; cx1 = cosA * dx - sinA * dy + pivotX; cy1 = sinA * dx + cosA * dy + pivotY;
        dx = qx2 - pivotX; dy = qy2 - pivotY; cx2 = cosA * dx - sinA * dy + pivotX; cy2 = sinA * dx + cosA * dy + pivotY;
        dx = qx3 - pivotX; dy = qy3 - pivotY; cx3 = cosA * dx - sinA * dy + pivotX; cy3 = sinA * dx + cosA * dy + pivotY;
    }

    emitTexturedQuad(gl, texId, cx0, cy0, cx1, cy1, cx2, cy2, cx3, cy3, u0, v0, u1, v1, r1, g1, b1, r2, g2, b2, r3, g3, b3, r4, g4, b4, alpha);
}

static void glDrawSpritePart(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha) {
    glDrawSpritePartColor(renderer, tpagIndex, srcOffX, srcOffY, srcW, srcH, x, y, xscale, yscale, angleDeg, pivotX, pivotY, color, color, color, color, alpha);
}

static void glDrawSpritePos(Renderer* renderer, int32_t tpagIndex, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    TexturePageItem* tpag;
    GLuint texId;
    int32_t texW, texH;
    float u0, v0, u1, v1;
    if (!resolveSpriteTexture(gl, tpagIndex, &tpag, &texId, &texW, &texH, &u0, &v0, &u1, &v1)) return;

    emitTexturedQuad(gl, texId, x1, y1, x2, y2, x3, y3, x4, y4, u0, v0, u1, v1, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, alpha);
}

// Emits a single colored quad into the batch using the white pixel texture
static void emitMultiColoredQuad(
    GLRenderer* gl,
    float x0,
    float y0,
    float x1,
    float y1,
    float x2,
    float y2,
    float x3,
    float y3,
    uint8_t r0,
    uint8_t g0,
    uint8_t b0,
    uint8_t r1,
    uint8_t g1,
    uint8_t b1,
    uint8_t r2,
    uint8_t g2,
    uint8_t b2,
    uint8_t r3,
    uint8_t g3,
    uint8_t b3,
    float a
) {
    emitTexturedQuad(
        gl,
        gl->whiteTexture,
        x0,
        y0,
        x1,
        y1,
        x2,
        y2,
        x3,
        y3,
        // Points to the middle of the whiteTexture
        0.5f,
        0.5f,
        0.5f,
        0.5f,
        r0,
        g0,
        b0,
        r1,
        g1,
        b1,
        r2,
        g2,
        b2,
        r3,
        g3,
        b3,
        a
    );
}

static void emitColoredQuad(
    GLRenderer* gl,
    float x0,
    float y0,
    float x1,
    float y1,
    float x2,
    float y2,
    float x3,
    float y3,
    uint8_t r,
    uint8_t g,
    uint8_t b,
    float a
) {
    emitTexturedQuad(
        gl,
        gl->whiteTexture,
        x0,
        y0,
        x1,
        y1,
        x2,
        y2,
        x3,
        y3,
        // Points to the middle of the whiteTexture
        0.5f,
        0.5f,
        0.5f,
        0.5f,
        r,
        g,
        b,
        r,
        g,
        b,
        r,
        g,
        b,
        r,
        g,
        b,
        a
    );
}

// Helper method that emits a colored rectangle
static void emitMultiColoredRectangle(
    GLRenderer* gl,
    float x0,
    float y0,
    float x1,
    float y1,
    uint8_t r0,
    uint8_t g0,
    uint8_t b0,
    uint8_t r1,
    uint8_t g1,
    uint8_t b1,
    uint8_t r2,
    uint8_t g2,
    uint8_t b2,
    uint8_t r3,
    uint8_t g3,
    uint8_t b3,
    float a
) {
    emitMultiColoredQuad(
        gl,
        // top-left
        x0,
        y0,
        // top-right
        x1,
        y0,
        // bottom-right
        x1,
        y1,
        // bottom-left
        x0,
        y1,
        r0,
        g0,
        b0,
        r1,
        g1,
        b1,
        r2,
        g2,
        b2,
        r3,
        g3,
        b3,
        a
    );
}

// Helper method that emits a colored rectangle
static void emitColoredRectangle(
    GLRenderer* gl,
    float x0,
    float y0,
    float x1,
    float y1,
    uint8_t r,
    uint8_t g,
    uint8_t b,
    float a
) {
    emitMultiColoredRectangle(gl, x0, y0, x1, y1, r, g, b, r, g, b, r, g, b, r, g, b, a);
}

static void glDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    GLRenderer* gl = (GLRenderer*) renderer;

    uint8_t r = (uint8_t) BGR_R(color), g = (uint8_t) BGR_G(color), b = (uint8_t) BGR_B(color);

    if (outline) {
        // Draw 4 one-pixel-wide edges: top, bottom, left, right
        emitColoredRectangle(gl, x1, y1, x2 + 1, y1 + 1, r, g, b, alpha); // top
        emitColoredRectangle(gl, x1, y2, x2 + 1, y2 + 1, r, g, b, alpha); // bottom
        emitColoredRectangle(gl, x1, y1 + 1, x1 + 1, y2, r, g, b, alpha); // left
        emitColoredRectangle(gl, x2, y1 + 1, x2 + 1, y2, r, g, b, alpha); // right
    } else {
        // Filled rectangle: GML adds +1 to width/height for filled rects
        emitColoredRectangle(gl, x1, y1, x2 + 1,y2 + 1, r, g, b, alpha);
    }
}

// ===[ Line Drawing ]===

static void glDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;

    uint8_t r = (uint8_t) BGR_R(color), g = (uint8_t) BGR_G(color), b = (uint8_t) BGR_B(color);

    // Compute perpendicular offset for line thickness
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;

    float halfW = width * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    emitColoredQuad(gl, x1 + px, y1 + py, x1 - px, y1 - py, x2 - px, y2 - py, x2 + px, y2 + py, r, g, b, alpha);
}

static void glDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;

    uint8_t r1 = (uint8_t) BGR_R(color1), g1 = (uint8_t) BGR_G(color1), b1 = (uint8_t) BGR_B(color1);
    uint8_t r2 = (uint8_t) BGR_R(color2), g2 = (uint8_t) BGR_G(color2), b2 = (uint8_t) BGR_B(color2);

    // Compute perpendicular offset for line thickness
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (0.0001f > len) return;

    float halfW = width * 0.5f;
    float px = (-dy / len) * halfW;
    float py = (dx / len) * halfW;

    emitMultiColoredQuad(
        gl,
        x1 + px,
        y1 + py,
        x1 - px,
        y1 - py,
        x2 - px,
        y2 - py,
        x2 + px,
        y2 + py,
        r1,
        g1,
        b1,
        r1,
        g1,
        b1,
        r2,
        g2,
        b2,
        r2,
        g2,
        b2,
        alpha
    );
}

static void glDrawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4, float alpha, bool outline) {
    GLRenderer* gl = (GLRenderer*) renderer;

    uint8_t r1 = (uint8_t) BGR_R(color1), g1 = (uint8_t) BGR_G(color1), b1 = (uint8_t) BGR_B(color1);
    uint8_t r2 = (uint8_t) BGR_R(color2), g2 = (uint8_t) BGR_G(color2), b2 = (uint8_t) BGR_B(color2);
    uint8_t r3 = (uint8_t) BGR_R(color3), g3 = (uint8_t) BGR_G(color3), b3 = (uint8_t) BGR_B(color3);
    uint8_t r4 = (uint8_t) BGR_R(color4), g4 = (uint8_t) BGR_G(color4), b4 = (uint8_t) BGR_B(color4);

    if (outline) {
        // Draw 4 one-pixel-wide edges: top, bottom, left, right
        glDrawLineColor(renderer, x1, y1, x2, y1, 1.0, color1, color2, alpha);
        glDrawLineColor(renderer, x2, y1, x2, y2, 1.0, color2, color3, alpha);
        glDrawLineColor(renderer, x2, y2, x1, y2, 1.0, color3, color4, alpha);
        glDrawLineColor(renderer, x1, y2, x1, y1, 1.0, color4, color1, alpha);
    } else {
        // Filled rectangle: GML adds +1 to width/height for filled rects
        emitMultiColoredRectangle(
            gl,
            x1,
            y1,
            x2 + 1,
            y2 + 1,
            r1,
            g1,
            b1,
            r2,
            g2,
            b2,
            r3,
            g3,
            b3,
            r4,
            g4,
            b4,
            alpha
        );
    }
}

static void glDrawTriangle(Renderer *renderer, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t color1, uint32_t color2, uint32_t color3, float alpha, bool outline) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (outline) {
        glDrawLineColor(renderer, x1, y1, x2, y2, 1, color1, color2, alpha);
        glDrawLineColor(renderer, x2, y2, x3, y3, 1, color2, color3, alpha);
        glDrawLineColor(renderer, x3, y3, x1, y1, 1, color3, color1, alpha);
    } else {
        flushIfNeededAndSetActiveState(gl, BATCHTYPE_TRIANGLE, gl->whiteTexture);

        // Woo, pointers!
        // This gets the vertex data for the new triangle batch
        Vertex* verts = gl->vertexData + gl->batchCount * VERTICES_PER_TRIANGLE;
        uint8_t ca = floatToUnormByte(alpha);

        verts[0].x = x1; verts[0].y = y1; verts[0].u = 0.0f; verts[0].v = 0.0f; verts[0].r = (uint8_t) BGR_R(color1); verts[0].g = (uint8_t) BGR_G(color1); verts[0].b = (uint8_t) BGR_B(color1); verts[0].a = ca;
        verts[1].x = x2; verts[1].y = y2; verts[1].u = 0.0f; verts[1].v = 0.0f; verts[1].r = (uint8_t) BGR_R(color2); verts[1].g = (uint8_t) BGR_G(color2); verts[1].b = (uint8_t) BGR_B(color2); verts[1].a = ca;
        verts[2].x = x3; verts[2].y = y3; verts[2].u = 0.0f; verts[2].v = 0.0f; verts[2].r = (uint8_t) BGR_R(color3); verts[2].g = (uint8_t) BGR_G(color3); verts[2].b = (uint8_t) BGR_B(color3); verts[2].a = ca;

        gl->batchCount++;
    }
}

// ===[ Text Drawing ]===

// Resolved font state shared between glDrawText and glDrawTextColor
typedef struct {
    Font* font;
    TexturePageItem* fontTpag; // single TPAG for regular fonts (nullptr for sprite fonts)
    GLuint texId;
    int32_t texW, texH;
    float fontU0, fontV0; // UV origin of the font TPAG region in the resolved texture's space
    Sprite* spriteFontSprite; // source sprite for sprite fonts (nullptr for regular fonts)
} GlFontState;

// Resolves font texture state
// Returns false if the font can't be drawn
static bool glResolveFontState(GLRenderer* gl, DataWin* dw, Font* font, GlFontState* state) {
    state->font = font;
    state->fontTpag = nullptr;
    state->texId = 0;
    state->texW = 0;
    state->texH = 0;
    state->fontU0 = 0.0f;
    state->fontV0 = 0.0f;
    state->spriteFontSprite = nullptr;

    if (!font->isSpriteFont) {
        int32_t fontTpagIndex = font->tpagIndex;
        if (0 > fontTpagIndex) return false;

        TexturePageItem* tpag;
        float u0, v0, u1, v1;
        if (!resolveSpriteTexture(gl, fontTpagIndex, &tpag, &state->texId, &state->texW, &state->texH, &u0, &v0, &u1, &v1)) return false;
        state->fontTpag = tpag;
        state->fontU0 = u0;
        state->fontV0 = v0;
    } else if (font->spriteIndex >= 0 && dw->sprt.count > (uint32_t) font->spriteIndex) {
        state->spriteFontSprite = &dw->sprt.sprites[font->spriteIndex];
    }
    return true;
}

// Resolves UV coordinates, texture ID, and local position for a single glyph
// Returns false if the glyph can't be drawn
static bool glResolveGlyph(GLRenderer* gl, GlFontState* state, FontGlyph* glyph, float cursorX, float cursorY, GLuint* outTexId, float* outU0, float* outV0, float* outU1, float* outV1, float* outLocalX0, float* outLocalY0) {
    Font* font = state->font;
    if (font->isSpriteFont && state->spriteFontSprite != nullptr) {
        Sprite* sprite = state->spriteFontSprite;
        int32_t glyphIndex = (int32_t) (glyph - font->glyphs);
        if (0 > glyphIndex ||  glyphIndex >= (int32_t) sprite->textureCount) return false;

        int32_t tpagIdx = sprite->tpagIndices[glyphIndex];
        if (0 > tpagIdx) return false;

        TexturePageItem* glyphTpag;
        int32_t gw, gh;
        if (!resolveSpriteTexture(gl, tpagIdx, &glyphTpag, outTexId, &gw, &gh, outU0, outV0, outU1, outV1)) return false;

        // Sprite-font glyphs sit at the cell offset. GM 2023.2+ subtracts the sprite origin, pre-2023.2 it cancels.
        // (See GameMaker-HTML5's commit a7c5b909209d5a28602fedfe2031965386a99921)
        *outLocalX0 = cursorX + (float) glyph->offset;
        *outLocalY0 = cursorY + (float) (int32_t) glyphTpag->targetY - (float) font->spriteOriginYAdjust;
    } else {
        *outTexId = state->texId;
        // Regular-font glyphs sample a sub-rectangle of the font's single TPAG texture, offset
        // by the TPAG region's UV origin within the resolved texture.
        *outU0 = state->fontU0 + (float) glyph->sourceX / (float) state->texW;
        *outV0 = state->fontV0 + (float) glyph->sourceY / (float) state->texH;
        *outU1 = state->fontU0 + (float) (glyph->sourceX + glyph->sourceWidth) / (float) state->texW;
        *outV1 = state->fontV0 + (float) (glyph->sourceY + glyph->sourceHeight) / (float) state->texH;

        *outLocalX0 = cursorX + glyph->offset;
        *outLocalY0 = cursorY;
    }
    return true;
}

static void drawText(
    Renderer* renderer,
    const char* text,
    float x,
    float y,
    float xscale,
    float yscale,
    float angleDeg,
    float lineSeparation,
    uint32_t _c1,
    uint32_t _c2,
    uint32_t _c3,
    uint32_t _c4,
    float alpha
) {
    GLRenderer* gl = (GLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || dw->font.count <= (uint32_t) fontIndex)
        return;

    Font* font = &dw->font.fonts[fontIndex];

    GlFontState fontState;
    if (!glResolveFontState(gl, dw, font, &fontState))
        return;

    int32_t textLen = (int32_t) strlen(text);
    if (textLen == 0)
        return;

    // Count lines, treating \r\n and \n\r as single breaks
    int32_t lineCount = TextUtils_countLines(text, textLen);

    float lineStride = (0.0f > lineSeparation) ? TextUtils_lineStride(font) : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));

    // Vertical alignment offset
    float totalHeight = (float) lineCount * lineStride;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;

    // Build transform matrix
    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);

    // Iterate through lines. HTML5 subtracts ascenderOffset from per-line y offset.
    float cursorY = valignOffset - (float) font->ascenderOffset;
    int32_t lineStart = 0;

    int32_t c1 = _c1;
    int32_t c2 = _c2;
    int32_t c3 = _c3;
    int32_t c4 = _c4;
    bool needsLerpingOnTheFly = c1 != c2 || c2 != c3 || c3 != c4;

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        // Find end of current line
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(text[lineEnd])) {
            lineEnd++;
        }
        int32_t lineLen = lineEnd - lineStart;

        // Horizontal alignment offset for this line
        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;
        // Pixel-position cursor for the gradient
        float gradientX = 0.0f;

        // Render each glyph in the line - decode each codepoint once and carry it forward as next iteration's ch (also used for kerning)
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

            if (glyph != nullptr) {
                float advance = (float) glyph->shift;
                float leftFrac = (lineWidth > 0.0f) ? (gradientX / lineWidth) : 0.0f;
                float rightFrac = (lineWidth > 0.0f) ? ((gradientX + advance) / lineWidth) : 1.0f;
                if (needsLerpingOnTheFly) {
                    c1 = Color_lerp(_c1, _c2, leftFrac);
                    c2 = Color_lerp(_c1, _c2, rightFrac);
                    c3 = Color_lerp(_c4, _c3, rightFrac);
                    c4 = Color_lerp(_c4, _c3, leftFrac);
                }

                uint8_t r0 = (uint8_t) BGR_R(c1), g0 = (uint8_t) BGR_G(c1), b0 = (uint8_t) BGR_B(c1);
                uint8_t r1 = (uint8_t) BGR_R(c2), g1 = (uint8_t) BGR_G(c2), b1 = (uint8_t) BGR_B(c2);
                uint8_t r2 = (uint8_t) BGR_R(c3), g2 = (uint8_t) BGR_G(c3), b2 = (uint8_t) BGR_B(c3);
                uint8_t r3 = (uint8_t) BGR_R(c4), g3 = (uint8_t) BGR_G(c4), b3 = (uint8_t) BGR_B(c4);

                bool drewSuccessfully = false;
                if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                    float u0, v0, u1, v1;
                    float localX0, localY0;
                    GLuint glyphTexId;

                    if (glResolveGlyph(gl, &fontState, glyph, cursorX, cursorY, &glyphTexId, &u0, &v0, &u1, &v1, &localX0, &localY0)) {
                        float localX1 = localX0 + (float) glyph->sourceWidth;
                        float localY1 = localY0 + (float) glyph->sourceHeight;

                        drawMultiColoredTextureWithTransform(
                            gl,
                            glyphTexId,
                            transform,
                            localX0,
                            localY0,
                            localX1,
                            localY1,
                            u0,
                            v0,
                            u1,
                            v1,
                            r0,
                            g0,
                            b0,
                            r1,
                            g1,
                            b1,
                            r2,
                            g2,
                            b2,
                            r3,
                            g3,
                            b3,
                            alpha
                        );
                        drewSuccessfully = true;
                    }
                }

                cursorX += glyph->shift;
                gradientX += glyph->shift;
                if (drewSuccessfully && hasNext) {
                    float kern = TextUtils_getKerningOffset(glyph, nextCh);
                    cursorX += kern;
                    gradientX += kern;
                }
            }

            ch = nextCh;
            hasCh = hasNext;
        }

        cursorY += lineStride;
        // Skip past the newline, treating \r\n and \n\r as single breaks
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        } else {
            lineStart = lineEnd;
        }
    }
}

static void glDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, float lineSeparation) {
    drawText(
        renderer,
        text,
        x,
        y,
        xscale,
        yscale,
        angleDeg,
        lineSeparation,
        renderer->drawColor,
        renderer->drawColor,
        renderer->drawColor,
        renderer->drawColor,
        renderer->drawAlpha
    );
}

static void glDrawTextColor(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t _c1, int32_t _c2, int32_t _c3, int32_t _c4, float alpha, float lineSeparation) {
    drawText(
        renderer,
        text,
        x,
        y,
        xscale,
        yscale,
        angleDeg,
        lineSeparation,
        _c1,
        _c2,
        _c3,
        _c4,
        alpha
    );
}

// ===[ Dynamic Sprite Creation/Deletion ]===

// Finds a free dynamic texture page slot (glTextures[i] == 0), or appends a new one.
// Used in paged mode (and on PLATFORM_VITA with VitaTextures): dynamic sprites created at
// runtime get their own whole-page slot, indexed by page id.
static uint32_t findOrAllocTexturePageSlot(GLRenderer* gl) {
    // Scan dynamic range for a reusable slot
    for (uint32_t i = gl->originalTexturePageCount; gl->textureCount > i; i++) {
        if (gl->glTextures[i] == 0) return i;
    }
    // No free slot found, grow the arrays
    uint32_t newPageId = gl->textureCount;
    gl->textureCount++;
    gl->glTextures = (GLuint *)safeRealloc(gl->glTextures, gl->textureCount * sizeof(GLuint));
    gl->textureWidths = (int32_t *)safeRealloc(gl->textureWidths, gl->textureCount * sizeof(int32_t));
    gl->textureHeights = (int32_t *)safeRealloc(gl->textureHeights, gl->textureCount * sizeof(int32_t));
    gl->textureLoaded = (bool *)safeRealloc(gl->textureLoaded, gl->textureCount * sizeof(bool));
    gl->glTextures[newPageId] = 0;
    gl->textureWidths[newPageId] = 0;
    gl->textureHeights[newPageId] = 0;
    gl->textureLoaded[newPageId] = false;
    return newPageId;
}

// Finds a free dynamic TPAG slot (texturePageId == -1), or appends a new one.
//
// In pageless mode the GL texture arrays are keyed by TPAG, so growing the TPAG table also
// grows the renderer's TPAG-keyed GL texture arrays here.  In paged mode (and VitaTextures)
// the caller still allocates a separate page slot via findOrAllocTexturePageSlot.
static uint32_t findOrAllocTpagSlot(GLRenderer* gl, DataWin* dw, uint32_t originalTpagCount) {
    for (uint32_t i = originalTpagCount; dw->tpag.count > i; i++) {
        if (dw->tpag.items[i].texturePageId == -1) return i;
    }
    uint32_t newIndex = dw->tpag.count;
    dw->tpag.count++;
    dw->tpag.items = (TexturePageItem *)safeRealloc(dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
    memset(&dw->tpag.items[newIndex], 0, sizeof(TexturePageItem));
    dw->tpag.items[newIndex].texturePageId = -1;

    // In pageless mode, grow the TPAG-keyed GL texture arrays to match the new TPAG count.
    // (In paged mode the caller uses a separate page slot.)
    if (gl->pagelessTextures) {
        uint32_t newCount = dw->tpag.count;
        gl->glTextures = (GLuint *)safeRealloc(gl->glTextures, newCount * sizeof(GLuint));
        gl->textureWidths = (int32_t *)safeRealloc(gl->textureWidths, newCount * sizeof(int32_t));
        gl->textureHeights = (int32_t *)safeRealloc(gl->textureHeights, newCount * sizeof(int32_t));
        gl->textureLoaded = (bool *)safeRealloc(gl->textureLoaded, newCount * sizeof(bool));
        for (uint32_t k = gl->textureCount; k < newCount; k++) {
            gl->glTextures[k] = 0;
            gl->textureWidths[k] = 0;
            gl->textureHeights[k] = 0;
            gl->textureLoaded[k] = false;
        }
        gl->textureCount = newCount;
    }
    return newIndex;
}

static int32_t glCreateSurface(Renderer* renderer, int32_t width, int32_t height) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);

    // Save the current FBO binding so creating a surface doesn't change the active render target.
    GLint prevBinding = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevBinding);

    uint32_t surfaceIndex = GLCommon_findOrAllocateSurfaceSlot(&gl->surfaces, &gl->surfaceTexture, &gl->surfaceWidth, &gl->surfaceHeight, &gl->surfaceCount);

    glGenFramebuffers(1, &gl->surfaces[surfaceIndex]);

    glGenTextures(1, &gl->surfaceTexture[surfaceIndex]);
    glBindTexture(GL_TEXTURE_2D, gl->surfaceTexture[surfaceIndex]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    bool isPOT = (width & (width - 1)) == 0 && (height & (height - 1)) == 0;
    GLint wrapMode = isPOT ? GL_REPEAT : GL_CLAMP_TO_EDGE;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapMode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapMode);


    glBindFramebuffer(GL_FRAMEBUFFER, gl->surfaces[surfaceIndex]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl->surfaceTexture[surfaceIndex], 0);

    gl->surfaceWidth[surfaceIndex] = width;
    gl->surfaceHeight[surfaceIndex] = height;

    logInfo("GL: Created surface %u with size (%dx%d)\n", surfaceIndex, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prevBinding);

    return (int32_t) surfaceIndex;
}

static int32_t glEnsureApplicationSurface(Renderer* renderer, int32_t width, int32_t height) {
    GLRenderer* gl = (GLRenderer*) renderer;
    int32_t id = renderer->runner->applicationSurfaceId;

    bool needsCreate = (id < 0) || ((uint32_t) id >= gl->surfaceCount) || (gl->surfaces[id] == 0);
    if (needsCreate) {
        id = glCreateSurface(renderer, width, height);
        // Publish immediately so anything that re-queries the runner during this frame sees the new ID.
        renderer->runner->applicationSurfaceId = id;
        return id;
    }

    if (gl->surfaceWidth[id] != width || gl->surfaceHeight[id] != height) {
        renderer->vtable->surfaceResize(renderer, id, width, height);
    }
    return id;
}

static void glSurfaceFree(Renderer* renderer, int32_t surfaceID) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);

    if (0 > surfaceID || (uint32_t) surfaceID >= gl->surfaceCount) return;

    // Freeing the application_surface is a no-op from GML; the runner manages its lifecycle via application_surface_enable.
    if (surfaceID == renderer->runner->applicationSurfaceId) return;

    if (gl->surfaceTexture[surfaceID] != 0) glDeleteTextures(1, &gl->surfaceTexture[surfaceID]);
    if (gl->surfaces[surfaceID] != 0) glDeleteFramebuffers(1, &gl->surfaces[surfaceID]);
    gl->surfaces[surfaceID] = 0;
    gl->surfaceTexture[surfaceID] = 0;
    gl->surfaceWidth[surfaceID] = 0;
    gl->surfaceHeight[surfaceID] = 0;
    logInfo("GL: Freed Surface %u\n", surfaceID);
}

static void glSurfaceResize(Renderer* renderer, int32_t surfaceID, int32_t width, int32_t height) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);

    if (0 > surfaceID || (uint32_t) surfaceID >= gl->surfaceCount) return;
    if (gl->surfaces[surfaceID] == 0) return;
    if (gl->surfaceWidth[surfaceID] == width && gl->surfaceHeight[surfaceID] == height) return;

    GLint prevBinding = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevBinding);

    if (gl->surfaceTexture[surfaceID] != 0) glDeleteTextures(1, &gl->surfaceTexture[surfaceID]);

    glGenTextures(1, &gl->surfaceTexture[surfaceID]);
    glBindTexture(GL_TEXTURE_2D, gl->surfaceTexture[surfaceID]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindFramebuffer(GL_FRAMEBUFFER, gl->surfaces[surfaceID]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl->surfaceTexture[surfaceID], 0);

    gl->surfaceWidth[surfaceID] = width;
    gl->surfaceHeight[surfaceID] = height;

    logInfo("GL: Resized Surface %u Size (%dx%d)\n", surfaceID, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prevBinding);
}

static bool glSurfaceExists(Renderer* renderer, int32_t surfaceId) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (0 > surfaceId || (uint32_t) surfaceId >= gl->surfaceCount) return false;
    return gl->surfaces[surfaceId] != 0;
}

static bool glSurfaceGetPixels(Renderer* renderer, int32_t surfaceId, uint8_t* outRGBA) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);
    return GLCommon_surfaceGetPixels(gl->surfaces, gl->surfaceWidth, gl->surfaceHeight, gl->surfaceCount, surfaceId, outRGBA);
}

static bool glSetRenderTarget(Renderer* renderer, int32_t surfaceId, bool implicitApplicationSurface) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);

    int32_t viewCurrent = 0;
    if (renderer->runner->viewsEnabled) {
    viewCurrent = renderer->runner->viewCurrent;
    }
    RuntimeView* view = &renderer->runner->views[viewCurrent];
    gl->base.cameraCurrent = view->cameraId;
    GMLCamera* camera = Runner_getCameraById(renderer->runner, gl->base.cameraCurrent);

    if (0 > surfaceId || (uint32_t) surfaceId >= gl->surfaceCount) return false;
    if (gl->surfaces[surfaceId] == 0) return false;

    glBindFramebuffer(GL_FRAMEBUFFER, gl->surfaces[surfaceId]);

    if (surfaceId == renderer->runner->applicationSurfaceId && implicitApplicationSurface) {
        glViewport(gl->base.CPortX, gl->base.CPortY, gl->base.CPortW, gl->base.CPortH);
        glEnable(GL_SCISSOR_TEST);

        glApplyProjection(renderer,&camera->viewMatrix,&camera->projectionMatrix);

        return true;
    }


    if (surfaceId == view->surfaceId) {
    //the surface belongs to the view we are rending, we use the view's camera.
    glViewport(0, 0, gl->surfaceWidth[surfaceId], gl->surfaceHeight[surfaceId]);
    glDisable(GL_SCISSOR_TEST);
    glApplyProjection(renderer,&camera->viewMatrix,&camera->projectionMatrix);
    return true;
    } else {
    //camera will use full surface.
    gl->base.cameraCurrent = SURFACE_CAMERA;
    GMLCamera* camera =  &renderer->runner->surfaceCamera;

    camera->allocated = true;
    camera->viewX = 0.0;
    camera->viewY = 0.0;
    camera->viewWidth = gl->surfaceWidth[surfaceId];
    camera->viewHeight = gl->surfaceHeight[surfaceId];
    camera->borderX = 0;
    camera->borderY = 0;
    camera->speedX = 0;
    camera->speedY = 0;
    camera->objectId = -1;
    camera->viewAngle = 0;
    Runner_updateCameraViewSimple(camera);

    glViewport(0, 0, gl->surfaceWidth[surfaceId], gl->surfaceHeight[surfaceId]);
    glDisable(GL_SCISSOR_TEST);
    glApplyProjection(renderer, &camera->viewMatrix,&camera->projectionMatrix);
    return true;
    }


    glViewport(0, 0, gl->surfaceWidth[surfaceId], gl->surfaceHeight[surfaceId]);
    glDisable(GL_SCISSOR_TEST);

    return true;
}

static void glSurfaceCopy(Renderer* renderer, int32_t destSurfaceID, int32_t destX, int32_t destY, int32_t srcSurfaceID, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, bool part) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);

    if (0 > srcSurfaceID || (uint32_t) srcSurfaceID >= gl->surfaceCount || gl->surfaces[srcSurfaceID] == 0) return;
    if (0 > destSurfaceID || (uint32_t) destSurfaceID >= gl->surfaceCount || gl->surfaces[destSurfaceID] == 0) return;

    if (gl->isGL3) {
        GLCommon_surfaceBlit(gl->surfaces, gl->surfaceWidth, gl->surfaceHeight, gl->surfaceCount, destSurfaceID, destX, destY, srcSurfaceID, srcX, srcY, srcW, srcH, part);
    } else {
        GLint prevBinding = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevBinding);
        Matrix4f prevProj = renderer->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION];
        bool prevBlend = glIsEnabled(GL_BLEND);
        GLint prevViewport[4];
        glGetIntegerv(GL_VIEWPORT, prevViewport);

        glBindFramebuffer(GL_FRAMEBUFFER, gl->surfaces[destSurfaceID]);
        glViewport(0, 0, gl->surfaceWidth[destSurfaceID], gl->surfaceHeight[destSurfaceID]);
        glDisable(GL_BLEND);

        renderer->vtable->setGuiProjection(renderer, gl->surfaceWidth[destSurfaceID], gl->surfaceHeight[destSurfaceID], gl->surfaceWidth[destSurfaceID], gl->surfaceHeight[destSurfaceID], true);

        int32_t sX = part ? srcX : 0;
        int32_t sY = part ? srcY : 0;
        int32_t sW = part ? srcW : gl->surfaceWidth[srcSurfaceID];
        int32_t sH = part ? srcH : gl->surfaceHeight[srcSurfaceID];

        renderer->vtable->drawSurface(renderer, srcSurfaceID, sX, sY, sW, sH, (float)destX, (float)destY, 1.0f, 1.0f, 0.0f, 0xFFFFFF, 1.0f);
        flushBatch(gl);

        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) prevBinding);
        renderer->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION] = prevProj;
        glShaderSettingsRefresh(renderer);
        if (prevBlend) glEnable(GL_BLEND);
        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    }
}

static float glGetSurfaceWidth(Renderer* renderer, int32_t surfaceId) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (0 > surfaceId || (uint32_t) surfaceId >= gl->surfaceCount) return 0.0f;
    if (gl->surfaces[surfaceId] == 0) return 0.0f;
    return (float) gl->surfaceWidth[surfaceId];
}

static float glGetSurfaceHeight(Renderer* renderer, int32_t surfaceId) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (0 > surfaceId || (uint32_t) surfaceId >= gl->surfaceCount) return 0.0f;
    if (gl->surfaces[surfaceId] == 0) return 0.0f;
    return (float) gl->surfaceHeight[surfaceId];
}

static void glDrawSurface(Renderer* renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight, float x, float y, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;

    if (0 > surfaceID || (uint32_t) surfaceID >= gl->surfaceCount) return;
    if (gl->surfaceTexture[surfaceID] == 0) return;

    GLuint texId = gl->surfaceTexture[surfaceID];
    int32_t texW = gl->surfaceWidth[surfaceID];
    int32_t texH = gl->surfaceHeight[surfaceID];

    if (0 > srcWidth) { srcLeft = 0; srcTop = 0; srcWidth = texW; srcHeight = texH; }

    float u0 = (float) srcLeft / (float) texW;
    float v0 = (float) srcTop / (float) texH;
    float u1 = (float) (srcLeft + srcWidth) / (float) texW;
    float v1 = (float) (srcTop + srcHeight) / (float) texH;

    float localX1 = (float) srcWidth;
    float localY1 = (float) srcHeight;

    drawTexture(
        gl,
        texId,
        x,
        y,
        0.0f,
        0.0f,
        localX1,
        localY1,
        u0,
        v0,
        u1,
        v1,
        xscale,
        yscale,
        angleDeg,
        color,
        alpha
    );
}

static void glDrawSurfaceColor(Renderer* renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight, float x, float y, float xscale, float yscale, float angleDeg, uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;

    if (0 > surfaceID || (uint32_t) surfaceID >= gl->surfaceCount) return;
    if (gl->surfaceTexture[surfaceID] == 0) return;

    GLuint texId = gl->surfaceTexture[surfaceID];
    int32_t texW = gl->surfaceWidth[surfaceID];
    int32_t texH = gl->surfaceHeight[surfaceID];

    if (0 > srcWidth) { srcLeft = 0; srcTop = 0; srcWidth = texW; srcHeight = texH; }

    float u0 = (float) srcLeft / (float) texW;
    float v0 = (float) srcTop / (float) texH;
    float u1 = (float) (srcLeft + srcWidth) / (float) texW;
    float v1 = (float) (srcTop + srcHeight) / (float) texH;

    float localX1 = (float) srcWidth;
    float localY1 = (float) srcHeight;

    uint8_t r1 = (uint8_t) BGR_R(color1);
    uint8_t g1 = (uint8_t) BGR_G(color1);
    uint8_t b1 = (uint8_t) BGR_B(color1);
    uint8_t r2 = (uint8_t) BGR_R(color2);
    uint8_t g2 = (uint8_t) BGR_G(color2);
    uint8_t b2 = (uint8_t) BGR_B(color2);
    uint8_t r3 = (uint8_t) BGR_R(color3);
    uint8_t g3 = (uint8_t) BGR_G(color3);
    uint8_t b3 = (uint8_t) BGR_B(color3);
    uint8_t r4 = (uint8_t) BGR_R(color4);
    uint8_t g4 = (uint8_t) BGR_G(color4);
    uint8_t b4 = (uint8_t) BGR_B(color4);

    float angleRad = -angleDeg * ((float) M_PI / 180.0f);

    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    drawMultiColoredTextureWithTransform(
        gl,
        texId,
        transform,
        0.0f,
        0.0f,
        localX1,
        localY1,
        u0,
        v0,
        u1,
        v1,
        r1,
        g1,
        b1,
        r2,
        g2,
        b2,
        r3,
        g3,
        b3,
        r4,
        g4,
        b4,
        alpha
    );
}

static void glDrawSurfaceTiled(Renderer* renderer, int32_t surfaceID, float x, float y, float xscale, float yscale, float roomW, float roomH, uint32_t color, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;

    if (0 > surfaceID || (uint32_t) surfaceID >= gl->surfaceCount) return;
    if (gl->surfaceTexture[surfaceID] == 0) return;

    GLuint texId = gl->surfaceTexture[surfaceID];
    int32_t texW = gl->surfaceWidth[surfaceID];
    int32_t texH = gl->surfaceHeight[surfaceID];

    float tileW = (float) texW * fabsf(xscale);
    float tileH = (float) texH * fabsf(yscale);

    drawTiled(
        gl,
        texId,
        x,
        y,
        tileW,
        tileH,
        true,
        true,
        roomW,
        roomH,
        0.0f,
        xscale * (float) texW,
        0.0f,
        yscale * (float) texH,
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        color,
        alpha
    );
}

static int32_t glCreateSpriteFromSurface(Renderer* renderer, int32_t surfaceID, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig) {
    // TODO: implement these
    (void)smooth;
    (void)removeback;
    GLRenderer* gl = (GLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 >= w || 0 >= h) return -1;
    if (0 > surfaceID || (uint32_t) surfaceID >= gl->surfaceCount) return -1;
    if (gl->surfaces[surfaceID] == 0) return -1;

    // Flush any pending draws before reading pixels
    flushBatch(gl);

    glBindFramebuffer(GL_FRAMEBUFFER, gl->surfaces[surfaceID]);

    uint8_t* pixels = (uint8_t *)safeMalloc((size_t) w * (size_t) h * 4);
    if (pixels == nullptr) return -1;

    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    // Create a new GL texture from the captured pixels
    GLuint newTexId;
    glGenTextures(1, &newTexId);
    glBindTexture(GL_TEXTURE_2D, newTexId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    free(pixels);

    // Find or allocate a slot for the dynamic sprite's GL texture.
    // In paged mode we allocate a dedicated page slot holding the whole captured region;
    // in pageless mode the TPAG itself owns a GL texture slot (grown by findOrAllocTpagSlot).
    bool usePageSlot = !gl->pagelessTextures;
    uint32_t pageId = 0;
    if (usePageSlot) {
        pageId = findOrAllocTexturePageSlot(gl);
        gl->glTextures[pageId] = newTexId;
        gl->textureWidths[pageId] = w;
        gl->textureHeights[pageId] = h;
        gl->textureLoaded[pageId] = true;
    }

    uint32_t tpagIndex = findOrAllocTpagSlot(gl, dw, gl->originalTpagCount);
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    tpag->sourceX = 0;
    tpag->sourceY = 0;
    tpag->sourceWidth = (uint16_t) w;
    tpag->sourceHeight = (uint16_t) h;
    tpag->targetX = 0;
    tpag->targetY = 0;
    tpag->targetWidth = (uint16_t) w;
    tpag->targetHeight = (uint16_t) h;
    tpag->boundingWidth = (uint16_t) w;
    tpag->boundingHeight = (uint16_t) h;
    if (usePageSlot) {
        // Paged mode: the TPAG references the whole-page texture slot we just allocated.
        tpag->texturePageId = (int16_t) pageId;
    } else {
        // Pageless mode: the TPAG owns its own GL texture slot containing the whole
        // captured region (it IS the whole region, so it covers the entire texture).
        tpag->texturePageId = 0;
        gl->glTextures[tpagIndex] = newTexId;
        gl->textureWidths[tpagIndex] = w;
        gl->textureHeights[tpagIndex] = h;
        gl->textureLoaded[tpagIndex] = true;
    }

    uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, gl->originalSpriteCount);
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    // name was set by DataWin_allocSpriteSlot ("__newsprite<N>"); don't overwrite it here
    sprite->width = (uint32_t) w;
    sprite->height = (uint32_t) h;
    sprite->originX = xorig;
    sprite->originY = yorig;
    sprite->textureCount = 1;
    sprite->tpagIndices = (int32_t *)safeMalloc(sizeof(int32_t));
    sprite->tpagIndices[0] = (int32_t) tpagIndex;
    sprite->maskCount = 0;
    sprite->masks = nullptr;

    logInfo("GL: Created dynamic sprite %u (%dx%d) from surface %d at (%d,%d)\n", spriteIndex, w, h, surfaceID, x, y);
    return (int32_t) spriteIndex;
}

static void glDeleteSprite(Renderer* renderer, int32_t spriteIndex) {
    GLRenderer* gl = (GLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > spriteIndex || dw->sprt.count <= (uint32_t) spriteIndex) return;

    // Refuse to delete original data.win sprites
    if (gl->originalSpriteCount > (uint32_t) spriteIndex) {
        logWarn("GL: Cannot delete data.win sprite %d\n", spriteIndex);
        return;
    }

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return; // already deleted

    // Clean up GL texture and TPAG entries owned by this sprite.
    // Slots with index >= originalTpagCount are dynamically allocated and ours to free.
    repeat(sprite->textureCount, i) {
        int32_t tpagIdx = sprite->tpagIndices[i];
        if (tpagIdx >= 0 && (uint32_t) tpagIdx >= gl->originalTpagCount) {
            TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
            if (gl->pagelessTextures) {
                // Pageless: the TPAG owns its own slot in the TPAG-keyed GL texture arrays.
                if (gl->textureCount > (uint32_t) tpagIdx) {
                    glDeleteTextures(1, &gl->glTextures[tpagIdx]);
                    gl->glTextures[tpagIdx] = 0;
                }
            } else {
                // Paged: the TPAG references a separate page slot; delete that whole-page texture.
                int16_t pageId = tpag->texturePageId;
                if (pageId >= 0 && gl->textureCount > (uint32_t) pageId) {
                    glDeleteTextures(1, &gl->glTextures[pageId]);
                    gl->glTextures[pageId] = 0;
                }
            }
            // Mark TPAG slot as free for reuse
            tpag->texturePageId = -1;
        }
    }

    // Clear the sprite entry so it won't be drawn and can be reused. Preserve `name` across the memset: the slot is still in sprt.count and must keep a valid string for asset_get_index / name lookups.
    free(sprite->tpagIndices);
    const char* keepName = sprite->name;
    memset(sprite, 0, sizeof(Sprite));
    sprite->name = keepName;

    logInfo("GL: Deleted sprite %d\n", spriteIndex);
}

static BlendFactors glGpuGetBlendFactors(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*)renderer;
    BlendFactors ret;
    ret.src = gl->currentSFactor;
    ret.dst = gl->currentDFactor;
    ret.srcAlpha = gl->currentSFactorAlpha;
    ret.dstAlpha = gl->currentDFactorAlpha;
    return ret;
}

static int32_t glGpuGetBlendMode(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    return gl->currentBlendMode;
}

static void glGpuSetBlendMode(Renderer* renderer, int32_t mode) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (mode == gl->currentBlendMode) return;
    flushBatch(gl);

    gl->currentBlendMode = mode;
    gl->currentSFactor = GLCommon_blendModeToSFactor(mode);
    gl->currentDFactor = GLCommon_blendModeToDFactor(mode);
    gl->currentSFactorAlpha = gl->currentSFactor;
    gl->currentDFactorAlpha = gl->currentDFactor;

    glBlendEquation(GLCommon_blendModeToEquation(mode));
    glBlendFunc(gl->currentSFactor, gl->currentDFactor);
}

static void glGpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor, int32_t sfactor_alpha, int32_t dfactor_alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (sfactor == gl->currentSFactor && dfactor == gl->currentDFactor && \
            sfactor_alpha == gl->currentSFactorAlpha && dfactor_alpha == gl->currentDFactorAlpha) return;
    flushBatch(gl);
    gl->currentBlendMode = bm_complex;
    gl->currentSFactor = sfactor;
    gl->currentDFactor = dfactor;
    gl->currentSFactorAlpha = sfactor_alpha;
    gl->currentDFactorAlpha = dfactor_alpha;

    glBlendFuncSeparate(
        GLCommon_blendFactorToGL(sfactor),
        GLCommon_blendFactorToGL(dfactor),
        GLCommon_blendFactorToGL(sfactor_alpha),
        GLCommon_blendFactorToGL(dfactor_alpha)
    );
}

static void glGpuSetBlendEnable(Renderer* renderer, bool enable) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (gl->blendEnable == enable) return;
    flushBatch(gl);
    enable ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    gl->blendEnable = enable;
}

static bool glGpuGetBlendEnable(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    return gl->blendEnable;
}

static void glGpuSetAlphaTestEnable(Renderer* renderer, bool enable) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (gl->alphaTestEnable == enable) return;
    flushBatch(gl);
    gl->alphaTestEnable = enable;
    glShaderSettingsRefresh(renderer);
}

static bool glGpuGetAlphaTestEnable(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    return gl->alphaTestEnable;
}

static void glGpuSetAlphaTestRef(Renderer* renderer, uint8_t ref) {
    GLRenderer* gl = (GLRenderer*) renderer;
    float refF = ref / 255.0f;
    if (gl->alphaTestRef == refF) return;
    flushBatch(gl);
    gl->alphaTestRef = refF;
    glShaderSettingsRefresh(renderer);
}

static void glGpuSetColorWriteEnable(Renderer* renderer, bool red, bool green, bool blue, bool alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (gl->colorWriteR == red && gl->colorWriteG == green && gl->colorWriteB == blue && gl->colorWriteA == alpha) return;
    flushBatch(gl);
    gl->colorWriteR = red;
    gl->colorWriteG = green;
    gl->colorWriteB = blue;
    gl->colorWriteA = alpha;
    glColorMask(red, green, blue, alpha);
}

static void glGpuGetColorWriteEnable(Renderer* renderer, bool* red, bool* green, bool* blue, bool* alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    *red = gl->colorWriteR;
    *green = gl->colorWriteG;
    *blue = gl->colorWriteB;
    *alpha = gl->colorWriteA;
}

static void glGpuSetFog(Renderer* renderer, bool enable, uint32_t color) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (gl->fogEnable == enable && gl->fogColor == color) return;
    flushBatch(gl);
    gl->fogEnable = enable;
    gl->fogColor = color;
    glShaderSettingsRefresh(renderer);
}

static int32_t glShaderGetUniform(Renderer* renderer, int32_t shaderIndex, char* uniform) {
    GLRenderer* gl = (GLRenderer*) renderer;
    int32_t targetShader = (shaderIndex != -1) ? shaderIndex : renderer->currentShader;
    if (targetShader == -1) return -1;

    GMLShader* shader = &gl->gmlShaders[targetShader];
    if (!shader->compiled || shader->shaderId == 0) return -1;

    GLint loc = glGetUniformLocation(shader->shaderId, uniform);
    if (loc != -1) return loc;

    char arrayName[256];
    snprintf(arrayName, sizeof(arrayName), "%s[0]", uniform);
    return glGetUniformLocation(shader->shaderId, arrayName);
}

static GLenum glShaderGetUniformTypeByLocation(GMLShader* shader, int32_t location) {
    if (!shader || location == -1) return GL_NONE;
    for (uint32_t i = 0; i < shader->uniformCount; i++) {
        if (shader->uniforms[i].location == location) {
            return shader->uniforms[i].type;
        }
    }
    return GL_NONE;
}

static void glShaderSetUniformF(Renderer* renderer, int32_t handle, int32_t count, float value1, float value2, float value3, float value4) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (handle == -1 || renderer->currentShader == -1) return;
    flushBatch(gl);

    GMLShader* shader = &gl->gmlShaders[renderer->currentShader];
    GLenum type = glShaderGetUniformTypeByLocation(shader, handle);

    switch (type) {
        case GL_FLOAT:      glUniform1f(handle, value1); break;
        case GL_FLOAT_VEC2: glUniform2f(handle, value1, value2); break;
        case GL_FLOAT_VEC3: glUniform3f(handle, value1, value2, value3); break;
        case GL_FLOAT_VEC4: glUniform4f(handle, value1, value2, value3, value4); break;
        case GL_INT:        glUniform1i(handle, (GLint)value1); break;
        case GL_INT_VEC2:   glUniform2i(handle, (GLint)value1, (GLint)value2); break;
        case GL_INT_VEC3:   glUniform3i(handle, (GLint)value1, (GLint)value2, (GLint)value3); break;
        case GL_INT_VEC4:   glUniform4i(handle, (GLint)value1, (GLint)value2, (GLint)value3, (GLint)value4); break;
        default:
            if (count == 1)      glUniform1f(handle, value1);
            else if (count == 2) glUniform2f(handle, value1, value2);
            else if (count == 3) glUniform3f(handle, value1, value2, value3);
            else if (count >= 4) glUniform4f(handle, value1, value2, value3, value4);
            break;
    }
}

static void glShaderSetUniformFArray(Renderer* renderer, int32_t handle, float* values, uint32_t count) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (handle == -1 || renderer->currentShader == -1 || values == NULL || count == 0) return;
    flushBatch(gl);

    GMLShader* shader = &gl->gmlShaders[renderer->currentShader];
    GLenum type = glShaderGetUniformTypeByLocation(shader, handle);

    switch (type) {
        case GL_FLOAT:      glUniform1fv(handle, count, values); break;
        case GL_FLOAT_VEC2: glUniform2fv(handle, count / 2, values); break;
        case GL_FLOAT_VEC3: glUniform3fv(handle, count / 3, values); break;
        case GL_FLOAT_VEC4: glUniform4fv(handle, count / 4, values); break;
        case GL_FLOAT_MAT2: glUniformMatrix2fv(handle, count / 4, GL_FALSE, values); break;
        case GL_FLOAT_MAT3: glUniformMatrix3fv(handle, count / 9, GL_FALSE, values); break;
        case GL_FLOAT_MAT4: glUniformMatrix4fv(handle, count / 16, GL_FALSE, values); break;
        default:            glUniform1fv(handle, count, values); break;
    }
}

static void glShaderSetUniformI(Renderer* renderer, int32_t handle, int32_t count, int32_t value1, int32_t value2, int32_t value3, int32_t value4) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);
    if (handle == -1 || renderer->currentShader == -1) return;

    GMLShader* shader = &gl->gmlShaders[renderer->currentShader];
    GLenum type = glShaderGetUniformTypeByLocation(shader, handle);

    switch (type) {
        case GL_INT: glUniform1i(handle, value1); break;
        case GL_INT_VEC2: glUniform2i(handle, value1, value2); break;
        case GL_INT_VEC3: glUniform3i(handle, value1, value2, value3); break;
        case GL_INT_VEC4: glUniform4i(handle, value1, value2, value3, value4); break;
        default:
            if (count == 1)      glUniform1i(handle, value1);
            else if (count == 2) glUniform2i(handle, value1, value2);
            else if (count == 3) glUniform3i(handle, value1, value2, value3);
            else if (count >= 4) glUniform4i(handle, value1, value2, value3, value4);
            break;
    }
}

static int32_t glShaderGetSamplerIndex(Renderer* renderer, int32_t shaderIndex, char* uniform) {
    GLRenderer* gl = (GLRenderer*) renderer;
    GMLShader* shader = &gl->gmlShaders[shaderIndex];

    repeat(shader->uniformCount, b) {
        if (strcmp(shader->uniforms[b].name, uniform) == 0) {
            return shader->uniforms[b].samplerSlot;
        }
    }

    fprintf(stderr, "GL: Sampler Index %s not found for shader %d!\n", uniform, shaderIndex);
    return -1;
}

static uint32_t glSpriteGetTexture(Renderer* renderer, int32_t tpagIndex) {
    GLRenderer* gl = (GLRenderer*) renderer;
    TexturePageItem* tpag;
    GLuint texId;
    int32_t texW, texH;
    if (!resolveSpriteTexture(gl, tpagIndex, &tpag, &texId, &texW, &texH, nullptr, nullptr, nullptr, nullptr)) return 0;
    return (uint32_t) (tpagIndex + 1);
}

// Decode a texture handle produced by glSpriteGetTexture (sprite/tpag) or glSurfaceGetTexture (surface)
// back into its GL id and pixel size. *outTpag is set to NULL for surface handles (no tpag sub-region).
// If non-NULL, the UV out-params receive the full-region UV rect for the tpag handle in the resolved
// texture's space (0..1 on the desktop/ES per-tpag path). Returns false for the 0 ("no texture") handle.
static bool glResolveTextureHandle(GLRenderer* gl, uint32_t texHandle, TexturePageItem** outTpag, GLuint* outTexId, int32_t* outTexW, int32_t* outTexH, float* outU0, float* outV0, float* outU1, float* outV1) {
    if (texHandle == 0) return false;
    if (texHandle & GL_SURFACE_TEXTURE_FLAG) {
        uint32_t sid = texHandle & ~GL_SURFACE_TEXTURE_FLAG;
        if (sid >= gl->surfaceCount || gl->surfaceTexture[sid] == 0) return false;
        if (outTpag) *outTpag = nullptr;
        *outTexId = gl->surfaceTexture[sid];
        *outTexW = gl->surfaceWidth[sid];
        *outTexH = gl->surfaceHeight[sid];
        if (outU0) { *outU0 = 0.0f; *outV0 = 0.0f; *outU1 = 1.0f; *outV1 = 1.0f; }
        return true;
    }
    return resolveSpriteTexture(gl, (int32_t) texHandle - 1, outTpag, outTexId, outTexW, outTexH, outU0, outV0, outU1, outV1);
}

// surface_get_texture: returns a handle that texture_get_texel_*/texture_get_uvs/texture_set_stage resolve.
static uint32_t glSurfaceGetTexture(Renderer* renderer, int32_t surfaceID) {
    GLRenderer* gl = (GLRenderer*) renderer;
    if (surfaceID < 0 || (uint32_t) surfaceID >= gl->surfaceCount) return 0;
    if (gl->surfaceTexture[surfaceID] == 0) return 0;
    return GL_SURFACE_TEXTURE_FLAG | (uint32_t) surfaceID;
}

static void glTextureSetStage(Renderer* renderer, int32_t slot, uint32_t texHandle) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);
    if (slot < 0) {
        logWarn("GL: Invalid Texture Stage\n");
        return;
    }
    TexturePageItem* tpag;
    GLuint texID = 0;
    int32_t texW, texH;
    glResolveTextureHandle(gl, texHandle, &tpag, &texID, &texW, &texH, nullptr, nullptr, nullptr, nullptr);
    if (slot == 0) {
        gl->currentTextureId = texID;
    }
    if (slot > MAX_TEXTURE_STAGES) {
        logWarn("GL: Texture Stage Higher Than Max\n");
        return;
    }
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, texID);
    glActiveTexture(GL_TEXTURE1);

}

// Look up a texture's pixel size from the renderer's own tables.
MAYBE_UNUSED static bool lookupTextureSize(GLRenderer* gl, uint32_t texID, int32_t* outW, int32_t* outH) {
    repeat(gl->textureCount, i) {
        if (gl->textureLoaded[i] && gl->glTextures[i] == (GLuint) texID) {
            *outW = gl->textureWidths[i];
            *outH = gl->textureHeights[i];
            return true;
        }
    }

    repeat(gl->textureCount, i) {
        if (gl->surfaceTexture[i] == (GLuint) texID) {
            *outW = gl->surfaceWidth[i];
            *outH = gl->surfaceHeight[i];
            return true;
        }
    }

    return false;
}

static float glTextureGetTexelWidth(Renderer* renderer, uint32_t texHandle) {
    GLRenderer* gl = (GLRenderer*) renderer;
    TexturePageItem* tpag;
    GLuint texId;
    int32_t width = 0, height = 0;
    if (!glResolveTextureHandle(gl, texHandle, &tpag, &texId, &width, &height, nullptr, nullptr, nullptr, nullptr) || 0 >= width) return 1.0f;
    return 1.0f / (float) width;
}

static float glTextureGetTexelHeight(Renderer* renderer, uint32_t texHandle) {
    GLRenderer* gl = (GLRenderer*) renderer;
    TexturePageItem* tpag;
    GLuint texId;
    int32_t width = 0, height = 0;
    if (!glResolveTextureHandle(gl, texHandle, &tpag, &texId, &width, &height, nullptr, nullptr, nullptr, nullptr) || 0 >= height) return 1.0f;
    return 1.0f / (float) height;
}

static bool glTextureGetUVs(Renderer* renderer, uint32_t texHandle, float* outUVs) {
    GLRenderer* gl = (GLRenderer*) renderer;
    TexturePageItem* tpag;
    GLuint texId;
    int32_t width = 0, height = 0;
    float u0, v0, u1, v1;
    if (!glResolveTextureHandle(gl, texHandle, &tpag, &texId, &width, &height, &u0, &v0, &u1, &v1) || 0 >= width || 0 >= height) return false;
    outUVs[0] = u0; outUVs[1] = v0; outUVs[2] = u1; outUVs[3] = v1;
    return true;
}

static bool glShaderIsCompiled(Renderer* renderer, int32_t shaderID) {
    GLRenderer* gl = (GLRenderer*) renderer;
    DataWin* dw = gl->base.dataWin;
    if (0 > shaderID || (uint32_t) shaderID >= dw->shdr.count) return false;
    return gl->gmlShaders[shaderID].compiled;
}

static bool glShadersSupported(void) {
    return true;
}

static void glSetMatrix(Renderer* renderer, int32_t matrixType, Matrix4f matrix) {
    GLRenderer* gl = (GLRenderer*) renderer;
    
    if (memcmp(&renderer->gmlMatrices[matrixType], &matrix, sizeof(Matrix4f)) == 0) return;
    
    flushBatch(gl);
    renderer->gmlMatrices[matrixType] = matrix;
    //yeah just recalculate everything when we change a matrix
    //TODO LATR: only allow these 3 to be changed directly, other ones should only be allowed to be calculated by the rest of the function
    Matrix4f world = renderer->gmlMatrices[MATRIX_WORLD];
    Matrix4f view = renderer->gmlMatrices[MATRIX_VIEW];
    Matrix4f projection = renderer->gmlMatrices[MATRIX_PROJECTION];

    Matrix4f worldView;
    Matrix4f_multiply(&worldView, &view, &world);

    Matrix4f worldViewProjection;
    Matrix4f_multiply(&worldViewProjection, &projection, &worldView);

    renderer->gmlMatrices[MATRIX_WORLD_VIEW] = worldView;
    renderer->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION] = worldViewProjection;


    glShaderSettingsRefresh(renderer);
}

// ===[ Vtable ]===

static RendererVtable glVtable;

// ===[ Public API ]===

Renderer* GLRenderer_create(bool pagelessTextures, size_t pageCacheSize) {
    GLRenderer* gl = (GLRenderer *)safeCalloc(1, sizeof(GLRenderer));

    // ===[ Texture loading mode ]===
    // The caller asks for a mode via `pagelessTextures`, but PLATFORM_VITA with the
    // VitaTextures feature active MUST use paged mode: it loads whole TXTR pages from
    // an external compressed file, so per-TPAG sub-rect extraction isn't possible.
    gl->pagelessTextures = pagelessTextures;
#if defined(PLATFORM_VITA)
    if (VitaTextures_Active()) {
        gl->pagelessTextures = false;
        logInfo("GL: VitaTextures active – forcing paged texture mode\n");
    }
#endif
    gl->pageCacheSize   = pageCacheSize;
    gl->base.vtable = &glVtable;
    glVtable.init = glInit;
    glVtable.destroy = glDestroy;
    glVtable.beginFrame = glBeginFrame;
    glVtable.endFrameInit = glEndFrameInit;
    glVtable.endFrameEnd = glEndFrameEnd;
    glVtable.beginView = glBeginView;
    glVtable.endView = glEndView;
    glVtable.applyProjection = glApplyProjection;
    glVtable.beginGUI = glBeginGUI;
    glVtable.setGuiProjection = glSetGuiProjection;
    glVtable.endGUI = glEndGUI;
    glVtable.drawSprite = glDrawSprite;
    glVtable.drawSpritePos = glDrawSpritePos;
    glVtable.drawSpritePart = glDrawSpritePart;
    glVtable.drawSpritePartColor = glDrawSpritePartColor;
    glVtable.drawRectangle = glDrawRectangle;
    glVtable.drawRectangleColor = glDrawRectangleColor;
    glVtable.drawLine = glDrawLine;
    glVtable.drawLineColor = glDrawLineColor;
    glVtable.drawTriangle = glDrawTriangle;
    glVtable.drawText = glDrawText;
    glVtable.drawTextColor = glDrawTextColor;
    glVtable.flush = glRendererFlush;
    glVtable.clearScreen = glClearScreen;
    glVtable.createSpriteFromSurface = glCreateSpriteFromSurface;
    glVtable.deleteSprite = glDeleteSprite;
    glVtable.gpuGetBlendFactors = glGpuGetBlendFactors;
    glVtable.gpuGetBlendMode = glGpuGetBlendMode;
    glVtable.gpuSetBlendMode = glGpuSetBlendMode;
    glVtable.gpuSetBlendModeExt = glGpuSetBlendModeExt;
    glVtable.gpuSetBlendEnable = glGpuSetBlendEnable;
    glVtable.gpuSetAlphaTestEnable = glGpuSetAlphaTestEnable;
    glVtable.gpuGetAlphaTestEnable = glGpuGetAlphaTestEnable;
    glVtable.gpuSetAlphaTestRef = glGpuSetAlphaTestRef;
    glVtable.gpuSetColorWriteEnable = glGpuSetColorWriteEnable;
    glVtable.gpuGetColorWriteEnable = glGpuGetColorWriteEnable;
    glVtable.gpuSetFog = glGpuSetFog;
    glVtable.gpuGetBlendEnable = glGpuGetBlendEnable;
    glVtable.drawTile = nullptr;
    glVtable.drawSpriteTiled = glDrawSpriteTiled;
    glVtable.createSurface = glCreateSurface;
    glVtable.surfaceExists = glSurfaceExists;
    glVtable.setRenderTarget = glSetRenderTarget;
    glVtable.ensureApplicationSurface = glEnsureApplicationSurface;
    glVtable.surfaceCopy = glSurfaceCopy;
    glVtable.surfaceGetPixels = glSurfaceGetPixels;
    glVtable.getSurfaceWidth = glGetSurfaceWidth;
    glVtable.getSurfaceHeight = glGetSurfaceHeight;
    glVtable.drawSurface = glDrawSurface;
    glVtable.drawSurfaceColor = glDrawSurfaceColor;
    glVtable.drawSurfaceTiled = glDrawSurfaceTiled;
    glVtable.surfaceResize = glSurfaceResize;
    glVtable.surfaceFree = glSurfaceFree;
    glVtable.gpuSetShader = glGpuSetShader,
    glVtable.gpuResetShader = glGpuResetShader,
    glVtable.shaderGetUniform = glShaderGetUniform,
    glVtable.shaderSetUniformF = glShaderSetUniformF,
    glVtable.shaderSetUniformFArray = glShaderSetUniformFArray,
    glVtable.shaderSetUniformI = glShaderSetUniformI,
    glVtable.spriteGetTexture = glSpriteGetTexture,
    glVtable.surfaceGetTexture = glSurfaceGetTexture,
    glVtable.textureGetTexelWidth = glTextureGetTexelWidth,
    glVtable.textureGetTexelHeight = glTextureGetTexelHeight,
    glVtable.textureGetUVs = glTextureGetUVs,
    glVtable.shaderGetSamplerIndex = glShaderGetSamplerIndex,
    glVtable.textureSetStage = glTextureSetStage,
    glVtable.shaderIsCompiled = glShaderIsCompiled,
    glVtable.shadersSupported = glShadersSupported,
    glVtable.setMatrix = glSetMatrix,

    gl->base.drawColor = 0xFFFFFF; // white (BGR)
    gl->base.drawAlpha = 1.0f;
    gl->base.drawFont = -1;
    gl->base.drawHalign = 0;
    gl->base.drawValign = 0;
    gl->base.circlePrecision = 24;
    gl->base.currentShader = -1;
    gl->base.cameraCurrent = 0;
    return (Renderer*) gl;
}
