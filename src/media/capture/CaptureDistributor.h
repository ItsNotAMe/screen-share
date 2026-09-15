#pragma once
#include "CaptureSession.h"
#include <memory>

namespace screenshare::media {
struct CaptureDeliveryStats {
    uint64_t delivered = 0, replaced = 0, rejected = 0;
    std::chrono::nanoseconds maxHandoffAge{};
    bool failed = false;
};
// One distributor per session, with a single replaceable frame per viewer.
// Publish is capture-thread safe; membership/Stop calls are serialized by the
// coordinator. Stop capture first, then Stop this distributor before releasing
// consumer state. A consumer must never remove/join itself from its callback.
class CaptureDistributor final {
public:
    explicit CaptureDistributor(uint64_t session);
    ~CaptureDistributor();
    CaptureDistributor(const CaptureDistributor&) = delete;
    CaptureDistributor& operator=(const CaptureDistributor&) = delete;
    void Add(uint64_t viewer, CaptureSession::Deliver);
    void Remove(uint64_t viewer);
    void Publish(CaptureSample);
    CaptureDeliveryStats stats(uint64_t viewer) const;
    void Stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
