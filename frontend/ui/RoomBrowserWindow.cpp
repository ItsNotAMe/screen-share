#include "ui/RoomBrowserWindow.h"
#include "ui/RoomApplication.h"
#include "shared/RoomLink.h"
#include "ui/UiStyle.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/win32_socket_init.h"
#include "rtc_base/logging.h"
#include <QApplication>
#include <QDialog>
#include <QPointer>
#include <QStackedWidget>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QResizeEvent>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QButtonGroup>
#include <QClipboard>
#include <QGridLayout>
#include <QListWidget>
#include <QScreen>
#include <QToolButton>
#include <QScrollBar>
#include <QPainter>
#include <QFile>
#include <QSvgRenderer>
using namespace screenshare;
using namespace screenshare::room::qt;

namespace {
QIcon entryIcon(const QString& name) {
    QFile file(QString(":/screenshare/ui/icons/%1.svg").arg(name)); if (!file.open(QIODevice::ReadOnly)) return {};
    auto svg = file.readAll(); svg.replace("currentColor", "#b7c8c0");
    QSvgRenderer renderer(svg); QPixmap pixels(24,24); pixels.fill(Qt::transparent);
    QPainter painter(&pixels); renderer.render(&painter); return QIcon(pixels);
}
QLabel* fieldLabel(const QString& text) {
    auto* label = new QLabel(text); label->setObjectName("OptionLabel"); return label;
}
QWidget* optionField(const QString& text, QWidget* control) {
    auto* field = new QWidget;
    auto* layout = new QVBoxLayout(field); layout->setContentsMargins(0,0,0,0); layout->setSpacing(6);
    auto* label = fieldLabel(text); label->setBuddy(control);
    layout->addWidget(label); layout->addWidget(control); return field;
}
QWidget* segments(const QStringList& labels, int selected, QWidget* owner, std::function<void(int)> change) {
    auto* widget = new QWidget(owner); widget->setObjectName("SegmentGroup");
    auto* layout = new QHBoxLayout(widget); layout->setContentsMargins(0,0,0,0); layout->setSpacing(6);
    auto* group = new QButtonGroup(widget); group->setExclusive(true);
    for (int i = 0; i < labels.size(); ++i) {
        auto* button = new QPushButton(labels[i]); button->setCheckable(true); button->setChecked(i == selected);
        button->setObjectName("SegmentButton"); button->setMinimumHeight(42);
        group->addButton(button, i); layout->addWidget(button, 1);
    }
    QObject::connect(group, &QButtonGroup::idClicked, owner, std::move(change));
    return widget;
}
QWidget* disclosure(const QString& title, QWidget* content, QWidget* owner) {
    auto* block = new QWidget(owner); block->setObjectName("DisclosureCard");
    auto* layout = new QVBoxLayout(block); layout->setContentsMargins(10,6,10,10); layout->setSpacing(8);
    auto* toggle = new QToolButton; toggle->setText(title); toggle->setCheckable(true);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); toggle->setArrowType(Qt::RightArrow);
    toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); toggle->setObjectName("OptionsDisclosure");
    layout->addWidget(toggle); layout->addWidget(content); content->hide();
    QObject::connect(toggle, &QToolButton::toggled, block, [toggle, content](bool open) {
        content->setVisible(open); toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    });
    return block;
}
}

