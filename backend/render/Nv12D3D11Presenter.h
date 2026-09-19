#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
struct ID3D11Texture2D;

namespace screenshare {

enum class PresentationOutcome { Unknown, Presented, Busy, Occluded, Minimized, Unavailable, Backoff, Failed };
inline const char* PresentationOutcomeName(PresentationOutcome value) noexcept {
    switch (value) {
    case PresentationOutcome::Presented: return "presented";
    case PresentationOutcome::Busy: return "busy";
    case PresentationOutcome::Occluded: return "occluded";
    case PresentationOutcome::Minimized: return "minimized";
    case PresentationOutcome::Unavailable: return "unavailable";
    case PresentationOutcome::Backoff: return "recovery-backoff";
    case PresentationOutcome::Failed: return "failed";
    default: return "unknown";
    }
}

class PresentationError : public std::runtime_error {
public:
    PresentationError(HRESULT result, const std::string& message)
        : std::runtime_error(message), result_(result) {}
    HRESULT result() const noexcept { return result_; }
private:
    HRESULT result_;
};

class Nv12D3D11Presenter {
public:
    struct FrameView {
        int width = 0;
        int height = 0;
        const std::uint8_t* data = nullptr;
        std::size_t dataSize = 0;
        ID3D11Texture2D* texture = nullptr;
    };

    enum class ScaleMode {
        Fit,
        OriginalSize,
    };

    Nv12D3D11Presenter();
    ~Nv12D3D11Presenter();

    Nv12D3D11Presenter(const Nv12D3D11Presenter&) = delete;
    Nv12D3D11Presenter& operator=(const Nv12D3D11Presenter&) = delete;

    void Attach(HWND hwnd);
    void Resize(std::uint32_t width, std::uint32_t height, bool redraw = true);
    void SetScaleMode(ScaleMode mode, bool redraw = true);
    void SetLinearSampling(bool enabled, bool redraw = true);
    void Present(const FrameView& frame);
    // Configure before Attach. Busy/occluded frames are dropped, never retried.
    void SetLowLatency(bool enabled);
    bool TryPresent(const FrameView& frame);
    [[nodiscard]] bool isHardwareAccelerated() const noexcept;
    [[nodiscard]] std::uint32_t maximumFrameLatency() const noexcept;
    [[nodiscard]] PresentationOutcome lastOutcome() const noexcept;
    void Clear();
    void Redraw();
    void Reset();

    [[nodiscard]] std::uint64_t framesPresented() const noexcept { return framesPresented_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::uint64_t framesPresented_ = 0;
};

} // namespace screenshare
