#pragma once

#include "core/ScreenShareSession.h"
#include "ui/QtSessionBackend.h"
#include "ui/ShareSessionUiState.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <cstdint>
#include <functional>

class QCheckBox;
class QComboBox;
class QEvent;
class QFrame;
class QLabel;
class QLineEdit;
class QObject;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;
class QWidget;

class ActiveShareWindow final : public QWidget {
public:
    struct Actions {
        std::function<void()> sessionStopped;
    };

    explicit ActiveShareWindow(QtSessionBackend* backend, Actions actions, QWidget* parent = nullptr);

    void setSession(const ShareSessionUiState& session);

private:
    bool eventFilter(QObject* watched, QEvent* event) override;

    QWidget* buildShell();
    QWidget* buildTopStatus();
    QWidget* buildShareCard();
    QWidget* buildViewersCard();
    QWidget* buildHealthCard();
    QWidget* buildMetricTile(const QString& label, QLabel*& valueLabel);
    QWidget* buildMetricCell(const QString& label, QLabel*& valueLabel);
    QWidget* buildFooter();
    QWidget* buildSettingsOverlay();
    QWidget* buildSettingsPanel();
    QWidget* buildMetricRow(const QString& label, QLabel*& valueLabel);
    QWidget* buildViewerRow(const screenshare::SessionViewer& viewer, int index);
    QPushButton* iconButton(const QString& text, const QString& objectName, const char* iconName);
    QLabel* textLabel(const QString& text, const char* objectName);
    QLabel* iconLabel(const char* iconName, int size, const QString& color = QStringLiteral("#eaf5f2"));
    void configureSettingsChoice(QComboBox* combo);

    void installBackendHandlers();
    void updateElapsed();
    void updateStatus(const screenshare::SessionStatus& status);
    void updateViewerList(const screenshare::SessionStatus& status);
    void updateHealth(const screenshare::SessionStatus& status);
    void updateShareSummary();
    void stopSharing();
    void toggleHostAudio();
    void toggleVideoPause();
    void updateHostControlButtons();
    void copyInvite();
    void refreshSourceChoices(bool preserveSelection = true, bool skipOpenPopup = false);
    void populateSettingsSourceChoices(const QString& preferredSourceValue = QString(), bool keepMissingSelection = false);
    void updateSettingsDefaultAudioChoiceText();
    void applySettings();
    screenshare::ShareSessionSettings selectedShareSettings() const;
    void updateSettingsApplyState();
    void showSettingsPanel(bool visible);
    void handleFinished(const QtSessionBackend::FinishInfo& info);
    QString stateText(const screenshare::SessionStatus& status) const;
    QString healthText(const screenshare::SessionStatus& status) const;

    QtSessionBackend* backend_ = nullptr;
    Actions actions_;
    ShareSessionUiState session_;
    QElapsedTimer elapsed_;
    QTimer* elapsedTimer_ = nullptr;
    QTimer* sourceRefreshTimer_ = nullptr;

    QLabel* stateLabel_ = nullptr;
    QLabel* elapsedLabel_ = nullptr;
    QLabel* roomLabel_ = nullptr;
    QLabel* shareTitleLabel_ = nullptr;
    QLabel* shareDisplayLabel_ = nullptr;
    QLabel* shareAudioLabel_ = nullptr;
    QLabel* viewerTitleLabel_ = nullptr;
    QVBoxLayout* viewerListLayout_ = nullptr;
    QLabel* healthStateLabel_ = nullptr;
    QLabel* bitrateLabel_ = nullptr;
    QLabel* resolutionLabel_ = nullptr;
    QLabel* fpsLabel_ = nullptr;
    QLabel* packetLossLabel_ = nullptr;
    QLabel* latencyLabel_ = nullptr;
    QLabel* viewerMetricLabel_ = nullptr;
    QLabel* adaptiveLabel_ = nullptr;
    QPushButton* muteAudioButton_ = nullptr;
    QPushButton* pauseVideoButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* settingsApplyButton_ = nullptr;
    QFrame* settingsOverlay_ = nullptr;
    QFrame* settingsPanel_ = nullptr;
    QScrollArea* settingsScrollArea_ = nullptr;
    QLineEdit* settingsRoomNameEdit_ = nullptr;
    QLineEdit* settingsRoomLinkEdit_ = nullptr;
    QComboBox* settingsDisplayCombo_ = nullptr;
    QComboBox* settingsResolutionCombo_ = nullptr;
    QComboBox* settingsFpsCombo_ = nullptr;
    QComboBox* bitrateCombo_ = nullptr;
    QComboBox* settingsAudioCombo_ = nullptr;
    QCheckBox* settingsLowLatencyCheck_ = nullptr;
    QString lastViewerSignature_;
    bool updatingSettingsUi_ = false;
    bool hostAudioMuted_ = false;
    bool videoPaused_ = false;
    QString appliedRoomName_;
    QString appliedDisplaySourceValue_;
    QString appliedResolutionValue_;
    int appliedFpsValue_ = 60;
    uint32_t appliedBitrateBps_ = 0;
    bool appliedLowLatency_ = false;
    QString appliedAudioDeviceValue_;
};
