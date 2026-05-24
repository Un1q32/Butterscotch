// Butterscotch iOS shell — targets iPhone OS 3.1.3 SDK (armv6),
// designed to run on a jailbroken iPod Touch 2G (PowerVR MBX Lite,
// 128 MB RAM, OpenGL ES 1.1 only).
//
// One compilation unit: AppDelegate + GamePickerVC + GLView +
// touch overlay (D-pad + Z/X/C action buttons). iOS 3 SDK predates
// ARC, so we use manual retain/release; Theos' default for our
// Makefile is -fno-objc-arc.
//
// Game discovery:
//   - Scans /var/mobile/Documents/Butterscotch/ for sub-directories
//     containing a data.win file (intended for jailbroken installs
//     via Cydia/.deb).
//   - Falls back to the app's own Documents/ directory inside the
//     sandbox if /var/mobile/Documents/Butterscotch/ is unreadable,
//     so the picker still works without escalated access.
//
// Each sub-directory containing data.win is shown as one row.
// Tapping a row presents a full-screen game view controller; the
// nav bar is hidden so the game uses the entire screen. A small
// translucent "✕" button in the corner dismisses back to the picker.
//
// The picker stays portrait; the game view runs in landscape — the
// natural orientation for a 640x480 4:3 GMS:Studio game on a
// 480x320 iPod Touch 2G screen, and the layout the user requested.

#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/EAGLDrawable.h>
#import <OpenGLES/ES1/gl.h>
#import <OpenGLES/ES1/glext.h>
#import <Foundation/Foundation.h>

// Butterscotch runtime headers.
#include "data_win.h"
#include "vm.h"
#include "runner.h"
#include "renderer.h"
#include "audio_system.h"
#include "file_system.h"
#include "noop_audio_system.h"
#include "overlay_file_system.h"
#include "runner_keyboard.h"

// From src/gles/gles1_renderer.h
Renderer* GLES1Renderer_create(void);
void GLES1Renderer_setDataWinPath(Renderer* r, const char* path);

// ============================================================================
// GameEntry — one playable folder
// ============================================================================

@interface BSGameEntry : NSObject {
@public
    NSString* name;          // display name (= folder name)
    NSString* directory;     // absolute path to the folder
    NSString* dataWinPath;   // absolute path to data.win inside it
}
@end

@implementation BSGameEntry
- (void)dealloc {
    [name release];
    [directory release];
    [dataWinPath release];
    [super dealloc];
}
@end

// Return paths to scan, in order. The first readable, non-empty
// directory wins.
static NSArray* BSCandidateRootDirectories(void) {
    NSMutableArray* roots = [NSMutableArray array];
    [roots addObject:@"/var/mobile/Documents/Butterscotch"];
    [roots addObject:@"/var/mobile/Media/Butterscotch"];
    NSArray* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    if ([docs count] > 0) {
        [roots addObject:[[docs objectAtIndex:0] stringByAppendingPathComponent:@"Butterscotch"]];
        [roots addObject:[docs objectAtIndex:0]];
    }
    return roots;
}

// Forward decl — full definition is just below BSScanGames.
static NSInteger BSGameEntryCompare(id a, id b, void* ctx);

// Scan a single directory for sub-directories that contain data.win.
// Sorted alphabetically by folder name.
static NSArray* BSScanGames(NSString* rootDir) {
    NSFileManager* fm = [NSFileManager defaultManager];
    BOOL isDir = NO;
    if (![fm fileExistsAtPath:rootDir isDirectory:&isDir] || !isDir) {
        return nil;
    }
    NSArray* children = [fm contentsOfDirectoryAtPath:rootDir error:NULL];
    if (children == nil) return nil;

    NSMutableArray* out = [NSMutableArray array];
    NSEnumerator* it = [children objectEnumerator];
    NSString* child;
    while ((child = [it nextObject]) != nil) {
        if ([child hasPrefix:@"."]) continue;
        NSString* full = [rootDir stringByAppendingPathComponent:child];
        BOOL childIsDir = NO;
        if (![fm fileExistsAtPath:full isDirectory:&childIsDir] || !childIsDir) continue;
        NSString* dataWin = [full stringByAppendingPathComponent:@"data.win"];
        if (![fm isReadableFileAtPath:dataWin]) continue;
        BSGameEntry* e = [[BSGameEntry alloc] init];
        e->name = [child retain];
        e->directory = [full retain];
        e->dataWinPath = [dataWin retain];
        [out addObject:e];
        [e release];
    }
    [out sortUsingFunction:BSGameEntryCompare context:NULL];
    return out;
}

static NSInteger BSGameEntryCompare(id a, id b, void* ctx) {
    (void) ctx;
    return [((BSGameEntry*) a)->name caseInsensitiveCompare:((BSGameEntry*) b)->name];
}

// ============================================================================
// GLView — owns the EAGLContext + framebuffer + renderbuffer
// ============================================================================

@protocol BSGLViewDelegate <NSObject>
- (void)glViewTick:(int)frameIndex backingWidth:(GLint)w backingHeight:(GLint)h;
@end

@interface BSGLView : UIView {
    EAGLContext* _ctx;
    GLuint _framebuffer;
    GLuint _renderbuffer;
    GLint _backingWidth;
    GLint _backingHeight;
    CADisplayLink* _displayLink;
    NSTimer* _fallbackTimer;
    int _frameCounter;
    id<BSGLViewDelegate> _delegate;  // weak, lifetime tied to view controller
}
@property (nonatomic, assign) id<BSGLViewDelegate> delegate;
@property (nonatomic, readonly) GLint backingWidth;
@property (nonatomic, readonly) GLint backingHeight;
- (void)bindDrawable;
- (void)presentDrawable;
- (void)makeContextCurrent;
- (void)resizeRenderbuffer;
- (void)recreateRenderbuffer;
- (void)startRunLoop;
- (void)stopRunLoop;
@end

