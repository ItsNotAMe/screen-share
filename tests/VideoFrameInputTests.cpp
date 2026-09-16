#include "ui/VideoFrameWidget.h"

#include <QtGui/QKeyEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>

#include <iostream>
#include <vector>
#include "render/Nv12D3D11Presenter.h"
#include <dxgi.h>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <source_location>

namespace {
using namespace std::chrono_literals;
struct RendererEvidence {
    std::atomic<HRESULT> failure{S_OK};
    std::atomic<unsigned> calls{0}, resets{0};
    std::atomic_bool destroyed{false}, wrongThread{false};
    std::atomic_bool block{false}, entered{false};
    std::thread::id owner;
};
class TestRenderer final : public FramePresentationBackend {
    std::shared_ptr<RendererEvidence> state_;
    void CheckThread() { if (std::this_thread::get_id() != state_->owner) state_->wrongThread = true; }
public:
    explicit TestRenderer(std::shared_ptr<RendererEvidence> state) : state_(std::move(state)) { state_->owner = std::this_thread::get_id(); }
    ~TestRenderer() override { CheckThread(); state_->destroyed = true; }
    bool Present(HWND, uint32_t, uint32_t, bool, bool, const screenshare::Nv12D3D11Presenter::FrameView& frame, screenshare::Nv12D3D11Presenter::ScaleMode) override {
        CheckThread(); ++state_->calls;
        if (state_->block) {
            state_->entered = true;
            while (state_->block) std::this_thread::sleep_for(1ms);
        }
        if (FAILED(state_->failure.load())) throw screenshare::PresentationError(state_->failure, "Injected renderer failure");
        if (frame.dataSize != 6) throw std::runtime_error("Invalid test pixels");
        return true;
    }
    void Update(HWND, uint32_t, uint32_t, bool, bool, screenshare::Nv12D3D11Presenter::ScaleMode) override { CheckThread(); }
    void Reset() noexcept override { CheckThread(); ++state_->resets; }
    uint32_t MaximumFrameLatency() const noexcept override { return 1; }
};
template<class F> void Await(F predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Presentation worker timed out");
        std::this_thread::sleep_for(1ms);
    }
}
void Require(bool value, std::source_location where = std::source_location::current()) {
    if (!value) throw std::runtime_error("Presentation recovery assertion failed at line " + std::to_string(where.line()));
}
void PresentationRecoveryScenario() {
    auto state = std::make_shared<RendererEvidence>();
    const auto mainThread = std::this_thread::get_id();
    {
        VideoFrameWidget widget(nullptr, [state] { return std::make_unique<TestRenderer>(state); });
        widget.setLowLatency(true);
        auto send = [&] {
            screenshare::Nv12VideoFrame frame; frame.width = frame.height = 2; frame.nv12.resize(6);
            Require(widget.presentVideoFrameAsync(std::move(frame)));
            Await([&] { auto stats = widget.presentationStats(); return stats.enqueuedFrames == stats.presentedFrames + stats.droppedFrames; });
        };
        send(); Require(widget.presentationStats().presentedFrames == 1);
        Require(state->owner != mainThread);
        // A stalled renderer still has only one replaceable pending frame;
        // replaced retained buffers are released immediately by the handoff.
        state->block = true;
        screenshare::Nv12VideoFrame blocked; blocked.width = blocked.height = 2; blocked.nv12.resize(6);
        Require(widget.presentVideoFrameAsync(std::move(blocked)));
        Await([&] { return state->entered.load(); });
        bool bounded = true;
        std::weak_ptr<const uint8_t> previous;
        for (int frame = 0; frame < 1000; ++frame) {
            auto owner = std::shared_ptr<const uint8_t>(new uint8_t[6]{}, std::default_delete<const uint8_t[]>());
            screenshare::Nv12VideoFrame pending; pending.width = pending.height = 2;
            pending.retainedPixels = owner; pending.retainedBytes = 6;
            bounded &= widget.presentVideoFrameAsync(std::move(pending));
            bounded &= previous.expired();
            previous = owner;
            bounded &= widget.presentationStats().queuedFrames == 1;
        }
        state->block = false; // Always release before assertions or widget destruction.
        Await([&] { auto stats = widget.presentationStats(); return stats.enqueuedFrames == stats.presentedFrames + stats.droppedFrames; });
        Await([&] { return previous.expired(); });
        Require(bounded && widget.presentationStats().droppedFrames == 999);
        for (unsigned failure = 1; failure <= 3; ++failure) {
            state->failure = DXGI_ERROR_DEVICE_REMOVED;
            send(); Require(widget.presentationStats().recoveries == failure);
            const auto calls = state->calls.load();
            // The actual worker drops new frames during backoff without invoking
            // the renderer or retaining a failed frame for a later retry.
            for (int frame = 0; frame < 10; ++frame) send();
            Require(state->calls == calls);
            state->failure = S_OK;
            std::this_thread::sleep_for(260ms);
            send(); Require(!widget.presentationStats().terminal);
        }
        state->failure = DXGI_ERROR_DEVICE_RESET;
        send(); Await([&] { return widget.presentationStats().terminal; });
        const auto calls = state->calls.load();
        for (int frame = 0; frame < 100; ++frame) send();
        Require(state->calls == calls && widget.presentationStats().recoveries == 3);
        // Only an explicit session clear refreshes the lifetime recovery budget.
        widget.clearFrame(); Await([&] { return !widget.presentationStats().terminal; });
        state->failure = S_OK; send();
        Require(widget.presentationStats().recoveries == 0);
        state->failure = E_INVALIDARG; send();
        Await([&] { return widget.presentationStats().terminal; });
        Require(widget.presentationStats().recoveries == 0);
        widget.clearFrame(); Await([&] { return !widget.presentationStats().terminal; });
        state->failure = DXGI_ERROR_DEVICE_HUNG; send();
        // Destruction below occurs during backoff; it must join without a timer
        // sleep and destroy the backend on its owning worker.
    }
    Require(state->destroyed && !state->wrongThread);
}
}

