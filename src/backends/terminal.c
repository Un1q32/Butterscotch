#include "common.h"
#include "platformdefs.h"
#include "gettime.h"
#include "runner_mouse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#else
#include <io.h>
#include <conio.h>
#include <windows.h>
#endif

/* Terminal backend: renders with the software renderer and presents the
 * framebuffer as ASCII art on stdout.
 *
 * Cells are ~twice as tall as wide, so the framebuffer is rendered at
 * cols x (rows*2) and each cell averages its 1x2 pixel block. Frames are
 * emitted differentially with absolute cursor addressing (no CR/LF on
 * the wire), so unchanged frames cost zero bytes.
 *
 * Colour support is selected via environment variables (a leading digit
 * is not a valid shell identifier, so the 256-colour flag spells it out):
 *   TRUECOLOR      set (to anything) -> 24-bit truecolor escapes
 *   TWOFIVESIXCOLOR set (to anything) -> xterm 256-colour escapes
 *   NO_COLOR       set (non-empty)   -> plain monochrome ASCII
 *   none of the above                -> 16-colour ANSI escapes
 */

static Runner *g_runner = NULL;
static int32_t g_width = 0;
static int32_t g_height = 0;
static bool g_initialized = false;
static bool g_headless = false;

static uint32_t *g_fb = NULL;
static int g_fbW = 0;
static int g_fbH = 0;

static bool g_truecolor = false;
static bool g_color256 = false;
static bool g_color16 = false;

static bool g_ttyOut = false;
static bool g_ttyIn = false;
static bool g_altScreen = false;

/* Previous-frame state for differential updates. */
static char *s_prevCh = NULL;
static int *s_prevCol = NULL;
static char *s_curCh = NULL;
static int *s_curCol = NULL;
static int s_prevCols = 0, s_prevRows = 0;
static bool s_prevValid = false;
static int s_termColor = -1; /* colour active on the terminal, -1 = unknown */

#ifndef _WIN32
static struct termios g_origTermios;
static bool g_rawEnabled = false;
#else
static DWORD g_origConsoleMode = 0;
static bool g_consoleModeSaved = false;
#endif

/* Keys held down by terminal input. Terminals only deliver key presses, so
 * we synthesize releases: a key stays down until ~200ms pass without a
 * repeat of that key. */
#define TERM_KEY_TIMEOUT_NS 200000000ULL
static bool s_keyHeld[GML_KEY_COUNT];
static bool s_keySeen[GML_KEY_COUNT];
static uint64_t s_keyLastSeen[GML_KEY_COUNT];

static const char *kRamp = " .:-=+*#%@";
static const int kRampLen = 10;

/* Nearest of the 16 ANSI colours (VGA-ish palette) for terminals with
 * no truecolor/256-colour support. Returns 0-15. */
static int rgbToAnsi16(int r, int g, int b) {
    static const int palette[16][3] = {
        {0, 0, 0}, {170, 0, 0}, {0, 170, 0}, {170, 85, 0},
        {0, 0, 170}, {170, 0, 170}, {0, 170, 170}, {170, 170, 170},
        {85, 85, 85}, {255, 85, 85}, {85, 255, 85}, {255, 255, 85},
        {85, 85, 255}, {255, 85, 255}, {85, 255, 255}, {255, 255, 255}
    };
    long bestDist = -1;
    int best = 7;
    int i;
    for (i = 0; i < 16; i++) {
        long dr = (long)r - palette[i][0];
        long dg = (long)g - palette[i][1];
        long db = (long)b - palette[i][2];
        long dist = dr * dr + dg * dg + db * db;
        if (bestDist < 0 || dist < bestDist) {
            bestDist = dist;
            best = i;
        }
    }
    return best;
}

static int rgbToXterm256(int r, int g, int b) {
    if (r == g && g == b) {
        if (r < 8) return 16;
        if (r > 248) return 231;
        return 232 + (r - 8) * 24 / 247;
    }
    {
        int ri = (r * 5 + 127) / 255;
        int gi = (g * 5 + 127) / 255;
        int bi = (b * 5 + 127) / 255;
        if (ri < 0) ri = 0;
        if (ri > 5) ri = 5;
        if (gi < 0) gi = 0;
        if (gi > 5) gi = 5;
        if (bi < 0) bi = 0;
        if (bi > 5) bi = 5;
        return 16 + 36 * ri + 6 * gi + bi;
    }
}

