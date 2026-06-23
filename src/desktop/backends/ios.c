#include <stdio.h>
#include <time.h>
#include <dlfcn.h>

#include <UIKit/UIKit.h>
#include <objc/message.h>
#include <OpenGLES/ES1/glext.h>
#include <OpenGLES/EAGL.h>
#include <QuartzCore/CAEAGLLayer.h>

#ifdef ENABLE_SW_RENDERER
#include <glad/glad.h>
#endif

#include "common.h"
#include "input_recording.h"
#include "desktop/platformdefs.h"
#include "gettime.h"
#include "runner_mouse.h"

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

#ifdef ENABLE_SW_RENDERER
static GLuint fbtexture;
#endif

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
        fprintf(stderr, "Failed to set the OpenGLES context\n");
        return false;
    }

    glGenFramebuffersOES(1, &framebuffer);
    glGenRenderbuffersOES(1, &renderbuffer);

    glBindFramebufferOES(GL_FRAMEBUFFER_OES, framebuffer);
    glBindRenderbufferOES(GL_RENDERBUFFER_OES, renderbuffer);

    [glcontext renderbufferStorage:GL_RENDERBUFFER_OES fromDrawable:layer];

    glGetRenderbufferParameterivOES(GL_RENDERBUFFER_OES,
                                     GL_RENDERBUFFER_WIDTH_OES,
                                     &fbWidth);

    glGetRenderbufferParameterivOES(GL_RENDERBUFFER_OES,
                                     GL_RENDERBUFFER_HEIGHT_OES,
                                     &fbHeight);

    glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES,
                                 GL_COLOR_ATTACHMENT0_OES,
                                 GL_RENDERBUFFER_OES,
                                 renderbuffer);

    return true;
}

void platformExit(void) {
    [EAGLContext setCurrentContext:glcontext];
    if (framebuffer) glDeleteFramebuffersOES(1, &framebuffer);
    if (renderbuffer) glDeleteRenderbuffersOES(1, &renderbuffer);
    [glcontext release];
}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;
#ifdef ENABLE_SW_RENDERER
    if (gfx == SOFTWARE) {
        glGenTextures(1, &fbtexture);
        glBindTexture(GL_TEXTURE_2D, fbtexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glEnable(GL_TEXTURE_2D);
    }
#endif
}

#ifdef ENABLE_SW_RENDERER

static uint32_t* nextFb = NULL;

void Runner_setNextFrame(uint32_t* framebuffer, int width, int height) {
    nextFb = framebuffer;
    fbWidth = width;
    fbHeight = height;
}

#endif

void platformSwapBuffers(void) {
#ifdef ENABLE_SW_RENDERER
    if (gfx == SOFTWARE && nextFb) {
        glBindFramebufferOES(GL_FRAMEBUFFER_OES, framebuffer);
        glBindTexture(GL_TEXTURE_2D, fbtexture);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            fbWidth,
            fbHeight,
            0,
            GL_BGRA,
            GL_UNSIGNED_BYTE,
            nextFb
        );

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrthof(-1, 1, -1, 1, -1, 1);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glBindTexture(GL_TEXTURE_2D, fbtexture);

        glEnable(GL_TEXTURE_2D);

        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);

        GLfloat verts[] = {
            -1, -1,
             1, -1,
            -1,  1,
             1,  1
        };

        GLfloat uv[] = {
            0, 1,
            1, 1,
            0, 0,
            1, 0
        };

        glVertexPointer(2, GL_FLOAT, 0, verts);
        glTexCoordPointer(2, GL_FLOAT, 0, uv);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        nextFb = NULL;
    }
#endif
    //static float color = 1.0f;
    //color -= 0.01f;
    //if (color < 0.0f)
    //    color = 1.0f;
    //glClearColor(color, color, color, 1.0f);
    //glClear(GL_COLOR_BUFFER_BIT);
    [glcontext presentRenderbuffer:GL_RENDERBUFFER_OES];
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
    static char arg2[] = "--renderer=software";
    char *argv[] = { arg0, arg1, arg2, NULL };
    game_main(3, argv);

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
    setbuf(stdout, NULL);

    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    int ret = UIApplicationMain(argc, argv, nil, @"AppDelegate");
    [pool release];

    return ret;
}