@implementation BSGLView
@synthesize delegate = _delegate;
@synthesize backingWidth = _backingWidth;
@synthesize backingHeight = _backingHeight;

+ (Class)layerClass {
    return [CAEAGLLayer class];
}

- (id)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self == nil) return nil;

    CAEAGLLayer* eaglLayer = (CAEAGLLayer*) self.layer;
    eaglLayer.opaque = YES;
    eaglLayer.drawableProperties = [NSDictionary dictionaryWithObjectsAndKeys:
        [NSNumber numberWithBool:NO], kEAGLDrawablePropertyRetainedBacking,
        kEAGLColorFormatRGBA8, kEAGLDrawablePropertyColorFormat,
        nil];
    NSLog(@"[Butterscotch] BSGLView initWithFrame: frame=%.0fx%.0f layer.bounds=%.0fx%.0f",
          frame.size.width, frame.size.height,
          eaglLayer.bounds.size.width, eaglLayer.bounds.size.height);

    _ctx = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES1];
    if (_ctx == nil || ![EAGLContext setCurrentContext:_ctx]) {
        NSLog(@"[Butterscotch] FATAL: failed to create GLES1 context");
        [self release];
        return nil;
    }

    glGenFramebuffersOES(1, &_framebuffer);
    glGenRenderbuffersOES(1, &_renderbuffer);
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, _framebuffer);
    glBindRenderbufferOES(GL_RENDERBUFFER_OES, _renderbuffer);
    [_ctx renderbufferStorage:GL_RENDERBUFFER_OES fromDrawable:eaglLayer];
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_RENDERBUFFER_OES, _renderbuffer);
    glGetRenderbufferParameterivOES(GL_RENDERBUFFER_OES, GL_RENDERBUFFER_WIDTH_OES, &_backingWidth);
    glGetRenderbufferParameterivOES(GL_RENDERBUFFER_OES, GL_RENDERBUFFER_HEIGHT_OES, &_backingHeight);

    if (glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES) != GL_FRAMEBUFFER_COMPLETE_OES) {
        NSLog(@"[Butterscotch] FATAL: framebuffer incomplete (%dx%d)", (int) _backingWidth, (int) _backingHeight);
    } else {
        NSLog(@"[Butterscotch] GLES1 context: initial framebuffer %dx%d", (int) _backingWidth, (int) _backingHeight);
    }

    glViewport(0, 0, _backingWidth, _backingHeight);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    [_ctx presentRenderbuffer:GL_RENDERBUFFER_OES];
    return self;
}

- (void)makeContextCurrent {
    [EAGLContext setCurrentContext:_ctx];
}

- (void)bindDrawable {
    [EAGLContext setCurrentContext:_ctx];
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, _framebuffer);
    glViewport(0, 0, _backingWidth, _backingHeight);
}

- (void)presentDrawable {
    glBindRenderbufferOES(GL_RENDERBUFFER_OES, _renderbuffer);
    [_ctx presentRenderbuffer:GL_RENDERBUFFER_OES];
}

- (void)resizeRenderbuffer {
    [EAGLContext setCurrentContext:_ctx];
    CGSize layerSize = self.layer.bounds.size;
    NSLog(@"[Butterscotch] resizeRenderbuffer pre: view.frame=%.0fx%.0f layer.bounds=%.0fx%.0f backing=%dx%d",
          self.frame.size.width, self.frame.size.height,
          layerSize.width, layerSize.height,
          (int) _backingWidth, (int) _backingHeight);

    // Rebind the framebuffer + renderbuffer before re-allocating storage so
    // we don't accidentally re-storage a different (stale) renderbuffer.
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, _framebuffer);
    glBindRenderbufferOES(GL_RENDERBUFFER_OES, _renderbuffer);
    // Force a fresh allocation against the layer's current drawable.
    [_ctx renderbufferStorage:GL_RENDERBUFFER_OES fromDrawable:(CAEAGLLayer*) self.layer];
    // Reattach to be safe.
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_RENDERBUFFER_OES, _renderbuffer);
    glGetRenderbufferParameterivOES(GL_RENDERBUFFER_OES, GL_RENDERBUFFER_WIDTH_OES, &_backingWidth);
    glGetRenderbufferParameterivOES(GL_RENDERBUFFER_OES, GL_RENDERBUFFER_HEIGHT_OES, &_backingHeight);
    NSLog(@"[Butterscotch] resizeRenderbuffer post: backing=%dx%d",
          (int) _backingWidth, (int) _backingHeight);
}

// Delete the existing renderbuffer object and create a fresh one bound to
// the current layer. Used after a major bounds change (eg landscape
// rotation in viewWillAppear) where -renderbufferStorage:fromDrawable: on
// the existing buffer may keep the original dimensions on iOS 3.
- (void)recreateRenderbuffer {
    [EAGLContext setCurrentContext:_ctx];
    CGSize layerSize = self.layer.bounds.size;
    NSLog(@"[Butterscotch] recreateRenderbuffer pre: view.frame=%.0fx%.0f layer.bounds=%.0fx%.0f backing=%dx%d",
          self.frame.size.width, self.frame.size.height,
          layerSize.width, layerSize.height,
          (int) _backingWidth, (int) _backingHeight);
    if (_renderbuffer != 0) {
        glDeleteRenderbuffersOES(1, &_renderbuffer);
        _renderbuffer = 0;
    }
    glGenRenderbuffersOES(1, &_renderbuffer);
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, _framebuffer);
    glBindRenderbufferOES(GL_RENDERBUFFER_OES, _renderbuffer);
    [_ctx renderbufferStorage:GL_RENDERBUFFER_OES fromDrawable:(CAEAGLLayer*) self.layer];
    glFramebufferRenderbufferOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_OES, GL_RENDERBUFFER_OES, _renderbuffer);
    glGetRenderbufferParameterivOES(GL_RENDERBUFFER_OES, GL_RENDERBUFFER_WIDTH_OES, &_backingWidth);
    glGetRenderbufferParameterivOES(GL_RENDERBUFFER_OES, GL_RENDERBUFFER_HEIGHT_OES, &_backingHeight);
    GLenum status = glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES);
    NSLog(@"[Butterscotch] recreateRenderbuffer post: backing=%dx%d status=0x%x",
          (int) _backingWidth, (int) _backingHeight, (unsigned) status);
}

