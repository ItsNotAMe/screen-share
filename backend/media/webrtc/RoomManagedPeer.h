#pragma once
#include "RoomPeerNegotiation.h"
#include "media/HostPeerRegistry.h"

namespace screenshare::media {
// Bridges authenticated negotiation into scheduled peer recovery/retirement.
// References remain alive until retire runs, after capture delivery is joined by
// HostPeerRegistry. All methods/destruction belong to the signaling executor.
// retire must not throw. It may publish retirement for an outer owner to consume.
class RoomManagedPeer final : public IMediaPeer {
public:
    using FreshId = std::function<std::string(uint64_t)>;
    RoomManagedPeer(PeerConnectionLifecycle& lifecycle, RoomPeerNegotiation& negotiation,
                    std::future<HostOperationResult> attachment, std::string initialId,
                    FreshId freshId, std::function<void()> retire)
        : lifecycle_(lifecycle), negotiation_(negotiation), attachment_(std::move(attachment)), initialId_(std::move(initialId)),
          freshId_(std::move(freshId)), retire_(std::move(retire)) {
        if (!attachment_.valid() || initialId_.empty() || !freshId_ || !retire_)
            throw std::invalid_argument("Managed room peer requires attachment and lifecycle hooks");
    }
    ~RoomManagedPeer() override { Close(); retire_(); }
    PeerConnectionLifecycle& lifecycle() noexcept override { return lifecycle_; }
    bool RequestIceRestart(uint64_t revision) override {
        return !closed_ && negotiation_.Offer(freshId_(revision), true);
    }
    bool Poll() override {
        if (closed_ || negotiation_.closed()) return false;
        if (!attachment_.valid()) return true;
        if (attachment_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return true;
        return attachment_.get().error == HostOperationError::None && negotiation_.Offer(std::move(initialId_));
    }
    void Close() noexcept override {
        if (closed_) return;
        closed_ = true; negotiation_.Close();
    }
private:
    PeerConnectionLifecycle& lifecycle_;
    RoomPeerNegotiation& negotiation_;
    std::future<HostOperationResult> attachment_;
    std::string initialId_;
    FreshId freshId_;
    std::function<void()> retire_;
    bool closed_ = false;
};
}
