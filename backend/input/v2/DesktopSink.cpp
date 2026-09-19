#include "DesktopSink.h"
#include "GamepadSink.h"
#include "input/RemoteInputInjector.h"
#include <Windows.h>
#include <dwmapi.h>
#include <stdexcept>

namespace screenshare::input {
DesktopSink::DesktopSink(std::shared_ptr<DesktopTargetState> target,std::shared_ptr<Sink> pads,Factory factory)
    :target_(std::move(target)),pads_(std::move(pads)),factory_(std::move(factory)) {
    if(!target_ || !factory_)throw std::invalid_argument("Desktop input requires explicit dependencies");
}
bool DesktopSink::Grant(const std::string& peer,uint8_t caps,int slot) {
    if(!caps || (caps&~7) || owners_.contains(peer))return false;
    const auto target=target_->Read();std::unique_ptr<DesktopDevice> device;
    if(caps&(Mouse|Keyboard)) {
        if(!target.generation || (target.target.window && (caps&Keyboard)))return false;
        for(const auto& [id,owner]:owners_)if(caps&owner.capabilities&(Mouse|Keyboard))return false;
        device=factory_(target.target,caps&3);
        if(!device || !device->Healthy())return false;
    }
    if((caps&Gamepad) && (!pads_ || !pads_->Grant(peer,Gamepad,slot)))return false;
    owners_.emplace(peer,Owner{caps,target.generation,std::move(device)});return true;
}
bool DesktopSink::Healthy(const std::string& peer) {
    const auto it=owners_.find(peer);if(it==owners_.end())return false;
    const auto& owner=it->second;
    return (!(owner.capabilities&Gamepad) || pads_->Healthy(peer)) &&
        (!owner.device || (target_->Read().generation==owner.generation && owner.device->Healthy()));
}
bool DesktopSink::Apply(const std::string& peer,const Event& event) {
    const auto it=owners_.find(peer);if(it==owners_.end() || !Valid(event) || !Healthy(peer))return false;
    auto& owner=it->second;
    if(event.kind==Kind::Pad)return (owner.capabilities&Gamepad) && pads_->Apply(peer,event);
    const uint8_t required=event.kind==Kind::Key?Keyboard:Mouse;
    return owner.device && (owner.capabilities&required) && event.sourceGeneration==owner.generation && owner.device->Apply(event);
}
void DesktopSink::Release(const std::string& peer) noexcept {
    const auto it=owners_.find(peer);if(it==owners_.end()) {if(pads_)pads_->Release(peer);return;}
    if(it->second.device)it->second.device->Release();
    if((it->second.capabilities&Gamepad) && pads_)pads_->Release(peer);
    owners_.erase(it);
}
namespace {
class WindowsDesktopDevice final : public DesktopDevice {
    DesktopTarget target_;
    RemoteInputInjector injector_;
    uint8_t caps_;
    std::wstring property_;
public:
    WindowsDesktopDevice(DesktopTarget target,uint8_t caps):target_(target),caps_(caps),property_(WindowIdentityProperty(target.source)) {
        if(target.window)injector_.SetTargetWindow(target.window);
        else injector_.SetTargetBounds(target.left,target.top,target.width,target.height);
        injector_.SetMouseMonitoringEnabled(caps&Mouse);
    }
    ~WindowsDesktopDevice() override {Release();}
    bool Healthy() override {
        if(!target_.window)return true;
        const auto window=reinterpret_cast<HWND>(target_.window);
        DWORD pid=0;GetWindowThreadProcessId(window,&pid);
        RECT rect{};
        return pid==target_.process && GetPropW(window,property_.c_str())==reinterpret_cast<HANDLE>(target_.source) && IsWindowVisible(window) && !IsIconic(window) &&
            GetAncestor(GetForegroundWindow(),GA_ROOT)==GetAncestor(window,GA_ROOT) &&
            SUCCEEDED(DwmGetWindowAttribute(window,DWMWA_EXTENDED_FRAME_BOUNDS,&rect,sizeof(rect))) &&
            rect.left==target_.left && rect.top==target_.top && rect.right-rect.left==target_.width && rect.bottom-rect.top==target_.height;
    }
    bool Apply(const Event& event) override {
        if(!Healthy())return false;
        if(event.kind==Kind::Key)return (caps_&Keyboard) && !target_.window && injector_.InjectKey(event.key,event.scan,event.down);
        if(!(caps_&Mouse))return false;
        auto point=std::pair{event.x,event.y};
        if(target_.window) {
            const auto window=reinterpret_cast<HWND>(target_.window);
            RECT client{};POINT origin{};
            if(!GetClientRect(window,&client) || !ClientToScreen(window,&origin) || client.right<2 || client.bottom<2)return false;
            const int x=target_.left+int(point.first*(target_.width-1)+.5f),y=target_.top+int(point.second*(target_.height-1)+.5f);
            // WGC captures the outer frame. Never stretch it into the client area.
            if(x<origin.x || y<origin.y || x>=origin.x+client.right || y>=origin.y+client.bottom)return false;
            if(GetAncestor(WindowFromPoint({x,y}),GA_ROOT)!=GetAncestor(window,GA_ROOT))return false;
            point={float(x-origin.x)/(client.right-1),float(y-origin.y)/(client.bottom-1)};
        }
        if(event.kind==Kind::Pointer)return injector_.InjectMouseMove(point.first,point.second);
        if(event.kind==Kind::Button)return injector_.InjectMouseButton(RemoteInputInjector::MouseButton(event.button),event.down,point.first,point.second);
        if(event.kind==Kind::Wheel)return injector_.InjectMouseMove(point.first,point.second) && injector_.InjectMouseScroll(event.wheelX,event.wheelY);
        return false;
    }
    void Release() noexcept override {injector_.ReleaseAllInjectedInput();injector_.SetMouseMonitoringEnabled(false);}
};
}
std::shared_ptr<Sink> CreateWindowsDesktopSink(std::shared_ptr<DesktopTargetState> target) {
    return std::make_shared<DesktopSink>(std::move(target),CreateWindowsGamepadSink(),
        [](auto geometry,auto caps){return std::make_unique<WindowsDesktopDevice>(geometry,caps);});
}
}
