// The X11 core protocol types 'Font' and 'Drawable' collide with Butterscotch's
// own types of the same name, so rename them while the X11 headers are parsed.
#define Font X11FontTy
#define Drawable X11DrawableTy
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#undef Drawable
#undef Font

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "string_compat.h"
#include "stdio_compat.h"
#include "common.h"
#include "input_recording.h"
#include "desktop/platformdefs.h"
#include "gettime.h"
#include "runner_mouse.h"

// Pure X11 backend. Software renderer ONLY.
//
// This backend talks to the server exclusively through the X11 core protocol
// (libX11), so it should work against just about any X server, including 80s
// and 90s era ones. All it needs is a way to blast pixels, which the server
// provides through XPutImage on whichever visual the root window uses.

static Runner *g_runner;
static Display *g_dpy;
static Window g_win;
static GC g_gc;
static Colormap g_cmap;
static int g_screen;
static Visual *g_visual;
static int g_visualDepth;
static int g_bitsPerPixel;
static int g_bytesPerPixel;
static bool g_headless;
static bool g_quitPending;
static bool g_hasFocus;
static bool g_needsRedraw;
static int32_t g_viewW, g_viewH;
static double g_pointerX, g_pointerY;
static Atom g_wmDeleteAtom;

static uint32_t *g_nextFb;
static int g_nextW, g_nextH;

// Visual / pixel format of the window.
static bool g_direct;                 // TrueColor or DirectColor
static bool g_pseudo;                 // PseudoColor / GrayScale / StaticColor / StaticGray
static bool g_hasAlpha;               // 32-bit TrueColor visuals carry an alpha byte
static int g_rShift, g_gShift, g_bShift, g_aShift;
static int g_rPrec, g_gPrec, g_bPrec, g_aPrec;
static uint8_t *g_palLut;             // 32x32x32 RGB quantisation -> palette pixel

// Reusable upload image + backing buffer.
static XImage *g_img;
static unsigned char *g_imgData;
static int g_imgW, g_imgH;

static Cursor g_cursor;
static int32_t g_curCursorGml;

static int x11PopCount(unsigned long v) {
    int c = 0;
    while (v) { v &= v - 1; c++; }
    return c;
}

static int x11Ctz(unsigned long v) {
    int s = 0;
    while (v && !(v & 1)) { v >>= 1; s++; }
    return s;
}

// Xlib renames Visual's class field to c_class for C++/c_plusplus compiles;
// pick the same field name the installed headers do.
#if defined(__cplusplus) || defined(c_plusplus)
#define X11VCLASS(v) ((v)->c_class)
#else
#define X11VCLASS(v) ((v)->class)
#endif

static int x11VisualClass(void) {
    if (!g_visual) return -1;
#if defined(__cplusplus) || defined(c_plusplus)
    return g_visual->c_class;   // C++-flavoured headers name the member c_class
#else
    return g_visual->class;
#endif
}

// Analyse the root screen's visual and set up our pixel conversion state.
static int x11AnalyzeVisual(void) {
    int cls = x11VisualClass();

    if (cls == TrueColor || cls == DirectColor) {
        unsigned long rMask = g_visual->red_mask;
        unsigned long m = g_visual->green_mask;
        unsigned long bkMask = g_visual->blue_mask;

        g_direct = true;
        g_rShift = x11Ctz(rMask);
        g_gShift = x11Ctz(m);
        g_bShift = x11Ctz(bkMask);
        g_rPrec = x11PopCount(rMask);
        g_gPrec = x11PopCount(m);
        g_bPrec = x11PopCount(bkMask);

        // There is no alpha mask in the Visual struct; on 32-bit TrueColor
        // visuals the high byte is conventionally the alpha channel.
        if (g_visualDepth == 32) {
            g_hasAlpha = true;
            g_aShift = 24;
            g_aPrec = 8;
        } else {
            g_hasAlpha = false;
            g_aShift = 0;
            g_aPrec = 0;
        }
    } else if (cls == PseudoColor || cls == GrayScale || cls == StaticColor || cls == StaticGray) {
        g_pseudo = true;
    } else {
        logError("X11: Unsupported visual class %d\n", cls);
        return 0;
    }

    g_bitsPerPixel = g_visualDepth;
    if (g_bitsPerPixel == 24)
        g_bitsPerPixel = 32; // 24-bit TrueColor images are carried 32 bits per pixel
    g_bytesPerPixel = g_bitsPerPixel / 8;
    if (g_visualDepth == 1)
        g_bytesPerPixel = 1;

    return 1;
}

