#include "ui/CreateRoomWindow.h"

#include "ui/UiStyle.h"

#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QIODevice>
#include <QtCore/QModelIndex>
#include <QtCore/QPointF>
#include <QtCore/QPointer>
#include <QtCore/QRect>
#include <QtCore/QRectF>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>
#include <QtCore/QSizeF>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtCore/QVector>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtGui/QScreen>
#include <QtSvg/QSvgRenderer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyledItemDelegate>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStyleOptionViewItem>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace {

constexpr const char* kRoomLinkPrefix = "screenshare-room-v1;";
constexpr const char* kDefaultStunServer = "stun.l.google.com:19302";
constexpr int kComboPopupItemHeight = 30;
constexpr int kSourceRefreshIntervalMs = 2000;

class ComboItemDelegate final : public QStyledItemDelegate {
public:
    explicit ComboItemDelegate(QComboBox* combo, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), combo_(combo)
    {
        setObjectName("RoomComboDelegate");
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(kComboPopupItemHeight);
        return size;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        if (painter == nullptr) {
            return;
        }

        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const bool hovered = (option.state & QStyle::State_MouseOver) != 0;
        const bool current = combo_ != nullptr && index.row() == combo_->currentIndex();
        const QColor background = selected ? QColor("#168f82") : (hovered ? QColor("#28302f") : QColor("#202625"));

        painter->save();
        painter->fillRect(option.rect, background);
        painter->setPen(QColor("#eef3f1"));
        painter->drawText(option.rect.adjusted(10, 0, current ? -34 : -10, 0),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          index.data(Qt::DisplayRole).toString());

        if (current) {
            const QRect checkRect(option.rect.right() - 26, option.rect.center().y() - 7, 16, 16);
            QPen pen(QColor("#dffbf8"), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            painter->setPen(pen);
            painter->drawLine(checkRect.left() + 3, checkRect.center().y(),
                              checkRect.left() + 7, checkRect.bottom() - 4);
            painter->drawLine(checkRect.left() + 7, checkRect.bottom() - 4,
                              checkRect.right() - 2, checkRect.top() + 3);
        }
        painter->restore();
    }

private:
    QPointer<QComboBox> combo_;
};

QPixmap renderSvgResource(const QString& path, const QSize& size, const QString& color = QString())
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QByteArray svg = file.readAll();
    if (!color.isEmpty()) {
        svg.replace("currentColor", color.toUtf8());
    }

    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) {
        return {};
    }

    QPixmap pixmap(size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(size)));
    return pixmap;
}

QString generatedRoomId()
{
    return QStringLiteral("room-%1").arg(QUuid::createUuid().toString(QUuid::Id128).left(10).toLower());
}

QString defaultRoomName()
{
    const QByteArray computerName = qgetenv("COMPUTERNAME");
    if (!computerName.isEmpty()) {
        return QString::fromLocal8Bit(computerName) + "'s room";
    }
    const QByteArray userName = qgetenv("USERNAME");
    if (!userName.isEmpty()) {
        return QString::fromLocal8Bit(userName) + "'s room";
    }
    return QStringLiteral("ScreenShare room");
}

QString generatedPassword()
{
    return QUuid::createUuid().toString(QUuid::Id128).left(8).toUpper();
}

bool validRoomId(const QString& roomId)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{3,96}$"));
    return pattern.match(roomId).hasMatch();
}

bool validOptionalRoomText(const QString& text, int maxLength)
{
    if (text.size() > maxLength) {
        return false;
    }
    for (const QChar ch : text) {
        if (ch.unicode() < 0x20 || ch.unicode() == 0x7f) {
            return false;
        }
    }
    return true;
}

bool validResolutionSize(const QSize& size)
{
    return size.width() > 0 && size.height() > 0;
}

QSize evenResolutionSize(QSize size)
{
    if (!validResolutionSize(size)) {
        return {};
    }
    size.setWidth(size.width() - (size.width() % 2));
    size.setHeight(size.height() - (size.height() % 2));
    return validResolutionSize(size) ? size : QSize();
}

QString resolutionChoiceValue(const QSize& size)
{
    return QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
}

QString resolutionChoiceText(const QSize& size)
{
    return QStringLiteral("%1 x %2").arg(size.width()).arg(size.height());
}

QString displaySourceValue(int displayIndex)
{
    return QStringLiteral("display:%1").arg(displayIndex);
}

QString windowSourceValue(uint64_t handle)
{
    return QStringLiteral("window:%1").arg(handle);
}

bool sourceValueIsWindow(const QString& value)
{
    return value.startsWith(QStringLiteral("window:"));
}

int displayIndexFromSourceValue(const QString& value)
{
    if (!value.startsWith(QStringLiteral("display:"))) {
        return 0;
    }
    bool ok = false;
    const int displayIndex = value.mid(8).toInt(&ok);
    return ok && displayIndex >= 0 ? displayIndex : 0;
}

