#pragma once

#include "H264StreamEncoder.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace screenshare {

struct UdpSenderConfig {
    std::string host;
    uint16_t port = 0;
    uint32_t maxPayloadBytes = 1'200;
};

struct UdpSenderStats {
    uint64_t framesSent = 0;
    uint64_t datagramsSent = 0;
    uint64_t payloadBytesSent = 0;
    uint64_t wireBytesSent = 0;
};

class UdpSender {
public:
    UdpSender();
    ~UdpSender();

    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;

    void Open(const UdpSenderConfig& config);
    void Close();
    void SendFrame(const EncodedPacket& packet);

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] const UdpSenderStats& stats() const noexcept { return stats_; }

private:
    void SendDatagram(const std::byte* payload, uint32_t payloadBytes, uint16_t fragmentIndex, uint16_t fragmentCount, const EncodedPacket& packet);

    uintptr_t socket_ = 0;
    std::vector<std::byte> address_;
    int addressLength_ = 0;
    UdpSenderConfig config_{};
    UdpSenderStats stats_{};
    uint64_t frameId_ = 0;
    bool winsockStarted_ = false;
};

UdpSenderConfig ParseUdpSenderTarget(const std::string& target);

} // namespace screenshare
