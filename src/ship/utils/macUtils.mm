// macUtils.mm
#ifdef __APPLE__
#import "ship/utils/macUtils.h"
#import <SDL_syswm.h>
#import <Cocoa/Cocoa.h>

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
    [NSApp setPresentationOptions:(NSApplicationPresentationAutoHideMenuBar |
                                   NSApplicationPresentationAutoHideDock)];
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