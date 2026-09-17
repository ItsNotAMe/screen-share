#pragma once
#include "render/Nv12D3D11Presenter.h"

namespace screenshare {
// Native video surfaces are child HWNDs. IsIconic(child) does not describe a
// minimized application shell; visibility alone does not describe minimization.
// Unknown means eligible to render, not proof that DXGI will present the frame.
inline PresentationOutcome PresentationTargetBlockReason(HWND window) noexcept {
    if (!window || !IsWindow(window)) return PresentationOutcome::Unavailable;
    const HWND root = GetAncestor(window, GA_ROOT);
    if (IsIconic(window) || (root && IsIconic(root))) return PresentationOutcome::Minimized;
    RECT bounds{};
    if (!IsWindowVisible(window) || !GetClientRect(window, &bounds) ||
        bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return PresentationOutcome::Unavailable;
    return PresentationOutcome::Unknown;
}
}
