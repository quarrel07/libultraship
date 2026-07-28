// macUtils.mm
#ifdef __APPLE__
#import "ship/utils/macUtils.h"
#import <SDL_syswm.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

//Just a simple function to toggle the native macOS fullscreen.
void toggleNativeMacOSFullscreen(SDL_Window *window) {
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(window, &wmInfo)) {
        NSWindow *nswindow = wmInfo.info.cocoa.window;
        [nswindow toggleFullScreen:nil];
    }
}

//Just a simple function to check if we are in native macOS fullscreen mode. Needed to avoid the game from crashing
//when going from native to SDL fullscreening modes or getting other forms of breakage.
bool isNativeMacOSFullscreenActive(SDL_Window *window) {
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(window, &wmInfo)) {
        NSWindow *nswindow = wmInfo.info.cocoa.window;
        return (([nswindow styleMask] & NSWindowStyleMaskFullScreen) == NSWindowStyleMaskFullScreen);
    }
    return false;
}

// See header. Cocoa frames have a bottom-left origin, so the window's top offset from
// the screen's top edge is screenTop - windowTop.
float getMacWindowNotchHiddenTopPoints(SDL_Window *window) {
    if (@available(macOS 12.0, *)) {
        SDL_SysWMinfo wmInfo;
        SDL_VERSION(&wmInfo.version);
        if (SDL_GetWindowWMInfo(window, &wmInfo)) {
            NSWindow *nswindow = wmInfo.info.cocoa.window;
            NSScreen *screen = [nswindow screen] ?: [NSScreen mainScreen];
            if (screen == nil) {
                return 0.0f;
            }
            const NSRect screenFrame = [screen frame];
            const NSRect contentFrame = [nswindow contentRectForFrameRect:[nswindow frame]];
            const CGFloat screenTop = screenFrame.origin.y + screenFrame.size.height;
            const CGFloat windowTop = contentFrame.origin.y + contentFrame.size.height;
            const CGFloat topOffset = screenTop - windowTop;
            const CGFloat inset = [screen safeAreaInsets].top;
            const CGFloat hidden = inset - topOffset;
            return hidden > 0.0 ? (float)hidden : 0.0f;
        }
    }
    return 0.0f;
}

static NSWindow *getNSWindow(SDL_Window *window) {
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(window, &wmInfo)) {
        return wmInfo.info.cocoa.window;
    }
    return nil;
}

static bool sFullPanelActive = false;
static NSUInteger sSavedStyleMask = 0;
static id sPanelActivityToken = nil;

// Holds a CADisplayLink while full-panel mode is live purely to declare a frame
// rate preference: without it, ProMotion wanders adaptively for our borderless
// window when the user is idle (~105 Hz average), and the game's steady 30 fps
// presents land on an irregular refresh grid -> visible judder. Pinning the
// preference at 120 (which 30 divides evenly) matches what fullscreen spaces
// get automatically. The tick itself does nothing.
@interface SKPanelDisplayLinkHolder : NSObject
- (void)tick:(CADisplayLink*)link;
@end
static double sDcSum = 0.0, sDcMin = 0.0, sDcMax = 0.0, sDcLast = 0.0;
static int sDcCount = 0;

@implementation SKPanelDisplayLinkHolder
- (void)tick:(CADisplayLink*)link {
    // Record actual display refresh cadence for the SK_FRAMELOG diagnostics.
    const double now = CACurrentMediaTime() * 1000.0;
    if (sDcLast > 0.0) {
        const double dt = now - sDcLast;
        sDcSum += dt;
        sDcCount++;
        if (sDcMin == 0.0 || dt < sDcMin) {
            sDcMin = dt;
        }
        if (dt > sDcMax) {
            sDcMax = dt;
        }
    }
    sDcLast = now;
    (void)link;
}
@end

void getAndResetDisplayCadence(double* avgMs, double* minMs, double* maxMs, int* count) {
    *avgMs = sDcCount > 0 ? sDcSum / sDcCount : 0.0;
    *minMs = sDcMin;
    *maxMs = sDcMax;
    *count = sDcCount;
    sDcSum = 0.0;
    sDcMin = 0.0;
    sDcMax = 0.0;
    sDcCount = 0;
}
static CADisplayLink* sPanelDisplayLink = nil;
static SKPanelDisplayLinkHolder* sPanelLinkHolder = nil;

