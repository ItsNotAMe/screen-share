#include "render/Nv12D3D11Presenter.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace screenshare {
namespace {

struct PreviewConstants {
    float lumaTexelWidth = 1.0f;
    float lumaTexelHeight = 1.0f;
    float sharpenAmount = 0.0f;
    float padding = 0.0f;
};

std::string HResultMessageLocal(HRESULT hr)
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

void ThrowIfFailed(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(operation) + " failed: " + HResultMessageLocal(hr));
    }
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
        std::string message = HResultMessageLocal(result);
        if (errors) {
            message += ": ";
            message.append(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        }
        throw std::runtime_error("D3DCompile failed for " + std::string(entryPoint) + ": " + message);
    }

    return shader;
}

std::uint32_t ClampDimension(std::uint32_t value)
{
    return std::max<std::uint32_t>(1, value);
}

void ValidateNv12Frame(const Nv12D3D11Presenter::FrameView& frame)
{
    if (frame.width <= 0 || frame.height <= 0 || frame.data == nullptr) {
        throw std::runtime_error("Embedded preview frame is missing NV12 data");
    }
    if ((frame.width % 2) != 0 || (frame.height % 2) != 0) {
        throw std::runtime_error("Embedded preview frame dimensions must be even for NV12");
    }

    const std::uint64_t lumaBytes = static_cast<std::uint64_t>(frame.width) * static_cast<std::uint64_t>(frame.height);
    const std::uint64_t requiredBytes = lumaBytes + lumaBytes / 2;
    if (requiredBytes > std::numeric_limits<std::size_t>::max() ||
        frame.dataSize < static_cast<std::size_t>(requiredBytes)) {
        throw std::runtime_error("Embedded preview NV12 frame data is too small");
    }
}

void SetSwapChainSdrColorSpace(IDXGISwapChain* swapChain)
{
    if (swapChain == nullptr) {
        return;
    }

    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3)))) {
        return;
    }

    UINT colorSpaceSupport = 0;
    if (FAILED(swapChain3->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &colorSpaceSupport))) {
        return;
    }
    if ((colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0) {
        return;
    }
    static_cast<void>(swapChain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709));
}

void DisableDxgiDefaultAltEnter(IDXGISwapChain* swapChain, HWND hwnd)
{
    if (swapChain == nullptr || hwnd == nullptr) {
        return;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
    if (FAILED(swapChain->GetParent(IID_PPV_ARGS(&factory)))) {
        return;
    }
    static_cast<void>(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));
}

} // namespace

struct Nv12D3D11Presenter::Impl {
    HWND hwnd = nullptr;
    std::uint32_t clientWidth = 1;
    std::uint32_t clientHeight = 1;
    bool swapChainResizePending = false;
    bool linearSampling = true;
    Nv12D3D11Presenter::ScaleMode scaleMode = Nv12D3D11Presenter::ScaleMode::Fit;

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> lumaTexture;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> chromaTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lumaView;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> chromaView;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> previewConstants;
    D3D11_TEXTURE2D_DESC lumaDesc{};
    D3D11_TEXTURE2D_DESC chromaDesc{};
    int frameWidth = 0;
    int frameHeight = 0;

    void ResetSwapChainResources()
    {
        renderTarget.Reset();
        swapChain.Reset();
        lumaView.Reset();
        chromaView.Reset();
        lumaTexture.Reset();
        chromaTexture.Reset();
        lumaDesc = {};
        chromaDesc = {};
        frameWidth = 0;
        frameHeight = 0;
    }

    void ResetDevice()
    {
        ResetSwapChainResources();
        previewConstants.Reset();
        sampler.Reset();
        pixelShader.Reset();
        vertexShader.Reset();
        context.Reset();
        device.Reset();
    }

    void UpdateClientSize()
    {
        RECT clientRect{};
        if (hwnd == nullptr || GetClientRect(hwnd, &clientRect) == 0) {
            clientRect.right = 1;
            clientRect.bottom = 1;
        }
        const auto width = ClampDimension(static_cast<std::uint32_t>(clientRect.right - clientRect.left));
        const auto height = ClampDimension(static_cast<std::uint32_t>(clientRect.bottom - clientRect.top));
        if (clientWidth == width && clientHeight == height) {
            return;
        }
        clientWidth = width;
        clientHeight = height;
        if (swapChain) {
            // A native child window can change size while its Qt page is hidden,
            // without a usable resize notification reaching the presentation
            // worker. Keep the actual DXGI buffers tied to GetClientRect rather
            // than treating the cached client dimensions as proof that they
            // already match.
            swapChainResizePending = true;
        }
    }

