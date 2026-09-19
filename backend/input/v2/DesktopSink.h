#pragma once
#include "DesktopTarget.h"
#include "InputService.h"
#include <functional>
#include <map>

namespace screenshare::input {
class DesktopDevice {
public:
    virtual ~DesktopDevice()=default;
    virtual bool Healthy()=0;
    virtual bool Apply(const Event&)=0;
    virtual void Release() noexcept=0;
};
class DesktopSink final : public Sink {
public:
    using Factory=std::function<std::unique_ptr<DesktopDevice>(DesktopTarget,uint8_t)>;
    DesktopSink(std::shared_ptr<DesktopTargetState>,std::shared_ptr<Sink> gamepads,Factory);
    bool Grant(const std::string&,uint8_t,int) override;
    bool Apply(const std::string&,const Event&) override;
    bool Healthy(const std::string&) override;
    void Release(const std::string&) noexcept override;
private:
    struct Owner { uint8_t capabilities; uint64_t generation; std::unique_ptr<DesktopDevice> device; };
    std::shared_ptr<DesktopTargetState> target_;
    std::shared_ptr<Sink> pads_;
    Factory factory_;
    std::map<std::string,Owner> owners_;
};
std::shared_ptr<Sink> CreateWindowsDesktopSink(std::shared_ptr<DesktopTargetState>);
}
