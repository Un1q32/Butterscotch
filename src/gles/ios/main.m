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

// Forward decl from the renderer skeleton. The renderer is allocated
// when the user enters a game; for now we don't actually run the VM
// from here (that wiring is the next milestone).
struct Renderer;
extern struct Renderer* GLES1Renderer_create(void);

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

@interface BSGLView : UIView {
    EAGLContext* _ctx;
    GLuint _framebuffer;
    GLuint _renderbuffer;
    GLint _backingWidth;
    GLint _backingHeight;
    CADisplayLink* _displayLink;
    NSTimer* _fallbackTimer;
    int _frameCounter;
    float _hue;  // cycling clear color so it is obvious the loop runs
}
- (void)startRunLoop;
- (void)stopRunLoop;
@end

@implementation BSGLView

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
    return self;
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

// HSV→RGB for the demo clear color cycle.
static void hsv2rgb(float h, float s, float v, float* r, float* g, float* b) {
    int i = (int) (h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - f * s);
    float t = v * (1.0f - (1.0f - f) * s);
    switch (i % 6) {
        case 0: *r = v; *g = t; *b = p; break;
        case 1: *r = q; *g = v; *b = p; break;
        case 2: *r = p; *g = v; *b = t; break;
        case 3: *r = p; *g = q; *b = v; break;
        case 4: *r = t; *g = p; *b = v; break;
        case 5: *r = v; *g = p; *b = q; break;
    }
}

- (void)tick {
    [EAGLContext setCurrentContext:_ctx];
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, _framebuffer);
    glViewport(0, 0, _backingWidth, _backingHeight);

    _hue += 1.0f / 240.0f;
    if (_hue >= 1.0f) _hue -= 1.0f;
    float r, g, b;
    hsv2rgb(_hue, 0.4f, 0.9f, &r, &g, &b);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindRenderbufferOES(GL_RENDERBUFFER_OES, _renderbuffer);
    [_ctx presentRenderbuffer:GL_RENDERBUFFER_OES];

    _frameCounter += 1;
    if (_frameCounter % 60 == 0) {
        NSLog(@"[Butterscotch] tick %d (%dx%d)", _frameCounter, (int) _backingWidth, (int) _backingHeight);
    }
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
// GameViewController — wraps the GLView, holds the picked game entry
// ============================================================================

@interface BSGameViewController : UIViewController {
    BSGameEntry* _game;
    BSGLView* _glView;
    UILabel* _hudLabel;
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
    [super dealloc];
}

- (void)loadView {
    CGRect bounds = [[UIScreen mainScreen] applicationFrame];
    UIView* root = [[UIView alloc] initWithFrame:bounds];
    root.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    root.backgroundColor = [UIColor blackColor];

    _glView = [[BSGLView alloc] initWithFrame:root.bounds];
    _glView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [root addSubview:_glView];

    _hudLabel = [[UILabel alloc] initWithFrame:CGRectMake(8, 8, root.bounds.size.width - 16, 60)];
    _hudLabel.backgroundColor = [UIColor colorWithWhite:0.0 alpha:0.5];
    _hudLabel.textColor = [UIColor whiteColor];
    _hudLabel.font = [UIFont systemFontOfSize:12];
    _hudLabel.numberOfLines = 3;
    _hudLabel.text = [NSString stringWithFormat:@"Butterscotch (GLES1)\nGame: %@\ndata.win: %@",
                       _game->name, _game->dataWinPath];
    _hudLabel.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    [root addSubview:_hudLabel];

    self.view = root;
    [root release];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    NSLog(@"[Butterscotch] entering game: %@ (data.win = %@)", _game->name, _game->dataWinPath);

    // TODO(milestone 2): wire DataWin/VM/Runner here. For now we just
    // confirm the GL context + run loop are alive on real hardware.
    // (void) GLES1Renderer_create();
    [_glView startRunLoop];
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    [_glView stopRunLoop];
}

- (BOOL)shouldAutorotateToInterfaceOrientation:(UIInterfaceOrientation)o {
    return YES;  // accept all orientations on iOS 3.x
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
