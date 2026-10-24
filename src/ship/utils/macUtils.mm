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

// Make Cmd+F the full screen shortcut: rebind the Window menu's full screen item (Ctrl+Cmd+F by default), so the
// menu and the shortcut share one native path. The menu consumes the key, so the game never sees the F press.
void useCommandFForNativeMacOSFullscreen(void) {
    NSMutableArray<NSMenu *> *menus = [NSMutableArray arrayWithObject:[NSApp mainMenu] ?: [NSMenu new]];
    while (menus.count > 0) {
        NSMenu *menu = menus.lastObject;
        [menus removeLastObject];
        for (NSMenuItem *item in menu.itemArray) {
            if (item.action == @selector(toggleFullScreen:)) {
                item.keyEquivalent = @"f";
                item.keyEquivalentModifierMask = NSEventModifierFlagCommand;
            }
            if (item.submenu != nil) {
                [menus addObject:item.submenu];
            }
        }
    }
}

bool isWindowOccluded(SDL_Window *window) {
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(window, &wmInfo)) {
        NSWindow *nswindow = wmInfo.info.cocoa.window;
        return ([nswindow occlusionState] & NSWindowOcclusionStateVisible) == 0;
    }
    return false;
}

// Opt out of App Nap. When the app isn't frontmost (unfocused or hidden) macOS
// otherwise throttles its timers and CPU scheduling across *all* threads, which
// slows the game loop and starves the audio thread (stutter). Registering a
// latency-critical activity keeps the process running at full speed in the
// background. The token is held for the app's lifetime via a strong static (ARC).
void disableMacOSAppNap(void) {
    static id<NSObject> sAppNapActivity = nil;
    if (sAppNapActivity == nil) {
        sAppNapActivity = [[NSProcessInfo processInfo]
            beginActivityWithOptions:(NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical)
                              reason:@"Starship keeps simulating and outputs audio while unfocused"];
    }
}
#endif