// Scale a channel from 8 bits to the visual's precision and shift it into place.
static unsigned long x11PackChannel(uint8_t v, int shift, int prec) {
    unsigned long maxv = (1UL << prec) - 1;
    return ((unsigned long)v * maxv + 127) / 255 << shift;
}

static inline uint32_t x11PackPixel(uint32_t argb) {
    uint32_t px =
        (uint32_t)x11PackChannel((uint8_t)(argb >> 16), g_rShift, g_rPrec) |
        (uint32_t)x11PackChannel((uint8_t)(argb >> 8),  g_gShift, g_gPrec) |
        (uint32_t)x11PackChannel((uint8_t)(argb),       g_bShift, g_bPrec);
    if (g_hasAlpha)
        px |= (uint32_t)x11PackChannel((uint8_t)(argb >> 24), g_aShift, g_aPrec);
    return px;
}

// Build a colourmap + palette for PseudoColor-style visuals (common on 8-bit
// displays, i.e. most of the 80s). A 32x32x32 lookup table maps any RGB to the
// cheapest palette pixel so per-frame conversion is cheap.
static void x11BuildPalette(void) {
    int cls = x11VisualClass();
    if (!(cls == PseudoColor || cls == GrayScale || cls == StaticColor || cls == StaticGray))
        return;

    const bool isGray = (cls == GrayScale || cls == StaticGray);
    const bool allocatable = (cls == PseudoColor || cls == GrayScale);
    const int palSize = (g_visualDepth >= 8) ? 256 : (1 << g_visualDepth);

    uint8_t palR[256], palG[256], palB[256], palPixel[256];

    if (isGray) {
        for (int i = 0; i < palSize; i++) {
            uint8_t lvl = (uint8_t)((i * 255U) / (palSize - 1));
            palR[i] = palG[i] = palB[i] = lvl;
        }
    } else if (g_visualDepth == 8) {
        // Classic 3-3-2 cube: 8 red x 8 green x 4 blue.
        for (int i = 0; i < 256; i++) {
            int r = (i >> 5) & 7, gr = (i >> 2) & 7, bl = i & 3;
            palR[i] = (uint8_t)(r * 255 / 7);
            palG[i] = (uint8_t)(gr * 255 / 7);
            palB[i] = (uint8_t)(bl * 255 / 3);
        }
    } else if (palSize == 16) {
        // 4 red x 2 green x 2 blue.
        for (int i = 0; i < 16; i++) {
            int r = (i >> 2) & 3, gr = (i >> 1) & 1, bl = i & 1;
            palR[i] = (uint8_t)(r * 255 / 3);
            palG[i] = gr ? 255 : 0;
            palB[i] = bl ? 255 : 0;
        }
    } else {
        palR[0] = palG[0] = palB[0] = 0;
        if (palSize > 1) palR[1] = palG[1] = palB[1] = 255;
    }

    bool allAll = false;
    g_cmap = 0;

    // For an allocatable visual we can own the whole colour map, which is both
    // faster and more reliable than grabbing individual cells from a shared map.
    if (allocatable && g_visualDepth == 8) {
        g_cmap = XCreateColormap(g_dpy, DefaultRootWindow(g_dpy), g_visual, AllocAll);
        if (g_cmap)
            allAll = true;
    }
    if (!g_cmap)
        g_cmap = XCreateColormap(g_dpy, DefaultRootWindow(g_dpy), g_visual, AllocNone);
    if (!g_cmap)
        g_cmap = DefaultColormap(g_dpy, g_screen);

    if (allAll) {
        XColor *cols = (XColor *) malloc((size_t)palSize * sizeof(XColor));
        if (!cols)
            return;
        for (int i = 0; i < palSize; i++) {
            cols[i].pixel = (unsigned long)i;
            cols[i].red   = (unsigned short)((palR[i] << 8) | palR[i]);
            cols[i].green = (unsigned short)((palG[i] << 8) | palG[i]);
            cols[i].blue  = (unsigned short)((palB[i] << 8) | palB[i]);
            cols[i].flags = DoRed | DoGreen | DoBlue;
        }
        XStoreColors(g_dpy, g_cmap, cols, palSize);
        for (int i = 0; i < palSize; i++)
            palPixel[i] = (uint8_t)i;
        free(cols);
    } else {
        for (int i = 0; i < palSize; i++) {
            XColor col;
            col.pixel = 0;
            col.red   = (unsigned short)((palR[i] << 8) | palR[i]);
            col.green = (unsigned short)((palG[i] << 8) | palG[i]);
            col.blue  = (unsigned short)((palB[i] << 8) | palB[i]);
            col.flags = DoRed | DoGreen | DoBlue;
            if (XAllocColor(g_dpy, g_cmap, &col))
                palPixel[i] = (uint8_t)(col.pixel & 0xff);
            else
                palPixel[i] = (uint8_t)i; // leave unmapped cells harmless
        }
    }

    g_palLut = (uint8_t *) malloc(32 * 32 * 32);
    if (!g_palLut)
        return;

    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++)
            for (int k = 0; k < 32; k++) {
                long rr = (i << 3) | 3;
                long gg = (j << 3) | 3;
                long bb = (k << 3) | 3;
                int best = 0;
                long bestD = LONG_MAX;
                for (int p = 0; p < palSize; p++) {
                    long dr = rr - palR[p];
                    long dg = gg - palG[p];
                    long db = bb - palB[p];
                    long d = dr * dr + dg * dg + db * db;
                    if (d < bestD) { bestD = d; best = p; }
                }
                g_palLut[(i << 10) | (j << 5) | k] = palPixel[best];
            }
}

