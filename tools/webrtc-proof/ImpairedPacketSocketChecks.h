#pragma once
#include "ImpairedPacketSocket.h"

namespace proof {
// In-memory native socket seam checks the wrapper independently of successful
// media negotiation: loss accounting, duplicate delivery, bounded retention and
// closing with queued packets. It never opens a listener or sends physical input.
class InjectedPacketSocket final : public webrtc::AsyncPacketSocket {
    webrtc::SocketAddress address_{"127.0.0.1", 1234};
public:
    void Receive(std::span<const uint8_t> data) { NotifyPacketReceived(webrtc::ReceivedIpPacket(data, address_)); }
    webrtc::SocketAddress GetLocalAddress() const override { return address_; }
    webrtc::SocketAddress GetRemoteAddress() const override { return address_; }
    int Send(const void*, size_t size, const webrtc::AsyncSocketPacketOptions&) override { return int(size); }
    int SendTo(const void*, size_t size, const webrtc::SocketAddress&, const webrtc::AsyncSocketPacketOptions&) override { return int(size); }
    int Close() override { return 0; }
    State GetState() const override { return STATE_BOUND; }
    int GetOption(webrtc::Socket::Option, int*) override { return -1; }
    int SetOption(webrtc::Socket::Option, int) override { return 0; }
    int GetError() const override { return 0; }
    void SetError(int) override {}
};

inline void CheckImpairedPacketSocket() {
    auto thread = webrtc::Thread::Create(); Check(thread->Start());
    auto link = std::make_shared<LinkControl>(12345);
    std::unique_ptr<ImpairedPacketSocket> wrapped;
    InjectedPacketSocket* injected = nullptr;
    std::atomic<unsigned> received{0};
    auto send = [&](unsigned count) {
        std::array<uint8_t, 1200> packet{};
        for (unsigned i = 0; i < count; ++i) injected->Receive(packet);
    };
    thread->BlockingCall([&] {
        auto socket = std::make_unique<InjectedPacketSocket>(); injected = socket.get();
        wrapped = std::make_unique<ImpairedPacketSocket>(std::move(socket), link);
        wrapped->RegisterReceivedPacketCallback([&](auto*, const auto& packet) { Check(packet.payload().size() == 1200); ++received; });
        auto config = link->Read().network; config.loss_percent = 100; link->Set(config);
        send(20);
    });
    try {
        Wait([&] { return link->lost == 20; });
        Check(!received && !link->queued && !link->bytes);
        thread->BlockingCall([&] {
            auto config = link->Read().network; config.loss_percent = 0; link->Set(config, 1); send(20);
        });
        Wait([&] { return received == 40; });
        Check(link->duplicated == 20 && !link->queued && !link->bytes);
        thread->BlockingCall([&] {
            auto config = link->Read().network; config.queue_delay_ms = 1000; link->Set(config); send(300);
            Check(link->queued == 256 && link->bytes == 256 * 1200 && link->overflow == 44);
            wrapped->Close(); Check(!link->queued && !link->bytes);
            wrapped.reset(); Check(!link->liveSockets && !link->invalid);
        });
    } catch (...) { thread->BlockingCall([&] { wrapped.reset(); }); thread->Stop(); throw; }
    thread->Stop();
}
}