uint64_t windowHandleFromSourceValue(const QString& value)
{
    if (!sourceValueIsWindow(value)) {
        return 0;
    }
    bool ok = false;
    const qulonglong handle = value.mid(7).toULongLong(&ok);
    return ok ? static_cast<uint64_t>(handle) : 0;
}

QVector<QSize> resolutionChoicesForDisplay(QSize displaySize)
{
    displaySize = evenResolutionSize(displaySize);
    const bool hasDisplaySize = validResolutionSize(displaySize);
    const double displayAspect = hasDisplaySize ?
        static_cast<double>(displaySize.width()) / static_cast<double>(displaySize.height()) :
        16.0 / 9.0;

    QVector<QSize> choices;
    auto addChoice = [&](QSize size) {
        size = evenResolutionSize(size);
        if (!validResolutionSize(size)) {
            return;
        }
        if (hasDisplaySize && (size.width() > displaySize.width() || size.height() > displaySize.height())) {
            return;
        }
        const auto duplicate = std::find_if(choices.begin(), choices.end(), [&](const QSize& existing) {
            return existing.width() == size.width() && existing.height() == size.height();
        });
        if (duplicate == choices.end()) {
            choices.push_back(size);
        }
    };

    addChoice(displaySize);
    const QVector<QSize> common16x9 = {
        QSize(3840, 2160),
        QSize(2560, 1440),
        QSize(1920, 1080),
        QSize(1600, 900),
        QSize(1280, 720),
    };
    for (const QSize& size : common16x9) {
        const double aspect = static_cast<double>(size.width()) / static_cast<double>(size.height());
        if (!hasDisplaySize || std::abs(aspect - displayAspect) < 0.02) {
            addChoice(size);
        }
    }

    if (hasDisplaySize && choices.size() < 3) {
        for (const double scale : {0.75, 0.625, 0.5}) {
            addChoice(QSize(
                static_cast<int>(std::round(static_cast<double>(displaySize.width()) * scale)),
                static_cast<int>(std::round(static_cast<double>(displaySize.height()) * scale))));
        }
    }

    std::sort(choices.begin(), choices.end(), [](const QSize& lhs, const QSize& rhs) {
        const qint64 lhsArea = static_cast<qint64>(lhs.width()) * static_cast<qint64>(lhs.height());
        const qint64 rhsArea = static_cast<qint64>(rhs.width()) * static_cast<qint64>(rhs.height());
        if (lhsArea != rhsArea) {
            return lhsArea > rhsArea;
        }
        return lhs.width() > rhs.width();
    });
    return choices;
}

std::string toStdUtf8(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<size_t>(bytes.size()));
}

