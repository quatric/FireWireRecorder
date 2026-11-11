/*
 * FireWireRecorder for old Macs: Mac OS X 10.3 on PowerPC and 10.4 on Intel,
 * up to 10.8 (10.9 and later run the regular app, viewer.m).
 *
 * The same engine (app_core.c receives the DV stream over FireWire, drives the
 * tape, records, and decodes with ffmpeg for the preview and the sound) under
 * a Cocoa window written for the oldest AppKit: manual retain/release, no
 * blocks, no Auto Layout, no layers. The picture goes to an OpenGL view, the
 * sound to Core Audio's default output (audio_mac_legacy.c).
 *
 * Raw DV recording only copies the frames to disk, which any of these Macs
 * can do; a G4 is short of time for MPEG-2 encoding at full speed (the
 * recording then falls behind and drops), and the PowerPC ffmpeg has no x264.
 * Turning the preview off leaves the encoder more of the CPU.
 */
#import <Cocoa/Cocoa.h>
#import <OpenGL/gl.h>
#import <OpenGL/glext.h>
#include <string.h>

#include "app_core.h"

#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

static app_t *g_app;

static int pot(int v)
{
    int p = 64;
    while (p < v)
        p *= 2;
    return p;
}

/* ---- preview ---- */

@interface PreviewView : NSOpenGLView {
    int _w, _h, _tw, _th; /* frame and texture size */
    uint32_t *_buf;
    uint64_t _seq;
    GLuint _tex;
    BOOL _glReady, _haveFrame;
    double _aspect;
}
- (void)setAspect:(double)aspect;
- (void)poll;
- (void)clear;
- (void)drawPicture;
@end

@implementation PreviewView
- (id)initWithFrame:(NSRect)frame
{
    NSOpenGLPixelFormatAttribute attrs[] = { NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize, 24, 0 };
    NSOpenGLPixelFormat *pf = [[[NSOpenGLPixelFormat alloc] initWithAttributes:attrs] autorelease];
    if ((self = [super initWithFrame:frame pixelFormat:pf]))
        _aspect = 16.0 / 9;
    return self;
}

- (void)dealloc
{
    free(_buf);
    [super dealloc];
}

- (BOOL)isOpaque { return YES; }

- (void)setAspect:(double)aspect
{
    if (aspect == _aspect)
        return;
    _aspect = aspect;
    [self setNeedsDisplay:YES];
}

- (void)clear
{
    if (!_haveFrame)
        return;
    _haveFrame = NO;
    [self setNeedsDisplay:YES];
}

- (void)setupGL
{
    [[self openGLContext] makeCurrentContext];
    GLint one = 1; /* swap on the screen's refresh, so frames are not torn */
    [[self openGLContext] setValues:&one forParameter:NSOpenGLCPSwapInterval];
    glGenTextures(1, &_tex);
    _glReady = YES;
}

- (void)poll
{
    if (!g_app || ![self window] || ![[self window] isVisible])
        return;
    if (!_glReady)
        [self setupGL];
    int w, h;
    app_frame_size(g_app, &w, &h);
    if (w != _w || h != _h || !_buf) {
        free(_buf);
        _buf = calloc((size_t)w * h, 4);
        _w = w;
        _h = h;
        _seq = 0;
    }
    if (!_buf || !app_get_frame(g_app, _buf, w, h, &_seq))
        return;
    [[self openGLContext] makeCurrentContext];
    glBindTexture(GL_TEXTURE_2D, _tex);
    if (pot(w) != _tw || pot(h) != _th) {
        _tw = pot(w);
        _th = pot(h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, _tw, _th, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    }
    /* the frames are 32-bit 0x00RRGGBB in the CPU's byte order, which is what this type reads on both CPUs */
    glPixelStorei(GL_UNPACK_ROW_LENGTH, w);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, _buf);
    _haveFrame = YES;
    [self drawPicture];
}

- (void)drawPicture
{
    NSRect b = [self bounds];
    float W = (float)b.size.width, H = (float)b.size.height;
    glViewport(0, 0, (GLsizei)W, (GLsizei)H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (_haveFrame && _tw && W > 0 && H > 0) {
        float ar = (float)_aspect, w = W, h = W / ar;
        if (h > H) {
            h = H;
            w = H * ar;
        }
        float x0 = -w / W, x1 = w / W, y0 = -h / H, y1 = h / H;
        float u = (float)_w / _tw, v = (float)_h / _th;
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, _tex);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        glBegin(GL_QUADS);
        glTexCoord2f(0, v); glVertex2f(x0, y0);
        glTexCoord2f(u, v); glVertex2f(x1, y0);
        glTexCoord2f(u, 0); glVertex2f(x1, y1);
        glTexCoord2f(0, 0); glVertex2f(x0, y1);
        glEnd();
        glDisable(GL_TEXTURE_2D);
    }
    [[self openGLContext] flushBuffer];
}

