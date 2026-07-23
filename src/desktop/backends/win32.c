/*
 * platform_win32.c
 *
 * Pure Win32 backend for Butterscotch. Software rendering only -- no
 * OpenGL, no SDL. Draws into a top-down 32bpp DIB section and blits it
 * to the window on WM_PAINT / platformSwapBuffers.
 *
 * Assumptions carried over from the reference implementation:
 *   - ANSI (non-UNICODE) build: WNDCLASS/CreateWindowEx/WM_CHAR all use
 *     the narrow API, matching the original snippet's use of `char*`
 *     titles and RegisterClass (not RegisterClassW).
 *   - GML_MB_* mouse-button and GML_CR_* cursor constants are
 *     Butterscotch-specific (from desktop/platformdefs.h) and are NOT
 *     the same values as Win32's own constants, so they still need an
 *     explicit mapping.
 *   - GML's keyboard codes (used by RunnerKeyboard_on{Key,Character})
 *     are the *same* numeric values as native Win32 virtual-key codes
 *     (this is true historically for GameMaker's vk_* constants), so
 *     WM_KEYDOWN/WM_KEYUP's wParam can be forwarded directly with no
 *     translation table, unlike the SDL backend which had to map SDL
 *     keysyms onto those codes.
 *   - No resolution-fallback logic: if the requested mode can't be
 *     created, platformInit just fails instead of trying anything else.
 */

#include <windows.h>
#include <string.h>
#include <stdio.h>

#include "common.h"
#include "input_recording.h"
#include "desktop/platformdefs.h"
#include "gettime.h"
#include "runner_mouse.h"

static const char *wndClassName = "ButterscotchWndClass";

static Runner *g_runner;
static HWND hwnd;
static bool isHeadless;
static bool g_quitRequested;

/* Software framebuffer (top-down 32bpp DIB section) */
static HDC memDC;
static HBITMAP dib;
static void *fbPixels;
static int32_t fbWidth, fbHeight;

/* Latest frame handed to us by the runner, blitted into fbPixels on swap */
static const uint32_t *nextFbPixels;
static int32_t nextFbWidth, nextFbHeight;

static LRESULT CALLBACK WindowProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam);
static bool CreateFramebufferDIB(int32_t w, int32_t h);

/* ---------------------------------------------------------------------- */

void platformSetWindowTitle(const char *title) {
    char windowTitle[256];
    snprintf(windowTitle, sizeof(windowTitle), "Butterscotch - %s", title);
    if (hwnd) SetWindowTextA(hwnd, windowTitle);
}

bool platformGetWindowSize(int32_t *outW, int32_t *outH) {
    if (!outW || !outH) return false;
    *outW = fbWidth;
    *outH = fbHeight;
    return true;
}

bool platformGetScaledWindowSize(int32_t *outW, int32_t *outH) {
    /* No HiDPI scaling handling for the plain Win32 backend -- client
       pixels are framebuffer pixels 1:1. */
    return platformGetWindowSize(outW, outH);
}