static void x11FreeImage(void) {
    if (g_imgData) {
        free(g_imgData);
        g_imgData = NULL;
    }
    if (g_img) {
        g_img->data = NULL;
        XDestroyImage(g_img);
        g_img = NULL;
    }
    g_imgW = g_imgH = 0;
}

static XImage *x11EnsureImage(int w, int h) {
    if (g_img && g_imgW == w && g_imgH == h)
        return g_img;

    x11FreeImage();

    int pad = 32;
    if (g_visualDepth <= 8)
        pad = 8;
    else if (g_visualDepth <= 16)
        pad = 16;

    g_img = XCreateImage(g_dpy, g_visual, g_visualDepth, ZPixmap, 0, NULL,
                         (unsigned)w, (unsigned)h, pad, 0);
    if (!g_img)
        return NULL;

    // We always write pixel data little-endian, in MSB-first bit order for
    // monochrome; XPutImage honours these fields, so this works on any server.
    g_img->byte_order = LSBFirst;
    g_img->bitmap_bit_order = MSBFirst;
    g_imgW = w;
    g_imgH = h;

    g_imgData = (unsigned char *) malloc((size_t)g_img->bytes_per_line * (size_t)h);
    if (!g_imgData) {
        g_img->data = NULL;
        XDestroyImage(g_img);
        g_img = NULL;
        g_imgW = g_imgH = 0;
        return NULL;
    }
    memset(g_imgData, 0, (size_t)g_img->bytes_per_line * (size_t)h);
    g_img->data = (char *)g_imgData;
    return g_img;
}

static inline uint8_t x11PaletteIndex(uint32_t argb) {
    uint8_t r = (uint8_t)(argb >> 16);
    uint8_t gg = (uint8_t)(argb >> 8);
    uint8_t b = (uint8_t)argb;
    return g_palLut[((r >> 3) << 10) | ((gg >> 3) << 5) | (b >> 3)];
}

