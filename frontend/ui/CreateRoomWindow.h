#pragma once

#include "core/ScreenShareSession.h"
#include "ui/QtSessionBackend.h"
#include "ui/ShareSessionUiState.h"

#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <functional>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;

class CreateRoomWindow final : public QWidget {
public:
    struct Actions {
        std::function<void()> back;
        std::function<void(const ShareSessionUiState&)> shareStarted;
    };

    explicit CreateRoomWindow(QtSessionBackend* backend, Actions actions, QWidget* parent = nullptr);

    void resetForNextRoom();

private:
    QWidget* buildShell();
    QWidget* buildHeader();
    QWidget* buildLabeledField(const QString& labelText, QWidget* field);
    QWidget* buildPasswordField();
    QWidget* buildLinkField();
    QWidget* buildSettingsPanel();
    QWidget* buildSettingRow(const char* iconName, const QString& labelText, QWidget* field, bool isLast = false);
    QWidget* buildSwitchField(QCheckBox* toggle, QWidget* field);
    QPushButton* iconButton(const QString& text, const QString& objectName, const char* iconName);
    QLabel* textLabel(const QString& text, const char* objectName);
    QLabel* iconLabel(const char* iconName, int size, const QString& color = QStringLiteral("#eaf5f2"));

    void refreshDisplays(bool preserveSelection = true, bool skipOpenPopup = false);
    void refreshAudioDevices();
    void populateCaptureSources(
        const std::vector<screenshare::SessionDisplayInfo>& displays,
        const std::vector<screenshare::SessionWindowInfo>& windows,
        const QString& preferredSourceValue = QString());
    void populateAudioDevices(const std::vector<screenshare::SessionAudioDeviceInfo>& devices);
    void populateResolutionChoices();
    void updateDefaultAudioChoiceText();
    void refreshRoomLink();
    void generatePassword();
    void installBackendHandlers();
    void startOrStop();
    void startShare();
    void stopShare();
    screenshare::ShareSessionConfig currentConfig() const;
    screenshare::StreamSettings currentStreamSettings() const;
    ShareSessionUiState currentShareUiState() const;
    bool validateFields();
    QString roomName() const;
    QString roomPassword() const;
    QString reportPath() const;
    QString roomLink() const;
    QSize selectedDisplaySize() const;
    QSize selectedResolution() const;
    void copyRoomLink();
    void updateRunningState(bool running);
    void setStatus(const QString& text, const QString& objectName);
    void handleStatusEvent(const screenshare::SessionEvent& event);
    void handleBack();

    Actions actions_;
    QtSessionBackend* backend_ = nullptr;
    QString roomId_;
    QLineEdit* roomNameEdit_ = nullptr;
    QLineEdit* roomPasswordEdit_ = nullptr;
    QLineEdit* roomLinkEdit_ = nullptr;
    QSpinBox* roomPortSpin_ = nullptr;
    QComboBox* displayCombo_ = nullptr;
    QComboBox* resolutionCombo_ = nullptr;
    QComboBox* fpsCombo_ = nullptr;
    QComboBox* audioDeviceCombo_ = nullptr;
    QCheckBox* captureAudioCheck_ = nullptr;
    QCheckBox* reportCheck_ = nullptr;
    QCheckBox* lowLatencyCheck_ = nullptr;
    QLineEdit* reportPathEdit_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QTimer* sourceRefreshTimer_ = nullptr;
};
