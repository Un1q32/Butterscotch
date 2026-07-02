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

GLuint framebuffer;
static EAGLContext *glcontext;
static GLuint renderbuffer;
static bool glInited = false;
static GLint fbWidth  = 0;
static GLint fbHeight = 0;
static CAEAGLLayer *layer;

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

/* Path to the selected game's data.win, filled in by the game list
 * controller before the game thread is started. */
static char g_gamePath[PATH_MAX];

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
#define BS_GAME_TOP_PADDING           (BS_QUIT_BUTTON_MARGIN + BS_QUIT_BUTTON_SIZE + 4.0f)

typedef struct {
    CGRect gameFrame;
    CGRect dpadFrame;
    CGRect buttonsFrame;
    CGRect quitFrame;
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
 * TODO: allow this to resize the render resolution when the target value is
 * lower than the screen resolution, so we can gain some performance on early
 * retina devices by not being forced to always render at full resolution.
 */
void platformSetWindowSize(int32_t width, int32_t height) {
    (void)width; (void)height;
}

/* TODO: touchscreen mouse support */
void platformGetMousePos(double *xPos, double *yPos) {
    *xPos = 0.0;
    *yPos = 0.0;
}

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless) {
    (void)title; (void)headless;

    g_aspectRatio = (reqH > 0) ? ((float)reqW / (float)reqH) : 1.0f;
    bsRequestRelayout();

#ifdef ENABLE_MODERN_GL
    if (gfx == MODERN_GL)
        glcontext = [[EAGLContext alloc] initWithAPI:3];
        if (!glcontext)
            glcontext = [[EAGLContext alloc] initWithAPI:2];
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
    [glcontext release];
    glcontext = nil;
    framebuffer = 0;
    renderbuffer = 0;
    glInited = false;
}

static void resizeFramebuffer(void) {
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
        glGenFramebuffers(1, &framebuffer);
        glGenRenderbuffers(1, &renderbuffer);
        glInited = true;
    }

    resizeFramebuffer();
    atomic_store(&needsResize, false);
}

void platformSwapBuffers(void) {
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

        UIScreen *screen = [UIScreen mainScreen];
        CGFloat scale = 1.0f; /* pre-iOS 4: no retina, 1x is correct */
        if ([screen respondsToSelector:@selector(scale)]) {
            CGFloat (*getScale)(id, SEL) = (CGFloat (*)(id, SEL))objc_msgSend;
            scale = getScale(screen, @selector(scale));
        }
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
    for (int i = 0; i < 3; i++) if (buttonTouches[i]) [buttonTouches[i] release];
    [super dealloc];
}

@end

/*
 * Binaries linked against pre-iOS-6 SDKs get rotation handled via the legacy
 * shouldAutorotateToInterfaceOrientation: API, even when running on newer
 * OS versions. Plain UIViewController's default implementation only allows
 * UIInterfaceOrientationPortrait, which silently blocks all rotation unless
 * overridden here.
 */
@interface BSViewController : UIViewController
@end

@implementation BSViewController

- (BOOL)shouldAutorotateToInterfaceOrientation:(UIInterfaceOrientation)interfaceOrientation {
    (void)interfaceOrientation;
    return YES;
}

#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 50000
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    bsRequestRelayout();
}
#endif

/* Pre-iOS-7: without this, the view is offset 20pt down to make room for
 * the status bar, even though we hide the status bar at launch. */
- (BOOL)wantsFullScreenLayout {
    return YES;
}

@end

/* ---------------------------------------------------------------------
 * Game selection menu. Scans BS_GAMES_ROOT_PATH for subfolders that
 * contain a data.win, and lets the user pick one. Re-scans every time
 * the view (re)appears so games dropped in via file transfer while the
 * app is running (or after returning from a game) show up.
 * ------------------------------------------------------------------- */

@interface BSGameListViewController : UITableViewController {
    NSMutableArray *games;
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
        [self reloadGames];
    }
    return self;
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

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    NSString *folder = [games objectAtIndex:indexPath.row];
    id delegate = [[UIApplication sharedApplication] delegate];
    [delegate performSelector:@selector(startGameWithFolder:) withObject:folder];
}

- (void)dealloc {
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
    BOOL usingRootViewController;
}
- (void)startGameWithFolder:(NSString *)folderName;
- (void)returnToMenu;
@end

@implementation AppDelegate

- (void)gameThread {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

    static char arg0[] = "butterscotch";
    char *argv[] = { arg0, g_gamePath, NULL };
    game_main(2, argv);

    [self performSelectorOnMainThread:@selector(returnToMenu) withObject:nil waitUntilDone:NO];

    [pool release];
}

- (void)startGameWithFolder:(NSString *)folderName {
    NSString *dataWin = [[BS_GAMES_ROOT_PATH stringByAppendingPathComponent:folderName]
                          stringByAppendingPathComponent:@"data.win"];
    strlcpy(g_gamePath, [dataWin fileSystemRepresentation], sizeof(g_gamePath));

    atomic_store(&quitRequested, false);

    CGRect bounds = [[UIScreen mainScreen] bounds];
    BSLayout bsLayout = computeLayout(bounds.size);

    view = [[GLView alloc] initWithFrame:bsLayout.gameFrame];
    overlay = [[BSTouchOverlay alloc] initWithFrame:bounds];
    g_glView = view;
    g_overlayView = overlay;

    rootView = [[UIView alloc] initWithFrame:bounds];
    rootView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [rootView addSubview:view];
    [rootView addSubview:overlay]; /* on top, so controls are visible over the game */

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
        [window performSelector:@selector(setRootViewController:) withObject:gameListVC];
    } else {
        NSArray *subs = [[window.subviews copy] autorelease];
        for (UIView *sub in subs) [sub removeFromSuperview];
        [window addSubview:gameListVC.view];
    }

    [overlay release];
    overlay = nil;
    [rootView release];
    rootView = nil;
    [view release];
    view = nil;
}

- (void)applicationDidFinishLaunching:(UIApplication *)application {
    [application setStatusBarHidden:YES];

    CGRect bounds = [[UIScreen mainScreen] bounds];
    window = [[UIWindow alloc] initWithFrame:bounds];
    usingRootViewController = [window respondsToSelector:@selector(setRootViewController:)];

    gameListVC = [[BSGameListViewController alloc] init];

    if (usingRootViewController) {
        [window performSelector:@selector(setRootViewController:) withObject:gameListVC];
    } else {
        [window addSubview:gameListVC.view];
    }
    [window makeKeyAndVisible];
}

- (void)dealloc {
    g_glView = nil;
    g_overlayView = nil;
    [overlay release];
    [rootView release];
    [view release];
    [gameListVC release];
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