void tuneComboPopup(QComboBox* combo)
{
    if (combo == nullptr) {
        return;
    }
    combo->setMaxVisibleItems(12);
    const int rows = std::clamp(combo->count(), 1, 12);
    QAbstractItemView* view = combo->view();
    if (view != nullptr) {
        if (view->itemDelegate() == nullptr ||
            view->itemDelegate()->objectName() != QStringLiteral("RoomComboDelegate")) {
            view->setItemDelegate(new ComboItemDelegate(combo, view));
        }
        const int popupHeight = (rows * kComboPopupItemHeight) + 2;
        view->setMinimumHeight(popupHeight);
        view->setMaximumHeight(popupHeight);
        view->setFrameShape(QFrame::NoFrame);
        view->setLineWidth(0);
        view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        view->setVerticalScrollBarPolicy(combo->count() > rows ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
        QScrollBar* scrollBar = view->verticalScrollBar();
        if (scrollBar != nullptr) {
            scrollBar->setSingleStep(kComboPopupItemHeight);
            scrollBar->setPageStep(rows * kComboPopupItemHeight);
            scrollBar->setStyleSheet(QStringLiteral(R"(
QScrollBar:vertical {
    background: #202625;
    border: 0;
    width: 10px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: #3c4946;
    border: 0;
    border-radius: 0;
    min-height: 24px;
}
QScrollBar::handle:vertical:hover {
    background: #4d5c58;
}
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical {
    background: transparent;
    border: 0;
    height: 0;
    width: 0;
}
QScrollBar::up-arrow:vertical,
QScrollBar::down-arrow:vertical {
    background: transparent;
    border: 0;
    height: 0;
    width: 0;
}
QScrollBar::add-page:vertical,
QScrollBar::sub-page:vertical {
    background: transparent;
}
)"));
        }
    }
}

} // namespace

CreateRoomWindow::CreateRoomWindow(QtSessionBackend* backend, Actions actions, QWidget* parent)
    : QWidget(parent), actions_(std::move(actions)), backend_(backend), roomId_(generatedRoomId())
{
    setObjectName("RoomWindow");
    setWindowTitle("Create Room - ScreenShare");
    setWindowIcon(QIcon(QStringLiteral(":/screenshare/brand/screenshare-mark.svg")));
    setStyleSheet(uiStyleSheet());
    resize(760, 630);
    setMinimumSize(720, 600);

    if (backend_ == nullptr) {
        backend_ = new QtSessionBackend(this);
    }
    installBackendHandlers();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(buildShell(), 1);

    sourceRefreshTimer_ = new QTimer(this);
    sourceRefreshTimer_->setInterval(kSourceRefreshIntervalMs);
    connect(sourceRefreshTimer_, &QTimer::timeout, this, [this] {
        refreshDisplays(true, true);
    });
    sourceRefreshTimer_->start();

    refreshDisplays(false);
    refreshAudioDevices();
    refreshRoomLink();
    updateRunningState(false);
}

void CreateRoomWindow::resetForNextRoom()
{
    roomId_ = generatedRoomId();
    refreshRoomLink();
    updateRunningState(false);
    setStatus("", "RoomInlineStatus");
}

QWidget* CreateRoomWindow::buildShell()
{
    auto* host = new QWidget;
    host->setObjectName("RoomContent");
    auto* layout = new QVBoxLayout(host);
    layout->setContentsMargins(36, 24, 36, 24);
    layout->setSpacing(8);
    layout->addWidget(buildHeader());
    layout->addWidget(buildLabeledField("Room Name", roomNameEdit_ = new QLineEdit(defaultRoomName())));
    roomNameEdit_->setObjectName("RoomLargeInput");
    layout->addWidget(buildPasswordField());
    layout->addWidget(buildLinkField());
    layout->addWidget(buildSettingsPanel());

    statusLabel_ = textLabel("", "RoomInlineStatus");
    statusLabel_->setAlignment(Qt::AlignCenter);
    statusLabel_->setVisible(false);
    layout->addWidget(statusLabel_);

    startButton_ = iconButton("Start Sharing", "RoomStartButton", "share");
    startButton_->setFixedHeight(48);
    connect(startButton_, &QPushButton::clicked, this, [this] { startOrStop(); });
    layout->addWidget(startButton_);

    return host;
}

QWidget* CreateRoomWindow::buildHeader()
{
    auto* header = new QWidget;
    header->setObjectName("RoomTransparentBlock");
    header->setFixedHeight(40);
    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);

    auto* back = iconButton("", "RoomBackButton", "back");
    back->setObjectName("RoomBackButton");
    back->setCursor(Qt::PointingHandCursor);
    back->setFixedSize(36, 36);
    connect(back, &QPushButton::clicked, this, [this] { handleBack(); });
    layout->addWidget(back, 0, Qt::AlignVCenter);

    layout->addWidget(textLabel("Create Room", "RoomHeaderTitle"), 1, Qt::AlignVCenter);
    return header;
}

QWidget* CreateRoomWindow::buildLabeledField(const QString& labelText, QWidget* field)
{
    field->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    field->setFixedHeight(40);

    auto* wrap = new QWidget;
    wrap->setObjectName("RoomFieldBlock");
    wrap->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* layout = new QVBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    layout->addWidget(textLabel(labelText, "RoomFieldTitle"));
    layout->addWidget(field);
    return wrap;
}

QWidget* CreateRoomWindow::buildPasswordField()
{
    auto* wrap = new QWidget;
    wrap->setObjectName("RoomFieldBlock");
    wrap->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* layout = new QVBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);
    layout->addWidget(textLabel("Password (optional)", "RoomFieldTitle"));

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);
    roomPasswordEdit_ = new QLineEdit;
    roomPasswordEdit_->setObjectName("RoomLargeInput");
    roomPasswordEdit_->setEchoMode(QLineEdit::Password);
    roomPasswordEdit_->setPlaceholderText("Leave empty for no password");
    roomPasswordEdit_->setFixedHeight(40);
    row->addWidget(roomPasswordEdit_, 1);

    auto* generate = iconButton("", "RoomSquareButton", "refresh");
    generate->setToolTip("Generate password");
    generate->setFixedSize(44, 40);
    connect(generate, &QPushButton::clicked, this, [this] {
        generatePassword();
    });
    row->addWidget(generate);
    layout->addLayout(row);
    return wrap;
}

QWidget* CreateRoomWindow::buildLinkField()
{
    auto* row = new QWidget;
    row->setObjectName("RoomTransparentBlock");
    row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    roomLinkEdit_ = new QLineEdit;
    roomLinkEdit_->setObjectName("RoomLargeInput");
    roomLinkEdit_->setReadOnly(true);
    roomLinkEdit_->setFixedHeight(40);
    layout->addWidget(roomLinkEdit_, 1);
    auto* copy = iconButton("Copy", "RoomSecondaryButton", "copy");
    copy->setMinimumWidth(124);
    copy->setFixedHeight(40);
    connect(copy, &QPushButton::clicked, this, [this] { copyRoomLink(); });
    layout->addWidget(copy);
    return buildLabeledField("Room Link (Share this link with others)", row);
}

