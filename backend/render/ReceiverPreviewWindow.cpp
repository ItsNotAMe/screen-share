#include "render/ReceiverPreviewWindow.h"


#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace screenshare {
namespace {

constexpr const wchar_t* WindowClassName = L"ScreenShareReceiverPreviewWindow";

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

ReceiverPreviewWindow::ReceiverPreviewWindow(FramePresentationFactory factory) : presenter_(std::move(factory))
{
    windowedPlacement_.length = sizeof(windowedPlacement_);
}

ReceiverPreviewWindow::~ReceiverPreviewWindow()
{
    presenter_.Release();
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
    inputMapping_={};
    PresentView({frame.width, frame.height, reinterpret_cast<const uint8_t*>(frame.data.data()), frame.data.size(), frame.texture.Get()});
}
void ReceiverPreviewWindow::PresentFrame(const Nv12VideoFrame& frame)
{
    const auto before=framesPresented_;
    if (frame.native) PresentView({frame.width, frame.height, nullptr, 0, frame.native->texture()});
    else PresentPixels(frame.width, frame.height, frame.pixels());
    inputMapping_=framesPresented_>before?frame.inputMapping:input::FrameMapping{};
}
void ReceiverPreviewWindow::SetLowLatency(bool enabled) {
    if (hwnd_) throw std::logic_error("Set latency mode before opening preview");
    lowLatency_ = enabled;
}
void ReceiverPreviewWindow::PresentPixels(int width, int height, std::span<const uint8_t> pixels)
{
    PresentView({width, height, pixels.data(), pixels.size()});
}
void ReceiverPreviewWindow::PresentView(const Nv12D3D11Presenter::FrameView& view)
{
    const int width = view.width, height = view.height;
    if (closeRequested_) return;
    // Validate before using dimensions to resize the native window. The shared
    // renderer also validates the full view before GPU upload.
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
        width % 2 || height % 2 || (!view.texture && (!view.data || view.dataSize < size_t(width) * size_t(height) * 3 / 2)))
        throw std::invalid_argument("Invalid preview NV12 frame");
    EnsureWindow(width, height);
    SizeWindowForFirstFrame(width, height);
    UpdateClientSize();
    frameWidth_ = width; frameHeight_ = height;
    const bool presented = presenter_.Present(hwnd_, clientWidth_, clientHeight_, true, lowLatency_,
        view,
        scaleMode_ == PreviewScaleMode::Fit ? Nv12D3D11Presenter::ScaleMode::Fit : Nv12D3D11Presenter::ScaleMode::OriginalSize);
    if (presented) ++framesPresented_; else ++framesDropped_;
    RefreshTitle();
}

void ReceiverPreviewWindow::ClearFrame()
{
    inputMapping_={};
    if (closeRequested_) return;
    presenter_.Clear();
    frameWidth_ = frameHeight_ = 0;
    Render();
}

void ReceiverPreviewWindow::SetStatusText(std::string_view statusText)
{
    statusText_.assign(statusText);
    RefreshTitle();
}

