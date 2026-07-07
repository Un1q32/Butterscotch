#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>
#include <limits.h>
#include "math_compat.h"

#include <UIKit/UIKit.h>
#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <OpenGLES/EAGL.h>
#include <QuartzCore/CAEAGLLayer.h>
#include <Availability.h>

/* Undefine macros that conflict with glad headers */
#undef GL_UNSIGNED_SHORT_1_5_5_5_REV

#include <glad/glad.h>

#include "common.h"
#include "input_recording.h"
#include "desktop/platformdefs.h"
#include "gettime.h"
#include "runner_mouse.h"
#ifdef ENABLE_MODERN_GL
#include "gl_renderer.h"
#endif

/*
 * TODO: look into implementing platformSetCursor and platformGetWindowFocus
 * They might actually be meaningful on newer iPadOS versions.
 */

static atomic_bool needsResize = false;
static atomic_bool quitRequested = false;

static Runner *g_runner;

#ifdef ENABLE_SW_RENDERER
static int NearestPO2(int i) {
    for (int j = 1; j < 1024 * 1024; j *= 2) {
        if (i < j)
            return j;
    }
    return i; /* fallback */
}
#endif

static EAGLContext *glcontext;
static GLuint framebuffer;
static GLuint renderbuffer;
static bool glInited = false;
static GLint fbWidth  = 0;
static GLint fbHeight = 0;
static CAEAGLLayer *layer;

#ifdef ENABLE_SW_RENDERER
static uint32_t* nextFb = NULL;
static GLuint swTexture = 0;
static uint32_t* swFbCopy = NULL;
static int swFbCopyWidth = 0, swFbCopyHeight = 0;
#endif

/* Requested render resolution, as set via platformSetWindowSize() (and
 * seeded from platformInit()'s reqW/reqH before that's ever called). Used
 * to pick a CAEAGLLayer contentsScale that renders at roughly this pixel
 * resolution instead of unconditionally rendering at the device's native
 * scale -- see applyRenderScale() below. */
static int32_t g_reqRenderWidth  = 0;
static int32_t g_reqRenderHeight = 0;

/* w/h of the game's requested resolution. Used only for layout decisions;
 * the renderer itself already letterboxes/pillarboxes to this ratio
 * regardless of the actual framebuffer size we hand it. */
static float g_aspectRatio = 1.0f;

/* The window/overlay are created on the main thread before platformInit()
 * (which runs on the game thread) knows the real aspect ratio, so the
 * initial layout uses the g_aspectRatio default of 1.0. These let us force
 * a relayout once the real value is known, instead of waiting for the
 * first rotation to happen to trigger one. */
static UIView *g_glView = nil;
static UIView *g_overlayView = nil;

/* Last UIDeviceOrientation actually applied to rootView by
 * applyDeviceOrientation:, used to no-op duplicate/redundant notifications.
 * Reset to Unknown in startGameWithFolder: so each new game session always
 * re-syncs to the device's current orientation, even if it's the same
 * orientation the previous session last settled into. */
static UIDeviceOrientation g_lastAppliedOrientation = UIDeviceOrientationUnknown;

/* Path to the selected game's data.win, filled in by the game list
 * controller before the game thread is started. */
static char g_gamePath[PATH_MAX];

/* Built from the persisted fast-forward speed setting right before each
 * game launch -- see AppDelegate startGameWithFolder:. Sized generously
 * for any reasonable floating-point value typed into the settings field. */
static char g_ffSpeedArg[64] = "--fast-forward-speed=4";

/* Cached copy of the "high resolution" setting (see BS_HIGH_RES_DEFAULTS_KEY
 * below), refreshed from NSUserDefaults once per game launch in
 * startGameWithFolder: rather than read on every applyRenderScale() call --
 * the setting can only be changed from the menu, never mid-game. */
static atomic_bool g_highResEnabled = false;

/* Cached copy of the renderer setting, refreshed from NSUserDefaults once per
 * game launch in startGameWithFolder:. */
static char g_rendererArg[32] = "software";

#define BS_GAMES_ROOT_PATH @"/var/mobile/Documents/Butterscotch"

static void bsRequestRelayout(void) {
    if (g_glView) {
        [g_glView performSelectorOnMainThread:@selector(setNeedsLayout) withObject:nil waitUntilDone:YES];
        [g_glView performSelectorOnMainThread:@selector(layoutIfNeeded) withObject:nil waitUntilDone:YES];
    }
    if (g_overlayView) {
        [g_overlayView performSelectorOnMainThread:@selector(setNeedsLayout) withObject:nil waitUntilDone:YES];
        [g_overlayView performSelectorOnMainThread:@selector(layoutIfNeeded) withObject:nil waitUntilDone:YES];
        [g_overlayView performSelectorOnMainThread:@selector(setNeedsDisplay) withObject:nil waitUntilDone:YES];
    }
}

/* ---------------------------------------------------------------------
 * Touch control layout
 *
 * Portrait: game view anchored to the top of the screen, dpad + z/x/c
 * strip along the bottom (gameboy-ish). Landscape: game view centered,
 * dpad on the left, z/x/c on the right. If the requested aspect ratio
 * needs more space than what's left after reserving the control strip,
 * the game view is allowed to grow into the control area rather than
 * get squashed -- the controls are translucent, so this is fine.
 * ------------------------------------------------------------------- */

#define BS_CONTROL_STRIP_PORTRAIT_H   160.0f
#define BS_CONTROL_STRIP_LANDSCAPE_W  120.0f
#define BS_QUIT_BUTTON_SIZE           22.0f
#define BS_QUIT_BUTTON_MARGIN         8.0f
#define BS_FF_BUTTON_SIZE             32.0f   /* a bit bigger than the quit button */
/* padded off the taller of the two top-corner buttons so neither gets clipped */
#define BS_GAME_TOP_PADDING           (BS_QUIT_BUTTON_MARGIN + BS_FF_BUTTON_SIZE + 4.0f)

typedef struct {
    CGRect gameFrame;
    CGRect dpadFrame;
    CGRect buttonsFrame;
    CGRect quitFrame;
    CGRect ffFrame;
    bool   portrait;
} BSLayout;

static BSLayout computeLayout(CGSize screen) {
    BSLayout layout;
    layout.portrait = screen.height >= screen.width;

    if (layout.portrait) {
        CGFloat neededH = screen.width / g_aspectRatio;
        CGFloat gameH = fminf(neededH, screen.height - BS_GAME_TOP_PADDING);
        layout.gameFrame = CGRectMake(0, BS_GAME_TOP_PADDING, screen.width, gameH);

        CGFloat stripY = screen.height - BS_CONTROL_STRIP_PORTRAIT_H;
        layout.dpadFrame    = CGRectMake(16, stripY + 10, 130, 130);
        layout.buttonsFrame = CGRectMake(screen.width - 16 - 150, stripY + 30, 150, 50);
    } else {
        CGFloat neededW = screen.height * g_aspectRatio;
        CGFloat gameW = fminf(neededW, screen.width);
        CGFloat gameX = (screen.width - gameW) / 2.0f;
        layout.gameFrame = CGRectMake(gameX, 0, gameW, screen.height);

        layout.dpadFrame    = CGRectMake(10, (screen.height - 130) / 2.0f + 35, 110, 130);
        layout.buttonsFrame = CGRectMake(screen.width - 10 - 170, (screen.height - 130) / 2.0f + 60, 170, 60);
    }

    layout.quitFrame = CGRectMake(screen.width - BS_QUIT_BUTTON_MARGIN - BS_QUIT_BUTTON_SIZE,
                                   BS_QUIT_BUTTON_MARGIN, BS_QUIT_BUTTON_SIZE, BS_QUIT_BUTTON_SIZE);

    /* Fast-forward: top-left, mirrors the quit button on the top-right. */
    layout.ffFrame = CGRectMake(BS_QUIT_BUTTON_MARGIN, BS_QUIT_BUTTON_MARGIN,
                                 BS_FF_BUTTON_SIZE, BS_FF_BUTTON_SIZE);
    return layout;
}

