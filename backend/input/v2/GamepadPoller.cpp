#include "GamepadPoller.h"
#include <stdexcept>

namespace screenshare::input {
GamepadPoller::GamepadPoller(std::shared_ptr<Port> port, std::string peer, Read read)
    : port_(std::move(port)), peer_(std::move(peer)) {
    if (!port_ || peer_.empty() || !read) throw std::invalid_argument("Controller polling requires a selected device and peer");
    for (const auto& state : port_->Read())
        if (state.peer == peer_ && (state.granted & Gamepad)) permission_ = state.permission;
    if (!permission_) return; // Grant disappeared before this owner started.
    worker_ = std::jthread([this, read = std::move(read)](std::stop_token stop) {
        std::vector<uint8_t> last;
        while (!stop.stop_requested()) {
            bool granted = false;
            for (const auto& state : port_->Read()) if (state.peer == peer_ && state.permission == permission_ && (state.granted & Gamepad)) granted = true;
            if (!granted) break;
            try {
                const auto state = read();
                if (!state || state->kind != Kind::Pad || !Valid(*state)) break;
                const auto bytes = Encode({"local", 1, 1, *state});
                if (bytes != last && !port_->SubmitIfCurrent(peer_, permission_, *state)) break;
                last = bytes;
            } catch (...) { break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        port_->RevokeIfCurrent(peer_, permission_);
    });
}
GamepadPoller::~GamepadPoller() {
    worker_.request_stop();
    if (worker_.joinable()) worker_.join();
    port_->RevokeIfCurrent(peer_, permission_);
}
}