void platformSetWindowSize(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return;

    if (!CreateFramebufferDIB(width, height)) {
        fprintf(stderr, "platformSetWindowSize: failed to resize framebuffer to %dx%d\n", width, height);
        return;
    }

    if (hwnd) {
        DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
        RECT r = {0, 0, width, height};
        AdjustWindowRect(&r, style, FALSE);
        SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void platformGetMousePos(double *xPos, double *yPos) {
    if (!xPos || !yPos) return;
    POINT p;
    if (!hwnd || !GetCursorPos(&p)) {
        *xPos = 0.0;
        *yPos = 0.0;
        return;
    }
    ScreenToClient(hwnd, &p);
    *xPos = (double)p.x;
    *yPos = (double)p.y;
}

static bool platformGetWindowFocus(void) {
    return hwnd && GetForegroundWindow() == hwnd;
}

/* ---------------------------------------------------------------------- */

static int32_t WinMouseButtonToGml(UINT msg) {
    switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:   return GML_MB_LEFT;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:   return GML_MB_RIGHT;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:   return GML_MB_MIDDLE;
        default:              return -1;
    }
}

static LRESULT CALLBACK WindowProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KEYDOWN: {
            /* bit 30 of lParam = "was this key already down" -> autorepeat */
            if (lParam & (1 << 30)) return 0;
            if (InputRecording_isPlaybackActive(globalInputRecording)) return 0;
            RunnerKeyboard_onKeyDown(g_runner->keyboard, (int32_t)wParam);
            return 0;
        }
        case WM_KEYUP: {
            if (InputRecording_isPlaybackActive(globalInputRecording)) return 0;
            RunnerKeyboard_onKeyUp(g_runner->keyboard, (int32_t)wParam);
            return 0;
        }
        case WM_CHAR: {
            if (InputRecording_isPlaybackActive(globalInputRecording)) return 0;
            if (wParam != 0)
                RunnerKeyboard_onCharacter(g_runner->keyboard, (int32_t)wParam);
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            if (InputRecording_isPlaybackActive(globalInputRecording)) return 0;
            int32_t gmlBtn = WinMouseButtonToGml(msg);
            if (gmlBtn >= 0) RunnerMouse_onButtonDown(g_runner->mouse, gmlBtn);
            SetCapture(h);
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP: {
            if (InputRecording_isPlaybackActive(globalInputRecording)) return 0;
            int32_t gmlBtn = WinMouseButtonToGml(msg);
            if (gmlBtn >= 0) RunnerMouse_onButtonUp(g_runner->mouse, gmlBtn);
            ReleaseCapture();
            return 0;
        }
#ifdef GET_WHEEL_DELTA_WPARAM
        case 0x020A: {
            if (InputRecording_isPlaybackActive(globalInputRecording)) return 0;
            double delta = (double)GET_WHEEL_DELTA_WPARAM(wParam) / 120.0;
            RunnerMouse_onWheel(g_runner->mouse, delta);
            return 0;
        }
#endif
        case WM_SIZE: {
            int32_t w = LOWORD(lParam);
            int32_t hgt = HIWORD(lParam);
            if (w > 0 && hgt > 0 && (w != fbWidth || hgt != fbHeight)) {
                CreateFramebufferDIB(w, hgt);
            }
            return 0;
        }
        case WM_ERASEBKGND:
            /* We repaint the whole client area every frame; avoid flicker
               from the default background erase. */
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(h, &ps);
            if (memDC) {
                BitBlt(hdc, 0, 0, fbWidth, fbHeight, memDC, 0, 0, SRCCOPY);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CLOSE:
            g_quitRequested = true;
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            hwnd = NULL;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(h, msg, wParam, lParam);
}

/* ---------------------------------------------------------------------- */

static bool CreateFramebufferDIB(int32_t w, int32_t h) {
    if (dib) {
        DeleteObject(dib);
        dib = NULL;
        fbPixels = NULL;
    }
    if (memDC) {
        DeleteDC(memDC);
        memDC = NULL;
    }

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; /* negative = top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screenDC = GetDC(NULL);
    memDC = CreateCompatibleDC(screenDC);
    ReleaseDC(NULL, screenDC);

    if (!memDC) {
        fprintf(stderr, "Fatal: CreateCompatibleDC failed: %lu\n", GetLastError());
        return false;
    }

    dib = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &fbPixels, NULL, 0);
    if (!dib || !fbPixels) {
        fprintf(stderr, "Fatal: CreateDIBSection failed: %lu\n", GetLastError());
        DeleteDC(memDC);
        memDC = NULL;
        return false;
    }

    SelectObject(memDC, dib);

    fbWidth = w;
    fbHeight = h;
    return true;
}

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless) {
    isHeadless = headless;
    g_quitRequested = false;

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASS wc = {0};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = wndClassName;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL; /* we own all painting */

        if (!RegisterClass(&wc)) {
            fprintf(stderr, "Fatal: RegisterClass failed: %lu\n", GetLastError());
            return false;
        }
        classRegistered = true;
    }

    if (!headless) {
        DWORD style = WS_OVERLAPPEDWINDOW;

        RECT r = {0, 0, reqW, reqH};
        AdjustWindowRect(&r, style, FALSE);

        hwnd = CreateWindowEx(
            0, wndClassName, title, style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            r.right - r.left, r.bottom - r.top,
            NULL, NULL, GetModuleHandle(NULL), NULL
        );

        if (!hwnd) {
            fprintf(stderr, "Fatal: Could not create window: %lu\n", GetLastError());
            return false;
        }
    } else {
        hwnd = NULL;
    }

    /* Software framebuffer, needed whether headless or not */
    if (!CreateFramebufferDIB(reqW, reqH)) {
        if (hwnd) DestroyWindow(hwnd);
        hwnd = NULL;
        return false;
    }

    if (hwnd) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    return true;
}

void platformExit(void) {
    if (dib) {
        DeleteObject(dib);
        dib = NULL;
        fbPixels = NULL;
    }
    if (memDC) {
        DeleteDC(memDC);
        memDC = NULL;
    }
    if (hwnd) {
        DestroyWindow(hwnd);
        hwnd = NULL;
    }
}

/* ---------------------------------------------------------------------- */

static bool g_cursorVisible = true;

static void platformSetCursor(int32_t cursorType) {
    bool wantVisible = (cursorType != GML_CR_NONE);
    if (wantVisible != g_cursorVisible) {
        ShowCursor(wantVisible);
        g_cursorVisible = wantVisible;
    }
}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;
    runner->windowHasFocus = platformGetWindowFocus;
    runner->setCursor = platformSetCursor;
    runner->currentCursor = GML_CR_DEFAULT;
}

