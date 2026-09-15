#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace screenshare::media {
// Control/signaling-executor membership owner. Feed complete, authenticated
// snapshots (already validated by room transport), never raw network deltas.
// Hooks must not reenter; Add owns cleanup on failure, Remove must not throw.
// Actual native peers/capture subscriptions remain behind these lifecycle hooks.
class RoomPeerRoster final {
public:
    enum class Result { Applied, Ignored, Invalid };
    using Add = std::function<bool(const std::string&)>;
    using Remove = std::function<void(const std::string&)>;
    RoomPeerRoster(Add add, Remove remove) : add_(std::move(add)), remove_(std::move(remove)) {}
    ~RoomPeerRoster() { Clear(); }
    RoomPeerRoster(const RoomPeerRoster&) = delete;
    RoomPeerRoster& operator=(const RoomPeerRoster&) = delete;
    Result Apply(uint64_t generation, uint64_t revision, std::vector<std::string> peers) {
        if (!generation || peers.size() > 63) return Result::Invalid;
        std::sort(peers.begin(), peers.end());
        if (std::adjacent_find(peers.begin(), peers.end()) != peers.end()) return Result::Invalid;
        for (const auto& id : peers) if (id.empty() || id.size() > 128 ||
            !std::all_of(id.begin(), id.end(), [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; })) return Result::Invalid;
        if (generation < generation_ || (generation == generation_ && (suspended_ || (revision_ && revision <= *revision_)))) return Result::Ignored;
        if (generation > generation_) { Clear(); generation_ = generation; revision_.reset(); suspended_ = false; }
        for (auto it = active_.begin(); it != active_.end();) {
            if (!std::binary_search(peers.begin(), peers.end(), it->first)) {
                if (it->second) remove_(it->first);
                it = active_.erase(it);
            } else ++it;
        }
        for (const auto& id : peers) if (!active_.contains(id)) {
            bool added = false;
            try { added = add_(id); } catch (...) {}
            // Failed peers stay terminal until they leave/rejoin; unrelated
            // profile/policy revisions must not cause offer/retry storms.
            active_.emplace(id, added);
        }
        revision_ = revision;
        return Result::Applied;
    }
    void TransportLost(uint64_t generation) {
        if (generation < generation_) return;
        Clear(); generation_ = generation; revision_.reset(); suspended_ = true;
    }
    size_t activeCount() const { return std::count_if(active_.begin(), active_.end(), [](const auto& p) { return p.second; }); }
    size_t failedCount() const { return active_.size() - activeCount(); }
private:
    void Clear() { for (const auto& [id, active] : active_) if (active) remove_(id); active_.clear(); }
    Add add_; Remove remove_;
    std::map<std::string, bool> active_;
    uint64_t generation_ = 0;
    std::optional<uint64_t> revision_;
    bool suspended_ = false;
};
}
