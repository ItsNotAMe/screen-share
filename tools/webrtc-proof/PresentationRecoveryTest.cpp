#include "CaptureTestWindow.h"
#include "render/PresentationRecovery.h"
#include <iostream>
#include <vector>
#include <string_view>

using namespace screenshare;
using namespace screenshare::media;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

int main(int argc, char** argv) try {
    PresentationRecovery recovery;
    auto now = PresentationRecovery::Clock::now();
    int draws = 0, resets = 0;
    auto reset = [&] { ++resets; };
    auto lost = [&]() -> bool { ++draws; throw PresentationError(DXGI_ERROR_DEVICE_REMOVED, "injected removal"); };
    Require(!recovery.Present(lost, reset, now), "Failed frame was accepted");
    Require(!recovery.Present(lost, reset, now), "Backoff failed");
    Require(draws == 1 && resets == 1, "Backoff touched the failed device");
    now += std::chrono::milliseconds(250);
    Require(recovery.Present([] { return true; }, reset, now), "Fresh frame did not resume");
    for (int i = 0; i < 2; ++i) {
        Require(!recovery.Present(lost, reset, now), "Loss accepted");
        now += std::chrono::milliseconds(250);
    }
    bool failed = false;
    try { recovery.Present(lost, reset, now); } catch (const PresentationError&) { failed = true; }
    Require(failed && resets == 4 && recovery.recoveries() == 3, "Recovery budget not enforced");
    failed = false;
    try { recovery.Present([] { return true; }, reset, now); } catch (const std::runtime_error&) { failed = true; }
    Require(failed, "Terminal failure resumed");
    PresentationRecovery invalid;
    failed = false;
    try { invalid.Present([]() -> bool { throw PresentationError(E_INVALIDARG, "bad argument"); }, reset, now); }
    catch (const PresentationError&) { failed = true; }
    Require(failed && resets == 4, "Unrelated failure triggered recovery");

    if (argc < 2 || std::string_view(argv[1]) != "--gpu") {
        std::cout << "Presentation recovery policy passed (desktop GPU proof: --gpu)\n";
        return 0;
    }

    // Exercise real resource destruction/recreation on an owned generated HWND.
    // Inject only the HRESULT boundary: this never resets the user's GPU/driver.
    proof::TestWindow window(false);
    window.Invoke([&] {
        Nv12D3D11Presenter presenter;
        presenter.SetLowLatency(true);
        presenter.Attach(window.handle());
        std::vector<uint8_t> pixels(320 * 180 * 3 / 2, 128);
        Nv12D3D11Presenter::FrameView frame{320, 180, pixels.data(), pixels.size()};
        PresentationRecovery actual;
        auto time = PresentationRecovery::Clock::now();
        auto render = [&] { return presenter.TryPresent(frame); };
        render();
        Require(!actual.Present(lost, [&] { presenter.Reset(); }, time), "Injected loss accepted");
        Require(presenter.maximumFrameLatency() == 0, "Old GPU resources retained");
        time += std::chrono::milliseconds(250);
        bool accepted = false;
        for (int attempt = 0; attempt < 30 && !accepted; ++attempt) {
            accepted = actual.Present(render, [&] { presenter.Reset(); }, time);
            if (!accepted) Sleep(5);
        }
        Require(accepted, "Recreated device did not accept a fresh frame");
        Require(presenter.maximumFrameLatency() == 1, "Rebuild lost low-latency settings");
        Require(actual.recoveries() == 1, "Unexpected rebuild count");
    });
    std::cout << "Presentation recovery: bounded retries, backoff, terminal/error isolation and GPU resource rebuild passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
}