/* Query the live terminal grid. Tries STDOUT, STDERR and STDIN (whichever
 * is still attached to the tty; stdout is often redirected), then falls
 * back to COLUMNS/LINES, then to 80x24. Returns true when a real terminal
 * size was found, false when falling back to defaults. */
static bool termQuerySize(int *outCols, int *outRows) {
    int cols = 0, rows = 0;
#ifdef _WIN32
    {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h != INVALID_HANDLE_VALUE) {
            CONSOLE_SCREEN_BUFFER_INFO info;
            if (GetConsoleScreenBufferInfo(h, &info)) {
                int w = info.srWindow.Right - info.srWindow.Left + 1;
                int hgt = info.srWindow.Bottom - info.srWindow.Top + 1;
                if (w > 0) cols = w;
                if (hgt > 0) rows = hgt;
            }
        }
    }
#else
    {
        /* ioctl succeeds only on ttys; probe every standard fd because any
         * of them may be redirected while the others stay on the terminal. */
        static const int fds[] = { STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO };
        size_t i;
        for (i = 0; i < sizeof(fds) / sizeof(fds[0]); i++) {
            struct winsize ws;
            memset(&ws, 0, sizeof(ws));
            if (ioctl(fds[i], TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
                cols = ws.ws_col;
                rows = ws.ws_row;
                break;
            }
        }
    }
#endif
    if (cols <= 0 || rows <= 0) {
        const char *c = getenv("COLUMNS");
        const char *l = getenv("LINES");
        if (c != NULL) {
            int v = atoi(c);
            if (v > 0) cols = v;
        }
        if (l != NULL) {
            int v = atoi(l);
            if (v > 0) rows = v;
        }
    }
    if (cols < 1 || rows < 1) {
        cols = 80;
        rows = 24;
        if (outCols) *outCols = cols;
        if (outRows) *outRows = rows;
        return false;
    }
    if (outCols) *outCols = cols;
    if (outRows) *outRows = rows;
    return true;
}

static void termGetSize(int *outCols, int *outRows) {
    int cols = 80, rows = 24;
    (void)termQuerySize(&cols, &rows);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (outCols) *outCols = cols;
    if (outRows) *outRows = rows;
}

/* Terminal cells are about half as wide as they are tall, so the render
 * resolution doubles the grid height: each ASCII cell covers a 1x2 pixel
 * block, and the two vertically stacked pixels are averaged into the one
 * cell. Without this everything looks squished vertically once the tall
 * cells stretch the image back out. */
static void termGridToFb(int cols, int rows, int *outW, int *outH) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (outW) *outW = cols;
    if (outH) *outH = rows * 2;
}

static void termPushKey(int32_t gmlKey, unsigned int ch) {
    if (g_runner == NULL || g_runner->keyboard == NULL) return;
    if (gmlKey < 0 || gmlKey >= GML_KEY_COUNT) {
        if (ch >= 32 && ch < 127) RunnerKeyboard_onCharacter(g_runner->keyboard, ch);
        return;
    }
    RunnerKeyboard_onKeyDown(g_runner->keyboard, gmlKey);
    if (ch >= 32) RunnerKeyboard_onCharacter(g_runner->keyboard, ch);
    s_keyHeld[gmlKey] = true;
    s_keySeen[gmlKey] = true;
    s_keyLastSeen[gmlKey] = nowNanos();
}

/* Parse one CSI sequence body (bytes between ESC [ and the final byte).
 * Returns the mapped GML key, or -1 if unknown. */
static int32_t termCsiToGml(const char *body, size_t len, char final) {
    if (len == 0) {
        switch (final) {
            case 'A': return VK_UP;
            case 'B': return VK_DOWN;
            case 'C': return VK_RIGHT;
            case 'D': return VK_LEFT;
            case 'H': return VK_HOME;
            case 'F': return VK_END;
            case 'Z': return VK_TAB;
            default: return -1;
        }
    }
    if (final == '~') {
        char tmp[16];
        size_t n = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
        memcpy(tmp, body, n);
        tmp[n] = '\0';
        {
            int v = atoi(tmp);
            switch (v) {
                case 1:
                case 7: return VK_HOME;
                case 2: return VK_INSERT;
                case 3: return VK_DELETE;
                case 4:
                case 8: return VK_END;
                case 5: return VK_PAGEUP;
                case 6: return VK_PAGEDOWN;
                case 11: return VK_F1;
                case 12: return VK_F2;
                case 13: return VK_F3;
                case 14: return VK_F4;
                case 15: return VK_F5;
                case 17: return VK_F6;
                case 18: return VK_F7;
                case 19: return VK_F8;
                case 20: return VK_F9;
                case 21: return VK_F10;
                case 23: return VK_F11;
                case 24: return VK_F12;
                default: return -1;
            }
        }
    }
    return -1;
}

