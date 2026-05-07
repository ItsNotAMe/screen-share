#include "capture/DesktopCapturer.h"

#include <Windows.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <inspectable.h>
#include <roapi.h>
#include <windows.graphics.capture.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>
#include <winrt/base.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

extern "C" HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(
    IDXGIDevice* dxgiDevice,
    IInspectable** graphicsDevice);

struct IDirect3DDxgiInterfaceAccess : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void** object) = 0;
};

namespace screenshare {

namespace capture = winrt::Windows::Graphics::Capture;
namespace direct3d11 = winrt::Windows::Graphics::DirectX::Direct3D11;
namespace directx = winrt::Windows::Graphics::DirectX;
namespace metadata = winrt::Windows::Foundation::Metadata;
namespace graphics = winrt::Windows::Graphics;
namespace appcap = winrt::Windows::Security::Authorization::AppCapabilityAccess;

struct WindowsGraphicsCaptureState {
    capture::GraphicsCaptureItem item{nullptr};
    capture::Direct3D11CaptureFramePool framePool{nullptr};
    capture::GraphicsCaptureSession session{nullptr};
    direct3d11::IDirect3DDevice device{nullptr};
    graphics::SizeInt32 size{};
    bool skippedInitialFrame = false;
};

namespace {

void ThrowIfFailed(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) {
        if (hr == E_ACCESSDENIED && std::string(operation).find("DuplicateOutput") != std::string::npos) {
            throw std::runtime_error(
                std::string(operation) +
                " failed: Access is denied. Desktop Duplication requires an interactive desktop session; "
                "close other screen-capture apps, avoid secure/admin desktops, or use the Windows Graphics Capture backend.");
        }

        throw std::runtime_error(std::string(operation) + " failed: " + HResultMessage(hr));
    }
}

std::wstring OutputDeviceName(const DXGI_OUTPUT_DESC& desc)
{
    return desc.DeviceName;
}

std::wstring AdapterName(const DXGI_ADAPTER_DESC1& desc)
{
    return desc.Description;
}

bool IsBgra8Format(DXGI_FORMAT format)
{
    return
        format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        format == DXGI_FORMAT_B8G8R8X8_UNORM ||
        format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

bool IsPlainBgra8Format(DXGI_FORMAT format)
{
    return
        format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        format == DXGI_FORMAT_B8G8R8X8_UNORM;
}

bool IsRgbUnormFormat(DXGI_FORMAT format)
{
    return
        format == DXGI_FORMAT_R10G10B10A2_UNORM;
}

bool IsSrgbFormat(DXGI_FORMAT format)
{
    return
        format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
        format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ||
        format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

bool IsScRgbFormat(DXGI_FORMAT format)
{
    return
        format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
        format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
        format == DXGI_FORMAT_R11G11B10_FLOAT;
}

bool IsHdrColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace)
{
    return
        colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
        colorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020 ||
        colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
}

bool IsPqP2020ColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace)
{
    return
        colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
        colorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
}

const char* AppCapabilityAccessStatusName(appcap::AppCapabilityAccessStatus status)
{
    switch (status) {
    case appcap::AppCapabilityAccessStatus::DeniedBySystem:
        return "DeniedBySystem";
    case appcap::AppCapabilityAccessStatus::NotDeclaredByApp:
        return "NotDeclaredByApp";
    case appcap::AppCapabilityAccessStatus::DeniedByUser:
        return "DeniedByUser";
    case appcap::AppCapabilityAccessStatus::UserPromptRequired:
        return "UserPromptRequired";
    case appcap::AppCapabilityAccessStatus::Allowed:
        return "Allowed";
    default:
        return "Unknown";
    }
}

void ConfigureWindowsGraphicsCaptureBorder(capture::GraphicsCaptureSession const& session, bool borderRequired)
{
    if (!metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession",
            L"IsBorderRequired")) {
        if (!borderRequired) {
            std::cerr << "WGC borderless capture is not supported by this Windows version; keeping capture border.\n";
        }
        return;
    }

    if (borderRequired) {
        session.IsBorderRequired(true);
        return;
    }

    try {
        if (metadata::ApiInformation::IsMethodPresent(
                L"Windows.Graphics.Capture.GraphicsCaptureAccess",
                L"RequestAccessAsync",
                1)) {
            const auto accessStatus =
                capture::GraphicsCaptureAccess::RequestAccessAsync(capture::GraphicsCaptureAccessKind::Borderless).get();
            if (accessStatus != appcap::AppCapabilityAccessStatus::Allowed) {
                std::cerr
                    << "WGC borderless access was not granted ("
                    << AppCapabilityAccessStatusName(accessStatus)
                    << "); Windows may keep the capture border.\n";
            }
        }

        session.IsBorderRequired(false);
    } catch (const winrt::hresult_error& error) {
        std::cerr
            << "WGC borderless capture request failed: "
            << Narrow(error.message().c_str())
            << "; keeping capture border.\n";
    }
}

