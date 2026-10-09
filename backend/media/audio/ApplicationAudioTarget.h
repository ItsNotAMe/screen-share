#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace screenshare::media {
struct AudioProcessIdentity {
    uint32_t id = 0, parent = 0;
    uint64_t created = 0;
};
// Only follow live, age-ordered parent links. A recycled parent PID must not
// attach an unrelated process to the selected application's capture scope.
inline std::vector<uint32_t> ApplicationAudioFamily(uint32_t root, std::span<const AudioProcessIdentity> processes) {
    const auto find = [&](uint32_t id) {
        return std::find_if(processes.begin(), processes.end(), [=](const auto& p) { return p.id == id; });
    };
    const auto owner = find(root);
    if (owner == processes.end() || !owner->created) return {};
    std::vector<uint32_t> family{root};
    for (size_t i = 0; i < family.size(); ++i) {
        const auto parent = find(family[i]);
        for (const auto& child : processes) {
            if (child.parent == parent->id && child.created >= parent->created &&
                std::find(family.begin(), family.end(), child.id) == family.end()) family.push_back(child.id);
        }
    }
    return family;
}
// Capture the smallest process subtree containing the app's render sessions.
// Windows can omit WebView2 audio when loopback targets its non-rendering host.
inline uint32_t ApplicationAudioTarget(uint32_t root, std::span<const AudioProcessIdentity> processes,
                                       std::span<const uint32_t> renderers) {
    const auto family = ApplicationAudioFamily(root, processes);
    std::vector<std::vector<uint32_t>> paths;
    for (const auto id : renderers) {
        if (std::find(family.begin(), family.end(), id) == family.end()) continue;
        auto& path = paths.emplace_back();
        auto current = id;
        while (true) {
            path.push_back(current);
            if (current == root) break;
            const auto process = std::find_if(processes.begin(), processes.end(), [=](const auto& p) { return p.id == current; });
            current = process->parent; // Membership above guarantees a path to root.
        }
    }
    if (paths.empty()) return root;
    for (const auto candidate : paths.front()) {
        if (std::all_of(paths.begin(), paths.end(), [&](const auto& path) {
            return std::find(path.begin(), path.end(), candidate) != path.end();
        })) return candidate;
    }
    return root;
}
}
