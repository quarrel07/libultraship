#pragma once

#include <SDL.h>
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Toggles the native macOS fullscreen mode for an SDL window.
 * @param window Pointer to the SDL_Window to toggle.
 */
void toggleNativeMacOSFullscreen(SDL_Window* window);

/**
 * @brief Checks whether a native macOS fullscreen transition is active for an SDL window.
 * @param window Pointer to the SDL_Window to query.
 * @return true if native macOS fullscreen is currently active, false otherwise.
 */
bool isNativeMacOSFullscreenActive(SDL_Window* window);

/**
 * @brief Points at the top of the window hidden by the display's camera housing (notch).
 *
 * macOS sizes fullscreen windows to the visible frame (screen minus menu bar), but on
 * notched displays the camera housing's safe-area inset is a few points taller than the
 * menu bar, so the top of a fullscreen window sits partly behind the housing. Returns
 * max(0, safeAreaInset.top - windowTopOffsetFromScreenTop) in logical points; 0 on
 * screens without a notch, in windowed mode, or on macOS versions before 12.0.
 */
float getMacWindowNotchHiddenTopPoints(SDL_Window* window);

/**
 * @brief EXPERIMENTAL: cover the entire display panel, including the camera-housing
 * (notch) rows, with a borderless window — the "full panel" fullscreen used by games
 * like World of Warcraft. Normal macOS fullscreen stops below the housing.
 * @return true if the window now covers the full screen frame.
 */
bool enterMacFullPanelMode(SDL_Window* window);

/**
 * @brief Leave full-panel mode: restore the standard window style and let the caller
 * re-apply windowed geometry.
 */
void exitMacFullPanelMode(SDL_Window* window);

/** @brief True while the window is in full-panel (into-the-notch) mode. */
bool isMacFullPanelModeActive(SDL_Window* window);

/**
 * @brief Diagnostics: returns and resets the display-link cadence stats gathered
 * while full-panel mode is active (callback-to-callback intervals in ms).
 * All outputs are zero when the display link is not running.
 */
void getAndResetDisplayCadence(double* avgMs, double* minMs, double* maxMs, int* count);

#ifdef __cplusplus
}
#endif