static void dpadArmRects(CGRect dpad, CGRect *up, CGRect *down, CGRect *left, CGRect *right) {
    CGFloat cx = dpad.origin.x + dpad.size.width  / 2.0f;
    CGFloat cy = dpad.origin.y + dpad.size.height / 2.0f;
    CGFloat armW = dpad.size.width  / 3.0f;
    CGFloat armH = dpad.size.height / 3.0f;
    *up    = CGRectMake(cx - armW / 2.0f, dpad.origin.y, armW, armH);
    *down  = CGRectMake(cx - armW / 2.0f, dpad.origin.y + dpad.size.height - armH, armW, armH);
    *left  = CGRectMake(dpad.origin.x, cy - armH / 2.0f, armW, armH);
    *right = CGRectMake(dpad.origin.x + dpad.size.width - armW, cy - armH / 2.0f, armW, armH);
}

/* 8-way split around the dpad's center so a single touch can express
 * diagonals (e.g. up+left) without needing two fingers. */
static void dpadDirectionsForPoint(CGRect dpad, CGPoint p, bool *up, bool *down, bool *left, bool *right) {
    CGFloat cx = dpad.origin.x + dpad.size.width  / 2.0f;
    CGFloat cy = dpad.origin.y + dpad.size.height / 2.0f;
    CGFloat dx = p.x - cx;
    CGFloat dy = p.y - cy;
    CGFloat deadzone = fminf(dpad.size.width, dpad.size.height) * 0.15f;

    *up = *down = *left = *right = false;
    if (fabs(dx) < deadzone && fabs(dy) < deadzone) return;

    CGFloat deg = atan2f(dy, dx) * 180.0f / (CGFloat)M_PI; /* 0 = right, 90 = down (screen coords) */
    if (deg < 0) deg += 360.0f;

    if      (deg >= 337.5f || deg < 22.5f) { *right = true; }
    else if (deg < 67.5f)                  { *right = true; *down = true; }
    else if (deg < 112.5f)                 { *down = true; }
    else if (deg < 157.5f)                 { *down = true; *left = true; }
    else if (deg < 202.5f)                 { *left = true; }
    else if (deg < 247.5f)                 { *left = true; *up = true; }
    else if (deg < 292.5f)                 { *up = true; }
    else                                    { *up = true; *right = true; }
}

static void actionButtonRects(CGRect bf, CGRect out[3]) {
    CGFloat btnSize = fminf(bf.size.height, bf.size.width / 3.0f) - 8.0f;
    for (int i = 0; i < 3; i++) {
        out[i] = CGRectMake(bf.origin.x + i * (bf.size.width / 3.0f) + 4.0f,
                             bf.origin.y + (bf.size.height - btnSize) / 2.0f,
                             btnSize, btnSize);
    }
}

void platformSetWindowTitle(const char* title) {
    (void)title;
}

bool platformGetWindowSize(int32_t* outW, int32_t* outH) {
    if (!outW || !outH) return false;
    if (fbWidth <= 0 || fbHeight <= 0) return false;
    *outW = fbWidth;
    *outH = fbHeight;
    return true;
}

bool platformGetScaledWindowSize(int32_t* outW, int32_t* outH) {
    if (!outW || !outH) return false;
    CGRect bounds = [[UIScreen mainScreen] bounds];
    if (bounds.size.width <= 0 || bounds.size.height <= 0) return false;
    *outW = bounds.size.width;
    *outH = bounds.size.height;
    return true;
}

/*
 * Stores the game's requested render resolution; applyRenderScale() (run
 * from resizeFramebuffer(), triggered here via needsResize) derives the
 * actual CAEAGLLayer contentsScale from this plus the game view's current
 * point size -- see applyRenderScale() for the up-vs-down-clamp logic.
 * We don't recompute the scale directly here because the frame's point
 * size can also change independently of this call (rotation), so both
 * paths just flag needsResize and let resizeFramebuffer() re-derive it
 * from whatever's current on the next frame.
 *
 * The requested width/height also determine g_aspectRatio, which drives
 * computeLayout()'s letterboxing -- previously only set once in
 * platformInit(), so a game changing its resolution mid-run (e.g. a
 * GameMaker room with a different size) kept the *old* aspect ratio's
 * gameFrame even though the framebuffer underneath it got resized to
 * match the new one. Recompute it here too, and force a relayout the
 * same way platformInit() does for the initial value.
 */
void platformSetWindowSize(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return;

    g_reqRenderWidth  = width;
    g_reqRenderHeight = height;
    g_aspectRatio = (float)width / (float)height;
    bsRequestRelayout();
    atomic_store(&needsResize, true);
}

/* TODO: touchscreen mouse support */
void platformGetMousePos(double *xPos, double *yPos) {
    *xPos = 0.0;
    *yPos = 0.0;
}

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless) {
    (void)title; (void)headless;

    g_aspectRatio = (reqH > 0) ? ((float)reqW / (float)reqH) : 1.0f;
    g_reqRenderWidth  = reqW;
    g_reqRenderHeight = reqH;
    bsRequestRelayout();

#ifdef ENABLE_MODERN_GL
    if (gfx == MODERN_GL)
        glcontext = [[EAGLContext alloc] initWithAPI:3];
        if (!glcontext)
            glcontext = [[EAGLContext alloc] initWithAPI:2];
#endif
#ifdef ENABLE_SW_RENDERER
    if (gfx == SOFTWARE)
        glcontext = [[EAGLContext alloc] initWithAPI:1];
#endif

    if (!glcontext) {
        fprintf(stderr, "Failed to create an OpenGLES context\n");
        return false;
    }

    if (![EAGLContext setCurrentContext:glcontext]) {
        [glcontext release];
        glcontext = nil;
        fprintf(stderr, "Failed to set the OpenGLES context\n");
        return false;
    }

    return true;
}

void platformExit(void) {
    if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
    if (renderbuffer) glDeleteRenderbuffers(1, &renderbuffer);
#ifdef ENABLE_SW_RENDERER
    if (swTexture) glDeleteTextures(1, &swTexture);
    if (swFbCopy) free(swFbCopy);
#endif
    [glcontext release];
    glcontext = nil;
    framebuffer = 0;
    renderbuffer = 0;
    glInited = false;
}

/* [screen respondsToSelector:@selector(scale)] guard is for pre-iOS-4,
 * where UIScreen has no notion of a Retina content scale and 1x is
 * correct. */
static CGFloat nativeScreenScale(void) {
    UIScreen *screen = [UIScreen mainScreen];
    CGFloat scale = 1.0f;
    if ([screen respondsToSelector:@selector(scale)]) {
        CGFloat (*getScale)(id, SEL) = (CGFloat (*)(id, SEL))objc_msgSend;
        scale = getScale(screen, @selector(scale));
    }
    return scale;
}

