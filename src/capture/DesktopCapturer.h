#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
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

struct WindowCaptureInfo {
    uint64_t handle = 0;
    uint32_t processId = 0;
    std::wstring title;
    std::wstring processName;
    long left = 0;
    long top = 0;
    long right = 0;
    long bottom = 0;
};

enum class CaptureBackend {
    DesktopDuplication,
    WindowsGraphicsCapture,
};

enum class CaptureSourceType {
    Display,
    Window,
};

struct WindowsGraphicsCaptureState;

struct CaptureConfig {
    CaptureSourceType sourceType = CaptureSourceType::Display;
    int displayIndex = 0;
    uint64_t windowHandle = 0;
    int targetWidth = 0;
    int targetHeight = 0;
    int targetFps = 60;
    CaptureBackend backend = CaptureBackend::WindowsGraphicsCapture;
    bool wgcBorderRequired = false;
    bool includeNv12 = false;
    bool includeNv12Readback = true;
    bool includeBgraReadback = true;
    bool hdrToSdr = true;
    float hdrSdrWhiteNits = 203.0f;
    float hdrSdrBgraExposure = 0.88f;
};

struct CapturedFrame {
    int sourceWidth = 0;
    int sourceHeight = 0;
    int width = 0;
    int height = 0;
    DXGI_FORMAT sourceFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DXGI_COLOR_SPACE_TYPE displayColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool displayHdrActive = false;
    uint32_t colorConversionMode = 0;
    uint32_t rowPitch = 0;
    int64_t lastPresentTimeQpc = 0;
    std::vector<std::byte> pixels;
    std::vector<std::byte> nv12Pixels;
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12Texture;
    uint32_t nv12TextureSubresource = 0;
    bool nv12GeneratedOnGpu = false;
};

class DesktopCapturer {
public:
    DesktopCapturer();
    ~DesktopCapturer();

    DesktopCapturer(const DesktopCapturer&) = delete;
    DesktopCapturer& operator=(const DesktopCapturer&) = delete;

    static std::vector<DisplayInfo> EnumerateDisplays();
    static std::vector<WindowCaptureInfo> EnumerateWindows();

    void Start(const CaptureConfig& config);
    void Stop();

    [[nodiscard]] std::optional<CapturedFrame> TryCaptureFrame(std::chrono::milliseconds timeout);
    [[nodiscard]] const CaptureConfig& config() const noexcept { return config_; }

private:
    void CreateDevice(IDXGIAdapter* adapter);
    void CreateDuplicationForDisplay(int displayIndex);
    void CreateWindowsGraphicsCaptureForDisplay(int displayIndex);
    void CreateWindowsGraphicsCaptureForWindow(uint64_t windowHandle);
    void DetectOutputColorSpace(IDXGIOutput* output);
    void DetectOutputColorSpaceForMonitor(HMONITOR monitor);
    void InitializeWinRt();
    void EnsureSourceTexture(const D3D11_TEXTURE2D_DESC& sourceDesc);
    void EnsureScalePipeline();
    void EnsureScaledTexture(const D3D11_TEXTURE2D_DESC& sourceDesc, int width, int height);
    void EnsureStagingTexture(const D3D11_TEXTURE2D_DESC& outputDesc);
    void EnsureNv12Pipeline();
    void EnsureNv12InputTexture(const D3D11_TEXTURE2D_DESC& bgraDesc);
    void EnsureNv12Textures(int width, int height);
    ID3D11Texture2D* ScaleFrameIfNeeded(ID3D11Texture2D* sourceTexture, const D3D11_TEXTURE2D_DESC& sourceDesc);
    void GenerateNv12Frame(ID3D11Texture2D* bgraTexture, const D3D11_TEXTURE2D_DESC& bgraDesc, CapturedFrame& frame);
    std::optional<CapturedFrame> ReadTextureFrame(
        ID3D11Texture2D* sourceTexture,
        const D3D11_TEXTURE2D_DESC& sourceDesc,
        int64_t presentTimeQpc);
    std::optional<CapturedFrame> TryCaptureWindowsGraphicsFrame(std::chrono::milliseconds timeout);

    CaptureConfig config_{};
    struct Nv12TextureSet {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> lumaTarget;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> chromaTarget;
    };

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
    Microsoft::WRL::ComPtr<ID3D11Buffer> scaleConstants_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12InputTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> nv12InputView_;
    std::vector<Nv12TextureSet> nv12Textures_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12StagingTexture_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> nv12VertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> nv12LumaPixelShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> nv12ChromaPixelShader_;
    std::unique_ptr<WindowsGraphicsCaptureState> wgc_;
    DXGI_COLOR_SPACE_TYPE outputColorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool outputHdrActive_ = false;
    uint32_t lastColorConversionMode_ = 0;
    bool winrtInitialized_ = false;
    D3D11_TEXTURE2D_DESC sourceTextureDesc_{};
    D3D11_TEXTURE2D_DESC scaledDesc_{};
    D3D11_TEXTURE2D_DESC stagingDesc_{};
    D3D11_TEXTURE2D_DESC nv12InputDesc_{};
    D3D11_TEXTURE2D_DESC nv12TextureDesc_{};
    D3D11_TEXTURE2D_DESC nv12StagingDesc_{};
    size_t nv12TextureCursor_ = 0;
};

std::string Narrow(const std::wstring& text);
std::string HResultMessage(HRESULT hr);
const char* DxgiFormatName(DXGI_FORMAT format);
const char* DxgiColorSpaceName(DXGI_COLOR_SPACE_TYPE colorSpace);
const char* CaptureColorConversionName(uint32_t mode);

} // namespace screenshare