uint32_t ColorConversionMode(DXGI_FORMAT format, DXGI_COLOR_SPACE_TYPE colorSpace, bool hdrToSdr, bool outputHdrActive)
{
    if (hdrToSdr && IsScRgbFormat(format)) {
        return 1;
    }
    if (hdrToSdr && IsPqP2020ColorSpace(colorSpace) && format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        return 4;
    }
    if (hdrToSdr && outputHdrActive && IsRgbUnormFormat(format)) {
        return 3;
    }
    if (hdrToSdr && outputHdrActive && IsPlainBgra8Format(format)) {
        return 5;
    }
    if (IsSrgbFormat(format)) {
        return 2;
    }
    return 0;
}

struct ScaleConstants {
    uint32_t colorConversionMode = 0;
    float hdrSdrWhiteNits = 203.0f;
    float hdrSdrBgraExposure = 0.88f;
    float padding = 0.0f;
};

Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(const char* source, const char* entryPoint, const char* target)
{
    Microsoft::WRL::ComPtr<ID3DBlob> shader;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;

    const HRESULT result = D3DCompile(
        source,
        std::strlen(source),
        nullptr,
        nullptr,
        nullptr,
        entryPoint,
        target,
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        &shader,
        &errors);

    if (FAILED(result)) {
        std::string message = HResultMessage(result);
        if (errors) {
            message += ": ";
            message.append(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }

        throw std::runtime_error("D3DCompile failed for " + std::string(entryPoint) + ": " + message);
    }

    return shader;
}

} // namespace

DesktopCapturer::DesktopCapturer()
{
}

DesktopCapturer::~DesktopCapturer()
{
    Stop();
}

std::vector<DisplayInfo> DesktopCapturer::EnumerateDisplays()
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

    std::vector<DisplayInfo> displays;
    int displayIndex = 0;

    for (UINT adapterIndex = 0;; ++adapterIndex) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        HRESULT adapterResult = factory->EnumAdapters1(adapterIndex, &adapter);
        if (adapterResult == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        ThrowIfFailed(adapterResult, "IDXGIFactory1::EnumAdapters1");

        DXGI_ADAPTER_DESC1 adapterDesc{};
        ThrowIfFailed(adapter->GetDesc1(&adapterDesc), "IDXGIAdapter1::GetDesc1");

        for (UINT outputIndex = 0;; ++outputIndex) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            HRESULT outputResult = adapter->EnumOutputs(outputIndex, &output);
            if (outputResult == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            ThrowIfFailed(outputResult, "IDXGIAdapter1::EnumOutputs");

            DXGI_OUTPUT_DESC outputDesc{};
            ThrowIfFailed(output->GetDesc(&outputDesc), "IDXGIOutput::GetDesc");

            DisplayInfo info;
            info.index = displayIndex++;
            info.adapterName = AdapterName(adapterDesc);
            info.outputName = OutputDeviceName(outputDesc);
            info.left = outputDesc.DesktopCoordinates.left;
            info.top = outputDesc.DesktopCoordinates.top;
            info.right = outputDesc.DesktopCoordinates.right;
            info.bottom = outputDesc.DesktopCoordinates.bottom;
            info.attachedToDesktop = outputDesc.AttachedToDesktop != FALSE;
            displays.push_back(std::move(info));
        }
    }

    return displays;
}

void DesktopCapturer::Start(const CaptureConfig& config)
{
    Stop();
    config_ = config;
    if (config.backend == CaptureBackend::WindowsGraphicsCapture) {
        CreateWindowsGraphicsCaptureForDisplay(config.displayIndex);
    } else {
        CreateDuplicationForDisplay(config.displayIndex);
    }
}

void DesktopCapturer::Stop()
{
    wgc_.reset();
    duplication_.Reset();
    sourceTexture_.Reset();
    sourceView_.Reset();
    scaledTexture_.Reset();
    scaledTarget_.Reset();
    stagingTexture_.Reset();
    scaleVertexShader_.Reset();
    scalePixelShader_.Reset();
    scaleSampler_.Reset();
    scaleConstants_.Reset();
    context_.Reset();
    device_.Reset();
    std::memset(&sourceTextureDesc_, 0, sizeof(sourceTextureDesc_));
    std::memset(&scaledDesc_, 0, sizeof(scaledDesc_));
    std::memset(&stagingDesc_, 0, sizeof(stagingDesc_));
    outputColorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    outputHdrActive_ = false;
    lastColorConversionMode_ = 0;
    if (winrtInitialized_) {
        RoUninitialize();
        winrtInitialized_ = false;
    }
}

