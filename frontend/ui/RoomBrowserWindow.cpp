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
#include "ui/HomeWindow.h"
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
#include <QAction>
#include <QThread>
#include <QStyledItemDelegate>
#include <QFrame>
#include <QHideEvent>
#include <QKeyEvent>
using namespace screenshare;
using namespace screenshare::room::qt;

namespace {
class ContentButton final : public QPushButton {
public:
    using QPushButton::QPushButton;
    QSize sizeHint() const override { return layout() ? (layout()->sizeHint()+QSize(12,8)).expandedTo(QSize(0,36)) : QPushButton::sizeHint(); }
    QSize minimumSizeHint() const override { return sizeHint(); }
};
class SourceCardDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        painter->save(); painter->setRenderHint(QPainter::Antialiasing);
        const auto card = option.rect.adjusted(8,6,-8,-6);
        const bool selected = option.state & QStyle::State_Selected;
        painter->setBrush(QColor(option.state & QStyle::State_MouseOver ? "#111d18" : "#080e0b"));
        painter->setPen(QPen(QColor(selected ? "#38d8c8" : "#30413a"),selected ? 1.5 : 1));
        painter->drawRoundedRect(card,7,7);
        const auto icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        const QRect preview(card.left()+12,card.top()+10,card.width()-24,72);
        icon.paint(painter,preview,Qt::AlignCenter,QIcon::Normal);
        const int y = card.bottom()-17;
        painter->setBrush(Qt::NoBrush); painter->setPen(QPen(QColor(selected ? "#38d8c8" : "#8b9d95"),1.5));
        painter->drawEllipse(QPoint(card.left()+18,y),5,5);
        if (selected) { painter->setBrush(QColor("#38d8c8")); painter->drawEllipse(QPoint(card.left()+18,y),2,2); }
        painter->setFont(option.font); painter->setPen(QColor("#dce7e1"));
        painter->drawText(QRect(card.left()+29,y-10,card.width()-37,20),Qt::AlignVCenter,
            option.fontMetrics.elidedText(index.data(Qt::DisplayRole).toString(),Qt::ElideRight,card.width()-37));
        painter->restore();
    }
};
class SourceList final : public QListWidget {
public:
    using QListWidget::QListWidget;
protected:
    void resizeEvent(QResizeEvent* event) override {
        QListWidget::resizeEvent(event);
        // Reserve scrollbar width even while it is hidden. Using viewport width
        // here made wrapping add/remove the scrollbar and oscillate indefinitely.
        const QSize cell(qMax(120, (width()-24)/2),128);
        if (gridSize() == cell) return;
        setGridSize(cell);
        for (int row=0; row<count(); ++row) item(row)->setSizeHint(cell);
    }
};
QIcon entryIcon(const QString& name, const QByteArray& color = "#b7c8c0") {
    QFile file(QString(":/screenshare/ui/icons/%1.svg").arg(name)); if (!file.open(QIODevice::ReadOnly)) return {};
    auto svg = file.readAll(); svg.replace("currentColor", color);
    QSvgRenderer renderer(svg); QPixmap pixels(56,56); pixels.fill(Qt::transparent);
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
void passwordEye(QLineEdit* field) {
    auto* eye = field->addAction(entryIcon("eye"), QLineEdit::TrailingPosition);
    eye->setObjectName("togglePasswordVisibility"); eye->setCheckable(true); eye->setText("Show password");
    QObject::connect(eye, &QAction::toggled, field, [field, eye](bool visible) {
        field->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
        eye->setIcon(entryIcon(visible ? "eye-off" : "eye")); eye->setText(visible ? "Hide password" : "Show password");
    });
}
QWidget* segments(const QStringList& labels, int selected, QWidget* owner, std::function<void(int)> change) {
    auto* widget = new QWidget(owner); widget->setObjectName("SegmentGroup");
    auto* layout = new QHBoxLayout(widget); layout->setContentsMargins(0,0,0,0); layout->setSpacing(6);
    auto* group = new QButtonGroup(widget); group->setExclusive(true);
    for (int i = 0; i < labels.size(); ++i) {
        auto* button = new QPushButton(labels[i]); button->setCheckable(true); button->setChecked(i == selected);
        button->setObjectName("SegmentButton"); button->setMinimumHeight(42);
        const QMap<QString, QString> icons{{"Public","globe"},{"Private","lock"},{"Display","display"},{"Window","window"},{"Gaming","gamepad"},{"Quality","quality"}};
        button->setIcon(entryIcon(icons.value(labels[i]))); button->setIconSize(QSize(20,20));
        group->addButton(button, i); layout->addWidget(button, 1);
    }
    QObject::connect(group, &QButtonGroup::idClicked, owner, std::move(change));
    return widget;
}
QWidget* disclosure(const QString& title, QWidget* content, QWidget* owner) {
    auto* block = new QWidget(owner); block->setObjectName("DisclosureCard");
    auto* layout = new QVBoxLayout(block); layout->setContentsMargins(0,0,0,0); layout->setSpacing(8);
    auto* toggle = new ContentButton; toggle->setText(title); toggle->setCheckable(true);
    toggle->setText({}); toggle->setAccessibleName(title); toggle->setObjectName("OptionsDisclosure");
    auto* row = new QHBoxLayout(toggle); row->setContentsMargins(6,6,6,6); row->setSpacing(8);
    auto* gear = new QLabel; gear->setPixmap(entryIcon("settings").pixmap(18,18));
    auto* label = new QLabel(title); auto* arrow = new QLabel; arrow->setPixmap(entryIcon("chevron-down").pixmap(14,14));
    for (auto* part : {gear,label,arrow}) { part->setAttribute(Qt::WA_TransparentForMouseEvents); row->addWidget(part); }
    label->setSizePolicy(QSizePolicy::Minimum,QSizePolicy::Preferred);
    toggle->setSizePolicy(QSizePolicy::Minimum,QSizePolicy::Fixed);
    layout->addWidget(toggle,0,Qt::AlignLeft);
    content->setObjectName("AdvancedOptionsContent"); layout->addWidget(content); content->hide();
    QObject::connect(toggle, &QPushButton::toggled, block, [arrow,content](bool open) {
        arrow->setPixmap(entryIcon(open ? "chevron-up" : "chevron-down").pixmap(14,14));
        content->setVisible(open);
    });
    return block;
}
}