- (void)drawRect:(NSRect)r
{
    (void)r;
    if (!_glReady)
        [self setupGL];
    [[self openGLContext] makeCurrentContext];
    [self drawPicture];
}
@end

/* ---- level meters (NSLevelIndicator needs 10.4) ---- */

@interface MeterView : NSView {
    float _v[2];
}
- (void)setLeft:(float)l right:(float)r;
@end

@implementation MeterView
- (void)setLeft:(float)l right:(float)r
{
    if (l == _v[0] && r == _v[1])
        return;
    _v[0] = l;
    _v[1] = r;
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)rect
{
    (void)rect;
    NSRect b = [self bounds];
    float bar = (float)floor((b.size.height - 3) / 2);
    int c;
    for (c = 0; c < 2; c++) {
        NSRect r = NSMakeRect(0, c ? 0 : bar + 3, b.size.width, bar);
        [[NSColor colorWithCalibratedWhite:0.2f alpha:1] set];
        NSRectFill(r);
        float v = _v[c];
        NSRect f = r;
        f.size.width = (float)floor(r.size.width * (v < 0.8f ? v : 0.8f));
        [[NSColor colorWithCalibratedRed:0.2f green:0.8f blue:0.2f alpha:1] set];
        NSRectFill(f);
        if (v > 0.8f) {
            f.origin.x = r.size.width * 0.8f;
            f.size.width = (float)floor(r.size.width * ((v < 0.95f ? v : 0.95f) - 0.8f));
            [[NSColor colorWithCalibratedRed:0.95f green:0.8f blue:0.1f alpha:1] set];
            NSRectFill(f);
        }
        if (v > 0.95f) {
            f.origin.x = r.size.width * 0.95f;
            f.size.width = (float)floor(r.size.width * (v - 0.95f));
            [[NSColor colorWithCalibratedRed:0.9f green:0.15f blue:0.1f alpha:1] set];
            NSRectFill(f);
        }
    }
}
@end

/* ---- the window ---- */

#ifdef __ppc__
#define FORMATS 2 /* no x264 in the PowerPC ffmpeg */
#else
#define FORMATS APP_FORMATS
#endif

enum { TAPE_REW, TAPE_PLAY, TAPE_PAUSE, TAPE_STOP, TAPE_FF, TAPE_BUTTONS };
static const fwdv_cmd_t tape_cmds[TAPE_BUTTONS] = { FWDV_REWIND, FWDV_PLAY, FWDV_PAUSE, FWDV_STOP, FWDV_FFWD };

@interface Controller : NSObject {
    NSWindow *_window;
    PreviewView *_preview;
    NSView *_bar;
    NSButton *_tape[TAPE_BUTTONS];
    NSTextField *_timecode, *_tapeState;
    NSPopUpButton *_format;
    NSComboBox *_stopAfter;
    NSButton *_autoStop, *_previewCheck, *_monitor, *_record, *_folderButton;
    NSTextField *_time, *_status;
    MeterView *_meters;
    NSTimer *_timer;
    NSString *_folder, *_saved;
    NSDate *_savedUntil;
    BOOL _finishing;
    int _ticks;
    double _level[2];
}
- (NSTextField *)label:(NSString *)s;
- (void)layoutBar;
@end

static NSTextField *make_label(NSString *s)
{
    NSTextField *l = [[[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 10, 17)] autorelease];
    [l setStringValue:s];
    [l setBezeled:NO];
    [l setDrawsBackground:NO];
    [l setEditable:NO];
    [l setSelectable:NO];
    [l setFont:[NSFont systemFontOfSize:[NSFont systemFontSize]]];
    [l sizeToFit];
    return l;
}

static NSButton *make_button(NSString *title, id target, SEL action, BOOL checkbox)
{
    NSButton *b = [[[NSButton alloc] initWithFrame:NSMakeRect(0, 0, 10, 24)] autorelease];
    [b setButtonType:checkbox ? NSSwitchButton : NSMomentaryPushInButton];
    if (!checkbox)
        [b setBezelStyle:NSRoundedBezelStyle];
    [b setFont:[NSFont systemFontOfSize:[NSFont systemFontSize]]];
    [b setTitle:title];
    [b setTarget:target];
    [b setAction:action];
    [b sizeToFit];
    return b;
}