std::optional<CapturedFrame> DesktopCapturer::TryCaptureFrame(std::chrono::milliseconds timeout)
{
    if (wgc_) {
        return TryCaptureWindowsGraphicsFrame(timeout);
    }

    if (!duplication_) {
        throw std::logic_error("DesktopCapturer::Start must be called before capturing frames");
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;
    const HRESULT acquireResult = duplication_->AcquireNextFrame(
        static_cast<UINT>(timeout.count()),
        &frameInfo,
        &desktopResource);

    if (acquireResult == DXGI_ERROR_WAIT_TIMEOUT) {
        return std::nullopt;
    }

    if (acquireResult == DXGI_ERROR_ACCESS_LOST) {
        Stop();
        throw std::runtime_error("Display duplication access was lost. Restart capture after display changes.");
    }

    ThrowIfFailed(acquireResult, "IDXGIOutputDuplication::AcquireNextFrame");

    struct ReleaseFrameOnExit {
        IDXGIOutputDuplication* duplication = nullptr;
        ~ReleaseFrameOnExit()
        {
            if (duplication != nullptr) {
                duplication->ReleaseFrame();
            }
        }
    } releaseFrame{duplication_.Get()};

    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopTexture;
    ThrowIfFailed(desktopResource.As(&desktopTexture), "IDXGIResource::QueryInterface(ID3D11Texture2D)");

    D3D11_TEXTURE2D_DESC sourceDesc{};
    desktopTexture->GetDesc(&sourceDesc);

    return ReadTextureFrame(desktopTexture.Get(), sourceDesc, frameInfo.LastPresentTime.QuadPart);
}

void DesktopCapturer::CreateDevice(IDXGIAdapter* adapter)
{
    static constexpr D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    D3D_FEATURE_LEVEL selectedFeatureLevel{};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT result = D3D11CreateDevice(
        adapter,
        adapter == nullptr ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &device_,
        &selectedFeatureLevel,
        &context_);

#if defined(_DEBUG)
    if (result == DXGI_ERROR_SDK_COMPONENT_MISSING) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        result = D3D11CreateDevice(
            adapter,
            adapter == nullptr ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            flags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &device_,
            &selectedFeatureLevel,
            &context_);
    }
#endif

    ThrowIfFailed(result, "D3D11CreateDevice");
}

void DesktopCapturer::CreateDuplicationForDisplay(int displayIndex)
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

    int currentDisplay = 0;
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidateAdapter;
        HRESULT adapterResult = factory->EnumAdapters1(adapterIndex, &candidateAdapter);
        if (adapterResult == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        ThrowIfFailed(adapterResult, "IDXGIFactory1::EnumAdapters1");

        for (UINT outputIndex = 0;; ++outputIndex) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            HRESULT outputResult = candidateAdapter->EnumOutputs(outputIndex, &output);
            if (outputResult == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            ThrowIfFailed(outputResult, "IDXGIAdapter1::EnumOutputs");

            if (currentDisplay == displayIndex) {
                CreateDevice(candidateAdapter.Get());

                DetectOutputColorSpace(output.Get());

                Microsoft::WRL::ComPtr<IDXGIOutput5> output5;
                if (SUCCEEDED(output.As(&output5))) {
                    constexpr DXGI_FORMAT hdrFormats[] = {
                        DXGI_FORMAT_R16G16B16A16_FLOAT,
                        DXGI_FORMAT_R10G10B10A2_UNORM,
                    };

                    if (outputHdrActive_) {
                        const HRESULT hdrDuplicateResult = output5->DuplicateOutput1(
                            device_.Get(),
                            0,
                            ARRAYSIZE(hdrFormats),
                            hdrFormats,
                            &duplication_);
                        if (SUCCEEDED(hdrDuplicateResult)) {
                            return;
                        }
                    }

                    constexpr DXGI_FORMAT sdrFormats[] = {
                        DXGI_FORMAT_B8G8R8A8_UNORM,
                    };

                    const HRESULT sdrDuplicateResult = output5->DuplicateOutput1(
                        device_.Get(),
                        0,
                        ARRAYSIZE(sdrFormats),
                        sdrFormats,
                        &duplication_);
                    if (SUCCEEDED(sdrDuplicateResult)) {
                        return;
                    }
                }

                Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
                ThrowIfFailed(output.As(&output1), "IDXGIOutput::QueryInterface(IDXGIOutput1)");
                ThrowIfFailed(output1->DuplicateOutput(device_.Get(), &duplication_), "IDXGIOutput1::DuplicateOutput");
                return;
            }

            ++currentDisplay;
        }
    }

    throw std::out_of_range("Display index was not found");
}

