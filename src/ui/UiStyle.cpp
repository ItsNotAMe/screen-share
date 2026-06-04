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
QFrame#AppShellFrame[chromeHidden="true"] {
    background: #000000;
    border: 0;
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
QWidget#HomeBrandBlock {
    background: transparent;
    border: 0;
}
QLabel#HomeVersion {
    background: #1d2826;
    color: #7be1d8;
    border: 1px solid #2b514d;
    border-radius: 7px;
    padding: 3px 8px;
    font-size: 9.5pt;
    font-weight: 720;
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
QDialog#UpdateDialog {
    background: transparent;
    color: #eef3f1;
    font-family: "Segoe UI", "Inter", Arial, sans-serif;
    font-size: 10pt;
}
QFrame#UpdateDialogFrame {
    background: #1a1f1e;
    border: 1px solid #303b38;
    border-radius: 10px;
}
QFrame#UpdateHeader {
    background: transparent;
    border: 0;
}
QLabel#UpdateDownloadBadge {
    background: transparent;
    border: 3px solid #28ded3;
    border-radius: 27px;
}
QFrame#UpdateNotesFrame {
    background: #1d2422;
    border: 1px solid #303b38;
    border-radius: 8px;
}
QFrame#UpdateNoteRow {
    background: transparent;
    border: 0;
    min-height: 50px;
}
QFrame#UpdateSeparator,
QFrame#UpdateNoteDivider {
    background: #2c3533;
    border: 0;
    color: #2c3533;
}
QLabel#UpdateBullet {
    background: #28ded3;
    border: 0;
    border-radius: 4px;
}
QLabel#UpdateTitle {
    background: transparent;
    color: #f4fbf9;
    font-size: 20pt;
    font-weight: 780;
}
QLabel#UpdateVersion {
    background: transparent;
    color: #d9e4e1;
    font-size: 12.5pt;
    font-weight: 540;
}
QLabel#UpdateBody,
QLabel#UpdateNote,
QLabel#UpdateSecurity {
    background: transparent;
    color: #c4cfcb;
    font-size: 11pt;
    font-weight: 540;
}
QLabel#UpdateNote {
    color: #d7e0dd;
    font-size: 12pt;
}
QFrame#UpdateSecurityRow {
    background: transparent;
    border: 0;
}
QLabel#UpdateSecurity {
    color: #b8c4c0;
    font-size: 11.5pt;
}
QLabel#UpdateSecurityIcon {
    background: transparent;
    border: 0;
}
QLabel#UpdateStatus {
    background: transparent;
    color: #82dad2;
    font-size: 9.5pt;
    font-weight: 600;
}
QPushButton#UpdateCloseButton {
    background: transparent;
    border: 0;
    border-radius: 6px;
    padding: 0;
}
QPushButton#UpdateCloseButton:hover {
    background: #26302e;
}
QPushButton#UpdatePrimary {
    background: #168f82;
    color: #ffffff;
    border: 0;
    border-radius: 8px;
    padding: 0 18px;
    font-size: 12pt;
    font-weight: 780;
}
QPushButton#UpdatePrimary:hover {
    background: #1aa99a;
}
QPushButton#UpdatePrimary:pressed {
    background: #107468;
}
QPushButton#UpdatePrimary:disabled {
    background: #244743;
    color: #90aaa6;
}
QPushButton#UpdateSecondary {
    background: #202625;
    color: #edf5f2;
    border: 1px solid #353e3c;
    border-radius: 8px;
    padding: 0 18px;
    font-size: 11.5pt;
    font-weight: 740;
}
QPushButton#UpdateSecondary:hover {
    background: #26302e;
}
QPushButton#UpdateSecondary:disabled {
    color: #667470;
}
QProgressBar#UpdateProgress {
    background: #101413;
    border: 1px solid #2f3a37;
    border-radius: 6px;
    min-height: 12px;
    max-height: 12px;
    text-align: center;
}
QProgressBar#UpdateProgress::chunk {
    background: #27c9bd;
    border-radius: 5px;
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
    padding: 2px 42px 2px 10px;
}
QComboBox#RoomSettingsInput::drop-down {
    subcontrol-origin: border;
    subcontrol-position: top right;
    width: 36px;
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
QWidget#ActiveShareWindow,
QWidget#ActiveContent,
QWidget#ActiveContentPage,
QWidget#ActiveTransparentBlock {
    background: #171a19;
    color: #eef3f1;
    font-family: "Segoe UI", "Inter", Arial, sans-serif;
    font-size: 10pt;
}
QFrame#ActiveSettingsOverlay {
    background: rgba(4, 8, 9, 150);
    border: 0;
}
QFrame#ActiveCard,
QFrame#ActiveSettingsPanel {
    background: #202625;
    border: 1px solid #303938;
    border-radius: 8px;
}
QFrame#ActiveSettingsPanel {
    background: #1d2322;
    border-radius: 0;
}
QScrollArea#ActiveSettingsScroll,
QWidget#ActiveSettingsForm,
QScrollArea#ActiveSettingsScroll > QWidget,
QScrollArea#ActiveSettingsScroll > QWidget > QWidget {
    background: transparent;
    border: 0;
}
QScrollArea#ActiveSettingsScroll QScrollBar:vertical {
    background: transparent;
    border: 0;
    width: 8px;
    margin: 0;
}
QScrollArea#ActiveSettingsScroll QScrollBar::handle:vertical {
    background: #3c4946;
    border-radius: 4px;
    min-height: 24px;
}
QScrollArea#ActiveSettingsScroll QScrollBar::add-line:vertical,
QScrollArea#ActiveSettingsScroll QScrollBar::sub-line:vertical,
QScrollArea#ActiveSettingsScroll QScrollBar::add-page:vertical,
QScrollArea#ActiveSettingsScroll QScrollBar::sub-page:vertical {
    background: transparent;
    border: 0;
    height: 0;
}
QWidget#ActiveFooter {
    background: transparent;
    border-top: 1px solid #2a3331;
}
QLabel#ActiveTopTitle {
    background: transparent;
    color: #eef3f1;
    font-size: 10.5pt;
    font-weight: 680;
}
QLabel#ActiveTopMuted {
    background: transparent;
    color: #9daaa6;
    font-size: 10pt;
    font-weight: 620;
}
QLabel#ActiveTopSharing,
QLabel#ActiveTopLive,
QLabel#ActiveTopIdle,
QLabel#ActiveTopError {
    background: transparent;
    font-size: 10pt;
    font-weight: 760;
}
QLabel#ActiveTopLive {
    color: #8dde7b;
}
QLabel#ActiveTopSharing,
QLabel#ActiveTopIdle {
    color: #ffd56a;
}
QLabel#ActiveTopError {
    color: #ff9b94;
}
QLabel#ActiveHeroTitle {
    background: transparent;
    color: #f4fbf9;
    font-size: 16pt;
    font-weight: 760;
}
QLabel#ActiveSectionTitle {
    background: transparent;
    color: #f4fbf9;
    font-size: 12pt;
    font-weight: 740;
    padding: 0;
    margin: 0;
}
QLabel#ActiveMuted {
    background: transparent;
    color: #a9b5b1;
    font-size: 10pt;
    font-weight: 600;
}
QLabel#ActiveMetricValue {
    background: transparent;
    color: #f1f7f5;
    font-size: 10.5pt;
    font-weight: 730;
}
QLabel#ActiveGoodText,
QLabel#ActiveHealthGood,
QLabel#ActiveViewerDot {
    background: transparent;
    color: #8dde7b;
    font-size: 10.5pt;
    font-weight: 760;
}
QLabel#ActiveHealthWaiting {
    background: transparent;
    color: #ffd56a;
    font-size: 10.5pt;
    font-weight: 760;
}
QLabel#ActiveHealthError {
    background: transparent;
    color: #ff9b94;
    font-size: 10.5pt;
    font-weight: 760;
}
QLabel#ActiveViewerName {
    background: transparent;
    color: #eef3f1;
    font-size: 10pt;
    font-weight: 700;
}
QLabel#ActiveSettingsHeading {
    background: transparent;
    color: #f4fbf9;
    font-size: 10.5pt;
    font-weight: 760;
    margin-top: 8px;
}
QLabel#ActiveSettingsLabel {
    background: transparent;
    color: #a9b5b1;
    font-size: 9pt;
    font-weight: 650;
    margin-top: 2px;
}
QFrame#ActiveDivider {
    background: #2a3331;
    border: 0;
}
QWidget#ActiveMetricRow {
    background: transparent;
    border-top: 1px solid #2a3331;
}
QWidget#ActiveViewerArea {
    background: transparent;
}
QFrame#ActiveStatsTable {
    background: transparent;
    border: 0;
}
QFrame#ActiveMetricTile {
    background: #1b211f;
    border: 1px solid #2b3432;
    border-radius: 7px;
}
QWidget#ActiveMetricCell {
    background: transparent;
    border: 0;
}
QFrame#ActiveVerticalDivider {
    background: #2a3331;
    border: 0;
}
QWidget#ActiveViewerRow {
    background: transparent;
}
QPushButton#ActiveSecondaryButton,
QPushButton#ActiveFooterButton,
QPushButton#ActiveSquareButton {
    background: #1c2221;
    color: #edf5f2;
    border: 1px solid #33403d;
    border-radius: 7px;
    padding: 10px 14px;
    font-weight: 720;
}
QPushButton#ActiveSecondaryButton:hover,
QPushButton#ActiveFooterButton:hover,
QPushButton#ActiveSquareButton:hover {
    background: #26302d;
    border: 1px solid #4a5b58;
}
QPushButton#ActiveSecondaryButton:disabled {
    background: #1a1f1e;
    color: #67736f;
    border: 1px solid #28302f;
}
QPushButton#ActiveFooterButton {
    min-width: 170px;
    min-height: 44px;
}
QPushButton#ActiveStopButton {
    background: #8f2424;
    color: #ffffff;
    border: 1px solid #d04f45;
    border-radius: 8px;
    padding: 12px 22px;
    min-width: 300px;
    min-height: 46px;
    font-size: 11pt;
    font-weight: 770;
}
QPushButton#ActiveStopButton:hover {
    background: #a9302f;
}
QPushButton#ActiveStopButton:disabled {
    background: #4a2524;
    color: #b49896;
    border: 1px solid #6c3835;
}
QCheckBox#ActiveSettingsCheck {
    background: transparent;
    color: #c8d2cf;
    spacing: 10px;
    font-weight: 650;
}
QCheckBox#ActiveSettingsCheck::indicator {
    background: transparent;
    border: 0;
    width: 44px;
    height: 24px;
    image: url(:/screenshare/ui/icons/toggle-off.svg);
}
QCheckBox#ActiveSettingsCheck::indicator:checked {
    image: url(:/screenshare/ui/icons/toggle-on.svg);
}
QCheckBox#ActiveSettingsCheck:disabled {
    color: #7b8783;
}
QWidget#JoinRoomWindow,
QWidget#JoinRoomContent,
QWidget#JoinTransparentBlock {
    background: #171a19;
    color: #eef3f1;
    font-family: "Segoe UI", "Inter", Arial, sans-serif;
    font-size: 10pt;
}
QLabel#JoinSectionTitle {
    background: transparent;
    color: #f4fbf9;
    font-size: 10.5pt;
    font-weight: 720;
}
QFrame#JoinRoomList,
QFrame#JoinAdvancedPanel {
    background: #202625;
    border: 1px solid #303938;
    border-radius: 8px;
}
QFrame#JoinRoomRow {
    background: transparent;
    border-bottom: 1px solid #2a3331;
}
QFrame#JoinRoomRow:hover {
    background: #242b29;
}
QLabel#JoinRoomName {
    background: transparent;
    color: #f4fbf9;
    font-size: 11.5pt;
    font-weight: 740;
}
QLabel#JoinRoomMeta {
    background: transparent;
    color: #a9b5b1;
    font-size: 9pt;
    font-weight: 600;
}
QLabel#JoinPublicStatus {
    background: transparent;
    color: #84e188;
    font-size: 9pt;
    font-weight: 730;
}
QLabel#JoinLockedStatus {
    background: transparent;
    color: #ffd56a;
    font-size: 9pt;
    font-weight: 730;
}
QPushButton#JoinRoomButton {
    background: #1c2221;
    color: #edf5f2;
    border: 1px solid #168f82;
    border-radius: 7px;
    padding: 8px 12px;
    font-weight: 760;
}
QPushButton#JoinRoomButton:hover {
    background: #203331;
    border: 1px solid #31c6b8;
}
QPushButton#JoinRoomButton:disabled {
    color: #67736f;
    border: 1px solid #28302f;
}
QLabel#JoinStatusIdle,
QLabel#JoinStatusConnecting,
QLabel#JoinStatusLive,
QLabel#JoinStatusError {
    background: transparent;
    border: 0;
    font-size: 9.5pt;
    font-weight: 680;
}
QLabel#JoinStatusIdle {
    color: #8dde7b;
}
QLabel#JoinStatusConnecting {
    color: #ffd56a;
}
QLabel#JoinStatusLive {
    color: #84e188;
}
QLabel#JoinStatusError {
    color: #ff9b94;
}
QWidget#ActiveWatchWindow,
QWidget#ActiveWatchContent {
    background: #171a19;
    color: #eef3f1;
    font-family: "Segoe UI", "Inter", Arial, sans-serif;
    font-size: 10pt;
}
QFrame#WatchPreviewPanel,
QFrame#WatchStatsPanel {
    background: #202625;
    border: 1px solid #303938;
    border-radius: 8px;
}
QFrame#WatchVolumePanel {
    background: transparent;
    border: 1px solid #303938;
    border-radius: 8px;
}
QFrame#WatchPreviewPanel {
    background: #111514;
}
QFrame#WatchPreviewPanel[streamFullscreen="true"] {
    background: #000000;
    border: 0;
    border-radius: 0;
}
QWidget#VideoFrameWidget {
    background: transparent;
    color: #a9b5b1;
}
QLabel#WatchPreviewStatus {
    background: transparent;
    color: #a9b5b1;
    font-size: 12pt;
    font-weight: 650;
}
QWidget#WatchStatRow {
    background: transparent;
    border-bottom: 1px solid #2a3331;
}
QLabel#WatchStatValue {
    background: transparent;
    color: #f1f7f5;
    font-size: 10.5pt;
    font-weight: 740;
}
QPushButton#WatchInlineButton {
    background: transparent;
    color: #edf5f2;
    border: 1px solid #33403d;
    border-radius: 7px;
    padding: 7px 10px;
    font-weight: 720;
}
QPushButton#WatchInlineButton:hover {
    background: #26302d;
    border: 1px solid #4a5b58;
}
QPushButton#WatchLeaveButton {
    background: #9f2727;
    color: #ffffff;
    border: 1px solid #d04f45;
    border-radius: 8px;
    padding: 0 16px;
    font-size: 10.5pt;
    font-weight: 770;
}
QPushButton#WatchLeaveButton:hover {
    background: #b33231;
}
QPushButton#WatchLeaveButton:disabled {
    background: #4a2524;
    color: #b49896;
    border: 1px solid #6c3835;
}
)");
}