static NSPopUpButton *make_popup(const char *const *names, int n, id target, SEL action)
{
    NSPopUpButton *p = [[[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 10, 26) pullsDown:NO] autorelease];
    int i;
    [p setFont:[NSFont systemFontOfSize:[NSFont systemFontSize]]];
    for (i = 0; i < n; i++)
        [p addItemWithTitle:[NSString stringWithUTF8String:names[i]]];
    [p setTarget:target];
    [p setAction:action];
    [p sizeToFit];
    return p;
}

/* sizes a control to its content, with a few points spare (text measured exactly can lose its last letter) */
static void fit(NSView *v, float minw, float maxw)
{
    if ([v respondsToSelector:@selector(sizeToFit)])
        [(NSControl *)v sizeToFit];
    NSRect f = [v frame];
    f.size.width = (float)ceil(f.size.width) + 4;
    if (f.size.width < minw)
        f.size.width = minw;
    if (maxw > 0 && f.size.width > maxw)
        f.size.width = maxw;
    [v setFrame:f];
}

/* places views left to right, vertically centred on a row, from x; returns where it ended */
static float place_row(NSArray *views, float x, float y, float h, float gap)
{
    unsigned i;
    for (i = 0; i < [views count]; i++) {
        NSView *v = [views objectAtIndex:i];
        if ([v isKindOfClass:[NSNumber class]]) {
            x += [(NSNumber *)v floatValue];
            continue;
        }
        NSRect f = [v frame];
        f.origin.x = x;
        f.origin.y = (float)floor(y + (h - f.size.height) / 2);
        [v setFrame:f];
        x += f.size.width + gap;
    }
    return x;
}

static void rec_done(void *user, int ok, const char *path);

@implementation Controller

- (NSUserDefaults *)ud { return [NSUserDefaults standardUserDefaults]; }

- (NSTextField *)label:(NSString *)s
{
    NSArray *subs = [_bar subviews];
    unsigned i;
    for (i = 0; i < [subs count]; i++) {
        id v = [subs objectAtIndex:i];
        if ([v isKindOfClass:[NSTextField class]] && v != _time && v != _status && v != _timecode &&
            v != _tapeState && [[v stringValue] isEqualToString:s])
            return v;
    }
    NSTextField *l = make_label(s);
    [_bar addSubview:l];
    return l;
}

- (void)layoutBar
{
    NSRect b = [_bar bounds];
    NSArray *row1 = [NSArray arrayWithObjects:_tape[TAPE_REW], _tape[TAPE_PLAY], _tape[TAPE_PAUSE], _tape[TAPE_STOP],
                                              _tape[TAPE_FF], [NSNumber numberWithFloat:10], _timecode, nil];
    NSArray *row2 = [NSArray arrayWithObjects:[self label:@"Format"], _format, [NSNumber numberWithFloat:8], _autoStop,
                                              [NSNumber numberWithFloat:8], _previewCheck, [NSNumber numberWithFloat:8],
                                              [self label:@"Save to"], _folderButton, nil];
    NSArray *row3 = [NSArray arrayWithObjects:_record, _time, _meters, _monitor, [self label:@"Stop after"], _stopAfter, nil];
    unsigned i;
    for (i = 0; i < [row2 count]; i++) {
        id v = [row2 objectAtIndex:i];
        if (v != _folderButton && ![v isKindOfClass:[NSNumber class]])
            fit(v, 0, 0);
    }
    for (i = 0; i < TAPE_BUTTONS; i++)
        fit(_tape[i], 56, 0);
    fit(_timecode, 0, 0);
    fit(_folderButton, 60, 200);
    fit(_record, 100, 0);
    fit(_time, 0, 0);
    fit(_monitor, 0, 0);
    fit([self label:@"Stop after"], 0, 0);
    {
        NSRect sf2 = [_stopAfter frame];
        sf2.size.width = 120;
        [_stopAfter setFrame:sf2];
    }
    float x = place_row(row1, 12, 78, 26, 4);
    NSRect tf = [_tapeState frame];
    tf.origin.x = x + 6;
    tf.origin.y = 83;
    tf.size.width = b.size.width - x - 18;
    [_tapeState setFrame:tf];
    place_row(row2, 12, 44, 26, 6);
    x = place_row(row3, 12, 8, 28, 12);
    NSRect sf = [_status frame];
    sf.origin.x = x;
    sf.origin.y = 13;
    sf.size.width = b.size.width - x - 12;
    [_status setFrame:sf];
}