static void x11UploadFrame(void) {
    if (!g_dpy || g_headless || !g_win)
        return;
    if (!g_nextFb || g_nextW <= 0 || g_nextH <= 0)
        return;

    int w = g_nextW, h = g_nextH;
    XImage *img = x11EnsureImage(w, h);
    if (!img)
        return;

    int rowBytes = img->bytes_per_line;
    uint8_t *buf = g_imgData;

    if (g_direct) {
        for (int y = 0; y < h; y++) {
            const uint32_t *src = g_nextFb + (size_t)y * (size_t)w;
            uint8_t *dst = buf + (size_t)y * (size_t)rowBytes;
            switch (g_bytesPerPixel) {
                case 1:
                    for (int x = 0; x < w; x++)
                        dst[x] = (uint8_t)x11PackPixel(src[x]);
                    break;
                case 2:
                    for (int x = 0; x < w; x++) {
                        uint32_t px = x11PackPixel(src[x]);
                        dst[x * 2] = (uint8_t)(px);
                        dst[x * 2 + 1] = (uint8_t)(px >> 8);
                    }
                    break;
                case 4:
                    for (int x = 0; x < w; x++) {
                        uint32_t px = x11PackPixel(src[x]);
                        dst[x * 4] = (uint8_t)(px);
                        dst[x * 4 + 1] = (uint8_t)(px >> 8);
                        dst[x * 4 + 2] = (uint8_t)(px >> 16);
                        dst[x * 4 + 3] = (uint8_t)(px >> 24);
                    }
                    break;
                default:
                    return;
            }
        }
    } else if (g_pseudo) {
        if (g_visualDepth == 1) {
            // Monochrome ZPixmap: 8 pixels packed per byte, MSB first.
            for (int y = 0; y < h; y++) {
                const uint32_t *src = g_nextFb + (size_t)y * (size_t)w;
                uint8_t *dst = buf + (size_t)y * (size_t)rowBytes;
                for (int x = 0; x < w; x++) {
                    if (x11PaletteIndex(src[x]))
                        dst[x >> 3] |= (uint8_t)(0x80u >> (x & 7));
                }
            }
        } else {
            for (int y = 0; y < h; y++) {
                const uint32_t *src = g_nextFb + (size_t)y * (size_t)w;
                uint8_t *dst = buf + (size_t)y * (size_t)rowBytes;
                for (int x = 0; x < w; x++)
                    dst[x] = x11PaletteIndex(src[x]);
            }
        }
    } else {
        return;
    }

    XPutImage(g_dpy, g_win, g_gc, img, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
    XFlush(g_dpy);
}

void Runner_setNextFrame(uint32_t *framebuffer, int width, int height) {
    g_nextFb = framebuffer;
    g_nextW = width;
    g_nextH = height;
}

void platformSetWindowTitle(const char *title) {
    char windowTitle[256];
    if (!g_dpy || !g_win)
        return;
    snprintf(windowTitle, sizeof(windowTitle), "Butterscotch - %s", title);
    XStoreName(g_dpy, g_win, windowTitle);
    XSetIconName(g_dpy, g_win, windowTitle);
    XFlush(g_dpy);
}

bool platformGetWindowSize(int32_t *outW, int32_t *outH) {
    if (!outW || !outH)
        return false;
    *outW = g_viewW;
    *outH = g_viewH;
    return g_viewW > 0 && g_viewH > 0;
}

bool platformGetScaledWindowSize(int32_t *outW, int32_t *outH) {
    return platformGetWindowSize(outW, outH);
}

void platformSetWindowSize(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0)
        return;
    g_viewW = width;
    g_viewH = height;
    if (!g_dpy || !g_win)
        return;
    XResizeWindow(g_dpy, g_win, (unsigned)width, (unsigned)height);
    XFlush(g_dpy);
}

void platformGetMousePos(double *xPos, double *yPos) {
    if (!xPos || !yPos)
        return;
    if (g_headless || !g_dpy || !g_win) {
        *xPos = g_pointerX;
        *yPos = g_pointerY;
        return;
    }
    Window root, child;
    int rx, ry, wx, wy;
    unsigned int mask;
    if (XQueryPointer(g_dpy, g_win, &root, &child, &rx, &ry, &wx, &wy, &mask)) {
        g_pointerX = wx;
        g_pointerY = wy;
    }
    *xPos = g_pointerX;
    *yPos = g_pointerY;
}

static bool platformGetWindowFocus(void) {
    if (!g_dpy || g_headless || !g_win)
        return false;
    if (g_hasFocus)
        return true;

    // Fall back to asking the server which window holds the X input focus (and
    // whether it is us or one of our ancestors).
    Window rootW = DefaultRootWindow(g_dpy);
    Window focus = rootW;
    int revert;
    XGetInputFocus(g_dpy, &focus, &revert);
    Window node = focus;
    while (node && node != rootW) {
        if (node == g_win)
            return true;
        Window rootN, parent;
        Window *children = NULL;
        unsigned int nchildren = 0;
        if (!XQueryTree(g_dpy, node, &rootN, &parent, &children, &nchildren))
            break;
        if (children)
            XFree(children);
        if (!parent)
            break;
        node = parent;
    }
    return false;
}