QWidget* CreateRoomWindow::buildSettingsPanel()
{
    auto* panel = new QFrame;
    panel->setObjectName("RoomSettingsPanel");
    panel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(14, 4, 14, 4);
    layout->setSpacing(0);

    displayCombo_ = new QComboBox;
    displayCombo_->setObjectName("RoomSettingsInput");
    displayCombo_->setFixedHeight(40);
    connect(displayCombo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        populateResolutionChoices();
        updateDefaultAudioChoiceText();
    });
    layout->addWidget(buildSettingRow("display", "Source", displayCombo_));

    resolutionCombo_ = new QComboBox;
    resolutionCombo_->setObjectName("RoomSettingsInput");
    resolutionCombo_->setFixedHeight(40);
    layout->addWidget(buildSettingRow("quality", "Resolution", resolutionCombo_));

    fpsCombo_ = new QComboBox;
    fpsCombo_->setObjectName("RoomSettingsInput");
    fpsCombo_->setFixedHeight(40);
    for (const int fps : {60, 30, 24, 15}) {
        fpsCombo_->addItem(QStringLiteral("%1 FPS").arg(fps), fps);
    }
    tuneComboPopup(fpsCombo_);
    layout->addWidget(buildSettingRow("fps", "FPS", fpsCombo_));

    lowLatencyCheck_ = new QCheckBox;
    lowLatencyCheck_->setObjectName("RoomSwitch");
    lowLatencyCheck_->setChecked(false);
    lowLatencyCheck_->setCursor(Qt::PointingHandCursor);
    lowLatencyCheck_->setFixedSize(44, 24);
    lowLatencyCheck_->setToolTip(
        "Send frames immediately for minimal input lag (best for gaming / remote control).\n"
        "Trades a little smoothing for responsiveness.");
    layout->addWidget(buildSettingRow("quality", "Low Latency", lowLatencyCheck_));

    audioDeviceCombo_ = new QComboBox;
    audioDeviceCombo_->setObjectName("RoomSettingsInput");
    audioDeviceCombo_->setFixedHeight(40);
    captureAudioCheck_ = new QCheckBox;
    captureAudioCheck_->setObjectName("RoomSwitch");
    captureAudioCheck_->setChecked(true);
    captureAudioCheck_->setCursor(Qt::PointingHandCursor);
    captureAudioCheck_->setFixedSize(44, 24);
    connect(captureAudioCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        audioDeviceCombo_->setEnabled(checked);
    });
    layout->addWidget(buildSettingRow("audio", "Audio Output", buildSwitchField(captureAudioCheck_, audioDeviceCombo_)));

    auto* reportRow = new QWidget;
    reportRow->setObjectName("RoomTransparentBlock");
    auto* reportLayout = new QHBoxLayout(reportRow);
    reportLayout->setContentsMargins(0, 0, 0, 0);
    reportLayout->setSpacing(8);
    reportCheck_ = new QCheckBox;
    reportCheck_->setObjectName("RoomSwitch");
    reportCheck_->setChecked(true);
    reportCheck_->setCursor(Qt::PointingHandCursor);
    reportCheck_->setFixedSize(44, 24);
    reportLayout->addWidget(reportCheck_, 0, Qt::AlignVCenter);
    reportPathEdit_ = new QLineEdit("sender-report.zip");
    reportPathEdit_->setObjectName("RoomSettingsInput");
    reportPathEdit_->setFixedHeight(40);
    reportLayout->addWidget(reportPathEdit_, 1, Qt::AlignVCenter);
    connect(reportCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        reportPathEdit_->setEnabled(checked);
    });
    layout->addWidget(buildSettingRow("report", "Report", reportRow, true));

    roomPortSpin_ = new QSpinBox(panel);
    roomPortSpin_->setRange(1, 65535);
    roomPortSpin_->setValue(5001);
    roomPortSpin_->hide();
    return panel;
}

QWidget* CreateRoomWindow::buildSettingRow(const char* iconName, const QString& labelText, QWidget* field, bool isLast)
{
    auto* row = new QWidget;
    row->setObjectName(isLast ? "RoomSettingRowLast" : "RoomSettingRow");
    row->setFixedHeight(52);
    row->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);
    layout->addWidget(iconLabel(iconName, 24, QStringLiteral("#b7c5c1")), 0, Qt::AlignVCenter);
    auto* label = textLabel(labelText, "RoomSettingLabel");
    label->setMinimumWidth(170);
    layout->addWidget(label, 0, Qt::AlignVCenter);
    layout->addWidget(field, 1, Qt::AlignVCenter);
    return row;
}

QWidget* CreateRoomWindow::buildSwitchField(QCheckBox* toggle, QWidget* field)
{
    auto* row = new QWidget;
    row->setObjectName("RoomTransparentBlock");
    row->setFixedHeight(40);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    layout->addWidget(toggle, 0, Qt::AlignVCenter);
    layout->addWidget(field, 1, Qt::AlignVCenter);
    return row;
}

QPushButton* CreateRoomWindow::iconButton(const QString& text, const QString& objectName, const char* iconName)
{
    auto* button = new QPushButton(text);
    button->setObjectName(objectName);
    button->setCursor(Qt::PointingHandCursor);
    const QPixmap pixmap = renderSvgResource(
        QStringLiteral(":/screenshare/ui/icons/%1.svg").arg(QString::fromUtf8(iconName)),
        QSize(18, 18),
        QStringLiteral("#ffffff"));
    if (!pixmap.isNull()) {
        button->setIcon(QIcon(pixmap));
        button->setIconSize(QSize(18, 18));
    }
    return button;
}

