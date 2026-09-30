#include "SessionDetailsWidget.h"
#include "UiStyle.h"
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>

using namespace screenshare::v2;
using namespace screenshare::media;
namespace {
const QString unavailable = QStringLiteral("Not available");
QString Size(int width, int height) {
    return width>0 && height>0 ? QString("%1 × %2").arg(width).arg(height) : unavailable;
}
QString Number(std::optional<double> number, const char* unit, int decimals=1) {
    return number ? QString::number(*number,'f',decimals)+unit : unavailable;
}
QString Codec(CodecImplementation codec) {
    switch(codec) {
    case CodecImplementation::MfH264Hardware:return "H.264 · Hardware";
    case CodecImplementation::MfH264Software:return "H.264 · Software";
    default:return unavailable;
    }
}
QString Phase(RoomPhase phase) {
    switch(phase) {
    case RoomPhase::Idle:return "Ready";
    case RoomPhase::Admitting:case RoomPhase::Connecting:return "Connecting";
    case RoomPhase::Active:return "Connected";
    case RoomPhase::Reconnecting:return "Reconnecting";
    case RoomPhase::Stopping:return "Stopping";
    case RoomPhase::Stopped:return "Stopped";
    case RoomPhase::Failed:return "Connection failed";
    }
    return unavailable;
}
QString Connection(PeerLifecycleState state) {
    switch(state) {
    case PeerLifecycleState::Connecting:return "Connecting";
    case PeerLifecycleState::Connected:return "Connected";
    case PeerLifecycleState::Backoff:case PeerLifecycleState::Restarting:return "Reconnecting";
    case PeerLifecycleState::Failed:return "Connection failed";
    case PeerLifecycleState::Closed:return "Disconnected";
    }
    return unavailable;
}
QString AudioState(AudioEndpointState state, bool host) {
    switch(state) {
    case AudioEndpointState::Running:return host?"Capturing audio":"Playing audio";
    case AudioEndpointState::Silent:return "Silent";
    case AudioEndpointState::Failed:return host?"Capture unavailable":"Output unavailable";
    default:return "Inactive";
    }
}
}
SessionDetailsWidget::SessionDetailsWidget(bool host,QWidget* parent):QWidget(parent),host_(host) {
    setObjectName("SessionDetailsOverview");
    auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,12,0,0);layout->setSpacing(12);
    if(host) {
        auto* row=new QHBoxLayout;auto* label=new QLabel("Viewer");
        peer_=new QComboBox;peer_->setObjectName("detailsViewer");peer_->setAccessibleName("Viewer to inspect");
        peer_->setMinimumContentsLength(12);peer_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        peer_->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);styleComboPopup(peer_);label->setBuddy(peer_);
        row->addWidget(label);row->addWidget(peer_,1);layout->addLayout(row);
        connect(peer_,&QComboBox::currentIndexChanged,this,[this] {
            Refresh();if(selectedPeerChanged)selectedPeerChanged(peer_->currentData().toString());
        });
    }
    note_=new QLabel;note_->setObjectName("detailsNotice");note_->setWordWrap(true);note_->setTextFormat(Qt::PlainText);
    note_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);layout->addWidget(note_);
    note_->setMinimumHeight(note_->fontMetrics().lineSpacing()*2);
    auto* scroll=new QScrollArea;scroll->setObjectName("SessionDetailsScroll");scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* metrics=new QWidget;metrics->setObjectName("DetailsMetrics");
    auto* body=new QVBoxLayout(metrics);body->setContentsMargins(0,0,8,12);body->setSpacing(20);
    scroll->setWidget(metrics);layout->addWidget(scroll,1);
    Section(body,"Connection",{"detailsConnectionState","detailsRtt","detailsTraffic"},
        {"Status","Network round trip",host?"Upload to this viewer":"Received traffic"});
    values_["detailsRtt"]->setToolTip("Time for a network round trip. This does not measure screen or input delay.");
    values_["detailsTraffic"]->setToolTip("Measured WebRTC traffic, including audio and video. IP and interface overhead are excluded.");
    if(host)Section(body,"Video",{"detailsVideoState","detailsEncodedSize","detailsEncodedFps","detailsEncoder","detailsDecodedSize","detailsDecodedFps"},
        {"Status","Encoded size","Encoded frame rate","Encoder","Viewer decoded size","Viewer decode rate"});
    else Section(body,"Video",{"detailsVideoState","detailsDecodedSize","detailsDecodedFps"},
        {"Status","Video size","Incoming frame rate"});
    Section(body,"Audio",{"detailsAudioState","detailsAudioSource"},
        {"Status",host?"Shared source":"Playback volume"});
    auto* hint=new QLabel("Not available means no current measurement. Network round trip is not screen or input delay.");
    hint->setObjectName("DetailsHint");hint->setWordWrap(true);hint->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);body->addWidget(hint);
    // Align values across sections using the actual themed/text-scaled labels.
    int captionWidth=0;
    for(auto* caption:findChildren<QLabel*>("DetailsCaption")) {caption->ensurePolished();captionWidth=qMax(captionWidth,caption->sizeHint().width());}
    for(auto* grid:metrics->findChildren<QGridLayout*>())grid->setColumnMinimumWidth(0,captionWidth);
    body->addStretch();Refresh();
}
void SessionDetailsWidget::Section(QVBoxLayout* layout,const QString& title,const QStringList& keys,const QStringList& labels) {
    auto* section=new QWidget;section->setObjectName("DetailsSection");
    auto* body=new QVBoxLayout(section);body->setContentsMargins(0,0,0,0);body->setSpacing(10);
    auto* heading=new QLabel(title);heading->setObjectName("DetailsSectionTitle");body->addWidget(heading);
    auto* grid=new QGridLayout;grid->setContentsMargins(0,0,0,0);grid->setHorizontalSpacing(20);grid->setVerticalSpacing(10);
    grid->setColumnStretch(1,1);
    for(int row=0;row<keys.size();++row) {
        auto* label=new QLabel(labels[row]);label->setObjectName("DetailsCaption");
        auto* value=new QLabel(unavailable);value->setObjectName(keys[row]);value->setProperty("detailValue",true);
        value->setTextFormat(Qt::PlainText);value->setWordWrap(true);value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        value->setProperty("metricCaption",title+": "+labels[row]);value->setAccessibleName(title+": "+labels[row]+": "+unavailable);
        grid->addWidget(label,row,0,Qt::AlignTop);grid->addWidget(value,row,1,Qt::AlignTop);values_[keys[row]]=value;
    }
    body->addLayout(grid);layout->addWidget(section);
}
void SessionDetailsWidget::Value(const QString& key,const QString& text) {
    if(auto* label=values_.value(key);label && label->text()!=text) {
        label->setText(text);label->setAccessibleName(label->property("metricCaption").toString()+": "+text);
    }
}
void SessionDetailsWidget::Update(const RoomStatus& status) {
    status_=status;
    if(peer_) {
        QStringList ids,names;
        for(const auto& member:status.members)if(!member.host) {
            const auto id=QString::fromStdString(member.peerId);ids<<id;
            const auto name=QString::fromStdString(member.nickname);
            const auto duplicates=std::count_if(status.members.begin(),status.members.end(),[&](const auto& other){return other.nickname==member.nickname;});
            names<<(name.isEmpty()?id:duplicates>1?name+" ["+id.right(6)+"]":name);
        }
        bool changed=peer_->count()!=ids.size();
        for(int i=0;!changed && i<ids.size();++i)changed=peer_->itemData(i).toString()!=ids[i] || peer_->itemText(i)!=names[i];
        if(changed) {
            const auto selected=peer_->currentData();const QSignalBlocker block(peer_);peer_->clear();
            for(int i=0;i<ids.size();++i)peer_->addItem(names[i],ids[i]);
            const int previous=peer_->findData(selected);peer_->setCurrentIndex(previous>=0?previous:ids.isEmpty()?-1:0);
        }
        peer_->setPlaceholderText("No viewers connected");peer_->setEnabled(!ids.isEmpty());
    }
    Refresh();
}
void SessionDetailsWidget::SetVideo(QSize size,double fps) {videoSize_=size;videoFps_=fps;Refresh();}
void SessionDetailsWidget::SelectPeer(const QString& id) {if(peer_) {const int index=peer_->findData(id);if(index>=0)peer_->setCurrentIndex(index);}}
QString SessionDetailsWidget::SelectedPeer() const {return peer_?peer_->currentData().toString():QString();}
void SessionDetailsWidget::Refresh() {
    const PeerStreamStatus* peer=nullptr;
    const auto id=peer_?peer_->currentData().toString().toStdString():std::string();
    for(const auto& candidate:status_.stream.peers)if(candidate.peerId==id)peer=&candidate;
    const bool active=status_.phase==RoomPhase::Active;
    QString connection=Phase(status_.phase);
    if(active && host_) {
        connection=peer_ && peer_->count()?"Connecting":"Waiting for viewers";
        if(peer)connection=Connection(peer->recovery.state);
        for(const auto& candidate:status_.stream.connections)if(candidate.peerId==id)connection=Connection(candidate.recovery.state);
    } else if(active && status_.failedPeers)connection="Connection failed";
    Value("detailsConnectionState",connection);
    const bool fresh=active && connection=="Connected" && peer && !peer->transportSampleStale;
    const bool stale=active && peer && peer->transportSampleStale;
    const auto rtt=host_?fresh?peer->sender.rttMs:std::nullopt:active?status_.stream.receiveRttMs:std::nullopt;
    Value("detailsRtt",host_ && stale?QString("Stale"):Number(rtt," ms",0));
    const auto rate=host_?fresh?peer->transportSendBps:std::nullopt:active?status_.stream.receiveBps:std::nullopt;
    Value("detailsTraffic",host_ && active && peer && peer->transportSampleStale?"Stale":rate?QString("%1 Mbps").arg(*rate/1000000.0,0,'f',2):unavailable);
    QString notice;
    if(status_.phase==RoomPhase::Failed || connection=="Connection failed")notice="The connection has failed. Leave and rejoin to try again.";
    else if(status_.phase==RoomPhase::Reconnecting || connection=="Reconnecting")notice="Reconnecting. Measurements will return when the connection recovers.";
    else if(!active)notice="Measurements will appear when the session is connected.";
    else if(host_ && (status_.stream.capture.state==HostMediaState::Failed || status_.stream.capture.state==HostMediaState::SourceClosed))notice="The shared source is unavailable. Use Change source to choose an available source.";
    else if(host_ && status_.stream.capture.state==HostMediaState::Minimized)notice="The shared window is minimized. Restore it to resume video.";
    else if(host_ && !peer)notice=peer_ && peer_->count()?"Waiting for measurements from this viewer.":"A viewer's measurements will appear here after they connect.";
    else if(host_ && peer->transportSampleStale)notice="Connection measurements are stale. Waiting for a fresh sample.";
    else notice=host_?"Live measurements for the selected viewer.":"Live measurements from this computer.";
    if(status_.phase==RoomPhase::Stopped)notice="The session has ended. Saved reports remain available in the log folder.";
    else if(status_.phase==RoomPhase::Stopping)notice="Ending the session. The log folder remains available.";
    note_->setText(notice);
    QString video=active?"Waiting for video":"Inactive";
    if(active && (connection=="Connection failed" || connection=="Disconnected"))video="Connection unavailable";
    else if(active && host_ && (status_.stream.capture.state==HostMediaState::Failed || status_.stream.capture.state==HostMediaState::SourceClosed))video="Shared source unavailable";
    else if(active && host_ && status_.stream.capture.state==HostMediaState::Minimized)video="Shared window minimized";
    else if(active && host_ && status_.stream.capture.state==HostMediaState::Recovering)video="Recovering capture";
    else if(active && host_ && peer) {
        video=peer->rejected?"Settings could not be applied":status_.stream.preferences.videoPaused?"Paused by host":
            peer->appliedRevision && !peer->appliedVideoBitrateBps?"Paused by upload allowance":
            peer->appliedRevision<status_.stream.requestedRevision?"Applying settings":
            peer->observedRevision?"Sending video":"Waiting for source";
    } else if(active && !host_ && videoSize_.isValid())video=videoFps_>0?"Receiving video":"No new video frames";
    Value("detailsVideoState",video);
    if(host_) {
        Value("detailsEncodedSize",active && peer && peer->observedRevision?Size(peer->width,peer->height):unavailable);
        Value("detailsEncodedFps",peer && active && peer->transportSampleStale?"Stale":Number(fresh?peer->sender.encodedFps:std::nullopt," FPS"));
        Value("detailsEncoder",Codec(fresh?peer->sender.encoder:CodecImplementation::Unknown));
        const bool received=active && peer && !peer->receiver.stale && peer->receiver.observation.has_value();
        Value("detailsDecodedSize",active && peer && peer->receiver.stale?"Stale":received?Size(peer->receiver.observation->width,peer->receiver.observation->height):unavailable);
        Value("detailsDecodedFps",active && peer && peer->receiver.stale?"Stale":received && peer->receiver.observation->fpsMilli?QString("%1 FPS").arg(*peer->receiver.observation->fpsMilli/1000.0,0,'f',1):unavailable);
        values_["detailsDecodedSize"]->setToolTip("Reported by the viewer's decoder. This does not confirm physical display.");
    } else {
        Value("detailsDecodedSize",active?Size(videoSize_.width(),videoSize_.height()):unavailable);
        Value("detailsDecodedFps",active && videoSize_.isValid()?QString("%1 FPS").arg(videoFps_,0,'f',1):unavailable);
    }
    const auto health=host_?status_.audio.health:status_.playback.health;
    Value("detailsAudioState",!active?"Inactive":host_ && status_.audio.selected.kind==AudioKind::None?"Not shared":
        health.state==AudioEndpointState::Failed?AudioState(health.state,host_):
        !host_ && status_.playback.selected.muted?"Muted":
        health.state==AudioEndpointState::Inactive?host_ && !status_.activePeers?"Waiting for a viewer":"Waiting for audio":AudioState(health.state,host_));
    if(host_) {
        QString source="System output";
        switch(status_.audio.selected.kind) {
        case AudioKind::Microphone:source="Microphone";break;
        case AudioKind::Process:source="Process output";break;
        case AudioKind::None:source="None";break;
        default:break;
        }
        Value("detailsAudioSource",source);
    } else Value("detailsAudioSource",QString("%1%").arg(status_.playback.selected.volume));
}
