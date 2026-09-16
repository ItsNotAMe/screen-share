#pragma once
#include <cstdint>
#include <future>
#include <stdexcept>
namespace screenshare::media {
enum class CaptureKind { Display, Window };
struct CaptureSelection { CaptureKind kind = CaptureKind::Display; int display = 0; uint64_t window = 0; int fps = 60; };
enum class CaptureUpdateError { None, Invalid, Busy, Unsupported, Unavailable, Failed, Timeout, Cancelled };
struct CaptureUpdateResult { CaptureUpdateError error = CaptureUpdateError::None; uint64_t revision = 0; };
struct CaptureSelectionStatus { CaptureSelection selected; uint64_t revision = 0; };
inline void ValidateCaptureSelection(const CaptureSelection& value) {
    if ((value.kind != CaptureKind::Display && value.kind != CaptureKind::Window) ||
        value.display < 0 || value.display > 63 || value.fps < 1 || value.fps > 240 ||
        (value.kind == CaptureKind::Window && !value.window) || (value.kind == CaptureKind::Display && value.window))
        throw std::invalid_argument("Invalid capture selection");
}
inline std::future<CaptureUpdateResult> CaptureUpdateReady(CaptureUpdateError error) {
    std::promise<CaptureUpdateResult> result; result.set_value({error}); return result.get_future();
}
}