void DesktopCapturer::InitializeWinRt()
{
    const HRESULT result = RoInitialize(RO_INIT_MULTITHREADED);
    if (SUCCEEDED(result)) {
        winrtInitialized_ = true;
        return;
    }
    if (result == RPC_E_CHANGED_MODE) {
        return;
    }

    ThrowIfFailed(result, "RoInitialize");
}

void DesktopCapturer::CreateWindowsGraphicsCaptureForDisplay(int displayIndex)
{
    InitializeWinRt();

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

    int currentDisplay = 0;
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidateAdapter;
        HRESULT adapterResult = factory->EnumAdapters1(adapterIndex, &candidateAdapter);
        if (adapterResult == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        ThrowIfFailed(adapterResult, "IDXGIFactory1::EnumAdapters1");

        for (UINT outputIndex = 0;; ++outputIndex) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            HRESULT outputResult = candidateAdapter->EnumOutputs(outputIndex, &output);
            if (outputResult == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            ThrowIfFailed(outputResult, "IDXGIAdapter1::EnumOutputs");

            if (currentDisplay == displayIndex) {
                DXGI_OUTPUT_DESC outputDesc{};
                ThrowIfFailed(output->GetDesc(&outputDesc), "IDXGIOutput::GetDesc");

                CreateDevice(candidateAdapter.Get());
                DetectOutputColorSpace(output.Get());

                Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
                ThrowIfFailed(device_.As(&dxgiDevice), "ID3D11Device::QueryInterface(IDXGIDevice)");

                winrt::com_ptr<IInspectable> inspectableDevice;
                ThrowIfFailed(
                    CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectableDevice.put()),
                    "CreateDirect3D11DeviceFromDXGIDevice");

                const auto d3dDevice = inspectableDevice.as<direct3d11::IDirect3DDevice>();
                auto itemFactory = winrt::get_activation_factory<capture::GraphicsCaptureItem>();
                const auto itemInterop = itemFactory.as<IGraphicsCaptureItemInterop>();

                capture::GraphicsCaptureItem item{nullptr};
                ThrowIfFailed(
                    itemInterop->CreateForMonitor(
                        outputDesc.Monitor,
                        winrt::guid_of<capture::IGraphicsCaptureItem>(),
                        winrt::put_abi(item)),
                    "IGraphicsCaptureItemInterop::CreateForMonitor");

                const graphics::SizeInt32 captureSize = item.Size();
                const directx::DirectXPixelFormat pixelFormat =
                    outputHdrActive_ && config_.hdrToSdr
                    ? directx::DirectXPixelFormat::R16G16B16A16Float
                    : directx::DirectXPixelFormat::B8G8R8A8UIntNormalized;

                auto state = std::make_unique<WindowsGraphicsCaptureState>();
                state->item = item;
                state->device = d3dDevice;
                state->size = captureSize;
                state->framePool = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                    d3dDevice,
                    pixelFormat,
                    2,
                    captureSize);
                state->session = state->framePool.CreateCaptureSession(item);
                ConfigureWindowsGraphicsCaptureBorder(state->session, config_.wgcBorderRequired);
                state->session.StartCapture();
                wgc_ = std::move(state);
                return;
            }

            ++currentDisplay;
        }
    }

    throw std::out_of_range("Display index was not found");
}

void DesktopCapturer::DetectOutputColorSpace(IDXGIOutput* output)
{
    outputColorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    outputHdrActive_ = false;

    Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
    if (output == nullptr || FAILED(output->QueryInterface(IID_PPV_ARGS(&output6)))) {
        return;
    }

    DXGI_OUTPUT_DESC1 outputDesc{};
    if (FAILED(output6->GetDesc1(&outputDesc))) {
        return;
    }

    outputColorSpace_ = outputDesc.ColorSpace;
    outputHdrActive_ = IsHdrColorSpace(outputDesc.ColorSpace);
}

