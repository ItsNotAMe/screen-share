#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QSize>
#include <QtCore/QStringList>
#include <QtGui/QTextCursor>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyle>
#include <QtWidgets/QTabWidget>
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
    if (!className.isEmpty()) {
        label->setProperty("class", className);
    }
    return label;
}

QFrame* makePanel()
{
    auto* frame = new QFrame;
    frame->setObjectName("Panel");
    frame->setFrameShape(QFrame::NoFrame);
    return frame;
}

void configureFormGrid(QGridLayout* grid)
{
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(12);
    grid->setColumnMinimumWidth(0, 96);
    grid->setColumnStretch(1, 1);
}

QGridLayout* addFormPanel(QVBoxLayout* parent, const QString& title)
{
    auto* panel = makePanel();
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(16, 14, 16, 16);
    panelLayout->setSpacing(12);
    panelLayout->addWidget(makeLabel(title, "PanelTitle"));

    auto* grid = new QGridLayout;
    configureFormGrid(grid);
    panelLayout->addLayout(grid);
    parent->addWidget(panel);
    return grid;
}

QString appStyleSheet(bool darkMode);

class MainWindow final : public QWidget {
public:
    MainWindow()
    {
        setWindowTitle("ScreenShare");
        resize(1080, 720);
        setMinimumSize(960, 620);

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
            const QString statusText = status == QProcess::NormalExit ? "finished" : "crashed";
            appendOutput("Process " + statusText + " with exit code " + QString::number(code) + "\n");
            setRunning(false);
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
        root->setContentsMargins(24, 22, 24, 22);
        root->setSpacing(18);

        auto* header = new QHBoxLayout;
        auto* titleBlock = new QVBoxLayout;
        titleBlock->setSpacing(4);
        titleBlock->addWidget(makeLabel("ScreenShare", "HeroTitle"));
        titleBlock->addWidget(makeLabel("Fast local screen sharing", "Subtle"));
        header->addLayout(titleBlock, 1);

        darkModeCheck_ = new QCheckBox("Dark");
        darkModeCheck_->setObjectName("ThemeSwitch");
        darkModeCheck_->setChecked(true);
        header->addWidget(darkModeCheck_);

        statusBadge_ = makeLabel("Idle", "StatusIdle");
        statusBadge_->setAlignment(Qt::AlignCenter);
        header->addWidget(statusBadge_);
        root->addLayout(header);

        auto* body = new QHBoxLayout;
        body->setSpacing(18);
        root->addLayout(body, 1);

        auto* controlsColumn = new QVBoxLayout;
        controlsColumn->setSpacing(14);
        body->addLayout(controlsColumn, 0);

        modeTabs_ = new QTabWidget;
        modeTabs_->setObjectName("ModeTabs");
        modeTabs_->addTab(buildShareTab(), "Share");
        modeTabs_->addTab(buildWatchTab(), "Watch");
        controlsColumn->addWidget(modeTabs_);

        controlsColumn->addWidget(buildSessionPanel());
        controlsColumn->addStretch(1);

        auto* outputColumn = new QVBoxLayout;
        outputColumn->setSpacing(14);
        body->addLayout(outputColumn, 1);
        outputColumn->addWidget(buildCommandPanel());
        outputColumn->addWidget(buildOutputPanel(), 1);

        auto* footer = new QHBoxLayout;
        footer->addStretch(1);
        stopButton_ = new QPushButton("Stop");
        startButton_ = new QPushButton("Start");
        startButton_->setObjectName("PrimaryButton");
        stopButton_->setObjectName("SecondaryButton");
        startButton_->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        stopButton_->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
        startButton_->setIconSize(QSize(16, 16));
        stopButton_->setIconSize(QSize(16, 16));
        footer->addWidget(stopButton_);
        footer->addWidget(startButton_);
        root->addLayout(footer);

        connect(modeTabs_, &QTabWidget::currentChanged, this, [this] {
            refreshReportPath();
            refreshCommand();
        });
        connect(startButton_, &QPushButton::clicked, this, [this] { startProcess(); });
        connect(stopButton_, &QPushButton::clicked, this, [this] { stopProcess(); });
        connect(darkModeCheck_, &QCheckBox::toggled, this, [this](bool checked) { applyTheme(checked); });

        bindCommandRefresh(this);
        modeTabs_->setMinimumWidth(380);
        applyTheme(darkModeCheck_->isChecked());
    }