void ReceiverPreviewWindow::SetControlCallbacks(ReceiverPreviewControlCallbacks callbacks)
{
    controlCallbacks_ = std::move(callbacks);
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
        try { return window->WindowProc(message, wParam, lParam); }
        catch (...) {
            // Never unwind C++ exceptions through the Win32 callback boundary.
            window->closeRequested_ = true;
            return 0;
        }
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void ReceiverPreviewWindow::SetRemoteInput(uint8_t capabilities,std::function<void(const input::Event&)> callback)
{
    inputCapabilities_=capabilities&3;inputCallback_=std::move(callback);
}

LRESULT ReceiverPreviewWindow::WindowProc(UINT message, WPARAM wParam, LPARAM lParam)
{
    if(message==WM_SIZE || (message==WM_ACTIVATE && LOWORD(wParam)==WA_INACTIVE))inputMapping_={};
    if(inputCallback_ && inputCapabilities_ && inputMapping_.Valid()) {
        input::Event event;event.sourceGeneration=inputMapping_.generation;
        bool send=false,mouse=false;
        if((inputCapabilities_&input::Keyboard) && (message==WM_KEYDOWN || message==WM_KEYUP || message==WM_SYSKEYDOWN || message==WM_SYSKEYUP)) {
            event.kind=input::Kind::Key;event.key=uint16_t(wParam);event.scan=uint16_t((lParam>>16)&0x1ff);
            event.down=message==WM_KEYDOWN || message==WM_SYSKEYDOWN;send=true;
        } else if(inputCapabilities_&input::Mouse) {
            if(message==WM_MOUSEMOVE) {event.kind=input::Kind::Pointer;send=mouse=true;}
            else if(message==WM_LBUTTONDOWN || message==WM_LBUTTONUP || message==WM_RBUTTONDOWN || message==WM_RBUTTONUP || message==WM_MBUTTONDOWN || message==WM_MBUTTONUP || message==WM_XBUTTONDOWN || message==WM_XBUTTONUP) {
                event.kind=input::Kind::Button;send=mouse=true;
                event.button=message==WM_LBUTTONDOWN || message==WM_LBUTTONUP?0:message==WM_RBUTTONDOWN || message==WM_RBUTTONUP?1:message==WM_MBUTTONDOWN || message==WM_MBUTTONUP?2:HIWORD(wParam)==XBUTTON1?3:4;
                event.down=message==WM_LBUTTONDOWN || message==WM_RBUTTONDOWN || message==WM_MBUTTONDOWN || message==WM_XBUTTONDOWN;
            } else if(message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL) {
                event.kind=input::Kind::Wheel;send=mouse=true;
                const auto delta=int16_t(std::clamp(int(short(HIWORD(wParam))),-1200,1200));
                if(message==WM_MOUSEWHEEL)event.wheelY=delta;else event.wheelX=delta;
            }
        }
        if(mouse) {
            POINT point{short(LOWORD(lParam)),short(HIWORD(lParam))};
            if(message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL)ScreenToClient(hwnd_,&point);
            const double scale=scaleMode_==PreviewScaleMode::Fit?std::min(double(clientWidth_)/inputMapping_.width,double(clientHeight_)/inputMapping_.height):1.0;
            const double width=inputMapping_.width*scale,height=inputMapping_.height*scale;
            const auto mapped=inputMapping_.Point(float((point.x-(clientWidth_-width)/2)/width),float((point.y-(clientHeight_-height)/2)/height));
            if(!mapped) {
                if(event.kind==input::Kind::Button && !event.down) {event.kind=input::Kind::Release;inputCallback_(event);}
                return 0;
            }
            event.x=mapped->first;event.y=mapped->second;
        }
        if(send) {if(input::Valid(event))inputCallback_(event);return 0;}
    }
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
        if (message == WM_KEYDOWN && wParam == 'M' && controlCallbacks_.toggleAudioMute) {
            controlCallbacks_.toggleAudioMute();
            return 0;
        }
        if (message == WM_KEYDOWN &&
            (wParam == VK_OEM_PLUS || wParam == VK_ADD) &&
            controlCallbacks_.adjustAudioVolumePercent) {
            controlCallbacks_.adjustAudioVolumePercent(5);
            return 0;
        }
        if (message == WM_KEYDOWN &&
            (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT) &&
            controlCallbacks_.adjustAudioVolumePercent) {
            controlCallbacks_.adjustAudioVolumePercent(-5);
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    case WM_CLOSE:
        closeRequested_ = true;
        presenter_.Release();
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        closeRequested_ = true;
        hwnd_ = nullptr;
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            clientWidth_ = ClampDimension(LOWORD(lParam));
            clientHeight_ = ClampDimension(HIWORD(lParam));
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
    // A launcher can supply a hidden startup show state that overrides the
    // first ShowWindow call. Explicit preview creation must still show its HWND;
    // this second call does not activate or steal focus from another window.
    if (!IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
    UpdateClientSize();
    Render();
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

void ReceiverPreviewWindow::SizeWindowForFirstFrame(int width, int height)
{
    if (sizedForFirstFrame_ || hwnd_ == nullptr) {
        return;
    }

    const SIZE clientSize = FitFrameToWorkArea(hwnd_, width, height);
    ResizeWindowClientTo(hwnd_, clientSize.cx, clientSize.cy);
    UpdateClientSize();
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

    const std::wstring title = PreviewTitle(presenter_.statistics().terminal
        ? "Video presentation failed. Leave and rejoin to retry." : statusText_, scaleMode_, fullscreen_);
    if (title == renderedTitle_) return;
    SetWindowTextW(hwnd_, title.c_str());
    renderedTitle_ = title;
}

bool ReceiverPreviewWindow::Render()
{
    if (!hwnd_ || closeRequested_ || IsIconic(hwnd_)) return false;
    UpdateClientSize();
    const bool rendered = presenter_.Update(hwnd_, clientWidth_, clientHeight_, true, lowLatency_,
        scaleMode_ == PreviewScaleMode::Fit ? Nv12D3D11Presenter::ScaleMode::Fit : Nv12D3D11Presenter::ScaleMode::OriginalSize);
    RefreshTitle();
    return rendered;
}

} // namespace screenshare
