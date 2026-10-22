/* window.m — NSWindow + CALayer presentation for the port (P3 v1).
 *
 * The engine renders into an ANGLE pbuffer (see egl_shim.c); eglSwapBuffers
 * hands us a top-down RGBA byte buffer (ownership transferred).  This file
 * keeps only the latest published frame, and a main-run-loop timer turns it
 * into a CGImage and points the content view's layer at it.  v1 is a CPU
 * readback + layer upload; zero-copy presentation (IOSurface) is future work.
 *
 * Compiled without ARC: everything lives for the process lifetime on purpose.
 */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pb.h"

static int g_buf_w = 1920, g_buf_h = 1080;
static int g_ready = 0;
static NSWindow *g_win = nil;
static NSView *g_view = nil;

/* latest published frame (owned), replaced under lock */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_frame = NULL;
static int g_fw = 0, g_fh = 0;

int pb_win_w(void) { return g_buf_w; }
int pb_win_h(void) { return g_buf_h; }
int pb_window_ready(void) { return g_ready; }

/* takes ownership of rgba */
void pb_window_publish(uint8_t *rgba, int w, int h)
{
    if (!g_ready) { free(rgba); return; }
    pthread_mutex_lock(&g_lock);
    free(g_frame);
    g_frame = rgba;
    g_fw = w;
    g_fh = h;
    pthread_mutex_unlock(&g_lock);
}

static void frame_release(void *info, const void *data, size_t size)
{
    (void)info; (void)size;
    free((void *)data);
}

/* main thread, ~60 Hz */
static void present_tick(void)
{
    uint8_t *b;
    int w, h;
    pthread_mutex_lock(&g_lock);
    b = g_frame; w = g_fw; h = g_fh;
    g_frame = NULL;
    pthread_mutex_unlock(&g_lock);
    if (!b) return;

    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, b, (size_t)w * h * 4,
                                                        frame_release);
    if (!dp) { free(b); return; }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, cs,
                                   (CGBitmapInfo)kCGImageAlphaNoneSkipLast, dp, NULL, false,
                                   kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(dp);        /* CGImage retains its own reference */
    if (!img) { free(b); return; }
    g_view.layer.contents = (id)img;
    CGImageRelease(img);
}

@interface PBWinDelegate : NSObject <NSWindowDelegate>
@end

@implementation PBWinDelegate
- (void)windowWillClose:(NSNotification *)note
{
    (void)note;
    [NSApplication.sharedApplication terminate:nil];
}
@end

static PBWinDelegate *g_delegate = nil;
static id g_event_monitor = nil;

/* macOS keycodes -> Android keycodes (the subset a game menu needs). */
static int mac_key_to_android(unsigned short kc)
{
    static const int map[128] = {
        [0] = 29, [1] = 47, [2] = 32, [3] = 34, [4] = 36, [5] = 35,
        [6] = 54, [7] = 52, [8] = 31, [9] = 50, [11] = 30,
        [12] = 45, [13] = 51, [14] = 33, [15] = 46, [16] = 53, [17] = 48,
        [18] = 8, [19] = 9, [20] = 10, [21] = 11, [22] = 13, [23] = 12,
        [25] = 16, [26] = 14, [28] = 15, [29] = 7,
        [31] = 43, [32] = 49, [34] = 37, [35] = 44,
        [37] = 40, [38] = 38, [40] = 39, [45] = 42, [46] = 41,
        [36] = 66,   /* Return  -> KEYCODE_ENTER   */
        [48] = 61,   /* Tab     -> KEYCODE_TAB     */
        [49] = 62,   /* Space   -> KEYCODE_SPACE   */
        [51] = 67,   /* Delete  -> KEYCODE_DEL     */
        [53] = 111,  /* Escape  -> KEYCODE_ESCAPE  */
        [123] = 21,  /* Left    -> KEYCODE_DPAD_LEFT  */
        [124] = 22,  /* Right   -> KEYCODE_DPAD_RIGHT */
        [125] = 20,  /* Down    -> KEYCODE_DPAD_DOWN  */
        [126] = 19,  /* Up      -> KEYCODE_DPAD_UP    */
    };
    return kc < 128 ? map[kc] : 0;
}

/* Map a point in the content view to engine-surface pixels.  The layer uses
 * kCAGravityResizeAspect, so letterbox the same way. */
static int view_to_surface(NSPoint p, double *ox, double *oy)
{
    if (!g_view) return 0;
    NSRect b = g_view.bounds;
    double vw = b.size.width, vh = b.size.height;
    double bw = pb_win_w(), bh = pb_win_h();
    if (vw <= 0 || vh <= 0 || bw <= 0 || bh <= 0) return 0;
    double scale = MIN(vw / bw, vh / bh);
    double dw = bw * scale, dh = bh * scale;
    double mx = (vw - dw) * 0.5, my = (vh - dh) * 0.5;
    double gx = (p.x - mx) / dw * bw;
    double gy = (1.0 - (p.y - my) / dh) * bh;   /* NSView is bottom-up */
    if (gx < 0 || gy < 0 || gx > bw || gy > bh) return 0;
    *ox = gx; *oy = gy;
    return 1;
}