static Cursor x11BlankCursor(void) {
    Pixmap src = XCreatePixmap(g_dpy, g_win, 1, 1, 1);
    Pixmap mask = XCreatePixmap(g_dpy, g_win, 1, 1, 1);
    XGCValues gv;
    GC gc = XCreateGC(g_dpy, src, 0, &gv);
    XSetForeground(g_dpy, gc, 0);
    XDrawPoint(g_dpy, src, gc, 0, 0);
    XFillRectangle(g_dpy, mask, gc, 0, 0, 1, 1);
    Cursor c = XCreatePixmapCursor(g_dpy, src, mask, NULL, NULL, 0, 0);
    XFreeGC(g_dpy, gc);
    XFreePixmap(g_dpy, src);
    XFreePixmap(g_dpy, mask);
    return c;
}

static unsigned int x11CursorShape(int32_t t) {
    switch (t) {
        case GML_CR_CROSS:     return XC_crosshair;
        case GML_CR_BEAM:      return XC_xterm;
        case GML_CR_SIZE_NS:   return XC_sb_v_double_arrow;
        case GML_CR_SIZE_WE:   return XC_sb_h_double_arrow;
        case GML_CR_SIZE_NESW: return XC_bottom_left_corner;
        case GML_CR_SIZE_NWSE: return XC_bottom_right_corner;
        case GML_CR_SIZE_ALL:  return XC_fleur;
        case GML_CR_HOURGLASS: return XC_watch;
        case GML_CR_UPARROW:   return XC_sb_up_arrow;
        case GML_CR_DRAG:
        case GML_CR_HANDPOINT: return XC_hand1;
        case GML_CR_APPSTART:  return XC_arrow;
        default:               return XC_left_ptr;
    }
}

static void platformSetCursor(int32_t cursorType) {
    Cursor cursor;
    if (!g_dpy || g_headless || !g_win)
        return;
    if (cursorType == g_curCursorGml)
        return;

    if (cursorType == GML_CR_NONE) {
        cursor = x11BlankCursor();
    } else {
        cursor = XCreateFontCursor(g_dpy, x11CursorShape(cursorType));
        if (!cursor)
            cursor = XCreateFontCursor(g_dpy, XC_left_ptr);
    }
    if (!cursor)
        return;

    XDefineCursor(g_dpy, g_win, cursor);
    XFlush(g_dpy);
    if (g_cursor)
        XFreeCursor(g_dpy, g_cursor);
    g_cursor = cursor;
    g_curCursorGml = cursorType;
}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;
    runner->windowHasFocus = platformGetWindowFocus;
    runner->setCursor = platformSetCursor;
    runner->currentCursor = GML_CR_DEFAULT;
}

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless) {
    const char *err = NULL;

    if (gfx != SOFTWARE) {
        logError("The X11 backend only supports the software renderer\n");
        return false;
    }

    g_headless = headless;
    g_viewW = reqW;
    g_viewH = reqH;

    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        if (g_headless)
            return false; // fully headless: no X server needed at all
        logError("Failed to open X display: %s\n", XDisplayName(NULL) ? XDisplayName(NULL) : "(null)");
        return false;
    }

    if (g_headless)
        return true;

    g_screen = DefaultScreen(g_dpy);
    g_visual = DefaultVisual(g_dpy, g_screen);
    g_visualDepth = DefaultDepth(g_dpy, g_screen);

    if (!x11AnalyzeVisual())
        err = "Unsupported X11 visual";
    if (!err)
        x11BuildPalette();
    if (err) {
        logError("X11: %s\n", err);
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
        return false;
    }

    {
        XSetWindowAttributes attrs;
        attrs.background_pixel = BlackPixel(g_dpy, g_screen);
        attrs.border_pixel = BlackPixel(g_dpy, g_screen);
        attrs.colormap = g_cmap;
        attrs.event_mask = ExposureMask
                         | KeyPressMask | KeyReleaseMask
                         | ButtonPressMask | ButtonReleaseMask
                         | PointerMotionMask
                         | StructureNotifyMask
                         | FocusChangeMask;
        g_win = XCreateWindow(g_dpy, RootWindow(g_dpy, g_screen),
                              0, 0, (unsigned)reqW, (unsigned)reqH, 0,
                              g_visualDepth, InputOutput, g_visual,
                              CWBackPixel | CWBorderPixel | CWEventMask | CWColormap,
                              &attrs);
    }
    if (!g_win) {
        logError("X11: failed to create window\n");
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
        return false;
    }

    g_wmDeleteAtom = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    {
        Atom protocols[] = { g_wmDeleteAtom };
        XSetWMProtocols(g_dpy, g_win, protocols, 1);
    }

    XStoreName(g_dpy, g_win, title);
    XSetIconName(g_dpy, g_win, title);

    {
        XSizeHints hints;
        memset(&hints, 0, sizeof(hints));
        hints.flags = PBaseSize | PMinSize | PResizeInc;
        hints.base_width = (int)reqW;
        hints.base_height = (int)reqH;
        hints.min_width = 1;
        hints.min_height = 1;
        hints.width_inc = 1;
        hints.height_inc = 1;
        XSetWMNormalHints(g_dpy, g_win, &hints);
    }

    {
        XWMHints wmhints;
        memset(&wmhints, 0, sizeof(wmhints));
        wmhints.flags = InputHint | StateHint;
        wmhints.input = True;
        wmhints.initial_state = NormalState;
        XSetWMHints(g_dpy, g_win, &wmhints);
    }

    XSetWMColormapWindows(g_dpy, g_win, &g_win, 1);

    {
        XGCValues gcv;
        gcv.graphics_exposures = False;
        g_gc = XCreateGC(g_dpy, g_win, GCGraphicsExposures, &gcv);
    }

    XMapWindow(g_dpy, g_win);
    if (g_cmap)
        XInstallColormap(g_dpy, g_cmap);
    XFlush(g_dpy);
    return true;
}

