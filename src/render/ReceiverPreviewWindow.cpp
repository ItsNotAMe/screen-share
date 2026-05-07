#include "render/ReceiverPreviewWindow.h"

#include "video/Nv12Convert.h"

#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>

namespace screenshare {
namespace {

constexpr const wchar_t* WindowClassName = L"ScreenShareReceiverPreviewWindow";

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
            message.append(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }

        throw std::runtime_error("D3DCompile failed for " + std::string(entryPoint) + ": " + message);
    }

    return shader;
}

void RegisterPreviewWindowClass()
{
    static bool registered = false;
    if (registered) {
        return;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = ReceiverPreviewWindow::StaticWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.lpszClassName = WindowClassName;

    if (RegisterClassExW(&windowClass) == 0) {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("RegisterClassExW failed: " + std::to_string(error));
        }
    }

    registered = true;
}

uint32_t ClampDimension(int value)
{
    return static_cast<uint32_t>(std::max(1, value));
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

SIZE FitFrameToWorkArea(HWND hwnd, int width, int height)
{
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr || GetMonitorInfoW(monitor, &monitorInfo) == 0) {
        return SIZE{width, height};
    }

    const int workWidth = std::max(320, static_cast<int>(monitorInfo.rcWork.right - monitorInfo.rcWork.left - 80));
    const int workHeight = std::max(240, static_cast<int>(monitorInfo.rcWork.bottom - monitorInfo.rcWork.top - 120));
    double scale = 1.0;
    scale = std::min(scale, static_cast<double>(workWidth) / static_cast<double>(width));
    scale = std::min(scale, static_cast<double>(workHeight) / static_cast<double>(height));

    return SIZE{
        std::max(320, static_cast<int>(std::round(static_cast<double>(width) * scale))),
        std::max(180, static_cast<int>(std::round(static_cast<double>(height) * scale))),
    };
}

} // namespace

ReceiverPreviewWindow::ReceiverPreviewWindow() = default;

ReceiverPreviewWindow::~ReceiverPreviewWindow()
{
    if (hwnd_ != nullptr) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void ReceiverPreviewWindow::Show()
{
    EnsureWindow(960, 540);
}

bool ReceiverPreviewWindow::PumpMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
        if (message.message == WM_QUIT) {
            closeRequested_ = true;
            return false;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return !closeRequested_;
}

void ReceiverPreviewWindow::PresentFrame(const DecodedFrameInfo& frame)
{
    if (closeRequested_) {
        return;
    }
    if (frame.width <= 0 || frame.height <= 0) {
        throw std::runtime_error("Decoded preview frame dimensions are not available");
    }

    EnsureWindow(frame.width, frame.height);
    SizeWindowForFirstFrame(frame.width, frame.height);
    ConvertNv12ToBgra(frame.data.data(), frame.data.size(), frame.width, frame.height, bgraPixels_);
    EnsureFrameTexture(frame.width, frame.height);

    context_->UpdateSubresource(
        frameTexture_.Get(),
        0,
        nullptr,
        bgraPixels_.data(),
        static_cast<UINT>(frame.width * 4),
        0);

    frameWidth_ = frame.width;
    frameHeight_ = frame.height;
    Render();
    ++framesPresented_;
}

LRESULT CALLBACK ReceiverPreviewWindow::StaticWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    ReceiverPreviewWindow* window = nullptr;
    if (message == WM_NCCREATE) {
        const auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = static_cast<ReceiverPreviewWindow*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->hwnd_ = hwnd;
    } else {
        window = reinterpret_cast<ReceiverPreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (window != nullptr) {
        return window->WindowProc(message, wParam, lParam);
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT ReceiverPreviewWindow::WindowProc(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CLOSE:
        closeRequested_ = true;
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        closeRequested_ = true;
        hwnd_ = nullptr;
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            clientWidth_ = ClampDimension(LOWORD(lParam));
            clientHeight_ = ClampDimension(HIWORD(lParam));
            swapChainResizePending_ = true;
        }
        return 0;
    default:
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }
}

void ReceiverPreviewWindow::EnsureWindow(int preferredWidth, int preferredHeight)
{
    if (hwnd_ != nullptr) {
        return;
    }

    RegisterPreviewWindowClass();

    RECT windowRect{
        0,
        0,
        std::max(320, preferredWidth),
        std::max(180, preferredHeight),
    };
    AdjustWindowRectEx(&windowRect, WS_OVERLAPPEDWINDOW, FALSE, 0);

    hwnd_ = CreateWindowExW(
        0,
        WindowClassName,
        L"ScreenShare Receiver Preview",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        this);

    if (hwnd_ == nullptr) {
        throw std::runtime_error("CreateWindowExW failed: " + std::to_string(GetLastError()));
    }

    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    UpdateClientSize();
    CreateDeviceAndSwapChain();
    EnsurePipeline();
}

void ReceiverPreviewWindow::CreateDeviceAndSwapChain()
{
    if (device_) {
        return;
    }

    DXGI_SWAP_CHAIN_DESC swapChainDesc{};
    swapChainDesc.BufferDesc.Width = clientWidth_;
    swapChainDesc.BufferDesc.Height = clientHeight_;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapChainDesc.BufferDesc.RefreshRate.Numerator = 0;
    swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = 2;
    swapChainDesc.OutputWindow = hwnd_;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    constexpr D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
    };

    D3D_FEATURE_LEVEL selectedFeatureLevel{};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        featureLevels,
        static_cast<UINT>(std::size(featureLevels)),
        D3D11_SDK_VERSION,
        &swapChainDesc,
        &swapChain_,
        &device_,
        &selectedFeatureLevel,
        &context_);

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
            &swapChain_,
            &device_,
            &selectedFeatureLevel,
            &context_);
    }

    ThrowIfFailed(result, "D3D11CreateDeviceAndSwapChain(receiver preview)");
    SetSwapChainSdrColorSpace(swapChain_.Get());
    EnsureRenderTarget();
}