static void termHandleByte(unsigned char c, int *escState, char *escBuf, size_t *escLen) {
    /* escState: 0 = normal, 1 = saw ESC, 2 = saw ESC [, 3 = saw ESC O */
    if (*escState == 0) {
        if (c == 0x1B) {
            *escState = 1;
            *escLen = 0;
            return;
        }
        if (c == '\r' || c == '\n') { termPushKey(VK_ENTER, (unsigned int)'\r'); return; }
        if (c == '\t') { termPushKey(VK_TAB, (unsigned int)'\t'); return; }
        if (c == 0x7F || c == 0x08) { termPushKey(VK_BACKSPACE, 8); return; }
        if (c == 0x03 || c == 0x04) {
            /* Ctrl+C / Ctrl+D: handled by caller as quit request; still
             * register a key press so the game sees something. */
            termPushKey('C', 0);
            return;
        }
        if (c < 0x20) {
            /* Other control codes: Ctrl+letter -> letter. */
            if (c >= 0x01 && c <= 0x1A) {
                int32_t k = (int32_t)('A' + c - 1);
                termPushKey(k, (unsigned int)k);
            }
            return;
        }
        if (c >= 'a' && c <= 'z') { int32_t k = (int32_t)(c - 'a' + 'A'); termPushKey(k, (unsigned int)k); return; }
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) { termPushKey((int32_t)c, (unsigned int)c); return; }
        if (c == ' ') { termPushKey(VK_SPACE, (unsigned int)' '); return; }
        if (c < 128) { termPushKey((int32_t)c, (unsigned int)c); return; }
        return;
    }
    if (*escState == 1) {
        if (c == '[') { *escState = 2; *escLen = 0; return; }
        if (c == 'O') { *escState = 3; return; }
        /* ESC followed by a normal key: treat as the key itself (Alt+key). */
        *escState = 0;
        if (c >= 'a' && c <= 'z') { int32_t k = (int32_t)(c - 'a' + 'A'); termPushKey(k, (unsigned int)k); return; }
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) { termPushKey((int32_t)c, (unsigned int)c); return; }
        if (c == 0x1B) { *escState = 1; *escLen = 0; return; }
        termPushKey(VK_ESCAPE, 0);
        if (c >= 0x20 && c < 0x7F) termPushKey((int32_t)c, (unsigned int)c);
        return;
    }
    if (*escState == 3) {
        int32_t k = -1;
        *escState = 0;
        switch (c) {
            case 'P': k = VK_F1; break;
            case 'Q': k = VK_F2; break;
            case 'R': k = VK_F3; break;
            case 'S': k = VK_F4; break;
            case 'H': k = VK_HOME; break;
            case 'F': k = VK_END; break;
            default: break;
        }
        if (k >= 0) termPushKey(k, 0);
        return;
    }
    /* escState == 2: inside CSI, accumulate params until final byte. */
    if (c >= 0x40 && c <= 0x7E) {
        int32_t k = termCsiToGml(escBuf, *escLen, (char)c);
        *escState = 0;
        *escLen = 0;
        if (k >= 0) termPushKey(k, 0);
        return;
    }
    if (*escLen + 1 < 16) {
        escBuf[*escLen] = (char)c;
        (*escLen)++;
    } else {
        *escState = 0;
        *escLen = 0;
    }
}

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless) {
    (void)title;
    if (gfx != SOFTWARE) {
        logError("Terminal backend requires the software renderer (--renderer software)\n");
        return false;
    }
    g_headless = headless;
    g_initialized = true;

    /* Render resolution follows the terminal grid (doubled vertically for
     * the tall cells, see termGridToFb). Only fall back to the requested
     * game window size when there is no terminal to measure (piped
     * output). */
    {
        int cols = 0, rows = 0;
        if (termQuerySize(&cols, &rows)) {
            termGridToFb(cols, rows, &g_width, &g_height);
        } else {
            g_width = reqW > 0 ? reqW : 80;
            g_height = reqH > 0 ? reqH : 24;
        }
    }

    {
        const char *noColor = getenv("NO_COLOR");
        bool noColorSet = noColor != NULL && noColor[0] != '\0';
        g_truecolor = getenv("TRUECOLOR") != NULL;
        g_color256 = !g_truecolor && getenv("TWOFIVESIXCOLOR") != NULL;
        /* 16 colours is the default for terminals without truecolor or
         * 256-colour support; monochrome is opt-in via NO_COLOR. */
        g_color16 = !g_truecolor && !g_color256 && !noColorSet;
    }

#ifndef _WIN32
    g_ttyOut = isatty(STDOUT_FILENO) != 0;
    g_ttyIn = isatty(STDIN_FILENO) != 0;
#else
    g_ttyOut = _isatty(_fileno(stdout)) != 0;
    g_ttyIn = _isatty(_fileno(stdin)) != 0;
#endif

    memset(s_keyHeld, 0, sizeof(s_keyHeld));
    memset(s_keySeen, 0, sizeof(s_keySeen));
    memset(s_keyLastSeen, 0, sizeof(s_keyLastSeen));

#ifndef _WIN32
    if (g_ttyIn) {
        if (tcgetattr(STDIN_FILENO, &g_origTermios) == 0) {
            struct termios raw = g_origTermios;
            raw.c_iflag &= (unsigned int)~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
            /* NOTE: c_oflag (OPOST/ONLCR) is deliberately left alone.
             * termios is shared by the whole tty, so clearing OPOST here
             * would also break our own stdout rendering: '\n' would stop
             * implying a carriage return and every frame would
             * stair-step, scroll and tear. We only need raw *input*. */
            raw.c_cflag |= (unsigned int)(CS8);
            raw.c_lflag &= (unsigned int)~(ECHO | ICANON | IEXTEN | ISIG);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) g_rawEnabled = true;
        }
        /* NOTE: stdin is deliberately left in blocking mode. VMIN=0/VTIME=0
         * above already makes read() return immediately, and setting
         * O_NONBLOCK here would be actively harmful: stdin and stdout
         * usually refer to the same open tty description (e.g. after the
         * test harness's dup2, or a shell that dup'd one open), so the flag
         * would leak onto stdout and large frame writes would fail with
         * EAGAIN partway, leaving stale cells from old frames on screen. */
    }
