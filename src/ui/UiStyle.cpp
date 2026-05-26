#include "ui/UiStyle.h"

QString uiStyleSheet()
{
    return QStringLiteral(R"(
QWidget#AppShellWindow,
QStackedWidget#AppPageStack,
QWidget#HomeWindow,
QWidget#HomeContent {
    background: transparent;
    color: #eef3f1;
    font-family: "Segoe UI", "Inter", Arial, sans-serif;
    font-size: 10pt;
}
QFrame#AppShellFrame {
    background: #171a19;
    border: 1px solid #2d3533;
    border-radius: 12px;
}
QFrame#AppShellFrame[maximized="true"] {
    border-radius: 0;
}
QWidget#AppTitleBar {
    background: transparent;
    border: 0;
}
QPushButton#WindowControlButton,
QPushButton#WindowCloseButton {
    background: transparent;
    border: 0;
    border-radius: 0;
    padding: 0;
}
QPushButton#WindowControlButton:hover {
    background: #222a28;
}
QPushButton#WindowControlButton:pressed {
    background: #28322f;
}
QPushButton#WindowCloseButton:hover {
    background: #c64a4a;
}
QPushButton#WindowCloseButton:pressed {
    background: #a83b3b;
}
QFrame#HomeTopBar {
    background: #171a19;
    border: 0;
}
QWidget#HomeContent {
    background: #171a19;
}
QWidget#RoomWindow,
QWidget#RoomContent,
QFrame#RoomTopBar {
    background: #171a19;
    color: #eef3f1;
    font-family: "Segoe UI", "Inter", Arial, sans-serif;
    font-size: 10pt;
}
QLabel#HomeBrand {
    color: #f4fbf9;
    font-size: 19pt;
    font-weight: 750;
    letter-spacing: 0;
}
QLabel#HomeSectionTitle {
    color: #edf5f2;
    font-size: 12pt;
    font-weight: 720;
}
QFrame#HomeDivider {
    color: #2d3533;
    background: #2d3533;
    border: 0;
}
QFrame#HomePanel {
    background: transparent;
    border: 0;
}
QLabel#HomeEmptyState {
    color: #8e9a96;
    font-size: 10pt;
    font-weight: 600;
}
QFrame#HomeRoomList {
    background: #191c1b;
    border: 1px solid #2a302f;
    border-radius: 8px;
}
QFrame#HomeRoomRow {
    background: transparent;
    border-bottom: 1px solid #2a302f;
}
QFrame#HomeRoomRow:hover {
    background: #202725;
}
QFrame#HomeMetric {
    background: #191c1b;
    border: 1px solid #2a302f;
    border-radius: 8px;
}
QWidget#HomeActionTextBlock {
    background: transparent;
    border: 0;
}
QLabel#HomeInfoTitle {
    color: #c5cfcc;
    font-size: 9.5pt;
    font-weight: 650;
}
QLabel#HomeInfoPrimary {
    color: #f4fbf9;
    font-size: 10.5pt;
    font-weight: 720;
}
QLabel#HomeInfoSecondary {
    color: #8e9a96;
    font-size: 8.8pt;
    font-weight: 520;
}
QLabel#HomePublicStatus {
    color: #84e188;
    font-size: 8.5pt;
    font-weight: 700;
}
QLabel#HomeLockedStatus {
    color: #ffd56a;
    font-size: 8.5pt;
    font-weight: 700;
}
QLabel#HomeMetricValue {
    color: #f4fbf9;
    font-size: 13pt;
    font-weight: 740;
}
QLabel#HomeMetricLabel {
    color: #87928f;
    font-size: 8.5pt;
    font-weight: 650;
}
QPushButton#HomePrimary {
    background: #168f82;
    color: #ffffff;
    border: 0;
    border-radius: 8px;
    padding: 14px 24px;
    font-weight: 750;
}
QPushButton#HomePrimary:hover {
    background: #1aa99a;
}
QPushButton#HomePrimary:pressed {
    background: #107468;
}
QPushButton#HomeSecondary {
    background: #262b2a;
    color: #edf5f2;
    border: 1px solid #353e3c;
    border-radius: 8px;
    padding: 14px 24px;
    font-weight: 720;
}
QLabel#HomeActionTitlePrimary,
QLabel#HomeActionTitle {
    background: transparent;
    border: 0;
    color: #ffffff;
    font-size: 11pt;
    font-weight: 760;
    padding: 0;
    margin: 0;
}
QLabel#HomeActionDetailPrimary {
    background: transparent;
    border: 0;
    color: #e6fffb;
    font-size: 8.3pt;
    font-weight: 520;
    padding: 0;
    margin: 0;
}
QLabel#HomeActionDetail {
    background: transparent;
    border: 0;
    color: #aeb9b5;
    font-size: 8.3pt;
    font-weight: 520;
    padding: 0;
    margin: 0;
}
QPushButton#HomeSecondary:hover {
    border: 1px solid #4a5b58;
    background: #2c3331;
}
QPushButton#HomeGhost {
    background: transparent;
    color: #9faaa6;
    border: 0;
    border-radius: 8px;
    padding: 8px 12px;
    font-weight: 700;
}
QPushButton#HomeGhost:hover {
    background: #232827;
    color: #edf5f2;
}
QPushButton#HomeTinyButton {
    background: transparent;
    color: #6ee8dc;
    border: 1px solid #3c615d;
    border-radius: 7px;
    padding: 7px 10px;
    font-weight: 720;
}
QPushButton#HomeTinyButton:hover {
    background: #203331;
}
QPushButton#HomeTinyButton:disabled {
    color: #4f6865;
    border: 1px solid #2d3c3a;
}
QWidget#RoomTransparentBlock,
QWidget#RoomFieldBlock,
QLabel#RoomIconLabel {
    background: transparent;
    border: 0;
}
QLabel#RoomHeaderTitle {
    background: transparent;
    border: 0;
    color: #f4fbf9;
    font-size: 18pt;
    font-weight: 760;
}
QLabel#RoomFieldTitle {
    background: transparent;
    border: 0;
    color: #b8c4c0;
    font-size: 10pt;
    font-weight: 650;
}
QLabel#RoomSettingLabel {
    background: transparent;
    border: 0;
    color: #c8d2cf;
    font-size: 10pt;
    font-weight: 650;
}
QLabel#RoomInlineStatus,
QLabel#RoomInlineStatusConnecting,
QLabel#RoomInlineStatusLive,
QLabel#RoomInlineStatusError {
    background: transparent;
    border: 0;
    color: #a9b5b1;
    font-size: 9pt;
    font-weight: 620;
}
QLabel#RoomInlineStatusConnecting {
    color: #ffd56a;
}
QLabel#RoomInlineStatusLive {
    color: #92e58e;
}
QLabel#RoomInlineStatusError {
    color: #ff9b94;
}
QLineEdit#RoomLargeInput,
QLineEdit#RoomSettingsInput,
QSpinBox#RoomSettingsInput,
QComboBox#RoomSettingsInput {
    background: #202625;
    color: #eef3f1;
    border: 1px solid #313c3a;
    border-radius: 6px;
    min-height: 34px;
    padding: 2px 10px;
    selection-background-color: #168f82;
}
QLineEdit#RoomLargeInput {
    min-height: 34px;
    font-size: 10pt;
}
QComboBox#RoomSettingsInput {
    padding: 2px 34px 2px 10px;
}
QComboBox#RoomSettingsInput::drop-down {
    subcontrol-origin: padding;
    subcontrol-position: top right;
    width: 30px;
    background: #252b2a;
    border-left: 1px solid #313c3a;
    border-top-right-radius: 6px;
    border-bottom-right-radius: 6px;
}
QComboBox#RoomSettingsInput::down-arrow {
    image: url(:/screenshare/ui/icons/chevron-down.svg);
    width: 14px;
    height: 14px;
}
QComboBox#RoomSettingsInput QAbstractItemView {
    background: #202625;
    color: #eef3f1;
    border: 1px solid #313c3a;
    border-radius: 0;
    padding: 0;
    margin: 0;
    selection-background-color: #168f82;
    selection-color: #ffffff;
    outline: 0;
}
QComboBox#RoomSettingsInput QAbstractItemView::item {
    min-height: 30px;
    padding: 0 10px;
    background: #202625;
}
QComboBox#RoomSettingsInput QAbstractItemView::item:hover {
    background: #28302f;
}
QComboBox#RoomSettingsInput QAbstractItemView::item:selected {
    background: #168f82;
    color: #ffffff;
}
QComboBox#RoomSettingsInput QScrollBar:vertical {
    background: #202625;
    border: 0;
    width: 10px;
    margin: 0;
}
QComboBox#RoomSettingsInput QScrollBar::handle:vertical {
    background: #3c4946;
    border-radius: 5px;
    min-height: 24px;
}
QComboBox#RoomSettingsInput QScrollBar::handle:vertical:hover {
    background: #4d5c58;
}
QComboBox#RoomSettingsInput QScrollBar::add-line:vertical,
QComboBox#RoomSettingsInput QScrollBar::sub-line:vertical {
    background: transparent;
    border: 0;
    height: 0;
}
QComboBox#RoomSettingsInput QScrollBar::add-page:vertical,
QComboBox#RoomSettingsInput QScrollBar::sub-page:vertical {
    background: transparent;
}
QLineEdit#RoomLargeInput:disabled {
    color: #77837f;
    background: #1b201f;
}
QLineEdit#RoomLargeInput:focus,
QLineEdit#RoomSettingsInput:focus,
QSpinBox#RoomSettingsInput:focus,
QComboBox#RoomSettingsInput:focus {
    border: 1px solid #4ecfc2;
}
QCheckBox#RoomSwitch {
    background: transparent;
    border: 0;
    padding: 0;
    margin: 0;
    spacing: 0;
}
QCheckBox#RoomSwitch::indicator {
    background: transparent;
    border: 0;
    width: 44px;
    height: 24px;
    image: url(:/screenshare/ui/icons/toggle-off.svg);
}
QCheckBox#RoomSwitch::indicator:checked {
    image: url(:/screenshare/ui/icons/toggle-on.svg);
}
QFrame#RoomSettingsPanel {
    background: #202625;
    border: 1px solid #303938;
    border-radius: 8px;
}
QWidget#RoomSettingRow,
QWidget#RoomSettingRowLast {
    background: transparent;
}
QWidget#RoomSettingRow {
    border-bottom: 1px solid #2a3331;
}
QWidget#RoomSettingRowLast {
    border: 0;
}
QPushButton#RoomStartButton {
    background: #168f82;
    color: #ffffff;
    border: 0;
    border-radius: 8px;
    padding: 10px 16px;
    font-size: 11pt;
    font-weight: 760;
}
QPushButton#RoomStartButton:hover {
    background: #1aa99a;
}
QPushButton#RoomStartButton:disabled {
    background: #25413d;
    color: #7e9691;
}
QPushButton#RoomSecondaryButton,
QPushButton#RoomSquareButton {
    background: #252b2a;
    color: #edf5f2;
    border: 1px solid #38413f;
    border-radius: 7px;
    padding: 6px 10px;
    font-weight: 700;
}
QPushButton#RoomBackButton {
    background: transparent;
    color: #cdd7d4;
    border: 0;
    border-radius: 7px;
    padding: 7px;
}
QPushButton#RoomSecondaryButton:hover,
QPushButton#RoomSquareButton:hover,
QPushButton#RoomBackButton:hover {
    background: #2c3331;
    border: 1px solid #4a5b58;
}
)");
}