void ReceiverPreviewWindow::UpdateClientSize()
{
    RECT clientRect{};
    if (hwnd_ == nullptr || GetClientRect(hwnd_, &clientRect) == 0) {
        clientWidth_ = 1;
        clientHeight_ = 1;
        return;
    }

    clientWidth_ = ClampDimension(clientRect.right - clientRect.left);
    clientHeight_ = ClampDimension(clientRect.bottom - clientRect.top);
}

void ReceiverPreviewWindow::ResizeSwapChainIfNeeded()
{
    if (!swapChain_ || !swapChainResizePending_ || clientWidth_ == 0 || clientHeight_ == 0) {
        return;
    }

    renderTarget_.Reset();
    ID3D11RenderTargetView* nullRenderTargets[] = {nullptr};
    context_->OMSetRenderTargets(1, nullRenderTargets, nullptr);
    ThrowIfFailed(
        swapChain_->ResizeBuffers(0, clientWidth_, clientHeight_, DXGI_FORMAT_UNKNOWN, 0),
        "IDXGISwapChain::ResizeBuffers(receiver preview)");
    SetSwapChainSdrColorSpace(swapChain_.Get());
    swapChainResizePending_ = false;
    EnsureRenderTarget();
}

void ReceiverPreviewWindow::EnsureRenderTarget()
{
    if (renderTarget_) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    ThrowIfFailed(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)), "IDXGISwapChain::GetBuffer(receiver preview)");
    ThrowIfFailed(
        device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget_),
        "ID3D11Device::CreateRenderTargetView(receiver preview)");
}

void ReceiverPreviewWindow::EnsurePipeline()
{
    if (vertexShader_ && pixelShader_ && sampler_) {
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

Texture2D frameTexture : register(t0);
SamplerState linearSampler : register(s0);

float4 ps_main(VertexOut input) : SV_Target
{
    return frameTexture.Sample(linearSampler, input.uv);
}
)";

    const auto vertexShader = CompileShader(shaderSource, "vs_main", "vs_4_0");
    const auto pixelShader = CompileShader(shaderSource, "ps_main", "ps_4_0");

    ThrowIfFailed(
        device_->CreateVertexShader(vertexShader->GetBufferPointer(), vertexShader->GetBufferSize(), nullptr, &vertexShader_),
        "ID3D11Device::CreateVertexShader(receiver preview)");
    ThrowIfFailed(
        device_->CreatePixelShader(pixelShader->GetBufferPointer(), pixelShader->GetBufferSize(), nullptr, &pixelShader_),
        "ID3D11Device::CreatePixelShader(receiver preview)");

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0.0f;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    ThrowIfFailed(device_->CreateSamplerState(&samplerDesc, &sampler_), "ID3D11Device::CreateSamplerState(receiver preview)");
}

