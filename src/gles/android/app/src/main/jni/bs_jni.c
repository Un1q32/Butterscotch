// Butterscotch Android — JNI bridge
// =================================================================
//
// This is the Android equivalent of the per-game bring-up + frame loop
// that src/gles/ios/main.m does inside BSGameViewController. The Java
// side (GameActivity + GLSurfaceView.Renderer) owns the EGL context and
// the UI; this file owns the Butterscotch C runtime and is called from
// the GL thread.
//
// Lifecycle (all called on the GLSurfaceView GL thread unless noted):
//
//   nativeLoad(dataWinPath, saveDir)      → parse data.win, build VM,
//                                            renderer, runner, audio.
//   nativeSurfaceChanged(w, h)            → remember the framebuffer size.
//   nativeStep()                          → one display tick: fixed-step
//                                            the game, draw a frame.
//   nativeKeyDown/Up(gmlKey)              → feed the GML keyboard (called
//                                            from the UI thread; guarded).
//   nativeTeardown()                      → free the whole runtime.
//   nativeOnTrimMemory()                  → evict GL atlases + audio cache.
//
// The renderer (../gles1_renderer.c) is already platform-neutral: on
// non-Apple builds it includes <GLES/gl.h> and uses the GL_*_OES FBO
// entry points, which is exactly what Android's GLES 1.1 driver exposes.

#include <jni.h>
#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>

#include <GLES/gl.h>

#include "data_win.h"
#include "vm.h"
#include "runner.h"
#include "renderer.h"
#include "audio_system.h"
#include "file_system.h"
#include "noop_audio_system.h"
#include "overlay_file_system.h"
#include "runner_keyboard.h"

// miniaudio-backed audio system. On Gingerbread miniaudio drops to its
// OpenSL|ES backend automatically (AAudio needs API 26+), giving us
// multi-voice PCM mixing + OGG streaming without any OpenAL dependency
// (Android has no system OpenAL, unlike iOS).
#include "ma_audio_system.h"

// GLES1Renderer entry points (defined in ../gles1_renderer.c).
Renderer* GLES1Renderer_create(void);
void GLES1Renderer_setDataWinPath(Renderer* r, const char* path);
void GLES1Renderer_handleMemoryWarning(Renderer* r);

#ifndef BS_GIT_VERSION
#define BS_GIT_VERSION "unknown"
#endif

#define LOG_TAG "Butterscotch"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ============================================================================
// Runtime state — the moral equivalent of BSGameViewController's ivars.
// ============================================================================

static DataWin*      g_dataWin    = NULL;
static VMContext*    g_vm         = NULL;
static Renderer*     g_renderer   = NULL;
static FileSystem*   g_fileSystem = NULL;
static AudioSystem*  g_audio      = NULL;
static Runner*       g_runner     = NULL;

static bool   g_runtimeReady = false;
static int    g_fbWidth      = 0;
static int    g_fbHeight     = 0;

// Fixed-timestep accumulator (see iOS glViewTick for the rationale).
static double g_lastTickTime    = 0.0;
static double g_stepAccumulator = 0.0;
static int32_t g_lastRoomIndex  = -1;
static long    g_logicFrameCount = 0;

// The keyboard is touched from the UI thread (button presses) and the GL
// thread (beginFrame clear). RunnerKeyboard mutates plain bool arrays, so
// a tiny mutex keeps that race honest without measurable cost.
static pthread_mutex_t g_kbLock = PTHREAD_MUTEX_INITIALIZER;

static double nowSeconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

// ============================================================================
// nativeLoad — parse data.win and stand up VM + renderer + runner + audio.
// Mirrors BSGameViewController.loadRuntime in src/gles/ios/main.m.
// Returns a status string (empty on success) so the Java side can show a
// toast / HUD line on failure.
// ============================================================================

static void teardownLocked(void); // fwd