- (void)layoutSubviews {
    [super layoutSubviews];
    CGSize sz = self.layer.bounds.size;
    NSLog(@"[Butterscotch] BSGLView layoutSubviews: frame=%.0fx%.0f layer.bounds=%.0fx%.0f rb=%u",
          self.frame.size.width, self.frame.size.height, sz.width, sz.height,
          (unsigned) _renderbuffer);
    if (_renderbuffer == 0) return;
    if (sz.width <= 0 || sz.height <= 0) return;
    // Only resize when dimensions actually changed — avoid thrashing the
    // drawable when iOS sends spurious layout passes with the same bounds.
    int newW = (int) sz.width;
    int newH = (int) sz.height;
    if (newW == _backingWidth && newH == _backingHeight) {
        NSLog(@"[Butterscotch] layoutSubviews: bounds unchanged, no resize");
        return;
    }
    [self resizeRenderbuffer];
}

- (void)dealloc {
    [self stopRunLoop];
    [EAGLContext setCurrentContext:_ctx];
    if (_framebuffer)  glDeleteFramebuffersOES(1, &_framebuffer);
    if (_renderbuffer) glDeleteRenderbuffersOES(1, &_renderbuffer);
    if ([EAGLContext currentContext] == _ctx) [EAGLContext setCurrentContext:nil];
    [_ctx release];
    [super dealloc];
}

