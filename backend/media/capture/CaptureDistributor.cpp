#include "CaptureDistributor.h"
#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace screenshare::media {
struct CaptureDistributor::Impl {
    struct Viewer {
        mutable std::mutex mutex;
        std::condition_variable wake;
        std::optional<CaptureSample> pending;
        CaptureDeliveryStats stats;
        bool stopped = false;
        std::thread worker;
        explicit Viewer(CaptureSession::Deliver deliver) {
            worker = std::thread([this, deliver = std::move(deliver)] {
                std::unique_lock lock(mutex);
                for (;;) {
                    wake.wait(lock, [&] { return stopped || pending.has_value(); });
                    if (stopped) return;
                    auto sample = std::move(*pending);
                    pending.reset();
                    stats.maxHandoffAge = std::max(stats.maxHandoffAge,
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - sample.capturedAt));
                    lock.unlock();
                    bool failed = false;
                    try { deliver(std::move(sample)); } catch (...) { failed = true; }
                    lock.lock();
                    if (failed) {
                        stats.failed = true;
                        stopped = true;
                        pending.reset();
                        return;
                    }
                    ++stats.delivered;
                }
            });
        }
        ~Viewer() { Stop(); }
        void Stop() {
            { std::lock_guard lock(mutex); stopped = true; pending.reset(); }
            wake.notify_one();
            if (worker.joinable()) worker.join();
        }
        void Publish(const CaptureSample& sample) {
            std::lock_guard lock(mutex);
            if (stopped) { ++stats.rejected; return; }
            if (pending) ++stats.replaced;
            pending = sample;
            wake.notify_one();
        }
    };
    explicit Impl(uint64_t value) : session(value) {}
    mutable std::mutex mutex;
    const uint64_t session;
    uint64_t generation = 0, sequence = 0;
    bool stopped = false;
    std::map<uint64_t, std::shared_ptr<Viewer>> viewers;
};
CaptureDistributor::CaptureDistributor(uint64_t session) : impl_(std::make_unique<Impl>(session)) {
    if (!session) throw std::invalid_argument("Missing capture session identity");
}
CaptureDistributor::~CaptureDistributor() { Stop(); }
void CaptureDistributor::Add(uint64_t viewer, CaptureSession::Deliver deliver) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopped || !viewer || !deliver || impl_->viewers.contains(viewer) || impl_->viewers.size() >= 63)
        throw std::invalid_argument("Invalid capture subscription");
    impl_->viewers.emplace(viewer, std::make_shared<Impl::Viewer>(std::move(deliver)));
}
void CaptureDistributor::Remove(uint64_t viewer) {
    std::shared_ptr<Impl::Viewer> removed;
    {
        std::lock_guard lock(impl_->mutex);
        auto found = impl_->viewers.find(viewer);
        if (found == impl_->viewers.end()) return;
        removed = std::move(found->second);
        impl_->viewers.erase(found);
    }
    removed->Stop();
}
void CaptureDistributor::Publish(CaptureSample sample) {
    std::lock_guard lock(impl_->mutex);
    // Device generations are monotonic within this session. Sequence numbers
    // continue across device recovery, as assigned by CaptureSession.
    if (impl_->stopped) return;
    if (!sample.resource || sample.session != impl_->session || !sample.generation ||
        sample.generation < impl_->generation || sample.sequence <= impl_->sequence) {
        for (const auto& [id, viewer] : impl_->viewers) {
            std::lock_guard viewerLock(viewer->mutex); ++viewer->stats.rejected;
        }
        return;
    }
    impl_->generation = sample.generation;
    impl_->sequence = sample.sequence;
    for (const auto& [id, viewer] : impl_->viewers) viewer->Publish(sample);
}
CaptureDeliveryStats CaptureDistributor::stats(uint64_t viewer) const {
    std::lock_guard lock(impl_->mutex);
    auto found = impl_->viewers.find(viewer);
    if (found == impl_->viewers.end()) throw std::out_of_range("Unknown capture viewer");
    std::lock_guard viewerLock(found->second->mutex);
    return found->second->stats;
}
void CaptureDistributor::Stop() {
    std::vector<std::shared_ptr<Impl::Viewer>> stopping;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopped = true;
        for (const auto& [id, viewer] : impl_->viewers) stopping.push_back(viewer);
    }
    for (const auto& viewer : stopping) viewer->Stop();
}
}
