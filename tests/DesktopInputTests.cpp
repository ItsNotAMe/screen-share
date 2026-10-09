#include "input/v2/DesktopSink.h"
#include "codec/InputMappingSei.h"
#include "input/WindowsKeyInput.h"
#include <dwmapi.h>
#include <iostream>
#include <stdexcept>
#include <source_location>
using namespace screenshare::input;
void Check(bool value,std::source_location where=std::source_location::current()) {
    if(!value)throw std::runtime_error("Desktop input assertion at "+std::to_string(where.line()));
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
    Check(!mappingGate.Grant("mouse", Mouse, 0));
    Check(!mappingGate.Grant("combined", Mouse | Keyboard, 0));
    Check(devices == 0 && mappingGate.Grant("keyboard", Keyboard, 0));
    mappingGate.Release("keyboard");
    auto sink = CreateWindowsDesktopSink(state);
    Check(sink->Grant("keyboard", Keyboard, 0) && sink->Healthy("keyboard"));
    Check(!sink->Grant("mouse", Mouse, 0));
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
    Check(!sink->Grant("combined", Mouse | Keyboard, 0));
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
int main() {try {
    MinimizedKeyboardGrant();
    WindowKeyboardWithoutMouseGeometry();
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