/*
 * Picks the CAEAGLLayer's contentsScale -- and therefore the framebuffer's
 * pixel resolution, since the drawable size handed to
 * renderbufferStorage:fromDrawable: is just (layer bounds in points) *
 * contentsScale -- based on the game's requested render resolution vs.
 * the device's native scale:
 *
 *   - requested < native: render at the requested (lower) resolution.
 *     Saves fill-rate, which matters a lot on early Retina devices that
 *     got a 4x pixel-count jump without a matching GPU jump.
 *   - requested > native: clamp to native. Upsampling past native just
 *     produces blur with no visible benefit, and burns performance that
 *     already-underpowered low-res devices can't spare.
 *
 * Recomputed from scratch every call (cheap) rather than cached, since
 * the inputs -- the requested resolution, the "high resolution" setting,
 * or the frame's current point size -- can each change independently
 * (platformSetWindowSize, the settings menu, or rotation respectively).
 */
static void applyRenderScale(void) {
    if (!layer) return;

    CGSize sz = layer.bounds.size;
    if (sz.width <= 0.0f || sz.height <= 0.0f) return;

    CGFloat nativeScale = nativeScreenScale();
    CGFloat targetScale = nativeScale;

    int32_t effReqWidth  = g_reqRenderWidth;
    int32_t effReqHeight = g_reqRenderHeight;

    /* "High resolution" setting (see BS_HIGH_RES_DEFAULTS_KEY): instead of
     * clamping down to the game's requested render resolution (e.g.
     * 640x480), target the full available logical screen size at native
     * pixel density instead -- most noticeable in landscape, where the
     * requested resolution would otherwise get scaled down well below
     * what the device can actually display. g_glView's superview
     * (rootView) already reflects the current device orientation (see
     * AppDelegate applyDeviceOrientation:), so its bounds are exactly the
     * logical on-screen size for however the device is currently held. */
    if (atomic_load(&g_highResEnabled) && g_glView && g_glView.superview) {
        CGSize logicalSize = g_glView.superview.bounds.size;
        effReqWidth  = (int32_t)(logicalSize.width  * nativeScale);
        effReqHeight = (int32_t)(logicalSize.height * nativeScale);
    }

    if (effReqWidth > 0 && effReqHeight > 0) {
        CGFloat sW = (CGFloat)effReqWidth  / sz.width;
        CGFloat sH = (CGFloat)effReqHeight / sz.height;
        /* Use whichever axis wants the smaller scale, so we never exceed
         * the requested pixel count on either dimension -- then clamp to
         * native so we never exceed the device's real resolution either. */
        targetScale = fminf(nativeScale, fminf(sW, sH));
    }

    if (targetScale <= 0.0f) targetScale = nativeScale; /* safety net */

    /* CAEAGLLayer derives its backing pixel size as bounds.size (points) *
     * contentsScale, and truncates rather than rounds that product down
     * to an integer. The division/multiplication chain above -- aspect
     * ratio, then layout, then the division just above -- can leave
     * targetScale a hair below the value that would exactly reproduce
     * the intended pixel count (e.g. 1.499998 instead of 1.5), which
     * then silently truncates a whole pixel off the framebuffer on each
     * axis (observed as 639x479 instead of 640x480 on an iPhone 5S in
     * landscape). Nudge just past that truncation boundary -- this is
     * far too small (a fraction of a point in scale) to visibly affect
     * the actual rendered resolution, but reliably avoids the off-by-one. */
    targetScale += 0.0005f;
    if (targetScale > nativeScale) targetScale = nativeScale;

    if ([layer respondsToSelector:@selector(setContentsScale:)]) {
        void (*setScale)(id, SEL, CGFloat) = (void (*)(id, SEL, CGFloat))objc_msgSend;
        setScale(layer, @selector(setContentsScale:), targetScale);
    }
}

static void resizeFramebuffer(void) {
    applyRenderScale();

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    [glcontext renderbufferStorage:GL_RENDERBUFFER fromDrawable:layer];
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &fbWidth);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &fbHeight);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);

    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);

#ifdef ENABLE_MODERN_GL
    if (g_runner && g_runner->renderer)
        ((GLRenderer *)g_runner->renderer)->hostFramebuffer = framebuffer;
#endif

    glViewport(0, 0, fbWidth, fbHeight);
}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;

    /* this can't be in platformInit because glad hasn't initialized yet */
    if (!glInited) {
#ifdef ENABLE_SW_RENDERER
        if (gfx == SOFTWARE) {
            glGenFramebuffers = glGenFramebuffersOES;
            glGenRenderbuffers = glGenRenderbuffersOES;
            glBindFramebuffer = glBindFramebufferOES;
            glBindRenderbuffer = glBindRenderbufferOES;
            glGetRenderbufferParameteriv = glGetRenderbufferParameterivOES;
            glFramebufferRenderbuffer = glFramebufferRenderbufferOES;
            glDeleteFramebuffers = glDeleteFramebuffersOES;
            glDeleteRenderbuffers = glDeleteRenderbuffersOES;
            glGenTextures(1, &swTexture);

            /* GLES defaults GL_TEXTURE_MIN_FILTER to
             * GL_NEAREST_MIPMAP_LINEAR, which requires a complete mipmap
             * chain. platformSwapBuffers() only ever uploads a single base
             * level via glTexImage2D, so without overriding this the
             * texture is "incomplete" per the GLES spec -- and on iOS's
             * PowerVR-derived driver stack, sampling an incomplete
             * texture returns solid white rather than black. That's the
             * pure-white-screen bug: set filtering (no mipmaps needed)
             * and clamp-to-edge wrap (harmless, and avoids any GL_REPEAT
             * seam artifacts from the unused padding in the PO2 buffer)
             * once, right after the texture object is created. */
            glBindTexture(GL_TEXTURE_2D, swTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
#endif
        glGenFramebuffers(1, &framebuffer);
        glGenRenderbuffers(1, &renderbuffer);
        glInited = true;
    }

    resizeFramebuffer();
    atomic_store(&needsResize, false);
}

#ifdef ENABLE_SW_RENDERER

void Runner_setNextFrame(uint32_t* framebuffer, int width, int height) {
    nextFb = framebuffer;
    fbWidth = width;
    fbHeight = height;

    /* Allocate power-of-2 sized buffer for texture upload */
    int glWidth = NearestPO2(fbWidth), glHeight = NearestPO2(fbHeight);
    if (swFbCopyWidth != glWidth || swFbCopyHeight != glHeight) {
        if (swFbCopy)
            free(swFbCopy);
        size_t rfbSize = sizeof(uint32_t) * glWidth * glHeight;
        swFbCopy = safeMalloc(rfbSize);
        memset(swFbCopy, 0, rfbSize);
        swFbCopyWidth = glWidth;
        swFbCopyHeight = glHeight;
    }
}

#endif

void platformSwapBuffers(void) {
#ifdef ENABLE_SW_RENDERER
    if (gfx == SOFTWARE && nextFb && swFbCopy) {
        glClear(GL_COLOR_BUFFER_BIT);

        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, swTexture);

        float xs = (float)swFbCopyWidth / (float)fbWidth;
        float ys = (float)swFbCopyHeight / (float)fbHeight;

        /* Copy to power-of-2 buffer */
        for (int y = 0; y < fbHeight; ++y) {
            uint32_t* dstline = swFbCopy + y * swFbCopyWidth;
            const uint32_t* srcline = nextFb + y * fbWidth;
            memcpy(dstline, srcline, fbWidth);
        }
        nextFb = NULL;

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, swFbCopyWidth, swFbCopyHeight, 0, GL_BGRA, GL_UNSIGNED_BYTE, swFbCopy);

        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        GLfloat vertices[] = { 
            // tri 1
            -1, -1,    0, 1.0f / ys,
            -1, 1,     0, 0,
            1, 1,      1.0f / xs, 0,
            // tri 2
            -1, -1,    0, 1.0f / ys,
            1, 1,      1.0f / xs, 0,
            1, -1,     1.0f / xs, 1.0f / ys,
        };

        // count, type, stride, pointer
        glVertexPointer(2, GL_FLOAT, 4 * sizeof(float), vertices);
        glTexCoordPointer(2, GL_FLOAT, 4 * sizeof(float), vertices + 2);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glDisable(GL_TEXTURE_2D);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
#endif
    [glcontext presentRenderbuffer:GL_RENDERBUFFER];
}

