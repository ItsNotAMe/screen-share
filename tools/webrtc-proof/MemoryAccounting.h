#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>

namespace proof {
// Explicit diagnostic only, after media shutdown. Never compact/decommit memory
// to improve a result. Do not allocate, format output or acquire other locks
// while walking a locked heap. Unsupported/incomplete walks remain visible.
inline void MemoryAccountingSample(int cycle, bool idle) {
    uint64_t busy = 0, free = 0, overhead = 0, blocks = 0, regionCommitted = 0;
    uint64_t privateCommitted = 0, mappedCommitted = 0, imageCommitted = 0;
    bool complete = true;
    std::array<HANDLE, 256> heaps{};
    const auto count = GetProcessHeaps(DWORD(heaps.size()), heaps.data());
    const auto deadline = GetTickCount64() + 5000;
    if (!count || count > heaps.size()) complete = false;
    else for (DWORD i = 0; i < count; ++i) {
        if (GetTickCount64() >= deadline) { complete = false; break; }
        if (!HeapLock(heaps[i])) { complete = false; continue; }
        PROCESS_HEAP_ENTRY entry{};
        while (HeapWalk(heaps[i], &entry)) {
            if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) { busy += entry.cbData; overhead += entry.cbOverhead; ++blocks; }
            else if (entry.wFlags & PROCESS_HEAP_REGION) regionCommitted += entry.Region.dwCommittedSize;
            else if (!(entry.wFlags & PROCESS_HEAP_UNCOMMITTED_RANGE)) { free += entry.cbData; overhead += entry.cbOverhead; }
            if (GetTickCount64() >= deadline) { complete = false; break; }
        }
        if (GetLastError() != ERROR_NO_MORE_ITEMS) complete = false;
        if (!HeapUnlock(heaps[i])) complete = false;
    }
    // VirtualQuery classifies committed regions, not allocation call sites.
    // Image copy-on-write/private accounting can differ from PrivateUsage.
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    auto address = uintptr_t(system.lpMinimumApplicationAddress);
    const auto maximum = uintptr_t(system.lpMaximumApplicationAddress);
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))) { complete = false; break; }
        if (region.State == MEM_COMMIT) {
            if (region.Type == MEM_PRIVATE) privateCommitted += region.RegionSize;
            else if (region.Type == MEM_MAPPED) mappedCommitted += region.RegionSize;
            else if (region.Type == MEM_IMAGE) imageCommitted += region.RegionSize;
        }
        const auto next = uintptr_t(region.BaseAddress) + region.RegionSize;
        if (next <= address || GetTickCount64() >= deadline) { complete = false; break; }
        address = next;
    }
    std::fprintf(stderr, "MEMORY_ACCOUNTING {\"cycle\":%d,\"stage\":\"%s\",\"complete\":%s,\"heaps\":%lu,"
        "\"heapBusyBytes\":%llu,\"heapFreeBytes\":%llu,\"heapOverheadBytes\":%llu,\"heapBusyBlocks\":%llu,"
        "\"heapRegionCommittedBytes\":%llu,\"privateCommittedBytes\":%llu,\"mappedCommittedBytes\":%llu,\"imageCommittedBytes\":%llu}\n",
        cycle, idle ? "idle" : "cycle", complete ? "true" : "false", count,
        static_cast<unsigned long long>(busy), static_cast<unsigned long long>(free), static_cast<unsigned long long>(overhead),
        static_cast<unsigned long long>(blocks), static_cast<unsigned long long>(regionCommitted),
        static_cast<unsigned long long>(privateCommitted), static_cast<unsigned long long>(mappedCommitted), static_cast<unsigned long long>(imageCommitted));
    std::fflush(stderr);
}
}