- (void)buildMenu
{
    NSMenu *bar = [[[NSMenu alloc] initWithTitle:@""] autorelease];
    NSMenuItem *appItem = [[[NSMenuItem alloc] initWithTitle:@"" action:NULL keyEquivalent:@""] autorelease];
    NSMenu *appMenu = [[[NSMenu alloc] initWithTitle:@APP_NAME] autorelease];
    [appMenu addItemWithTitle:@"Hide " APP_NAME action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit " APP_NAME action:@selector(terminate:) keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
    [bar addItem:appItem];

    NSMenuItem *fileItem = [[[NSMenuItem alloc] initWithTitle:@"File" action:NULL keyEquivalent:@""] autorelease];
    NSMenu *file = [[[NSMenu alloc] initWithTitle:@"File"] autorelease];
    [[file addItemWithTitle:@"Start/Stop Recording" action:@selector(toggleRecord:) keyEquivalent:@"r"] setTarget:self];
    [[file addItemWithTitle:@"Save Snapshot" action:@selector(snapshot:) keyEquivalent:@"s"] setTarget:self];
    [file addItem:[NSMenuItem separatorItem]];
    [[file addItemWithTitle:@"Choose Recording Folder..." action:@selector(chooseFolder:) keyEquivalent:@""]
        setTarget:self];
    [[file addItemWithTitle:@"Show Recordings in Finder" action:@selector(revealFolder:) keyEquivalent:@""]
        setTarget:self];
    [file addItem:[NSMenuItem separatorItem]];
    [file addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    [fileItem setSubmenu:file];
    [bar addItem:fileItem];

    NSMenuItem *tapeItem = [[[NSMenuItem alloc] initWithTitle:@"Tape" action:NULL keyEquivalent:@""] autorelease];
    NSMenu *tape = [[[NSMenu alloc] initWithTitle:@"Tape"] autorelease];
    static NSString *const titles[TAPE_BUTTONS] = { @"Rewind", @"Play", @"Pause", @"Stop", @"Fast Forward" };
    static NSString *const keys[TAPE_BUTTONS] = { @"[", @"\r", @"p", @".", @"]" };
    int i;
    for (i = 0; i < TAPE_BUTTONS; i++) {
        NSMenuItem *it = [tape addItemWithTitle:titles[i] action:@selector(tapeButton:) keyEquivalent:keys[i]];
        [it setTag:i];
        [it setTarget:self];
    }
    [tapeItem setSubmenu:tape];
    [bar addItem:tapeItem];

    NSMenuItem *viewItem = [[[NSMenuItem alloc] initWithTitle:@"View" action:NULL keyEquivalent:@""] autorelease];
    NSMenu *view = [[[NSMenu alloc] initWithTitle:@"View"] autorelease];
    [[view addItemWithTitle:@"Show Preview" action:@selector(togglePreview:) keyEquivalent:@"e"] setTarget:self];
    [viewItem setSubmenu:view];
    [bar addItem:viewItem];

    [NSApp setMainMenu:bar];
    if ([NSApp respondsToSelector:@selector(setAppleMenu:)]) /* 10.3 needs it to treat the first menu as the app menu */
        [NSApp performSelector:@selector(setAppleMenu:) withObject:appMenu];
}

- (void)updateFolderButton
{
    [_folderButton setTitle:[[NSFileManager defaultManager] displayNameAtPath:_folder]];
    [_folderButton setToolTip:_folder];
    [self layoutBar];
}

- (void)setStatus:(NSString *)s
{
    if (![[_status stringValue] isEqualToString:s ? s : @""])
        [_status setStringValue:s ? s : @""];
}

- (void)updateEnabled
{
    BOOL rec = app_is_recording(g_app) || _finishing;
    [_format setEnabled:!rec];
    [_folderButton setEnabled:!rec];
}

- (void)buildWindow
{
    NSUserDefaults *ud = [self ud];
    int barH = 112;
    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 800, [[NSScreen mainScreen] visibleFrame].size.height - 60 < 600 + barH ? [[NSScreen mainScreen] visibleFrame].size.height - 60 : 600 + barH)
                                          styleMask:NSTitledWindowMask | NSClosableWindowMask |
                                                    NSMiniaturizableWindowMask | NSResizableWindowMask
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    [_window setDelegate:self];
    [_window setTitle:@APP_NAME];
    [_window setMinSize:NSMakeSize(720, 300 + barH)];
    NSView *content = [_window contentView];

    _preview = [[[PreviewView alloc] initWithFrame:NSMakeRect(0, barH, 800, 600)] autorelease];
    [_preview setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [_preview setAspect:4.0 / 3];
    [content addSubview:_preview];
    _bar = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 800, barH)] autorelease];
    [_bar setAutoresizingMask:NSViewWidthSizable | NSViewMaxYMargin];
    [content addSubview:_bar];

    static NSString *const titles[TAPE_BUTTONS] = { @"Rew", @"Play", @"Pause", @"Stop", @"FF" };
    static NSString *const tips[TAPE_BUTTONS] = { @"Rewind", @"Play", @"Pause", @"Stop", @"Fast forward" };
    int i;
    for (i = 0; i < TAPE_BUTTONS; i++) {
        _tape[i] = make_button(titles[i], self, @selector(tapeButton:), NO);
        [_tape[i] setTag:i];
        [_tape[i] setToolTip:tips[i]];
        [_bar addSubview:_tape[i]];
    }
    _timecode = make_label(@"--:--:--.--");
    [_timecode setFont:[NSFont userFixedPitchFontOfSize:14]];
    _tapeState = make_label(@"");
    [_tapeState setTextColor:[NSColor disabledControlTextColor]];
    [_tapeState setAutoresizingMask:NSViewWidthSizable];

    _stopAfter = [[[NSComboBox alloc] initWithFrame:NSMakeRect(0, 0, 120, 22)] autorelease];
    [_stopAfter setFont:[NSFont systemFontOfSize:[NSFont systemFontSize]]];
    {
        int k, saved;
        char lb[48];
        for (k = 0; k < APP_STOP_AFTERS; k++)
            [_stopAfter addItemWithObjectValue:[NSString stringWithUTF8String:app_stop_after_names[k]]];
        [_stopAfter setTarget:self];
        [_stopAfter setAction:@selector(stopAfterChanged:)];
        [_stopAfter setDelegate:self]; /* typing a time and leaving the box counts too */
        saved = [ud objectForKey:@"stopAfterMinutes"] ? (int)[ud integerForKey:@"stopAfterMinutes"]
                                                      : app_stop_after_from_index((int)[ud integerForKey:@"stopAfter"]);
        if (saved < 0 || saved > APP_STOP_AFTER_MAX)
            saved = 0;
        app_stop_after_label(saved, lb, sizeof(lb));
        [_stopAfter setStringValue:[NSString stringWithUTF8String:lb]];
        app_set_stop_after_minutes(g_app, saved);
    }
    [_stopAfter setToolTip:@"Stop the recording by itself after this long.\nPick a time, or type your own: 45, 2h, 1:30..."];
    _format = make_popup(app_format_names, FORMATS, self, @selector(formatChanged:));
    [_format selectItemAtIndex:[ud integerForKey:@"format"] < FORMATS ? [ud integerForKey:@"format"] : 0];
    [_format setToolTip:@"Raw DV keeps the tape exactly (about 13 GB an hour). MPEG-2 makes DVD-ready .mpg files."];
    _autoStop = make_button(@"Stop at end of tape", self, @selector(autoStopChanged:), YES);
    [_autoStop setState:[ud boolForKey:@"autoStop"] ? NSOnState : NSOffState];
    _previewCheck = make_button(@"Preview", self, @selector(previewChanged:), YES);
    [_previewCheck setState:[ud boolForKey:@"preview"] ? NSOnState : NSOffState];
    [_previewCheck setToolTip:@"Show the picture. Off saves the computer the work of decoding it."];
    _folderButton = make_button(@"Movies", self, @selector(chooseFolder:), NO);

    _record = make_button(@"Record", self, @selector(toggleRecord:), NO);
    [_record setKeyEquivalent:@"r"];
    [_record setKeyEquivalentModifierMask:NSCommandKeyMask];
    _time = make_label(@"00:00:00");
    [_time setFont:[NSFont userFixedPitchFontOfSize:14]];
    _meters = [[[MeterView alloc] initWithFrame:NSMakeRect(0, 0, 90, 15)] autorelease];
    _monitor = make_button(@"Play audio", self, @selector(monitorChanged:), YES);
    [_monitor setState:[ud boolForKey:@"monitor"] ? NSOnState : NSOffState];
    _status = make_label(@"");
    [_status setTextColor:[NSColor disabledControlTextColor]];
    [[_status cell] setLineBreakMode:NSLineBreakByTruncatingMiddle];
    [_status setAutoresizingMask:NSViewWidthSizable];

    NSArray *all = [NSArray arrayWithObjects:_timecode, _tapeState, _format, _autoStop, _previewCheck, _folderButton,
                                             _record, _time, _meters, _monitor, _stopAfter, _status, nil];
    unsigned k;
    for (k = 0; k < [all count]; k++)
        [_bar addSubview:[all objectAtIndex:k]];
    [self updateFolderButton];
    [self updateEnabled];
    [_window setFrameAutosaveName:@"FireWireRecorderWindow"];
    [_window center];
    [_window makeKeyAndOrderFront:nil];
}

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;
    NSUserDefaults *ud = [self ud];
    [ud registerDefaults:[NSDictionary dictionaryWithObjectsAndKeys:
        [NSNumber numberWithInt:0], @"format", [NSNumber numberWithBool:YES], @"autoStop",
        [NSNumber numberWithBool:YES], @"preview", [NSNumber numberWithBool:YES], @"monitor", nil]];

    /* Cocoa must know threads exist before the engine's threads call back into it */
    [NSThread detachNewThreadSelector:@selector(noop:) toTarget:self withObject:nil];

    char folder[1024];
    app_default_folder(folder, sizeof(folder));
    NSString *saved = [ud stringForKey:@"folder"];
    _folder = [(saved ? saved : [NSString stringWithUTF8String:folder]) retain];
    NSArray *args = [[NSProcessInfo processInfo] arguments];
    if ([args containsObject:@"-v"]) fwdv_set_verbose(1);
    if ([args containsObject:@"-vv"]) fwdv_set_verbose(2);

    [self buildMenu];
    g_app = app_start();
    if (!g_app) {
        NSRunAlertPanel(@APP_NAME " cannot start", @"Out of memory or threads.", @"Quit", nil, nil);
        [NSApp terminate:nil];
        return;
    }
    app_set_monitor(g_app, [ud boolForKey:@"monitor"]);
    app_set_preview(g_app, [ud boolForKey:@"preview"]);
    app_set_auto_stop(g_app, [ud boolForKey:@"autoStop"]);
    [self buildWindow];
    [NSApp activateIgnoringOtherApps:YES];

    _timer = [[NSTimer scheduledTimerWithTimeInterval:1.0 / 60 target:self selector:@selector(tick:) userInfo:nil
                                              repeats:YES] retain];
    [[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSModalPanelRunLoopMode];
    [[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSEventTrackingRunLoopMode];
}

- (void)noop:(id)arg
{
    (void)arg;
}

- (void)windowDidResize:(NSNotification *)note
{
    (void)note;
    [self layoutBar];
}

/* ---- controls ---- */

/* AV/C commands take a moment on the bus: sent from a thread of their own */
- (void)sendTape:(NSNumber *)cmd
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    app_tape(g_app, (fwdv_cmd_t)[cmd intValue]);
    [pool release];
}