void *platformGetProcAddress(const char *name) {
    return dlsym(RTLD_NEXT, name);
}

bool platformHandleEvents(void) {
    if (atomic_exchange(&needsResize, false))
        resizeFramebuffer();
    if (atomic_load(&quitRequested))
        return true;
    return false;
}

void platformSleepUntil(uint64_t time) {
    int64_t remaining = time - nowNanos();
    if (remaining > 2000000) {
        remaining -= 1000000;
        struct timespec ts;
        ts.tv_sec  = 0;
        ts.tv_nsec = remaining;
        nanosleep(&ts, NULL);
    }
    while (nowNanos() < time) {
        // Spin-wait for the remaining sub-millisecond
        YIELD();
    }
}

@interface GLView : UIView
@end

@implementation GLView

+ (Class)layerClass {
    return [CAEAGLLayer class];
}

- (id)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        layer = (CAEAGLLayer *)self.layer;
        layer.opaque = YES;
        /* Frame is fully recomputed in -layoutSubviews on every layout pass
         * (rotation, first layout, etc), so we don't rely on autoresizing. */
        self.autoresizingMask = UIViewAutoresizingNone;

        /* Seed with the native scale so the layer is sane for the brief
         * window between view creation and the first resizeFramebuffer()
         * call; applyRenderScale() will correct this to the requested
         * render resolution's scale once sizing kicks in. */
        CGFloat scale = nativeScreenScale();
        if ([layer respondsToSelector:@selector(setContentsScale:)]) {
            void (*setScale)(id, SEL, CGFloat) = (void (*)(id, SEL, CGFloat))objc_msgSend;
            setScale(layer, @selector(setContentsScale:), scale);
        }
    }
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    if (self.superview) {
        BSLayout bsLayout = computeLayout(self.superview.bounds.size);
        self.frame = bsLayout.gameFrame;
    }
    atomic_store(&needsResize, true);
}

- (void)dealloc {
    [super dealloc];
}

@end

/* ---------------------------------------------------------------------
 * On-screen touch controls: dpad + z/x/c + quit. Sits as a sibling of
 * GLView, sized to the full screen (so it can draw controls in the area
 * outside the game view, and translucently over it when the game view
 * grows into that area).
 * ------------------------------------------------------------------- */

@interface BSTouchOverlay : UIView {
    UITouch *dpadTouch;
    int32_t  dpadKeysDown[4]; /* up, down, left, right */
    UITouch *buttonTouches[3]; /* z, x, c */
    UITouch *quitTouch;
    UITouch *ffTouch;
}
@end

@implementation BSTouchOverlay

- (id)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.backgroundColor = [UIColor clearColor];
        self.opaque = NO;
        self.multipleTouchEnabled = YES;
        self.userInteractionEnabled = YES;
        self.autoresizingMask = UIViewAutoresizingNone;
        dpadTouch = nil;
        quitTouch = nil;
        ffTouch = nil;
        for (int i = 0; i < 4; i++) dpadKeysDown[i] = 0;
        for (int i = 0; i < 3; i++) buttonTouches[i] = nil;
    }
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    if (self.superview) {
        self.frame = self.superview.bounds;
    }
    [self setNeedsDisplay];
}

static void drawTranslucentCircle(CGContextRef ctx, CGRect frame, BOOL highlighted) {
    CGFloat alpha = highlighted ? 0.55f : 0.30f;
    CGContextSetRGBFillColor(ctx, 1.0f, 1.0f, 1.0f, alpha);
    CGContextFillEllipseInRect(ctx, frame);
    CGContextSetRGBStrokeColor(ctx, 1.0f, 1.0f, 1.0f, 0.6f);
    CGContextStrokeEllipseInRect(ctx, frame);
}

static void drawCenteredLabel(NSString *text, CGRect rect, UIFont *font) {
#if __IPHONE_OS_VERSION_MIN_REQUIRED >= 70000
    CGSize size = [text sizeWithAttributes:@{NSFontAttributeName: font}];
#else
    CGSize size = [text sizeWithFont:font];
#endif
    CGPoint pt = CGPointMake(rect.origin.x + (rect.size.width  - size.width)  / 2.0f,
                              rect.origin.y + (rect.size.height - size.height) / 2.0f);
    [[UIColor whiteColor] set];
#if __IPHONE_OS_VERSION_MIN_REQUIRED >= 70000
    [text drawAtPoint:pt withAttributes:@{NSFontAttributeName: font}];
#else
    [text drawAtPoint:pt withFont:font];
#endif
}

- (void)drawRect:(CGRect)rect {
    (void)rect;
    CGContextRef ctx = UIGraphicsGetCurrentContext();
    BSLayout bsLayout = computeLayout(self.bounds.size);

    CGRect up, down, left, right;
    dpadArmRects(bsLayout.dpadFrame, &up, &down, &left, &right);

    CGContextSetRGBFillColor(ctx, 1.0f, 1.0f, 1.0f, dpadKeysDown[0] ? 0.55f : 0.28f);
    CGContextFillRect(ctx, up);
    CGContextSetRGBFillColor(ctx, 1.0f, 1.0f, 1.0f, dpadKeysDown[1] ? 0.55f : 0.28f);
    CGContextFillRect(ctx, down);
    CGContextSetRGBFillColor(ctx, 1.0f, 1.0f, 1.0f, dpadKeysDown[2] ? 0.55f : 0.28f);
    CGContextFillRect(ctx, left);
    CGContextSetRGBFillColor(ctx, 1.0f, 1.0f, 1.0f, dpadKeysDown[3] ? 0.55f : 0.28f);
    CGContextFillRect(ctx, right);

    CGRect actionRects[3];
    actionButtonRects(bsLayout.buttonsFrame, actionRects);
    NSString *actionLabels[3] = { @"Z", @"X", @"C" };
    UIFont *actionFont = [UIFont boldSystemFontOfSize:18.0f];
    for (int i = 0; i < 3; i++) {
        drawTranslucentCircle(ctx, actionRects[i], buttonTouches[i] != nil);
        drawCenteredLabel(actionLabels[i], actionRects[i], actionFont);
    }

    drawTranslucentCircle(ctx, bsLayout.quitFrame, quitTouch != nil);
    drawCenteredLabel(@"X", bsLayout.quitFrame, [UIFont boldSystemFontOfSize:14.0f]);

    drawTranslucentCircle(ctx, bsLayout.ffFrame, ffTouch != nil);
    drawCenteredLabel(@">>", bsLayout.ffFrame, [UIFont boldSystemFontOfSize:16.0f]);
}

- (void)updateDpadUp:(bool)up down:(bool)down left:(bool)left right:(bool)right {
    bool newState[4] = { up, down, left, right };
    int32_t keys[4] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
    for (int i = 0; i < 4; i++) {
        if (newState[i] && !dpadKeysDown[i]) {
            if (g_runner) RunnerKeyboard_onKeyDown(g_runner->keyboard, keys[i]);
        } else if (!newState[i] && dpadKeysDown[i]) {
            if (g_runner) RunnerKeyboard_onKeyUp(g_runner->keyboard, keys[i]);
        }
        dpadKeysDown[i] = newState[i] ? 1 : 0;
    }
}