    QWidget* buildShareTab()
    {
        auto* page = new QWidget;
        page->setObjectName("TabPage");
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 22, 0, 0);
        layout->setSpacing(16);

        auto* networkGrid = addFormPanel(layout, "Friend");
        shareHostEdit_ = new QLineEdit("127.0.0.1");
        sharePortSpin_ = new QSpinBox;
        sharePortSpin_->setRange(1, 65535);
        sharePortSpin_->setValue(5000);
        networkGrid->addWidget(makeLabel("Address"), 0, 0);
        networkGrid->addWidget(shareHostEdit_, 0, 1);
        networkGrid->addWidget(makeLabel("Port"), 1, 0);
        networkGrid->addWidget(sharePortSpin_, 1, 1);

        auto* videoGrid = addFormPanel(layout, "Video");
        displaySpin_ = new QSpinBox;
        displaySpin_->setRange(0, 16);
        displaySpin_->setValue(0);
        fpsSpin_ = new QSpinBox;
        fpsSpin_->setRange(15, 240);
        fpsSpin_->setValue(60);
        resolutionCombo_ = new QComboBox;
        resolutionCombo_->addItem("Native", QSize(0, 0));
        resolutionCombo_->addItem("1920 x 1080", QSize(1920, 1080));
        resolutionCombo_->addItem("1600 x 900", QSize(1600, 900));
        resolutionCombo_->addItem("1280 x 720", QSize(1280, 720));
        videoGrid->addWidget(makeLabel("Display"), 0, 0);
        videoGrid->addWidget(displaySpin_, 0, 1);
        videoGrid->addWidget(makeLabel("FPS"), 1, 0);
        videoGrid->addWidget(fpsSpin_, 1, 1);
        videoGrid->addWidget(makeLabel("Resolution"), 2, 0);
        videoGrid->addWidget(resolutionCombo_, 2, 1);

