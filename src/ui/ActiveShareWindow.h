#pragma once

#include "core/ScreenShareSession.h"
#include "ui/QtSessionBackend.h"
#include "ui/ShareSessionUiState.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <functional>

class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
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

    void installBackendHandlers();
    void updateElapsed();
    void updateStatus(const screenshare::SessionStatus& status);
    void updateViewerList(const screenshare::SessionStatus& status);
    void updateHealth(const screenshare::SessionStatus& status);
    void updateShareSummary();
    void stopSharing();
    void copyInvite();
    void applySettings();
    screenshare::StreamSettings selectedStreamSettings() const;
    void showSettingsPanel(bool visible);
    void handleFinished(const QtSessionBackend::FinishInfo& info);
    QString stateText(const screenshare::SessionStatus& status) const;
    QString healthText(const screenshare::SessionStatus& status) const;

    QtSessionBackend* backend_ = nullptr;
    Actions actions_;
    ShareSessionUiState session_;
    QElapsedTimer elapsed_;
    QTimer* elapsedTimer_ = nullptr;

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
    QPushButton* stopButton_ = nullptr;
    QPushButton* settingsApplyButton_ = nullptr;
    QFrame* settingsOverlay_ = nullptr;
    QFrame* settingsPanel_ = nullptr;
    QLineEdit* settingsRoomNameEdit_ = nullptr;
    QLineEdit* settingsRoomLinkEdit_ = nullptr;
    QComboBox* settingsResolutionCombo_ = nullptr;
    QComboBox* qualityCombo_ = nullptr;
    QCheckBox* adaptiveBitrateCheck_ = nullptr;
    QCheckBox* adaptiveResolutionCheck_ = nullptr;
    QCheckBox* adaptiveFpsCheck_ = nullptr;
    QString lastViewerSignature_;
    bool updatingSettingsUi_ = false;
    QString appliedResolutionValue_;
};