- (void)touchesBegan:(NSSet *)touches withEvent:(UIEvent *)event {
    (void)event;
    BSLayout bsLayout = computeLayout(self.bounds.size);
    CGRect actionRects[3];
    actionButtonRects(bsLayout.buttonsFrame, actionRects);

    for (UITouch *touch in touches) {
        CGPoint p = [touch locationInView:self];

        if (!quitTouch && CGRectContainsPoint(CGRectInset(bsLayout.quitFrame, -6, -6), p)) {
            quitTouch = [touch retain];
            [self setNeedsDisplay];
            continue;
        }

        if (!ffTouch && CGRectContainsPoint(CGRectInset(bsLayout.ffFrame, -6, -6), p)) {
            ffTouch = [touch retain];
            if (g_runner) RunnerKeyboard_onKeyDown(g_runner->keyboard, VK_TAB);
            [self setNeedsDisplay];
            continue;
        }

        if (!dpadTouch && CGRectContainsPoint(CGRectInset(bsLayout.dpadFrame, -20, -20), p)) {
            dpadTouch = [touch retain];
            bool up, down, left, right;
            dpadDirectionsForPoint(bsLayout.dpadFrame, p, &up, &down, &left, &right);
            [self updateDpadUp:up down:down left:left right:right];
            [self setNeedsDisplay];
            continue;
        }

        for (int i = 0; i < 3; i++) {
            if (!buttonTouches[i] && CGRectContainsPoint(CGRectInset(actionRects[i], -6, -6), p)) {
                buttonTouches[i] = [touch retain];
                int32_t vk = (i == 0) ? 'Z' : (i == 1) ? 'X' : 'C';
                if (g_runner) RunnerKeyboard_onKeyDown(g_runner->keyboard, vk);
                [self setNeedsDisplay];
                break;
            }
        }
    }
}

- (void)touchesMoved:(NSSet *)touches withEvent:(UIEvent *)event {
    (void)event;
    if (!dpadTouch) return;
    BSLayout bsLayout = computeLayout(self.bounds.size);
    for (UITouch *touch in touches) {
        if (touch == dpadTouch) {
            CGPoint p = [touch locationInView:self];
            bool up, down, left, right;
            dpadDirectionsForPoint(bsLayout.dpadFrame, p, &up, &down, &left, &right);
            [self updateDpadUp:up down:down left:left right:right];
            [self setNeedsDisplay];
            break;
        }
    }
}

- (void)handleTouchEnd:(NSSet *)touches {
    for (UITouch *touch in touches) {
        if (touch == dpadTouch) {
            [self updateDpadUp:false down:false left:false right:false];
            [dpadTouch release];
            dpadTouch = nil;
            [self setNeedsDisplay];
            continue;
        }
        if (touch == quitTouch) {
            [quitTouch release];
            quitTouch = nil;
            atomic_store(&quitRequested, true);
            [self setNeedsDisplay];
            continue;
        }
        if (touch == ffTouch) {
            if (g_runner) RunnerKeyboard_onKeyUp(g_runner->keyboard, VK_TAB);
            [ffTouch release];
            ffTouch = nil;
            [self setNeedsDisplay];
            continue;
        }
        for (int i = 0; i < 3; i++) {
            if (touch == buttonTouches[i]) {
                int32_t vk = (i == 0) ? 'Z' : (i == 1) ? 'X' : 'C';
                if (g_runner) RunnerKeyboard_onKeyUp(g_runner->keyboard, vk);
                [buttonTouches[i] release];
                buttonTouches[i] = nil;
                [self setNeedsDisplay];
                break;
            }
        }
    }
}

- (void)touchesEnded:(NSSet *)touches withEvent:(UIEvent *)event {
    (void)event;
    [self handleTouchEnd:touches];
}

- (void)touchesCancelled:(NSSet *)touches withEvent:(UIEvent *)event {
    (void)event;
    [self handleTouchEnd:touches];
}

- (void)dealloc {
    if (dpadTouch) [dpadTouch release];
    if (quitTouch) [quitTouch release];
    if (ffTouch) [ffTouch release];
    for (int i = 0; i < 3; i++) if (buttonTouches[i]) [buttonTouches[i] release];
    [super dealloc];
}

@end

@interface BSViewController : UIViewController
@end

@implementation BSViewController

/*
 * Apple keeps changing how you're supposed to tell UIKit to handle shit,
 * so we just pretend we're portrait only and then rotate manually.
 */
- (BOOL)shouldAutorotateToInterfaceOrientation:(UIInterfaceOrientation)interfaceOrientation {
    return interfaceOrientation == UIInterfaceOrientationPortrait;
}

/* iOS 6+ query this pair instead of the method above. NSUInteger (rather
 * than UIInterfaceOrientationMask) as the return type keeps this
 * compiling against SDKs that predate that type -- it's still called
 * correctly at runtime on OS versions that support it. */
- (BOOL)shouldAutorotate {
    return NO;
}

- (NSUInteger)supportedInterfaceOrientations {
    return 1; /* UIInterfaceOrientationMaskPortrait's raw value (1 << UIInterfaceOrientationPortrait) */
}

/* Pre-iOS-7: without this, the view is offset 20pt down to make room for
 * the status bar, even though we hide the status bar at launch. */
- (BOOL)wantsFullScreenLayout {
    return YES;
}

@end

/* ---------------------------------------------------------------------
 * Settings: the fast-forward speed multiplier and the "high resolution"
 * toggle, both persisted via NSUserDefaults. The fast-forward speed is
 * applied as a --fast-forward-speed=<n> argv entry, and the high-res flag
 * is cached into g_highResEnabled, the next time a game is launched (see
 * AppDelegate startGameWithFolder:).
 * ------------------------------------------------------------------- */

#define BS_FF_SPEED_DEFAULTS_KEY @"BSFastForwardSpeed"
#define BS_DEFAULT_FF_SPEED      4.0

#define BS_HIGH_RES_DEFAULTS_KEY @"BSHighResolution"

#define BS_RENDERER_DEFAULTS_KEY @"BSRenderer"
#define BS_RENDERER_SOFTWARE    0
#define BS_RENDERER_MODERN_GL   1

/* Falls back to the default any time the stored value is missing or
 * non-positive (e.g. first launch, or a corrupted/edited defaults plist). */
static double bsLoadFastForwardSpeed(void) {
    double v = [[NSUserDefaults standardUserDefaults] doubleForKey:BS_FF_SPEED_DEFAULTS_KEY];
    return (v > 0.0) ? v : BS_DEFAULT_FF_SPEED;
}

/* boolForKey: returns NO when the key has never been set, so this is off
 * by default with no separate first-launch handling needed. */
static bool bsLoadHighResEnabled(void) {
    return [[NSUserDefaults standardUserDefaults] boolForKey:BS_HIGH_RES_DEFAULTS_KEY];
}

/* Returns the saved renderer preference, defaulting to software if never set. */
static int bsLoadRendererPreference(void) {
    NSInteger v = [[NSUserDefaults standardUserDefaults] integerForKey:BS_RENDERER_DEFAULTS_KEY];
    if (v != BS_RENDERER_SOFTWARE && v != BS_RENDERER_MODERN_GL)
        return BS_RENDERER_SOFTWARE;
    return (int)v;
}

/* Checks if the device supports OpenGL ES 2.0. On iOS 2.0+, this can be
 * checked by trying to create an EAGLContext with API=2. */
static bool bsSupportsGLES2(void) {
    EAGLContext *testContext = [[EAGLContext alloc] initWithAPI:2];
    if (testContext) {
        [testContext release];
        return true;
    }
    return false;
}

