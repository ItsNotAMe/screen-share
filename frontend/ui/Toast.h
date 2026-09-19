#pragma once

#include <QtWidgets/QWidget>

class QLabel;

// Lightweight, non-blocking notification that fades in near the bottom of a host
// widget, stays for a short time, then fades out and deletes itself. Unlike a
// modal QMessageBox it never steals focus or blocks the event loop, and it is
// transparent to mouse events so it does not interrupt what the user is doing.
class Toast final : public QWidget {
public:
    // Shows a transient notice centered near the bottom of `host`. The Toast owns
    // its own lifetime (deletes itself once it has faded out).
    static void show(QWidget* host, const QString& message, int visibleMs = 3200);

private:
    Toast(QWidget* host, const QString& message, int visibleMs);
    void start();
    void reposition();
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent*) override;

    QWidget* host_ = nullptr;
    QLabel* label_ = nullptr;
    int visibleMs_ = 3200;
};