        layout->addStretch(1);
        return page;
    }

    QWidget* buildWatchTab()
    {
        auto* page = new QWidget;
        page->setObjectName("TabPage");
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 22, 0, 0);
        layout->setSpacing(16);

        auto* networkGrid = addFormPanel(layout, "Listen");
        watchPortSpin_ = new QSpinBox;
        watchPortSpin_->setRange(1, 65535);
        watchPortSpin_->setValue(5000);
        networkGrid->addWidget(makeLabel("Port"), 0, 0);
        networkGrid->addWidget(watchPortSpin_, 0, 1);

        auto* audioGrid = addFormPanel(layout, "Audio");
        mutedCheck_ = new QCheckBox("Muted playback");
        volumeSpin_ = new QSpinBox;
        volumeSpin_->setRange(0, 200);
        volumeSpin_->setSuffix("%");
        volumeSpin_->setValue(100);
        audioGrid->addWidget(mutedCheck_, 0, 0, 1, 2);
        audioGrid->addWidget(makeLabel("Volume"), 1, 0);
        audioGrid->addWidget(volumeSpin_, 1, 1);

        auto* timingGrid = addFormPanel(layout, "Timing");
        previewLatencySpin_ = new QSpinBox;
        previewLatencySpin_->setRange(0, 2000);
        previewLatencySpin_->setSuffix(" ms");
        previewLatencySpin_->setValue(100);
        timingGrid->addWidget(makeLabel("Preview latency"), 0, 0);
        timingGrid->addWidget(previewLatencySpin_, 0, 1);
        layout->addStretch(1);
        return page;
    }

    QWidget* buildSessionPanel()
    {
        auto* panel = makePanel();
        auto* layout = new QVBoxLayout(panel);
        layout->setContentsMargins(16, 14, 16, 14);
        layout->setSpacing(10);
        layout->addWidget(makeLabel("Session", "PanelTitle"));

        sessionEdit_ = new QLineEdit;
        sessionEdit_->setPlaceholderText("Auto");
        layout->addWidget(makeLabel("ID", "TinyLabel"));
        layout->addWidget(sessionEdit_);

        reportCheck_ = new QCheckBox("Save report");
        reportCheck_->setChecked(true);
        layout->addWidget(reportCheck_);

        auto* reportRow = new QHBoxLayout;
        reportPathEdit_ = new QLineEdit;
        browseReportButton_ = new QPushButton("Browse");
        browseReportButton_->setObjectName("SecondaryButton");
        browseReportButton_->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
        browseReportButton_->setIconSize(QSize(16, 16));
        reportRow->addWidget(reportPathEdit_, 1);
        reportRow->addWidget(browseReportButton_);
        layout->addLayout(reportRow);

        connect(reportPathEdit_, &QLineEdit::textEdited, this, [this] {
            reportPathEdited_ = true;
            refreshCommand();
        });
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
        auto* panel = makePanel();
        auto* layout = new QVBoxLayout(panel);
        layout->setContentsMargins(16, 14, 16, 14);
        layout->setSpacing(10);
        layout->addWidget(makeLabel("Command", "PanelTitle"));
        commandPreview_ = makeLabel("", "CommandPreview");
        commandPreview_->setWordWrap(true);
        layout->addWidget(commandPreview_);
        return panel;
    }

    QWidget* buildOutputPanel()
    {
        auto* panel = makePanel();
        auto* layout = new QVBoxLayout(panel);
        layout->setContentsMargins(16, 14, 16, 14);
        layout->setSpacing(10);

        auto* header = new QHBoxLayout;
        header->addWidget(makeLabel("Output", "PanelTitle"));
        header->addStretch(1);
        auto* clearButton = new QPushButton("Clear");
        clearButton->setObjectName("SecondaryButton");
        clearButton->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
        clearButton->setIconSize(QSize(16, 16));
        header->addWidget(clearButton);
        layout->addLayout(header);

        outputEdit_ = new QPlainTextEdit;
        outputEdit_->setReadOnly(true);
        outputEdit_->setLineWrapMode(QPlainTextEdit::NoWrap);
        layout->addWidget(outputEdit_, 1);
        connect(clearButton, &QPushButton::clicked, outputEdit_, &QPlainTextEdit::clear);
        return panel;
    }

    void bindCommandRefresh(QObject* parent)
    {
        const auto bindLineEdit = [this, parent](QLineEdit* edit) {
            connect(edit, &QLineEdit::textChanged, parent, [this] { refreshCommand(); });
        };
        const auto bindSpinBox = [this, parent](QSpinBox* spin) {
            connect(spin, qOverload<int>(&QSpinBox::valueChanged), parent, [this] { refreshCommand(); });
        };
        const auto bindCheckBox = [this, parent](QCheckBox* check) {
            connect(check, &QCheckBox::toggled, parent, [this] { refreshCommand(); });
        };

        bindLineEdit(shareHostEdit_);
        bindSpinBox(sharePortSpin_);
        bindSpinBox(displaySpin_);
        bindSpinBox(fpsSpin_);
        connect(resolutionCombo_, qOverload<int>(&QComboBox::currentIndexChanged), parent, [this] { refreshCommand(); });
        bindSpinBox(watchPortSpin_);
        bindCheckBox(mutedCheck_);
        bindSpinBox(volumeSpin_);
        bindSpinBox(previewLatencySpin_);
        bindLineEdit(sessionEdit_);
        bindCheckBox(reportCheck_);
    }

    bool shareMode() const
    {
        return modeTabs_->currentIndex() == 0;
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

        if (reportCheck_->isChecked() && !reportPathEdit_->text().trimmed().isEmpty()) {
            args << "--save-report" << reportPathEdit_->text().trimmed();
        }

        return args;
    }

    void refreshCommand()
    {
        if (commandPreview_ == nullptr) {
            return;
        }
        commandPreview_->setText(formatCommand(enginePath(), currentArguments()));
    }

    void applyTheme(bool darkMode)
    {
        qApp->setStyleSheet(appStyleSheet(darkMode));
        if (layout() != nullptr) {
            layout()->invalidate();
            layout()->activate();
        }
        if (statusBadge_ != nullptr) {
            statusBadge_->style()->unpolish(statusBadge_);
            statusBadge_->style()->polish(statusBadge_);
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
        const QString program = enginePath();
        if (!QFileInfo::exists(program)) {
            QMessageBox::critical(this, "Missing engine", "ScreenShare.exe was not found beside the UI executable.");
            return;
        }

        const QStringList args = currentArguments();
        appendOutput("\n" + formatCommand(program, args) + "\n");
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
        process_->terminate();
        if (!process_->waitForFinished(2500)) {
            process_->kill();
        }
    }

    void setRunning(bool running)
    {
        startButton_->setEnabled(!running);
        stopButton_->setEnabled(running);
        statusBadge_->setText(running ? "Running" : "Idle");
        statusBadge_->setProperty("class", running ? "StatusRunning" : "StatusIdle");
        statusBadge_->style()->unpolish(statusBadge_);
        statusBadge_->style()->polish(statusBadge_);
    }

    void appendOutput(const QString& text)
    {
        outputEdit_->moveCursor(QTextCursor::End);
        outputEdit_->insertPlainText(text);
        outputEdit_->moveCursor(QTextCursor::End);
    }

    QTabWidget* modeTabs_ = nullptr;
    QLabel* statusBadge_ = nullptr;
    QCheckBox* darkModeCheck_ = nullptr;
    QLabel* commandPreview_ = nullptr;
    QPlainTextEdit* outputEdit_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QProcess* process_ = nullptr;

    QLineEdit* shareHostEdit_ = nullptr;
    QSpinBox* sharePortSpin_ = nullptr;
    QSpinBox* displaySpin_ = nullptr;
    QSpinBox* fpsSpin_ = nullptr;
    QComboBox* resolutionCombo_ = nullptr;
    QSpinBox* watchPortSpin_ = nullptr;
    QCheckBox* mutedCheck_ = nullptr;
    QSpinBox* volumeSpin_ = nullptr;
    QSpinBox* previewLatencySpin_ = nullptr;

    QLineEdit* sessionEdit_ = nullptr;
    QCheckBox* reportCheck_ = nullptr;
    QLineEdit* reportPathEdit_ = nullptr;
    QPushButton* browseReportButton_ = nullptr;
    bool reportPathEdited_ = false;
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
    background: #101418;
    color: #edf2f7;
}
QWidget#TabPage, QLabel {
    background: transparent;
}
QLabel[class="HeroTitle"] {
    font-size: 25pt;
    font-weight: 700;
    color: #f6f8fb;
}
QLabel[class="Subtle"], QLabel[class="TinyLabel"] {
    color: #9aa7b5;
}
QLabel[class="PanelTitle"] {
    font-size: 12pt;
    font-weight: 650;
    color: #edf2f7;
}
QLabel[class="CommandPreview"] {
    background: #07090c;
    color: #e7f7f4;
    border: 1px solid #202a34;
    border-radius: 8px;
    padding: 12px;
    font-family: "Cascadia Mono", "Consolas";
    font-size: 9.5pt;
}
QLabel[class="StatusIdle"], QLabel[class="StatusRunning"] {
    border-radius: 14px;
    padding: 6px 14px;
    font-weight: 650;
    min-width: 78px;
}
QLabel[class="StatusIdle"] {
    background: #232b34;
    color: #b4bfcb;
}
QLabel[class="StatusRunning"] {
    background: #143b35;
    color: #6ee2cf;
}
QFrame#Panel {
    background: #171c22;
    border: 1px solid #2a333e;
    border-radius: 8px;
}
QLineEdit, QSpinBox, QComboBox {
    background: #0d1116;
    border: 1px solid #354150;
    border-radius: 6px;
    color: #edf2f7;
    padding: 7px 8px;
    min-height: 24px;
    selection-background-color: #1a9b89;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus {
    border: 1px solid #22b8a5;
}
QComboBox QAbstractItemView {
    background: #171c22;
    color: #edf2f7;
    border: 1px solid #354150;
    selection-background-color: #1a9b89;
}
QCheckBox {
    spacing: 8px;
    color: #d8e0ea;
}
QCheckBox#ThemeSwitch {
    color: #cbd5e1;
    font-weight: 650;
}
QTabWidget::pane {
    border: 0;
    background: transparent;
    margin-top: 8px;
}
QTabBar::tab {
    background: #232b34;
    color: #aeb9c6;
    border-radius: 8px;
    padding: 9px 28px;
    margin-right: 8px;
    font-weight: 650;
}
QTabBar::tab:selected {
    background: #1a9b89;
    color: #ffffff;
}
QPushButton {
    border: 0;
    border-radius: 8px;
    padding: 9px 18px;
    font-weight: 650;
}
QPushButton#PrimaryButton {
    background: #1a9b89;
    color: #ffffff;
}
QPushButton#PrimaryButton:hover {
    background: #148876;
}
QPushButton#SecondaryButton {
    background: #252e38;
    color: #e4ebf3;
}
QPushButton#SecondaryButton:hover {
    background: #303a46;
}
QPushButton:disabled {
    background: #222a33;
    color: #6d7885;
}
QPlainTextEdit {
    background: #07090c;
    color: #e7f7f4;
    border: 1px solid #202a34;
    border-radius: 8px;
    padding: 12px;
    font-family: "Cascadia Mono", "Consolas";
    font-size: 9.5pt;
}
QScrollBar:vertical {
    background: #07090c;
    width: 12px;
}
QScrollBar::handle:vertical {
    background: #3d4a58;
    border-radius: 6px;
    min-height: 28px;
}
)");
    }

    return QString::fromUtf8(R"(
* {
    font-family: "Segoe UI", "Inter", "Arial";
    font-size: 10.5pt;
}
QWidget {
    background: #f7f8fa;
    color: #17202a;
}
QWidget#TabPage, QLabel {
    background: transparent;
}
QLabel[class="HeroTitle"] {
    font-size: 25pt;
    font-weight: 700;
    color: #101820;
}
QLabel[class="Subtle"], QLabel[class="TinyLabel"] {
    color: #657080;
}
QLabel[class="PanelTitle"] {
    font-size: 12pt;
    font-weight: 650;
    color: #17202a;
}
QLabel[class="CommandPreview"] {
    background: #101820;
    color: #edf7f5;
    border-radius: 8px;
    padding: 12px;
    font-family: "Cascadia Mono", "Consolas";
    font-size: 9.5pt;
}
QLabel[class="StatusIdle"], QLabel[class="StatusRunning"] {
    border-radius: 14px;
    padding: 6px 14px;
    font-weight: 650;
    min-width: 78px;
}
QLabel[class="StatusIdle"] {
    background: #e9edf3;
    color: #526070;
}
QLabel[class="StatusRunning"] {
    background: #dff3ed;
    color: #116b5f;
}
QFrame#Panel {
    background: #ffffff;
    border: 1px solid #dfe4ea;
    border-radius: 8px;
}
QLineEdit, QSpinBox, QComboBox {
    background: #ffffff;
    border: 1px solid #cfd6df;
    border-radius: 6px;
    padding: 7px 8px;
    min-height: 24px;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus {
    border: 1px solid #157a6e;
}
QCheckBox {
    spacing: 8px;
}
QCheckBox#ThemeSwitch {
    color: #526070;
    font-weight: 650;
}
QTabWidget::pane {
    border: 0;
    background: transparent;
    margin-top: 8px;
}
QTabBar::tab {
    background: #eceff4;
    color: #526070;
    border-radius: 8px;
    padding: 9px 28px;
    margin-right: 8px;
    font-weight: 650;
}
QTabBar::tab:selected {
    background: #157a6e;
    color: #ffffff;
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
}
QPushButton#PrimaryButton:hover {
    background: #116b5f;
}
QPushButton#SecondaryButton {
    background: #e9edf3;
    color: #243140;
}
QPushButton#SecondaryButton:hover {
    background: #dfe5ec;
}
QPushButton:disabled {
    background: #d7dde5;
    color: #8a95a3;
}
QPlainTextEdit {
    background: #101820;
    color: #e9f3f1;
    border: 0;
    border-radius: 8px;
    padding: 12px;
    font-family: "Cascadia Mono", "Consolas";
    font-size: 9.5pt;
}
QScrollBar:vertical {
    background: #101820;
    width: 12px;
}
QScrollBar::handle:vertical {
    background: #4b5a68;
    border-radius: 6px;
    min-height: 28px;
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
