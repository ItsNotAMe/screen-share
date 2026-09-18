#pragma once
#include "p2p/base/basic_packet_socket_factory.h"
#include "test/network/simulated_network.h"
#include "rtc_base/task_utils/repeating_task.h"
#include "rtc_base/thread.h"
#include "rtc_base/time_utils.h"
#include <atomic>
#include <map>
#include <mutex>

namespace proof {
// Test-only ingress link. SRTP, DTLS/SCTP, STUN and RTCP remain real packets;
// neither the crypto nor signaling is replaced. No machine-wide shaping.
struct LinkControl {
    struct Settings { webrtc::BuiltInNetworkBehaviorConfig network; unsigned duplicateEvery = 0; uint64_t revision = 1; };
    mutable std::mutex mutex;
    Settings settings;
    const uint64_t seed;
    std::atomic<uint64_t> received{0}, delivered{0}, deliveredBytes{0}, lost{0}, overflow{0}, duplicated{0}, reordered{0};
    std::atomic<uint64_t> queued{0}, bytes{0}, peakQueued{0}, peakBytes{0}, liveSockets{0}, socketSerial{0};
    std::atomic<uint64_t> maximumSchedulingDelayUs{0};
    std::atomic<bool> invalid{false};
    explicit LinkControl(uint64_t value) : seed(value) { settings.network.link_capacity = webrtc::DataRate::KilobitsPerSec(20000); settings.network.queue_length_packets = 256; }
    Settings Read() const { std::lock_guard lock(mutex); return settings; }
    void Set(webrtc::BuiltInNetworkBehaviorConfig config, unsigned duplicateEvery = 0) {
        std::lock_guard lock(mutex); settings.network = config; settings.duplicateEvery = duplicateEvery; ++settings.revision;
    }
    static void Peak(std::atomic<uint64_t>& peak, uint64_t value) {
        auto old = peak.load(); while (old < value && !peak.compare_exchange_weak(old, value)) {}
    }
};

class ImpairedPacketSocket final : public webrtc::AsyncPacketSocket {
    struct Packet { std::vector<uint8_t> data; webrtc::SocketAddress source; webrtc::ReceivedIpPacket::DecryptionInfo decryption; uint64_t sequence; };
    std::unique_ptr<webrtc::AsyncPacketSocket> socket_;
    std::shared_ptr<LinkControl> control_;
    webrtc::SimulatedNetwork network_;
    std::map<uint64_t, Packet> packets_;
    uint64_t next_ = 0, revision_ = 0, sequence_ = 0, lastDelivered_ = 0, bytes_ = 0;
    unsigned duplicateEvery_ = 0;
    bool closed_ = false;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    webrtc::RepeatingTaskHandle pump_;
    void Configure() {
        const auto config = control_->Read();
        if (config.revision == revision_) return;
        network_.SetConfig(config.network, webrtc::Timestamp::Micros(webrtc::TimeMicros()));
        duplicateEvery_ = config.duplicateEvery; revision_ = config.revision;
    }
    void Enqueue(const webrtc::ReceivedIpPacket& packet, uint64_t sequence) {
        constexpr size_t maxBytes = 2 * 1024 * 1024;
        if (packets_.size() >= 256 || packet.payload().size() > 65536 || bytes_ + packet.payload().size() > maxBytes) { ++control_->overflow; return; }
        const auto id = ++next_;
        if (!network_.EnqueuePacket(webrtc::PacketInFlightInfo(packet.payload().size(), webrtc::TimeMicros(), id, packet.ecn()))) { ++control_->overflow; return; }
        Packet stored{{packet.payload().begin(), packet.payload().end()}, packet.source_address(), packet.decryption_info(), sequence};
        bytes_ += stored.data.size(); control_->bytes += stored.data.size(); ++control_->queued;
        packets_.emplace(id, std::move(stored));
        LinkControl::Peak(control_->peakQueued, control_->queued); LinkControl::Peak(control_->peakBytes, control_->bytes);
    }
    void Pump() {
        if (closed_) return;
        Configure();
        const auto alive = alive_;
        for (const auto& delivery : network_.DequeueDeliverablePackets(webrtc::TimeMicros())) {
            auto at = packets_.find(delivery.packet_id);
            if (at == packets_.end()) { control_->invalid = true; continue; }
            auto packet = std::move(at->second); packets_.erase(at);
            bytes_ -= packet.data.size(); control_->bytes -= packet.data.size(); --control_->queued;
            if (delivery.receive_time_us == webrtc::PacketDeliveryInfo::kNotReceived) { ++control_->lost; continue; }
            if (packet.sequence < lastDelivered_) ++control_->reordered;
            lastDelivered_ = std::max(lastDelivered_, packet.sequence);
            ++control_->delivered; control_->deliveredBytes += packet.data.size();
            // Match upstream LinkEmulation::Process: the model owns arrival
            // timestamps. Polling wake-up jitter must not become an additional,
            // unconfigured congestion signal. Record that lateness separately.
            LinkControl::Peak(control_->maximumSchedulingDelayUs,
                uint64_t(std::max<int64_t>(0, webrtc::TimeMicros() - delivery.receive_time_us)));
            NotifyPacketReceived(webrtc::ReceivedIpPacket(packet.data, packet.source,
                webrtc::Timestamp::Micros(delivery.receive_time_us), delivery.ecn, packet.decryption));
            if (!*alive) return;
        }
    }
    void Retire() {
        if (closed_) return;
        closed_ = true; *alive_ = false; pump_.Stop();
        control_->queued -= packets_.size(); control_->bytes -= bytes_; packets_.clear(); bytes_ = 0;
    }
public:
    ImpairedPacketSocket(std::unique_ptr<webrtc::AsyncPacketSocket> socket, std::shared_ptr<LinkControl> control)
        : socket_(std::move(socket)), control_(std::move(control)), network_(control_->Read().network, control_->seed + control_->socketSerial++) {
        ++control_->liveSockets;
        socket_->RegisterReceivedPacketCallback([this](auto*, const auto& packet) {
            if (closed_) return;
            Configure(); ++control_->received; const auto sequence = ++sequence_;
            Enqueue(packet, sequence);
            if (duplicateEvery_ && sequence % duplicateEvery_ == 0) { Enqueue(packet, sequence); ++control_->duplicated; }
        });
        socket_->SubscribeSentPacket(this, [this](auto*, const auto& packet) { NotifySentPacket(this, packet); });
        socket_->SubscribeReadyToSend(this, [this](auto*) { NotifyReadyToSend(this); });
        socket_->SubscribeAddressReady(this, [this](auto*, const auto& address) { NotifyAddressReady(this, address); });
        socket_->SubscribeConnect(this, [this](auto*) { NotifyConnect(this); });
        socket_->SubscribeCloseEvent(this, [this](auto*, int error) { NotifyClosed(error); });
        pump_ = webrtc::RepeatingTaskHandle::DelayedStart(webrtc::Thread::Current(), webrtc::TimeDelta::Millis(1),
            [this] { Pump(); return webrtc::TimeDelta::Millis(1); }, webrtc::TaskQueueBase::DelayPrecision::kHigh);
    }
    ~ImpairedPacketSocket() override {
        Retire(); socket_->DeregisterReceivedPacketCallback();
        socket_->UnsubscribeSentPacket(this); socket_->UnsubscribeReadyToSend(this); socket_->UnsubscribeAddressReady(this);
        socket_->UnsubscribeConnect(this); socket_->UnsubscribeCloseEvent(this); socket_->Close(); --control_->liveSockets;
    }
    webrtc::SocketAddress GetLocalAddress() const override { return socket_->GetLocalAddress(); }
    webrtc::SocketAddress GetRemoteAddress() const override { return socket_->GetRemoteAddress(); }
    int Send(const void* data, size_t size, const webrtc::AsyncSocketPacketOptions& options) override { return socket_->Send(data, size, options); }
    int SendTo(const void* data, size_t size, const webrtc::SocketAddress& address, const webrtc::AsyncSocketPacketOptions& options) override { return socket_->SendTo(data, size, address, options); }
    int Close() override { Retire(); return socket_->Close(); }
    State GetState() const override { return socket_->GetState(); }
    int GetOption(webrtc::Socket::Option option, int* value) override { return socket_->GetOption(option, value); }
    int SetOption(webrtc::Socket::Option option, int value) override { return socket_->SetOption(option, value); }
    int GetError() const override { return socket_->GetError(); }
    void SetError(int error) override { socket_->SetError(error); }
};

class ImpairedPacketFactory final : public webrtc::BasicPacketSocketFactory {
    std::shared_ptr<LinkControl> control_;
public:
    ImpairedPacketFactory(webrtc::SocketFactory* sockets, std::shared_ptr<LinkControl> control)
        : BasicPacketSocketFactory(sockets), control_(std::move(control)) {}
    std::unique_ptr<webrtc::AsyncPacketSocket> CreateUdpSocket(const webrtc::Environment& env, const webrtc::SocketAddress& address, uint16_t low, uint16_t high) override {
        if (control_->liveSockets >= 8) { control_->invalid = true; return {}; }
        auto socket = BasicPacketSocketFactory::CreateUdpSocket(env, address, low, high);
        return socket ? std::make_unique<ImpairedPacketSocket>(std::move(socket), control_) : nullptr;
    }
    // This scenario deliberately tests UDP. A TCP/DTLS fallback must not bypass
    // the impairment and turn a failed path into an apparent successful test.
    std::unique_ptr<webrtc::AsyncListenSocket> CreateServerTcpSocket(const webrtc::Environment&, const webrtc::SocketAddress&, uint16_t, uint16_t, int) override { return {}; }
    std::unique_ptr<webrtc::AsyncPacketSocket> CreateClientTcpSocket(const webrtc::Environment&, const webrtc::SocketAddress&, const webrtc::SocketAddress&, const webrtc::PacketSocketTcpOptions&) override { return {}; }
    std::unique_ptr<webrtc::AsyncPacketSocket> CreateClientUdpSocket(const webrtc::Environment&, const webrtc::SocketAddress&, const webrtc::SocketAddress&, uint16_t, uint16_t, const webrtc::PacketSocketTcpOptions&) override { return {}; }
};
}
