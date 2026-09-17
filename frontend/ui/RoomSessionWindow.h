#pragma once
#include "ui/QtRoomSession.h"
#include <QWidget>
class QLabel;
class QLineEdit;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QPushButton;
class VideoFrameWidget;
class RoomProfile;

class RoomSessionWindow final : public QWidget {
public:
    // Optional profile is owned by the browser and must outlive this window.
    // Config-file entry points pass none and remain independent of local defaults.
    explicit RoomSessionWindow(RoomSessionConfig, QtRoomSession::Factory = screenshare::media::WindowsRoomRuntimeFactory,
                               bool diagnosticLoopback = false, RoomProfile* profile = nullptr);
    ~RoomSessionWindow() override;
    QtRoomSession& session() { return session_; }
    std::function<void()> closed;
protected:
    void closeEvent(QCloseEvent*) override;
private:
    screenshare::media::StreamPreferences ReadPreferences() const;
    QtRoomSession session_;
    QLabel *phase_, *room_, *settingsState_, *error_;
    QSpinBox *width_, *height_, *fps_, *bitrate_;
    QSpinBox* uploadBudget_;
    QCheckBox* uploadBudgetEnabled_;
    QLabel* uploadState_;
    QComboBox *resolution_, *fpsMode_, *bitrateMode_, *preset_;
    QComboBox* captureSource_;
    QPushButton *switchCapture_, *refreshCapture_;
    QLabel* captureState_;
    QComboBox *audioKind_, *audioDevice_;
    QSpinBox* audioProcess_;
    QPushButton *switchAudio_, *refreshAudio_;
    QLabel* audioState_;
    QLabel* audioHealth_;
    QComboBox* playbackDevice_;
    QSpinBox* playbackVolume_;
    QCheckBox* playbackMuted_;
    QPushButton *applyPlayback_, *refreshPlayback_;
    QLabel* playbackState_;
    QLabel* playbackHealth_;
    QCheckBox* bitrateLimit_;
    QPushButton *apply_, *stop_;
    VideoFrameWidget* video_;
    QLineEdit *nickname_, *name_;
    QLineEdit* roomLink_;
    QPushButton* copyLink_;
    QCheckBox* publicRoom_;
    QSpinBox* viewerLimit_;
    QLabel *members_, *roomUpdateState_;
    QPushButton *updateNickname_, *updatePolicy_;
    uint64_t editRevision_ = 0, nicknameRevision_ = 0;
    bool editingRoom_ = false, editingNickname_ = false, updatingNickname_ = false;
    bool closing_ = false;
};
int RunRoomSessionWindow(const QString& configurationPath);