void ReceiverPreviewWindow::EnsureFrameTexture(int width, int height)
{
    if (frameTexture_ &&
        frameDesc_.Width == static_cast<UINT>(width) &&
        frameDesc_.Height == static_cast<UINT>(height)) {
        return;
    }

    frameView_.Reset();
    frameTexture_.Reset();

    frameDesc_ = {};
    frameDesc_.Width = static_cast<UINT>(width);
    frameDesc_.Height = static_cast<UINT>(height);
    frameDesc_.MipLevels = 1;
    frameDesc_.ArraySize = 1;
    frameDesc_.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    frameDesc_.SampleDesc.Count = 1;
    frameDesc_.SampleDesc.Quality = 0;
    frameDesc_.Usage = D3D11_USAGE_DEFAULT;
    frameDesc_.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ThrowIfFailed(device_->CreateTexture2D(&frameDesc_, nullptr, &frameTexture_), "ID3D11Device::CreateTexture2D(receiver frame)");

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    viewDesc.Format = frameDesc_.Format;
    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.MipLevels = 1;
    ThrowIfFailed(device_->CreateShaderResourceView(frameTexture_.Get(), &viewDesc, &frameView_), "ID3D11Device::CreateShaderResourceView(receiver frame)");
}

void ReceiverPreviewWindow::SizeWindowForFirstFrame(int width, int height)
{
    if (sizedForFirstFrame_ || hwnd_ == nullptr) {
        return;
    }

    const SIZE clientSize = FitFrameToWorkArea(hwnd_, width, height);
    RECT windowRect{0, 0, clientSize.cx, clientSize.cy};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    AdjustWindowRectEx(&windowRect, style, FALSE, exStyle);
    SetWindowPos(
        hwnd_,
        nullptr,
        0,
        0,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateClientSize();
    swapChainResizePending_ = true;
    sizedForFirstFrame_ = true;
}

void ReceiverPreviewWindow::Render()
{
    if (!swapChain_ || !frameView_ || hwnd_ == nullptr || IsIconic(hwnd_) != FALSE) {
        return;
    }

    UpdateClientSize();
    ResizeSwapChainIfNeeded();
    EnsureRenderTarget();
    EnsurePipeline();

    const float clearColor[] = {0.02f, 0.02f, 0.02f, 1.0f};
    context_->ClearRenderTargetView(renderTarget_.Get(), clearColor);

    const float clientAspect = static_cast<float>(clientWidth_) / static_cast<float>(clientHeight_);
    const float frameAspect = static_cast<float>(frameWidth_) / static_cast<float>(frameHeight_);
    D3D11_VIEWPORT viewport{};
    if (clientAspect > frameAspect) {
        viewport.Height = static_cast<float>(clientHeight_);
        viewport.Width = viewport.Height * frameAspect;
        viewport.TopLeftX = (static_cast<float>(clientWidth_) - viewport.Width) * 0.5f;
        viewport.TopLeftY = 0.0f;
    } else {
        viewport.Width = static_cast<float>(clientWidth_);
        viewport.Height = viewport.Width / frameAspect;
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = (static_cast<float>(clientHeight_) - viewport.Height) * 0.5f;
    }
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    ID3D11RenderTargetView* renderTargets[] = {renderTarget_.Get()};
    ID3D11ShaderResourceView* shaderResources[] = {frameView_.Get()};
    ID3D11SamplerState* samplers[] = {sampler_.Get()};

    context_->OMSetRenderTargets(1, renderTargets, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, shaderResources);
    context_->PSSetSamplers(0, 1, samplers);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullShaderResources[] = {nullptr};
    ID3D11RenderTargetView* nullRenderTargets[] = {nullptr};
    context_->PSSetShaderResources(0, 1, nullShaderResources);
    context_->OMSetRenderTargets(1, nullRenderTargets, nullptr);

    ThrowIfFailed(swapChain_->Present(1, 0), "IDXGISwapChain::Present(receiver preview)");
}

} // namespace screenshare
