#include "ship/window/MouseStateManager.h"

#include "ship/Context.h"
#include "ship/window/Window.h"

namespace Ship {
MouseStateManager::MouseStateManager() {
}

MouseStateManager::~MouseStateManager() {
}

void MouseStateManager::StartFrame() {
    CursorVisibilityTimeoutTick();
}

void MouseStateManager::CursorVisibilityTimeoutTick() {
    std::shared_ptr<Window> wnd = Context::GetInstance()->GetWindow();
    if (wnd->IsMouseCaptured()) {
        return;
    }

    // The OS cursor exists only while there is something to point at: the port menu or
    // menu bar, or when the user forces it via the "Cursor Always Visible" setting.
    // During normal play it stays hidden, even when the mouse moves — the game renders
    // its own hand cursor where pointing is part of the game (e.g. the file select).
    bool shouldShow = ShouldForceCursorVisibility() || wnd->GetGui()->GetMenuOrMenubarVisible();
    wnd->SetCursorVisibility(shouldShow);
}

bool MouseStateManager::ShouldAutoCaptureMouse() {
    return mAutoCaptureMouse;
}

void MouseStateManager::SetAutoCaptureMouse(bool capture) {
    mAutoCaptureMouse = capture;
}

bool MouseStateManager::ShouldForceCursorVisibility() {
    return mForceCursorVisibility;
}

void MouseStateManager::SetForceCursorVisibility(bool visible) {
    mForceCursorVisibility = visible;
}

void MouseStateManager::ToggleMouseCaptureOverride() {
    const std::shared_ptr<Window> window = Context::GetInstance()->GetWindow();
    window->SetMouseCapture(!window->IsMouseCaptured());
}

void MouseStateManager::UpdateMouseCapture() {
    const std::shared_ptr<Window> window = Context::GetInstance()->GetWindow();
    if (!window->GetGui()->GetMenuOrMenubarVisible()) {
        window->SetMouseCapture(ShouldAutoCaptureMouse());
    } else {
        window->SetMouseCapture(false);
        ResetCursorVisibilityTimer();
    }
}

void MouseStateManager::ResetCursorVisibilityTimer() {
    mCursorVisibleTicksCounter = mCursorVisibleTicks;
}

void MouseStateManager::SetCursorVisibilityTimeTicks(uint32_t ticks) {
    mCursorVisibleTicks = ticks;
}

uint32_t MouseStateManager::GetCursorVisibilityTimeTicks() {
    return mCursorVisibleTicks;
}
} // namespace Ship