QLabel* CreateRoomWindow::textLabel(const QString& text, const char* objectName)
{
    auto* label = new QLabel(text);
    label->setObjectName(QString::fromUtf8(objectName));
    label->setWordWrap(true);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    label->setAutoFillBackground(false);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    return label;
}

QLabel* CreateRoomWindow::iconLabel(const char* iconName, int size, const QString& color)
{
    auto* label = new QLabel;
    label->setObjectName("RoomIconLabel");
    label->setFixedSize(size, size);
    label->setAlignment(Qt::AlignCenter);
    label->setPixmap(renderSvgResource(
        QStringLiteral(":/screenshare/ui/icons/%1.svg").arg(QString::fromUtf8(iconName)),
        QSize(size, size),
        color));
    return label;
}

void CreateRoomWindow::refreshDisplays(bool preserveSelection, bool skipOpenPopup)
{
    if (displayCombo_ == nullptr || backend_ == nullptr || backend_->isRunning()) {
        return;
    }
    if (skipOpenPopup && displayCombo_->view() != nullptr && displayCombo_->view()->isVisible()) {
        return;
    }

    const QString previousSource = preserveSelection ? displayCombo_->currentData().toString() : QString();
    QString displayError;
    QString windowError;
    const auto displays = backend_->listDisplays(&displayError);
    const auto windows = backend_->listWindows(&windowError);
    populateCaptureSources(displays, windows, previousSource);
    if (!displayError.isEmpty()) {
        setStatus("Using Qt display fallback: " + displayError, "RoomInlineStatus");
    } else if (!windowError.isEmpty()) {
        setStatus("Application list unavailable: " + windowError, "RoomInlineStatus");
    }
}

void CreateRoomWindow::refreshAudioDevices()
{
    QString error;
    const auto devices = backend_->listAudioDevices(&error);
    populateAudioDevices(devices);
    if (!error.isEmpty()) {
        setStatus("Audio devices unavailable: " + error, "RoomInlineStatusError");
    }
}

void CreateRoomWindow::populateCaptureSources(
    const std::vector<screenshare::SessionDisplayInfo>& displays,
    const std::vector<screenshare::SessionWindowInfo>& windows,
    const QString& preferredSourceValue)
{
    const bool blocked = displayCombo_->blockSignals(true);
    displayCombo_->clear();
    if (!displays.empty()) {
        for (const auto& display : displays) {
            QStringList parts;
            parts << QStringLiteral("Display %1").arg(display.index);
            if (!display.outputName.empty()) {
                parts << QString::fromStdString(display.outputName);
            }
            if (display.width > 0 && display.height > 0) {
                parts << QStringLiteral("%1 x %2").arg(display.width).arg(display.height);
            }
            displayCombo_->addItem(parts.join(" - "), displaySourceValue(display.index));
            displayCombo_->setItemData(displayCombo_->count() - 1, QSize(display.width, display.height), Qt::UserRole + 1);
            displayCombo_->setItemData(displayCombo_->count() - 1, 0U, Qt::UserRole + 2);
        }
    } else {
        const QList<QScreen*> screens = QApplication::screens();
        for (int i = 0; i < screens.size(); ++i) {
            const QRect geometry = screens[i]->geometry();
            displayCombo_->addItem(
                QStringLiteral("Display %1 - %2 x %3").arg(i).arg(geometry.width()).arg(geometry.height()),
                displaySourceValue(i));
            displayCombo_->setItemData(displayCombo_->count() - 1, geometry.size(), Qt::UserRole + 1);
            displayCombo_->setItemData(displayCombo_->count() - 1, 0U, Qt::UserRole + 2);
        }
        if (displayCombo_->count() == 0) {
            displayCombo_->addItem("Display 0", displaySourceValue(0));
            displayCombo_->setItemData(displayCombo_->count() - 1, 0U, Qt::UserRole + 2);
        }
    }
    if (!windows.empty()) {
        displayCombo_->insertSeparator(displayCombo_->count());
        for (const auto& window : windows) {
            QString text;
            const QString title = QString::fromStdString(window.title);
            const QString process = QString::fromStdString(window.processName);
            if (!process.isEmpty()) {
                text = QStringLiteral("%1 - %2").arg(process, title);
            } else {
                text = title;
            }
            if (window.width > 0 && window.height > 0) {
                text += QStringLiteral(" - %1 x %2").arg(window.width).arg(window.height);
            }
            displayCombo_->addItem(text, windowSourceValue(window.handle));
            displayCombo_->setItemData(displayCombo_->count() - 1, QSize(window.width, window.height), Qt::UserRole + 1);
            displayCombo_->setItemData(displayCombo_->count() - 1, window.processId, Qt::UserRole + 2);
        }
    }
    if (!preferredSourceValue.isEmpty()) {
        const int index = displayCombo_->findData(preferredSourceValue);
        if (index >= 0) {
            displayCombo_->setCurrentIndex(index);
        }
    }
    displayCombo_->blockSignals(blocked);
    populateResolutionChoices();
    updateDefaultAudioChoiceText();
    tuneComboPopup(displayCombo_);
}