    HRESULT CreateSwapChainWithEffect(DXGI_SWAP_EFFECT swapEffect, UINT bufferCount)
    {
        DXGI_SWAP_CHAIN_DESC swapChainDesc{};
        swapChainDesc.BufferDesc.Width = clientWidth;
        swapChainDesc.BufferDesc.Height = clientHeight;
        swapChainDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapChainDesc.BufferDesc.RefreshRate.Numerator = 0;
        swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = bufferCount;
        swapChainDesc.OutputWindow = hwnd;
        swapChainDesc.Windowed = TRUE;
        swapChainDesc.SwapEffect = swapEffect;

        constexpr D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
        };
        D3D_FEATURE_LEVEL selectedFeatureLevel{};
        constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

        HRESULT result = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            featureLevels,
            static_cast<UINT>(std::size(featureLevels)),
            D3D11_SDK_VERSION,
            &swapChainDesc,
            &swapChain,
            &device,
            &selectedFeatureLevel,
            &context);

        if (FAILED(result)) {
            result = D3D11CreateDeviceAndSwapChain(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                flags,
                featureLevels,
                static_cast<UINT>(std::size(featureLevels)),
                D3D11_SDK_VERSION,
                &swapChainDesc,
                &swapChain,
                &device,
                &selectedFeatureLevel,
                &context);
        }

        return result;
    }

    void CreateDeviceAndSwapChain()
    {
        if (device || hwnd == nullptr) {
            return;
        }

        UpdateClientSize();
        HRESULT result = CreateSwapChainWithEffect(DXGI_SWAP_EFFECT_FLIP_DISCARD, 2);
        if (FAILED(result)) {
            ResetDevice();
            result = CreateSwapChainWithEffect(DXGI_SWAP_EFFECT_DISCARD, 1);
        }
        ThrowIfFailed(result, "D3D11CreateDeviceAndSwapChain(embedded preview)");
        DisableDxgiDefaultAltEnter(swapChain.Get(), hwnd);
        SetSwapChainSdrColorSpace(swapChain.Get());
        EnsureRenderTarget();
        swapChainResizePending = false;
    }

    void ResizeSwapChainIfNeeded()
    {
        if (!swapChain || !swapChainResizePending || clientWidth == 0 || clientHeight == 0) {
            return;
        }

        renderTarget.Reset();
        ID3D11RenderTargetView* nullRenderTargets[] = {nullptr};
        context->OMSetRenderTargets(1, nullRenderTargets, nullptr);
        ThrowIfFailed(
            swapChain->ResizeBuffers(0, clientWidth, clientHeight, DXGI_FORMAT_UNKNOWN, 0),
            "IDXGISwapChain::ResizeBuffers(embedded preview)");
        SetSwapChainSdrColorSpace(swapChain.Get());
        swapChainResizePending = false;
        EnsureRenderTarget();
    }

    void EnsureRenderTarget()
    {
        if (renderTarget || !swapChain) {
            return;
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        ThrowIfFailed(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), "IDXGISwapChain::GetBuffer(embedded preview)");
        ThrowIfFailed(
            device->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget),
            "ID3D11Device::CreateRenderTargetView(embedded preview)");
    }

    void EnsurePipeline()
    {
        if (vertexShader && pixelShader && sampler && previewConstants) {
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

Texture2D lumaTexture : register(t0);
Texture2D chromaTexture : register(t1);
SamplerState previewSampler : register(s0);

cbuffer PreviewConstants : register(b0)
{
    float lumaTexelWidth;
    float lumaTexelHeight;
    float sharpenAmount;
    float padding;
};

float SharpenLuma(float2 uv, float y)
{
    if (sharpenAmount <= 0.0001) {
        return y;
    }

    float2 texel = float2(lumaTexelWidth, lumaTexelHeight);
    float neighbor =
        lumaTexture.Sample(previewSampler, uv + float2(-texel.x, 0.0)).r +
        lumaTexture.Sample(previewSampler, uv + float2( texel.x, 0.0)).r +
        lumaTexture.Sample(previewSampler, uv + float2(0.0, -texel.y)).r +
        lumaTexture.Sample(previewSampler, uv + float2(0.0,  texel.y)).r;
    neighbor *= 0.25;
    return saturate(y + (y - neighbor) * sharpenAmount);
}

float4 ps_main(VertexOut input) : SV_Target
{
    float y = lumaTexture.Sample(previewSampler, input.uv).r;
    y = SharpenLuma(input.uv, y);
    float2 uv = chromaTexture.Sample(previewSampler, input.uv).rg;

    float c = max(0.0, y - (16.0 / 255.0));
    float u = uv.x - (128.0 / 255.0);
    float v = uv.y - (128.0 / 255.0);

    float3 rgb;
    rgb.r = 1.16438356 * c + 1.79274107 * v;
    rgb.g = 1.16438356 * c - 0.21324861 * u - 0.53290933 * v;
    rgb.b = 1.16438356 * c + 2.11240179 * u;
    return float4(saturate(rgb), 1.0);
}
)";

        if (!vertexShader) {
            const auto compiledVertexShader = CompileShader(shaderSource, "vs_main", "vs_4_0");
            ThrowIfFailed(
                device->CreateVertexShader(
                    compiledVertexShader->GetBufferPointer(),
                    compiledVertexShader->GetBufferSize(),
                    nullptr,
                    &vertexShader),
                "ID3D11Device::CreateVertexShader(embedded preview)");
        }
        if (!pixelShader) {
            const auto compiledPixelShader = CompileShader(shaderSource, "ps_main", "ps_4_0");
            ThrowIfFailed(
                device->CreatePixelShader(
                    compiledPixelShader->GetBufferPointer(),
                    compiledPixelShader->GetBufferSize(),
                    nullptr,
                    &pixelShader),
                "ID3D11Device::CreatePixelShader(embedded preview)");
        }

        if (!sampler) {
            D3D11_SAMPLER_DESC samplerDesc{};
            samplerDesc.Filter = linearSampling ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
            samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
            samplerDesc.MinLOD = 0.0f;
            samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
            ThrowIfFailed(
                device->CreateSamplerState(&samplerDesc, &sampler),
                "ID3D11Device::CreateSamplerState(embedded preview)");
        }

        if (!previewConstants) {
            D3D11_BUFFER_DESC constantDesc{};
            constantDesc.ByteWidth = sizeof(PreviewConstants);
            constantDesc.Usage = D3D11_USAGE_DEFAULT;
            constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            ThrowIfFailed(
                device->CreateBuffer(&constantDesc, nullptr, &previewConstants),
                "ID3D11Device::CreateBuffer(embedded preview constants)");
        }
    }

    void EnsureFrameTextures(int width, int height)
    {
        if (lumaTexture &&
            chromaTexture &&
            lumaDesc.Width == static_cast<UINT>(width) &&
            lumaDesc.Height == static_cast<UINT>(height) &&
            chromaDesc.Width == static_cast<UINT>(width / 2) &&
            chromaDesc.Height == static_cast<UINT>(height / 2)) {
            return;
        }

        lumaView.Reset();
        chromaView.Reset();
        lumaTexture.Reset();
        chromaTexture.Reset();

        lumaDesc = {};
        lumaDesc.Width = static_cast<UINT>(width);
        lumaDesc.Height = static_cast<UINT>(height);
        lumaDesc.MipLevels = 1;
        lumaDesc.ArraySize = 1;
        lumaDesc.Format = DXGI_FORMAT_R8_UNORM;
        lumaDesc.SampleDesc.Count = 1;
        lumaDesc.SampleDesc.Quality = 0;
        lumaDesc.Usage = D3D11_USAGE_DEFAULT;
        lumaDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        chromaDesc = lumaDesc;
        chromaDesc.Width = static_cast<UINT>(width / 2);
        chromaDesc.Height = static_cast<UINT>(height / 2);
        chromaDesc.Format = DXGI_FORMAT_R8G8_UNORM;

        ThrowIfFailed(device->CreateTexture2D(&lumaDesc, nullptr, &lumaTexture), "ID3D11Device::CreateTexture2D(embedded luma)");
        ThrowIfFailed(device->CreateTexture2D(&chromaDesc, nullptr, &chromaTexture), "ID3D11Device::CreateTexture2D(embedded chroma)");

        D3D11_SHADER_RESOURCE_VIEW_DESC lumaViewDesc{};
        lumaViewDesc.Format = lumaDesc.Format;
        lumaViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        lumaViewDesc.Texture2D.MipLevels = 1;

        D3D11_SHADER_RESOURCE_VIEW_DESC chromaViewDesc{};
        chromaViewDesc.Format = chromaDesc.Format;
        chromaViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        chromaViewDesc.Texture2D.MipLevels = 1;

        ThrowIfFailed(
            device->CreateShaderResourceView(lumaTexture.Get(), &lumaViewDesc, &lumaView),
            "ID3D11Device::CreateShaderResourceView(embedded luma)");
        ThrowIfFailed(
            device->CreateShaderResourceView(chromaTexture.Get(), &chromaViewDesc, &chromaView),
            "ID3D11Device::CreateShaderResourceView(embedded chroma)");
    }

    D3D11_VIEWPORT ComputeViewport() const
    {
        D3D11_VIEWPORT viewport{};
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        if (clientWidth == 0 || clientHeight == 0 || frameWidth <= 0 || frameHeight <= 0) {
            viewport.Width = 1.0f;
            viewport.Height = 1.0f;
            return viewport;
        }

        const float targetWidth = static_cast<float>(clientWidth);
        const float targetHeight = static_cast<float>(clientHeight);
        const float sourceWidth = static_cast<float>(frameWidth);
        const float sourceHeight = static_cast<float>(frameHeight);
        float scale = std::min(targetWidth / sourceWidth, targetHeight / sourceHeight);
        if (scaleMode == Nv12D3D11Presenter::ScaleMode::OriginalSize) {
            scale = std::min(scale, 1.0f);
        }

        viewport.Width = std::max(1.0f, sourceWidth * scale);
        viewport.Height = std::max(1.0f, sourceHeight * scale);
        viewport.TopLeftX = (targetWidth - viewport.Width) * 0.5f;
        viewport.TopLeftY = (targetHeight - viewport.Height) * 0.5f;
        return viewport;
    }

    void Render()
    {
        if (!swapChain || hwnd == nullptr || IsWindow(hwnd) == 0 || IsIconic(hwnd) != FALSE) {
            return;
        }

        UpdateClientSize();
        ResizeSwapChainIfNeeded();
        EnsureRenderTarget();

        const float clearColor[] = {0.067f, 0.082f, 0.078f, 1.0f};
        context->ClearRenderTargetView(renderTarget.Get(), clearColor);

        if (!lumaView || !chromaView) {
            ThrowIfFailed(swapChain->Present(0, 0), "IDXGISwapChain::Present(embedded preview clear)");
            return;
        }

        EnsurePipeline();
        const D3D11_VIEWPORT viewport = ComputeViewport();
        const float sourceWidth = static_cast<float>(std::max(frameWidth, 1));
        const float sourceHeight = static_cast<float>(std::max(frameHeight, 1));
        const float displayScale = viewport.Width / sourceWidth;
        PreviewConstants constants;
        constants.lumaTexelWidth = 1.0f / sourceWidth;
        constants.lumaTexelHeight = 1.0f / sourceHeight;
        constants.sharpenAmount = displayScale > 1.01f
            ? std::min(0.22f, 0.06f + (displayScale - 1.0f) * 0.35f)
            : 0.0f;
        context->UpdateSubresource(previewConstants.Get(), 0, nullptr, &constants, 0, 0);

        ID3D11RenderTargetView* renderTargets[] = {renderTarget.Get()};
        ID3D11ShaderResourceView* shaderResources[] = {lumaView.Get(), chromaView.Get()};
        ID3D11SamplerState* samplers[] = {sampler.Get()};
        ID3D11Buffer* constantBuffers[] = {previewConstants.Get()};

        context->OMSetRenderTargets(1, renderTargets, nullptr);
        context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertexShader.Get(), nullptr, 0);
        context->PSSetShader(pixelShader.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 2, shaderResources);
        context->PSSetSamplers(0, 1, samplers);
        context->PSSetConstantBuffers(0, 1, constantBuffers);
        context->Draw(3, 0);

        ID3D11ShaderResourceView* nullShaderResources[] = {nullptr, nullptr};
        ID3D11RenderTargetView* nullRenderTargets[] = {nullptr};
        ID3D11Buffer* nullConstantBuffers[] = {nullptr};
        context->PSSetShaderResources(0, 2, nullShaderResources);
        context->PSSetConstantBuffers(0, 1, nullConstantBuffers);
        context->OMSetRenderTargets(1, nullRenderTargets, nullptr);

        ThrowIfFailed(swapChain->Present(0, 0), "IDXGISwapChain::Present(embedded preview)");
    }
};