void DesktopCapturer::EnsureSourceTexture(const D3D11_TEXTURE2D_DESC& sourceDesc)
{
    if (sourceTexture_ &&
        sourceTextureDesc_.Width == sourceDesc.Width &&
        sourceTextureDesc_.Height == sourceDesc.Height &&
        sourceTextureDesc_.Format == sourceDesc.Format) {
        return;
    }

    sourceView_.Reset();
    sourceTexture_.Reset();

    sourceTextureDesc_ = sourceDesc;
    sourceTextureDesc_.Usage = D3D11_USAGE_DEFAULT;
    sourceTextureDesc_.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sourceTextureDesc_.CPUAccessFlags = 0;
    sourceTextureDesc_.MiscFlags = 0;
    sourceTextureDesc_.MipLevels = 1;
    sourceTextureDesc_.ArraySize = 1;
    sourceTextureDesc_.SampleDesc.Count = 1;
    sourceTextureDesc_.SampleDesc.Quality = 0;

    ThrowIfFailed(device_->CreateTexture2D(&sourceTextureDesc_, nullptr, &sourceTexture_), "ID3D11Device::CreateTexture2D(source)");

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    viewDesc.Format = sourceTextureDesc_.Format;
    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.MipLevels = 1;
    ThrowIfFailed(device_->CreateShaderResourceView(sourceTexture_.Get(), &viewDesc, &sourceView_), "ID3D11Device::CreateShaderResourceView");
}

void DesktopCapturer::EnsureScalePipeline()
{
    if (scaleVertexShader_ && scalePixelShader_ && scaleSampler_ && scaleConstants_) {
        return;
    }

    static constexpr const char* shaderSource = R"(
struct VertexOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOut vs_main(uint vertexId : SV_VertexID)
{
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    float2 uvs[3] = {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0)
    };

    VertexOut output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}

Texture2D desktopTexture : register(t0);
SamplerState linearSampler : register(s0);

cbuffer ColorConversion : register(b0)
{
    uint colorConversionMode;
    float hdrSdrWhiteNits;
    float hdrSdrBgraExposure;
    float padding;
};

float3 LinearToSrgb(float3 linearRgb)
{
    linearRgb = max(linearRgb, 0.0);
    float3 low = linearRgb * 12.92;
    float3 high = 1.055 * pow(linearRgb, 1.0 / 2.4) - 0.055;
    return float3(
        linearRgb.r <= 0.0031308 ? low.r : high.r,
        linearRgb.g <= 0.0031308 ? low.g : high.g,
        linearRgb.b <= 0.0031308 ? low.b : high.b);
}

float3 SrgbToLinear(float3 srgb)
{
    srgb = saturate(srgb);
    float3 low = srgb / 12.92;
    float3 high = pow((srgb + 0.055) / 1.055, 2.4);
    return float3(
        srgb.r <= 0.04045 ? low.r : high.r,
        srgb.g <= 0.04045 ? low.g : high.g,
        srgb.b <= 0.04045 ? low.b : high.b);
}

float3 ScRgbToSdr(float3 scRgb)
{
    float whiteScale = max(hdrSdrWhiteNits / 80.0, 0.01);
    float3 normalized = max(scRgb, 0.0) / whiteScale;
    return LinearToSrgb(saturate(normalized));
}

float3 LinearHdrToSdr(float3 linearRgb)
{
    float whiteScale = max(hdrSdrWhiteNits / 80.0, 0.01);
    return LinearToSrgb(saturate(max(linearRgb, 0.0) / whiteScale));
}

float3 PqToNits(float3 pq)
{
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;

    float3 p = pow(saturate(pq), 1.0 / m2);
    float3 numerator = max(p - c1, 0.0);
    float3 denominator = max(c2 - c3 * p, 0.000001);
    return 10000.0 * pow(numerator / denominator, 1.0 / m1);
}

float3 Rec2020ToRec709(float3 rec2020)
{
    return float3(
         1.6605 * rec2020.r - 0.5876 * rec2020.g - 0.0728 * rec2020.b,
        -0.1246 * rec2020.r + 1.1329 * rec2020.g - 0.0083 * rec2020.b,
        -0.0182 * rec2020.r - 0.1006 * rec2020.g + 1.1187 * rec2020.b);
}

float3 PqP2020ToSdr(float3 pqRec2020)
{
    float3 nits2020 = PqToNits(pqRec2020);
    float3 nits709 = Rec2020ToRec709(nits2020);
    float3 linear709 = max(nits709, 0.0) / max(hdrSdrWhiteNits, 1.0);
    return LinearToSrgb(saturate(linear709));
}

float3 HdrDesktopBgraToSdr(float3 sdrRgb)
{
    float exposure = max(hdrSdrBgraExposure, 0.01);
    float3 linearRgb = SrgbToLinear(sdrRgb);
    return LinearToSrgb(saturate(linearRgb * exposure));
}