JNIEXPORT jstring JNICALL
Java_com_mrpowergamerbr_butterscotch_NativeBridge_nativeLoad(
        JNIEnv* env, jclass clazz, jstring jDataWinPath, jstring jSaveDir) {
    (void) clazz;

    // Defensive: if a previous game is still resident, tear it down first.
    if (g_runtimeReady || g_dataWin != NULL) {
        LOGW("nativeLoad called while a runtime is resident — tearing down old one first");
        teardownLocked();
    }

    const char* dataWinPath = (*env)->GetStringUTFChars(env, jDataWinPath, NULL);
    const char* saveDir     = (*env)->GetStringUTFChars(env, jSaveDir, NULL);

    LOGI("nativeLoad: data.win=%s saveDir=%s (build %s)", dataWinPath, saveDir, BS_GIT_VERSION);

    // ---- 1. Parse data.win -------------------------------------------------
    // Same options the iOS port uses: lazy rooms + streamed TXTR/AUDO blobs
    // to keep peak RAM low on a 768 MB-class Gingerbread device, and skip
    // shaders entirely (GLES 1.1 fixed-function path has no shader pipeline).
    DataWinParserOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.parseGen8 = true;
    opts.parseOptn = true;
    opts.parseLang = true;
    opts.parseExtn = false;
    opts.parseSond = true;
    opts.parseAgrp = true;
    opts.parseSprt = true;
    opts.parseBgnd = true;
    opts.parsePath = true;
    opts.parseScpt = true;
    opts.parseGlob = true;
    opts.parseShdr = false;
    opts.parseFont = true;
    opts.parseTmln = true;
    opts.parseObjt = true;
    opts.parseRoom = true;
    opts.parseTpag = true;
    opts.parseCode = true;
    opts.parseVari = true;
    opts.parseFunc = true;
    opts.parseStrg = true;
    opts.parseTxtr = true;
    opts.parseAudo = true;
    opts.skipLoadingPreciseMasksForNonPreciseSprites = true;
    opts.lazyLoadRooms = true;
    opts.skipLoadingTxtrBlobs = true;
    opts.skipLoadingAudoBlobs = true;
    opts.eagerlyLoadedRooms = NULL;
    opts.progressCallback = NULL;
    opts.progressCallbackUserData = NULL;

    g_dataWin = DataWin_parse(dataWinPath, opts);
    if (g_dataWin == NULL) {
        LOGE("DataWin_parse returned NULL for %s", dataWinPath);
        (*env)->ReleaseStringUTFChars(env, jDataWinPath, dataWinPath);
        (*env)->ReleaseStringUTFChars(env, jSaveDir, saveDir);
        return (*env)->NewStringUTF(env, "Failed to parse data.win (unsupported version or corrupt file)");
    }
    LOGI("data.win parsed: name=%s bytecodeVer=%u default=%ux%u",
         g_dataWin->gen8.name ? g_dataWin->gen8.name : "(null)",
         (unsigned) g_dataWin->gen8.wadVersion,
         (unsigned) g_dataWin->gen8.defaultWindowWidth,
         (unsigned) g_dataWin->gen8.defaultWindowHeight);

    // ---- 2. VM -------------------------------------------------------------
    g_vm = VM_create(g_dataWin);
    if (g_vm == NULL) {
        LOGE("VM_create failed");
        teardownLocked();
        (*env)->ReleaseStringUTFChars(env, jDataWinPath, dataWinPath);
        (*env)->ReleaseStringUTFChars(env, jSaveDir, saveDir);
        return (*env)->NewStringUTF(env, "VM_create failed");
    }

    // ---- 3. File system ----------------------------------------------------
    // The game's bundled assets live next to data.win; writable saves are
    // redirected to the app-private saveDir (Android's sandbox blocks
    // writes next to assets extracted from the APK, same as iOS seatbelt).
    char dataWinDir[1024];
    {
        strncpy(dataWinDir, dataWinPath, sizeof(dataWinDir) - 1);
        dataWinDir[sizeof(dataWinDir) - 1] = '\0';
        char* lastSlash = strrchr(dataWinDir, '/');
        if (lastSlash != NULL) *lastSlash = '\0';
        else strcpy(dataWinDir, ".");
    }
    LOGI("bundle dir: %s   save dir: %s", dataWinDir, saveDir);
    OverlayFileSystem* overlay = OverlayFileSystem_create(dataWinDir, saveDir);
    g_fileSystem = (FileSystem*) overlay;

    // ---- 4. Audio ----------------------------------------------------------
    // miniaudio → OpenSL|ES on Gingerbread. Falls back to silent audio if
    // the device's audio HAL refuses to open (rare, but be graceful).
    g_audio = (AudioSystem*) MaAudioSystem_create();
    if (g_audio == NULL) {
        LOGW("MaAudioSystem_create failed, falling back to silent audio");
        g_audio = (AudioSystem*) NoopAudioSystem_create();
    }

    // ---- 5. Renderer (GLES 1.1) -------------------------------------------
    // A valid GLES 1.1 context is already current on this thread — the
    // GLSurfaceView GL thread is the one calling us.
    g_renderer = GLES1Renderer_create();
    GLES1Renderer_setDataWinPath(g_renderer, dataWinPath);

    // ---- 6. Runner ---------------------------------------------------------
    g_runner = Runner_create(g_dataWin, g_vm, g_renderer, g_fileSystem, g_audio);
    g_runner->osType = OS_ANDROID;

    LOGI("Runner_initFirstRoom");
    Runner_initFirstRoom(g_runner);
    LOGI("first room: %s",
         g_runner->currentRoom != NULL ? g_runner->currentRoom->name : "(NULL)");

    g_runtimeReady    = true;
    g_lastTickTime    = nowSeconds();
    g_stepAccumulator = 0.0;
    g_lastRoomIndex   = -1;
    g_logicFrameCount = 0;

    (*env)->ReleaseStringUTFChars(env, jDataWinPath, dataWinPath);
    (*env)->ReleaseStringUTFChars(env, jSaveDir, saveDir);
    return (*env)->NewStringUTF(env, ""); // empty == success
}