Nv12D3D11Presenter::Nv12D3D11Presenter()
    : impl_(new Impl)
{
}

Nv12D3D11Presenter::~Nv12D3D11Presenter()
{
    delete impl_;
    impl_ = nullptr;
}

void Nv12D3D11Presenter::Attach(HWND hwnd)
{
    if (impl_->hwnd == hwnd && impl_->device) {
        return;
    }

    if (impl_->hwnd != hwnd) {
        impl_->ResetDevice();
        impl_->hwnd = hwnd;
    }
    impl_->CreateDeviceAndSwapChain();
    impl_->EnsurePipeline();
}

void Nv12D3D11Presenter::Resize(std::uint32_t width, std::uint32_t height)
{
    const std::uint32_t clampedWidth = ClampDimension(width);
    const std::uint32_t clampedHeight = ClampDimension(height);
    if (impl_->clientWidth == clampedWidth &&
        impl_->clientHeight == clampedHeight &&
        !impl_->swapChainResizePending) {
        return;
    }

    impl_->clientWidth = clampedWidth;
    impl_->clientHeight = clampedHeight;
    impl_->swapChainResizePending = true;
    if (impl_->swapChain) {
        impl_->Render();
    }
}

void Nv12D3D11Presenter::SetScaleMode(ScaleMode mode)
{
    if (impl_->scaleMode == mode) {
        return;
    }
    impl_->scaleMode = mode;
    if (impl_->swapChain) {
        impl_->Render();
    }
}