float4 ps_main(VertexOut input) : SV_Target
{
    float4 color = desktopTexture.Sample(linearSampler, input.uv);
    if (colorConversionMode == 1) {
        color.rgb = ScRgbToSdr(color.rgb);
    } else if (colorConversionMode == 2) {
        color.rgb = LinearToSrgb(color.rgb);
    } else if (colorConversionMode == 3) {
        color.rgb = LinearHdrToSdr(color.rgb);
    } else if (colorConversionMode == 4) {
        color.rgb = PqP2020ToSdr(color.rgb);
    } else if (colorConversionMode == 5) {
        color.rgb = HdrDesktopBgraToSdr(color.rgb);
    }
    return saturate(color);
}
)";

    const auto vertexShader = CompileShader(shaderSource, "vs_main", "vs_4_0");
    const auto pixelShader = CompileShader(shaderSource, "ps_main", "ps_4_0");

    ThrowIfFailed(
        device_->CreateVertexShader(vertexShader->GetBufferPointer(), vertexShader->GetBufferSize(), nullptr, &scaleVertexShader_),
        "ID3D11Device::CreateVertexShader");
    ThrowIfFailed(
        device_->CreatePixelShader(pixelShader->GetBufferPointer(), pixelShader->GetBufferSize(), nullptr, &scalePixelShader_),
        "ID3D11Device::CreatePixelShader");

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0.0f;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    ThrowIfFailed(device_->CreateSamplerState(&samplerDesc, &scaleSampler_), "ID3D11Device::CreateSamplerState");

    D3D11_BUFFER_DESC constantDesc{};
    constantDesc.ByteWidth = sizeof(ScaleConstants);
    constantDesc.Usage = D3D11_USAGE_DEFAULT;
    constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ThrowIfFailed(device_->CreateBuffer(&constantDesc, nullptr, &scaleConstants_), "ID3D11Device::CreateBuffer(scale constants)");
}

void DesktopCapturer::EnsureScaledTexture(const D3D11_TEXTURE2D_DESC& sourceDesc, int width, int height)
{
    if (scaledTexture_ &&
        scaledDesc_.Width == static_cast<UINT>(width) &&
        scaledDesc_.Height == static_cast<UINT>(height) &&
        scaledDesc_.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
        return;
    }

    scaledTarget_.Reset();
    scaledTexture_.Reset();

    scaledDesc_ = sourceDesc;
    scaledDesc_.Width = static_cast<UINT>(width);
    scaledDesc_.Height = static_cast<UINT>(height);
    scaledDesc_.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scaledDesc_.Usage = D3D11_USAGE_DEFAULT;
    scaledDesc_.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    scaledDesc_.CPUAccessFlags = 0;
    scaledDesc_.MiscFlags = 0;
    scaledDesc_.MipLevels = 1;
    scaledDesc_.ArraySize = 1;
    scaledDesc_.SampleDesc.Count = 1;
    scaledDesc_.SampleDesc.Quality = 0;

    ThrowIfFailed(device_->CreateTexture2D(&scaledDesc_, nullptr, &scaledTexture_), "ID3D11Device::CreateTexture2D(scaled)");

    D3D11_RENDER_TARGET_VIEW_DESC targetDesc{};
    targetDesc.Format = scaledDesc_.Format;
    targetDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ThrowIfFailed(device_->CreateRenderTargetView(scaledTexture_.Get(), &targetDesc, &scaledTarget_), "ID3D11Device::CreateRenderTargetView");
}

void DesktopCapturer::EnsureStagingTexture(const D3D11_TEXTURE2D_DESC& outputDesc)
{
    if (stagingTexture_ &&
        stagingDesc_.Width == outputDesc.Width &&
        stagingDesc_.Height == outputDesc.Height &&
        stagingDesc_.Format == outputDesc.Format) {
        return;
    }

    stagingDesc_ = outputDesc;
    stagingDesc_.Usage = D3D11_USAGE_STAGING;
    stagingDesc_.BindFlags = 0;
    stagingDesc_.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc_.MiscFlags = 0;
    stagingDesc_.MipLevels = 1;
    stagingDesc_.ArraySize = 1;
    stagingDesc_.SampleDesc.Count = 1;
    stagingDesc_.SampleDesc.Quality = 0;

    ThrowIfFailed(device_->CreateTexture2D(&stagingDesc_, nullptr, &stagingTexture_), "ID3D11Device::CreateTexture2D");
}

