#include "input/v2/DesktopSink.h"
#include "codec/InputMappingSei.h"
#include <iostream>
#include <stdexcept>
#include <source_location>
using namespace screenshare::input;
void Check(bool value,std::source_location where=std::source_location::current()) {
    if(!value)throw std::runtime_error("Desktop input assertion at "+std::to_string(where.line()));
}
struct Evidence {unsigned applied=0,released=0;bool healthy=true,fail=false;};
class Device final:public DesktopDevice {
    std::shared_ptr<Evidence> evidence_;
public:
    explicit Device(std::shared_ptr<Evidence> evidence):evidence_(std::move(evidence)){}
    bool Healthy() override {return evidence_->healthy;}
    bool Apply(const Event&) override {++evidence_->applied;return !evidence_->fail;}
    void Release() noexcept override {++evidence_->released;}
};
int main() {try {
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
    Check(!sink.Grant("viewer",Keyboard,0));Check(sink.Grant("viewer",Mouse,0));
    evidence->healthy=false;Check(!sink.Healthy("viewer"));sink.Release("viewer");
    evidence->healthy=true;Check(sink.Grant("viewer",Mouse,0));state->Invalidate();Check(!sink.Healthy("viewer"));sink.Release("viewer");
    Check(evidence->released==3);
    std::cout<<"{\"passed\":true,\"desktop_mapping\":true,\"physical_input\":false}\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
