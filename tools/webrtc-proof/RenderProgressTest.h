#pragma once
#include "media/audio/PcmRenderLoop.h"
#include "media/audio/PlaybackControl.h"
#include <iostream>
#include <set>
#include <thread>
#include <vector>

namespace render_progress_test {
using namespace screenshare::media;
using namespace std::chrono_literals;
inline void Check(bool value, const char* failure) { if (!value) throw std::runtime_error(failure); }
inline void Boundaries() {
    auto now = std::chrono::steady_clock::time_point{};
    unsigned writes = 0;
    bool stalled = false;
    try {
        WritePcmRenderBlock({}, [] { return 0u; }, [&](auto, auto) { ++writes; }, [&] {
            now += 10ms;
            // Bound the negative proof too; the old loop never expires.
            Check(now <= std::chrono::steady_clock::time_point{} + 2s, "Render buffer wait remained unbounded");
        }, [&] { return now; });
    } catch (const PcmRenderStalled&) { stalled = true; }
    Check(stalled && writes == 0 && now == std::chrono::steady_clock::time_point{} + 1s,
        "A full render buffer did not expire at its progress deadline");
    // Neither early event notifications nor tiny writes may extend the block's
    // deadline. A stuck engine that drains one frame at a time is also bounded.
    now = {}; unsigned available = 0, frames = 0; stalled = false;
    try {
        WritePcmRenderBlock({}, [&] { return available; }, [&](auto, auto count) { frames += count; available = 0; },
            [&] { now += 100ms; available = 1; }, [&] { return now; });
    } catch (const PcmRenderStalled&) { stalled = true; }
    Check(stalled && frames == 9 && now == std::chrono::steady_clock::time_point{} + 1s,
        "Tiny render writes renewed a wedged engine indefinitely");

    // Partial capacity, exact sample order, and the last instant before timeout.
    now = {}; frames = 0; available = 0;
    PcmBlock input{}, rendered{};
    for (unsigned i = 0; i < input.size(); ++i) input[i] = int16_t(i - 480);
    WritePcmRenderBlock({}, [&] { return available; }, [&](auto offset, auto count) {
        Check(offset == frames, "Partial render writes skipped/replayed frames"); frames += count;
        std::copy_n(input.begin() + offset * 2, count * 2, rendered.begin() + offset * 2);
    }, [&] { now += 999ms; available = 127; }, [&] { return now; });
    Check(frames == 480 && input == rendered && now == std::chrono::steady_clock::time_point{} + 999ms,
        "Healthy partial render writes changed samples or expired early");

    // Fresh capacity wins after a long scheduling gap; don't rebuild a healthy
    // engine simply because its owner was not scheduled for a while.
    now = {}; frames = 0;
    WritePcmRenderBlock({}, [&] { now += 5s; return 960u; }, [&](auto, auto count) { frames += count; },
        [] { Check(false, "Ready render buffer waited for an event"); }, [&] { return now; });
    Check(frames == 480, "Delayed owner rejected a fresh ready render buffer");

    // Stop wins at the deadline and after the native padding call, without
    // committing any PCM or reporting a device failure.
    now = {}; frames = 0; std::stop_source stop;
    WritePcmRenderBlock(stop.get_token(), [] { return 0u; }, [&](auto, auto count) { frames += count; },
        [&] { now += 1s; stop.request_stop(); }, [&] { return now; });
    std::stop_source duringPadding;
    WritePcmRenderBlock(duringPadding.get_token(), [&] { duringPadding.request_stop(); return 480u; },
        [&](auto, auto count) { frames += count; }, [] {}, [&] { return now; });
    WritePcmRenderBlock(stop.get_token(), []() -> unsigned { Check(false, "Stopped render queried the device"); return 0; },
        [&](auto, auto count) { frames += count; }, [] {}, [&] { return now; });
    Check(frames == 0, "Render wrote samples after cancellation");
}
struct State {
    unsigned created = 0, live = 0;
    bool wrongThread = false;
    std::set<unsigned> stalled;
    std::vector<int16_t> samples;
    std::chrono::steady_clock::time_point now{};
};
class Output final : public PcmPlayoutEndpoint {
    std::shared_ptr<State> state_;
    std::thread::id owner_ = std::this_thread::get_id();
    unsigned id_;
public:
    explicit Output(std::shared_ptr<State> state) : state_(std::move(state)), id_(++state_->created) { ++state_->live; }
    ~Output() override { state_->wrongThread |= owner_ != std::this_thread::get_id(); --state_->live; }
    void Start() override {}
    void Write(const PcmBlock& block, std::stop_token stop) override {
        WritePcmRenderBlock(stop, [&] { return state_->stalled.contains(id_) ? 0u : 480u; },
            [&](auto offset, auto frames) { state_->samples.insert(state_->samples.end(), block.begin() + offset * 2, block.begin() + (offset + frames) * 2); },
            [&] { state_->now += 10ms; }, [&] { return state_->now; });
    }
    uint32_t DelayMs() const override { return 0; }
    uint32_t BufferFrames() const override { return 2400; }
};
inline void Recovery() {
    auto state = std::make_shared<State>();
    auto factory = [state] { return std::make_unique<Output>(state); };
    auto control = std::make_shared<PlaybackControl>(PlaybackSelection{L"pinned",25,false}, factory);
    {
        ControlledPcmPlayout output(control, [&] { return state->now; }); output.Start();
        PcmBlock block{}; block.fill(2400); output.Write(block, {});
        Check(state->samples.size() == 960 && state->samples.back() == 600, "Initial output gain failed");
        state->stalled.insert(1); state->samples.clear(); output.Write(block, {});
        Check(control->Status().health.state == AudioEndpointState::Failed && control->Status().health.failures == 1 &&
            state->live == 0 && state->samples.empty(), "Render stall did not retire its failed output");
        state->now += 249ms; output.Write(block, {});
        Check(state->created == 1, "Render stall retried before its backoff");
        state->now += 1ms; output.Write(block, {});
        Check(state->created == 2 && state->live == 1 && state->samples.size() == 960 && state->samples.back() == 0 &&
            control->Status().health.state == AudioEndpointState::Running && control->Status().revision == 1 &&
            control->Status().selected.deviceId == L"pinned", "Render recovery lost settings or replayed old PCM");
        state->samples.clear(); block.fill(3200); output.Write(block, {});
        Check(state->samples.size() == 960 && state->samples.back() == 800, "Recovered output did not receive fresh PCM");
        auto mute = control->Submit({L"pinned",25,true}, factory); output.Write(block, {});
        Check(mute.get().error == AudioUpdateError::None, "Mute update failed");
        state->stalled.insert(2); output.Write(block, {});
        Check(state->live == 0 && control->Status().health.failures == 2, "Muted render stall was hidden");
        state->now += 250ms; output.Write(block, {}); state->samples.clear(); output.Write(block, {});
        Check(state->created == 3 && state->samples.size() == 960 && state->samples.back() == 0 &&
            control->Status().selected.muted && control->Status().revision == 2, "Render recovery lost mute state");
        control->Close(); std::stop_source stop; stop.request_stop(); output.Write(block, stop.get_token());
        Check(control->Status().health.state == AudioEndpointState::Inactive, "Stopped playback revived recovery");
    }
    Check(state->live == 0 && !state->wrongThread, "Render recovery leaked output ownership");
}
inline void Run() {
    Boundaries(); Recovery();
    std::cout << "Render stalls: bounded full/partial writes, delayed owner, exact deadline, cancellation, pinned recovery, fresh PCM and mute passed\n";
}
}