#else
    if (g_ttyIn) {
        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        if (hIn != INVALID_HANDLE_VALUE && GetConsoleMode(hIn, &g_origConsoleMode)) {
            DWORD mode = g_origConsoleMode;
            mode &= (DWORD)~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
            mode |= (DWORD)ENABLE_VIRTUAL_TERMINAL_INPUT;
            if (SetConsoleMode(hIn, mode)) g_consoleModeSaved = true;
        }
    }
    if (g_ttyOut) {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut != INVALID_HANDLE_VALUE) {
            DWORD mode = 0;
            if (GetConsoleMode(hOut, &mode)) {
                mode |= (DWORD)ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                (void)SetConsoleMode(hOut, mode);
            }
        }
    }
#endif

    if (g_ttyOut && !g_headless) {
        fputs("\x1b[?1049h\x1b[?25l\x1b[H\x1b[2J", stdout);
        fflush(stdout);
        g_altScreen = true;
    }

    {
        const char *mode = g_truecolor ? "truecolor" : g_color256 ? "256-colour" : g_color16 ? "16-colour" : "monochrome (NO_COLOR set)";
        int cols = 0, rows = 0;
        if (termQuerySize(&cols, &rows)) {
            logInfo("Terminal backend: %dx%d cells (%dx%d px framebuffer, %s ASCII)\n",
                    cols, rows, g_width, g_height, mode);
        } else {
            logInfo("Terminal backend: %dx%d (no tty, %s ASCII)\n", g_width, g_height, mode);
        }
    }
    return true;
}

void platformExit(void) {
    if (g_ttyOut && g_altScreen) {
        fputs("\x1b[0m\x1b[?25h\x1b[?1049l", stdout);
        fflush(stdout);
        g_altScreen = false;
    }
#ifndef _WIN32
    if (g_rawEnabled) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &g_origTermios);
        g_rawEnabled = false;
    }
#else
    if (g_consoleModeSaved) {
        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        if (hIn != INVALID_HANDLE_VALUE) (void)SetConsoleMode(hIn, g_origConsoleMode);
        g_consoleModeSaved = false;
    }
