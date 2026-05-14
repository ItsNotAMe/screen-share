#include "transport/UdpCrypto.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QIODevice>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QSize>
#include <QtCore/QStringList>
#include <QtGui/QClipboard>
#include <QtGui/QTextCursor>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

namespace {

QString enginePath()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString besideUi = appDir.filePath("ScreenShare.exe");
    if (QFileInfo::exists(besideUi)) {
        return besideUi;
    }

    return QDir::current().filePath("ScreenShare.exe");
}

QString quoted(QString value)
{
    if (value.isEmpty()) {
        return "\"\"";
    }
    if (!value.contains(' ') && !value.contains('\t') && !value.contains('"')) {
        return value;
    }
    value.replace("\"", "\\\"");
    return "\"" + value + "\"";
}

QString formatCommand(const QString& program, const QStringList& arguments)
{
    QStringList parts;
    parts << quoted(QFileInfo(program).fileName());
    for (const QString& argument : arguments) {
        parts << quoted(argument);
    }
    return parts.join(' ');
}

QLabel* makeLabel(const QString& text, const QString& className = {})
{
    auto* label = new QLabel(text);
    label->setProperty("class", className);
    return label;
}

void repolish(QWidget* widget)
{
    if (widget == nullptr) {
        return;
    }
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

constexpr int kRowHeight = 34;
constexpr int kLabelWidth = 96;

void prepareInput(QWidget* input)
{
    input->setFixedHeight(kRowHeight);
    input->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QFrame* makePanel(const QString& title, QVBoxLayout** outContent)
{
    auto* frame = new QFrame;
    frame->setObjectName("Panel");
    frame->setFrameShape(QFrame::NoFrame);
    frame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);

    auto* outer = new QVBoxLayout(frame);
    outer->setContentsMargins(18, 14, 18, 16);
    outer->setSpacing(10);

    if (!title.isEmpty()) {
        outer->addWidget(makeLabel(title, "PanelTitle"));
    }

    auto* content = new QVBoxLayout;
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(10);
    outer->addLayout(content);

    if (outContent != nullptr) {
        *outContent = content;
    }
    return frame;
}

void addRow(QVBoxLayout* content, const QString& labelText, QWidget* field)
{
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(14);

    if (!labelText.isEmpty()) {
        auto* label = makeLabel(labelText, "FieldLabel");
        label->setFixedWidth(kLabelWidth);
        row->addWidget(label);
    } else {
        row->addSpacing(kLabelWidth);
    }
    row->addWidget(field, 1);
    content->addLayout(row);
}

void addFullRow(QVBoxLayout* content, QWidget* widget)
{
    widget->setFixedHeight(kRowHeight);
    content->addWidget(widget);
}

QString appStyleSheet(bool darkMode);

class MainWindow final : public QWidget {
public:
    MainWindow()
    {
        setWindowTitle("ScreenShare");
        resize(1080, 820);
        setMinimumSize(960, 780);

        process_ = new QProcess(this);
        process_->setProcessChannelMode(QProcess::MergedChannels);
        connect(process_, &QProcess::readyReadStandardOutput, this, [this] {
            appendOutput(QString::fromLocal8Bit(process_->readAllStandardOutput()));
        });
        connect(process_, &QProcess::started, this, [this] {
            appendOutput("Started " + QFileInfo(enginePath()).fileName() + "\n");
            setRunning(true);
        });
        connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            Q_UNUSED(error);
            appendOutput("Process error: " + process_->errorString() + "\n");
            setRunning(false);
        });
        connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus status) {
            if (stopRequested_) {
                appendOutput(forcedStop_ ?
                    "Process was forced closed after stop timed out\n" :
                    "Process stopped cleanly with exit code " + QString::number(code) + "\n");
            } else {
                const QString statusText = status == QProcess::NormalExit ? "finished" : "crashed";
                appendOutput("Process " + statusText + " with exit code " + QString::number(code) + "\n");
            }
            cleanupStopFile();
            stopRequested_ = false;
            forcedStop_ = false;
            setRunning(false);
        });

        discoveryProcess_ = new QProcess(this);
        discoveryProcess_->setProcessChannelMode(QProcess::MergedChannels);
        connect(discoveryProcess_, &QProcess::readyReadStandardOutput, this, [this] {
            const QString text = QString::fromLocal8Bit(discoveryProcess_->readAllStandardOutput());
            discoveryOutput_ += text;
            appendOutput(text);
        });
        connect(discoveryProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            Q_UNUSED(error);
            appendOutput("LAN discovery error: " + discoveryProcess_->errorString() + "\n");
            setDiscovering(false);
        });
        connect(discoveryProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus status) {
            finishDiscovery(code, status);
        });

        buildUi();
        refreshReportPath();
        refreshCommand();
        setRunning(false);
    }

