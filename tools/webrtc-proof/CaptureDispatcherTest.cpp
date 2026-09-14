#include "capture/WindowsCaptureDispatcher.h"
#include <stdexcept>
#include <iostream>

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Run() {
    using screenshare::WindowsCaptureDispatcher;
    using winrt::Windows::System::DispatcherQueue;
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
    Require(!DispatcherQueue::GetForCurrentThread(), "Unexpected preexisting dispatcher");
    for (int cycle = 0; cycle < 20; ++cycle) {
        {
            WindowsCaptureDispatcher owner;
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
        }
        Require(!DispatcherQueue::GetForCurrentThread(), "Owned dispatcher remained after shutdown");
    }
}
}
int main() {
    try { Run(); std::cout << "Capture dispatcher ownership, delivery, quit preservation and deadlines passed.\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