void platformExit(void) {
    if (g_dpy) {
        if (g_cursor) {
            XFreeCursor(g_dpy, g_cursor);
            g_cursor = 0;
        }
        x11FreeImage();
        if (g_gc) {
            XFreeGC(g_dpy, g_gc);
            g_gc = NULL;
        }
        if (g_win)
            XDestroyWindow(g_dpy, g_win);
        g_win = 0;
        if (g_cmap && g_cmap != DefaultColormap(g_dpy, g_screen))
            XFreeColormap(g_dpy, g_cmap);
        g_cmap = 0;
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
    }
    if (g_palLut) {
        free(g_palLut);
        g_palLut = NULL;
    }
}

void platformSwapBuffers(void) {
    x11UploadFrame();
}

void *platformGetProcAddress(const char *name) {
    (void)name;
    return NULL; // no GL in this backend
}

static int32_t x11KeyToGml(KeySym keysym) {
    if (keysym >= XK_a && keysym <= XK_z)
        return keysym - XK_a + 'A';
    if (keysym >= XK_A && keysym <= XK_Z)
        return keysym;
    if (keysym >= XK_0 && keysym <= XK_9)
        return keysym;
    if (keysym >= XK_exclam && keysym <= XK_asciitilde)
        return keysym;
    switch (keysym) {
        case XK_Escape:   return VK_ESCAPE;
        case XK_Return:   return VK_ENTER;
        case XK_Tab:      return VK_TAB;
        case XK_BackSpace:return VK_BACKSPACE;
        case XK_space:    return VK_SPACE;
        case XK_Shift_L:
        case XK_Shift_R:  return VK_SHIFT;
        case XK_Control_L:
        case XK_Control_R:return VK_CONTROL;
        case XK_Alt_L:
        case XK_Alt_R:
        case XK_Meta_L:
        case XK_Meta_R:   return VK_ALT;
        case XK_Up:       return VK_UP;
        case XK_Down:     return VK_DOWN;
        case XK_Left:     return VK_LEFT;
        case XK_Right:    return VK_RIGHT;
        case XK_F1:       return VK_F1;
        case XK_F2:       return VK_F2;
        case XK_F3:       return VK_F3;
        case XK_F4:       return VK_F4;
        case XK_F5:       return VK_F5;
        case XK_F6:       return VK_F6;
        case XK_F7:       return VK_F7;
        case XK_F8:       return VK_F8;
        case XK_F9:       return VK_F9;
        case XK_F10:      return VK_F10;
        case XK_F11:      return VK_F11;
        case XK_F12:      return VK_F12;
        case XK_Insert:   return VK_INSERT;
        case XK_Delete:   return VK_DELETE;
        case XK_Home:     return VK_HOME;
        case XK_End:      return VK_END;
        case XK_Prior:    return VK_PAGEUP;
        case XK_Next:     return VK_PAGEDOWN;
        default:          return -1;
    }
}