void Nv12D3D11Presenter::SetLinearSampling(bool enabled)
{
    if (impl_->linearSampling == enabled) {
        return;
    }
    impl_->linearSampling = enabled;
    impl_->sampler.Reset();
    if (impl_->swapChain) {
        impl_->Render();
    }
}

void Nv12D3D11Presenter::Present(const FrameView& frame)
{
    ValidateNv12Frame(frame);
    if (impl_->hwnd == nullptr) {
        throw std::runtime_error("Embedded preview presenter has no target window");
    }

    impl_->CreateDeviceAndSwapChain();
    impl_->EnsurePipeline();
    impl_->EnsureFrameTextures(frame.width, frame.height);

    const std::size_t lumaBytes = static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
    const auto* luma = frame.data;
    const auto* chroma = frame.data + lumaBytes;

    impl_->context->UpdateSubresource(
        impl_->lumaTexture.Get(),
        0,
        nullptr,
        luma,
        static_cast<UINT>(frame.width),
        0);
    impl_->context->UpdateSubresource(
        impl_->chromaTexture.Get(),
        0,
        nullptr,
        chroma,
        static_cast<UINT>(frame.width),
        0);

    impl_->frameWidth = frame.width;
    impl_->frameHeight = frame.height;
    impl_->Render();
    ++framesPresented_;
}

void Nv12D3D11Presenter::Clear()
{
    impl_->lumaView.Reset();
    impl_->chromaView.Reset();
    impl_->lumaTexture.Reset();
    impl_->chromaTexture.Reset();
    impl_->lumaDesc = {};
    impl_->chromaDesc = {};
    impl_->frameWidth = 0;
    impl_->frameHeight = 0;
    if (impl_->swapChain) {
        impl_->Render();
    }
}

void Nv12D3D11Presenter::Reset()
{
    impl_->ResetDevice();
}

} // namespace screenshare
