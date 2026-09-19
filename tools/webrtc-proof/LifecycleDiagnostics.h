#pragma once
#include <windows.h>
#include <psapi.h>
#include <chrono>
#include <cstdio>

namespace proof {
// Diagnostic-only scopes: declare immediately before the resources they observe.
struct TeardownMarker {
    const char* name;
    ~TeardownMarker() { std::fprintf(stderr, "Teardown complete: %s\n", name); std::fflush(stderr); }
};
inline void LifecycleSample(int cycle, double seconds, const char* label = "LIFECYCLE") {
    PROCESS_MEMORY_COUNTERS_EX memory{}; memory.cb = sizeof(memory);
    DWORD handles = 0;
    const bool memoryOk = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    const bool handlesOk = GetProcessHandleCount(GetCurrentProcess(), &handles);
    std::fprintf(stderr, "%s {\"cycle\":%d,\"seconds\":%.6f,\"privateBytes\":", label, cycle, seconds);
    if (memoryOk) std::fprintf(stderr, "%llu", static_cast<unsigned long long>(memory.PrivateUsage));
    else std::fputs("null", stderr);
    std::fputs(",\"workingSetBytes\":", stderr);
    if (memoryOk) std::fprintf(stderr, "%llu", static_cast<unsigned long long>(memory.WorkingSetSize));
    else std::fputs("null", stderr);
    std::fputs(",\"handles\":", stderr);
    if (handlesOk) std::fprintf(stderr, "%lu", handles); else std::fputs("null", stderr);
    std::fprintf(stderr, ",\"gdiObjects\":%lu,\"userObjects\":%lu}\n",
        GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS));
    std::fflush(stderr);
}
inline LONG WINAPI ReportUnhandledException(EXCEPTION_POINTERS* exception) {
    std::fprintf(stderr, "Unhandled native exception: 0x%08lX\n", exception->ExceptionRecord->ExceptionCode);
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
}