// ============================================================================
// nativeSurfaceChanged — record the GL framebuffer size.
// ============================================================================

JNIEXPORT void JNICALL
Java_com_mrpowergamerbr_butterscotch_NativeBridge_nativeSurfaceChanged(
        JNIEnv* env, jclass clazz, jint width, jint height) {
    (void) env; (void) clazz;
    g_fbWidth  = (int) width;
    g_fbHeight = (int) height;
    LOGI("surfaceChanged: %dx%d", g_fbWidth, g_fbHeight);
}

// ============================================================================
// nativeStep — one display tick. Direct port of iOS glViewTick.
// ============================================================================

JNIEXPORT void JNICALL
Java_com_mrpowergamerbr_butterscotch_NativeBridge_nativeStep(
        JNIEnv* env, jclass clazz) {
    (void) env; (void) clazz;

    if (!g_runtimeReady) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    double now = nowSeconds();
    double dt = now - g_lastTickTime;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1; // clamp huge stalls (GC pause, app switch)
    g_lastTickTime = now;

    uint32_t roomSpeed = 30;
    if (g_runner != NULL && g_runner->currentRoom != NULL && g_runner->currentRoom->speed > 0) {
        roomSpeed = g_runner->currentRoom->speed;
    }
    double stepDt = 1.0 / (double) roomSpeed;
    g_stepAccumulator += dt;
    if (g_stepAccumulator > 4.0 * stepDt) g_stepAccumulator = 4.0 * stepDt;

    int stepsThisTick = 0;
    while (g_stepAccumulator >= stepDt) {
        pthread_mutex_lock(&g_kbLock);
        Runner_step(g_runner);
        pthread_mutex_unlock(&g_kbLock);
        if (g_audio != NULL) g_audio->vtable->update(g_audio, (float) stepDt);
        Runner_handlePendingRoomChange(g_runner);
        g_stepAccumulator -= stepDt;
        g_logicFrameCount += 1;
        stepsThisTick++;
        if (stepsThisTick >= 4) break;
    }

    bool roomChanged = false;
    if (g_renderer != NULL && g_runner != NULL) {
        int32_t roomIdx = g_runner->currentRoomIndex;
        if (g_lastRoomIndex >= 0 && roomIdx != g_lastRoomIndex) {
            LOGI("room change %d->%d — purge atlases after beginFrame", g_lastRoomIndex, roomIdx);
            roomChanged = true;
        }
        g_lastRoomIndex = roomIdx;
    }

    // No logic step this tick (display faster than room speed) → skip the
    // draw, re-presenting the same frame just burns battery.
    if (stepsThisTick == 0) return;

    int32_t gameW = (int32_t) g_dataWin->gen8.defaultWindowWidth;
    int32_t gameH = (int32_t) g_dataWin->gen8.defaultWindowHeight;
    if (gameW <= 0) gameW = g_fbWidth;
    if (gameH <= 0) gameH = g_fbHeight;

    float scaleX = 1.0f, scaleY = 1.0f;
    Runner_computeViewDisplayScale(g_runner, gameW, gameH, &scaleX, &scaleY);

    g_renderer->vtable->beginFrame(g_renderer, gameW, gameH, g_fbWidth, g_fbHeight);

    if (roomChanged) {
        GLES1Renderer_handleMemoryWarning(g_renderer);
    }

    if (g_runner->drawBackgroundColor) {
        uint32_t c = g_runner->backgroundColor;
        float rF = ((c >> 0)  & 0xFF) / 255.0f;
        float gF = ((c >> 8)  & 0xFF) / 255.0f;
        float bF = ((c >> 16) & 0xFF) / 255.0f;
        glClearColor(rF, gF, bF, 1.0f);
    } else {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    }
    glClear(GL_COLOR_BUFFER_BIT);

    Runner_drawViews(g_runner, gameW, gameH, scaleX, scaleY, false);

    g_renderer->vtable->endFrameInit(g_renderer);
    g_renderer->vtable->endFrameEnd(g_renderer);

    // Clear pressed/released edges AFTER both step and draw consumed them
    // (games can read input in Draw events, e.g. Undertale's naming screen).
    if (g_runner != NULL && g_runner->keyboard != NULL) {
        pthread_mutex_lock(&g_kbLock);
        RunnerKeyboard_beginFrame(g_runner->keyboard);
        pthread_mutex_unlock(&g_kbLock);
    }

    if (g_logicFrameCount % 60 == 0) {
        LOGI("frame %ld (game %dx%d, fb %dx%d, dt=%.2fms rs=%u steps=%d)",
             g_logicFrameCount, gameW, gameH, g_fbWidth, g_fbHeight,
             dt * 1000.0, (unsigned) roomSpeed, stepsThisTick);
    }
}

