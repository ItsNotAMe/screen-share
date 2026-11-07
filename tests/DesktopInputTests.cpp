#include "input/v2/DesktopSink.h"
#include "codec/InputMappingSei.h"
#include "input/WindowsKeyInput.h"
#include "input/WindowsMouseInput.h"
#include "input/HostMouseMonitorThread.h"
#include "RecordingGamepadSink.h"
#include <dwmapi.h>
#include <iostream>
#include <stdexcept>
#include <source_location>
#include <thread>
#include <future>
#include <atomic>
#include <cstdlib>
using namespace screenshare::input;
void Check(bool value,std::source_location where=std::source_location::current()) {
    if(!value)throw std::runtime_error("Desktop input assertion at "+std::to_string(where.line()));
}
void JoinMouseMonitor(std::unique_ptr<screenshare::HostMouseMonitorThread> monitor, const char* failure) {
    auto stopped = std::async(std::launch::async, [owner = std::move(monitor)]() mutable { owner.reset(); });
    if (stopped.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        std::cerr << failure << '\n' << std::flush;
        std::_Exit(1); // A stuck join must produce a useful test failure, not hang.
    }
    stopped.get();
}
void MouseMonitorDelayedStartup() {
    // No hook/SendInput. Hold installation before the thread creates a queue,
    // request stop, then let startup finish. Shutdown must retain that request.
    std::promise<void> entered, resume;
    auto resumed = resume.get_future();
    std::atomic<unsigned> removed{0};
    auto monitor = std::make_unique<screenshare::HostMouseMonitorThread>([&] {
        entered.set_value(); resumed.wait(); return reinterpret_cast<HHOOK>(1);
    }, [&](HHOOK hook) { if (hook == reinterpret_cast<HHOOK>(1)) ++removed; });
    entered.get_future().get();
    monitor->RequestStop();
    resume.set_value();
    JoinMouseMonitor(std::move(monitor), "Mouse monitor lost stop during delayed queue startup");
    Check(removed == 1);
}
void MouseMonitorFullQueue() {
    std::promise<DWORD> entered;
    std::promise<void> resume;
    auto resumed = resume.get_future();
    std::atomic<unsigned> removed{0};
    auto monitor = std::make_unique<screenshare::HostMouseMonitorThread>([&] {
        MSG message{}; PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        entered.set_value(GetCurrentThreadId()); resumed.wait();
        return reinterpret_cast<HHOOK>(1);
    }, [&](HHOOK) { ++removed; });
    const auto thread = entered.get_future().get();
    unsigned queued = 0;
    while (queued < 100000 && PostThreadMessageW(thread, WM_APP, 0, 0)) ++queued;
    const auto error = GetLastError();
    // Always release startup before an assertion can unwind the owner.
    monitor->RequestStop(); resume.set_value();
    JoinMouseMonitor(std::move(monitor), "Mouse monitor could not stop with a full message queue");
    Check(queued > 0 && queued < 100000 && error == ERROR_NOT_ENOUGH_QUOTA && removed == 1);
}
void MouseMonitorImmediateStop() {
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        std::atomic<unsigned> installed{0}, removed{0};
        DWORD owner = 0;
        auto monitor = std::make_unique<screenshare::HostMouseMonitorThread>([&] {
            owner = GetCurrentThreadId(); ++installed;
            return cycle % 2 ? nullptr : reinterpret_cast<HHOOK>(1);
        }, [&](HHOOK) { if (owner == GetCurrentThreadId()) ++removed; });
        monitor->RequestStop(); monitor->RequestStop();
        JoinMouseMonitor(std::move(monitor), "Immediate mouse monitor stop did not join");
        Check(installed == 1 && removed == (cycle % 2 ? 0u : 1u));
    }
}
void MouseMonitorMessagePump() {
    constexpr auto name = L"ScreenShareMouseMonitorPumpTest";
    WNDCLASSW type{}; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = name;
    type.lpfnWndProc = [](HWND window, UINT message, WPARAM w, LPARAM l) -> LRESULT {
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        if (message == WM_APP) {
            ++*reinterpret_cast<std::atomic<unsigned>*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            return 37;
        }
        return DefWindowProcW(window, message, w, l);
    };
    Check(RegisterClassW(&type));
    struct Class { const wchar_t* name; ~Class() { UnregisterClassW(name, GetModuleHandleW(nullptr)); } } registered{name};
    std::promise<HWND> created;
    std::atomic<unsigned> messages{0}, removed{0};
    auto monitor = std::make_unique<screenshare::HostMouseMonitorThread>([&] {
        const auto window = CreateWindowExW(0, name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, &messages);
        created.set_value(window); return reinterpret_cast<HHOOK>(window);
    }, [&](HHOOK hook) { if (DestroyWindow(reinterpret_cast<HWND>(hook))) ++removed; });
    const auto window = created.get_future().get();
    DWORD_PTR result = 0;
    const bool sent = window && SendMessageTimeoutW(window, WM_APP, 0, 0, SMTO_ABORTIFHUNG, 1000, &result);
    const bool posted = window && PostMessageW(window, WM_APP, 0, 0);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (messages < 2 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    JoinMouseMonitor(std::move(monitor), "Mouse monitor could not stop after processing messages");
    Check(sent && result == 37 && posted && messages == 2 && removed == 1);
}
struct Evidence {unsigned applied=0,released=0,neutralized=0;bool healthy=true,focused=true,held=false,fail=false;};
class Device final:public DesktopDevice {
    std::shared_ptr<Evidence> evidence_;
public:
    explicit Device(std::shared_ptr<Evidence> evidence):evidence_(std::move(evidence)){}
    bool Healthy() override {return evidence_->healthy;}
    bool Focused() override {return evidence_->focused;}
    void ReleaseHeldInput() noexcept override {
        if(evidence_->held)++evidence_->neutralized;
        evidence_->held=false;
    }
    bool Apply(const Event& event) override {++evidence_->applied;evidence_->held=event.down;return !evidence_->fail;}
    void Release() noexcept override {++evidence_->released;}
    bool Restrict(uint8_t) override {ReleaseHeldInput();return !evidence_->fail;}
};
void MinimizedKeyboardGrant() {
    // Use a real minimized HWND and the production Windows device, but never
    // call Apply/SendInput. Geometry stands in for a previously captured frame.
    struct Window {
        HWND handle = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Paused input test", WS_OVERLAPPEDWINDOW,
            0, 0, 320, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (handle) DestroyWindow(handle); }
    } window;
    Check(window.handle != nullptr);
    ShowWindow(window.handle, SW_SHOWMINNOACTIVE);
    Check(IsWindowVisible(window.handle) && IsIconic(window.handle));
    const auto source = (uint64_t(GetCurrentProcessId()) << 32) | 1;
    const auto property = WindowIdentityProperty(source);
    Check(SetPropW(window.handle, property.c_str(), reinterpret_cast<HANDLE>(source)));
    auto target = std::make_shared<DesktopTargetState>();
    target->Publish({source, reinterpret_cast<uint64_t>(window.handle), GetCurrentProcessId(), 0, 0, 320, 180});
    auto sink = CreateWindowsDesktopSink(target);
    Check(sink->Grant("paused", Keyboard, 0) && sink->Healthy("paused"));
    // Minimization may pause injection; losing source identity must still revoke.
    RemovePropW(window.handle, property.c_str());
    Check(!sink->Healthy("paused"));
    sink->Release("paused");
}
void WindowKeyboardWithoutMouseGeometry() {
    // Generated borderless game-style window. Grant/health only: no SendInput.
    struct Window {
        HWND handle = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Window keyboard test", WS_POPUP,
            30, 30, 320, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ~Window() { if (handle) DestroyWindow(handle); }
    } window;
    Check(window.handle != nullptr);
    ShowWindow(window.handle, SW_SHOWNOACTIVATE);
    RECT bounds{};
    Check(SUCCEEDED(DwmGetWindowAttribute(window.handle, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))));
    const auto source = (uint64_t(GetCurrentProcessId()) << 32) | 2;
    const auto property = WindowIdentityProperty(source);
    Check(SetPropW(window.handle, property.c_str(), reinterpret_cast<HANDLE>(source)));
    auto state = std::make_shared<DesktopTargetState>();
    // Model captured game pixels that differ from DWM's outer rectangle.
    const DesktopTarget identity{source, reinterpret_cast<uint64_t>(window.handle), GetCurrentProcessId(),
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top};
    const auto captured = CapturedInputTarget(identity, identity.width + 8, identity.height + 8);
    Check(captured.Valid() && !captured.mouseMapped);
    state->Publish(captured);
    unsigned devices = 0;
    DesktopSink mappingGate(state, {}, [&](auto, uint8_t) {
        ++devices; return std::make_unique<Device>(std::make_shared<Evidence>());
    });
    Check(mappingGate.Grant("mouse", Mouse, 0));mappingGate.Release("mouse");
    Check(mappingGate.Grant("combined", Mouse | Keyboard, 0));mappingGate.Release("combined");
    Check(devices == 2 && mappingGate.Grant("keyboard", Keyboard, 0));
    mappingGate.Release("keyboard");
    auto sink = CreateWindowsDesktopSink(state);
    Check(sink->Grant("keyboard", Keyboard, 0) && sink->Healthy("keyboard"));
    const auto foreground=GetForegroundWindow();
    Check(sink->Grant("mouse", Mouse, 0));
    Check(sink->Healthy("mouse") && GetForegroundWindow()==foreground);
    sink->Release("mouse");
    ShowWindow(window.handle, SW_HIDE);
    Check(!sink->Healthy("keyboard"));
    sink->Release("keyboard");
    ShowWindow(window.handle, SW_SHOWNOACTIVATE);
    Check(sink->Grant("keyboard", Keyboard, 0));
    RemovePropW(window.handle, property.c_str());
    Check(!sink->Healthy("keyboard"));
    sink->Release("keyboard");
    Check(SetPropW(window.handle, property.c_str(), reinterpret_cast<HANDLE>(source)));
    // DWM bounds can be unavailable while the captured window identity is live.
    auto noBounds = identity; noBounds.width = noBounds.height = 0;
    noBounds = CapturedInputTarget(noBounds, 640, 360);
    Check(noBounds.Valid() && !noBounds.mouseMapped);
    state->Publish(noBounds);
    Check(sink->Grant("keyboard", Keyboard, 0));
    sink->Release("keyboard");
    Check(sink->Grant("combined", Mouse | Keyboard, 0));sink->Release("combined");
    Check(sink->Grant("keyboard", Keyboard, 0));
    auto wrongProcess = noBounds; ++wrongProcess.process;
    state->Publish(wrongProcess);
    Check(!sink->Healthy("keyboard"));
    sink->Release("keyboard");
    Check(!sink->Grant("keyboard", Keyboard, 0));
    // Matching geometry still allows mouse control, with exclusive ownership.
    state->Publish(CapturedInputTarget(identity, identity.width, identity.height));
    Check(sink->Grant("combined", Mouse | Keyboard, 0));
    sink->Release("combined");
    RemovePropW(window.handle, property.c_str());
}
void RestrictKeepsDevices() {
    auto target=std::make_shared<DesktopTargetState>();
    const auto epoch=target->Publish({57,0,0,0,0,640,360});
    auto evidence=std::make_shared<Evidence>();auto pads=std::make_shared<RecordingGamepadSink>();
    unsigned devices=0;
    DesktopSink sink(target,pads,[&](auto,uint8_t){++devices;return std::make_unique<Device>(evidence);});
    Check(sink.Grant("viewer",Mouse|Keyboard|Gamepad,0));
    Event key;key.kind=Kind::Key;key.key=65;key.down=true;key.sourceGeneration=epoch;
    Check(sink.Apply("viewer",key) && evidence->held);
    Check(sink.Restrict("viewer",Keyboard|Gamepad));
    Check(devices==1 && !evidence->released && !evidence->held && pads->grants==1 && !pads->released);
    Event mouse;mouse.kind=Kind::Pointer;mouse.sourceGeneration=epoch;
    Check(!sink.Apply("viewer",mouse) && sink.Apply("viewer",key));
    Check(!sink.Restrict("viewer",7)); // Cannot add back a withdrawn control.
    Check(sink.Restrict("viewer",Gamepad));
    Check(evidence->released==1 && pads->grants==1 && !pads->released && sink.Healthy("viewer"));
    sink.Release("viewer");Check(pads->released==1);
}
void StaleCaptureKeepsOwnership() {
    auto state=std::make_shared<DesktopTargetState>();auto evidence=std::make_shared<Evidence>();
    auto pads=std::make_shared<RecordingGamepadSink>();
    DesktopSink sink(state,pads,[evidence](auto,uint8_t){return std::make_unique<Device>(evidence);});
    DesktopTarget target{55,0,0,0,0,640,360};const auto generation=state->Publish(target);
    Check(sink.Grant("viewer",Mouse|Keyboard|Gamepad,0));
    Event key;key.kind=Kind::Key;key.key=65;key.down=true;key.sourceGeneration=generation;
    Check(sink.Apply("viewer",key) && evidence->held);
    Event pad;pad.kind=Kind::Pad;pad.buttons=1;Check(sink.Apply("viewer",pad) && pads->buttons==1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    Check(!state->Read().generation && state->Identity().generation==generation && sink.Healthy("viewer"));
    Check(!evidence->held && !evidence->released && !sink.Grant("other",Keyboard,0));
    const auto applied=evidence->applied;
    Check(sink.Apply("viewer",key) && evidence->applied==applied); // Stale geometry never injects.
    state->Touch(target);Check(sink.Apply("viewer",key) && evidence->held);
    Check(sink.Pause("viewer") && !evidence->held && !evidence->released && sink.Healthy("viewer") &&
        !pads->buttons && pads->grants==1 && !pads->released);
    Check(sink.Apply("viewer",key));
    state->Invalidate(target.source);Check(!sink.Healthy("viewer"));sink.Release("viewer");
    Check(evidence->released==1 && pads->released==1);
}
int main() {try {
    MouseMonitorDelayedStartup();
    MouseMonitorFullQueue();
    MouseMonitorImmediateStop();
    MouseMonitorMessagePump();
    RestrictKeepsDevices();
    const auto relative=screenshare::WindowsRelativeMouseInput(-123,456);
    Check(relative.type==INPUT_MOUSE && relative.mi.dx==-123 && relative.mi.dy==456);
    Check(relative.mi.dwFlags==(MOUSEEVENTF_MOVE|MOUSEEVENTF_MOVE_NOCOALESCE));
    MinimizedKeyboardGrant();
    WindowKeyboardWithoutMouseGeometry();
    StaleCaptureKeepsOwnership();
    // Inspect Windows payloads without calling SendInput or changing local keys.
    for (const auto [key, scan] : {std::pair{VK_LEFT, 0x14b}, {VK_UP, 0x148},
            {VK_RIGHT, 0x14d}, {VK_DOWN, 0x150}, {VK_RCONTROL, 0x11d},
            {VK_RMENU, 0x138}, {VK_RETURN, 0x11c}, {VK_DELETE, 0x153}}) {
        for (bool down : {false, true}) {
            const auto input = screenshare::WindowsKeyInput(key, scan, down);
            Check(input.type == INPUT_KEYBOARD && input.ki.wVk == 0 && input.ki.wScan == (scan & 0xff));
            Check(input.ki.dwFlags == (KEYEVENTF_SCANCODE | KEYEVENTF_EXTENDEDKEY | (down ? 0 : KEYEVENTF_KEYUP)));
        }
    }
    // The keypad shares scan bytes with navigation keys, without the E0 prefix.
    for (const auto [key, scan] : {std::pair{VK_NUMPAD4, 0x4b}, {VK_NUMPAD8, 0x48},
            {VK_NUMPAD6, 0x4d}, {VK_NUMPAD2, 0x50}, {VK_LCONTROL, 0x1d},
            {VK_RETURN, 0x1c}, {0x41, 0x1e}}) {
        const auto input = screenshare::WindowsKeyInput(key, scan, true);
        Check(input.ki.wScan == scan && input.ki.dwFlags == KEYEVENTF_SCANCODE);
    }
    const auto fallback = screenshare::WindowsKeyInput(VK_LEFT, 0, true);
    Check(fallback.ki.wVk == VK_LEFT && fallback.ki.dwFlags == KEYEVENTF_EXTENDEDKEY);
    const auto pause = screenshare::WindowsKeyInput(VK_PAUSE, 0x45, true);
    Check(pause.ki.wVk == VK_PAUSE && !(pause.ki.dwFlags & KEYEVENTF_SCANCODE));
    const FrameMapping mapping{0x100000001,640,480,0,60,640,360};
    Check(mapping.Valid() && !mapping.Point(.5f,0) && !mapping.Point(.5f,1));
    const auto center=mapping.Point(.5f,.5f);Check(center && std::abs(center->first-.5f)<.001f && std::abs(center->second-.5f)<.001f);
    std::vector<std::byte> encoded;InsertMappingSei(encoded,mapping);
    auto decode=[](const auto& bytes){return ReadMappingSei({reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size()});};
    Check(decode(encoded)==mapping);
    std::vector<std::byte> keyframe;
    for(auto value:{0,0,0,1,0x67,0x11,0,0,0,1,0x68,0x22,0,0,0,1,0x65,0x33})keyframe.push_back(std::byte(value));
    InsertMappingSei(keyframe,mapping);Check(keyframe[4]==std::byte{0x67} && decode(keyframe)==mapping);
    for(size_t length=0;length<encoded.size();++length)Check(!ReadMappingSei({reinterpret_cast<const uint8_t*>(encoded.data()),length}).Valid());
    auto duplicate=encoded;duplicate.insert(duplicate.end(),encoded.begin(),encoded.end());Check(!decode(duplicate).Valid());
    auto corrupt=encoded;corrupt.back()=std::byte{0};Check(!decode(corrupt).Valid());
    auto state=std::make_shared<DesktopTargetState>();auto evidence=std::make_shared<Evidence>();
    DesktopSink sink(state,{},[evidence](auto,uint8_t){return std::make_unique<Device>(evidence);});
    Check(!sink.Grant("viewer",Mouse,0));
    DesktopTarget target{1,0,0,-1920,0,1920,1080};const auto generation=state->Publish(target);
    Check(generation && state->Publish(target)==generation && sink.Grant("viewer",Mouse|Keyboard,0));
    Check(!sink.Grant("other",Mouse,0));
    Event move;move.kind=Kind::Pointer;move.x=.5f;move.y=.5f;move.sourceGeneration=generation;
    Check(sink.Apply("viewer",move));move.sourceGeneration=generation+1;Check(!sink.Apply("viewer",move));
    Check(evidence->applied==1);target.left=0;state->Touch(target);Check(!sink.Healthy("viewer"));sink.Release("viewer");Check(evidence->released==1);
    const auto next=state->Publish(target);Check(next>generation);target.window=123;target.process=42;state->Publish(target);
    const auto windowGeneration=state->Read().generation;
    // Window keyboard grants work even while the host is using another app.
    evidence->focused=false;Check(sink.Grant("viewer",Keyboard,0));
    Event key;key.kind=Kind::Key;key.key=0x41;key.down=true;key.sourceGeneration=windowGeneration;
    const auto applied=evidence->applied;
    Check(sink.Healthy("viewer") && sink.Apply("viewer",key));
    Check(evidence->applied==applied && !evidence->held && evidence->released==1);
    evidence->focused=true;Check(sink.Apply("viewer",key));Check(evidence->held);
    // Health polling releases held keys even when no next input packet arrives.
    evidence->focused=false;Check(sink.Healthy("viewer"));
    Check(!evidence->held && evidence->neutralized==1 && evidence->released==1);
    Check(sink.Apply("viewer",key));Check(evidence->applied==applied+1);
    Check(!sink.Grant("other",Keyboard,0)); // Pausing retains exclusive ownership.
    key.sourceGeneration=windowGeneration+1;Check(!sink.Apply("viewer",key));
    key.sourceGeneration=windowGeneration;
    evidence->focused=true;Check(sink.Apply("viewer",key));Check(evidence->applied==applied+2);
    evidence->healthy=false;Check(!sink.Healthy("viewer"));sink.Release("viewer");
    evidence->healthy=true;Check(sink.Grant("viewer",Mouse,0));state->Invalidate();Check(!sink.Healthy("viewer"));sink.Release("viewer");
    Check(evidence->released==3);
    std::cout<<"{\"passed\":true,\"desktop_mapping\":true,\"physical_input\":false}\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
