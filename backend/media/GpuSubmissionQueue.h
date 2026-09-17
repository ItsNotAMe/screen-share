#pragma once
#include <deque>
#include <stdexcept>
#include <utility>

namespace screenshare::media {
// FIFO completion tokens for work already submitted to the GPU. A completion
// probe must be nonblocking; the caller drops new work when Ready returns false.
template<class Completion> class GpuSubmissionQueue {
    std::deque<Completion> pending_;
public:
    static constexpr unsigned Limit = 4;
    template<class Probe> bool Ready(Probe complete) {
        while (!pending_.empty() && complete(pending_.front())) pending_.pop_front();
        return pending_.size() < Limit;
    }
    void Submitted(Completion completion) {
        if (pending_.size() >= Limit) throw std::logic_error("Unbounded GPU submission");
        pending_.push_back(std::move(completion));
    }
    unsigned size() const { return unsigned(pending_.size()); }
};
}