// EXPERIMENTAL full-panel ("into the notch") mode: borderless window covering the
// screen's entire frame, menu bar and Dock auto-hidden. Normal fullscreen spaces
// stop below the camera housing; a borderless window is allowed to cover the full
// frame, with the housing physically occluding the top-center.
bool enterMacFullPanelMode(SDL_Window *window) {
    NSWindow *nswindow = getNSWindow(window);
    if (nswindow == nil) {
        return false;
    }
    // Never restyle a window that is inside a native fullscreen space: AppKit
    // throws and terminates the app. Callers must leave native fullscreen first.
    if (([nswindow styleMask] & NSWindowStyleMaskFullScreen) == NSWindowStyleMaskFullScreen) {
        return false;
    }
    NSScreen *screen = [nswindow screen] ?: [NSScreen mainScreen];
    if (screen == nil) {
        return false;
    }
    sSavedStyleMask = [nswindow styleMask];
    // Fully hide the menu bar and Dock (not auto-hide): auto-hide keeps the
    // system UI armed on the screen edges over our borderless window, and the
    // periodic renegotiation around it showed up as sporadic 100-600 ms frame
    // stalls on otherwise-static screens. Full hide is the standard game setup.
    [NSApp setPresentationOptions:(NSApplicationPresentationHideMenuBar |
                                   NSApplicationPresentationHideDock)];
    [nswindow setStyleMask:NSWindowStyleMaskBorderless];
    // Kill the default (white) window chrome so no light border can peek out
    // around the edges of the full-panel surface.
    [nswindow setBackgroundColor:[NSColor blackColor]];
    [nswindow setOpaque:YES];
    // Regular (non-fullscreen-space) windows get a shadow that includes a subtle
    // 1px light rim; native fullscreen never shows it, so drop it here too.
    [nswindow setHasShadow:NO];
    [nswindow setFrame:[screen frame] display:YES];
    [nswindow makeKeyAndOrderFront:nil];
    // Hold a latency-critical activity assertion while full-panel mode is live.
    // A borderless window is not a "fullscreen app" to macOS, so without this the
    // system applies idle heuristics (timer coalescing / App Nap / refresh
    // downclocking) whenever the user stops giving input - which showed up as
    // constant menu stutter that paused while the mouse moved. Native fullscreen
    // gets this exemption automatically; we have to ask for it.
    if (sPanelActivityToken == nil) {
        sPanelActivityToken = [[NSProcessInfo processInfo]
            beginActivityWithOptions:(NSActivityUserInitiated | NSActivityLatencyCritical)
                              reason:@"Full-panel fullscreen rendering"];
        [sPanelActivityToken retain];
    }
    if (@available(macOS 14.0, *)) {
        if (sPanelDisplayLink == nil) {
            sPanelLinkHolder = [[SKPanelDisplayLinkHolder alloc] init];
            sPanelDisplayLink =
                [[nswindow displayLinkWithTarget:sPanelLinkHolder selector:@selector(tick:)] retain];
            sPanelDisplayLink.preferredFrameRateRange = CAFrameRateRangeMake(60.0f, 120.0f, 120.0f);
            [sPanelDisplayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
        }
    }
    const NSRect got = [nswindow frame];
    const NSRect want = [screen frame];
    sFullPanelActive = NSEqualRects(got, want);
    return sFullPanelActive;
}

void exitMacFullPanelMode(SDL_Window *window) {
    NSWindow *nswindow = getNSWindow(window);
    sFullPanelActive = false;
    if (nswindow == nil) {
        return;
    }
    [NSApp setPresentationOptions:NSApplicationPresentationDefault];
    [nswindow setHasShadow:YES];
    if (sPanelActivityToken != nil) {
        [[NSProcessInfo processInfo] endActivity:sPanelActivityToken];
        [sPanelActivityToken release];
        sPanelActivityToken = nil;
    }
    if (sPanelDisplayLink != nil) {
        [sPanelDisplayLink invalidate];
        [sPanelDisplayLink release];
        sPanelDisplayLink = nil;
        [sPanelLinkHolder release];
        sPanelLinkHolder = nil;
    }
    // setStyleMask throws inside a native fullscreen space; guard and swallow so a
    // state mismatch can never take the game down.
    if (([nswindow styleMask] & NSWindowStyleMaskFullScreen) == NSWindowStyleMaskFullScreen) {
        return;
    }
    @try {
        [nswindow setStyleMask:(sSavedStyleMask != 0
                                    ? sSavedStyleMask
                                    : (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                       NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable))];
    } @catch (NSException *e) {
        printf("exitMacFullPanelMode: setStyleMask threw: %s\n", [[e reason] UTF8String]);
    }
}

bool isMacFullPanelModeActive(SDL_Window *window) {
    // Verify against the real window state so a stale flag can't misroute the
    // fullscreen logic: native fullscreen always wins, and panel mode requires
    // the borderless style we set.
    NSWindow *nswindow = getNSWindow(window);
    if (nswindow != nil) {
        if (([nswindow styleMask] & NSWindowStyleMaskFullScreen) == NSWindowStyleMaskFullScreen) {
            return false;
        }
        return sFullPanelActive && ([nswindow styleMask] == NSWindowStyleMaskBorderless);
    }
    return sFullPanelActive;
}
#endif