void CreateRoomWindow::populateAudioDevices(const std::vector<screenshare::SessionAudioDeviceInfo>& devices)
{
    audioDeviceCombo_->clear();
    audioDeviceCombo_->addItem("System Audio (default)", QString());
    for (const auto& device : devices) {
        if (device.source != screenshare::SessionAudioDeviceSource::SystemOutput) {
            continue;
        }
        QString name = QString::fromStdString(device.name);
        if (device.isDefault) {
            name += " (default)";
        }
        audioDeviceCombo_->addItem(name, QString::fromStdString(device.id));
    }
    updateDefaultAudioChoiceText();
    tuneComboPopup(audioDeviceCombo_);
}

void CreateRoomWindow::updateDefaultAudioChoiceText()
{
    if (audioDeviceCombo_ == nullptr || audioDeviceCombo_->count() == 0) {
        return;
    }
    const QString sourceValue = displayCombo_ == nullptr ? QString() : displayCombo_->currentData().toString();
    audioDeviceCombo_->setItemText(
        0,
        sourceValueIsWindow(sourceValue) ?
            QStringLiteral("Application Audio (selected window)") :
            QStringLiteral("System Audio (default)"));
}

void CreateRoomWindow::populateResolutionChoices()
{
    if (resolutionCombo_ == nullptr) {
        return;
    }
    const QString previous = resolutionCombo_->currentData().toString();
    const bool blocked = resolutionCombo_->blockSignals(true);
    resolutionCombo_->clear();
    resolutionCombo_->addItem("Auto (Recommended)", "auto");
    for (const QSize& size : resolutionChoicesForDisplay(selectedDisplaySize())) {
        resolutionCombo_->addItem(resolutionChoiceText(size), resolutionChoiceValue(size));
    }
    const int index = resolutionCombo_->findData(previous);
    resolutionCombo_->setCurrentIndex(index >= 0 ? index : 0);
    resolutionCombo_->blockSignals(blocked);
    tuneComboPopup(resolutionCombo_);
}

void CreateRoomWindow::refreshRoomLink()
{
    if (roomLinkEdit_ != nullptr) {
        roomLinkEdit_->setText(roomLink());
    }
}

void CreateRoomWindow::generatePassword()
{
    if (roomPasswordEdit_ != nullptr) {
        roomPasswordEdit_->setText(generatedPassword());
        roomPasswordEdit_->setFocus();
    }
}

void CreateRoomWindow::installBackendHandlers()
{
    if (backend_ == nullptr) {
        return;
    }

    backend_->setVideoFrameHandler({});
    backend_->setDirectVideoFrameHandler({});
    backend_->setStartedHandler([this] {
        updateRunningState(true);
        setStatus("Opening room...", "RoomInlineStatusConnecting");
    });
    backend_->setErrorHandler([this](const QString& message) {
        setStatus(message.isEmpty() ? QStringLiteral("Session failed") : message, "RoomInlineStatusError");
    });
    backend_->setFinishedHandler([this](const QtSessionBackend::FinishInfo& info) {
        updateRunningState(false);
        if (info.stopRequested) {
            setStatus("Sharing stopped", "RoomInlineStatus");
        } else if (info.failed) {
            setStatus("Sharing failed", "RoomInlineStatusError");
        } else {
            setStatus("Sharing finished", "RoomInlineStatus");
        }
    });
    backend_->setStatusHandler([this](const screenshare::SessionEvent& event) {
        handleStatusEvent(event);
    });
}

void CreateRoomWindow::startOrStop()
{
    if (backend_->isRunning()) {
        stopShare();
    } else {
        startShare();
    }
}

void CreateRoomWindow::startShare()
{
    if (!validateFields()) {
        return;
    }
    installBackendHandlers();
    setStatus("Starting room...", "RoomInlineStatusConnecting");
    QString error;
    if (!backend_->startShare(currentConfig(), &error)) {
        setStatus(error.isEmpty() ? QStringLiteral("Could not start sharing") : error, "RoomInlineStatusError");
        updateRunningState(false);
        return;
    }
    if (actions_.shareStarted) {
        actions_.shareStarted(currentShareUiState());
    }
}

void CreateRoomWindow::stopShare()
{
    if (!backend_->isRunning()) {
        return;
    }
    setStatus("Stopping...", "RoomInlineStatusConnecting");
    startButton_->setEnabled(false);
    backend_->stop();
}

