#pragma once
#include "capture/DesktopCapturer.h"
#include <QImage>

// Queued UI updates outlive the captured frame. QImage::scaled can return a
// shallow copy when the requested size equals the input, so detach explicitly.
inline QImage CapturePreviewImage(const screenshare::CapturedFrame& frame, QSize size) {
    if (frame.width <= 0 || frame.height <= 0 || frame.pixels.empty()) return {};
    return QImage(reinterpret_cast<const uchar*>(frame.pixels.data()), frame.width, frame.height,
                  frame.rowPitch, QImage::Format_RGB32)
        .scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation).copy();
}