- (void)tapeButton:(id)sender
{
    int i = (int)[sender tag];
    if (i < 0 || i >= TAPE_BUTTONS || !g_app)
        return;
    fwdv_cmd_t cmd = tape_cmds[i];
    fwdv_status_t st;
    if (cmd == FWDV_PLAY && app_tape_status(g_app, &st) == 0 && st.mode == FWDV_MODE_PLAYING)
        cmd = FWDV_PAUSE; /* Play while playing pauses, as on the camcorder */
    [NSThread detachNewThreadSelector:@selector(sendTape:) toTarget:self withObject:[NSNumber numberWithInt:cmd]];
}

- (void)formatChanged:(id)sender
{
    (void)sender;
    [[self ud] setInteger:[_format indexOfSelectedItem] forKey:@"format"];
}

- (void)autoStopChanged:(id)sender
{
    (void)sender;
    BOOL on = [_autoStop state] == NSOnState;
    app_set_auto_stop(g_app, on);
    [[self ud] setBool:on forKey:@"autoStop"];
}

- (void)stopAfterChanged:(id)sender
{
    int mins = app_parse_stop_after([[_stopAfter stringValue] UTF8String]);
    char lb[48];
    (void)sender;
    if (mins < 0)
        mins = (int)[[self ud] integerForKey:@"stopAfterMinutes"]; /* not a time: keep the last one */
    app_stop_after_label(mins, lb, sizeof(lb));
    [_stopAfter setStringValue:[NSString stringWithUTF8String:lb]];
    if (g_app) /* the box can end editing while the app quits */
        app_set_stop_after_minutes(g_app, mins);
    [[self ud] setInteger:mins forKey:@"stopAfterMinutes"];
}