private:
    void buildUi()
    {
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(22, 20, 22, 20);
        root->setSpacing(16);

        root->addLayout(buildHeader());
        root->addWidget(buildModeSelector());

        auto* body = new QHBoxLayout;
        body->setSpacing(16);
        root->addLayout(body, 1);

        auto* leftColumn = new QVBoxLayout;
        leftColumn->setSpacing(12);
        leftColumn->addWidget(buildOptionStack());
        leftColumn->addWidget(buildSessionPanel());
        leftColumn->addStretch(1);

        auto* leftHost = new QWidget;
        leftHost->setObjectName("LeftHost");
        leftHost->setLayout(leftColumn);
        leftHost->setFixedWidth(430);
        body->addWidget(leftHost);

        auto* rightColumn = new QVBoxLayout;
        rightColumn->setSpacing(12);
        rightColumn->addWidget(buildCommandPanel());
        rightColumn->addWidget(buildOutputPanel(), 1);
        body->addLayout(rightColumn, 1);

        root->addLayout(buildFooter());

        bindCommandRefresh();
        applyTheme(darkModeCheck_->isChecked());
    }

    QHBoxLayout* buildHeader()
    {
        auto* header = new QHBoxLayout;
        header->setSpacing(12);

        auto* titleBlock = new QVBoxLayout;
        titleBlock->setSpacing(2);
        titleBlock->addWidget(makeLabel("ScreenShare", "HeroTitle"));
        titleBlock->addWidget(makeLabel("Fast local screen sharing", "Subtle"));
        header->addLayout(titleBlock, 1);

        darkModeCheck_ = new QCheckBox("Dark");
        darkModeCheck_->setObjectName("ThemeSwitch");
        darkModeCheck_->setChecked(true);
        connect(darkModeCheck_, &QCheckBox::toggled, this, [this](bool checked) { applyTheme(checked); });
        header->addWidget(darkModeCheck_, 0, Qt::AlignVCenter);

        return header;
    }

    QWidget* buildModeSelector()
    {
        auto* bar = new QFrame;
        bar->setObjectName("ModeBar");
        bar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

        auto* layout = new QHBoxLayout(bar);
        layout->setContentsMargins(6, 6, 6, 6);
        layout->setSpacing(6);

        shareModeButton_ = new QPushButton("Share");
        watchModeButton_ = new QPushButton("Watch");
        shareModeButton_->setCheckable(true);
        watchModeButton_->setCheckable(true);
        shareModeButton_->setObjectName("ModeButton");
        watchModeButton_->setObjectName("ModeButton");
        shareModeButton_->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
        watchModeButton_->setIcon(style()->standardIcon(QStyle::SP_DesktopIcon));
        shareModeButton_->setIconSize(QSize(16, 16));
        watchModeButton_->setIconSize(QSize(16, 16));
        shareModeButton_->setCursor(Qt::PointingHandCursor);
        watchModeButton_->setCursor(Qt::PointingHandCursor);
        shareModeButton_->setChecked(true);

        layout->addWidget(shareModeButton_);
        layout->addWidget(watchModeButton_);
        layout->addStretch(1);

        connect(shareModeButton_, &QPushButton::clicked, this, [this] { setMode(0); });
        connect(watchModeButton_, &QPushButton::clicked, this, [this] { setMode(1); });
        return bar;
    }

    QWidget* buildOptionStack()
    {
        optionStack_ = new QStackedWidget;
        optionStack_->setObjectName("OptionStack");
        optionStack_->addWidget(buildSharePage());
        optionStack_->addWidget(buildWatchPage());
        return optionStack_;
    }

    QWidget* buildSharePage()
    {
        auto* page = new QWidget;
        page->setObjectName("OptionPage");
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);

        QVBoxLayout* connectionContent = nullptr;
        layout->addWidget(makePanel("Connection", &connectionContent));
        shareHostEdit_ = new QLineEdit("127.0.0.1");
        sharePortSpin_ = new QSpinBox;
        sharePortSpin_->setRange(1, 65535);
        sharePortSpin_->setValue(5000);
        prepareInput(shareHostEdit_);
        prepareInput(sharePortSpin_);
        addRow(connectionContent, "Address", shareHostEdit_);
        addRow(connectionContent, "Port", sharePortSpin_);
        findLanButton_ = new QPushButton("Find on LAN");
        findLanButton_->setObjectName("SecondaryButton");
        findLanButton_->setIcon(style()->standardIcon(QStyle::SP_DriveNetIcon));
        findLanButton_->setIconSize(QSize(14, 14));
        findLanButton_->setCursor(Qt::PointingHandCursor);
        addFullRow(connectionContent, findLanButton_);
        connect(findLanButton_, &QPushButton::clicked, this, [this] { startDiscovery(); });

        QVBoxLayout* videoContent = nullptr;
        layout->addWidget(makePanel("Video", &videoContent));
        displaySpin_ = new QSpinBox;
        displaySpin_->setRange(0, 16);
        displaySpin_->setValue(0);
        fpsSpin_ = new QSpinBox;
        fpsSpin_->setRange(15, 240);
        fpsSpin_->setValue(60);
        resolutionCombo_ = new QComboBox;
        resolutionCombo_->addItem("Native", QSize(0, 0));
        resolutionCombo_->addItem("1920 × 1080", QSize(1920, 1080));
        resolutionCombo_->addItem("1600 × 900", QSize(1600, 900));
        resolutionCombo_->addItem("1280 × 720", QSize(1280, 720));
        prepareInput(displaySpin_);
        prepareInput(fpsSpin_);
        prepareInput(resolutionCombo_);
        addRow(videoContent, "Display", displaySpin_);
        addRow(videoContent, "FPS", fpsSpin_);
        addRow(videoContent, "Resolution", resolutionCombo_);

        return page;
    }

    QWidget* buildWatchPage()
    {
        auto* page = new QWidget;
        page->setObjectName("OptionPage");
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);

        QVBoxLayout* listenContent = nullptr;
        layout->addWidget(makePanel("Listen", &listenContent));
        watchPortSpin_ = new QSpinBox;
        watchPortSpin_->setRange(1, 65535);
        watchPortSpin_->setValue(5000);
        prepareInput(watchPortSpin_);
        addRow(listenContent, "Port", watchPortSpin_);
        lanDiscoverableCheck_ = new QCheckBox("LAN discoverable");
        lanDiscoverableCheck_->setChecked(true);
        addFullRow(listenContent, lanDiscoverableCheck_);

        QVBoxLayout* audioContent = nullptr;
        layout->addWidget(makePanel("Audio", &audioContent));
        mutedCheck_ = new QCheckBox("Mute playback");
        volumeSpin_ = new QSpinBox;
        volumeSpin_->setRange(0, 200);
        volumeSpin_->setSuffix("%");
        volumeSpin_->setValue(100);
        prepareInput(volumeSpin_);
        addFullRow(audioContent, mutedCheck_);
        addRow(audioContent, "Volume", volumeSpin_);

        QVBoxLayout* timingContent = nullptr;
        layout->addWidget(makePanel("Timing", &timingContent));
        previewLatencySpin_ = new QSpinBox;
        previewLatencySpin_->setRange(0, 2000);
        previewLatencySpin_->setSuffix(" ms");
        previewLatencySpin_->setValue(100);
        prepareInput(previewLatencySpin_);
        addRow(timingContent, "Preview latency", previewLatencySpin_);

        return page;
    }

    QWidget* buildSessionPanel()
    {
        QVBoxLayout* content = nullptr;
        auto* panel = makePanel("Session", &content);

        sessionEdit_ = new QLineEdit;
        sessionEdit_->setPlaceholderText("Auto");
        prepareInput(sessionEdit_);
        addRow(content, "ID", sessionEdit_);

        auto* accessRow = new QWidget;
        accessRow->setObjectName("FormRow");
        prepareInput(accessRow);
        auto* accessLayout = new QHBoxLayout(accessRow);
        accessLayout->setContentsMargins(0, 0, 0, 0);
        accessLayout->setSpacing(8);
        accessCodeEdit_ = new QLineEdit;
        accessCodeEdit_->setPlaceholderText("Generate or paste");
        accessCodeEdit_->setEchoMode(QLineEdit::Password);
        prepareInput(accessCodeEdit_);
        generateAccessCodeButton_ = new QPushButton("Generate");
        generateAccessCodeButton_->setObjectName("SecondaryButton");
        generateAccessCodeButton_->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
        generateAccessCodeButton_->setIconSize(QSize(14, 14));
        generateAccessCodeButton_->setCursor(Qt::PointingHandCursor);
        generateAccessCodeButton_->setFixedHeight(kRowHeight);
        copyAccessCodeButton_ = new QPushButton("Copy");
        copyAccessCodeButton_->setObjectName("SecondaryButton");
        copyAccessCodeButton_->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
        copyAccessCodeButton_->setIconSize(QSize(14, 14));
        copyAccessCodeButton_->setCursor(Qt::PointingHandCursor);
        copyAccessCodeButton_->setFixedHeight(kRowHeight);
        accessLayout->addWidget(accessCodeEdit_, 1);
        accessLayout->addWidget(generateAccessCodeButton_);
        accessLayout->addWidget(copyAccessCodeButton_);
        addRow(content, "Access code", accessRow);

        allowPlaintextCheck_ = new QCheckBox("Allow plaintext");
        addFullRow(content, allowPlaintextCheck_);

        reportCheck_ = new QCheckBox("Save report");
        reportCheck_->setChecked(true);
        addFullRow(content, reportCheck_);

        auto* reportRow = new QWidget;
        reportRow->setObjectName("FormRow");
        auto* reportLayout = new QHBoxLayout(reportRow);
        reportLayout->setContentsMargins(0, 0, 0, 0);
        reportLayout->setSpacing(8);
        reportPathEdit_ = new QLineEdit;
        browseReportButton_ = new QPushButton("Browse");
        browseReportButton_->setObjectName("SecondaryButton");
        browseReportButton_->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
        browseReportButton_->setIconSize(QSize(14, 14));
        browseReportButton_->setCursor(Qt::PointingHandCursor);
        prepareInput(reportPathEdit_);
        browseReportButton_->setFixedHeight(kRowHeight);
        reportLayout->addWidget(reportPathEdit_, 1);
        reportLayout->addWidget(browseReportButton_);
        addRow(content, "File", reportRow);

        connect(reportPathEdit_, &QLineEdit::textEdited, this, [this] {
            reportPathEdited_ = true;
            refreshCommand();
        });
        connect(accessCodeEdit_, &QLineEdit::textChanged, this, [this](const QString& text) {
            if (!text.isEmpty()) {
                allowPlaintextCheck_->setChecked(false);
            }
        });
        connect(generateAccessCodeButton_, &QPushButton::clicked, this, [this] { generateAccessCode(); });
        connect(copyAccessCodeButton_, &QPushButton::clicked, this, [this] { copyAccessCode(); });
        connect(browseReportButton_, &QPushButton::clicked, this, [this] {
            const QString selected = QFileDialog::getSaveFileName(
                this,
                "Save report",
                reportPathEdit_->text(),
                "Zip reports (*.zip)");
            if (!selected.isEmpty()) {
                reportPathEdited_ = true;
                reportPathEdit_->setText(selected);
                refreshCommand();
            }
        });
        return panel;
    }

    QWidget* buildCommandPanel()
    {
        auto* panel = new QFrame;
        panel->setObjectName("Panel");
        panel->setFrameShape(QFrame::NoFrame);
        panel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

        auto* layout = new QVBoxLayout(panel);
        layout->setContentsMargins(18, 14, 18, 16);
        layout->setSpacing(10);
        layout->addWidget(makeLabel("Command", "PanelTitle"));
        commandPreview_ = makeLabel("", "CommandPreview");
        commandPreview_->setWordWrap(true);
        commandPreview_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(commandPreview_);
        return panel;
    }

    QWidget* buildOutputPanel()
    {
        auto* panel = new QFrame;
        panel->setObjectName("Panel");
        panel->setFrameShape(QFrame::NoFrame);

        auto* layout = new QVBoxLayout(panel);
        layout->setContentsMargins(18, 14, 18, 16);
        layout->setSpacing(10);

        auto* header = new QHBoxLayout;
        header->addWidget(makeLabel("Output", "PanelTitle"));
        header->addStretch(1);
        auto* clearButton = new QPushButton("Clear");
        clearButton->setObjectName("GhostButton");
        clearButton->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
        clearButton->setIconSize(QSize(14, 14));
        clearButton->setCursor(Qt::PointingHandCursor);
        header->addWidget(clearButton);
        layout->addLayout(header);

        outputEdit_ = new QPlainTextEdit;
        outputEdit_->setReadOnly(true);
        outputEdit_->setLineWrapMode(QPlainTextEdit::NoWrap);
        layout->addWidget(outputEdit_, 1);
        connect(clearButton, &QPushButton::clicked, outputEdit_, &QPlainTextEdit::clear);
        return panel;
    }

    QHBoxLayout* buildFooter()
    {
        auto* footer = new QHBoxLayout;
        footer->setSpacing(12);

        statusBadge_ = makeLabel("Idle", "StatusIdle");
        statusBadge_->setAlignment(Qt::AlignCenter);
        statusBadge_->setMinimumHeight(34);
        footer->addWidget(statusBadge_);
        footer->addStretch(1);

        actionButton_ = new QPushButton("Start");
        actionButton_->setObjectName("PrimaryButton");
        actionButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        actionButton_->setIconSize(QSize(16, 16));
        actionButton_->setCursor(Qt::PointingHandCursor);
        actionButton_->setMinimumHeight(40);
        actionButton_->setMinimumWidth(140);
        footer->addWidget(actionButton_);

        connect(actionButton_, &QPushButton::clicked, this, [this] { toggleProcess(); });
        return footer;
    }

    void setMode(int index)
    {
        optionStack_->setCurrentIndex(index);
        shareModeButton_->setChecked(index == 0);
        watchModeButton_->setChecked(index == 1);
        refreshReportPath();
        refreshCommand();
    }

    void bindCommandRefresh()
    {
        const auto bindLineEdit = [this](QLineEdit* edit) {
            connect(edit, &QLineEdit::textChanged, this, [this] { refreshCommand(); });
        };
        const auto bindSpinBox = [this](QSpinBox* spin) {
            connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this] { refreshCommand(); });
        };
        const auto bindCheckBox = [this](QCheckBox* check) {
            connect(check, &QCheckBox::toggled, this, [this] { refreshCommand(); });
        };

        bindLineEdit(shareHostEdit_);
        bindSpinBox(sharePortSpin_);
        bindCheckBox(lanDiscoverableCheck_);
        bindSpinBox(displaySpin_);
        bindSpinBox(fpsSpin_);
        connect(resolutionCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { refreshCommand(); });
        bindSpinBox(watchPortSpin_);
        bindCheckBox(mutedCheck_);
        bindSpinBox(volumeSpin_);
        bindSpinBox(previewLatencySpin_);
        bindLineEdit(sessionEdit_);
        bindLineEdit(accessCodeEdit_);
        bindCheckBox(allowPlaintextCheck_);
        bindCheckBox(reportCheck_);
    }

    bool shareMode() const
    {
        return optionStack_->currentIndex() == 0;
    }

    QString defaultReportName() const
    {
        return shareMode() ? "sender-report.zip" : "receiver-report.zip";
    }

    void refreshReportPath()
    {
        if (!reportPathEdited_) {
            reportPathEdit_->setText(defaultReportName());
        }
    }

    void generateAccessCode()
    {
        try {
            accessCodeEdit_->setText(QString::fromStdString(screenshare::GenerateUdpAccessCode()));
            allowPlaintextCheck_->setChecked(false);
            copyAccessCode();
        } catch (const std::exception& error) {
            QMessageBox::critical(this, "Access code", QString::fromLocal8Bit(error.what()));
        }
    }

    void copyAccessCode()
    {
        const QString accessCode = accessCodeEdit_->text();
        if (accessCode.isEmpty()) {
            return;
        }
        QApplication::clipboard()->setText(accessCode);
        appendOutput("Access code copied to clipboard\n");
    }

    QStringList currentArguments() const
    {
        QStringList args;
        if (shareMode()) {
            args << "--share" << (shareHostEdit_->text().trimmed() + ":" + QString::number(sharePortSpin_->value()));
            args << "--display" << QString::number(displaySpin_->value());
            args << "--fps" << QString::number(fpsSpin_->value());

            const QSize resolution = resolutionCombo_->currentData().toSize();
            if (resolution.width() > 0 && resolution.height() > 0) {
                args << "--width" << QString::number(resolution.width());
                args << "--height" << QString::number(resolution.height());
            }
        } else {
            args << "--watch" << QString::number(watchPortSpin_->value());
            if (lanDiscoverableCheck_->isChecked()) {
                args << "--lan-advertise";
            }
            args << "--preview-latency-ms" << QString::number(previewLatencySpin_->value());
            args << "--audio-playback-volume" << QString::number(volumeSpin_->value());
            if (mutedCheck_->isChecked()) {
                args << "--audio-playback-muted";
            }
        }

        const QString session = sessionEdit_->text().trimmed();
        if (!session.isEmpty()) {
            args << "--session" << session;
        }

        const QString accessCode = accessCodeEdit_->text();
        if (!accessCode.isEmpty()) {
            args << "--access-code" << accessCode;
        } else if (allowPlaintextCheck_->isChecked()) {
            args << "--allow-plaintext";
        }

        if (reportCheck_->isChecked() && !reportPathEdit_->text().trimmed().isEmpty()) {
            args << "--save-report" << reportPathEdit_->text().trimmed();
        }

        return args;
    }

    QStringList displayArguments() const
    {
        QStringList args = currentArguments();
        for (int index = 0; index + 1 < args.size(); ++index) {
            if (args[index] == "--access-code") {
                args[index + 1] = "<redacted>";
                ++index;
            }
        }
        return args;
    }

    void refreshCommand()
    {
        if (commandPreview_ == nullptr) {
            return;
        }
        commandPreview_->setText(formatCommand(enginePath(), displayArguments()));
    }

    void applyTheme(bool darkMode)
    {
        qApp->setStyleSheet(appStyleSheet(darkMode));
        repolish(this);
    }

    void toggleProcess()
    {
        if (process_->state() == QProcess::NotRunning) {
            startProcess();
        } else {
            stopProcess();
        }
    }

    void startProcess()
    {
        if (process_->state() != QProcess::NotRunning) {
            return;
        }
        if (shareMode() && shareHostEdit_->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, "Missing address", "Enter a target address before sharing.");
            return;
        }
        if (accessCodeEdit_->text().isEmpty() && !allowPlaintextCheck_->isChecked()) {
            QMessageBox::warning(
                this,
                "Security choice",
                "Generate or paste an access code, or allow plaintext for this run.");
            return;
        }
        const QString program = enginePath();
        if (!QFileInfo::exists(program)) {
            QMessageBox::critical(this, "Missing engine", "ScreenShare.exe was not found beside the UI executable.");
            return;
        }

        stopRequested_ = false;
        forcedStop_ = false;
        cleanupStopFile();
        stopFilePath_ = QDir::temp().filePath(
            "ScreenShare-stop-" +
            QString::number(QCoreApplication::applicationPid()) +
            "-" +
            QString::number(++runSerial_) +
            ".signal");
        QFile::remove(stopFilePath_);

        const QStringList displayArgs = displayArguments();
        QStringList args = currentArguments();
        args << "--stop-file" << stopFilePath_;
        appendOutput("\n" + formatCommand(program, displayArgs) + "\n");
        process_->setProgram(program);
        process_->setArguments(args);
        process_->setWorkingDirectory(QFileInfo(program).absolutePath());
        process_->start();
    }

    void stopProcess()
    {
        if (process_->state() == QProcess::NotRunning) {
            return;
        }
        appendOutput("Stopping...\n");
        stopRequested_ = true;

        QFile stopFile(stopFilePath_);
        if (stopFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            stopFile.write("stop\n");
            stopFile.close();
        } else {
            appendOutput("Could not write graceful stop signal; forcing close if needed\n");
        }

        if (!process_->waitForFinished(7000)) {
            appendOutput("Graceful stop timed out; forcing process closed...\n");
            forcedStop_ = true;
            process_->kill();
            process_->waitForFinished(1000);
        }
    }

    void cleanupStopFile()
    {
        if (!stopFilePath_.isEmpty()) {
            QFile::remove(stopFilePath_);
            stopFilePath_.clear();
        }
    }

    void startDiscovery()
    {
        if (discoveryProcess_->state() != QProcess::NotRunning) {
            return;
        }
        if (process_->state() != QProcess::NotRunning) {
            QMessageBox::information(this, "Already running", "Stop the current session before discovering receivers.");
            return;
        }

        const QString program = enginePath();
        if (!QFileInfo::exists(program)) {
            QMessageBox::critical(this, "Missing engine", "ScreenShare.exe was not found beside the UI executable.");
            return;
        }

        discoveryOutput_.clear();
        appendOutput("\nDiscovering LAN receivers...\n");
        discoveryProcess_->setProgram(program);
        discoveryProcess_->setArguments({"--lan-discover", "--lan-discover-seconds", "2"});
        discoveryProcess_->setWorkingDirectory(QFileInfo(program).absolutePath());
        setDiscovering(true);
        discoveryProcess_->start();
    }

    void finishDiscovery(int code, QProcess::ExitStatus status)
    {
        setDiscovering(false);
        if (status != QProcess::NormalExit || code != 0) {
            appendOutput("LAN discovery finished with exit code " + QString::number(code) + "\n");
            return;
        }

        const QRegularExpression targetPattern(QStringLiteral(R"(share_target=([^:\s]+):(\d+))"));
        const QRegularExpressionMatch match = targetPattern.match(discoveryOutput_);
        if (!match.hasMatch()) {
            QMessageBox::information(this, "No receivers found", "No LAN receivers were found. Start Watch with LAN discoverable enabled on the other computer.");
            return;
        }

        bool ok = false;
        const int port = match.captured(2).toInt(&ok);
        if (!ok || port < sharePortSpin_->minimum() || port > sharePortSpin_->maximum()) {
            appendOutput("LAN discovery returned an invalid port\n");
            return;
        }

        shareHostEdit_->setText(match.captured(1));
        sharePortSpin_->setValue(port);
        appendOutput("Selected LAN receiver " + shareHostEdit_->text() + ":" + QString::number(port) + "\n");
        refreshCommand();
    }

    void setDiscovering(bool discovering)
    {
        if (findLanButton_ != nullptr) {
            findLanButton_->setEnabled(!discovering && process_->state() == QProcess::NotRunning);
            findLanButton_->setText(discovering ? "Finding..." : "Find on LAN");
        }
        if (process_->state() == QProcess::NotRunning) {
            statusBadge_->setText(discovering ? "Finding" : "Idle");
            statusBadge_->setProperty("class", discovering ? "StatusRunning" : "StatusIdle");
            repolish(statusBadge_);
        }
    }

    void setRunning(bool running)
    {
        if (running) {
            actionButton_->setText("Stop");
            actionButton_->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
            actionButton_->setProperty("class", "Danger");
            statusBadge_->setText("Running");
            statusBadge_->setProperty("class", "StatusRunning");
        } else {
            actionButton_->setText("Start");
            actionButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
            actionButton_->setProperty("class", "");
            statusBadge_->setText("Idle");
            statusBadge_->setProperty("class", "StatusIdle");
        }
        repolish(actionButton_);
        repolish(statusBadge_);

        shareModeButton_->setEnabled(!running);
        watchModeButton_->setEnabled(!running);
        if (findLanButton_ != nullptr) {
            findLanButton_->setEnabled(!running && discoveryProcess_->state() == QProcess::NotRunning);
        }
    }

    void appendOutput(const QString& text)
    {
        outputEdit_->moveCursor(QTextCursor::End);
        outputEdit_->insertPlainText(text);
        outputEdit_->moveCursor(QTextCursor::End);
    }

    QStackedWidget* optionStack_ = nullptr;
    QPushButton* shareModeButton_ = nullptr;
    QPushButton* watchModeButton_ = nullptr;
    QLabel* statusBadge_ = nullptr;
    QCheckBox* darkModeCheck_ = nullptr;
    QLabel* commandPreview_ = nullptr;
    QPlainTextEdit* outputEdit_ = nullptr;
    QPushButton* actionButton_ = nullptr;
    QProcess* process_ = nullptr;
    QProcess* discoveryProcess_ = nullptr;

    QLineEdit* shareHostEdit_ = nullptr;
    QSpinBox* sharePortSpin_ = nullptr;
    QPushButton* findLanButton_ = nullptr;
    QSpinBox* displaySpin_ = nullptr;
    QSpinBox* fpsSpin_ = nullptr;
    QComboBox* resolutionCombo_ = nullptr;
    QSpinBox* watchPortSpin_ = nullptr;
    QCheckBox* lanDiscoverableCheck_ = nullptr;
    QCheckBox* mutedCheck_ = nullptr;
    QSpinBox* volumeSpin_ = nullptr;
    QSpinBox* previewLatencySpin_ = nullptr;

    QLineEdit* sessionEdit_ = nullptr;
    QLineEdit* accessCodeEdit_ = nullptr;
    QPushButton* generateAccessCodeButton_ = nullptr;
    QPushButton* copyAccessCodeButton_ = nullptr;
    QCheckBox* allowPlaintextCheck_ = nullptr;
    QCheckBox* reportCheck_ = nullptr;
    QLineEdit* reportPathEdit_ = nullptr;
    QPushButton* browseReportButton_ = nullptr;
    bool reportPathEdited_ = false;
    bool stopRequested_ = false;
    bool forcedStop_ = false;
    quint64 runSerial_ = 0;
    QString stopFilePath_;
    QString discoveryOutput_;
};

