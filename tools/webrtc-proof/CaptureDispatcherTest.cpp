#include "capture/WindowsCaptureDispatcher.h"
#include "core/WindowsMediaRuntime.h"
#include <future>
#include <stdexcept>
#include <iostream>
#include <memory>

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void RunStaRuntime() {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
    const auto requireSta = [] {
        APTTYPE type{};
        APTTYPEQUALIFIER qualifier{};
        Require(SUCCEEDED(CoGetApartmentType(&type, &qualifier)) &&
            (type == APTTYPE_STA || type == APTTYPE_MAINSTA), "Media runtime changed the UI apartment");
    };
    {
        screenshare::WindowsMediaRuntime runtime;
        Require(SUCCEEDED(runtime.result()), "Media runtime initialization failed");
        requireSta();
        for (int cycle = 0; cycle < 20; ++cycle) {
            std::async(std::launch::async, [] {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
                screenshare::WindowsMediaRuntime nested;
                Require(SUCCEEDED(nested.result()), "Nested media runtime initialization failed");
            }).get();
        }
        requireSta();
    }
    requireSta();
}
void Run() {
    using screenshare::WindowsCaptureDispatcher;
    using winrt::Windows::System::DispatcherQueue;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
    Require(!DispatcherQueue::GetForCurrentThread(), "Unexpected preexisting dispatcher");
    for (int cycle = 0; cycle < 20; ++cycle) {
        {
            auto owner = std::make_unique<WindowsCaptureDispatcher>();
            auto queue = DispatcherQueue::GetForCurrentThread();
            Require(bool(queue), "Capture dispatcher missing");
            { WindowsCaptureDispatcher borrowed; }
            bool called = false;
            Require(queue.TryEnqueue([&] { called = true; }), "Borrowed dispatcher shut down its caller's queue");
            Require(WindowsCaptureDispatcher::Wait([&] { return called; },
                std::chrono::steady_clock::now() + std::chrono::seconds(1)), "Queued callback was not dispatched");
            PostQuitMessage(37);
            WindowsCaptureDispatcher::Pump();
            MSG message{};
            Require(PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) && message.wParam == 37,
                "Message pump swallowed or changed WM_QUIT");
            Require(!WindowsCaptureDispatcher::Wait([] { return false; },
                std::chrono::steady_clock::now() + std::chrono::milliseconds(1)), "Unsignaled wait incorrectly completed");
            // A live source switch starts its replacement before retiring the
            // original. The replacement must keep their shared queue alive.
            auto replacement = std::make_unique<WindowsCaptureDispatcher>();
            owner.reset();
            Require(bool(DispatcherQueue::GetForCurrentThread()), "Source retirement shut down the replacement's dispatcher");
            called = false;
            Require(queue.TryEnqueue([&] { called = true; }), "Replacement queue stopped accepting callbacks");
            Require(WindowsCaptureDispatcher::Wait([&] { return called; },
                std::chrono::steady_clock::now() + std::chrono::seconds(1)), "Replacement callback was not dispatched");
        }
        Require(!DispatcherQueue::GetForCurrentThread(), "Owned dispatcher remained after shutdown");
    }
    // An externally owned queue is still borrowed, never shut down by capture.
    winrt::Windows::System::DispatcherQueueController external{nullptr};
    DispatcherQueueOptions options{sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
    winrt::check_hresult(CreateDispatcherQueueController(options,
        reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(external))));
    auto queue = DispatcherQueue::GetForCurrentThread();
    { WindowsCaptureDispatcher borrowed; }
    bool called = false;
    Require(queue.TryEnqueue([&] { called = true; }), "Capture shut down an external queue");
    Require(WindowsCaptureDispatcher::Wait([&] { return called; },
        std::chrono::steady_clock::now() + std::chrono::seconds(1)), "External callback was not dispatched");
    auto shutdown = external.ShutdownQueueAsync();
    Require(WindowsCaptureDispatcher::Wait([&] { return shutdown.Status() != winrt::Windows::Foundation::AsyncStatus::Started; },
        std::chrono::steady_clock::now() + std::chrono::seconds(1)), "External dispatcher shutdown timed out");
    shutdown.GetResults();
    shutdown.Close();
}
}
int main() {
    try { RunStaRuntime(); Run(); std::cout << "Media runtime STA compatibility and worker restarts; capture dispatcher ownership, delivery, quit preservation and deadlines passed.\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