namespace {

bool Check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    try { PresentationRecoveryScenario(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    VideoFrameWidget widget;
    std::vector<screenshare::RemoteInputEvent> inputs;
    widget.setRemoteInputHandler([&](const screenshare::RemoteInputEvent& input) {
        inputs.push_back(input);
    });
    widget.setControlCapture(true, true, true);

    QKeyEvent press(
        QEvent::KeyPress,
        Qt::Key_A,
        Qt::NoModifier,
        0x1e,
        0x41,
        0,
        QStringLiteral("a"),
        false,
        1);
    QApplication::sendEvent(&widget, &press);
    QKeyEvent repeatPress(
        QEvent::KeyPress,
        Qt::Key_A,
        Qt::NoModifier,
        0x1e,
        0x41,
        0,
        QStringLiteral("a"),
        true,
        1);
    QApplication::sendEvent(&widget, &repeatPress);
    QKeyEvent repeatRelease(
        QEvent::KeyRelease,
        Qt::Key_A,
        Qt::NoModifier,
        0x1e,
        0x41,
        0,
        QStringLiteral("a"),
        true,
        1);
    QApplication::sendEvent(&widget, &repeatRelease);
    QKeyEvent release(
        QEvent::KeyRelease,
        Qt::Key_A,
        Qt::NoModifier,
        0x1e,
        0x41,
        0,
        QStringLiteral("a"),
        false,
        1);
    QApplication::sendEvent(&widget, &release);

    bool ok = Check(inputs.size() == 3, "auto-repeat release is suppressed");
    ok &= Check(
        inputs.size() == 3 &&
            inputs[0].kind == screenshare::RemoteInputKind::Key &&
            inputs[0].pressed &&
            inputs[1].pressed &&
            !inputs[2].pressed,
        "key repeat keeps one held key until the physical release");

    inputs.clear();
    QWheelEvent precisionWheel(
        QPointF(10, 10),
        QPointF(10, 10),
        QPoint(0, 15),
        QPoint(),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::ScrollUpdate,
        false);
    QApplication::sendEvent(&widget, &precisionWheel);
    ok &= Check(
        inputs.size() == 1 &&
            inputs[0].kind == screenshare::RemoteInputKind::MouseScroll &&
            inputs[0].scrollX == 0 &&
            inputs[0].scrollY == 120,
        "pixel-only precision scrolling converts to Win32 wheel units");

    inputs.clear();
    QWheelEvent highResolutionWheel(
        QPointF(10, 10),
        QPointF(10, 10),
        QPoint(0, 15),
        QPoint(0, 30),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::ScrollUpdate,
        false);
    QApplication::sendEvent(&widget, &highResolutionWheel);
    ok &= Check(
        inputs.size() == 1 && inputs[0].scrollY == 30,
        "high-resolution angle delta is preserved when available");

    return ok ? 0 : 1;
}