static int32_t x11MouseButtonToGml(unsigned int button) {
    switch (button) {
        case 1: return GML_MB_LEFT;
        case 2: return GML_MB_MIDDLE;
        case 3: return GML_MB_RIGHT;
        case 6: return GML_MB_SIDE1;
        case 7: return GML_MB_SIDE2;
        default: return -1;
    }
}

static void x11HandleEvent(XEvent *ev) {
    switch (ev->type) {
        case Expose:
            if (ev->xexpose.count == 0)
                g_needsRedraw = true;
            break;
        case ConfigureNotify:
            g_viewW = ev->xconfigure.width;
            g_viewH = ev->xconfigure.height;
            break;
        case FocusIn:
            g_hasFocus = true;
            if (g_cmap)
                XInstallColormap(g_dpy, g_cmap);
            break;
        case FocusOut:
            g_hasFocus = false;
            break;
        case ClientMessage:
            if ((Atom)ev->xclient.data.l[0] == g_wmDeleteAtom)
                g_quitPending = true;
            break;
        case KeyPress:
        case KeyRelease: {
            char buf[8];
            KeySym keysym;
            int n = XLookupString(&ev->xkey, buf, (int)sizeof(buf), &keysym, NULL);
            int32_t gmlKey = x11KeyToGml(keysym);
            if (InputRecording_isPlaybackActive(globalInputRecording))
                break;
            if (ev->type == KeyPress) {
                RunnerKeyboard_onKeyDown(g_runner->keyboard, gmlKey);
                if (n > 0)
                    RunnerKeyboard_onCharacter(g_runner->keyboard, (unsigned int)(unsigned char)buf[0]);
            } else {
                RunnerKeyboard_onKeyUp(g_runner->keyboard, gmlKey);
            }
            break;
        }
        case ButtonPress:
            if (InputRecording_isPlaybackActive(globalInputRecording))
                break;
            if (ev->xbutton.button == 4)
                RunnerMouse_onWheel(g_runner->mouse, 1.0);
            else if (ev->xbutton.button == 5)
                RunnerMouse_onWheel(g_runner->mouse, -1.0);
            else {
                int32_t gmlButton = x11MouseButtonToGml(ev->xbutton.button);
                if (gmlButton >= 0)
                    RunnerMouse_onButtonDown(g_runner->mouse, gmlButton);
            }
            break;
        case ButtonRelease:
            if (InputRecording_isPlaybackActive(globalInputRecording))
                break;
            if (ev->xbutton.button != 4 && ev->xbutton.button != 5) {
                int32_t gmlButton = x11MouseButtonToGml(ev->xbutton.button);
                if (gmlButton >= 0)
                    RunnerMouse_onButtonUp(g_runner->mouse, gmlButton);
            }
            break;
        case MotionNotify:
            g_pointerX = ev->xmotion.x;
            g_pointerY = ev->xmotion.y;
            break;
        case DestroyNotify:
            g_quitPending = true;
            break;
        default:
            break;
    }
}

bool platformHandleEvents(void) {
    if (g_headless || !g_dpy)
        return g_quitPending;

    XEvent ev;
    while (XPending(g_dpy)) {
        XNextEvent(g_dpy, &ev);
        x11HandleEvent(&ev);
    }

    if (g_needsRedraw) {
        g_needsRedraw = false;
        x11UploadFrame();
    }
    return g_quitPending;
}

void platformSleepUntil(uint64_t time) {
    int64_t remaining = time - nowNanos();
    if (remaining > 2000000) {
        remaining -= 1000000;
        usleep((unsigned int)(remaining / 1000)); // ns -> us
    }
    while (nowNanos() < time) {
        YIELD();
    }
}
