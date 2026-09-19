#pragma once
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace screenshare::room::wire {
// Confined to the networking owner thread. One instance per subscription.
class RevisionTracker {
public:
    enum class Decision { Ignore, Apply, Resync };
    static constexpr std::uint64_t MaxRevision = 9007199254740991ULL;
    std::uint64_t Start() { Advance(); active_ = true; revision_.reset(); pending_ = false; return generation_; }
    void Stop() { Advance(); active_ = false; revision_.reset(); pending_ = false; }
    Decision Snapshot(std::uint64_t generation, std::uint64_t revision) {
        if (!Current(generation, revision) || (revision_ && revision < *revision_)) return Decision::Ignore;
        revision_ = revision; pending_ = false; return Decision::Apply;
    }
    Decision Delta(std::uint64_t generation, std::uint64_t revision) {
        if (!Current(generation, revision) || pending_ || (revision_ && revision <= *revision_)) return Decision::Ignore;
        if (!revision_ || revision != *revision_ + 1) { pending_ = true; return Decision::Resync; }
        revision_ = revision; return Decision::Apply;
    }
    std::optional<std::uint64_t> Revision() const { return revision_; }
    Decision RequestResync(std::uint64_t generation) {
        if (!active_ || generation != generation_ || pending_) return Decision::Ignore;
        pending_ = true; return Decision::Resync;
    }
private:
    void Advance() {
        if (generation_ == MaxRevision) throw std::overflow_error("Subscription generation exhausted");
        ++generation_;
    }
    bool Current(std::uint64_t generation, std::uint64_t revision) const {
        return active_ && generation == generation_ && revision <= MaxRevision;
    }
    std::uint64_t generation_ = 0;
    std::optional<std::uint64_t> revision_;
    bool active_ = false;
    bool pending_ = false;
};
}
