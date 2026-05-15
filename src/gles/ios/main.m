// Butterscotch iOS shell — targets iPhone OS 3.1.3 SDK (armv6),
// designed to run on a jailbroken iPod Touch 2G (PowerVR MBX Lite,
// 128 MB RAM, OpenGL ES 1.1 only).
//
// This file is intentionally one compilation unit: AppDelegate +
// GamePickerVC + GLView. iOS 3 SDK predates ARC, so we use manual
// retain/release; Theos' default for our Makefile is -fno-objc-arc.
//
// Game discovery:
//   - Scans /var/mobile/Documents/Butterscotch/ for sub-directories
//     containing a data.win file (intended for jailbroken
//     installs via Cydia/.deb).
//   - Falls back to the app's own Documents/ directory inside the
//     sandbox if /var/mobile/Documents/Butterscotch/ is unreadable,
//     so the picker still works without escalated access.
//
// Each sub-directory containing data.win is shown as one row.
// Tapping a row opens an EAGL view that creates a GLES 1.1 context
// and pumps frames via CADisplayLink (iOS 3.1+) at 60 Hz.

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
        NSLog(@"[Butterscotch] GLES1 context: framebuffer %dx%d", (int) _backingWidth, (int) _backingHeight);
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
        // No delegate yet — just present a black frame so the layer is
        // showing something other than the previous frame's stale buffer.
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
// GameViewController — wraps the GLView, owns the Butterscotch runtime
// ============================================================================

@interface BSGameViewController : UIViewController <BSGLViewDelegate> {
    BSGameEntry* _game;
    BSGLView* _glView;
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
    int _logicFrameCount;
}
- (id)initWithGame:(BSGameEntry*)game;
@end

@implementation BSGameViewController

- (id)initWithGame:(BSGameEntry*)game {
    self = [super init];
    if (self == nil) return nil;
    _game = [game retain];
    self.title = game->name;
    return self;
}

- (void)dealloc {
    [_game release];
    [_glView release];
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
    CGRect bounds = [[UIScreen mainScreen] applicationFrame];
    UIView* root = [[UIView alloc] initWithFrame:bounds];
    root.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    root.backgroundColor = [UIColor blackColor];

    _glView = [[BSGLView alloc] initWithFrame:root.bounds];
    _glView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _glView.delegate = self;
    [root addSubview:_glView];

    _hudLabel = [[UILabel alloc] initWithFrame:CGRectMake(8, 8, root.bounds.size.width - 16, 96)];
    _hudLabel.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.55];
    _hudLabel.textColor = [UIColor whiteColor];
    _hudLabel.font = [UIFont systemFontOfSize:11];
    _hudLabel.numberOfLines = 6;
    _hudLabel.lineBreakMode = UILineBreakModeWordWrap;
    _hudLabel.text = [NSString stringWithFormat:@"Butterscotch / GLES1\nLoading %@…", _game->name];
    _hudLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    [root addSubview:_hudLabel];

    self.view = root;
    [root release];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    // Defer loading until after the view is fully on-screen so the
    // navigation push animation and "Loading…" HUD are visible. The
    // run loop is started immediately so the GL view keeps painting
    // (black) while the parser is busy.
    [_glView startRunLoop];
    if (!_runtimeReady && !_runtimeFailed) {
        [self performSelector:@selector(loadRuntime) withObject:nil afterDelay:0.05];
    }
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    [_glView stopRunLoop];
}

- (BOOL)shouldAutorotateToInterfaceOrientation:(UIInterfaceOrientation)o {
    return YES;  // accept all orientations on iOS 3.x
}

// ------------------------------------------------------------------ runtime