RoomBrowserWindow::RoomBrowserWindow(QUrl origin, QtRoomSession::Factory factory, bool loopback, QString profileFile, bool enumerateSources)
    : origin_(std::move(origin)), factory_(std::move(factory)), loopback_(loopback), enumerateSources_(enumerateSources), profile_(profileFile), directory_(loopback) {
    setWindowTitle("ScreenShare — Rooms"); setStyleSheet(uiStyleSheet()); resize(900, 720);
    setObjectName("RoomBrowser");
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(28, 20, 28, 24); layout->setSpacing(16);
    heading_ = new QLabel("Create a room"); heading_->setObjectName("PageHeading"); layout->addWidget(heading_);
    auto* scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setObjectName("RoomBrowserScroll");
    auto* content = new QWidget; auto* body = new QVBoxLayout(content); body->setContentsMargins(0,0,8,0); body->setSpacing(18);
    scroll->setWidget(content); layout->addWidget(scroll, 1);
    createPanel_ = new QWidget; createColumns_ = new QBoxLayout(QBoxLayout::TopToBottom, createPanel_);
    createColumns_->setContentsMargins(0,0,0,0); createColumns_->setSpacing(24);
    auto* details = new QWidget; details->setObjectName("FormCard"); detailsBody_ = new QVBoxLayout(details); createColumns_->addWidget(details, 1);
    detailsBody_->setContentsMargins(20,20,20,20); detailsBody_->setSpacing(18);
    auto* detailsHeading = new QLabel("Room details"); detailsHeading->setObjectName("SectionHeading"); detailsBody_->addWidget(detailsHeading);
    name_ = new QLineEdit("My room"); name_->setObjectName("roomName"); name_->setMaxLength(256); detailsBody_->addWidget(optionField("Room name", name_));
    public_ = new QCheckBox(this); public_->setObjectName("publicRoom"); public_->setChecked(true); public_->hide();
    auto* visibility = segments({"Public", "Private"}, 0, details, [this](int index) { public_->setChecked(index == 0); });
    visibility->setObjectName("RoomVisibility");
    connect(public_, &QCheckBox::toggled, visibility, [visibility](bool enabled) { visibility->findChild<QButtonGroup*>()->button(enabled ? 0 : 1)->setChecked(true); });
    detailsBody_->addWidget(optionField("Room visibility", visibility));
    password_ = new QLineEdit; password_->setObjectName("roomPassword"); password_->setEchoMode(QLineEdit::Password); password_->setMaxLength(256);
    password_->setPlaceholderText("Password");
    passwordPanel_ = optionField("Password (optional)", password_); passwordPanel_->setObjectName("RoomPasswordPanel"); detailsBody_->addWidget(passwordPanel_);
    passwordActions_ = new QWidget; auto* passwordActions = new QHBoxLayout(passwordActions_); passwordActions->setContentsMargins(0,0,0,0); passwordActions->addStretch();
    auto* cancelPassword = new QPushButton("Cancel"); cancelPassword->setObjectName("cancelRoomPassword"); passwordActions->addWidget(cancelPassword);
    auto* joinPassword = new QPushButton("Join"); joinPassword->setObjectName("joinWithPassword"); passwordActions->addWidget(joinPassword);
    static_cast<QVBoxLayout*>(passwordPanel_->layout())->addWidget(passwordActions_); passwordActions_->hide();
    connect(cancelPassword, &QPushButton::clicked, this, [this] { password_->clear(); passwordPanel_->hide(); findChild<QPushButton*>("JoinPasswordToggle")->setChecked(false); });
    connect(joinPassword, &QPushButton::clicked, this, [this] { Launch(false); });
    viewerLimit_ = new QSpinBox; viewerLimit_->setObjectName("createViewerLimit"); viewerLimit_->setRange(1,63); viewerLimit_->setValue(4);
    detailsBody_->addWidget(disclosure("Advanced settings", optionField("Viewer limit", viewerLimit_), details)); detailsBody_->addStretch();
    auto* stream = new QWidget; stream->setObjectName("FormCard"); auto* streamBody = new QVBoxLayout(stream); createColumns_->addWidget(stream, 1);
    streamBody->setContentsMargins(20,20,20,20); streamBody->setSpacing(14);
    auto* sourceHeading = new QHBoxLayout;
    auto* streamHeading = new QLabel("Share source"); streamHeading->setObjectName("SectionHeading"); sourceHeading->addWidget(streamHeading, 1);
    source_ = new QComboBox; source_->setObjectName("captureSource"); source_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    source_->setParent(this); source_->hide();
    auto* refreshSources = new QPushButton; refreshSources->setIcon(entryIcon("refresh")); refreshSources->setToolTip("Refresh sources"); refreshSources->setAccessibleName("Refresh sources"); refreshSources->setFixedSize(36,36); refreshSources->setObjectName("refreshCaptureSources"); sourceHeading->addWidget(refreshSources);
    streamBody->addLayout(sourceHeading);
    auto* sourceKinds = segments({"Display", "Window"}, 0, stream, [this](int index) { windowSources_ = index == 1; RefreshSourceCards(true); });
    sourceKinds->setObjectName("SourceKinds"); streamBody->addWidget(sourceKinds);
    sourceCards_ = new QListWidget; sourceCards_->setObjectName("SourceCards");
    sourceCards_->setViewMode(QListView::IconMode); sourceCards_->setResizeMode(QListView::Adjust); sourceCards_->setMovement(QListView::Static);
    sourceCards_->setIconSize(QSize(144,80)); sourceCards_->setGridSize(QSize(164,120)); sourceCards_->setSpacing(4);
    sourceCards_->setMinimumHeight(140); sourceCards_->setMaximumHeight(160); sourceCards_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    streamBody->addWidget(sourceCards_);
    connect(sourceCards_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (item && item->data(Qt::UserRole).isValid()) { source_->setCurrentIndex(item->data(Qt::UserRole).toInt()); error_->clear(); }
    });
    connect(refreshSources, &QPushButton::clicked, this, [this] { RefreshSources(); });
    const auto preferences = profile_.streamPreferences();
    preset_ = new QComboBox; preset_->setObjectName("createPreset"); preset_->addItems({"Gaming — responsiveness", "Quality — detail"});
    preset_->setCurrentIndex(int(preferences.preset)); preset_->setParent(this); preset_->hide();
    auto* presets = segments({"Gaming", "Quality"}, preset_->currentIndex(), stream, [this](int index) { preset_->setCurrentIndex(index); });
    presets->setObjectName("StreamPresets");
    connect(preset_, &QComboBox::currentIndexChanged, presets, [presets](int index) { if(auto* button = presets->findChild<QButtonGroup*>()->button(index)) button->setChecked(true); });
    streamBody->addWidget(optionField("Streaming preset", presets));
    resolution_ = new QComboBox; resolution_->setObjectName("createResolution");
    resolution_->addItem("Auto", "auto"); resolution_->addItem("Native source size", "native");
    for (const auto& size : {QSize(1280,720), QSize(1920,1080), QSize(2560,1440), QSize(3840,2160)})
        resolution_->addItem(QString("%1 × %2").arg(size.width()).arg(size.height()), size);
    if (preferences.resolution == media::ResolutionMode::Native) resolution_->setCurrentIndex(1);
    if (preferences.resolution == media::ResolutionMode::Fixed) {
        const QSize size(preferences.width, preferences.height);
        int index = resolution_->findData(size);
        if (index < 0) { resolution_->addItem(QString("%1 × %2").arg(size.width()).arg(size.height()), size); index = resolution_->count()-1; }
        resolution_->setCurrentIndex(index);
    }
    fps_ = new QComboBox; fps_->setObjectName("createFps"); fps_->addItem("Auto", 0);
    for(int value : {30,60,90,120,144,240}) fps_->addItem(QString("%1 FPS").arg(value), value);
    int frameIndex = fps_->findData(preferences.fpsMode == media::SettingMode::Auto ? 0 : preferences.fps);
    if(frameIndex < 0) { fps_->addItem(QString("%1 FPS").arg(preferences.fps), preferences.fps); frameIndex = fps_->count()-1; }
    fps_->setCurrentIndex(frameIndex);
    bitrate_ = new QComboBox; bitrate_->setObjectName("createBitrate"); bitrate_->addItem("Auto", 0);
    for(int value : {2,4,8,12,20,40,80}) bitrate_->addItem(QString("%1 Mbps limit").arg(value), value*1000000);
    const int rate = preferences.bitrateLimitBps.value_or(0);
    int rateIndex = bitrate_->findData(rate);
    if(rateIndex < 0) { bitrate_->addItem(QString("%1 Mbps limit").arg(rate/1000000.0), rate); rateIndex = bitrate_->count()-1; }
    bitrate_->setCurrentIndex(rateIndex);
    audio_ = new QComboBox; audio_->addItems({"System audio", "Microphone", "No shared audio"});
    auto* quality = new QGridLayout; quality->setSpacing(12);
    quality->addWidget(optionField("Resolution", resolution_),0,0); quality->addWidget(optionField("Frame rate", fps_),0,1);
    quality->addWidget(optionField("Bitrate", bitrate_),1,0); quality->addWidget(optionField("Shared audio", audio_),1,1);
    quality->setColumnStretch(0,1); quality->setColumnStretch(1,1); streamBody->addLayout(quality); streamBody->addStretch();
    body->addWidget(createPanel_);
    joinPanel_ = new QWidget; joinBody_ = new QVBoxLayout(joinPanel_); joinBody_->setContentsMargins(0,0,0,0); joinBody_->setSpacing(16);
    roomId_ = new QLineEdit; roomId_->setObjectName("joinRoomId"); roomId_->setMaxLength(512);
    roomId_->setPlaceholderText("Paste a room link or room ID"); joinBody_->addWidget(fieldLabel("Room link"));
    auto* linkRow = new QHBoxLayout; linkRow->setSpacing(10); linkRow->addWidget(roomId_, 1);
    auto* paste = new QPushButton("Paste"); paste->setObjectName("pasteRoomLink"); paste->setIcon(entryIcon("paste")); linkRow->addWidget(paste);
    connect(paste, &QPushButton::clicked, this, [this] { roomId_->setText(QApplication::clipboard()->text().trimmed().left(512)); roomId_->setFocus(); });
    auto* join = new QPushButton("Join room"); join->setObjectName("joinV2Room"); linkRow->addWidget(join); joinBody_->addLayout(linkRow);
    auto* passwordToggle = new QPushButton("Use a password"); passwordToggle->setObjectName("JoinPasswordToggle"); passwordToggle->setCheckable(true);
    joinBody_->addWidget(passwordToggle, 0, Qt::AlignLeft);
    connect(passwordToggle, &QPushButton::toggled, this, [this](bool open) {
        if (open) joinBody_->insertWidget(3, passwordPanel_);
        passwordPanel_->setVisible(open);
    });
    decoder_ = new QComboBox; decoder_->setObjectName("roomDecoder");
    decoder_->addItem("Automatic (prefer hardware)", "auto"); decoder_->addItem("Software (compatibility)", "software");
    decoder_->setCurrentIndex(decoder_->findData(profile_.decoder()));
    body->addWidget(joinPanel_);
    directoryPanel_ = new QWidget; auto* directoryBody = new QVBoxLayout(directoryPanel_); directoryBody->setContentsMargins(0,0,0,0); directoryBody->setSpacing(12);
    auto* available = new QLabel("Available rooms"); available->setObjectName("SectionHeading"); directoryBody->addWidget(available);
    status_ = new QLabel; status_->setWordWrap(true); status_->setObjectName("FormHint"); directoryBody->addWidget(status_);
    rooms_ = new QTableWidget(0, 5); rooms_->setObjectName("publicRooms"); rooms_->setHorizontalHeaderLabels({"Room", "Viewers", "Status", "Password", ""});
    rooms_->setEditTriggers(QAbstractItemView::NoEditTriggers); rooms_->setSelectionBehavior(QAbstractItemView::SelectRows);
    rooms_->setSelectionMode(QAbstractItemView::SingleSelection); rooms_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    rooms_->setShowGrid(false); rooms_->setColumnHidden(3, true); rooms_->setColumnWidth(1,80); rooms_->setColumnWidth(2,150); rooms_->setColumnWidth(4,100);
    rooms_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    rooms_->verticalHeader()->setDefaultSectionSize(58);
    rooms_->verticalHeader()->hide(); rooms_->setMinimumHeight(120); directoryBody->addWidget(rooms_, 1);
    retry_ = new QPushButton("Reconnect to room list"); retry_->setEnabled(false); directoryBody->addWidget(retry_);
    directoryBody->addWidget(disclosure("Playback options", optionField("Video decoding", decoder_), directoryPanel_));
    body->addWidget(directoryPanel_);
    body->addStretch();
    error_ = new QLabel; error_->setObjectName("browserError"); error_->setTextFormat(Qt::PlainText); error_->setWordWrap(true); layout->addWidget(error_);
    auto* actions = new QHBoxLayout; actions->addStretch();
    auto* create = new QPushButton("Create && start sharing"); create->setObjectName("createV2Room"); actions->addWidget(create);
    layout->addLayout(actions);
    connect(create, &QPushButton::clicked, this, [this] { Launch(true); });
    connect(join, &QPushButton::clicked, this, [this] { Launch(false); });
    connect(retry_, &QPushButton::clicked, this, [this] { directory_.Start(origin_); });
    directory_.changed = [this](const auto& value) {
        Refresh(value);
        if (directoryChanged) directoryChanged(value);
        if (closing_ && !closedNotified_ && !active_ && !directory_.running()) QTimer::singleShot(0, this, [this] { close(); });
    };
    for (auto* optionForm : findChildren<QFormLayout*>()) alignOptionRows(optionForm);
    for (auto* combo : findChildren<QComboBox*>()) styleComboPopup(combo);
    OpenCreate();
}
RoomBrowserWindow::~RoomBrowserWindow() = default;
void RoomBrowserWindow::resizeEvent(QResizeEvent* event) {
    createColumns_->setDirection(event->size().width() >= 780 ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    QWidget::resizeEvent(event);
}
void RoomBrowserWindow::OpenPreferences(bool playback, QWidget* owner) {
    if (closing_) return;
    if (!owner) owner = window();
    if (auto* existing = owner->findChild<QWidget*>("ProfilePreferences")) {
        existing->findChild<QTabWidget*>()->setCurrentIndex(playback ? 1 : 0); return;
    }
    auto* dialog = new QWidget(owner); dialog->setObjectName("ProfilePreferences");
    auto* stack = owner->findChild<QStackedWidget*>("AppPageStack");
    QPointer<QWidget> previous = stack ? stack->currentWidget() : nullptr;
    QPointer<QWidget> previousFocus = QApplication::focusWidget();
    const bool keptDirectory = keepDirectoryOnHide;
    if (stack) keepDirectoryOnHide = true;
    auto finish = [this, stack, dialog, previous, previousFocus, keptDirectory] {
        keepDirectoryOnHide = keptDirectory;
        if (stack) {
            if (stack->currentWidget() == dialog && previous) stack->setCurrentWidget(previous);
            stack->removeWidget(dialog);
        }
        dialog->hide(); dialog->deleteLater();
        if (previousFocus && previousFocus->isVisible()) previousFocus->setFocus();
    };
    auto* layout = new QVBoxLayout(dialog); layout->setContentsMargins(24,24,24,24); layout->setSpacing(16);
    auto* headingRow = new QHBoxLayout;
    auto* backButton = new QPushButton; backButton->setObjectName("preferencesBack");
    backButton->setIcon(QIcon(":/screenshare/ui/icons/chevron-left.svg")); backButton->setIconSize(QSize(22,22));
    backButton->setFixedSize(40,40); backButton->setToolTip("Back"); backButton->setAccessibleName("Back to previous page");
    headingRow->addWidget(backButton, 0, Qt::AlignVCenter);
    connect(backButton, &QPushButton::clicked, dialog, finish);
    auto* heading = new QLabel("Settings"); heading->setObjectName("PageHeading"); headingRow->addWidget(heading); headingRow->addStretch(); layout->addLayout(headingRow);
    auto* tabs = new QTabWidget; tabs->setObjectName("PreferencesTabs"); layout->addWidget(tabs);
    auto* profilePage = new QWidget; auto* profileForm = new QFormLayout(profilePage);
    profileForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    profileForm->setContentsMargins(24,24,24,24); profileForm->setVerticalSpacing(16);
    auto* nickname = new QLineEdit(profile_.nickname()); nickname->setObjectName("profileNickname"); nickname->setMaxLength(32);
    profileForm->addRow("Nickname", nickname);
    auto* nicknameError = new QLabel; nicknameError->setObjectName("profileError"); nicknameError->setWordWrap(true);
    profileForm->addRow(nicknameError);
    tabs->addTab(profilePage, "Profile");
    auto* playbackPage = new QWidget; auto* playbackForm = new QFormLayout(playbackPage);
    playbackForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    playbackForm->setContentsMargins(24,24,24,24); playbackForm->setVerticalSpacing(16);
    auto* playbackHeading = new QLabel("Playback defaults"); playbackHeading->setObjectName("SectionHeading"); playbackForm->addRow(playbackHeading);
    auto* decoder = new QComboBox; decoder->addItem("Automatic", "auto"); decoder->addItem("Software compatibility", "software");
    styleComboPopup(decoder);
    decoder->setCurrentIndex(decoder->findData(profile_.decoder())); decoder->setObjectName("profileDecoder");
    playbackForm->addRow("Video decoding", decoder);
    auto* volume = new QSpinBox; volume->setRange(0,100); volume->setSuffix(" %"); volume->setValue(profile_.playback().volume); volume->setObjectName("profileVolume");
    playbackForm->addRow("Initial volume", volume);
    auto* muted = new QCheckBox("Start muted"); muted->setChecked(profile_.playback().muted); playbackForm->addRow(muted);
    tabs->addTab(playbackPage, "Playback");
    alignOptionRows(profileForm); alignOptionRows(playbackForm);
    tabs->setCurrentIndex(playback ? 1 : 0);
    layout->addStretch();
    auto* error = new QLabel; error->setTextFormat(Qt::PlainText); error->setWordWrap(true); error->setObjectName("playbackError"); layout->addWidget(error);
    connect(nickname, &QLineEdit::textChanged, dialog, [this, nickname, nicknameError] {
        if (!RoomProfile::normalizeNickname(nickname->text())) { nicknameError->setText("Use 1–32 characters without control characters."); return; }
        if (!profile_.saveNickname(nickname->text())) { nicknameError->setText("Could not save nickname. Check your settings folder is writable."); return; }
        nicknameError->clear();
        if (profileChanged) profileChanged();
    });
    connect(decoder, &QComboBox::currentIndexChanged, dialog, [this, decoder, error] {
        if (!profile_.saveDecoder(decoder->currentData().toString())) { error->setText("Could not save video decoding preference."); return; }
        error->clear();
        decoder_->setCurrentIndex(decoder_->findData(profile_.decoder()));
    });
    auto savePlayback = [this, volume, muted, error] {
        if (!profile_.savePlayback({volume->value(), muted->isChecked()})) error->setText("Could not save playback preferences.");
        else error->clear();
    };
    connect(volume, &QSpinBox::valueChanged, dialog, savePlayback);
    connect(muted, &QCheckBox::toggled, dialog, savePlayback);
    if (stack) {
        stack->addWidget(dialog); stack->setCurrentWidget(dialog); dialog->show();
    } else dialog->show();
    if (!playback) nickname->setFocus();
}
void RoomBrowserWindow::RefreshSources() {
    const auto previous = source_->currentData();
    source_->clear();
    if (!enumerateSources_) source_->addItem("Default display", QVariantMap{{"display", 0}});
    else try {
        for (const auto& display : DesktopCapturer::EnumerateDisplays()) if (display.attachedToDesktop)
            source_->addItem(QString("Display %1 — %2").arg(display.index).arg(QString::fromStdWString(display.outputName)), QVariantMap{{"display", display.index}});
        for (const auto& window : DesktopCapturer::EnumerateWindows())
            source_->addItem(QString::fromStdWString(window.title), QVariantMap{{"window", QVariant::fromValue<qulonglong>(window.handle)}});
    } catch (...) { source_->clear(); }
    if (previous.isValid()) source_->setCurrentIndex(source_->findData(previous));
    if (!source_->count()) error_->setText("No capture sources available. Refresh to try again.");
    RefreshSourceCards();
}
void RoomBrowserWindow::RefreshSourceCards(bool selectFirst) {
    const QSignalBlocker blocked(sourceCards_);
    sourceCards_->clear();
    QListWidgetItem* selected = nullptr;
    for (int index = 0; index < source_->count(); ++index) {
        const auto data = source_->itemData(index).toMap();
        if (data.contains("window") != windowSources_) continue;
        QPixmap preview;
        if (enumerateSources_) {
            if (windowSources_) {
                if (auto* screen = QGuiApplication::primaryScreen()) preview = screen->grabWindow(WId(data["window"].toULongLong()));
            } else {
                const auto name = source_->itemText(index).section(QString::fromUtf8(" — "), 1);
                for (auto* screen : QGuiApplication::screens()) {
                    if (screen->name() == name) { preview = screen->grabWindow(0); break; }
                }
            }
        }
        if (preview.isNull()) {
            preview = QPixmap(144,80); preview.fill(QColor("#0c1110"));
            QPainter painter(&preview); painter.setPen(QColor("#a3b5af"));
            painter.drawRect(52,16,40,28); painter.drawLine(72,44,72,52); painter.drawLine(61,52,83,52);
            painter.drawText(QRect(0,56,144,20), Qt::AlignCenter, windowSources_ ? "Window" : "Display");
        }
        const auto fullName = source_->itemText(index);
        const auto shortName = fontMetrics().elidedText(fullName, Qt::ElideRight, 146);
        const auto scaled = preview.scaled(144,80,Qt::KeepAspectRatio,Qt::SmoothTransformation);
        QIcon icon; icon.addPixmap(scaled, QIcon::Normal); icon.addPixmap(scaled, QIcon::Selected);
        auto* item = new QListWidgetItem(icon, shortName, sourceCards_);
        item->setToolTip(fullName); item->setData(Qt::AccessibleTextRole, fullName); item->setData(Qt::UserRole, index);
        if (index == source_->currentIndex()) selected = item;
    }
    if (!selected && selectFirst && sourceCards_->count()) selected = sourceCards_->item(0);
    if (selected) { sourceCards_->setCurrentItem(selected); source_->setCurrentIndex(selected->data(Qt::UserRole).toInt()); }
    else if (selectFirst) source_->setCurrentIndex(-1);
    if (!sourceCards_->count()) {
        auto* empty = new QListWidgetItem(windowSources_ ? "No available windows" : "No available displays", sourceCards_);
        empty->setFlags(Qt::NoItemFlags);
    }
    if (selected) error_->clear();
}
void RoomBrowserWindow::JoinListedRoom(const QString& id, bool passwordRequired) {
    error_->clear();
    if (roomId_->text() != id) password_->clear();
    roomId_->setText(id);
    if (passwordRequired && password_->text().isEmpty()) {
        findChild<QPushButton*>("JoinPasswordToggle")->setChecked(true);
        static_cast<QVBoxLayout*>(directoryPanel_->layout())->insertWidget(3, passwordPanel_);
        passwordPanel_->show();
        findChild<QScrollArea*>("RoomBrowserScroll")->ensureWidgetVisible(passwordPanel_);
        password_->setFocus(); return;
    }
    Launch(false);
}
void RoomBrowserWindow::ShowBackButton() {
    auto* button = new QPushButton("‹ Home", this); button->setObjectName("roomBack");
    button->setFlat(true);
    static_cast<QVBoxLayout*>(layout())->insertWidget(0, button, 0, Qt::AlignLeft);
    connect(button, &QPushButton::clicked, this, [this] { password_->clear(); if (back) back(); });
}
void RoomBrowserWindow::OpenCreate() {
    setWindowTitle("ScreenShare — Create room"); heading_->setText("Create a room");
    createPanel_->show(); joinPanel_->hide(); directoryPanel_->hide(); rooms_->hide(); retry_->hide(); status_->hide();
    detailsBody_->insertWidget(3, passwordPanel_); passwordPanel_->show();
    passwordActions_->hide(); passwordPanel_->findChild<QLabel*>("OptionLabel")->setText("Password (optional)");
    findChild<QPushButton*>("createV2Room")->show(); findChild<QPushButton*>("joinV2Room")->hide();
    RefreshSources(); password_->clear(); error_->clear(); name_->setFocus();
    QTimer::singleShot(0, this, [this] { findChild<QScrollArea*>("RoomBrowserScroll")->verticalScrollBar()->setValue(0); });
}
void RoomBrowserWindow::OpenJoin(const QString& roomId) {
    setWindowTitle("ScreenShare — Join room"); heading_->setText("Join a room");
    createPanel_->hide(); joinPanel_->show(); directoryPanel_->show(); rooms_->show(); retry_->setVisible(directory_.status().phase == RoomDirectory::Phase::Failed);
    passwordActions_->show(); passwordPanel_->findChild<QLabel*>("OptionLabel")->setText("Room password");
    joinBody_->insertWidget(3, passwordPanel_); findChild<QPushButton*>("JoinPasswordToggle")->setChecked(false); passwordPanel_->hide();
    status_->setVisible(directory_.status().phase != RoomDirectory::Phase::Ready);
    findChild<QPushButton*>("createV2Room")->hide(); findChild<QPushButton*>("joinV2Room")->show();
    password_->clear(); error_->clear(); roomId_->setText(roomId); roomId_->setFocus();
    for (const auto& room : directory_.status().rooms) {
        if (room.id == roomId && room.password) findChild<QPushButton*>("JoinPasswordToggle")->setChecked(true);
    }
    QTimer::singleShot(0, this, [this] { findChild<QScrollArea*>("RoomBrowserScroll")->verticalScrollBar()->setValue(0); });
}
void RoomBrowserWindow::Refresh(const RoomDirectory::Status& state) {
    retry_->setEnabled(state.phase == RoomDirectory::Phase::Failed);
    retry_->setVisible(!joinPanel_->isHidden() && state.phase == RoomDirectory::Phase::Failed);
    QString selected;
    if (rooms_->currentRow() >= 0 && rooms_->item(rooms_->currentRow(), 0)) selected = rooms_->item(rooms_->currentRow(), 0)->data(Qt::UserRole).toString();
    const QSignalBlocker blocked(rooms_); rooms_->clearSelection(); rooms_->setCurrentCell(-1, -1);
    rooms_->setRowCount(int(state.rooms.size()));
    for (size_t i = 0; i < state.rooms.size(); ++i) {
        const auto& room = state.rooms[i]; auto* title = new QTableWidgetItem(room.name); title->setData(Qt::UserRole, room.id);
        rooms_->setItem(int(i), 0, title); rooms_->setItem(int(i), 1, new QTableWidgetItem(QString("%1/%2").arg(room.viewers).arg(room.limit)));
        auto* stateItem = new QTableWidgetItem(room.status != "open" ? room.status : room.password ? "Password required" : "Live");
        stateItem->setData(Qt::UserRole, room.status);
        rooms_->setItem(int(i), 2, stateItem); rooms_->setItem(int(i), 3, new QTableWidgetItem(room.password ? "Required" : "No"));
        auto* cell = rooms_->cellWidget(int(i), 4);
        if (!cell || cell->property("roomId").toString() != room.id) {
            cell = new QWidget; cell->setProperty("roomId", room.id);
            auto* rowActions = new QHBoxLayout(cell); rowActions->setContentsMargins(8,10,8,10);
            auto* join = new QPushButton("Join"); join->setObjectName("JoinListedRoom"); join->setMinimumHeight(34); rowActions->addWidget(join);
            connect(join, &QPushButton::clicked, this, [this, cell, id = room.id] { JoinListedRoom(id, cell->property("passwordRequired").toBool()); });
            rooms_->setCellWidget(int(i), 4, cell);
        }
        cell->setProperty("passwordRequired", room.password);
        cell->findChild<QPushButton*>()->setEnabled(state.phase == RoomDirectory::Phase::Ready && room.status == "open");
        if (room.id == selected) rooms_->selectRow(int(i));
    }
    rooms_->setFixedHeight(rooms_->horizontalHeader()->sizeHint().height() + qBound(80, int(state.rooms.size()) * 58, 290) + 2);
    switch (state.phase) {
    case RoomDirectory::Phase::Stopped: status_->setText("Room list paused."); break;
    case RoomDirectory::Phase::Connecting: status_->setText("Connecting to room list…"); break;
    case RoomDirectory::Phase::Ready: status_->clear(); break;
    case RoomDirectory::Phase::Reconnecting: status_->setText("Reconnecting. The room list may be outdated."); break;
    case RoomDirectory::Phase::Failed: status_->setText("Room list unavailable. Reconnect to try again."); break;
    }
    status_->setVisible(!joinPanel_->isHidden() && state.phase != RoomDirectory::Phase::Ready);
}
void RoomBrowserWindow::Launch(bool host) {
    if (active_ || closing_) return;
    if (host && source_->currentIndex() < 0) { error_->setText("Choose an available capture source before sharing."); return; }
    const auto nickname = RoomProfile::normalizeNickname(profile_.nickname());
    const auto roomId = ParseRoomReference(roomId_->text().trimmed());
    if (!nickname || (!host && !roomId)) { error_->setText("Enter a valid room ID or room link."); return; }
    QJsonObject input{{"origin", origin_.toString()}, {"host", host}, {"nickname", *nickname}, {"name", name_->text()},
        {"roomId", host ? QString{} : *roomId}, {"password", password_->text()}, {"public", public_->isChecked()}, {"viewerLimit", viewerLimit_->value()},
        {"capture", QJsonObject::fromVariantMap(source_->currentData().toMap())},
        {"audio", QJsonObject{{"source", audio_->currentIndex() == 0 ? "system" : audio_->currentIndex() == 1 ? "microphone" : "none"}}}};
    // Window handles are strings at the shared configuration boundary.
    auto capture = input["capture"].toObject();
    if (capture.contains("window")) capture["window"] = QString::number(source_->currentData().toMap()["window"].toULongLong());
    input["capture"] = capture;
    if (!host) input["decoder"] = decoder_->currentData().toString();
    try {
        auto config = ParseRoomSessionConfig(input, loopback_);
        config.media.preferences = profile_.streamPreferences();
        if (host) {
            auto& p = config.media.preferences;
            p.preset = media::StreamPreset(preset_->currentIndex());
            p.resolution = resolution_->currentIndex() == 0 ? media::ResolutionMode::Auto :
                resolution_->currentIndex() == 1 ? media::ResolutionMode::Native : media::ResolutionMode::Fixed;
            if (p.resolution == media::ResolutionMode::Fixed) { const auto size = resolution_->currentData().toSize(); p.width = size.width(); p.height = size.height(); }
            p.fpsMode = fps_->currentData().toInt() ? media::SettingMode::Manual : media::SettingMode::Auto;
            if (p.fpsMode == media::SettingMode::Manual) p.fps = fps_->currentData().toInt();
            const int rate = bitrate_->currentData().toInt();
            p.bitrateMode = rate ? media::SettingMode::Manual : media::SettingMode::Auto;
            p.bitrateLimitBps = rate ? std::optional<int>(rate) : std::nullopt;
            media::ValidateStreamPreferences(p);
        }
        const auto playback = profile_.playback();
        config.media.playbackVolume = playback.volume; config.media.playbackMuted = playback.muted;
        if (!host && !profile_.saveDecoder(decoder_->currentData().toString())) { error_->setText("Could not save the decoding preference."); return; }
        error_->clear();
        active_ = std::make_unique<RoomSessionWindow>(std::move(config), factory_, loopback_, &profile_);
        active_->closed = [this] { QTimer::singleShot(0, this, [this] {
            active_.reset();
            if (closing_) close();
            else if (returnFromSession) returnFromSession();
            else if (presentPage) presentPage(this);
            else show();
        }); };
        password_->clear();
        if (presentPage) presentPage(active_.get());
        else { active_->show(); hide(); }
    } catch (...) { error_->setText("Invalid room or capture settings."); }
}
void RoomBrowserWindow::showEvent(QShowEvent* event) { QWidget::showEvent(event); if (!closing_) directory_.Start(origin_); }
void RoomBrowserWindow::hideEvent(QHideEvent* event) { if (!keepDirectoryOnHide) directory_.Stop(); QWidget::hideEvent(event); }
void RoomBrowserWindow::closeEvent(QCloseEvent* event) {
    closing_ = true;
    if (active_ || directory_.running()) {
        if (active_) active_->close(); directory_.Stop(); event->ignore(); return;
    }
    event->accept(); if (!closedNotified_) { closedNotified_ = true; if (closed) closed(); }
}
int RunRoomBrowserWindow(const QUrl& origin, bool normalHome, std::function<void(AppShellWindow&)> initializeShell) {
    try { ParseRoomSessionConfig(QJsonObject{{"origin", origin.toString()}, {"host", true}}); }
    catch (...) { QMessageBox::critical(nullptr, "Invalid service", "Use an HTTPS service origin without credentials or a path."); return 1; }
    webrtc::WinsockInitializer winsock;
    if (winsock.error() || !webrtc::InitializeSSL()) return 1;
    struct Ssl { ~Ssl() { webrtc::CleanupSSL(); } } ssl;
    webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
    webrtc::InitializeLogging(std::move(logging));
    const bool previous = QApplication::quitOnLastWindowClosed(); QApplication::setQuitOnLastWindowClosed(false);
    RoomApplication window(origin, screenshare::media::WindowsRoomRuntimeFactory, false, {}, true, normalHome);
    if (initializeShell) initializeShell(window.window());
    window.closed = [] { QApplication::quit(); }; window.show();
    const int result = QApplication::exec(); QApplication::setQuitOnLastWindowClosed(previous); return result;
}
