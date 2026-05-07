#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace screenshare {

struct DisplayInfo {
    int index = 0;
    std::wstring adapterName;
    std::wstring outputName;
    long left = 0;
    long top = 0;
    long right = 0;
    long bottom = 0;
    bool attachedToDesktop = false;
};

struct CaptureConfig {
    int displayIndex = 0;
    int targetWidth = 0;
    int targetHeight = 0;
    int targetFps = 60;
};

struct CapturedFrame {
    int sourceWidth = 0;
    int sourceHeight = 0;
    int width = 0;
    int height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t rowPitch = 0;
    int64_t lastPresentTimeQpc = 0;
    std::vector<std::byte> pixels;
};

class DesktopCapturer {
public:
    DesktopCapturer();
    ~DesktopCapturer();

    DesktopCapturer(const DesktopCapturer&) = delete;
    DesktopCapturer& operator=(const DesktopCapturer&) = delete;

    static std::vector<DisplayInfo> EnumerateDisplays();

    void Start(const CaptureConfig& config);
    void Stop();

    [[nodiscard]] std::optional<CapturedFrame> TryCaptureFrame(std::chrono::milliseconds timeout);
    [[nodiscard]] const CaptureConfig& config() const noexcept { return config_; }

private:
    void CreateDevice(IDXGIAdapter* adapter);
    void CreateDuplicationForDisplay(int displayIndex);
    void EnsureSourceTexture(const D3D11_TEXTURE2D_DESC& sourceDesc);
    void EnsureScalePipeline();
    void EnsureScaledTexture(const D3D11_TEXTURE2D_DESC& sourceDesc, int width, int height);
    void EnsureStagingTexture(const D3D11_TEXTURE2D_DESC& outputDesc);
    ID3D11Texture2D* ScaleFrameIfNeeded(ID3D11Texture2D* sourceTexture, const D3D11_TEXTURE2D_DESC& sourceDesc);

    CaptureConfig config_{};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sourceTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sourceView_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> scaledTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> scaledTarget_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTexture_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> scaleVertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> scalePixelShader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> scaleSampler_;
    D3D11_TEXTURE2D_DESC sourceTextureDesc_{};
    D3D11_TEXTURE2D_DESC scaledDesc_{};
    D3D11_TEXTURE2D_DESC stagingDesc_{};
};

std::string Narrow(const std::wstring& text);
std::string HResultMessage(HRESULT hr);

} // namespace screenshare
