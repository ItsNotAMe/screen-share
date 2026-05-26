#pragma once

#include "core/ScreenShareSession.h"
#include "ui/QtSessionBackend.h"
#include "ui/WatchSessionUiState.h"

#include <QtCore/QElapsedTimer>
#include <QtWidgets/QWidget>

#include <functional>
#include <optional>

class QLabel;
class QPushButton;
class QSlider;
class QTimer;
class VideoFrameWidget;

class ActiveWatchWindow final : public QWidget {
public:
    struct Actions {
        std::function<void()> sessionStopped;
    };

    explicit ActiveWatchWindow(QtSessionBackend* backend, Actions actions, QWidget* parent = nullptr);

    void setSession(const WatchSessionUiState& session);

private:
    bool eventFilter(QObject* watched, QEvent* event) override;

    QWidget* buildShell();
    QWidget* buildTopStatus();
    QWidget* buildPreviewPanel();
    QWidget* buildSideStats();
    QWidget* buildFooter();
    QWidget* buildStatRow(const QString& label, QLabel*& valueLabel);
    QPushButton* iconButton(const QString& text, const QString& objectName, const char* iconName);
    QLabel* textLabel(const QString& text, const char* objectName);

    void installBackendHandlers();
    void startWatch();
    void updateElapsed();
    void updateStatus(const screenshare::SessionStatus& status);
    void updateAudioControls(const screenshare::SessionAudioStatus& audio);
    void applyVolume(int volumePercent);
    void toggleMute();
    void toggleFullscreen();
    void closeStreamFullscreen();
    void setPreviewStatusText(const QString& text);
    void leaveRoom();
    void handleFinished(const QtSessionBackend::FinishInfo& info);
    QString stateText(const screenshare::SessionStatus& status) const;
    QString connectionText(const screenshare::SessionStatus& status) const;
    QString resolutionText(const screenshare::SessionStatus& status) const;

    QtSessionBackend* backend_ = nullptr;
    Actions actions_;
    WatchSessionUiState session_;
    QElapsedTimer elapsed_;
    QTimer* elapsedTimer_ = nullptr;

    QLabel* stateLabel_ = nullptr;
    QLabel* elapsedLabel_ = nullptr;
    QLabel* roomLabel_ = nullptr;
    VideoFrameWidget* videoFrameWidget_ = nullptr;
    QLabel* hostLabel_ = nullptr;
    QLabel* connectionLabel_ = nullptr;
    QLabel* avSyncLabel_ = nullptr;
    QLabel* qualityLabel_ = nullptr;
    QLabel* resolutionLabel_ = nullptr;
    QLabel* fpsLabel_ = nullptr;
    QLabel* bitrateLabel_ = nullptr;
    QPushButton* leaveButton_ = nullptr;
    QPushButton* muteButton_ = nullptr;
    QPushButton* fullscreenButton_ = nullptr;
    QSlider* volumeSlider_ = nullptr;
    QLabel* volumePercentLabel_ = nullptr;
    bool audioMuted_ = false;
    int audioVolumePercent_ = 100;
    bool audioControlsInitialized_ = false;
    bool audioControlsTouched_ = false;
    QWidget* fullscreenVideoWindow_ = nullptr;
    VideoFrameWidget* fullscreenVideoWidget_ = nullptr;
    QString previewStatusText_;
    std::optional<screenshare::SessionEvent::VideoFrame> latestVideoFrame_;
};