/* Renders a simple gear glyph into a UIImage via Core Graphics, rather than
 * relying on a Unicode gear character rendering correctly on very old
 * font/rendering stacks. Uses UIGraphicsBeginImageContext (iOS 2+) rather
 * than the *WithOptions variant (iOS 4+) to stay compatible with the low
 * end of the SDK matrix. */
static UIImage *createGearIconImage(CGFloat size, UIColor *color) {
    UIGraphicsBeginImageContext(CGSizeMake(size, size));
    CGContextRef ctx = UIGraphicsGetCurrentContext();

    CGPoint center = CGPointMake(size / 2.0f, size / 2.0f);
    CGFloat outerR = size * 0.46f;
    CGFloat toothR = size * 0.12f;
    CGFloat innerR = size * 0.28f;
    const int teeth = 8;

    CGContextSetFillColorWithColor(ctx, color.CGColor);

    CGMutablePathRef path = CGPathCreateMutable();
    for (int i = 0; i < teeth * 2; i++) {
        CGFloat angle = (CGFloat)i * (CGFloat)M_PI / (CGFloat)teeth;
        CGFloat r = (i % 2 == 0) ? (outerR + toothR) : outerR;
        CGFloat x = center.x + r * cosf(angle);
        CGFloat y = center.y + r * sinf(angle);
        if (i == 0) CGPathMoveToPoint(path, NULL, x, y);
        else        CGPathAddLineToPoint(path, NULL, x, y);
    }
    CGPathCloseSubpath(path);
    CGContextAddPath(ctx, path);
    CGContextFillPath(ctx);
    CGPathRelease(path);

    /* Punch the center hole so it reads as a gear rather than a spiky disc. */
    CGContextSetBlendMode(ctx, kCGBlendModeClear);
    CGContextFillEllipseInRect(ctx, CGRectMake(center.x - innerR, center.y - innerR,
                                                innerR * 2.0f, innerR * 2.0f));

    UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    return img;
}

@interface BSSettingsViewController : UIViewController <UITextFieldDelegate> {
    UITextField *speedField;
    UISwitch *highResSwitch;
    UISegmentedControl *rendererControl;
}
@end

/* UIKeyboardTypeDecimalPad was added in iOS 4.1. Rather than gate on the
 * SDK this file happens to be compiled against (which says nothing about
 * whether the *running* device actually supports it, and may not even
 * declare the symbol), reference its known raw enum value directly and
 * decide whether to use it purely from the device's reported OS version
 * at runtime. Falls back to NumbersAndPunctuation (available since iOS
 * 2.0) on anything older -- still exposes a decimal point. */
static UIKeyboardType bsNumericKeyboardType(void) {
    NSString *sysVersion = [[UIDevice currentDevice] systemVersion];
    if ([sysVersion compare:@"4.1" options:NSNumericSearch] != NSOrderedAscending)
        return (UIKeyboardType)8; /* UIKeyboardTypeDecimalPad's raw value */
    return UIKeyboardTypeNumbersAndPunctuation;
}

@implementation BSSettingsViewController

- (void)loadView {
    CGRect bounds = [[UIScreen mainScreen] bounds];
    UIView *root = [[[UIView alloc] initWithFrame:bounds] autorelease];
    root.backgroundColor = [UIColor whiteColor];
    root.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

    UILabel *fieldLabel = [[[UILabel alloc] initWithFrame:CGRectMake(20, 20, bounds.size.width - 40, 24)] autorelease];
    fieldLabel.text = @"Fast forward speed (multiplier):";
    fieldLabel.font = [UIFont systemFontOfSize:15.0f];
    fieldLabel.backgroundColor = [UIColor clearColor];
    [root addSubview:fieldLabel];

    speedField = [[UITextField alloc] initWithFrame:CGRectMake(20, 48, bounds.size.width - 40, 36)];
    speedField.borderStyle = UITextBorderStyleRoundedRect;
    speedField.keyboardType = bsNumericKeyboardType();
    speedField.delegate = self;
    speedField.text = [NSString stringWithFormat:@"%g", bsLoadFastForwardSpeed()];
    [root addSubview:speedField];

    /* sizeToFit rather than a hardcoded frame, since UISwitch's on-screen
     * size has drifted slightly across OS versions. */
    highResSwitch = [[UISwitch alloc] initWithFrame:CGRectZero];
    [highResSwitch sizeToFit];
    CGRect swFrame = highResSwitch.frame;
    swFrame.origin = CGPointMake(bounds.size.width - 20 - swFrame.size.width, 100);
    highResSwitch.frame = swFrame;
    highResSwitch.on = bsLoadHighResEnabled();
    [root addSubview:highResSwitch];

    UILabel *highResLabel = [[[UILabel alloc] initWithFrame:CGRectMake(20, 100, swFrame.origin.x - 30, swFrame.size.height)] autorelease];
    highResLabel.text = @"High resolution (landscape):";
    highResLabel.font = [UIFont systemFontOfSize:15.0f];
    highResLabel.backgroundColor = [UIColor clearColor];
    [root addSubview:highResLabel];

    /* Renderer selection: segmented control with Software and Modern GL options.
     * Only show this if both renderers are available, otherwise force the appropriate one. */
#if !defined(ENABLE_MODERN_GL) || !defined(ENABLE_SW_RENDERER)
    rendererControl = nil;
#else
    bool supportsGLES2 = bsSupportsGLES2();
    rendererControl = [[UISegmentedControl alloc] initWithItems:[NSArray arrayWithObjects:@"Software", @"Modern GL", nil]];
    [rendererControl sizeToFit];
    CGRect rendererFrame = rendererControl.frame;
    rendererFrame.origin = CGPointMake(20, 150);
    rendererFrame.size.width = bounds.size.width - 40;
    rendererControl.frame = rendererFrame;
    rendererControl.selectedSegmentIndex = bsLoadRendererPreference();
    rendererControl.enabled = supportsGLES2;
    if (!supportsGLES2) {
        rendererControl.selectedSegmentIndex = BS_RENDERER_SOFTWARE;
    }
    [root addSubview:rendererControl];

    UILabel *rendererLabel = [[[UILabel alloc] initWithFrame:CGRectMake(20, 130, bounds.size.width - 40, 20)] autorelease];
    rendererLabel.text = @"Renderer:";
    rendererLabel.font = [UIFont systemFontOfSize:15.0f];
    rendererLabel.backgroundColor = [UIColor clearColor];
    [root addSubview:rendererLabel];
#endif

    self.view = root;
}

- (id)init {
    if ((self = [super init])) {
        self.title = @"Settings";
        UIBarButtonItem *saveItem = [[UIBarButtonItem alloc] initWithTitle:@"Save"
                                      style:UIBarButtonItemStyleDone
                                      target:self action:@selector(saveTapped)];
        self.navigationItem.rightBarButtonItem = saveItem;
        [saveItem release];
    }
    return self;
}

- (void)saveTapped {
    [speedField resignFirstResponder];
    double v = [speedField.text doubleValue];
    if (v > 0.0) {
        [[NSUserDefaults standardUserDefaults] setDouble:v forKey:BS_FF_SPEED_DEFAULTS_KEY];
    }
    /* Invalid/non-positive input is silently ignored -- previously saved
     * value (or the default) is left untouched. */

    [[NSUserDefaults standardUserDefaults] setBool:highResSwitch.isOn forKey:BS_HIGH_RES_DEFAULTS_KEY];
#if defined(ENABLE_MODERN_GL) && defined(ENABLE_SW_RENDERER)
    if (rendererControl)
        [[NSUserDefaults standardUserDefaults] setInteger:rendererControl.selectedSegmentIndex forKey:BS_RENDERER_DEFAULTS_KEY];
#endif
    [[NSUserDefaults standardUserDefaults] synchronize];

    [[[UIApplication sharedApplication] delegate] performSelector:@selector(settingsDone)];
}