#endif
    free(s_prevCh); s_prevCh = NULL;
    free(s_prevCol); s_prevCol = NULL;
    free(s_curCh); s_curCh = NULL;
    free(s_curCol); s_curCol = NULL;
    s_prevCols = 0; s_prevRows = 0;
    s_prevValid = false;
    s_termColor = -1;
    g_initialized = false;
    g_fb = NULL;
}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;
    runner->setCursor = NULL;
    runner->currentCursor = GML_CR_DEFAULT;
}

bool platformGetWindowSize(int32_t *outW, int32_t *outH) {
    int cols = 0, rows = 0;
    if (!outW || !outH) return false;
    if (!g_initialized) return false;
    /* Re-query every call so resizes take effect on the next frame: the
     * runner polls this each frame and the software renderer reallocates
     * its framebuffer when the size changes. */
    if (termQuerySize(&cols, &rows)) {
        termGridToFb(cols, rows, &g_width, &g_height);
    }
    if (g_width < 1) g_width = 80;
    if (g_height < 1) g_height = 24;
    *outW = g_width;
    *outH = g_height;
    return true;
}

bool platformGetScaledWindowSize(int32_t *outW, int32_t *outH) {
    return platformGetWindowSize(outW, outH);
}

void platformSetWindowSize(int32_t width, int32_t height) {
    int cols = 0, rows = 0;
    /* The terminal grid owns the render resolution; ignore game-driven
     * resize requests while a live terminal is present. They only apply
     * to the non-tty (piped) fallback size. */
    if (termQuerySize(&cols, &rows)) {
        termGridToFb(cols, rows, &g_width, &g_height);
        return;
    }
    if (width > 0) g_width = width;
    if (height > 0) g_height = height;
}

void platformSetWindowTitle(const char *title) {
    if (!g_ttyOut || title == NULL) return;
    fprintf(stdout, "\x1b]0;Butterscotch - %s\x07", title);
    fflush(stdout);
}

void platformGetMousePos(double *xPos, double *yPos) {
    if (xPos) *xPos = 0.0;
    if (yPos) *yPos = 0.0;
}

void *platformGetProcAddress(const char *name) {
    (void)name;
    return NULL;
}

void platformSetNextFramebuffer(uint32_t *framebuffer, int width, int height) {
    g_fb = framebuffer;
    g_fbW = width;
    g_fbH = height;
}

/* Worst-case output bytes for one cell (escape + glyph) in the active mode. */
static size_t termPerCell(void) {
    if (g_truecolor) return 20; /* "\x1b[38;2;RRR;GGG;BBBm" + char */
    if (g_color256) return 12;  /* "\x1b[38;5;NNNm" + char */
    if (g_color16) return 6;    /* "\x1b[9Xm" + char */
    return 1;                   /* monochrome: just the char */
}