- (void)controlTextDidEndEditing:(NSNotification *)n
{
    if ([n object] == _stopAfter)
        [self stopAfterChanged:_stopAfter];
}

- (void)previewChanged:(id)sender
{
    (void)sender;
    BOOL on = [_previewCheck state] == NSOnState;
    app_set_preview(g_app, on);
    [[self ud] setBool:on forKey:@"preview"];
    if (!on)
        [_preview clear];
}

- (void)togglePreview:(id)sender
{
    [_previewCheck setState:[_previewCheck state] == NSOnState ? NSOffState : NSOnState];
    [self previewChanged:sender];
}

- (void)monitorChanged:(id)sender
{
    (void)sender;
    BOOL on = [_monitor state] == NSOnState;
    app_set_monitor(g_app, on);
    [[self ud] setBool:on forKey:@"monitor"];
}

- (void)chooseFolder:(id)sender
{
    (void)sender;
    if (app_is_recording(g_app))
        return;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    [p setCanChooseDirectories:YES];
    [p setCanChooseFiles:NO];
    [p setPrompt:@"Choose"];
    if ([p respondsToSelector:@selector(setCanCreateDirectories:)])
        [p setCanCreateDirectories:YES];
    if ([p runModalForDirectory:_folder file:nil types:nil] != NSOKButton || [[p filenames] count] == 0)
        return;
    [_folder release];
    _folder = [[[p filenames] objectAtIndex:0] retain];
    [[self ud] setObject:_folder forKey:@"folder"];
    [self updateFolderButton];
}