- (BOOL)textFieldShouldReturn:(UITextField *)textField {
    [textField resignFirstResponder];
    return YES;
}

- (void)dealloc {
    [speedField release];
    [highResSwitch release];
    if (rendererControl) [rendererControl release];
    [super dealloc];
}

@end

/* ---------------------------------------------------------------------
 * Game selection menu. Scans BS_GAMES_ROOT_PATH for subfolders that
 * contain a data.win, and lets the user pick one. Re-scans every time
 * the view (re)appears so games dropped in via file transfer while the
 * app is running (or after returning from a game) show up.
 * ------------------------------------------------------------------- */

@interface BSGameListViewController : UITableViewController <UIAlertViewDelegate> {
    NSMutableArray *games;
    NSIndexPath *pendingDeleteIndexPath;
}
- (void)reloadGames;
@end

@implementation BSGameListViewController

- (id)init {
#if __IPHONE_OS_VERSION_MIN_REQUIRED >= 30000
    if ((self = [super initWithStyle:UITableViewStylePlain])) {
#else
    if ((self = [super init])) {
#endif
        games = [[NSMutableArray alloc] init];
        self.title = @"Butterscotch";

        UIImage *gearImg = createGearIconImage(22.0f, [UIColor darkGrayColor]);
        UIBarButtonItem *gearItem = [[UIBarButtonItem alloc] initWithImage:gearImg
                                      style:UIBarButtonItemStylePlain
                                      target:self action:@selector(settingsTapped)];
        self.navigationItem.leftBarButtonItem = gearItem;
        [gearItem release];

        [self reloadGames];
    }
    return self;
}

- (void)settingsTapped {
    [[[UIApplication sharedApplication] delegate] performSelector:@selector(showSettings)];
}

- (void)reloadGames {
    [games removeAllObjects];

    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray *entries = [fm contentsOfDirectoryAtPath:BS_GAMES_ROOT_PATH error:NULL];
    for (NSString *name in entries) {
        NSString *dir = [BS_GAMES_ROOT_PATH stringByAppendingPathComponent:name];
        BOOL isDir = NO;
        if (![fm fileExistsAtPath:dir isDirectory:&isDir] || !isDir) continue;

        NSString *dataWin = [dir stringByAppendingPathComponent:@"data.win"];
        if ([fm fileExistsAtPath:dataWin]) [games addObject:name];
    }

    [self.tableView reloadData];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self reloadGames];
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return [games count];
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    static NSString *reuseId = @"BSGameCell";
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:reuseId];
    if (!cell) {
#if __IPHONE_OS_VERSION_MIN_REQUIRED >= 30000
        cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:reuseId] autorelease];
#else
        cell = [[[UITableViewCell alloc] initWithFrame:CGRectZero reuseIdentifier:reuseId] autorelease];
#endif
    }
#if __IPHONE_OS_VERSION_MIN_REQUIRED >= 30000
    cell.textLabel.text = [games objectAtIndex:indexPath.row];
#else
    cell.text = [games objectAtIndex:indexPath.row];
#endif
    return cell;
}

/* Enables the standard swipe-left-to-delete gesture for every row; UIKit
 * reveals its built-in red "Delete" button automatically once this
 * returns YES, no custom gesture recognizer needed. */
- (BOOL)tableView:(UITableView *)tableView canEditRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView; (void)indexPath;
    return YES;
}

/* Called when the user actually taps the revealed Delete button. We don't
 * touch the filesystem here -- removing a game folder also wipes any save
 * data living alongside data.win, so we confirm first via UIAlertView
 * (rather than UIAlertController, which needs iOS 8+) and do the real
 * removal from the alert's delegate callback below. */
- (void)tableView:(UITableView *)tableView commitEditingStyle:(UITableViewCellEditingStyle)editingStyle
       forRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView;
    if (editingStyle != UITableViewCellEditingStyleDelete) return;

    NSString *name = [games objectAtIndex:indexPath.row];

    [pendingDeleteIndexPath release];
    pendingDeleteIndexPath = [indexPath retain];

    NSString *msg = [NSString stringWithFormat:
        @"Delete \"%@\" and all of its save data? This cannot be undone.", name];
    UIAlertView *alert = [[UIAlertView alloc] initWithTitle:@"Delete Game"
                                                      message:msg
                                                     delegate:self
                                            cancelButtonTitle:@"Cancel"
                                            otherButtonTitles:@"Delete", nil];
    [alert show];
    [alert release];
}

- (void)alertView:(UIAlertView *)alertView clickedButtonAtIndex:(NSInteger)buttonIndex {
    NSIndexPath *indexPath = pendingDeleteIndexPath;
    pendingDeleteIndexPath = nil;

    if (!indexPath) return;
    if (buttonIndex == alertView.cancelButtonIndex) {
        [indexPath release];
        return;
    }

    NSString *name = [games objectAtIndex:indexPath.row];
    NSString *dir = [BS_GAMES_ROOT_PATH stringByAppendingPathComponent:name];

    /* removeItemAtPath: recursively removes directory contents, so this
     * takes data.win and any save files sitting next to it in one shot. */
    NSError *error = nil;
    if ([[NSFileManager defaultManager] removeItemAtPath:dir error:&error]) {
        [games removeObjectAtIndex:indexPath.row];
        [self.tableView deleteRowsAtIndexPaths:[NSArray arrayWithObject:indexPath]
                               withRowAnimation:UITableViewRowAnimationFade];
    } else {
        NSLog(@"Butterscotch: failed to delete game directory %@: %@", dir, error);
        /* Directory listing may now be stale (partial delete, permissions
         * error, etc) -- resync from disk rather than trust our cache. */
        [self reloadGames];
    }

    [indexPath release];
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    NSString *folder = [games objectAtIndex:indexPath.row];
    id delegate = [[UIApplication sharedApplication] delegate];
    [delegate performSelector:@selector(startGameWithFolder:) withObject:folder];
}

- (void)dealloc {
    [pendingDeleteIndexPath release];
    [games release];
    [super dealloc];
}

@end

extern int game_main(int argc, char *argv[]);

@interface AppDelegate : NSObject <UIApplicationDelegate> {
    UIWindow *window;
    GLView *view;
    BSTouchOverlay *overlay;
    UIView *rootView;
    BSGameListViewController *gameListVC;
    BSSettingsViewController *settingsVC;
    UINavigationController *navController;
    BOOL usingRootViewController;
}
- (void)startGameWithFolder:(NSString *)folderName;
- (void)returnToMenu;
- (void)showSettings;
- (void)settingsDone;
- (void)orientationChanged:(NSNotification *)note;
- (void)applyDeviceOrientation:(UIDeviceOrientation)devOrientation;
@end

@implementation AppDelegate

- (void)gameThread {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

    static char arg0[] = "butterscotch";
    static char arg1[] = "--lazy-textures";
    static char arg2[] = "--lazy-rooms";
    static char arg3[] = "--renderer";
    char *argv[] = { arg0, arg1, arg2, g_ffSpeedArg, arg3, g_rendererArg, g_gamePath, NULL };
    game_main(7, argv);

    [self performSelectorOnMainThread:@selector(returnToMenu) withObject:nil waitUntilDone:NO];

    [pool release];
}

