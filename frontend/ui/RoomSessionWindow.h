#pragma once
#include "ui/QtRoomSession.h"
#include <QWidget>
class QLabel;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QPushButton;
class VideoFrameWidget;

class RoomSessionWindow final : public QWidget {
public:
    explicit RoomSessionWindow(RoomSessionConfig, QtRoomSession::Factory = screenshare::media::WindowsRoomRuntimeFactory,
                               bool diagnosticLoopback = false);
    ~RoomSessionWindow() override;
    QtRoomSession& session() { return session_; }
protected:
    void closeEvent(QCloseEvent*) override;
private:
    QtRoomSession session_;
    QLabel *phase_, *room_, *settingsState_, *error_;
    QSpinBox *width_, *height_, *fps_, *bitrate_;
    QComboBox *resolution_, *fpsMode_, *bitrateMode_, *preset_;
    QCheckBox* bitrateLimit_;
    QPushButton *apply_, *stop_;
    VideoFrameWidget* video_;
    bool closing_ = false;
};
int RunRoomSessionWindow(const QString& configurationPath);