- (void)revealFolder:(id)sender
{
    (void)sender;
    [[NSWorkspace sharedWorkspace] openFile:_folder];
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    if (!g_app)
        return NO;
    if ([item action] == @selector(toggleRecord:)) {
        [item setTitle:app_is_recording(g_app) ? @"Stop Recording" : @"Start Recording"];
        return !_finishing && (app_is_recording(g_app) || app_state(g_app) == APP_STREAMING);
    }
    if ([item action] == @selector(chooseFolder:))
        return !app_is_recording(g_app);
    if ([item action] == @selector(snapshot:))
        return app_state(g_app) == APP_STREAMING && app_preview(g_app);
    if ([item action] == @selector(togglePreview:))
        [item setState:app_preview(g_app) ? NSOnState : NSOffState];
    if ([item action] == @selector(tapeButton:))
        return app_state(g_app) == APP_STREAMING || app_state(g_app) == APP_NO_SIGNAL;
    return YES;
}

/* ---- recording ---- */

- (void)stopRecording
{
    _finishing = YES;
    [_record setEnabled:NO];
    [_record setTitle:@"Finishing..."];
    app_record_stop(g_app, rec_done, self);
    [self layoutBar];
}

- (void)toggleRecord:(id)sender
{
    (void)sender;
    if (_finishing)
        return;
    if (app_is_recording(g_app)) {
        [self stopRecording];
        return;
    }
    if (app_state(g_app) != APP_STREAMING)
        return;
    char path[1100], err[300] = "";
    if (app_record_start(g_app, [_folder fileSystemRepresentation], (int)[_format indexOfSelectedItem], path,
                         sizeof(path), err, sizeof(err))) {
        NSRunAlertPanel(@"Cannot start recording", @"%@", @"OK", nil, nil, [NSString stringWithUTF8String:err]);
        return;
    }
    [_record setTitle:@"Stop"];
    [self updateEnabled];
    [self layoutBar];
}

- (void)showSaved:(NSString *)s
{
    [_saved release];
    _saved = [s retain];
    [_savedUntil release];
    _savedUntil = [[NSDate dateWithTimeIntervalSinceNow:8] retain];
}

- (void)recordingDone:(NSDictionary *)info
{
    _finishing = NO;
    [_record setEnabled:YES];
    [_record setTitle:@"Record"];
    [self updateEnabled];
    [self layoutBar];
    NSString *path = [info objectForKey:@"path"];
    if ([[info objectForKey:@"ok"] boolValue]) {
        [self showSaved:[@"Saved " stringByAppendingString:[path lastPathComponent]]];
        [[NSWorkspace sharedWorkspace] selectFile:path inFileViewerRootedAtPath:@""];
    } else {
        [self showSaved:@"Recording failed: the file could not be written"];
    }
}

static void rec_done(void *user, int ok, const char *path)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSDictionary *info = [NSDictionary dictionaryWithObjectsAndKeys:[NSNumber numberWithBool:ok != 0], @"ok",
                                                                    [NSString stringWithUTF8String:path], @"path", nil];
    [(Controller *)user performSelectorOnMainThread:@selector(recordingDone:) withObject:info waitUntilDone:NO];
    [pool release];
}

