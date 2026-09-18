#pragma once

#include "codec/H264StreamDecoder.h"
#include "Nv12VideoFrame.h"
#include "FramePresentationBackend.h"
#include "input/v2/InputProtocol.h"

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace screenshare {

enum class PreviewScaleMode {
    Fit,
    OriginalSize,
};

struct ReceiverPreviewControlCallbacks {
    std::function<void()> toggleAudioMute;
    std::function<void(int)> adjustAudioVolumePercent;
};

class ReceiverPreviewWindow {
public:
    explicit ReceiverPreviewWindow(FramePresentationFactory factory = {});
    ~ReceiverPreviewWindow();

    ReceiverPreviewWindow(const ReceiverPreviewWindow&) = delete;
    ReceiverPreviewWindow& operator=(const ReceiverPreviewWindow&) = delete;

    void Show();
    bool PumpMessages();
    void PresentFrame(const DecodedFrameInfo& frame);
    void PresentFrame(const Nv12VideoFrame& frame);
    // Configure before Show/PresentFrame. Busy/occluded frames are discarded.
    void SetLowLatency(bool enabled);
    uint32_t maximumFrameLatency() const noexcept { return presenter_.statistics().maximumFrameLatency; }
    uint64_t framesDropped() const noexcept { return framesDropped_; }
    FramePresentationSession::Statistics presentationStats() const noexcept { return presenter_.statistics(); }
    HWND windowHandle() const noexcept { return hwnd_; }
    void ClearFrame();
    void SetStatusText(std::string_view statusText);
    void SetControlCallbacks(ReceiverPreviewControlCallbacks callbacks);
    void SetRemoteInput(uint8_t capabilities, std::function<void(const input::Event&)> callback);
    input::FrameMapping presentedInputMapping() const { return inputMapping_; }

    [[nodiscard]] bool closeRequested() const noexcept { return closeRequested_; }
    [[nodiscard]] uint64_t framesPresented() const noexcept { return framesPresented_; }

    static LRESULT CALLBACK StaticWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    LRESULT WindowProc(UINT message, WPARAM wParam, LPARAM lParam);

    void EnsureWindow(int preferredWidth, int preferredHeight);
    void UpdateClientSize();
    void SizeWindowForFirstFrame(int width, int height);
    void SizeWindowForCurrentFrame();
    void ToggleFullscreen();
    void SetFullscreen(bool fullscreen);
    void ToggleScaleMode();
    void RefreshTitle();
    bool Render();
    void PresentPixels(int width, int height, std::span<const uint8_t> pixels);
    void PresentView(const Nv12D3D11Presenter::FrameView& view);

    HWND hwnd_ = nullptr;
    uint32_t clientWidth_ = 0;
    uint32_t clientHeight_ = 0;
    bool closeRequested_ = false;
    bool sizedForFirstFrame_ = false;
    bool fullscreen_ = false;
    DWORD windowedStyle_ = 0;
    DWORD windowedExStyle_ = 0;
    WINDOWPLACEMENT windowedPlacement_{};
    PreviewScaleMode scaleMode_ = PreviewScaleMode::Fit;
    ReceiverPreviewControlCallbacks controlCallbacks_;

    FramePresentationSession presenter_;

    int frameWidth_ = 0;
    int frameHeight_ = 0;
    uint64_t framesPresented_ = 0;
    uint64_t framesDropped_ = 0;
    bool lowLatency_ = false;
    std::string statusText_;
    std::wstring renderedTitle_;
    input::FrameMapping inputMapping_;
    uint8_t inputCapabilities_ = 0;
    std::function<void(const input::Event&)> inputCallback_;
};

} // namespace screenshare