screenshare::ShareSessionConfig CreateRoomWindow::currentConfig() const
{
    screenshare::ShareSessionConfig config;
    config.connectionMode = screenshare::ShareConnectionMode::Room;
    const QString sourceValue = displayCombo_ != nullptr ? displayCombo_->currentData().toString() : displaySourceValue(0);
    if (sourceValueIsWindow(sourceValue)) {
        config.captureSourceType = screenshare::SessionCaptureSourceType::Window;
        config.windowHandle = windowHandleFromSourceValue(sourceValue);
        config.windowProcessId = displayCombo_ != nullptr ?
            displayCombo_->currentData(Qt::UserRole + 2).toUInt() :
            0U;
    } else {
        config.captureSourceType = screenshare::SessionCaptureSourceType::Display;
        config.displayIndex = displayIndexFromSourceValue(sourceValue);
    }
    config.roomPort = static_cast<uint16_t>(roomPortSpin_->value());
    config.roomId = toStdUtf8(roomId_);
    config.roomName = toStdUtf8(roomName());
    config.roomPassword = toStdUtf8(roomPassword());
    config.signalingStunServer = kDefaultStunServer;
    config.reportPath = toStdUtf8(reportPath());
    config.audioDeviceId = toStdUtf8(audioDeviceCombo_->currentData().toString());
    config.captureSystemAudio = captureAudioCheck_->isChecked();
    config.stream = currentStreamSettings();
    return config;
}

screenshare::StreamSettings CreateRoomWindow::currentStreamSettings() const
{
    screenshare::StreamSettings settings;
    settings.fps = fpsCombo_ != nullptr ? fpsCombo_->currentData().toInt() : 60;
    settings.adaptBitrate = true;
    settings.adaptResolution = resolutionCombo_->currentData().toString() == "auto";
    settings.lowLatency = lowLatencyCheck_ != nullptr && lowLatencyCheck_->isChecked();
    const QSize resolution = selectedResolution();
    if (validResolutionSize(resolution)) {
        settings.outputResolution = screenshare::SessionResolution{resolution.width(), resolution.height()};
    }
    return settings;
}

ShareSessionUiState CreateRoomWindow::currentShareUiState() const
{
    ShareSessionUiState state;
    state.config = currentConfig();
    state.roomId = roomId_;
    state.roomName = roomName();
    state.roomLink = roomLink();
    state.displayText = displayCombo_ != nullptr ? displayCombo_->currentText() : QStringLiteral("Display");
    state.displaySourceValue = displayCombo_ != nullptr ? displayCombo_->currentData().toString() : displaySourceValue(0);
    state.displayValue = state.config.displayIndex;
    state.windowHandle = state.config.windowHandle;
    state.windowProcessId = state.config.windowProcessId;
    if (displayCombo_ != nullptr) {
        for (int index = 0; index < displayCombo_->count(); ++index) {
            const QString value = displayCombo_->itemData(index).toString();
            if (value.isEmpty()) {
                continue;
            }
            state.displayChoices.push_back(ShareDisplayChoice{
                displayCombo_->itemText(index),
                value,
                displayCombo_->itemData(index, Qt::UserRole + 2).toUInt(),
            });
        }
    }
    state.resolutionText = resolutionCombo_ != nullptr ? resolutionCombo_->currentText() : QStringLiteral("Auto");
    state.resolutionValue = resolutionCombo_ != nullptr ? resolutionCombo_->currentData().toString() : QStringLiteral("auto");
    if (resolutionCombo_ != nullptr) {
        for (int index = 0; index < resolutionCombo_->count(); ++index) {
            state.resolutionChoices.push_back(ShareResolutionChoice{
                resolutionCombo_->itemText(index),
                resolutionCombo_->itemData(index).toString(),
            });
        }
    }
    state.fpsValue = fpsCombo_ != nullptr ? fpsCombo_->currentData().toInt() : 60;
    state.fpsText = QStringLiteral("%1 FPS").arg(state.fpsValue);
    state.captureSystemAudio = captureAudioCheck_ != nullptr && captureAudioCheck_->isChecked();
    state.audioDeviceValue = audioDeviceCombo_ != nullptr ? audioDeviceCombo_->currentData().toString() : QString();
    if (audioDeviceCombo_ != nullptr) {
        for (int index = 0; index < audioDeviceCombo_->count(); ++index) {
            state.audioChoices.push_back(ShareAudioChoice{
                audioDeviceCombo_->itemText(index),
                audioDeviceCombo_->itemData(index).toString(),
            });
        }
    }
    state.audioText = state.captureSystemAudio ?
        (audioDeviceCombo_ != nullptr ? audioDeviceCombo_->currentText() : QStringLiteral("System Audio")) :
        QStringLiteral("Audio off");
    state.passwordProtected = !roomPassword().isEmpty();
    return state;
}

