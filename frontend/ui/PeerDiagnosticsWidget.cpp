#include "ui/PeerDiagnosticsWidget.h"
#include "shared/RoomStreamDiagnostics.h"
#include <QDialog>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

using namespace screenshare::media;
PeerDiagnosticsWidget::PeerDiagnosticsWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* diagnostics = new QTableWidget(0, 8, this);
    diagnostics->setObjectName("peerDiagnostics");
    diagnostics->setHorizontalHeaderLabels({"Viewer", "Settings", "Source size", "Applied cap", "Transport upload", "Receiver decoded", "Video / FPS", "WebRTC limit"});
    diagnostics->setEditTriggers(QAbstractItemView::NoEditTriggers);
    diagnostics->setSelectionBehavior(QAbstractItemView::SelectRows);
    diagnostics->setSelectionMode(QAbstractItemView::SingleSelection);
    diagnostics->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    diagnostics->setMaximumHeight(180); diagnostics->setVisible(true); layout->addWidget(diagnostics);
    auto* details = new QLabel(this); details->setObjectName("peerDiagnosticsDetails");
    details->setTextFormat(Qt::PlainText); details->setWordWrap(true);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse); details->setVisible(true); layout->addWidget(details);
    auto* detailsButton = new QPushButton("Viewer details", this); detailsButton->setObjectName("openPeerDetails");
    detailsButton->setVisible(true); detailsButton->setEnabled(false); layout->addWidget(detailsButton);
    auto* detailsDialog = new QDialog(this); detailsDialog->setObjectName("peerDetailsDialog");
    detailsDialog->setWindowTitle("Viewer details"); detailsDialog->resize(660, 540);
    auto* dialogLayout = new QVBoxLayout(detailsDialog);
    auto* dialogText = new QPlainTextEdit(detailsDialog); dialogText->setObjectName("peerDetailsText"); dialogText->setReadOnly(true); dialogLayout->addWidget(dialogText);
    auto* closeDetails = new QPushButton("Close", detailsDialog); dialogLayout->addWidget(closeDetails);
    connect(closeDetails, &QPushButton::clicked, detailsDialog, &QDialog::hide);
    auto refreshDetails = [diagnostics, details, detailsButton, detailsDialog, dialogText] {
        auto* item = diagnostics->item(diagnostics->currentRow(), 0);
        details->setText(item ? item->toolTip() : "Select a viewer for settings details. Source observation does not confirm remote display.");
        detailsButton->setEnabled(item != nullptr);
        if (!detailsDialog->isVisible()) return;
        QString text = "This viewer has left or the session has ended. Measurements are unavailable.";
        for (int row = 0; row < diagnostics->rowCount(); ++row) {
            const auto* peer = diagnostics->item(row, 0);
            if (peer && peer->data(Qt::UserRole) == detailsDialog->property("peerId")) { text = peer->toolTip(); break; }
        }
        if (dialogText->toPlainText() != text) dialogText->setPlainText(text);
    };
    connect(diagnostics, &QTableWidget::itemSelectionChanged, this, refreshDetails);
    connect(detailsButton, &QPushButton::clicked, this, [diagnostics, detailsDialog, refreshDetails] {
        auto* item = diagnostics->item(diagnostics->currentRow(), 0); if (!item) return;
        detailsDialog->setProperty("peerId", item->data(Qt::UserRole)); detailsDialog->show(); refreshDetails();
    });
    diagnostics_ = diagnostics;
    refreshDetails_ = refreshDetails;
}
void PeerDiagnosticsWidget::Update(const screenshare::v2::RoomStatus& value) {
    auto* diagnostics = diagnostics_;
    const auto& refreshDetails = refreshDetails_;
    QJsonArray rows;
    for (const auto& peer : value.stream.peers) {
        auto row = StreamPeerJson(peer, value.stream.requestedRevision);
        for (const auto& member : value.members) if (member.peerId == peer.peerId)
            row["nickname"] = QString::fromStdString(member.nickname);
        rows.append(row);
    }
    // Status/control ticks stay responsive, but unchanged measurements do
    // not rebuild the table. No additional service requests or timers.
    const auto snapshot = QJsonDocument(QJsonObject{{"rows", rows}, {"preferences", StreamPreferencesJson(value.stream.preferences)}}).toJson(QJsonDocument::Compact);
    if (diagnostics->property("snapshot").toByteArray() != snapshot) {
        diagnostics->setProperty("snapshot", snapshot);
        QString selected;
        if (auto* current = diagnostics->item(diagnostics->currentRow(), 0)) selected = current->data(Qt::UserRole).toString();
        const QSignalBlocker blocked(diagnostics);
        diagnostics->setRowCount(int(value.stream.peers.size()));
        diagnostics->setCurrentCell(-1, -1);
        diagnostics->clearSelection();
        for (int index = 0; index < int(value.stream.peers.size()); ++index) {
            const auto& peer = value.stream.peers[index];
            const auto id = QString::fromStdString(peer.peerId);
            const auto nickname = rows[index].toObject()["nickname"].toString();
            const auto rate = !peer.transportSampleStale && peer.transportSendBps ?
                QString("%1 Mbps").arg(*peer.transportSendBps / 1000000.0, 0, 'f', 2) : StreamSampleState(peer);
            const auto received = rows[index].toObject()["receiver"].toObject();
            const auto receiverSize = received["sampleState"] == "fresh" ?
                QString("%1 × %2").arg(received["width"].toInt()).arg(received["height"].toInt()) : received["sampleState"].toString();
            const auto sender = rows[index].toObject()["sender"].toObject();
            const auto videoRate = sender["videoPayloadBps"].isNull() ? QString("unknown") : QString("%1 Mbps").arg(sender["videoPayloadBps"].toDouble() / 1000000, 0, 'f', 2);
            const auto fps = sender["encodedFps"].isNull() ? QString("unknown") : QString("%1 FPS").arg(sender["encodedFps"].toDouble(), 0, 'f', 1);
            const QStringList cells{nickname.isEmpty() ? id : nickname + " [" + id + "]",
                StreamPeerState(peer, value.stream.requestedRevision),
                peer.observedRevision ? QString("%1 × %2").arg(peer.width).arg(peer.height) : "unknown",
                peer.appliedRevision ? QString("%1 Mbps").arg(peer.appliedVideoBitrateBps / 1000000.0, 0, 'f', 2) : "unknown", rate, receiverSize,
                videoRate + " / " + fps, sender["limitingReason"].toString()};
            const auto& preferences = value.stream.preferences;
            const auto requested = QString("%1; resolution %2 (%3 × %4); FPS %5 (%6); bitrate %7 (%8).")
                .arg(preferences.preset == StreamPreset::Gaming ? "Gaming" : "Quality")
                .arg(preferences.resolution == ResolutionMode::Fixed ? "Fixed" : preferences.resolution == ResolutionMode::Native ? "Native" : "Auto")
                .arg(preferences.width).arg(preferences.height)
                .arg(preferences.fpsMode == SettingMode::Manual ? "Manual" : "Auto").arg(preferences.fps)
                .arg(preferences.bitrateMode == SettingMode::Manual ? "Manual target" : "Auto ceiling")
                .arg(preferences.bitrateLimitBps ? QString::number(*preferences.bitrateLimitBps) + " bps" : "automatic allowance");
            auto detail = QString("Peer %1\nRequested / applied / source-observed revisions: %2 / %3 / %4\nAllocated video cap: %5 bps; applied video cap: %6 bps. Transport sample: %7 (expires after 3 seconds).\nTransport includes audio and protocol traffic, excludes IP/interface overhead. Remote display, latency and congestion reason: unknown.\nRequested settings: %8\nReceiver-reported decode: %9; frames %10; FPS %11. Decoder reports expire after 3 seconds and do not confirm presentation.")
                .arg(id).arg(value.stream.requestedRevision).arg(peer.appliedRevision).arg(peer.observedRevision)
                .arg(peer.allocatedVideoBitrateBps).arg(peer.appliedVideoBitrateBps).arg(StreamSampleState(peer))
                .arg(requested).arg(receiverSize)
                .arg(received["framesDecoded"].isNull() ? "unknown" : QString::number(received["framesDecoded"].toInteger()))
                .arg(received["decodeFps"].isNull() ? "unknown" : QString::number(received["decodeFps"].toDouble(), 'f', 1));
            auto metric = [&](const char* key, double scale, const char* unit) {
                return sender[key].isNull() ? QString("unknown") : QString::number(sender[key].toDouble() * scale, 'f', 2) + unit;
            };
            detail += QString("\nWebRTC video payload: %1; encoded FPS: %2.\nSelected-path RTT: %3 (not image/input latency); estimated available upload: %4.\nVideo RTCP loss: %5; jitter: %6.\nWebRTC limiting reason: %7. Receiver report age: %8 seconds.\nUnknown values are not zero. Low bitrate alone does not establish congestion.")
                .arg(metric("videoPayloadBps", 0.000001, " Mbps")).arg(metric("encodedFps", 1, ""))
                .arg(metric("rttMs", 1, " ms")).arg(metric("availableOutgoingBps", 0.000001, " Mbps"))
                .arg(metric("lossFraction", 100, "%")).arg(metric("jitterMs", 1, " ms"))
                .arg(sender["limitingReason"].toString())
                .arg(received["ageSeconds"].isNull() ? "unknown" : QString::number(received["ageSeconds"].toInteger()));
            for (int column = 0; column < cells.size(); ++column) {
                auto* item = diagnostics->item(index, column);
                if (!item) { item = new QTableWidgetItem; diagnostics->setItem(index, column, item); }
                item->setText(cells[column]); item->setToolTip(detail); item->setData(Qt::UserRole, id);
            }
            if (id == selected) { diagnostics->setCurrentCell(index, 0); diagnostics->selectRow(index); }
        }
        refreshDetails();
    }
}
