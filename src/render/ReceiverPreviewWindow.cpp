#include "render/ReceiverPreviewWindow.h"

#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

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

std::wstring PreviewTitle(std::string_view statusText, PreviewScaleMode scaleMode, bool fullscreen)
{
    std::wstring title = L"ScreenShare Receiver Preview [";
    title += scaleMode == PreviewScaleMode::Fit ? L"fit" : L"1:1";
    if (fullscreen) {
        title += L", fullscreen";
    }
    title += L"]";

    if (statusText.empty()) {
        return title;
    }

    title += L" - ";
    for (const char character : statusText) {
        title.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));
    }
    return title;
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

void ValidateNv12Frame(const DecodedFrameInfo& frame)
{
    if (frame.width <= 0 || frame.height <= 0) {
        throw std::runtime_error("Decoded preview frame dimensions are not available");
    }
    if ((frame.width % 2) != 0 || (frame.height % 2) != 0) {
        throw std::runtime_error("Decoded preview frame dimensions must be even for NV12");
    }

    const uint64_t lumaBytes = static_cast<uint64_t>(frame.width) * static_cast<uint64_t>(frame.height);
    const uint64_t requiredBytes = lumaBytes + lumaBytes / 2;
    if (requiredBytes > std::numeric_limits<size_t>::max() || frame.data.size() < static_cast<size_t>(requiredBytes)) {
        throw std::runtime_error("Decoded preview NV12 frame data is too small");
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

void ResizeWindowClientTo(HWND hwnd, int clientWidth, int clientHeight)
{
    RECT windowRect{
        0,
        0,
        std::max(320, clientWidth),
        std::max(180, clientHeight),
    };
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    AdjustWindowRectEx(&windowRect, style, FALSE, exStyle);
    SetWindowPos(
        hwnd,
        nullptr,
        0,
        0,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

} // namespace

ReceiverPreviewWindow::ReceiverPreviewWindow()
{
    windowedPlacement_.length = sizeof(windowedPlacement_);
}

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
    ValidateNv12Frame(frame);

    EnsureWindow(frame.width, frame.height);
    SizeWindowForFirstFrame(frame.width, frame.height);
    EnsureFrameTextures(frame.width, frame.height);

    const size_t lumaBytes = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
    const auto* luma = reinterpret_cast<const uint8_t*>(frame.data.data());
    const auto* chroma = luma + lumaBytes;

    context_->UpdateSubresource(
        lumaTexture_.Get(),
        0,
        nullptr,
        luma,
        static_cast<UINT>(frame.width),
        0);
    context_->UpdateSubresource(
        chromaTexture_.Get(),
        0,
        nullptr,
        chroma,
        static_cast<UINT>(frame.width),
        0);

    frameWidth_ = frame.width;
    frameHeight_ = frame.height;
    Render();
    ++framesPresented_;
}

void ReceiverPreviewWindow::SetStatusText(std::string_view statusText)
{
    statusText_.assign(statusText);
    RefreshTitle();
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
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wParam == VK_F11 || (message == WM_SYSKEYDOWN && wParam == VK_RETURN)) {
            ToggleFullscreen();
            return 0;
        }
        if (message == WM_KEYDOWN && wParam == VK_ESCAPE && fullscreen_) {
            SetFullscreen(false);
            return 0;
        }
        if (message == WM_KEYDOWN && wParam == 'F') {
            ToggleScaleMode();
            return 0;
        }
        if (message == WM_KEYDOWN && wParam == '1') {
            scaleMode_ = PreviewScaleMode::OriginalSize;
            SizeWindowForCurrentFrame();
            RefreshTitle();
            Render();
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
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
            Render();
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
        PreviewTitle(statusText_, scaleMode_, fullscreen_).c_str(),
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

Texture2D lumaTexture : register(t0);
Texture2D chromaTexture : register(t1);
SamplerState linearSampler : register(s0);

float4 ps_main(VertexOut input) : SV_Target
{
    float y = lumaTexture.Sample(linearSampler, input.uv).r;
    float2 uv = chromaTexture.Sample(linearSampler, input.uv).rg;

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

void ReceiverPreviewWindow::EnsureFrameTextures(int width, int height)
{
    if (lumaTexture_ &&
        chromaTexture_ &&
        lumaDesc_.Width == static_cast<UINT>(width) &&
        lumaDesc_.Height == static_cast<UINT>(height) &&
        chromaDesc_.Width == static_cast<UINT>(width / 2) &&
        chromaDesc_.Height == static_cast<UINT>(height / 2)) {
        return;
    }

    lumaView_.Reset();
    chromaView_.Reset();
    lumaTexture_.Reset();
    chromaTexture_.Reset();

    lumaDesc_ = {};
    lumaDesc_.Width = static_cast<UINT>(width);
    lumaDesc_.Height = static_cast<UINT>(height);
    lumaDesc_.MipLevels = 1;
    lumaDesc_.ArraySize = 1;
    lumaDesc_.Format = DXGI_FORMAT_R8_UNORM;
    lumaDesc_.SampleDesc.Count = 1;
    lumaDesc_.SampleDesc.Quality = 0;
    lumaDesc_.Usage = D3D11_USAGE_DEFAULT;
    lumaDesc_.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    chromaDesc_ = lumaDesc_;
    chromaDesc_.Width = static_cast<UINT>(width / 2);
    chromaDesc_.Height = static_cast<UINT>(height / 2);
    chromaDesc_.Format = DXGI_FORMAT_R8G8_UNORM;

    ThrowIfFailed(device_->CreateTexture2D(&lumaDesc_, nullptr, &lumaTexture_), "ID3D11Device::CreateTexture2D(receiver luma)");
    ThrowIfFailed(device_->CreateTexture2D(&chromaDesc_, nullptr, &chromaTexture_), "ID3D11Device::CreateTexture2D(receiver chroma)");

    D3D11_SHADER_RESOURCE_VIEW_DESC lumaViewDesc{};
    lumaViewDesc.Format = lumaDesc_.Format;
    lumaViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    lumaViewDesc.Texture2D.MipLevels = 1;

    D3D11_SHADER_RESOURCE_VIEW_DESC chromaViewDesc{};
    chromaViewDesc.Format = chromaDesc_.Format;
    chromaViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    chromaViewDesc.Texture2D.MipLevels = 1;

    ThrowIfFailed(device_->CreateShaderResourceView(lumaTexture_.Get(), &lumaViewDesc, &lumaView_), "ID3D11Device::CreateShaderResourceView(receiver luma)");
    ThrowIfFailed(device_->CreateShaderResourceView(chromaTexture_.Get(), &chromaViewDesc, &chromaView_), "ID3D11Device::CreateShaderResourceView(receiver chroma)");
}

void ReceiverPreviewWindow::SizeWindowForFirstFrame(int width, int height)
{
    if (sizedForFirstFrame_ || hwnd_ == nullptr) {
        return;
    }

    const SIZE clientSize = FitFrameToWorkArea(hwnd_, width, height);
    ResizeWindowClientTo(hwnd_, clientSize.cx, clientSize.cy);
    UpdateClientSize();
    swapChainResizePending_ = true;
    sizedForFirstFrame_ = true;
}

void ReceiverPreviewWindow::SizeWindowForCurrentFrame()
{
    if (hwnd_ == nullptr || fullscreen_ || frameWidth_ <= 0 || frameHeight_ <= 0) {
        return;
    }

    const SIZE clientSize = FitFrameToWorkArea(hwnd_, frameWidth_, frameHeight_);
    ResizeWindowClientTo(hwnd_, clientSize.cx, clientSize.cy);
    UpdateClientSize();
    swapChainResizePending_ = true;
}

void ReceiverPreviewWindow::ToggleFullscreen()
{
    SetFullscreen(!fullscreen_);
}

void ReceiverPreviewWindow::SetFullscreen(bool fullscreen)
{
    if (hwnd_ == nullptr || fullscreen_ == fullscreen) {
        return;
    }

    if (fullscreen) {
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        const HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        if (monitor == nullptr || GetMonitorInfoW(monitor, &monitorInfo) == 0) {
            return;
        }

        windowedStyle_ = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
        windowedExStyle_ = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
        windowedPlacement_.length = sizeof(windowedPlacement_);
        if (GetWindowPlacement(hwnd_, &windowedPlacement_) == 0) {
            return;
        }

        fullscreen_ = true;
        const DWORD fullscreenStyle = (windowedStyle_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, static_cast<LONG_PTR>(fullscreenStyle));
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, static_cast<LONG_PTR>(windowedExStyle_));
        SetWindowPos(
            hwnd_,
            HWND_TOP,
            monitorInfo.rcMonitor.left,
            monitorInfo.rcMonitor.top,
            monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
            monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        fullscreen_ = false;
        SetWindowLongPtrW(hwnd_, GWL_STYLE, static_cast<LONG_PTR>(windowedStyle_));
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, static_cast<LONG_PTR>(windowedExStyle_));
        windowedPlacement_.length = sizeof(windowedPlacement_);
        SetWindowPlacement(hwnd_, &windowedPlacement_);
        SetWindowPos(
            hwnd_,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }

    UpdateClientSize();
    swapChainResizePending_ = true;
    RefreshTitle();
    Render();
}

void ReceiverPreviewWindow::ToggleScaleMode()
{
    scaleMode_ = scaleMode_ == PreviewScaleMode::Fit ? PreviewScaleMode::OriginalSize : PreviewScaleMode::Fit;
    if (scaleMode_ == PreviewScaleMode::OriginalSize) {
        SizeWindowForCurrentFrame();
    }
    RefreshTitle();
    Render();
}

void ReceiverPreviewWindow::RefreshTitle()
{
    if (hwnd_ == nullptr) {
        return;
    }

    const std::wstring title = PreviewTitle(statusText_, scaleMode_, fullscreen_);
    SetWindowTextW(hwnd_, title.c_str());
}

D3D11_VIEWPORT ReceiverPreviewWindow::ComputeViewport() const
{
    D3D11_VIEWPORT viewport{};
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    if (clientWidth_ == 0 || clientHeight_ == 0 || frameWidth_ <= 0 || frameHeight_ <= 0) {
        viewport.Width = 1.0f;
        viewport.Height = 1.0f;
        return viewport;
    }

    const float clientWidth = static_cast<float>(clientWidth_);
    const float clientHeight = static_cast<float>(clientHeight_);
    const float frameWidth = static_cast<float>(frameWidth_);
    const float frameHeight = static_cast<float>(frameHeight_);
    float scale = std::min(clientWidth / frameWidth, clientHeight / frameHeight);
    if (scaleMode_ == PreviewScaleMode::OriginalSize) {
        scale = std::min(scale, 1.0f);
    }

    viewport.Width = std::max(1.0f, frameWidth * scale);
    viewport.Height = std::max(1.0f, frameHeight * scale);
    viewport.TopLeftX = (clientWidth - viewport.Width) * 0.5f;
    viewport.TopLeftY = (clientHeight - viewport.Height) * 0.5f;
    return viewport;
}

void ReceiverPreviewWindow::Render()
{
    if (!swapChain_ || !lumaView_ || !chromaView_ || hwnd_ == nullptr || IsIconic(hwnd_) != FALSE) {
        return;
    }

    UpdateClientSize();
    ResizeSwapChainIfNeeded();
    EnsureRenderTarget();
    EnsurePipeline();

    const float clearColor[] = {0.02f, 0.02f, 0.02f, 1.0f};
    context_->ClearRenderTargetView(renderTarget_.Get(), clearColor);

    const D3D11_VIEWPORT viewport = ComputeViewport();

    ID3D11RenderTargetView* renderTargets[] = {renderTarget_.Get()};
    ID3D11ShaderResourceView* shaderResources[] = {lumaView_.Get(), chromaView_.Get()};
    ID3D11SamplerState* samplers[] = {sampler_.Get()};

    context_->OMSetRenderTargets(1, renderTargets, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 2, shaderResources);
    context_->PSSetSamplers(0, 1, samplers);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullShaderResources[] = {nullptr, nullptr};
    ID3D11RenderTargetView* nullRenderTargets[] = {nullptr};
    context_->PSSetShaderResources(0, 2, nullShaderResources);
    context_->OMSetRenderTargets(1, nullRenderTargets, nullptr);

    ThrowIfFailed(swapChain_->Present(1, 0), "IDXGISwapChain::Present(receiver preview)");
}

} // namespace screenshare
