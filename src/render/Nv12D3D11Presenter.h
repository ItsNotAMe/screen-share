#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace screenshare {

class Nv12D3D11Presenter {
public:
    struct FrameView {
        int width = 0;
        int height = 0;
        const std::uint8_t* data = nullptr;
        std::size_t dataSize = 0;
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
    void Resize(std::uint32_t width, std::uint32_t height);
    void SetScaleMode(ScaleMode mode);
    void SetLinearSampling(bool enabled);
    void Present(const FrameView& frame);
    void Clear();
    void Reset();

    [[nodiscard]] std::uint64_t framesPresented() const noexcept { return framesPresented_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::uint64_t framesPresented_ = 0;
};

} // namespace screenshare
