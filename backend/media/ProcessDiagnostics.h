#pragma once
#include "DiagnosticHistory.h"
#include <Windows.h>
#include <psapi.h>

namespace screenshare::media {
class ProcessDiagnostics {
    uint64_t previousCpu_ = 0;
    std::chrono::steady_clock::time_point previousAt_{};
public:
    void Sample(DiagnosticRecord& record) {
        const auto process = GetCurrentProcess();
        PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
        if (K32GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
            record.numbers["workingSetBytes"] = double(memory.WorkingSetSize);
            record.numbers["privateBytes"] = double(memory.PrivateUsage);
        }
        DWORD handles = 0;
        if (GetProcessHandleCount(process, &handles)) record.numbers["processHandles"] = handles;
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
            const uint64_t cpu = ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
                ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
            const auto now = std::chrono::steady_clock::now();
            if (previousAt_ != std::chrono::steady_clock::time_point{} && cpu >= previousCpu_) {
                const double elapsed = std::chrono::duration<double>(now - previousAt_).count();
                if (elapsed > 0) record.numbers["processCpuPercentAllCores"] =
                    double(cpu - previousCpu_) / 100000.0 / elapsed / std::max(1ul, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
            }
            previousCpu_ = cpu; previousAt_ = now;
        }
    }
};
}
