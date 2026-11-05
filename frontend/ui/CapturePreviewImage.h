#pragma once
#include "capture/DesktopCapturer.h"
#include <QImage>
#include <QThread>
#include <algorithm>
#include <thread>

inline constexpr std::chrono::milliseconds CapturePreviewInterval{200};
inline void ConfigureCapturePreview(screenshare::CaptureConfig& config, QSize maximum) {
    config.targetWidth = maximum.width(); config.targetHeight = maximum.height();
    config.fitWithinTarget = true; config.targetFps = 5;
}
inline void WaitForCapturePreview(std::chrono::steady_clock::time_point next) {
    while (!QThread::currentThread()->isInterruptionRequested()) {
        const auto remaining = next - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) break;
        std::this_thread::sleep_for(std::min(remaining,
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(10))));
    }
}

// Queued UI updates outlive the captured frame. QImage::scaled can return a
// shallow copy when the requested size equals the input, so detach explicitly.
inline QImage CapturePreviewImage(const screenshare::CapturedFrame& frame, QSize size) {
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) return {};
    return QImage(reinterpret_cast<const uchar*>(frame.pixels.data()), frame.width, frame.height,
                  frame.rowPitch, QImage::Format_RGB32)
        .scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation).copy();
}