/* Reduces one framebuffer cell to its ASCII glyph + colour key. */
static void termComputeCell(int tx, int ty, int cols, int rows, char *outCh, int *outKey) {
    unsigned long sumR = 0, sumG = 0, sumB = 0;
    unsigned long count = 0;
    int r, g, b, lum, idx;
    if (g_fbW == cols && g_fbH == rows * 2) {
        /* Fast path: average the two vertically stacked pixels. */
        uint32_t px0 = g_fb[(size_t)(ty * 2) * (size_t)g_fbW + (size_t)tx];
        uint32_t px1 = g_fb[(size_t)(ty * 2 + 1) * (size_t)g_fbW + (size_t)tx];
        sumB = (unsigned long)(px0 & 0xFFu) + (unsigned long)(px1 & 0xFFu);
        sumG = (unsigned long)((px0 >> 8) & 0xFFu) + (unsigned long)((px1 >> 8) & 0xFFu);
        sumR = (unsigned long)((px0 >> 16) & 0xFFu) + (unsigned long)((px1 >> 16) & 0xFFu);
        count = 2;
    } else {
        /* Generic fallback: box-sample (resize races, non-tty sizes). */
        int y0 = ty * g_fbH / rows;
        int y1 = (ty + 1) * g_fbH / rows;
        int x0 = tx * g_fbW / cols;
        int x1 = (tx + 1) * g_fbW / cols;
        int rw, rh, stepX, stepY, y, x;
        if (y1 <= y0) y1 = y0 + 1;
        if (y0 < 0) y0 = 0;
        if (y1 > g_fbH) y1 = g_fbH;
        if (x1 <= x0) x1 = x0 + 1;
        if (x0 < 0) x0 = 0;
        if (x1 > g_fbW) x1 = g_fbW;
        rw = x1 - x0;
        rh = y1 - y0;
        stepX = (rw + 7) / 8;
        stepY = (rh + 7) / 8;
        if (stepX < 1) stepX = 1;
        if (stepY < 1) stepY = 1;
        for (y = y0; y < y1; y += stepY) {
            for (x = x0; x < x1; x += stepX) {
                uint32_t px = g_fb[(size_t)y * (size_t)g_fbW + (size_t)x];
                sumB += (unsigned long)(px & 0xFFu);
                sumG += (unsigned long)((px >> 8) & 0xFFu);
                sumR += (unsigned long)((px >> 16) & 0xFFu);
                count++;
            }
        }
    }
    if (count == 0) { r = 0; g = 0; b = 0; }
    else { r = (int)(sumR / count); g = (int)(sumG / count); b = (int)(sumB / count); }

    /* Rec. 601 luma for the glyph ramp. */
    lum = (r * 299 + g * 587 + b * 114) / 1000;
    idx = lum * (kRampLen - 1) / 255;
    if (idx < 0) idx = 0;
    if (idx >= kRampLen) idx = kRampLen - 1;
    *outCh = kRamp[idx];

    if (g_truecolor) *outKey = (r << 16) | (g << 8) | b;
    else if (g_color256) *outKey = rgbToXterm256(r, g, b);
    else if (g_color16) *outKey = rgbToAnsi16(r, g, b);
    else *outKey = 0;
}

/* Appends the colour escape for key. Returns false if it wouldn't fit. */
static bool termEmitColor(char **pp, size_t *lenp, size_t cap, int key) {
    char *p = *pp;
    size_t len = *lenp;
    int n;
    if (g_truecolor) {
        n = snprintf(p, cap - len, "\x1b[38;2;%d;%d;%dm", (key >> 16) & 255, (key >> 8) & 255, key & 255);
    } else if (g_color256) {
        n = snprintf(p, cap - len, "\x1b[38;5;%dm", key);
    } else {
        n = snprintf(p, cap - len, "\x1b[%dm", key < 8 ? 30 + key : 90 + (key - 8));
    }
    if (n < 0 || (size_t)n >= cap - len) return false;
    *pp = p + n;
    *lenp = len + (size_t)n;
    return true;
}

