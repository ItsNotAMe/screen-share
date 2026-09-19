#pragma once
#include "input/v2/InputService.h"
#include <atomic>
class RecordingGamepadSink final : public screenshare::input::Sink {
public:
    std::atomic<unsigned> applied{0}, released{0};
    std::atomic<uint16_t> buttons{0};
    std::atomic<bool> fail{false};
    bool Grant(const std::string&, uint8_t caps, int) override { return caps == screenshare::input::Gamepad && !fail; }
    bool Apply(const std::string&, const screenshare::input::Event& event) override {
        if (event.kind != screenshare::input::Kind::Pad) return false;
        buttons = event.buttons; ++applied; return !fail;
    }
    void Release(const std::string&) noexcept override { buttons = 0; ++released; }
};