- (void)snapshot:(id)sender
{
    (void)sender;
    char path[1100], err[300] = "";
    if (app_snapshot(g_app, [_folder fileSystemRepresentation], path, sizeof(path), err, sizeof(err)))
        [self showSaved:[NSString stringWithUTF8String:err]];
    else
        [self showSaved:[@"Saved " stringByAppendingString:[[NSString stringWithUTF8String:path] lastPathComponent]]];
}

/* ---- timer: the picture every refresh, the rest a few times a second ---- */

- (void)tick:(NSTimer *)t
{
    (void)t;
    if (!g_app)
        return;
    if (app_preview(g_app))
        [_preview poll];
    if (++_ticks % 3)
        return;
    double db[2];
    app_peaks(g_app, db);
    int c;
    for (c = 0; c < 2; c++) {
        double v = db[c] + 60;
        _level[c] = v > _level[c] - 1.5 ? v : _level[c] - 1.5;
    }
    [_meters setLeft:(float)(_level[0] / 60) right:(float)(_level[1] / 60)];
    [_preview setAspect:app_aspect(g_app)];

    app_state_t st = app_state(g_app);
    BOOL rec = app_is_recording(g_app);
    [_record setEnabled:!_finishing && (rec || st == APP_STREAMING)];
    BOOL connected = st == APP_STREAMING || st == APP_NO_SIGNAL;
    int i;
    for (i = 0; i < TAPE_BUTTONS; i++)
        [_tape[i] setEnabled:connected];
    if (st != APP_STREAMING)
        [_preview clear];

    fwdv_status_t ts;
    NSString *tc = @"--:--:--.--", *mode = @"";
    if (connected && app_tape_status(g_app, &ts) == 0) {
        static NSString *const modes[] = { @"", @"Stopped", @"Playing", @"Paused", @"Rewinding",
                                           @"Fast forwarding", @"Reverse", @"Recording", @"Camera" };
        if (ts.tc_valid)
            tc = [NSString stringWithFormat:@"%02d:%02d:%02d.%02d", ts.tc_h, ts.tc_m, ts.tc_s, ts.tc_f];
        if ((unsigned)ts.mode < sizeof(modes) / sizeof(modes[0]))
            mode = modes[ts.mode];
        if (ts.has_tape == 0 && ts.mode != FWDV_MODE_CAMERA)
            mode = @"No tape";
    }
    if (![[_timecode stringValue] isEqualToString:tc])
        [_timecode setStringValue:tc];
    if (![[_tapeState stringValue] isEqualToString:mode])
        [_tapeState setStringValue:mode];

    if (rec && !_finishing && (app_auto_stop_due(g_app) || app_stop_after_due(g_app)))
        [self stopRecording]; /* "Stop at end of tape" or "Stop after" */

    char status[600], name[128];
    app_status(g_app, status, sizeof(status));
    NSString *s = [NSString stringWithUTF8String:status];
    if (st == APP_STREAMING && !app_preview(g_app))
        s = [@"Preview off - " stringByAppendingString:s];
    app_device_name(g_app, name, sizeof(name));
    NSString *title = name[0] ? [NSString stringWithFormat:@APP_NAME " - %s", name] : @APP_NAME;
    if (![[_window title] isEqualToString:title])
        [_window setTitle:title];
    if (rec) {
        int sec = (int)app_record_seconds(g_app);
        [_time setStringValue:[NSString stringWithFormat:@"%02d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60]];
        s = [NSString stringWithFormat:@"Recording %.1f MB - %@", app_record_bytes(g_app) / 1e6, s];
    } else if (_finishing) {
        s = @"Finishing the file...";
    } else {
        [_time setStringValue:@"00:00:00"];
        if (_saved && [_savedUntil timeIntervalSinceNow] > 0)
            s = _saved;
    }
    [self setStatus:s];
}

/* ---- quitting ---- */

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app
{
    (void)app;
    if (!g_app || !app_is_recording(g_app))
        return NSTerminateNow;
    if (NSRunAlertPanel(@"A recording is in progress", @"Stop the recording and quit?", @"Stop and Quit", @"Cancel",
                        nil) != NSAlertDefaultReturn)
        return NSTerminateCancel;
    return NSTerminateNow;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    [_timer invalidate];
    app_close(g_app); /* finishes a recording still in progress */
    g_app = NULL;
}
@end

int main(int argc, const char **argv)
{
    (void)argc, (void)argv;
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];
    Controller *c = [[Controller alloc] init];
    [NSApp setDelegate:c];
    [NSApp run];
    [pool release];
    return 0;
}