// Progress callback — invoked from inside DataWin_parse on each chunk.
// We can't call back into UIKit from arbitrary call sites on iOS 3
// (no GCD), so we just NSLog so the chunk name shows in syslog and
// the user can see how far parsing got if it crashes mid-way.
static void BSDataWinProgress(const char* chunkName, int chunkIndex, int totalChunks, DataWin* dw, void* userData) {
    (void) dw;
    (void) userData;
    NSLog(@"[Butterscotch] parse chunk %d/%d: %s", chunkIndex + 1, totalChunks, chunkName);
}

- (void)setStatus:(NSString*)line {
    NSLog(@"[Butterscotch] %@", line);
    _hudLabel.text = [NSString stringWithFormat:@"Butterscotch / GLES1\n%@\n%@",
                       _game->name, line];
}

- (void)loadRuntime {
    NSLog(@"[Butterscotch] loadRuntime: %@", _game->dataWinPath);
    [self setStatus:@"Parsing data.win… (this can take a minute)"];

    // Stage 1: parse data.win.
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
    opts.lazyLoadRooms = true;       // critical for 128 MB MBX Lite RAM budget
    // We parse the TXTR / AUDO metadata so we know each blob's
    // offset+size inside data.win, but we deliberately do NOT load the
    // PNG / audio bytes into RAM. The renderer (when it grows real
    // texture support) and audio system can stream them on demand
    // from data.win via dw->lazyLoadFile + texture.blobOffset.
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

    // Stage 2: build VM, file system, audio, renderer, runner.
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

    // Make sure the EAGL context is current before the renderer init
    // call — it issues GL state setup.
    [_glView makeContextCurrent];
    [_glView bindDrawable];
    _renderer = GLES1Renderer_create();
    // Renderer needs to fopen its own FILE handle into data.win so it
    // can stream TXTR PNG blobs without racing the parser's room
    // lazy-loader on a shared file position.
    GLES1Renderer_setDataWinPath(_renderer, [_game->dataWinPath UTF8String]);

    _runner = Runner_create(_dataWin, _vm, _renderer, _fileSystem, _audio);
    _runner->osType = OS_IOS;

    // CRITICAL: Runner_create does NOT load the first room. Without this,
    // Runner_step crashes in dispatchOutsideRoomEvents because
    // runner->currentRoom is NULL. Mirrors what the PS2 port does at
    // src/ps2/main.c:521.
    [self setStatus:@"Initializing first room…"];
    NSLog(@"[Butterscotch] Runner_initFirstRoom");
    Runner_initFirstRoom(_runner);
    NSLog(@"[Butterscotch] first room loaded: %s",
          _runner->currentRoom != NULL ? _runner->currentRoom->name : "(NULL)");

    _runtimeReady = YES;
    _lastTickTime = [NSDate timeIntervalSinceReferenceDate];
    [self setStatus:[NSString stringWithFormat:@"Ready. Bytecode v%u, %ux%u",
                      (unsigned) _dataWin->gen8.bytecodeVersion,
                      (unsigned) _dataWin->gen8.defaultWindowWidth,
                      (unsigned) _dataWin->gen8.defaultWindowHeight]];
}

// ------------------------------------------------------------------ frame

- (void)glViewTick:(int)frameIndex backingWidth:(GLint)w backingHeight:(GLint)h {
    if (!_runtimeReady) {
        // While loading: just clear black so the framebuffer is in a
        // defined state and the layer keeps presenting fresh content.
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
    float dt = (float) (now - _lastTickTime);
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;
    _lastTickTime = now;

    // Run one game step (Begin Step, Keyboard, Alarms, Step, End Step).
    Runner_step(_runner);
    if (_audio != NULL) _audio->vtable->update(_audio, dt);
    _logicFrameCount += 1;

    // Draw.
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

    Runner_handlePendingRoomChange(_runner);

    if (_logicFrameCount % 60 == 0) {
        NSLog(@"[Butterscotch] frame %d (game %dx%d, fb %dx%d, dt=%.3fms)",
              _logicFrameCount, gameW, gameH, w, h, dt * 1000.0f);
    }
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
    [self.navigationController pushViewController:gvc animated:YES];
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