- (void)startGameWithFolder:(NSString *)folderName {
    NSString *dataWin = [[BS_GAMES_ROOT_PATH stringByAppendingPathComponent:folderName]
                          stringByAppendingPathComponent:@"data.win"];
    strlcpy(g_gamePath, [dataWin fileSystemRepresentation], sizeof(g_gamePath));
    snprintf(g_ffSpeedArg, sizeof(g_ffSpeedArg), "--fast-forward-speed=%g", bsLoadFastForwardSpeed());
    atomic_store(&g_highResEnabled, bsLoadHighResEnabled());

#if defined(ENABLE_MODERN_GL) && defined(ENABLE_SW_RENDERER)
    int rendererPref = bsLoadRendererPreference();
    if (rendererPref == BS_RENDERER_MODERN_GL && bsSupportsGLES2()) {
        snprintf(g_rendererArg, sizeof(g_rendererArg), "modern-gl");
    } else {
        snprintf(g_rendererArg, sizeof(g_rendererArg), "software");
    }
#elif defined(ENABLE_MODERN_GL)
    snprintf(g_rendererArg, sizeof(g_rendererArg), "modern-gl");
#else
    snprintf(g_rendererArg, sizeof(g_rendererArg), "software");
#endif

    atomic_store(&quitRequested, false);

    CGRect bounds = [[UIScreen mainScreen] bounds];
    BSLayout bsLayout = computeLayout(bounds.size);

    view = [[GLView alloc] initWithFrame:bsLayout.gameFrame];
    overlay = [[BSTouchOverlay alloc] initWithFrame:bounds];
    g_glView = view;
    g_overlayView = overlay;

    rootView = [[UIView alloc] initWithFrame:bounds];
    rootView.autoresizingMask = UIViewAutoresizingNone; /* we drive rootView's frame manually now -- autoresizing masks are unreliable once a transform is applied */
    [rootView addSubview:view];
    [rootView addSubview:overlay]; /* on top, so controls are visible over the game */

    /* Force a fresh sync even if the device is in the same physical
     * orientation the last game session ended in -- g_lastAppliedOrientation
     * is stale for this brand-new rootView. */
    g_lastAppliedOrientation = UIDeviceOrientationUnknown;
    [self applyDeviceOrientation:[[UIDevice currentDevice] orientation]];

    if (usingRootViewController) {
        UIViewController *vc = [[BSViewController alloc] init];
        vc.view = rootView;
        [window performSelector:@selector(setRootViewController:) withObject:vc];
        [vc release];
    } else {
        NSArray *subs = [[window.subviews copy] autorelease];
        for (UIView *sub in subs) [sub removeFromSuperview];
        [window addSubview:rootView];
    }

    [NSThread detachNewThreadSelector:@selector(gameThread) toTarget:self withObject:nil];
}

- (void)returnToMenu {
    g_glView = nil;
    g_overlayView = nil;

    if (usingRootViewController) {
        [window performSelector:@selector(setRootViewController:) withObject:navController];
    } else {
        NSArray *subs = [[window.subviews copy] autorelease];
        for (UIView *sub in subs) [sub removeFromSuperview];
        [window addSubview:navController.view];
    }

    [overlay release];
    overlay = nil;
    [rootView release];
    rootView = nil;
    [view release];
    view = nil;
}

- (void)showSettings {
    if (!settingsVC) settingsVC = [[BSSettingsViewController alloc] init];
    [navController pushViewController:settingsVC animated:YES];
}

- (void)settingsDone {
    [navController popViewControllerAnimated:YES];
}

- (void)orientationChanged:(NSNotification *)note {
    (void)note;
    [self applyDeviceOrientation:[[UIDevice currentDevice] orientation]];
}

/* ---------------------------------------------------------------------
 * Manual rotation for the game screen. Locks the interface orientation
 * (see BSViewController) and instead rotates rootView ourselves off raw
 * UIDeviceOrientation notifications, which have been stable since iOS
 * 2.0 -- unlike the view-controller rotation callback chain, which isn't.
 *
 * rootView's children (GLView, BSTouchOverlay) are untouched by this;
 * they just read superview.bounds.size on layout, same as always, so
 * computeLayout()'s existing portrait/landscape logic keeps working
 * without any changes on that side.
 *
 * Only applies when a game is running (rootView != nil). The menu /
 * settings navigation stack is left in the OS's default fixed
 * orientation and does not rotate.
 * ------------------------------------------------------------------- */
- (void)applyDeviceOrientation:(UIDeviceOrientation)devOrientation {
    if (!rootView) return;

    CGFloat angle;
    BOOL swapped;
    switch (devOrientation) {
        case UIDeviceOrientationPortrait:           angle = 0.0f;             swapped = NO;  break;
        case UIDeviceOrientationPortraitUpsideDown: angle = (CGFloat)M_PI;    swapped = NO;  break;
        /* Device rotated so its left edge points "up" corresponds to
         * interface orientation LandscapeRight, i.e. a +90 degree
         * compensating rotation; the other case is the mirror image. */
        case UIDeviceOrientationLandscapeLeft:      angle = (CGFloat)M_PI_2;  swapped = YES; break;
        case UIDeviceOrientationLandscapeRight:     angle = -(CGFloat)M_PI_2; swapped = YES; break;
        default:
            /* FaceUp / FaceDown / Unknown: not a usable orientation --
             * keep whatever we last applied. */
            return;
    }

    if (devOrientation == g_lastAppliedOrientation) return;
    g_lastAppliedOrientation = devOrientation;

    CGRect nativeBounds = [[UIScreen mainScreen] bounds];
    CGSize logicalSize = swapped ? CGSizeMake(nativeBounds.size.height, nativeBounds.size.width)
                                  : nativeBounds.size;

    [UIView beginAnimations:nil context:NULL];
    [UIView setAnimationDuration:0.3];
    rootView.transform = CGAffineTransformMakeRotation(angle);
    rootView.bounds = CGRectMake(0, 0, logicalSize.width, logicalSize.height);
    rootView.center = CGPointMake(CGRectGetMidX(nativeBounds), CGRectGetMidY(nativeBounds));
    [UIView commitAnimations];

    bsRequestRelayout();
}

- (void)applicationDidFinishLaunching:(UIApplication *)application {
    [application setStatusBarHidden:YES];

    [[UIDevice currentDevice] beginGeneratingDeviceOrientationNotifications];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                              selector:@selector(orientationChanged:)
                                                  name:UIDeviceOrientationDidChangeNotification
                                                object:nil];

    CGRect bounds = [[UIScreen mainScreen] bounds];
    window = [[UIWindow alloc] initWithFrame:bounds];
    usingRootViewController = [window respondsToSelector:@selector(setRootViewController:)];

    gameListVC = [[BSGameListViewController alloc] init];
    navController = [[UINavigationController alloc] initWithRootViewController:gameListVC];

    if (usingRootViewController) {
        [window performSelector:@selector(setRootViewController:) withObject:navController];
    } else {
        [window addSubview:navController.view];
    }
    [window makeKeyAndVisible];
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self name:UIDeviceOrientationDidChangeNotification object:nil];
    [[UIDevice currentDevice] endGeneratingDeviceOrientationNotifications];

    g_glView = nil;
    g_overlayView = nil;
    [overlay release];
    [rootView release];
    [view release];
    [gameListVC release];
    [settingsVC release];
    [navController release];
    [window release];
    [super dealloc];
}

@end

int main(int argc, char *argv[]) {
    freopen("/tmp/bsout", "w", stdout);
    dup2(fileno(stdout), fileno(stderr));
    setbuf(stderr, NULL);
    setbuf(stdout, NULL);

    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    int ret = UIApplicationMain(argc, argv, nil, @"AppDelegate");
    [pool release];

    return ret;
}
