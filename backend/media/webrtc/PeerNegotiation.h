#pragma once
#include "api/peer_connection_interface.h"
#include <cstdint>
#include <future>
#include <memory>
#include <string>

namespace screenshare::media {
enum class NegotiationError { None, StaleGeneration, Busy, Closed, Cancelled, Invalid, CreateFailed, ApplyFailed };
struct NegotiationResult {
    uint64_t operation = 0, generation = 0;
    NegotiationError error = NegotiationError::None;
    std::string sdp;
    int rtcErrorType = 0, rtcErrorDetail = 0;
};
// Private WebRTC adapter boundary. Invoke every method, including destruction,
// on the peer's signaling executor. Operations return immediately; futures may
// be observed elsewhere. One operation at a time, no unbounded internal queue.
// Close cancels completion delivery; it does not roll back native SDP already
// applied. The peer owner must close the connection when cancelling its lifetime.
class PeerNegotiation final {
public:
    PeerNegotiation(webrtc::scoped_refptr<webrtc::PeerConnectionInterface>, uint64_t generation);
    ~PeerNegotiation();
    PeerNegotiation(const PeerNegotiation&) = delete;
    PeerNegotiation& operator=(const PeerNegotiation&) = delete;
    std::future<NegotiationResult> CreateLocal(uint64_t generation, bool offer, bool iceRestart = false);
    std::future<NegotiationResult> ApplyRemote(uint64_t generation, bool offer, std::string sdp);
    const std::string& localUsername() const;
    void Close();
private:
    struct State;
    std::shared_ptr<State> state_;
};
}