static void termPresent(void) {
    int cols, rows;
    size_t n, i;
    int tx, ty;
    bool useColor, full;
    long changed, runs;
    size_t cap, len;
    char *buf, *p;

    if (g_fb == NULL || g_fbW <= 0 || g_fbH <= 0) return;
    if (g_headless) return;

    termGetSize(&cols, &rows);
    if (cols < 1) cols = 80;
    if (rows < 1) rows = 24;

    n = (size_t)cols * (size_t)rows;

    /* (Re)allocate the cell buffers when the grid changes; the previous
     * frame is then invalid so the next present fully redraws. */
    if (cols != s_prevCols || rows != s_prevRows) {
        free(s_prevCh); s_prevCh = NULL;
        free(s_prevCol); s_prevCol = NULL;
        free(s_curCh); s_curCh = NULL;
        free(s_curCol); s_curCol = NULL;
        s_prevCh = (char *)malloc(n);
        s_prevCol = (int *)malloc(n * sizeof(int));
        s_curCh = (char *)malloc(n);
        s_curCol = (int *)malloc(n * sizeof(int));
        if (s_prevCh == NULL || s_prevCol == NULL || s_curCh == NULL || s_curCol == NULL) {
            free(s_prevCh); s_prevCh = NULL;
            free(s_prevCol); s_prevCol = NULL;
            free(s_curCh); s_curCh = NULL;
            free(s_curCol); s_curCol = NULL;
            s_prevCols = 0; s_prevRows = 0;
            s_prevValid = false;
            return;
        }
        s_prevCols = cols;
        s_prevRows = rows;
        s_prevValid = false;
    }

    useColor = g_truecolor || g_color256 || g_color16;
    full = !s_prevValid;

    /* Pass 1: reduce every cell, diff against the previous frame. */
    changed = 0;
    for (ty = 0; ty < rows; ty++) {
        for (tx = 0; tx < cols; tx++) {
            char ch;
            int key;
            i = (size_t)ty * (size_t)cols + (size_t)tx;
            termComputeCell(tx, ty, cols, rows, &ch, &key);
            s_curCh[i] = ch;
            s_curCol[i] = key;
            if (full || ch != s_prevCh[i] || key != s_prevCol[i]) changed++;
        }
    }
    /* Identical frame: emit zero bytes. */
    if (!full && changed == 0) return;

    /* Count runs so the output buffer is sized exactly for this update:
     * changed cells at the active mode's worst case, one absolute cursor
     * address per run, plus framing. Lower colour modes allocate less. */
    if (full) {
        runs = rows;
    } else {
        runs = 0;
        for (ty = 0; ty < rows; ty++) {
            bool inRun = false;
            for (tx = 0; tx < cols; tx++) {
                i = (size_t)ty * (size_t)cols + (size_t)tx;
                if (s_curCh[i] != s_prevCh[i] || s_curCol[i] != s_prevCol[i]) {
                    if (!inRun) { runs++; inRun = true; }
                } else {
                    inRun = false;
                }
            }
        }
    }

    if (g_ttyOut) {
        cap = 19 + (size_t)runs * 16 + (size_t)changed * termPerCell() + 4 + 32;
    } else {
        cap = n * (termPerCell() + 1) + (size_t)rows + 32;
    }
    buf = (char *)malloc(cap);
    if (buf == NULL) return;
    p = buf;
    len = 0;

    if (g_ttyOut) {
        /* Absolute cursor addressing: no CR/LF bytes on the wire, so a
         * frame can never scroll the screen no matter what. Only changed
         * runs are emitted, inside synchronized output so the update
         * presents atomically where supported. */
        bool force = true;
        bool ok = true;
        memcpy(p, "\x1b[?2026h", 8);
        p += 8;
        len += 8;
        for (ty = 0; ok && ty < rows; ty++) {
            int runStart = -1;
            for (tx = 0; ok && tx <= cols; tx++) {
                bool dirty;
                if (tx < cols) {
                    i = (size_t)ty * (size_t)cols + (size_t)tx;
                    dirty = full || s_curCh[i] != s_prevCh[i] || s_curCol[i] != s_prevCol[i];
                } else {
                    dirty = false;
                }
                if (dirty && runStart < 0) runStart = tx;
                if (!dirty && runStart >= 0) {
                    int x, nn;
                    nn = snprintf(p, cap - len, "\x1b[%d;%dH", ty + 1, runStart + 1);
                    if (nn < 0 || (size_t)nn >= cap - len) { ok = false; break; }
                    p += nn;
                    len += (size_t)nn;
                    for (x = runStart; x < tx; x++) {
                        int key;
                        i = (size_t)ty * (size_t)cols + (size_t)x;
                        key = s_curCol[i];
                        if (useColor && (force || key != s_termColor)) {
                            if (!termEmitColor(&p, &len, cap, key)) { ok = false; break; }
                            s_termColor = key;
                            force = false;
                        }
                        if (len + 1 >= cap) { ok = false; break; }
                        *p++ = s_curCh[i];
                        len++;
                    }
                    runStart = -1;
                }
            }
        }
        if (ok && useColor) {
            if (len + 4 >= cap) {
                ok = false;
            } else {
                memcpy(p, "\x1b[0m", 4);
                p += 4;
                len += 4;
                s_termColor = -1;
            }
        }
        if (ok) {
            if (len + 8 >= cap) {
                ok = false;
            } else {
                memcpy(p, "\x1b[?2026l", 8);
                p += 8;
                len += 8;
            }
        }
        if (ok) {
            (void)fwrite(buf, 1, len, stdout);
            fflush(stdout);
            memcpy(s_prevCh, s_curCh, n);
            memcpy(s_prevCol, s_curCol, n * sizeof(int));
            s_prevValid = true;
        }
        free(buf);
        return;
    }

    /* Non-tty (piped): full grid with newlines, only when changed. */
    {
        bool force = true;
        bool ok = true;
        for (ty = 0; ok && ty < rows; ty++) {
            for (tx = 0; tx < cols; tx++) {
                int key;
                i = (size_t)ty * (size_t)cols + (size_t)tx;
                key = s_curCol[i];
                if (useColor && (force || key != s_termColor)) {
                    if (!termEmitColor(&p, &len, cap, key)) { ok = false; break; }
                    s_termColor = key;
                    force = false;
                }
                if (len + 1 >= cap) { ok = false; break; }
                *p++ = s_curCh[i];
                len++;
            }
            if (!ok) break;
            if (len + 1 >= cap) { ok = false; break; }
            *p++ = '\n';
            len++;
        }
        if (ok && useColor) {
            if (len + 4 >= cap) {
                ok = false;
            } else {
                memcpy(p, "\x1b[0m", 4);
                p += 4;
                len += 4;
                s_termColor = -1;
            }
        }
        if (ok) {
            (void)fwrite(buf, 1, len, stdout);
            fflush(stdout);
            memcpy(s_prevCh, s_curCh, n);
            memcpy(s_prevCol, s_curCol, n * sizeof(int));
            s_prevValid = true;
        }
        free(buf);
    }
}

