#include "input/XInputGamepad.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWidget>
#include <array>

// A target for user-operated acceptance checks. It never injects input, creates
// virtual devices, records raw input, opens audio, or contacts the network.
class FieldScene final : public QWidget {
public:
    explicit FieldScene(bool selfTest) : selfTest_(selfTest) {
        setWindowTitle("ScreenShare test scene — silent, read-only input");
        resize(960, 720); setMinimumSize(800, 640);
        setFocusPolicy(Qt::StrongFocus); setMouseTracking(true);
        elapsed_.start();
        connect(&timer_, &QTimer::timeout, this, [this] {
            if (!selfTest_) for (int slot = 0; slot < 4; ++slot)
                pads_[slot] = screenshare::XInputGamepad::ReadState(slot);
            ++frames_; update();
        });
        if (!selfTest_) timer_.start(16);
    }
    bool active() const { return space_ || mouse_; }
protected:
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) { space_ = true; update(); }
        else QWidget::keyPressEvent(e);
    }
    void keyReleaseEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) { space_ = false; update(); }
        else QWidget::keyReleaseEvent(e);
    }
    void mousePressEvent(QMouseEvent* e) override { mouse_ |= e->button(); position_ = e->position(); update(); }
    void mouseReleaseEvent(QMouseEvent* e) override { mouse_ &= ~e->button(); position_ = e->position(); update(); }
    void mouseMoveEvent(QMouseEvent* e) override { position_ = e->position(); update(); }
    void focusOutEvent(QFocusEvent* e) override { space_ = false; mouse_ = {}; update(); QWidget::focusOutEvent(e); }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.fillRect(rect(), QColor(18, 22, 30));
        p.setPen(Qt::white); p.setFont(QFont("Segoe UI", 15));
        p.drawText(24, 36, "ScreenShare physical test scene");
        p.setFont(QFont("Consolas", 12));
        p.drawText(24, 67, QString("Host clock: %1 ms   Paint tick: %2").arg(elapsed_.elapsed()).arg(frames_));
        p.drawText(24, 96, "Hold Space / a mouse button: marker turns green. Release: gray.");
        p.drawText(24, 120, "Keyboard requires a display share. Keep this host scene focused.");
        const QRect marker(24, 144, width() - 48, 115);
        p.fillRect(marker, active() ? QColor(0, 220, 120) : QColor(110, 115, 125));
        p.setPen(Qt::black);
        p.drawText(marker, Qt::AlignCenter, QString("SPACE %1   MOUSE %2").arg(space_ ? "DOWN" : "UP").arg(mouse_ ? "DOWN" : "UP"));
        p.fillRect(24, 277, width() - 48, 18, QColor(50, 55, 65));
        p.fillRect(24 + int(frames_ * 7 % (width() - 80)), 277, 32, 18, Qt::white);
        p.setPen(Qt::white);
        p.drawText(24, 325, QString("Pointer: %1, %2").arg(int(position_.x())).arg(int(position_.y())));
        for (int slot = 0; slot < 4; ++slot) {
            const auto& pad = pads_[slot]; const int y = 360 + slot * 44;
            const bool pressed = pad && (pad->buttons || pad->leftTrigger > 30 || pad->rightTrigger > 30);
            p.fillRect(24, y, width() - 48, 36, pressed ? QColor(0, 115, 70) : QColor(40, 45, 55));
            const QString detail = pad ? QString("buttons %1   LT/RT %2/%3   L %4,%5   R %6,%7")
                .arg(pad->buttons, 4, 16, QChar('0')).arg(pad->leftTrigger).arg(pad->rightTrigger)
                .arg(pad->thumbLX).arg(pad->thumbLY).arg(pad->thumbRX).arg(pad->thumbRY) : "not connected";
            p.drawText(34, y + 24, QString("XInput %1: %2").arg(slot).arg(detail));
        }
        p.drawText(24, 565, "Pad values are read only. No sound, recording, driver changes or input injection.");
        p.drawText(24, 590, "The clock is not an automatic latency result; film both displays to measure it.");
    }
private:
    bool selfTest_, space_ = false;
    Qt::MouseButtons mouse_{};
    QPointF position_;
    uint64_t frames_ = 0;
    QElapsedTimer elapsed_;
    QTimer timer_;
    std::array<std::optional<screenshare::RemoteGamepadState>, 4> pads_;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const bool selfTest = app.arguments().contains("--self-test");
    FieldScene scene(selfTest);
    if (selfTest) {
        QKeyEvent down(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        QKeyEvent up(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
        QApplication::sendEvent(&scene, &down);
        if (!scene.active()) return 1;
        QImage image(scene.size(), QImage::Format_RGB32); scene.render(&image);
        if (image.pixelColor(25, 145) != QColor(0, 220, 120)) return 2;
        QApplication::sendEvent(&scene, &up);
        if (scene.active()) return 3;
        scene.render(&image);
        if (image.pixelColor(25, 145) != QColor(110, 115, 125)) return 4;
        QApplication::sendEvent(&scene, &down);
        QFocusEvent lost(QEvent::FocusOut); QApplication::sendEvent(&scene, &lost);
        return scene.active() ? 5 : 0;
    }
    scene.show();
    return app.exec();
}
