#pragma once

#include "codec/H264StreamDecoder.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

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
    ReceiverPreviewWindow();
    ~ReceiverPreviewWindow();

    ReceiverPreviewWindow(const ReceiverPreviewWindow&) = delete;
    ReceiverPreviewWindow& operator=(const ReceiverPreviewWindow&) = delete;

    void Show();
    bool PumpMessages();
    void PresentFrame(const DecodedFrameInfo& frame);
    void ClearFrame();
    void SetStatusText(std::string_view statusText);
    void SetControlCallbacks(ReceiverPreviewControlCallbacks callbacks);

    [[nodiscard]] bool closeRequested() const noexcept { return closeRequested_; }
    [[nodiscard]] uint64_t framesPresented() const noexcept { return framesPresented_; }

    static LRESULT CALLBACK StaticWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    LRESULT WindowProc(UINT message, WPARAM wParam, LPARAM lParam);

    void EnsureWindow(int preferredWidth, int preferredHeight);
    void CreateDeviceAndSwapChain();
    void UpdateClientSize();
    void ResizeSwapChainIfNeeded();
    void EnsureRenderTarget();
    void EnsurePipeline();
    void EnsureFrameTextures(int width, int height);
    void SizeWindowForFirstFrame(int width, int height);
    void SizeWindowForCurrentFrame();
    void ToggleFullscreen();
    void SetFullscreen(bool fullscreen);
    void ToggleScaleMode();
    void RefreshTitle();
    [[nodiscard]] D3D11_VIEWPORT ComputeViewport() const;
    void Render();

    HWND hwnd_ = nullptr;
    uint32_t clientWidth_ = 0;
    uint32_t clientHeight_ = 0;
    bool closeRequested_ = false;
    bool swapChainResizePending_ = false;
    bool sizedForFirstFrame_ = false;
    bool fullscreen_ = false;
    DWORD windowedStyle_ = 0;
    DWORD windowedExStyle_ = 0;
    WINDOWPLACEMENT windowedPlacement_{};
    PreviewScaleMode scaleMode_ = PreviewScaleMode::Fit;
    ReceiverPreviewControlCallbacks controlCallbacks_;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> lumaTexture_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> chromaTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lumaView_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> chromaView_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> previewConstants_;
    D3D11_TEXTURE2D_DESC lumaDesc_{};
    D3D11_TEXTURE2D_DESC chromaDesc_{};

    int frameWidth_ = 0;
    int frameHeight_ = 0;
    uint64_t framesPresented_ = 0;
    std::string statusText_;
};

} // namespace screenshare