bool CreateRoomWindow::validateFields()
{
    if (!validRoomId(roomId_)) {
        QMessageBox::warning(this, "Room ID", "The generated room ID is invalid. Close and reopen Create Room.");
        return false;
    }
    if (!validOptionalRoomText(roomName(), 80)) {
        QMessageBox::warning(this, "Room name", "The room name must be 80 characters or fewer.");
        roomNameEdit_->setFocus();
        return false;
    }
    if (!validOptionalRoomText(roomPassword(), 128)) {
        QMessageBox::warning(this, "Room password", "Room passwords must be 128 characters or fewer.");
        roomPasswordEdit_->setFocus();
        return false;
    }
    const QString sourceValue = displayCombo_ != nullptr ? displayCombo_->currentData().toString() : QString();
    if (sourceValueIsWindow(sourceValue) && windowHandleFromSourceValue(sourceValue) == 0) {
        QMessageBox::warning(this, "Source", "Choose a valid application window.");
        displayCombo_->setFocus();
        return false;
    }
    return true;
}

QString CreateRoomWindow::roomName() const
{
    return roomNameEdit_->text().trimmed();
}

QString CreateRoomWindow::roomPassword() const
{
    return roomPasswordEdit_->text();
}

QString CreateRoomWindow::reportPath() const
{
    if (!reportCheck_->isChecked()) {
        return {};
    }
    return reportPathEdit_->text().trimmed();
}

QString CreateRoomWindow::roomLink() const
{
    return QStringLiteral("%1room=%2").arg(QString::fromUtf8(kRoomLinkPrefix), roomId_);
}

QSize CreateRoomWindow::selectedDisplaySize() const
{
    return displayCombo_ == nullptr ? QSize() : displayCombo_->currentData(Qt::UserRole + 1).toSize();
}

QSize CreateRoomWindow::selectedResolution() const
{
    const QString value = resolutionCombo_ == nullptr ? QString() : resolutionCombo_->currentData().toString();
    const QStringList parts = value.split('x');
    if (parts.size() != 2) {
        return {};
    }
    bool widthOk = false;
    bool heightOk = false;
    const int width = parts[0].toInt(&widthOk);
    const int height = parts[1].toInt(&heightOk);
    return widthOk && heightOk ? QSize(width, height) : QSize();
}

void CreateRoomWindow::copyRoomLink()
{
    if (!validateFields()) {
        return;
    }
    QGuiApplication::clipboard()->setText(roomLink());
    setStatus("Room link copied", "RoomInlineStatus");
}

void CreateRoomWindow::updateRunningState(bool running)
{
    startButton_->setEnabled(true);
    startButton_->setText(running ? "Stop Sharing" : "Start Sharing");
    const QPixmap pixmap = renderSvgResource(
        running ? QStringLiteral(":/screenshare/ui/icons/stop.svg") : QStringLiteral(":/screenshare/ui/icons/share.svg"),
        QSize(20, 20),
        QStringLiteral("#ffffff"));
    if (!pixmap.isNull()) {
        startButton_->setIcon(QIcon(pixmap));
        startButton_->setIconSize(QSize(20, 20));
    }
}

void CreateRoomWindow::setStatus(const QString& text, const QString& objectName)
{
    if (statusLabel_ == nullptr) {
        return;
    }
    statusLabel_->setObjectName(objectName);
    statusLabel_->setText(text);
    statusLabel_->setVisible(!text.isEmpty());
    statusLabel_->style()->unpolish(statusLabel_);
    statusLabel_->style()->polish(statusLabel_);
}

void CreateRoomWindow::handleStatusEvent(const screenshare::SessionEvent& event)
{
    if (!event.message.empty() && event.type == screenshare::SessionEventType::Issue) {
        setStatus(QString::fromStdString(event.message), "RoomInlineStatusError");
        return;
    }

    switch (event.status.state) {
    case screenshare::SessionState::Starting:
        setStatus("Opening room...", "RoomInlineStatusConnecting");
        break;
    case screenshare::SessionState::Connecting:
        setStatus(
            event.status.role == screenshare::SessionRole::Share ?
                QStringLiteral("Idle - waiting for viewers") :
                QStringLiteral("Connecting..."),
            event.status.role == screenshare::SessionRole::Share ?
                QStringLiteral("RoomInlineStatus") :
                QStringLiteral("RoomInlineStatusConnecting"));
        break;
    case screenshare::SessionState::Live:
        setStatus("Room is live", "RoomInlineStatusLive");
        break;
    case screenshare::SessionState::Disconnected:
        setStatus(
            event.status.role == screenshare::SessionRole::Share ?
                QStringLiteral("Idle - waiting for viewers") :
                QStringLiteral("Disconnected"),
            event.status.role == screenshare::SessionRole::Share ?
                QStringLiteral("RoomInlineStatus") :
                QStringLiteral("RoomInlineStatusConnecting"));
        break;
    case screenshare::SessionState::Stopping:
        setStatus("Stopping...", "RoomInlineStatusConnecting");
        break;
    case screenshare::SessionState::Failed:
        setStatus("Sharing failed", "RoomInlineStatusError");
        break;
    default:
        break;
    }
}

void CreateRoomWindow::handleBack()
{
    if (backend_->isRunning()) {
        QMessageBox::information(this, "Sharing is running", "Stop sharing before returning to the home screen.");
        return;
    }
    if (actions_.back) {
        actions_.back();
    }
}