std::optional<CapturedFrame> DesktopCapturer::ReadTextureFrame(
    ID3D11Texture2D* sourceTexture,
    const D3D11_TEXTURE2D_DESC& sourceDesc,
    int64_t presentTimeQpc)
{
    ID3D11Texture2D* outputTexture = ScaleFrameIfNeeded(sourceTexture, sourceDesc);

    D3D11_TEXTURE2D_DESC outputDesc{};
    outputTexture->GetDesc(&outputDesc);
    EnsureStagingTexture(outputDesc);

    context_->CopyResource(stagingTexture_.Get(), outputTexture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    ThrowIfFailed(context_->Map(stagingTexture_.Get(), 0, D3D11_MAP_READ, 0, &mapped), "ID3D11DeviceContext::Map");

    CapturedFrame frame;
    frame.sourceWidth = static_cast<int>(sourceDesc.Width);
    frame.sourceHeight = static_cast<int>(sourceDesc.Height);
    frame.width = static_cast<int>(outputDesc.Width);
    frame.height = static_cast<int>(outputDesc.Height);
    frame.sourceFormat = sourceDesc.Format;
    frame.format = outputDesc.Format;
    frame.displayColorSpace = outputColorSpace_;
    frame.displayHdrActive = outputHdrActive_;
    frame.colorConversionMode = lastColorConversionMode_;
    frame.rowPitch = mapped.RowPitch;
    frame.lastPresentTimeQpc = presentTimeQpc;

    const size_t totalBytes = static_cast<size_t>(mapped.RowPitch) * outputDesc.Height;
    frame.pixels.resize(totalBytes);
    std::memcpy(frame.pixels.data(), mapped.pData, totalBytes);

    context_->Unmap(stagingTexture_.Get(), 0);
    return frame;
}

std::optional<CapturedFrame> DesktopCapturer::TryCaptureWindowsGraphicsFrame(std::chrono::milliseconds timeout)
{
    if (!wgc_) {
        throw std::logic_error("DesktopCapturer::Start must be called before capturing frames");
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        capture::Direct3D11CaptureFrame captureFrame{nullptr};
        try {
            captureFrame = wgc_->framePool.TryGetNextFrame();
        } catch (const winrt::hresult_error& error) {
            throw std::runtime_error("Direct3D11CaptureFramePool::TryGetNextFrame failed: " + HResultMessage(error.code()));
        }

        if (captureFrame) {
            if (!wgc_->skippedInitialFrame) {
                wgc_->skippedInitialFrame = true;
                if (timeout.count() == 0 || std::chrono::steady_clock::now() >= deadline) {
                    return std::nullopt;
                }
                Sleep(1);
                continue;
            }

            const graphics::SizeInt32 contentSize = captureFrame.ContentSize();
            if (contentSize.Width != wgc_->size.Width || contentSize.Height != wgc_->size.Height) {
                wgc_->size = contentSize;
                wgc_->framePool.Recreate(
                    wgc_->device,
                    outputHdrActive_ && config_.hdrToSdr
                        ? directx::DirectXPixelFormat::R16G16B16A16Float
                        : directx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    2,
                    contentSize);
            }

            const auto surface = captureFrame.Surface();
            static constexpr GUID IID_IDirect3DDxgiInterfaceAccess = {
                0xA9B3D012,
                0x3DF2,
                0x4EE3,
                {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1},
            };
            winrt::com_ptr<IDirect3DDxgiInterfaceAccess> dxgiAccess;
            auto* surfaceUnknown = static_cast<IUnknown*>(winrt::get_abi(surface));
            ThrowIfFailed(
                surfaceUnknown->QueryInterface(IID_IDirect3DDxgiInterfaceAccess, dxgiAccess.put_void()),
                "IDirect3DSurface::QueryInterface(IDirect3DDxgiInterfaceAccess)");
            Microsoft::WRL::ComPtr<ID3D11Texture2D> frameTexture;
            ThrowIfFailed(dxgiAccess->GetInterface(IID_PPV_ARGS(&frameTexture)), "IDirect3DDxgiInterfaceAccess::GetInterface");

            D3D11_TEXTURE2D_DESC sourceDesc{};
            frameTexture->GetDesc(&sourceDesc);
            return ReadTextureFrame(frameTexture.Get(), sourceDesc, 0);
        }

        if (timeout.count() == 0 || std::chrono::steady_clock::now() >= deadline) {
            return std::nullopt;
        }

        Sleep(1);
    }
}

ID3D11Texture2D* DesktopCapturer::ScaleFrameIfNeeded(ID3D11Texture2D* sourceTexture, const D3D11_TEXTURE2D_DESC& sourceDesc)
{
    lastColorConversionMode_ = 0;
    const int outputWidth = config_.targetWidth > 0 ? config_.targetWidth : static_cast<int>(sourceDesc.Width);
    const int outputHeight = config_.targetHeight > 0 ? config_.targetHeight : static_cast<int>(sourceDesc.Height);
    const uint32_t colorConversionMode = ColorConversionMode(sourceDesc.Format, outputColorSpace_, config_.hdrToSdr, outputHdrActive_);
    const bool needsColorTransform = colorConversionMode != 0;
    const bool needsTransform =
        outputWidth != static_cast<int>(sourceDesc.Width) ||
        outputHeight != static_cast<int>(sourceDesc.Height) ||
        !IsBgra8Format(sourceDesc.Format) ||
        needsColorTransform;

    if (!needsTransform) {
        return sourceTexture;
    }

    EnsureSourceTexture(sourceDesc);
    EnsureScalePipeline();
    EnsureScaledTexture(sourceDesc, outputWidth, outputHeight);

    context_->CopyResource(sourceTexture_.Get(), sourceTexture);

    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(outputWidth);
    viewport.Height = static_cast<float>(outputHeight);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    ID3D11RenderTargetView* renderTargets[] = {scaledTarget_.Get()};
    ID3D11ShaderResourceView* shaderResources[] = {sourceView_.Get()};
    ID3D11SamplerState* samplers[] = {scaleSampler_.Get()};
    ID3D11Buffer* constantBuffers[] = {scaleConstants_.Get()};

    ScaleConstants constants;
    constants.colorConversionMode = colorConversionMode;
    constants.hdrSdrWhiteNits = std::clamp(config_.hdrSdrWhiteNits, 80.0f, 1000.0f);
    constants.hdrSdrBgraExposure = std::clamp(config_.hdrSdrBgraExposure, 0.25f, 2.0f);
    lastColorConversionMode_ = constants.colorConversionMode;
    context_->UpdateSubresource(scaleConstants_.Get(), 0, nullptr, &constants, 0, 0);

    context_->OMSetRenderTargets(1, renderTargets, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(scaleVertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(scalePixelShader_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, shaderResources);
    context_->PSSetSamplers(0, 1, samplers);
    context_->PSSetConstantBuffers(0, 1, constantBuffers);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullShaderResources[] = {nullptr};
    ID3D11RenderTargetView* nullRenderTargets[] = {nullptr};
    ID3D11Buffer* nullConstantBuffers[] = {nullptr};
    context_->PSSetShaderResources(0, 1, nullShaderResources);
    context_->PSSetConstantBuffers(0, 1, nullConstantBuffers);
    context_->OMSetRenderTargets(1, nullRenderTargets, nullptr);

    return scaledTexture_.Get();
}

std::string Narrow(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);

    if (size <= 0) {
        return {};
    }

    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        size,
        nullptr,
        nullptr);
    return result;
}

std::string HResultMessage(HRESULT hr)
{
    char* buffer = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer),
        0,
        nullptr);

    if (size > 0 && buffer != nullptr) {
        std::string message(buffer, size);
        LocalFree(buffer);

        while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
            message.pop_back();
        }

        return message;
    }

    std::ostringstream stream;
    stream << "HRESULT 0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return stream.str();
}

