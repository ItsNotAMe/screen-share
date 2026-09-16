#pragma once

#include "render/Nv12VideoFrame.h"
#include <Windows.h>
#include <cstdint>
#include <functional>
#include <memory>

// Constructed, used and destroyed on the presentation worker. The factory is
// injectable so headless tests exercise the real worker without a desktop/GPU.
class FramePresentationBackend {
public:
    virtual ~FramePresentationBackend() = default;
    virtual bool Present(HWND window, uint32_t width, uint32_t height,
        bool smooth, bool lowLatency, const screenshare::Nv12VideoFrame& frame) = 0;
    virtual void Reset() noexcept = 0;
    virtual void Update(HWND window, uint32_t width, uint32_t height, bool smooth, bool lowLatency) = 0;
    virtual uint32_t MaximumFrameLatency() const noexcept = 0;
};
using FramePresentationFactory = std::function<std::unique_ptr<FramePresentationBackend>()>;
std::unique_ptr<FramePresentationBackend> CreateNativeFramePresentation();
