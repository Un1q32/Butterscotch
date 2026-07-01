#include <stdio.h>
#include <time.h>
#include <dlfcn.h>

#include <UIKit/UIKit.h>
#include <objc/message.h>
#include <OpenGLES/EAGL.h>
#include <QuartzCore/CAEAGLLayer.h>

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

static Runner *g_runner;

static EAGLContext *glcontext;
static GLuint framebuffer;
static GLuint renderbuffer;
static GLint fbWidth  = 0;
static GLint fbHeight = 0;
static CAEAGLLayer *layer;

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
    (void)reqW; (void)reqH;
    (void)title; (void)headless;

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
        fprintf(stderr, "Failed to set the OpenGLES context\n");
        return false;
    }

    return true;
}

void platformExit(void) {
    [EAGLContext setCurrentContext:glcontext];
    if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
    if (renderbuffer) glDeleteRenderbuffers(1, &renderbuffer);
    [glcontext release];
}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;

    /* this can't be in platformInit because glad hasn't initialized yet */
    glGenFramebuffers(1, &framebuffer);
    glGenRenderbuffers(1, &renderbuffer);

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    // Color renderbuffer — sized by the drawable
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    [glcontext renderbufferStorage:GL_RENDERBUFFER fromDrawable:layer];
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &fbWidth);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &fbHeight);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);

    // IMPORTANT: presentRenderbuffer: presents whatever is bound to
    // GL_RENDERBUFFER at call time, not a framebuffer attachment.
    // Leave the *color* renderbuffer bound, not the depth one.
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "iOS framebuffer incomplete: 0x%x\n", status);
    }

    ((GLRenderer *)runner->renderer)->hostFramebuffer = framebuffer;
}

void platformSwapBuffers(void) {
    [glcontext presentRenderbuffer:GL_RENDERBUFFER];
}

void *platformGetProcAddress(const char *name) {
    return dlsym(RTLD_NEXT, name);
}

bool platformHandleEvents(void) {
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
    }
    return self;
}

- (void)dealloc {
    [super dealloc];
}

@end

extern int game_main(int argc, char *argv[]);

@interface AppDelegate : NSObject <UIApplicationDelegate> {
    UIWindow *window;
    GLView *view;
}
@end

@implementation AppDelegate

- (void)game {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

    static char arg0[] = "butterscotch";
    static char arg1[] = "/var/mobile/Documents/Butterscotch/undertale/data.win";
    char *argv[] = { arg0, arg1, NULL };
    game_main(2, argv);

    [pool release];
}

- (void)applicationDidFinishLaunching:(UIApplication *)application {
    CGRect bounds = [[UIScreen mainScreen] bounds];

    window = [[UIWindow alloc] initWithFrame:bounds];
    view = [[GLView alloc] initWithFrame:bounds];

    if ([window respondsToSelector:@selector(setRootViewController:)]) {
        UIViewController *vc = [[UIViewController alloc] init];
        vc.view = view;
        [window performSelector:@selector(setRootViewController:) withObject:vc];
    } else {
        [window addSubview:view];
    }
    [window makeKeyAndVisible];

    [NSThread detachNewThreadSelector:@selector(game) toTarget:self withObject:nil];
}

- (void)dealloc {
    [view release];
    [window release];
    [super dealloc];
}

@end

int main(int argc, char *argv[]) {
    freopen("/tmp/bsout", "w", stderr);
    freopen("/tmp/bsout", "w", stdout);
    setbuf(stderr, NULL);
    setbuf(stdout, NULL);

    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    int ret = UIApplicationMain(argc, argv, nil, @"AppDelegate");
    [pool release];

    return ret;
}