QString appStyleSheet(bool darkMode)
{
    if (darkMode) {
        return QString::fromUtf8(R"(
* {
    font-family: "Segoe UI", "Inter", "Arial";
    font-size: 10.5pt;
}
QWidget {
    background: #0e1216;
    color: #e6ecf2;
}
QWidget#LeftHost, QWidget#OptionPage, QWidget#FormRow,
QStackedWidget#OptionStack {
    background: transparent;
}
QLabel {
    background: transparent;
}
QLabel[class="HeroTitle"] {
    font-size: 18pt;
    font-weight: 700;
    color: #f4f7fb;
    letter-spacing: 0.3px;
}
QLabel[class="Subtle"] {
    color: #8a96a5;
    font-size: 9.5pt;
}
QLabel[class="PanelTitle"] {
    font-size: 10.5pt;
    font-weight: 700;
    color: #cdd6e1;
    text-transform: uppercase;
    letter-spacing: 1.2px;
}
QLabel[class="FieldLabel"] {
    color: #9aa6b5;
    font-weight: 500;
}
QLabel[class="CommandPreview"] {
    background: #050709;
    color: #d8f3ec;
    border: 1px solid #1a2530;
    border-radius: 8px;
    padding: 12px 14px;
    font-family: "Cascadia Mono", "Consolas";
    font-size: 9.5pt;
}
QLabel[class="StatusIdle"], QLabel[class="StatusRunning"] {
    border-radius: 17px;
    padding: 0 18px;
    font-weight: 650;
    min-width: 96px;
}
QLabel[class="StatusIdle"] {
    background: #1a212a;
    color: #93a0b0;
    border: 1px solid #232c37;
}
QLabel[class="StatusRunning"] {
    background: #0f2e2a;
    color: #6cdcc6;
    border: 1px solid #1f4a44;
}
QFrame#Panel {
    background: #161b22;
    border: 1px solid #232c37;
    border-radius: 10px;
}
QFrame#ModeBar {
    background: #161b22;
    border: 1px solid #232c37;
    border-radius: 12px;
}
QPushButton#ModeButton {
    background: transparent;
    color: #a5b1c0;
    border: 0;
    border-radius: 8px;
    padding: 9px 22px;
    font-weight: 650;
}
QPushButton#ModeButton:hover {
    background: #1c232c;
    color: #e6ecf2;
}
QPushButton#ModeButton:checked {
    background: #1a9b89;
    color: #ffffff;
}
QPushButton#ModeButton:disabled {
    color: #4d5663;
}
QLineEdit, QSpinBox, QComboBox {
    background: #0b0f14;
    border: 1px solid #2a3340;
    border-radius: 6px;
    color: #e6ecf2;
    padding: 6px 9px;
    selection-background-color: #1a9b89;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus {
    border: 1px solid #22b8a5;
}
QSpinBox::up-button, QSpinBox::down-button {
    width: 16px;
    background: transparent;
    border: 0;
}
QComboBox::drop-down {
    border: 0;
    width: 22px;
}
QComboBox QAbstractItemView {
    background: #161b22;
    color: #e6ecf2;
    border: 1px solid #2a3340;
    selection-background-color: #1a9b89;
    padding: 4px;
}
QCheckBox {
    spacing: 8px;
    color: #c9d2dd;
    background: transparent;
}
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border-radius: 4px;
    border: 1px solid #3a4655;
    background: #0b0f14;
}
QCheckBox::indicator:checked {
    background: #1a9b89;
    border: 1px solid #1a9b89;
    image: none;
}
QCheckBox#ThemeSwitch {
    color: #aab5c3;
    font-weight: 600;
    background: transparent;
}
QPushButton {
    border: 0;
    border-radius: 8px;
    padding: 9px 18px;
    font-weight: 650;
    color: #ffffff;
}
QPushButton#PrimaryButton {
    background: #1a9b89;
    color: #ffffff;
    padding: 0 24px;
}
QPushButton#PrimaryButton:hover {
    background: #21b8a3;
}
QPushButton#PrimaryButton:pressed {
    background: #148876;
}
QPushButton#PrimaryButton[class="Danger"] {
    background: #c64a4a;
}
QPushButton#PrimaryButton[class="Danger"]:hover {
    background: #d96060;
}
QPushButton#PrimaryButton[class="Danger"]:pressed {
    background: #a83b3b;
}
QPushButton#SecondaryButton {
    background: #1f2731;
    color: #dde4ee;
}
QPushButton#SecondaryButton:hover {
    background: #28323e;
}
QPushButton#GhostButton {
    background: transparent;
    color: #93a0b0;
    padding: 6px 12px;
}
QPushButton#GhostButton:hover {
    background: #1c232c;
    color: #e6ecf2;
}
QPushButton:disabled {
    background: #1a2027;
    color: #5d6776;
}
QPlainTextEdit {
    background: #050709;
    color: #d8f3ec;
    border: 1px solid #1a2530;
    border-radius: 8px;
    padding: 12px;
    font-family: "Cascadia Mono", "Consolas";
    font-size: 9.5pt;
}
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 4px;
}
QScrollBar::handle:vertical {
    background: #3a4655;
    border-radius: 4px;
    min-height: 28px;
}
QScrollBar::handle:vertical:hover {
    background: #4a5667;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0;
    background: transparent;
}
)");
    }

    return QString::fromUtf8(R"(
* {
    font-family: "Segoe UI", "Inter", "Arial";
    font-size: 10.5pt;
}
QWidget {
    background: #f4f6fa;
    color: #15202b;
}
QWidget#LeftHost, QWidget#OptionPage, QWidget#FormRow,
QStackedWidget#OptionStack {
    background: transparent;
}
QLabel {
    background: transparent;
}
QLabel[class="HeroTitle"] {
    font-size: 18pt;
    font-weight: 700;
    color: #0f1820;
    letter-spacing: 0.3px;
}
QLabel[class="Subtle"] {
    color: #5e6b7a;
    font-size: 9.5pt;
}
QLabel[class="PanelTitle"] {
    font-size: 10.5pt;
    font-weight: 700;
    color: #3a4756;
    text-transform: uppercase;
    letter-spacing: 1.2px;
}
QLabel[class="FieldLabel"] {
    color: #5e6b7a;
    font-weight: 500;
}
QLabel[class="CommandPreview"] {
    background: #0f1820;
    color: #e6f3ef;
    border-radius: 8px;
    padding: 12px 14px;
    font-family: "Cascadia Mono", "Consolas";
    font-size: 9.5pt;
}
QLabel[class="StatusIdle"], QLabel[class="StatusRunning"] {
    border-radius: 17px;
    padding: 0 18px;
    font-weight: 650;
    min-width: 96px;
}
QLabel[class="StatusIdle"] {
    background: #e6eaf1;
    color: #4f5d6e;
    border: 1px solid #dadfe7;
}
QLabel[class="StatusRunning"] {
    background: #d9f1ea;
    color: #0f6b5d;
    border: 1px solid #b6e2d5;
}
QFrame#Panel {
    background: #ffffff;
    border: 1px solid #e0e5ec;
    border-radius: 10px;
}
QFrame#ModeBar {
    background: #ffffff;
    border: 1px solid #e0e5ec;
    border-radius: 12px;
}
QPushButton#ModeButton {
    background: transparent;
    color: #5e6b7a;
    border: 0;
    border-radius: 8px;
    padding: 9px 22px;
    font-weight: 650;
}
QPushButton#ModeButton:hover {
    background: #eef1f6;
    color: #15202b;
}
QPushButton#ModeButton:checked {
    background: #157a6e;
    color: #ffffff;
}
QPushButton#ModeButton:disabled {
    color: #aab3bf;
}
QLineEdit, QSpinBox, QComboBox {
    background: #ffffff;
    border: 1px solid #cfd6df;
    border-radius: 6px;
    color: #15202b;
    padding: 6px 9px;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus {
    border: 1px solid #157a6e;
}
QSpinBox::up-button, QSpinBox::down-button {
    width: 16px;
    background: transparent;
    border: 0;
}
QComboBox::drop-down {
    border: 0;
    width: 22px;
}
QCheckBox {
    spacing: 8px;
    background: transparent;
}
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border-radius: 4px;
    border: 1px solid #b8c1ce;
    background: #ffffff;
}
QCheckBox::indicator:checked {
    background: #157a6e;
    border: 1px solid #157a6e;
}
QCheckBox#ThemeSwitch {
    color: #4f5d6e;
    font-weight: 600;
    background: transparent;
}
QPushButton {
    border: 0;
    border-radius: 8px;
    padding: 9px 18px;
    font-weight: 650;
}
QPushButton#PrimaryButton {
    background: #157a6e;
    color: #ffffff;
    padding: 0 24px;
}
QPushButton#PrimaryButton:hover {
    background: #1a9385;
}
QPushButton#PrimaryButton:pressed {
    background: #0f6b5d;
}
QPushButton#PrimaryButton[class="Danger"] {
    background: #c84545;
    color: #ffffff;
}
QPushButton#PrimaryButton[class="Danger"]:hover {
    background: #d65b5b;
}
QPushButton#PrimaryButton[class="Danger"]:pressed {
    background: #ad3838;
}
QPushButton#SecondaryButton {
    background: #eef1f6;
    color: #243140;
}
QPushButton#SecondaryButton:hover {
    background: #e2e7ee;
}
QPushButton#GhostButton {
    background: transparent;
    color: #5e6b7a;
    padding: 6px 12px;
}
QPushButton#GhostButton:hover {
    background: #eef1f6;
    color: #15202b;
}
QPushButton:disabled {
    background: #dee3ec;
    color: #8a95a3;
}
QPlainTextEdit {
    background: #0f1820;
    color: #e9f3f1;
    border: 0;
    border-radius: 8px;
    padding: 12px;
    font-family: "Cascadia Mono", "Consolas";
    font-size: 9.5pt;
}
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 4px;
}
QScrollBar::handle:vertical {
    background: #c4ccd7;
    border-radius: 4px;
    min-height: 28px;
}
QScrollBar::handle:vertical:hover {
    background: #aab5c3;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0;
    background: transparent;
}
)");
}

} // namespace

int main(int argc, char** argv)
{
    for (int index = 1; index < argc; ++index) {
        const QString arg = QString::fromLocal8Bit(argv[index]);
        if (arg == "--self-test") {
            return QFileInfo::exists(enginePath()) ? 0 : 1;
        }
    }

    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    app.setStyleSheet(appStyleSheet(true));

    MainWindow window;
    window.show();
    return app.exec();
}
