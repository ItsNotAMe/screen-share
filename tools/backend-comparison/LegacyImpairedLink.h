#pragma once
#include "../webrtc-proof/ImpairedPacketSocket.h"
#include "api/environment/environment_factory.h"
#include <stdexcept>

namespace proof {
// Loopback-only legacy UDP relay. Host -> viewer traverses the exact same
// ImpairedPacketSocket used by v2. Feedback is forwarded without impairment.
// Neither encrypted payloads nor legacy protocol messages are interpreted.
class LegacyImpairedLink {
    std::unique_ptr<webrtc::Thread> thread_ = webrtc::Thread::CreateWithSocketServer();
    std::unique_ptr<webrtc::AsyncPacketSocket> ingress_, receiver_;
    webrtc::SocketAddress host_;
    std::shared_ptr<LinkControl> control_;
public:
    LegacyImpairedLink(uint16_t front, uint16_t destination, std::shared_ptr<LinkControl> control)
        : control_(std::move(control)) {
        if (!thread_ || !thread_->Start()) throw std::runtime_error("Legacy impairment relay thread failed");
        try {
            thread_->BlockingCall([&] {
                const auto environment = webrtc::CreateEnvironment();
                webrtc::BasicPacketSocketFactory factory(thread_->socketserver());
                auto ingress = factory.CreateUdpSocket(environment, webrtc::SocketAddress("127.0.0.1", front), 0, 0);
                receiver_ = factory.CreateUdpSocket(environment, webrtc::SocketAddress("127.0.0.1", 0), 0, 0);
                if (!ingress || !receiver_) throw std::runtime_error("Legacy impairment relay bind failed");
                ingress_ = std::make_unique<ImpairedPacketSocket>(std::move(ingress), control_);
                ingress_->RegisterReceivedPacketCallback([this, destination](auto*, const auto& packet) {
                    if (host_.IsNil()) host_ = packet.source_address();
                    if (packet.source_address() != host_) { control_->invalid = true; return; }
                    const auto data = packet.payload();
                    if (receiver_->SendTo(data.data(), data.size(), webrtc::SocketAddress("127.0.0.1", destination), {}) != int(data.size()))
                        control_->invalid = true;
                });
                receiver_->RegisterReceivedPacketCallback([this, destination](auto*, const auto& packet) {
                    if (packet.source_address() != webrtc::SocketAddress("127.0.0.1", destination) || host_.IsNil()) {
                        control_->invalid = true; return;
                    }
                    const auto data = packet.payload();
                    if (ingress_->SendTo(data.data(), data.size(), host_, {}) != int(data.size())) control_->invalid = true;
                });
            });
        } catch (...) { Stop(); throw; }
    }
    ~LegacyImpairedLink() { Stop(); }
    void Stop() {
        if (!thread_) return;
        thread_->BlockingCall([&] { ingress_.reset(); receiver_.reset(); });
        thread_->Stop(); thread_.reset();
    }
};
}
