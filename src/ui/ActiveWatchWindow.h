#pragma once

#include "core/ScreenShareSession.h"
#include "ui/QtSessionBackend.h"
#include "ui/WatchSessionUiState.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QRect>
#include <QtCore/Qt>
#include <QtWidgets/QWidget>

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>

class QLabel;
class QPushButton;
class QSlider;
class QComboBox;
class QTimer;
class QHBoxLayout;
class QVBoxLayout;
class VideoFrameWidget;

class ActiveWatchWindow final : public QWidget {
public:
    struct Actions {
        std::function<void()> sessionStopped;
        std::function<void()> sessionEndedByHost;
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
    void updateReceiveBitrate(uint64_t payloadBytes);
    void applyVolume(int volumePercent);
    void toggleMute();
    void refreshMuteButton();
    void toggleFullscreen();
    void closeStreamFullscreen();
    void setStreamFullscreen(bool enabled);
    void applyStreamFullscreenUi(bool enabled);
    void finishStreamFullscreenExit(QWidget* topLevel);
    void updatePresentedFps();
    void refreshFpsLabel(double streamFps);
    void installFullscreenEventFilter(bool installed);
    void setPreviewStatusText(const QString& text);
    void handleHostLeft();
    void exitToHomeAfterHostLeft();
    void handleControlState(uint32_t capabilities);
    void pollGamepad();
    void refreshGamepadSlots();
    void sendNeutralGamepad();
    void setControlRequestPending(bool pending);
    void toggleControlRequest();
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
    QVBoxLayout* rootLayout_ = nullptr;
    QHBoxLayout* bodyLayout_ = nullptr;
    QWidget* topStatusWidget_ = nullptr;
    QWidget* previewPanel_ = nullptr;
    QWidget* sideStatsPanel_ = nullptr;
    QWidget* footerWidget_ = nullptr;
    VideoFrameWidget* videoFrameWidget_ = nullptr;
    QLabel* hostLabel_ = nullptr;
    QLabel* connectionLabel_ = nullptr;
    QLabel* avSyncLabel_ = nullptr;
    QLabel* qualityLabel_ = nullptr;
    QLabel* resolutionLabel_ = nullptr;
    QLabel* fpsLabel_ = nullptr;
    QLabel* bitrateLabel_ = nullptr;
    QLabel* latencyLabel_ = nullptr;
    QPushButton* leaveButton_ = nullptr;
    QPushButton* controlButton_ = nullptr;
    QWidget* controlStatusRow_ = nullptr;
    QLabel* controlMouseIcon_ = nullptr;
    QLabel* controlKeyboardIcon_ = nullptr;
    QLabel* controlGamepadIcon_ = nullptr;
    QComboBox* gamepadSelector_ = nullptr;
    QTimer* gamepadTimer_ = nullptr;
    QTimer* controlRequestTimer_ = nullptr;
    QElapsedTimer gamepadClock_;
    std::optional<screenshare::RemoteGamepadState> lastGamepadState_;
    qint64 lastGamepadSentMs_ = -1;
    int gamepadScanTicks_ = 0;
    uint32_t controlCapabilities_ = 0;
    bool controlRequestPending_ = false;
    bool controlRequestCancellationPending_ = false;
    uint64_t lastBitrateBytes_ = 0;
    qint64 lastBitrateSampleMs_ = -1;
    double receiveBitrateMbps_ = 0.0;
    QPushButton* muteButton_ = nullptr;
    QPushButton* fullscreenButton_ = nullptr;
    QSlider* volumeSlider_ = nullptr;
    QLabel* volumePercentLabel_ = nullptr;
    bool audioMuted_ = false;
    int audioVolumePercent_ = 100;
    bool audioControlsInitialized_ = false;
    bool audioControlsTouched_ = false;
    bool receivedVideoFrame_ = false;
    bool hostLeft_ = false;
    bool hostLeftHandled_ = false;
    bool leaveRequested_ = false;
    std::atomic_bool firstVideoFramePosted_{false};
    bool streamFullscreen_ = false;
    bool fullscreenEventFilterInstalled_ = false;
    std::uint64_t lastPresentedFrameCount_ = 0;
    double presentedFps_ = 0.0;
    double latestStreamFps_ = 0.0;
    QElapsedTimer presentedFpsTimer_;
    QRect preFullscreenGeometry_;
    Qt::WindowStates preFullscreenState_{};
    QString previewStatusText_;
};
