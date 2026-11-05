#pragma once
#include "HostShareStatus.h"
#include <algorithm>
#include <chrono>
#include <span>
#include <string>
#include <vector>

namespace screenshare::media {
// A separate message on the existing bidirectional telemetry channel. Old
// viewers ignore host messages; old hosts simply leave this observation absent.
// Network byte order, 14 + connection-ID bytes (maximum 142). No credentials,
// source titles/handles or remote clocks are exposed.
struct HostShareMessage {
    std::string connection;
    uint64_t sequence = 0;
    HostShareState state = HostShareState::Live;
};
inline std::vector<uint8_t> EncodeHostShare(const HostShareMessage& value) {
    if (value.connection.empty() || value.connection.size() > 128 || !value.sequence ||
        uint8_t(value.state) > uint8_t(HostShareState::Recovering)) return {};
    std::vector<uint8_t> bytes{'S','H','S',1,uint8_t(value.connection.size()),uint8_t(value.state)};
    for (unsigned i = 8; i; --i) bytes.push_back(uint8_t(value.sequence >> ((i-1)*8)));
    bytes.insert(bytes.end(),value.connection.begin(),value.connection.end());
    return bytes;
}
inline std::optional<HostShareMessage> DecodeHostShare(std::span<const uint8_t> bytes) {
    if (bytes.size() < 15 || bytes.size() > 142 || bytes[0]!='S' || bytes[1]!='H' || bytes[2]!='S' ||
        bytes[3]!=1 || !bytes[4] || bytes[4]>128 || bytes.size()!=14u+bytes[4] ||
        bytes[5]>uint8_t(HostShareState::Recovering)) return {};
    HostShareMessage message;
    for (size_t i=6; i<14; ++i) message.sequence=(message.sequence<<8)|bytes[i];
    if (!message.sequence) return {};
    message.connection.assign(reinterpret_cast<const char*>(bytes.data()+14),bytes[4]);
    message.state=HostShareState(bytes[5]);return message;
}
// Signaling-owner only. Connection binding and sequence validation reject old
// negotiations/reordered packets. Only new accepted packets renew the lease.
class HostShareInbox {
public:
    using Clock=std::chrono::steady_clock;
    void Bind(const std::string& connection) {
        if (connection_==connection) return;
        connection_=connection;sequence_=0;value_.reset();sampled_={};nextWindow_={};accepted_=0;
    }
    bool Receive(std::span<const uint8_t> bytes,Clock::time_point now=Clock::now()) {
        if (connection_.empty()) return false;
        if (now>=nextWindow_) {nextWindow_=now+std::chrono::seconds(1);accepted_=0;}
        if (accepted_++>=8) {accepted_=8;return false;}
        const auto message=DecodeHostShare(bytes);
        if (!message || message->connection!=connection_ || message->sequence<=sequence_) return false;
        sequence_=message->sequence;value_=message->state;sampled_=now;return true;
    }
    HostShareStatus Read(Clock::time_point now=Clock::now()) const {
        const bool stale=value_ && now-sampled_>=std::chrono::seconds(3);
        const auto age=std::chrono::duration_cast<std::chrono::seconds>(now-sampled_).count();
        return {stale?std::nullopt:value_,stale,value_?std::optional(uint32_t(std::clamp<int64_t>(age,0,UINT32_MAX))):std::nullopt};
    }
private:
    std::string connection_;
    uint64_t sequence_=0;
    std::optional<HostShareState> value_;
    Clock::time_point sampled_{},nextWindow_{};
    unsigned accepted_=0;
};
}