static void install_input_monitor(void)
{
    if (getenv("PB_NO_HOST_INPUT")) return;   /* scripted-test mode */
    NSEventMask mask = NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp |
                       NSEventMaskLeftMouseDragged |
                       NSEventMaskRightMouseDown | NSEventMaskRightMouseUp |
                       NSEventMaskKeyDown | NSEventMaskKeyUp;
    g_event_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
                                                            handler:^NSEvent *(NSEvent *ev) {
        if (!g_win || !g_view || ev.window != g_win) return ev;
        if (ev.type == NSEventTypeKeyDown || ev.type == NSEventTypeKeyUp) {
            int code = mac_key_to_android(ev.keyCode);
            if (code) pb_input_key(ev.type == NSEventTypeKeyDown ? 0 : 1, code);
            return ev;
        }
        NSPoint p = [g_view convertPoint:ev.locationInWindow fromView:nil];
        double x, y;
        if (!view_to_surface(p, &x, &y)) return ev;
        switch (ev.type) {
            case NSEventTypeLeftMouseDown:
            case NSEventTypeRightMouseDown:  pb_input_motion(0, (float)x, (float)y); break;
            case NSEventTypeLeftMouseUp:
            case NSEventTypeRightMouseUp:    pb_input_motion(1, (float)x, (float)y); break;
            case NSEventTypeLeftMouseDragged: pb_input_motion(2, (float)x, (float)y); break;
            default: break;
        }
        return ev;
    }];
}

void pb_window_create(int w, int h, const char *title)
{
    @autoreleasepool {
        g_buf_w = w;
        g_buf_h = h;

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        /* minimal menu so Cmd+Q quits */
        NSMenu *menubar = [[NSMenu alloc] init];
        NSMenuItem *app_item = [[NSMenuItem alloc] init];
        [menubar addItem:app_item];
        NSMenu *app_menu = [[NSMenu alloc] init];
        [app_menu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
        [app_item setSubmenu:app_menu];
        [app setMainMenu:menubar];

        /* show the engine buffer in whatever size fits the screen */
        NSScreen *scr = [NSScreen mainScreen];
        NSRect avail = scr ? scr.visibleFrame : NSMakeRect(0, 0, 1280, 720);
        CGFloat cw = MIN((CGFloat)w, avail.size.width - 20);
        CGFloat ch = MIN((CGFloat)h, avail.size.height - 60);
        if (cw < 320) cw = 320;
        if (ch < 180) ch = 180;

        NSRect frame = NSMakeRect(avail.origin.x + 20, avail.origin.y + 20, cw, ch);
        g_win = [[NSWindow alloc] initWithContentRect:frame
                                            styleMask:(NSWindowStyleMaskTitled |
                                                       NSWindowStyleMaskClosable |
                                                       NSWindowStyleMaskMiniaturizable |
                                                       NSWindowStyleMaskResizable)
                                              backing:NSBackingStoreBuffered
                                                defer:NO];
        g_win.title = [NSString stringWithUTF8String:(title ? title : "Payback 2")];
        g_win.releasedWhenClosed = NO;
        g_win.minSize = NSMakeSize(320, 180);

        g_delegate = [[PBWinDelegate alloc] init];
        g_win.delegate = g_delegate;

        NSView *cv = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, cw, ch)];
        cv.wantsLayer = YES;
        cv.layer.backgroundColor = [NSColor blackColor].CGColor;
        cv.layer.contentsGravity = kCAGravityResizeAspect;
        cv.layer.contentsScale = scr ? scr.backingScaleFactor : 1.0;
        g_view = cv;
        g_win.contentView = cv;

        [g_win center];
        [g_win makeKeyAndOrderFront:nil];
        [app activateIgnoringOtherApps:YES];

        g_ready = 1;
        install_input_monitor();

        NSTimer *timer = [NSTimer timerWithTimeInterval:(1.0 / 60.0)
                                                repeats:YES
                                                  block:^(NSTimer *t) {
                                                      (void)t;
                                                      present_tick();
                                                  }];
        [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];

        pb_log("[win] window %dx%d pt showing %dx%d buffer\n",
               (int)cw, (int)ch, w, h);
    }
}

void pb_window_run(void)
{
    const char *hold = getenv("PB_HOLD");
    if (hold) {
        double s = atof(hold);
        [NSTimer scheduledTimerWithTimeInterval:(s > 0 ? s : 2.0)
                                        repeats:NO
                                          block:^(NSTimer *t) {
                                              (void)t;
                                              [NSApplication.sharedApplication terminate:nil];
                                          }];
    }
    [NSApplication.sharedApplication run];
}