const char* DxgiFormatName(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        return "B8G8R8X8_UNORM";
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return "B8G8R8X8_UNORM_SRGB";
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return "R11G11B10_FLOAT";
    case DXGI_FORMAT_NV12:
        return "NV12";
    case DXGI_FORMAT_UNKNOWN:
        return "UNKNOWN";
    default:
        return "OTHER";
    }
}

const char* DxgiColorSpaceName(DXGI_COLOR_SPACE_TYPE colorSpace)
{
    switch (colorSpace) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        return "RGB_FULL_G22_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return "RGB_FULL_G10_NONE_P709";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return "RGB_FULL_G2084_NONE_P2020";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
        return "RGB_STUDIO_G2084_NONE_P2020";
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020:
        return "RGB_STUDIO_G22_NONE_P2020";
    default:
        return "OTHER";
    }
}

const char* CaptureColorConversionName(uint32_t mode)
{
    switch (mode) {
    case 0:
        return "none";
    case 1:
        return "scRGB_to_SDR";
    case 2:
        return "sRGB_to_SDR";
    case 3:
        return "linear_HDR_to_SDR";
    case 4:
        return "PQ_Rec2020_to_SDR";
    case 5:
        return "HDR_desktop_BGRA_exposure";
    default:
        return "unknown";
    }
}

} // namespace screenshare
