#pragma once
#include <windows.h>
#include <DispatcherQueue.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>

namespace screenshare {
// WGC's free-threaded frame pool does not remove the capture item's dispatcher
// requirement. This object, and all capture operations, belong to one thread.
class WindowsCaptureDispatcher {
public:
    WindowsCaptureDispatcher() {
        if (winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) return;
        DispatcherQueueOptions options{sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
        winrt::check_hresult(CreateDispatcherQueueController(options,
            reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(controller_))));
    }
    ~WindowsCaptureDispatcher() {
        // Never shut down a queue supplied by the caller. An owned queue must
        // finish shutdown while its thread and COM apartment still exist.
        if (!controller_) return;
        try {
            auto shutdown = controller_.ShutdownQueueAsync();
            Wait([&] { return shutdown.Status() != winrt::Windows::Foundation::AsyncStatus::Started; },
                std::chrono::steady_clock::time_point::max());
            shutdown.GetResults();
            shutdown.Close();
        } catch (const winrt::hresult_error& error) {
            std::fprintf(stderr, "Capture dispatcher shutdown failed: 0x%08X\n", static_cast<unsigned>(error.code().value));
        }
    }
    WindowsCaptureDispatcher(const WindowsCaptureDispatcher&) = delete;
    WindowsCaptureDispatcher& operator=(const WindowsCaptureDispatcher&) = delete;

    static void Pump() { RepostQuit quit; Drain(quit); }
    template<class Predicate>
    static bool Wait(Predicate complete, std::chrono::steady_clock::time_point deadline) {
        RepostQuit quit;
        for (;;) {
            Drain(quit);
            if (complete()) return true;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;
            const DWORD wait = deadline == std::chrono::steady_clock::time_point::max() ? INFINITE :
                static_cast<DWORD>(std::max<int64_t>(1,
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()));
            if (MsgWaitForMultipleObjectsEx(0, nullptr, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED)
                winrt::throw_last_error();
        }
    }
private:
    struct RepostQuit {
        std::optional<int> code;
        ~RepostQuit() { if (code) PostQuitMessage(*code); }
    };
    static void Drain(RepostQuit& quit) {
        MSG message{};
        // Bound each batch so a busy queue cannot starve capture or its deadline.
        for (unsigned count = 0; count < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
            if (message.message == WM_QUIT) { quit.code = static_cast<int>(message.wParam); continue; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    winrt::Windows::System::DispatcherQueueController controller_{nullptr};
};
}