// ============================================================================
// Input — fed from the Java UI thread (touch overlay buttons).
// ============================================================================

JNIEXPORT void JNICALL
Java_com_mrpowergamerbr_butterscotch_NativeBridge_nativeKeyDown(
        JNIEnv* env, jclass clazz, jint gmlKey) {
    (void) env; (void) clazz;
    if (g_runner != NULL && g_runner->keyboard != NULL && gmlKey > 0) {
        pthread_mutex_lock(&g_kbLock);
        RunnerKeyboard_onKeyDown(g_runner->keyboard, (int32_t) gmlKey);
        pthread_mutex_unlock(&g_kbLock);
    }
}

JNIEXPORT void JNICALL
Java_com_mrpowergamerbr_butterscotch_NativeBridge_nativeKeyUp(
        JNIEnv* env, jclass clazz, jint gmlKey) {
    (void) env; (void) clazz;
    if (g_runner != NULL && g_runner->keyboard != NULL && gmlKey > 0) {
        pthread_mutex_lock(&g_kbLock);
        RunnerKeyboard_onKeyUp(g_runner->keyboard, (int32_t) gmlKey);
        pthread_mutex_unlock(&g_kbLock);
    }
}

// Optional: feed a touch position into the GML mouse, mapped to game
// coordinates. The UI passes normalized [0,1] coords; we scale to the
// game's logical resolution so mouse_x / mouse_y read sensibly.
JNIEXPORT void JNICALL
Java_com_mrpowergamerbr_butterscotch_NativeBridge_nativeTouch(
        JNIEnv* env, jclass clazz, jfloat nx, jfloat ny) {
    (void) env; (void) clazz;
    if (g_runner == NULL || g_dataWin == NULL) return;
    int32_t gameW = (int32_t) g_dataWin->gen8.defaultWindowWidth;
    int32_t gameH = (int32_t) g_dataWin->gen8.defaultWindowHeight;
    if (gameW <= 0) gameW = g_fbWidth;
    if (gameH <= 0) gameH = g_fbHeight;
    double mx = (double) nx * (double) gameW;
    double my = (double) ny * (double) gameH;
    Runner_updateMousePosition(g_runner, gameW, gameH, mx, my);
}

// ============================================================================
// Memory pressure — onTrimMemory / onLowMemory from Java.
// ============================================================================

JNIEXPORT void JNICALL
Java_com_mrpowergamerbr_butterscotch_NativeBridge_nativeOnTrimMemory(
        JNIEnv* env, jclass clazz) {
    (void) env; (void) clazz;
    LOGW("onTrimMemory — purging non-current atlases");
    if (g_renderer != NULL) {
        GLES1Renderer_handleMemoryWarning(g_renderer);
    }
}

// ============================================================================
// Teardown — free everything in the same order iOS teardownRuntime uses.
// ============================================================================

static void teardownLocked(void) {
    if (g_audio != NULL) {
        if (g_audio->vtable->stopAll != NULL) g_audio->vtable->stopAll(g_audio);
        g_audio->vtable->destroy(g_audio);
        g_audio = NULL;
    }
    if (g_renderer != NULL) {
        // The renderer owns GL objects tied to the (current) EGL context.
        g_renderer->vtable->destroy(g_renderer);
        g_renderer = NULL;
    }
    if (g_runner != NULL) { Runner_free(g_runner); g_runner = NULL; }
    if (g_fileSystem != NULL) {
        OverlayFileSystem_destroy((OverlayFileSystem*) g_fileSystem);
        g_fileSystem = NULL;
    }
    if (g_vm != NULL) { VM_free(g_vm); g_vm = NULL; }
    if (g_dataWin != NULL) { DataWin_free(g_dataWin); g_dataWin = NULL; }
    g_runtimeReady = false;
}

JNIEXPORT void JNICALL
Java_com_mrpowergamerbr_butterscotch_NativeBridge_nativeTeardown(
        JNIEnv* env, jclass clazz) {
    (void) env; (void) clazz;
    LOGI("nativeTeardown");
    pthread_mutex_lock(&g_kbLock);
    teardownLocked();
    pthread_mutex_unlock(&g_kbLock);
}

JNIEXPORT jstring JNICALL
Java_com_mrpowergamerbr_butterscotch_NativeBridge_nativeVersion(
        JNIEnv* env, jclass clazz) {
    (void) clazz;
    return (*env)->NewStringUTF(env, BS_GIT_VERSION);
}