- (void)tick {
    [self bindDrawable];
    _frameCounter += 1;
    if (_delegate != nil) {
        [_delegate glViewTick:_frameCounter backingWidth:_backingWidth backingHeight:_backingHeight];
    } else {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    [self presentDrawable];
}

- (void)startRunLoop {
    if (_displayLink != nil || _fallbackTimer != nil) return;
    Class dlClass = NSClassFromString(@"CADisplayLink");
    if (dlClass != nil) {
        _displayLink = [[dlClass displayLinkWithTarget:self selector:@selector(tick)] retain];
        [_displayLink setFrameInterval:1];
        [_displayLink addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
    } else {
        _fallbackTimer = [[NSTimer scheduledTimerWithTimeInterval:(1.0 / 60.0)
                                                            target:self
                                                          selector:@selector(tick)
                                                          userInfo:nil
                                                           repeats:YES] retain];
    }
}

- (void)stopRunLoop {
    if (_displayLink != nil) {
        [_displayLink invalidate];
        [_displayLink release];
        _displayLink = nil;
    }
    if (_fallbackTimer != nil) {
        [_fallbackTimer invalidate];
        [_fallbackTimer release];
        _fallbackTimer = nil;
    }
}

@end

// ============================================================================
// Touch overlay — D-pad + Z/X/C action buttons + back button
// ============================================================================
//
// One BSPadButton == one on-screen key. We bind UIControlEventTouchDown
// to a "key down" GML code, and TouchUpInside / TouchUpOutside /
// TouchCancel to "key up". Sliding off the button counts as key up;
// sliding back onto a different button does NOT count as a new press
// (UIControl doesn't support drag-in). For a D-pad this is OK in
// practice — players reposition fingers in the dead-time between
// movements anyway.

@interface BSPadButton : UIButton {
    int32_t _gmlKey;
}
@property (nonatomic, assign) int32_t gmlKey;
@end

@implementation BSPadButton
@synthesize gmlKey = _gmlKey;
@end

@protocol BSTouchOverlayDelegate <NSObject>
- (void)touchOverlayKeyDown:(int32_t)gmlKey;
- (void)touchOverlayKeyUp:(int32_t)gmlKey;
- (void)touchOverlayBackTapped;
@end

@interface BSTouchOverlay : UIView {
    id<BSTouchOverlayDelegate> _delegate;
    NSMutableArray* _buttons;
    BSPadButton* _backButton;
}
@property (nonatomic, assign) id<BSTouchOverlayDelegate> delegate;
- (id)initWithFrame:(CGRect)f delegate:(id<BSTouchOverlayDelegate>)delegate;
- (void)layoutForSize:(CGSize)size;
@end

@implementation BSTouchOverlay
@synthesize delegate = _delegate;

// Build one translucent rounded square with white text. Wired up to
// the same -keyDown:/-keyUp: handlers regardless of which key it
// represents.
- (BSPadButton*)makePadButtonWithGmlKey:(int32_t)gmlKey label:(NSString*)label fontSize:(CGFloat)fontSize {
    BSPadButton* b = [BSPadButton buttonWithType:UIButtonTypeCustom];
    b.gmlKey = gmlKey;
    [b setTitle:label forState:UIControlStateNormal];
    [b setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    [b setTitleColor:[UIColor colorWithRed:0.6f green:0.85f blue:1.0f alpha:1.0f] forState:UIControlStateHighlighted];
    b.titleLabel.font = [UIFont boldSystemFontOfSize:fontSize];
    b.backgroundColor = [UIColor colorWithWhite:0.15f alpha:0.55f];
    b.layer.cornerRadius = 8.0f;
    b.layer.borderColor = [[UIColor colorWithWhite:1.0f alpha:0.35f] CGColor];
    b.layer.borderWidth = 1.0f;
    [b addTarget:self action:@selector(keyDown:) forControlEvents:UIControlEventTouchDown];
    [b addTarget:self action:@selector(keyUp:) forControlEvents:UIControlEventTouchUpInside];
    [b addTarget:self action:@selector(keyUp:) forControlEvents:UIControlEventTouchUpOutside];
    [b addTarget:self action:@selector(keyUp:) forControlEvents:UIControlEventTouchCancel];
    [b addTarget:self action:@selector(keyUp:) forControlEvents:UIControlEventTouchDragOutside];
    [self addSubview:b];
    [_buttons addObject:b];
    return b;
}

- (id)initWithFrame:(CGRect)f delegate:(id<BSTouchOverlayDelegate>)delegate {
    self = [super initWithFrame:f];
    if (self == nil) return nil;
    _delegate = delegate;
    _buttons = [[NSMutableArray alloc] init];
    self.opaque = NO;
    self.backgroundColor = [UIColor clearColor];
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

    // D-pad: 4 arrow buttons in a + cross. GML vk_left/right/up/down.
    [self makePadButtonWithGmlKey:VK_UP    label:@"\u25B2" fontSize:22]; // ▲
    [self makePadButtonWithGmlKey:VK_DOWN  label:@"\u25BC" fontSize:22]; // ▼
    [self makePadButtonWithGmlKey:VK_LEFT  label:@"\u25C0" fontSize:22]; // ◀
    [self makePadButtonWithGmlKey:VK_RIGHT label:@"\u25B6" fontSize:22]; // ▶

    // Action buttons: Z = confirm, X = cancel, C = menu, plus Shift = hold-to-run.
    [self makePadButtonWithGmlKey:'Z' label:@"Z" fontSize:24];
    [self makePadButtonWithGmlKey:'X' label:@"X" fontSize:24];
    [self makePadButtonWithGmlKey:'C' label:@"C" fontSize:24];
    [self makePadButtonWithGmlKey:VK_SHIFT label:@"\u21E7" fontSize:22]; // ⇧

    // Back button: top-left, mostly transparent so it doesn't disrupt
    // the game graphics; tap returns to picker.
    _backButton = [BSPadButton buttonWithType:UIButtonTypeCustom];
    _backButton.gmlKey = -1;
    [_backButton setTitle:@"\u2715" forState:UIControlStateNormal]; // ✕
    [_backButton setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
    _backButton.titleLabel.font = [UIFont boldSystemFontOfSize:16];
    _backButton.backgroundColor = [UIColor colorWithWhite:0.0f alpha:0.35f];
    _backButton.layer.cornerRadius = 13.0f;
    _backButton.layer.borderColor = [[UIColor colorWithWhite:1.0f alpha:0.3f] CGColor];
    _backButton.layer.borderWidth = 1.0f;
    [_backButton addTarget:self action:@selector(backTapped:) forControlEvents:UIControlEventTouchUpInside];
    [_backButton retain];
    [self addSubview:_backButton];

    [self layoutForSize:f.size];
    return self;
}

- (void)dealloc {
    [_buttons release];
    [_backButton release];
    [super dealloc];
}

- (void)layoutSubviews {
    [super layoutSubviews];
    [self layoutForSize:self.bounds.size];
}

// Position buttons relative to current bounds — works the same for
// portrait and landscape; coordinate system always grows down-right.
- (void)layoutForSize:(CGSize)size {
    CGFloat w = size.width;
    CGFloat h = size.height;
    if (w <= 0 || h <= 0) return;

    // ---- D-pad ----
    // The arrows are sized + spaced so neighboring buttons have visible
    // air between them (looks symmetric instead of clumped). The cross
    // is anchored ~60 px from the bottom-left corner of the screen.
    CGFloat ds = 40;       // arrow button edge
    CGFloat dgap = 14;     // gap from the center to each arrow's inner edge
    CGFloat dpadCx = 16 + ds + dgap;        // outer-left of LEFT arrow sits 16 px from screen edge
    CGFloat dpadCy = h - 16 - ds - dgap;    // outer-bottom of DOWN arrow sits 16 px from screen edge

    // _buttons[0..3] = up, down, left, right
    ((UIView*) [_buttons objectAtIndex:0]).frame = CGRectMake(dpadCx - ds/2,         dpadCy - ds - dgap,    ds, ds); // up
    ((UIView*) [_buttons objectAtIndex:1]).frame = CGRectMake(dpadCx - ds/2,         dpadCy + dgap,         ds, ds); // down
    ((UIView*) [_buttons objectAtIndex:2]).frame = CGRectMake(dpadCx - ds - dgap,    dpadCy - ds/2,         ds, ds); // left
    ((UIView*) [_buttons objectAtIndex:3]).frame = CGRectMake(dpadCx + dgap,         dpadCy - ds/2,         ds, ds); // right

    // ---- Action row: Z X C left-to-right (per user request) ----
    // Z = confirm/attack (GMS keycode 90), X = cancel/menu (88),
    // C = item-menu (67). The row is anchored to the bottom-right
    // corner of the screen with C touching the right edge.
    CGFloat as = 46;
    CGFloat agap = 10;
    CGFloat ay = h - 16 - as;        // baseline shared with D-pad's bottom edge
    CGFloat axRight = w - 16 - as;   // outer-right of C sits 16 px from screen edge
    ((UIView*) [_buttons objectAtIndex:4]).frame = CGRectMake(axRight - 2 * (as + agap), ay, as, as); // Z (left)
    ((UIView*) [_buttons objectAtIndex:5]).frame = CGRectMake(axRight - (as + agap),     ay, as, as); // X (middle)
    ((UIView*) [_buttons objectAtIndex:6]).frame = CGRectMake(axRight,                   ay, as, as); // C (right)

    // Shift sits in the top-left of the screen (out of the way of the
    // game's HUD which usually anchors top-center / top-right). It's
    // also smaller because it's a hold-to-run modifier, not a primary
    // action.
    ((UIView*) [_buttons objectAtIndex:7]).frame = CGRectMake(8, 8, 40, 26);

    // Back button: top-right corner, far from the action buttons.
    _backButton.frame = CGRectMake(w - 32, 6, 26, 26);
}

- (void)keyDown:(BSPadButton*)sender {
    if (_delegate != nil) [_delegate touchOverlayKeyDown:sender.gmlKey];
}

- (void)keyUp:(BSPadButton*)sender {
    if (_delegate != nil) [_delegate touchOverlayKeyUp:sender.gmlKey];
}

- (void)backTapped:(BSPadButton*)sender {
    (void) sender;
    if (_delegate != nil) [_delegate touchOverlayBackTapped];
}

@end

// ============================================================================
// GameViewController — wraps the GLView, owns the Butterscotch runtime
// ============================================================================

@interface BSGameViewController : UIViewController <BSGLViewDelegate, BSTouchOverlayDelegate> {
    BSGameEntry* _game;
    BSGLView* _glView;
    BSTouchOverlay* _overlay;
    UILabel* _hudLabel;

    // Butterscotch runtime state. All nil/NULL until -loadRuntime
    // succeeds.
    DataWin* _dataWin;
    VMContext* _vm;
    Renderer* _renderer;
    AudioSystem* _audio;
    FileSystem* _fileSystem;
    Runner* _runner;
    BOOL _runtimeReady;
    BOOL _runtimeFailed;
    NSString* _lastError;
    NSTimeInterval _lastTickTime;
    double _stepAccumulator;   // seconds of unstepped logic time
    int _logicFrameCount;
    UIInterfaceOrientation _savedOrientation;
}
- (id)initWithGame:(BSGameEntry*)game;
@end

@implementation BSGameViewController

- (id)initWithGame:(BSGameEntry*)game {
    self = [super init];
    if (self == nil) return nil;
    _game = [game retain];
    self.title = game->name;
    // wantsFullScreenLayout exists on iOS 3+ — lets our view extend
    // under the status bar (which is hidden anyway via UIStatusBarHidden).
    self.wantsFullScreenLayout = YES;
    return self;
}

- (void)dealloc {
    [_game release];
    [_glView release];
    [_overlay release];
    [_hudLabel release];
    [_lastError release];

    // We intentionally do NOT free the Butterscotch runtime here on
    // teardown: there's no clean Runner_destroy / DataWin_free path
    // currently that we trust on iOS 3 (and the user backing out to
    // the picker is rare on a 128 MB device anyway — they'll just
    // close the app). Letting iOS reclaim the process memory is the
    // safer behaviour for now.
    [super dealloc];
}

- (void)loadView {
    CGRect bounds = [[UIScreen mainScreen] bounds];
    UIView* root = [[UIView alloc] initWithFrame:bounds];
    root.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    root.backgroundColor = [UIColor blackColor];

    _glView = [[BSGLView alloc] initWithFrame:root.bounds];
    _glView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _glView.delegate = self;
    [root addSubview:_glView];

    _hudLabel = [[UILabel alloc] initWithFrame:CGRectMake(8, 8, root.bounds.size.width - 16, 50)];
    _hudLabel.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.55];
    _hudLabel.textColor = [UIColor whiteColor];
    _hudLabel.font = [UIFont systemFontOfSize:10];
    _hudLabel.numberOfLines = 3;
    _hudLabel.lineBreakMode = UILineBreakModeWordWrap;
    _hudLabel.text = [NSString stringWithFormat:@"Loading %@…", _game->name];
    _hudLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    [root addSubview:_hudLabel];

    _overlay = [[BSTouchOverlay alloc] initWithFrame:root.bounds delegate:self];
    [root addSubview:_overlay];

    self.view = root;
    [root release];
}

- (void)hideHud {
    _hudLabel.hidden = YES;
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    // After iOS has finished window placement, recreate the renderbuffer
    // one more time in case the layer's drawable shifted between
    // viewWillAppear and now.
    NSLog(@"[Butterscotch] viewDidAppear pre: self.view.bounds=%.0fx%.0f _glView.frame=%.0fx%.0f _glView.layer.bounds=%.0fx%.0f backing=%dx%d",
          self.view.bounds.size.width, self.view.bounds.size.height,
          _glView.frame.size.width, _glView.frame.size.height,
          _glView.layer.bounds.size.width, _glView.layer.bounds.size.height,
          (int) _glView.backingWidth, (int) _glView.backingHeight);
    [_glView recreateRenderbuffer];
    [_glView startRunLoop];
    if (!_runtimeReady && !_runtimeFailed) {
        [self performSelector:@selector(loadRuntime) withObject:nil afterDelay:0.05];
    }
    // Once the user can see anything, fade the HUD out after a few
    // seconds — it's startup noise, the GL surface should own the screen.
    [self performSelector:@selector(hideHud) withObject:nil afterDelay:5.0];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    // Force landscape and fullscreen. iOS 3.1's modal-VC autorotation
    // is unreliable for VCs presented from a portrait-only parent, so
    // we manually rotate the root view and resize it to the screen's
    // landscape bounds. This is the same trick original third-party
    // landscape iPhone apps used pre-iOS 5.
    UIApplication* app = [UIApplication sharedApplication];
    _savedOrientation = app.statusBarOrientation;
    [app setStatusBarHidden:YES animated:NO];
    if (UIInterfaceOrientationIsPortrait(_savedOrientation)) {
        [app setStatusBarOrientation:UIInterfaceOrientationLandscapeRight animated:NO];
    }
    // Full physical screen size in portrait orientation (e.g. 320x480
    // on iPod Touch 2G). We swap width/height for the landscape bounds
    // and rotate the view -90° (LandscapeRight) so the visible region
    // covers the whole 480x320 surface with no black bars.
    CGRect screen = [[UIScreen mainScreen] bounds];
    CGFloat screenW = screen.size.width;
    CGFloat screenH = screen.size.height;
    NSLog(@"[Butterscotch] viewWillAppear pre: self.view.bounds=%.0fx%.0f _glView.frame=%.0fx%.0f screen=%.0fx%.0f",
          self.view.bounds.size.width, self.view.bounds.size.height,
          _glView.frame.size.width, _glView.frame.size.height,
          screenW, screenH);
    self.view.transform = CGAffineTransformIdentity;
    self.view.bounds = CGRectMake(0, 0, screenH, screenW);
    self.view.center = CGPointMake(screenW / 2.0f, screenH / 2.0f);
    self.view.transform = CGAffineTransformMakeRotation((CGFloat) (M_PI / 2.0));

    // Don't trust autoresize to push the new bounds into _glView's frame:
    // explicitly set it, set the overlay's frame too, and force a fresh
    // renderbuffer allocation against the (now landscape) layer.
    CGRect landscape = CGRectMake(0, 0, screenH, screenW);
    _glView.frame = landscape;
    _overlay.frame = landscape;
    [_overlay layoutForSize:landscape.size];
    NSLog(@"[Butterscotch] viewWillAppear mid: self.view.bounds=%.0fx%.0f _glView.frame=%.0fx%.0f _glView.layer.bounds=%.0fx%.0f",
          self.view.bounds.size.width, self.view.bounds.size.height,
          _glView.frame.size.width, _glView.frame.size.height,
          _glView.layer.bounds.size.width, _glView.layer.bounds.size.height);
    [_glView recreateRenderbuffer];
    NSLog(@"[Butterscotch] viewWillAppear post: _glView.backing=%dx%d",
          (int) _glView.backingWidth, (int) _glView.backingHeight);
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    [_glView stopRunLoop];
    // Restore picker's portrait orientation + status bar when we go back.
    UIApplication* app = [UIApplication sharedApplication];
    [app setStatusBarHidden:NO animated:NO];
    if (_savedOrientation != 0 && _savedOrientation != app.statusBarOrientation) {
        [app setStatusBarOrientation:_savedOrientation animated:NO];
    }
}

- (BOOL)shouldAutorotateToInterfaceOrientation:(UIInterfaceOrientation)o {
    return UIInterfaceOrientationIsLandscape(o);
}

// ------------------------------------------------------------------ overlay
- (void)touchOverlayKeyDown:(int32_t)gmlKey {
    // Log every press so we can confirm in syslog that the overlay is
    // actually firing the right vk_* code into the GML keyboard state.
    // Especially useful while diagnosing why the D-pad seems inert on
    // some screens while Z/X/C clearly work.
    NSLog(@"[Butterscotch] keyDown gmlKey=%d runner=%p kb=%p",
          (int) gmlKey, _runner,
          _runner != NULL ? (void*) _runner->keyboard : NULL);
    if (_runner != NULL && _runner->keyboard != NULL && gmlKey > 0) {
        RunnerKeyboard_onKeyDown(_runner->keyboard, gmlKey);
    }
}

- (void)touchOverlayKeyUp:(int32_t)gmlKey {
    NSLog(@"[Butterscotch] keyUp gmlKey=%d", (int) gmlKey);
    if (_runner != NULL && _runner->keyboard != NULL && gmlKey > 0) {
        RunnerKeyboard_onKeyUp(_runner->keyboard, gmlKey);
    }
}

- (void)touchOverlayBackTapped {
    [self.parentViewController dismissModalViewControllerAnimated:YES];
}

// ------------------------------------------------------------------ runtime

// Progress callback — invoked from inside DataWin_parse on each chunk.
static void BSDataWinProgress(const char* chunkName, int chunkIndex, int totalChunks, DataWin* dw, void* userData) {
    (void) dw;
    (void) userData;
    NSLog(@"[Butterscotch] parse chunk %d/%d: %s", chunkIndex + 1, totalChunks, chunkName);
}

- (void)setStatus:(NSString*)line {
    NSLog(@"[Butterscotch] %@", line);
    _hudLabel.text = [NSString stringWithFormat:@"%@ — %@", _game->name, line];
}

- (void)loadRuntime {
    NSLog(@"[Butterscotch] loadRuntime: %@", _game->dataWinPath);
    [self setStatus:@"Parsing data.win… (this can take a minute)"];

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
    opts.parseShdr = true;
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
    opts.progressCallback = BSDataWinProgress;
    opts.progressCallbackUserData = NULL;

    const char* path = [_game->dataWinPath UTF8String];
    _dataWin = DataWin_parse(path, opts);
    if (_dataWin == NULL) {
        _runtimeFailed = YES;
        [_lastError release];
        _lastError = [[NSString stringWithFormat:@"DataWin_parse returned NULL for %@", _game->dataWinPath] retain];
        [self setStatus:_lastError];
        return;
    }
    NSLog(@"[Butterscotch] data.win parsed: gen8.name=%s bytecodeVer=%u defaultW=%u defaultH=%u",
          _dataWin->gen8.name ? _dataWin->gen8.name : "(null)",
          (unsigned) _dataWin->gen8.bytecodeVersion,
          (unsigned) _dataWin->gen8.defaultWindowWidth,
          (unsigned) _dataWin->gen8.defaultWindowHeight);

    [self setStatus:@"Initialising VM + Runner…"];

    _vm = VM_create(_dataWin);
    if (_vm == NULL) {
        _runtimeFailed = YES;
        [self setStatus:@"VM_create failed"];
        return;
    }

    const char* dataWinDir = [_game->directory UTF8String];
    OverlayFileSystem* overlay = OverlayFileSystem_create(dataWinDir, dataWinDir);
    _fileSystem = (FileSystem*) overlay;

    _audio = (AudioSystem*) NoopAudioSystem_create();

    [_glView makeContextCurrent];
    [_glView bindDrawable];
    _renderer = GLES1Renderer_create();
    GLES1Renderer_setDataWinPath(_renderer, [_game->dataWinPath UTF8String]);

    _runner = Runner_create(_dataWin, _vm, _renderer, _fileSystem, _audio);
    _runner->osType = OS_IOS;

    [self setStatus:@"Initializing first room…"];
    NSLog(@"[Butterscotch] Runner_initFirstRoom");
    Runner_initFirstRoom(_runner);
    NSLog(@"[Butterscotch] first room loaded: %s",
          _runner->currentRoom != NULL ? _runner->currentRoom->name : "(NULL)");

    _runtimeReady = YES;
    _lastTickTime = [NSDate timeIntervalSinceReferenceDate];
    _stepAccumulator = 0.0;
    [self setStatus:[NSString stringWithFormat:@"Ready. Bytecode v%u, %ux%u",
                      (unsigned) _dataWin->gen8.bytecodeVersion,
                      (unsigned) _dataWin->gen8.defaultWindowWidth,
                      (unsigned) _dataWin->gen8.defaultWindowHeight]];
}

// ------------------------------------------------------------------ frame

- (void)glViewTick:(int)frameIndex backingWidth:(GLint)w backingHeight:(GLint)h {
    if (!_runtimeReady) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
    double dt = now - _lastTickTime;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1;   // clamp huge stalls (eg debugger pause)
    _lastTickTime = now;

    // Fixed-timestep accumulator: step the game at currentRoom->speed Hz
    // regardless of how often the CADisplayLink fires.  On iPod Touch 2G
    // the display link runs at 60 Hz but Undertale's room_speed is 30 Hz
    // — calling Runner_step every display tick made the game run at 2x.
    // We accumulate real time and drain the accumulator in fixed slices.
    uint32_t roomSpeed = 30;
    if (_runner != NULL && _runner->currentRoom != NULL && _runner->currentRoom->speed > 0) {
        roomSpeed = _runner->currentRoom->speed;
    }
    double stepDt = 1.0 / (double) roomSpeed;
    _stepAccumulator += dt;
    // Guard against runaway accumulators (e.g. after a long stall): never
    // try to catch up more than 4 steps in one tick.
    if (_stepAccumulator > 4.0 * stepDt) _stepAccumulator = 4.0 * stepDt;

    int stepsThisTick = 0;
    while (_stepAccumulator >= stepDt) {
        Runner_step(_runner);
        if (_audio != NULL) _audio->vtable->update(_audio, (float) stepDt);
        Runner_handlePendingRoomChange(_runner);
        _stepAccumulator -= stepDt;
        _logicFrameCount += 1;
        stepsThisTick++;
        if (stepsThisTick >= 4) break;
    }
    // If no logic step happened this tick (display ran faster than game),
    // skip drawing too — re-presenting the same frame just burns power.
    if (stepsThisTick == 0) {
        return;
    }

    int32_t gameW = (int32_t) _dataWin->gen8.defaultWindowWidth;
    int32_t gameH = (int32_t) _dataWin->gen8.defaultWindowHeight;
    if (gameW <= 0) gameW = w;
    if (gameH <= 0) gameH = h;

    float scaleX = 1.0f, scaleY = 1.0f;
    Runner_computeViewDisplayScale(_runner, gameW, gameH, &scaleX, &scaleY);

    _renderer->vtable->beginFrame(_renderer, gameW, gameH, w, h);

    if (_runner->drawBackgroundColor) {
        uint32_t c = _runner->backgroundColor;
        float rF = ((c >>  0) & 0xFF) / 255.0f;
        float gF = ((c >>  8) & 0xFF) / 255.0f;
        float bF = ((c >> 16) & 0xFF) / 255.0f;
        glClearColor(rF, gF, bF, 1.0f);
    } else {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    }
    glClear(GL_COLOR_BUFFER_BIT);

    Runner_drawViews(_runner, gameW, gameH, scaleX, scaleY, false);

    _renderer->vtable->endFrame(_renderer);

    // Clear the pressed/released edges AFTER BOTH step and draw have
    // consumed them. This is what the PS2 port does, and the comment in
    // its main.c spells out exactly why this matters here too:
    // "MUST be after Runner_draw because games CAN handle input in Draw
    //  events (e.g. Undertale's naming screen)".
    if (_runner != NULL && _runner->keyboard != NULL) {
        RunnerKeyboard_beginFrame(_runner->keyboard);
    }

    if (_logicFrameCount % 60 == 0) {
        NSLog(@"[Butterscotch] frame %d (game %dx%d, fb %dx%d, dt=%.3fms rs=%u steps=%d)",
              _logicFrameCount, gameW, gameH, w, h, dt * 1000.0,
              (unsigned) roomSpeed, stepsThisTick);
    }
}

// Called by UIKit when the process is approaching its jetsam threshold.
// On a 128 MB iPod Touch 2G this is the only warning we get before the
// kernel SIGKILLs us, so we aggressively dump every GL atlas that
// wasn't sampled this frame and let the runtime re-stream them on
// demand.
- (void)didReceiveMemoryWarning {
    NSLog(@"[Butterscotch] didReceiveMemoryWarning — purging non-current atlases");
    if (_renderer != NULL) {
        GLES1Renderer_handleMemoryWarning(_renderer);
    }
    [super didReceiveMemoryWarning];
}

@end

// ============================================================================
// GamePickerViewController — UITableView of folders containing data.win
// ============================================================================

@interface BSGamePickerViewController : UITableViewController {
    NSMutableArray* _games;
    NSString* _activeRoot;
    NSArray* _searchedRoots;
}
@end

@implementation BSGamePickerViewController

- (id)init {
    self = [super initWithStyle:UITableViewStylePlain];
    if (self == nil) return nil;
    self.title = @"Butterscotch";
    _games = [[NSMutableArray alloc] init];
    return self;
}

- (void)dealloc {
    [_games release];
    [_activeRoot release];
    [_searchedRoots release];
    [super dealloc];
}

- (BOOL)shouldAutorotateToInterfaceOrientation:(UIInterfaceOrientation)o {
    return o == UIInterfaceOrientationPortrait;
}

- (void)refresh {
    [_games removeAllObjects];
    [_activeRoot release];
    _activeRoot = nil;
    [_searchedRoots release];
    _searchedRoots = [BSCandidateRootDirectories() retain];

    NSEnumerator* it = [_searchedRoots objectEnumerator];
    NSString* candidate;
    while ((candidate = [it nextObject]) != nil) {
        NSArray* found = BSScanGames(candidate);
        if (found != nil && [found count] > 0) {
            _activeRoot = [candidate retain];
            [_games addObjectsFromArray:found];
            break;
        }
    }
    [self.tableView reloadData];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    UIBarButtonItem* refresh = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
                                                                              target:self
                                                                              action:@selector(refresh)];
    self.navigationItem.rightBarButtonItem = refresh;
    [refresh release];
    [self refresh];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView*)tv {
    return ([_games count] == 0) ? 1 : 2;
}

- (NSInteger)tableView:(UITableView*)tv numberOfRowsInSection:(NSInteger)section {
    if ([_games count] == 0) {
        return [_searchedRoots count];
    }
    if (section == 0) return [_games count];
    return [_searchedRoots count];
}

- (NSString*)tableView:(UITableView*)tv titleForHeaderInSection:(NSInteger)section {
    if ([_games count] == 0) return @"No games found — searched paths";
    if (section == 0) {
        return [NSString stringWithFormat:@"Games in %@", _activeRoot ? _activeRoot : @"(none)"];
    }
    return @"Searched paths";
}

- (NSString*)tableView:(UITableView*)tv titleForFooterInSection:(NSInteger)section {
    if ([_games count] == 0 && section == 0) {
        return @"Drop a folder containing data.win into one of the paths "
               @"above (over SSH / iFunBox / Filza), then tap Refresh.";
    }
    return nil;
}

- (UITableViewCell*)tableView:(UITableView*)tv cellForRowAtIndexPath:(NSIndexPath*)ip {
    static NSString* kReuse = @"BSCell";
    UITableViewCell* cell = [tv dequeueReusableCellWithIdentifier:kReuse];
    if (cell == nil) {
        cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                       reuseIdentifier:kReuse] autorelease];
    }
    if ([_games count] == 0) {
        cell.textLabel.text = [_searchedRoots objectAtIndex:ip.row];
        cell.detailTextLabel.text = @"(not found / unreadable)";
        cell.accessoryType = UITableViewCellAccessoryNone;
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
    } else if (ip.section == 0) {
        BSGameEntry* g = [_games objectAtIndex:ip.row];
        cell.textLabel.text = g->name;
        cell.detailTextLabel.text = g->directory;
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        cell.selectionStyle = UITableViewCellSelectionStyleBlue;
    } else {
        cell.textLabel.text = [_searchedRoots objectAtIndex:ip.row];
        cell.detailTextLabel.text = ([_activeRoot isEqualToString:[_searchedRoots objectAtIndex:ip.row]]) ? @"(active)" : @"";
        cell.accessoryType = UITableViewCellAccessoryNone;
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
    }
    return cell;
}

