#pragma once
#include "input/v2/DesktopSink.h"
#include "media/capture/SyntheticCaptureSource.h"
#include <atomic>
struct DesktopInputEvidence {
    std::atomic<unsigned> applied{0},released{0},keys{0},wheels{0};
    std::atomic<bool> healthy{true};
    std::atomic<float> x{0},y{0};
};
class RecordingDesktopDevice final:public screenshare::input::DesktopDevice {
    std::shared_ptr<DesktopInputEvidence> state_;
public:
    explicit RecordingDesktopDevice(std::shared_ptr<DesktopInputEvidence> state):state_(std::move(state)){}
    bool Healthy() override {return state_->healthy;}
    bool Apply(const screenshare::input::Event& event) override {
        state_->x=event.x;state_->y=event.y;
        if(event.kind==screenshare::input::Kind::Key)++state_->keys;
        if(event.kind==screenshare::input::Kind::Wheel)++state_->wheels;
        ++state_->applied;return true;
    }
    void Release() noexcept override {++state_->released;}
};
// Synthetic source with the same capture-publication contract as Windows, never
// a fake receiver-side mapping. The metadata crosses the real H.264 bitstream.
class MappedSyntheticCapture final:public screenshare::media::ICaptureSource {
    screenshare::media::SyntheticCaptureSource source_;
    std::shared_ptr<screenshare::input::DesktopTargetState> target_;
    screenshare::input::DesktopTarget geometry_;
    inline static std::atomic<uint64_t> next_{0};
public:
    MappedSyntheticCapture(int width,int height,int fps,std::shared_ptr<screenshare::input::DesktopTargetState> target)
        :source_(width,height,fps),target_(std::move(target)),geometry_{++next_,0,0,0,0,width,height}{}
    ~MappedSyntheticCapture() override {target_->Invalidate(geometry_.source);}
    void Start() override {source_.Start();}
    std::optional<screenshare::media::CaptureSample> Poll() override {
        auto sample=source_.Poll();if(sample)sample->resource->inputGeneration=target_->Publish(geometry_);return sample;
    }
    bool Closed() const override {return false;}
    void Retire() noexcept override {target_->Invalidate(geometry_.source);}
    void Rebuild() override {target_->Invalidate(geometry_.source);source_.Rebuild();}
};
