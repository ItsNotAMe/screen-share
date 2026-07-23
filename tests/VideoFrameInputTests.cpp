#include "ui/VideoFrameWidget.h"

#include <QtGui/QKeyEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>

#include <iostream>
#include <vector>

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
