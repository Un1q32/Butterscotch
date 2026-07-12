#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common.h"
#include "desktop/platformdefs.h"
#include "gettime.h"

static Runner *g_runner;
static int32_t fbWidth, fbHeight;

void platformSetWindowTitle(const char* title) {
    (void)title;
}

bool platformGetWindowSize(int32_t* outW, int32_t* outH) {
    if (!outW || !outH) return false;
    *outW = fbWidth;
    *outH = fbHeight;
    return true;
}

bool platformGetScaledWindowSize(int32_t* outW, int32_t* outH) {
    return platformGetWindowSize(outW, outH);
}

void platformSetWindowSize(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return;
    fbWidth = width;
    fbHeight = height;
}

void platformGetMousePos(double *xPos, double *yPos) {
    if (!xPos || !yPos) return;
    *xPos = 0.0;
    *yPos = 0.0;
}

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless) {
    (void)title;
    (void)headless;
    fbWidth = reqW;
    fbHeight = reqH;
    return true;
}

void platformExit(void) {}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;
    runner->currentCursor = GML_CR_DEFAULT;
}

#ifdef ENABLE_SW_RENDERER

void Runner_setNextFrame(uint32_t* framebuffer, int width, int height) {
    (void)width;
    (void)height;
    (void)framebuffer;
}

#endif

void platformSwapBuffers(void) {
    RunnerKeyboard_onKeyDown(g_runner->keyboard, VK_BACKSPACE);
    RunnerKeyboard_onKeyUp(g_runner->keyboard, VK_BACKSPACE);
}

void *platformGetProcAddress(const char *name) {
    (void)name;
    return NULL;
}

bool platformHandleEvents(void) {
    return false;
}

void platformSleepUntil(uint64_t time) {
    int64_t remaining = time - nowNanos();
    if (remaining > 2000000) {
        remaining -= 1000000;
#ifdef _WIN32
        Sleep(remaining / 1000000);
#else
        struct timespec ts;
        ts.tv_sec  = 0;
        ts.tv_nsec = remaining;
        nanosleep(&ts, NULL);
#endif
    }
    while (nowNanos() < time) {
        // Spin-wait for the remaining sub-millisecond
        YIELD();
    }
}