- (void)tableView:(UITableView*)tv didSelectRowAtIndexPath:(NSIndexPath*)ip {
    [tv deselectRowAtIndexPath:ip animated:YES];
    if ([_games count] == 0 || ip.section != 0) return;
    BSGameEntry* g = [_games objectAtIndex:ip.row];
    BSGameViewController* gvc = [[BSGameViewController alloc] initWithGame:g];
    // Present modally so the navigation bar disappears and the game
    // owns the whole screen. modalTransitionStyle is iOS 3.0+.
    gvc.modalTransitionStyle = UIModalTransitionStyleCrossDissolve;
    [self presentModalViewController:gvc animated:YES];
    [gvc release];
}

@end

// ============================================================================
// AppDelegate
// ============================================================================

@interface BSAppDelegate : NSObject <UIApplicationDelegate> {
    UIWindow* _window;
    UINavigationController* _nav;
}
@end

@implementation BSAppDelegate

- (void)dealloc {
    [_window release];
    [_nav release];
    [super dealloc];
}

- (void)applicationDidFinishLaunching:(UIApplication*)app {
    NSLog(@"[Butterscotch] launch (build %s %s)", __DATE__, __TIME__);
    _window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    BSGamePickerViewController* picker = [[BSGamePickerViewController alloc] init];
    _nav = [[UINavigationController alloc] initWithRootViewController:picker];
    [picker release];
    [_window addSubview:_nav.view];
    [_window makeKeyAndVisible];
}

@end

// ============================================================================
// main
// ============================================================================

int main(int argc, char* argv[]) {
    NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];
    int r = UIApplicationMain(argc, argv, nil, @"BSAppDelegate");
    [pool drain];
    return r;
}