void platformSwapBuffers(void) {
    termPresent();
}

bool platformHandleEvents(void) {
    int escState = 0;
    char escBuf[16];
    size_t escLen = 0;
    bool quit = false;
    int i;

    if (g_runner == NULL || g_runner->keyboard == NULL) return false;

    memset(s_keySeen, 0, sizeof(s_keySeen));

    if (g_ttyIn) {
#ifndef _WIN32
        for (;;) {
            unsigned char chunk[256];
            ssize_t n = read(STDIN_FILENO, chunk, sizeof(chunk));
            if (n <= 0) break;
            {
                ssize_t k;
                for (k = 0; k < n; k++) {
                    if (chunk[k] == 0x03 || chunk[k] == 0x04) quit = true;
                    termHandleByte(chunk[k], &escState, escBuf, &escLen);
                }
            }
            if (n < (ssize_t)sizeof(chunk)) break;
        }
#else
        while (_kbhit()) {
            int c = _getch();
            if (c < 0) break;
            if (c == 0 || c == 0xE0) {
                int c2 = _getch();
                int32_t k = -1;
                switch (c2) {
                    case 72: k = VK_UP; break;
                    case 80: k = VK_DOWN; break;
                    case 75: k = VK_LEFT; break;
                    case 77: k = VK_RIGHT; break;
                    case 71: k = VK_HOME; break;
                    case 79: k = VK_END; break;
                    case 73: k = VK_PAGEUP; break;
                    case 81: k = VK_PAGEDOWN; break;
                    case 82: k = VK_INSERT; break;
                    case 83: k = VK_DELETE; break;
                    case 59: k = VK_F1; break;
                    case 60: k = VK_F2; break;
                    case 61: k = VK_F3; break;
                    case 62: k = VK_F4; break;
                    case 63: k = VK_F5; break;
                    case 64: k = VK_F6; break;
                    case 65: k = VK_F7; break;
                    case 66: k = VK_F8; break;
                    case 67: k = VK_F9; break;
                    case 68: k = VK_F10; break;
                    default: break;
                }
                if (k >= 0) termPushKey(k, 0);
                continue;
            }
            if (c == 0x03 || c == 0x04) quit = true;
            termHandleByte((unsigned char)c, &escState, escBuf, &escLen);
        }
#endif
        if (escState == 1) {
            /* Lone ESC with nothing following: it is the Escape key. */
            termPushKey(VK_ESCAPE, 0);
        }
    }

    /* Synthesize releases for keys with no recent repeat. */
    {
        uint64_t now = nowNanos();
        for (i = 0; i < GML_KEY_COUNT; i++) {
            if (!s_keyHeld[i]) continue;
            if (s_keySeen[i]) continue;
            if (now - s_keyLastSeen[i] >= TERM_KEY_TIMEOUT_NS) {
                s_keyHeld[i] = false;
                RunnerKeyboard_onKeyUp(g_runner->keyboard, (int32_t)i);
            }
        }
    }

    if (InputRecording_isPlaybackActive(globalInputRecording)) {
        /* Suppress real input during playback like the other backends do;
         * quit requests (Ctrl+C/D) still go through. */
    }

    return quit;
}

void platformSleepUntil(uint64_t time) {
    int64_t remaining = (int64_t)time - (int64_t)nowNanos();
    if (remaining > 2000000) {
        remaining -= 1000000;
#ifdef _WIN32
        Sleep((DWORD)(remaining / 1000000));
#else
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = remaining;
        nanosleep(&ts, NULL);
#endif
    }
    while (nowNanos() < time) {
        YIELD();
    }
}