RoomBrowserWindow::RoomBrowserWindow(QUrl origin, QtRoomSession::Factory factory, bool loopback, QString profileFile, bool enumerateSources)
    : origin_(std::move(origin)), factory_(std::move(factory)), loopback_(loopback), enumerateSources_(enumerateSources), profile_(profileFile), directory_(loopback) {
    setWindowTitle("ScreenShare — Rooms"); setStyleSheet(uiStyleSheet()); resize(900, 720);
    setObjectName("RoomBrowser");
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(24, 12, 24, 16); layout->setSpacing(12);
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
    passwordEye(password_);
    passwordPanel_ = optionField("Password (optional)", password_); passwordPanel_->setObjectName("RoomPasswordPanel"); detailsBody_->addWidget(passwordPanel_);
    viewerLimit_ = new QSpinBox; viewerLimit_->setObjectName("createViewerLimit"); viewerLimit_->setRange(1,63); viewerLimit_->setValue(4);
    detailsBody_->addStretch();
    auto* stream = new QWidget; stream->setObjectName("FormCard"); createColumns_->addWidget(stream, 1);
    auto* streamFrame = new QVBoxLayout(stream); streamFrame->setContentsMargins(1,1,1,1);
    auto* streamScroll = new QScrollArea; streamScroll->setObjectName("StreamSectionScroll"); streamScroll->setWidgetResizable(true);
    // Keep the content width stable as advanced options add/remove overflow.
    streamScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    streamScroll->setFrameShape(QFrame::NoFrame); streamScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* streamContent = new QWidget; auto* streamBody = new QVBoxLayout(streamContent);
    streamScroll->setWidget(streamContent); streamFrame->addWidget(streamScroll);
    stream->setMinimumHeight(330);
    streamBody->setContentsMargins(20,16,20,16); streamBody->setSpacing(10);
    auto* sourceHeading = new QHBoxLayout;
    auto* streamHeading = new QLabel("Share source"); streamHeading->setObjectName("SectionHeading"); sourceHeading->addWidget(streamHeading, 1);
    source_ = new QComboBox; source_->setObjectName("captureSource"); source_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    source_->setParent(this); source_->hide();
    auto* refreshSources = new QPushButton; refreshSources->setIcon(entryIcon("refresh")); refreshSources->setToolTip("Refresh sources"); refreshSources->setAccessibleName("Refresh sources"); refreshSources->setFixedSize(36,36); refreshSources->setObjectName("refreshCaptureSources"); sourceHeading->addWidget(refreshSources);
    streamBody->addLayout(sourceHeading);
    auto* sourceKinds = segments({"Display", "Window"}, 0, stream, [this](int index) { windowSources_ = index == 1; RefreshSourceCards(true); });
    sourceKinds->setObjectName("SourceKinds"); streamBody->addWidget(sourceKinds);
    sourceCards_ = new SourceList; sourceCards_->setObjectName("SourceCards");
    sourceCards_->setItemDelegate(new SourceCardDelegate(sourceCards_)); sourceCards_->setMouseTracking(true);
    sourceCards_->setViewMode(QListView::IconMode); sourceCards_->setResizeMode(QListView::Adjust); sourceCards_->setMovement(QListView::Static);
    sourceCards_->setUniformItemSizes(true);
    sourceCards_->setIconSize(QSize(144,80)); sourceCards_->setGridSize(QSize(184,128)); sourceCards_->setSpacing(0);
    sourceCards_->setFixedHeight(140); sourceCards_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
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
    const QStringList descriptions{"Lower latency, smoother gameplay", "Higher quality, best for work"};
    for (int index = 0; index < 2; ++index) {
        auto* button = qobject_cast<QPushButton*>(presets->findChild<QButtonGroup*>()->button(index));
        const auto title = button->text(); button->setText({}); button->setIcon({});
        button->setObjectName("PresetCard"); button->setAccessibleName(title); button->setAccessibleDescription(descriptions[index]);
        button->setMinimumHeight(60); button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        auto* row = new QHBoxLayout(button); row->setContentsMargins(12,8,12,8); row->setSpacing(12);
        auto* icon = new QLabel; icon->setFixedSize(28,28); row->addWidget(icon);
        auto* text = new QVBoxLayout; text->setSpacing(3); auto* name = new QLabel(title); name->setObjectName("PresetTitle");
        auto* description = new QLabel(descriptions[index]); description->setObjectName("PresetDescription"); description->setWordWrap(true);
        text->addWidget(name); text->addWidget(description); row->addLayout(text,1);
        for (auto* label : {icon,name,description}) label->setAttribute(Qt::WA_TransparentForMouseEvents);
        const auto iconName = index == 0 ? "preset-gaming" : "preset-quality";
        auto refresh = [button,icon,name,description,iconName] {
            const bool selected = button->isChecked();
            icon->setPixmap(entryIcon(iconName, selected ? "#38d8c8" : "#b7c8c0").pixmap(28,28));
            name->setStyleSheet(selected ? "color: #68dfd1; background: transparent; border: 0;" : "color: #edf5f2; background: transparent; border: 0;");
            description->setStyleSheet(selected ? "color: #84c2b9; background: transparent; border: 0;" : "color: #a3b5af; background: transparent; border: 0;");
        };
        connect(button,&QPushButton::toggled,button,refresh); refresh();
    }
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
    quality->setColumnStretch(0,1); quality->setColumnStretch(1,1); streamBody->addLayout(quality);
    streamBody->addWidget(disclosure("Advanced settings", optionField("Viewer limit", viewerLimit_), stream)); streamBody->addStretch();
    body->addWidget(createPanel_,1);
    joinPanel_ = new QWidget; joinBody_ = new QVBoxLayout(joinPanel_); joinBody_->setContentsMargins(0,0,0,0); joinBody_->setSpacing(16);
    roomId_ = new QLineEdit; roomId_->setObjectName("joinRoomId"); roomId_->setMaxLength(512);
    roomId_->setPlaceholderText("Paste a room link or room ID"); joinBody_->addWidget(fieldLabel("Room link"));
    roomId_->addAction(entryIcon("link"), QLineEdit::LeadingPosition);
    auto* linkRow = new QHBoxLayout; linkRow->setSpacing(10); linkRow->addWidget(roomId_, 1);
    auto* paste = new QPushButton("Paste"); paste->setObjectName("pasteRoomLink"); paste->setIcon(entryIcon("paste")); linkRow->addWidget(paste);
    connect(paste, &QPushButton::clicked, this, [this] { roomId_->setText(QApplication::clipboard()->text().trimmed().left(512)); roomId_->setFocus(); });
    auto* join = new QPushButton("Join room"); join->setObjectName("joinV2Room"); linkRow->addWidget(join); joinBody_->addLayout(linkRow);
    for (auto* control : {static_cast<QWidget*>(roomId_), static_cast<QWidget*>(paste), static_cast<QWidget*>(join)}) control->setFixedHeight(42);
    body->addWidget(joinPanel_);
    rooms_ = new RoomDirectoryWidget([this] { directory_.Stop(); directory_.Start(origin_); },
        [this](const QString& id) { JoinListedRoom(id); }, this);
    rooms_->setObjectName("publicRooms"); rooms_->setMinimumHeight(260);
    directoryPanel_ = rooms_; body->addWidget(directoryPanel_,1);
    body->addStretch();
    error_ = new QLabel; error_->setObjectName("browserError"); error_->setTextFormat(Qt::PlainText); error_->setWordWrap(true); layout->addWidget(error_);
    auto* actions = new QHBoxLayout; actions->addStretch();
    auto* create = new QPushButton("Create && start sharing"); create->setObjectName("createV2Room"); actions->addWidget(create);
    layout->addLayout(actions);
    connect(create, &QPushButton::clicked, this, [this] { Launch(true); });
    connect(join, &QPushButton::clicked, this, [this] { Launch(false); });
    directory_.changed = [this](const auto& value) {
        Refresh(value);
        if (directoryChanged) directoryChanged(value);
        if (closing_ && !closedNotified_ && !active_ && !directory_.running()) QTimer::singleShot(0, this, [this] { close(); });
    };
    for (auto* optionForm : findChildren<QFormLayout*>()) alignOptionRows(optionForm);
    for (auto* combo : findChildren<QComboBox*>()) styleComboPopup(combo);
    OpenCreate();
}
RoomBrowserWindow::~RoomBrowserWindow() {
    if (previewThread_) { previewThread_->requestInterruption(); previewThread_->wait(); delete previewThread_; }
}
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
        for (const auto& display : DesktopCapturer::EnumerateDisplays()) if (display.attachedToDesktop) {
            source_->addItem(QString("Display %1 · %2 × %3").arg(display.index + 1).arg(display.right-display.left).arg(display.bottom-display.top), QVariantMap{{"display", display.index}});
            source_->setItemData(source_->count()-1, QString::fromStdWString(display.outputName), Qt::UserRole+1);
        }
        for (const auto& window : DesktopCapturer::EnumerateWindows())
            source_->addItem(QString::fromStdWString(window.title), QVariantMap{{"window", QVariant::fromValue<qulonglong>(window.handle)}});
    } catch (...) { source_->clear(); }
    if (previous.isValid()) source_->setCurrentIndex(source_->findData(previous));
    if (!source_->count()) error_->setText("No capture sources available. Refresh to try again.");
    RefreshSourceCards();
}
void RoomBrowserWindow::RefreshSourceCards(bool selectFirst) {
    ++previewRevision_;
    if (previewThread_) previewThread_->requestInterruption();
    const QSignalBlocker blocked(sourceCards_);
    sourceCards_->clear();
    QListWidgetItem* selected = nullptr;
    for (int index = 0; index < source_->count(); ++index) {
        const auto data = source_->itemData(index).toMap();
        if (data.contains("window") != windowSources_) continue;
        QPixmap preview;
        if (enumerateSources_) {
            if (!windowSources_) {
                const auto name = source_->itemData(index, Qt::UserRole+1).toString();
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
        QPixmap scaled(144,80); scaled.fill(QColor("#0c1110"));
        { QPainter painter(&scaled); const auto fitted = preview.scaled(144,80,Qt::KeepAspectRatio,Qt::SmoothTransformation);
          painter.drawPixmap((144-fitted.width())/2, (80-fitted.height())/2, fitted); }
        QIcon icon; icon.addPixmap(scaled, QIcon::Normal); icon.addPixmap(scaled, QIcon::Selected);
        auto* item = new QListWidgetItem(icon, shortName, sourceCards_);
        item->setSizeHint(sourceCards_->gridSize()); item->setTextAlignment(Qt::AlignHCenter);
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
    LoadSourcePreviews();
}
void RoomBrowserWindow::LoadSourcePreviews() {
    if (!enumerateSources_ || closing_ || !isVisible() || createPanel_->isHidden()) return;
    if (previewThread_) return; // A repeated show event must not cancel the current capture pass.
    QVector<QVariantMap> sources;
    for (int row = 0; row < sourceCards_->count(); ++row) {
        const auto index = sourceCards_->item(row)->data(Qt::UserRole);
        if (index.isValid()) sources.push_back(source_->itemData(index.toInt()).toMap());
    }
    if (sources.isEmpty()) return;
    sourceCards_->setProperty("previewCaptureCount", 0);
    sourceCards_->setProperty("previewCaptureFinished", false);
    const auto revision = previewRevision_;
    auto previews = std::make_shared<QVector<QImage>>();
    auto failure = std::make_shared<QString>();
    // A single capture owner keeps GPU work off the UI thread. Never fall back
    // to another window/display when the requested source cannot be captured.
    previewThread_ = QThread::create([sources, previews, failure] {
        for (const auto& source : sources) {
            if (QThread::currentThread()->isInterruptionRequested()) break;
            QImage image;
            try {
                // Read a native-size still and resize it on the CPU. The live
                // video scaling path is unnecessary for a one-shot thumbnail.
                CaptureConfig config; config.targetFps = 15;
                config.sourceType = source.contains("window") ? CaptureSourceType::Window : CaptureSourceType::Display;
                config.backend = CaptureBackend::WindowsGraphicsCapture;
                config.windowHandle = source.value("window").toULongLong(); config.displayIndex = source.value("display").toInt();
                config.allowDisplayFallback = false;
                DesktopCapturer capturer; capturer.Start(config);
                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
                while (!QThread::currentThread()->isInterruptionRequested() && std::chrono::steady_clock::now() < until) {
                    auto frame = capturer.TryCaptureFrame(std::chrono::milliseconds(30));
                    if (frame && frame->width > 0 && frame->height > 0 && !frame->pixels.empty()) {
                        image = QImage(reinterpret_cast<const uchar*>(frame->pixels.data()), frame->width, frame->height,
                                       frame->rowPitch, QImage::Format_RGB32).copy(); break;
                    }
                }
            } catch (const std::exception& error) { *failure = QString::fromUtf8(error.what()); }
            catch (...) { *failure = "Source unavailable"; }
            previews->push_back(std::move(image));
        }
    });
    connect(previewThread_, &QThread::finished, this, [this, revision, previews, failure] {
        auto* completed = previewThread_; previewThread_ = nullptr; completed->deleteLater();
        if (revision != previewRevision_) { LoadSourcePreviews(); return; }
        int captured = 0;
        for (int row = 0; row < previews->size() && row < sourceCards_->count(); ++row) {
            if ((*previews)[row].isNull()) continue;
            QPixmap thumbnail(144,80); thumbnail.fill(QColor("#0c1110"));
            QPainter painter(&thumbnail); const auto scaled = (*previews)[row].scaled(144,80,Qt::KeepAspectRatio,Qt::SmoothTransformation);
            painter.drawImage((144-scaled.width())/2,(80-scaled.height())/2,scaled); painter.end();
            QIcon icon; icon.addPixmap(thumbnail,QIcon::Normal); icon.addPixmap(thumbnail,QIcon::Selected);
            sourceCards_->item(row)->setIcon(icon);
            ++captured;
        }
        sourceCards_->setProperty("previewCaptureCount", captured);
        sourceCards_->setProperty("previewCaptureFinished", true);
        sourceCards_->setProperty("previewCaptureError", *failure);
    });
    previewThread_->start();
}
void RoomBrowserWindow::JoinListedRoom(const QString& id) {
    error_->clear();
    if (roomId_->text() != id) password_->clear();
    roomId_->setText(id);
    Launch(false);
}
void RoomBrowserWindow::PromptPassword() {
    auto* dialog = new QDialog(this); dialog->setObjectName("RoomPasswordDialog");
    dialog->setWindowTitle("Room password"); dialog->setWindowModality(Qt::WindowModal); dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setMinimumWidth(360); dialog->setStyleSheet(uiStyleSheet());
    auto* body = new QVBoxLayout(dialog); body->setContentsMargins(24,24,24,24); body->setSpacing(16);
    auto* title = new QLabel(passwordRejected_ ? "Incorrect password. Try again." : "Enter room password"); title->setObjectName("SectionHeading"); body->addWidget(title);
    auto* field = new QLineEdit; field->setObjectName("joinPassword"); field->setEchoMode(QLineEdit::Password); field->setMaxLength(128);
    field->setPlaceholderText("Password"); passwordEye(field); body->addWidget(field);
    auto* actions = new QHBoxLayout; actions->addStretch();
    auto* cancel = new QPushButton("Cancel"); cancel->setObjectName("cancelRoomPassword");
    auto* join = new QPushButton("Join room"); join->setObjectName("joinWithPassword"); join->setDefault(true);
    for (auto* button : {cancel, join}) { button->setFixedSize(110,42); actions->addWidget(button); }
    body->addLayout(actions);
    connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
    connect(join, &QPushButton::clicked, dialog, [this, dialog, field] {
        if (field->text().isEmpty()) { field->setFocus(); return; }
        password_->setText(field->text()); field->clear(); dialog->accept(); Launch(false);
    });
    dialog->open(); field->setFocus();
}
void RoomBrowserWindow::ShowBackButton() {
    auto* button = new QPushButton("Home", this); button->setObjectName("roomBack");
    button->setIcon(entryIcon("back")); button->setIconSize(QSize(24,24)); button->setMinimumHeight(42); button->setFlat(true);
    auto* body = static_cast<QVBoxLayout*>(layout()); body->removeWidget(heading_);
    auto* header = new QHBoxLayout; header->setSpacing(20); header->addWidget(button); header->addWidget(heading_); header->addStretch();
    body->insertLayout(0,header);
    connect(button, &QPushButton::clicked, this, [this] { password_->clear(); if (back) back(); });
}
void RoomBrowserWindow::OpenCreate() {
    setWindowTitle("ScreenShare — Create room"); heading_->setText("Create a room");
    createPanel_->show(); joinPanel_->hide(); directoryPanel_->hide();
    detailsBody_->insertWidget(3, passwordPanel_); passwordPanel_->show();
    password_->findChild<QAction*>("togglePasswordVisibility")->setChecked(false);
    findChild<QPushButton*>("createV2Room")->show(); findChild<QPushButton*>("joinV2Room")->hide();
    RefreshSources(); password_->clear(); error_->clear(); name_->setFocus();
    QTimer::singleShot(0, this, [this] { findChild<QScrollArea*>("RoomBrowserScroll")->verticalScrollBar()->setValue(0); });
}
void RoomBrowserWindow::OpenJoin(const QString& roomId) {
    setWindowTitle("ScreenShare — Join room"); heading_->setText("Join a room");
    createPanel_->hide(); joinPanel_->show(); directoryPanel_->show();
    findChild<QPushButton*>("createV2Room")->hide(); findChild<QPushButton*>("joinV2Room")->show();
    password_->clear(); error_->clear(); roomId_->setText(roomId); roomId_->setFocus();
    QTimer::singleShot(0, this, [this] { findChild<QScrollArea*>("RoomBrowserScroll")->verticalScrollBar()->setValue(0); });
}
void RoomBrowserWindow::Refresh(const RoomDirectory::Status& state) {
    QVector<HomeActiveRoom> rooms;
    for (const auto& room : state.rooms)
        rooms.push_back({room.id,room.name,room.viewers,room.password,0,
            state.phase == RoomDirectory::Phase::Ready && room.status == "open"});
    QString message;
    switch (state.phase) {
    case RoomDirectory::Phase::Stopped: message = "Room list paused."; break;
    case RoomDirectory::Phase::Connecting: message = "Connecting to room list…"; break;
    case RoomDirectory::Phase::Ready: break;
    case RoomDirectory::Phase::Reconnecting: message = "Reconnecting. The room list may be outdated."; break;
    case RoomDirectory::Phase::Failed: message = "Room list unavailable. Refresh to try again."; break;
    }
    rooms_->setPushedRooms(rooms,message);
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
    if (!host) input["decoder"] = profile_.decoder();
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
        error_->clear();
        active_ = std::make_unique<RoomSessionWindow>(std::move(config), factory_, loopback_, &profile_);
        if (!host) {
            auto update = active_->session().statusChanged;
            active_->session().statusChanged = [this, update, suppliedPassword = !password_->text().isEmpty()](const auto& status) {
                if (update) update(status);
                if (!passwordRetry_ && status.phase == v2::RoomPhase::Failed && status.error == v2::RoomError::AdmissionDenied) {
                    passwordRetry_ = true;
                    passwordRejected_ = suppliedPassword;
                    QTimer::singleShot(0, this, [this] { if (active_) active_->close(); });
                }
            };
        }
        active_->closed = [this] { QTimer::singleShot(0, this, [this] {
            active_.reset();
            if (passwordRetry_ && !closing_) {
                passwordRetry_ = false;
                if (presentPage) presentPage(this); else show();
                PromptPassword(); return;
            }
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
void RoomBrowserWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!closing_) { directory_.Start(origin_); if (!createPanel_->isHidden()) LoadSourcePreviews(); }
}
void RoomBrowserWindow::hideEvent(QHideEvent* event) {
    ++previewRevision_; // A cancelled hidden-page pass must never satisfy a later Create visit.
    if (previewThread_) previewThread_->requestInterruption();
    if (!keepDirectoryOnHide) directory_.Stop(); QWidget::hideEvent(event);
}
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
