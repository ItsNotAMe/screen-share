#include "capture/DesktopCapturer.h"

#include <Windows.h>
#include <d3dcompiler.h>

#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace screenshare {
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
    CreateDuplicationForDisplay(config.displayIndex);
}

void DesktopCapturer::Stop()
{
    duplication_.Reset();
    sourceTexture_.Reset();
    sourceView_.Reset();
    scaledTexture_.Reset();
    scaledTarget_.Reset();
    stagingTexture_.Reset();
    scaleVertexShader_.Reset();
    scalePixelShader_.Reset();
    scaleSampler_.Reset();
    context_.Reset();
    device_.Reset();
    std::memset(&sourceTextureDesc_, 0, sizeof(sourceTextureDesc_));
    std::memset(&scaledDesc_, 0, sizeof(scaledDesc_));
    std::memset(&stagingDesc_, 0, sizeof(stagingDesc_));
}

std::optional<CapturedFrame> DesktopCapturer::TryCaptureFrame(std::chrono::milliseconds timeout)
{
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

    ID3D11Texture2D* outputTexture = ScaleFrameIfNeeded(desktopTexture.Get(), sourceDesc);

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
    frame.format = outputDesc.Format;
    frame.rowPitch = mapped.RowPitch;
    frame.lastPresentTimeQpc = frameInfo.LastPresentTime.QuadPart;

    const size_t totalBytes = static_cast<size_t>(mapped.RowPitch) * outputDesc.Height;
    frame.pixels.resize(totalBytes);
    std::memcpy(frame.pixels.data(), mapped.pData, totalBytes);

    context_->Unmap(stagingTexture_.Get(), 0);
    return frame;
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
    if (scaleVertexShader_ && scalePixelShader_ && scaleSampler_) {
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

float4 ps_main(VertexOut input) : SV_Target
{
    return desktopTexture.Sample(linearSampler, input.uv);
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

ID3D11Texture2D* DesktopCapturer::ScaleFrameIfNeeded(ID3D11Texture2D* sourceTexture, const D3D11_TEXTURE2D_DESC& sourceDesc)
{
    const int outputWidth = config_.targetWidth > 0 ? config_.targetWidth : static_cast<int>(sourceDesc.Width);
    const int outputHeight = config_.targetHeight > 0 ? config_.targetHeight : static_cast<int>(sourceDesc.Height);
    const bool needsTransform =
        outputWidth != static_cast<int>(sourceDesc.Width) ||
        outputHeight != static_cast<int>(sourceDesc.Height) ||
        !IsBgra8Format(sourceDesc.Format);

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

    context_->OMSetRenderTargets(1, renderTargets, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(scaleVertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(scalePixelShader_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, shaderResources);
    context_->PSSetSamplers(0, 1, samplers);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullShaderResources[] = {nullptr};
    ID3D11RenderTargetView* nullRenderTargets[] = {nullptr};
    context_->PSSetShaderResources(0, 1, nullShaderResources);
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

} // namespace screenshare