/* ---------------------------------------------------------------------- */

void Runner_setNextFrame(uint32_t *framebuffer, int width, int height) {
    /* Just remember the pointer/size; the actual copy into the DIB
       happens in platformSwapBuffers so we only touch fbPixels once
       per frame. Caller owns `framebuffer` and must keep it valid
       until the next call or until after platformSwapBuffers returns. */
    nextFbPixels = framebuffer;
    nextFbWidth = width;
    nextFbHeight = height;
}

void platformSwapBuffers(void) {
    if (!nextFbPixels || !fbPixels) return;

    /* Framebuffer is assumed to already be in 0x00RRGGBB (top-down,
       BGRX-in-memory) order, matching the DIB's BI_RGB layout, so a
       straight row-by-row copy (clamped to the DIB's current size) is
       sufficient -- no per-pixel conversion needed. */
    int32_t copyW = (nextFbWidth < fbWidth) ? nextFbWidth : fbWidth;
    int32_t copyH = (nextFbHeight < fbHeight) ? nextFbHeight : fbHeight;

    const uint8_t *src = (const uint8_t *)nextFbPixels;
    uint8_t *dst = (uint8_t *)fbPixels;
    size_t srcStride = (size_t)nextFbWidth * 4;
    size_t dstStride = (size_t)fbWidth * 4;

    for (int32_t y = 0; y < copyH; y++) {
        memcpy(dst + (size_t)y * dstStride, src + (size_t)y * srcStride, (size_t)copyW * 4);
    }

    if (hwnd) {
        InvalidateRect(hwnd, NULL, FALSE);
        UpdateWindow(hwnd); /* forces a synchronous WM_PAINT, like SDL_Flip */
    }
}

/* ---------------------------------------------------------------------- */

bool platformHandleEvents(void) {
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            g_quitRequested = true;
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return g_quitRequested;
}

void platformSleepUntil(uint64_t time) {
    int64_t remaining = time - nowNanos();
    //if (remaining > 2000000) {
    //    remaining -= 1000000;
    //    Sleep((DWORD)(remaining / 1000000));
    //}
    while (nowNanos() < time) {
        YIELD();
    }
}
