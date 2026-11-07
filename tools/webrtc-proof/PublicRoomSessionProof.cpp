#include "PublicRoomSessionFixture.h"
#include <QSslSocket>
#include <QNetworkAccessManager>
#include <QNetworkReply>
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
    webrtc::InitializeLogging(std::move(logging)); webrtc::WinsockInitializer winsock;
    if (winsock.error() || !webrtc::InitializeSSL()) return 1;
    int result = 0;
    std::optional<int64_t> inputResponseMs;
    try {
        const bool socketReconnect = argc == 3 && std::string(argv[2]) == "socket-reconnect";
        Check(argc == 2 || socketReconnect);
        // HTTPS runs exercise the production transport policy. Only the local
        // Worker fixture needs the diagnostic plaintext exception.
        const bool diagnosticPlaintext = std::string(argv[1]).starts_with("http://");
        if (!diagnosticPlaintext && !QSslSocket::supportsSsl())
            throw std::runtime_error("Qt TLS backend unavailable; deploy the networking runtime before live-service testing");
#ifdef SCREENSHARE_WINDOWS_ROOM_PROOF
        screenshare::WindowsMediaRuntime windowsRuntime;
        Check(SUCCEEDED(windowsRuntime.result()));
        proof::TestWindow window;
        captureWindow = window.handle();
#endif
        auto hostEvidence = std::make_shared<Evidence>();
#ifndef SCREENSHARE_WINDOWS_ROOM_PROOF
        hostEvidence->sharedSourceAudio = true;
        hostEvidence->switchableCapture = true;
#endif
        auto factory = [](auto evidence) { return [evidence](auto identity, auto send) { return std::make_unique<Runtime>(identity, std::move(send), evidence); }; };
        RoomOptions hostOptions; hostOptions.origin = argv[1]; hostOptions.host = true; hostOptions.nickname = "FacadeHost"; hostOptions.name = "Facade media";
        RoomSession host(factory(hostEvidence), diagnosticPlaintext);
        auto start = host.Start(hostOptions); auto duplicate = host.Start(hostOptions);
        Check(Get(duplicate).error == RoomError::Busy);
        const auto started = Get(start);
        if (started.error != RoomError::None) std::cerr << "Host admission error " << int(started.error) << '\n';
        Check(started.error == RoomError::None);
        std::array<std::unique_ptr<RoomSession>, 4> viewers;
        std::array<std::shared_ptr<Evidence>, 4> evidence;
        auto options = hostOptions; options.host = false; options.roomId = host.Status().roomId;
        for (size_t i = 0; i < viewers.size(); ++i) {
            evidence[i] = std::make_shared<Evidence>(); options.nickname = "FacadeViewer" + std::to_string(i);
            viewers[i] = std::make_unique<RoomSession>(factory(evidence[i]), diagnosticPlaintext);
            auto joining = viewers[i]->Start(options); const auto joined = Get(joining);
            if (joined.error != RoomError::None) std::cerr << "Join error " << int(joined.error) << "; host error " << int(host.Status().error) << '\n';
            Check(joined.error == RoomError::None);
        }
        Wait([&] { for (auto& value : evidence) if (value->frames < 45 || value->audio->audibleBlocks < 20) return false; return host.Status().activePeers == 4; });
        // Actual encrypted data channels and public port while four media peers
        // run. The sink changes only a synthetic scene, never Windows input.
        const auto hostId = host.Status().peerId, controllerId = viewers[0]->Status().peerId;
        Wait([&] {
            if (!host.Input() || !viewers[0]->Input()) return false;
            for (const auto& p : viewers[0]->Input()->Read()) if (p.peer == hostId && p.ready && p.permission) return true;
            return false;
        });
        auto granted = [&](const std::shared_ptr<screenshare::input::Port>& port, const std::string& peer) {
            for (const auto& p : port->Read()) if (p.peer == peer) return p.granted;
            return uint8_t(0);
        };
        const uint8_t capability =
#ifdef SCREENSHARE_WINDOWS_ROOM_PROOF
            screenshare::input::Mouse | screenshare::input::Keyboard;
#else
            screenshare::input::Keyboard;
#endif
        Check(viewers[0]->Input()->Request(hostId, capability));
        Wait([&] { for (const auto& p : host.Input()->Read()) if (p.peer == controllerId && p.requested == capability) return true; return false; });
        Check(!granted(host.Input(), controllerId));
        Check(host.Input()->Grant(controllerId, capability));
        Wait([&] { return granted(viewers[0]->Input(), hostId) == capability; });
        screenshare::input::Event press;
#ifdef SCREENSHARE_WINDOWS_ROOM_PROOF
        press.kind = screenshare::input::Kind::Button; press.x = press.y = .5f;
#else
        press.kind = screenshare::input::Kind::Key; press.key = 65;
#endif
        press.down = true;
        const auto inputStart = std::chrono::steady_clock::now();
        Check(viewers[0]->Input()->Submit(hostId, press));
        Wait([&] { return hostEvidence->input->applied > 0; });
#ifndef SCREENSHARE_WINDOWS_ROOM_PROOF
        Wait([&] { return evidence[0]->responseFrames > 0; });
        inputResponseMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - inputStart).count();
#endif
        // Stop transport advancement, not the input owner: neutralize held input
        // while keeping the host's grant, then resume with a fresh epoch.
        uint64_t preGap=0;for(const auto& p:host.Input()->Read())if(p.peer==controllerId)preGap=p.permission;
        evidence[0]->pauseAdvance = true;
        hostEvidence->pauseAdvance = true;
        Wait([&] {for(const auto& p:host.Input()->Read())if(p.peer==controllerId)return p.paused && p.granted==capability;return false;});
        Check(hostEvidence->input->paused>0 && granted(viewers[0]->Input(),hostId)==capability);
        Check(!hostEvidence->input->pressed);
        hostEvidence->pauseAdvance = false;
        evidence[0]->pauseAdvance = false;
        Wait([&] {for(const auto& p:viewers[0]->Input()->Read())if(p.peer==hostId)return p.granted==capability && p.permission>preGap;return false;});
        Check(viewers[0]->Input()->Submit(hostId,press));
        Wait([&]{return hostEvidence->input->pressed.load();});
        press.down=false;Check(viewers[0]->Input()->Submit(hostId,press));
        Wait([&]{return !hostEvidence->input->pressed.load();});press.down=true;
        host.Input()->Revoke();
        Wait([&] { return !granted(viewers[0]->Input(), hostId); });
#ifndef SCREENSHARE_WINDOWS_ROOM_PROOF
        // A failed source must not disconnect any peer or stop shared audio.
        Check(host.Input()->Grant(controllerId, capability));
        Wait([&] { return granted(viewers[0]->Input(), hostId) == capability; });
        Check(viewers[0]->Input()->Submit(hostId, press));
        Wait([&] { return hostEvidence->input->pressed.load(); });
        const auto beforeSourceFailure = host.Status();
        hostEvidence->capture->failRead = true;
        Wait([&] {
            Check(host.Status().phase == RoomPhase::Active);
            if (host.Status().stream.capture.state != HostMediaState::SourceUnavailable || hostEvidence->input->pressed) return false;
            for (auto& viewer : viewers) if (viewer->Status().stream.remoteShare.state != HostShareState::SourceUnavailable) return false;
            return !granted(host.Input(), controllerId) && !granted(viewers[0]->Input(), hostId);
        });
        Check(host.Status().stream.capture.failure == CaptureFailure::Source && hostEvidence->capture->sources == 0 &&
            !host.Input()->Grant(controllerId, capability));
        std::array<uint64_t,4> failureAudio;
        std::array<unsigned,4> failureFrames;
        for (size_t i=0;i<4;++i) { failureAudio[i]=evidence[i]->audio->audibleBlocks; failureFrames[i]=evidence[i]->frames; }
        Wait([&] { for (size_t i=0;i<4;++i) if (evidence[i]->audio->audibleBlocks<failureAudio[i]+10) return false; return true; });
        auto replacement = host.SwitchCaptureSource({CaptureKind::Display,0,0,30});
        Check(Get(replacement).error == CaptureUpdateError::None);
        Wait([&] {
            for (size_t i=0;i<4;++i) if (evidence[i]->frames<failureFrames[i]+10 || viewers[i]->Status().stream.remoteShare.state!=HostShareState::Live) return false;
            return host.Status().stream.capture.failure==CaptureFailure::None;
        });
        const auto afterSourceFailure = host.Status();
        Check(afterSourceFailure.roomId==beforeSourceFailure.roomId && afterSourceFailure.peerId==beforeSourceFailure.peerId &&
            afterSourceFailure.revision==beforeSourceFailure.revision && afterSourceFailure.activePeers==4 &&
            afterSourceFailure.audio.revision==beforeSourceFailure.audio.revision &&
            afterSourceFailure.capture.revision==beforeSourceFailure.capture.revision+1 && hostEvidence->capture->sources==1);
        for (const auto& old : beforeSourceFailure.stream.connections) {
            const auto current = std::find_if(afterSourceFailure.stream.connections.begin(),afterSourceFailure.stream.connections.end(),
                [&](const auto& peer) {return peer.peerId==old.peerId;});
            Check(current!=afterSourceFailure.stream.connections.end() && current->generation==old.generation);
        }
        Check(!granted(host.Input(),controllerId));
        Check(host.Input()->Grant(controllerId,capability));
        Wait([&] {return granted(viewers[0]->Input(),hostId)==capability;});
        host.Input()->Revoke(); Wait([&] {return !granted(viewers[0]->Input(),hostId);});
#endif
        const auto beforeRoute = viewers[0]->Status();
        const auto routeStarts = evidence[0]->audio->outputStarts.load();
        const auto routeFrames = evidence[0]->frames.load();
        const auto routeAudio = evidence[0]->audio->audibleBlocks.load();
        const auto otherRouteAudio = evidence[1]->audio->audibleBlocks.load();
        ++evidence[0]->audio->outputRoute;
        Wait([&] { return evidence[0]->audio->outputStarts > routeStarts && evidence[0]->frames >= routeFrames + 5 &&
            evidence[0]->audio->audibleBlocks >= routeAudio + 5 && evidence[1]->audio->audibleBlocks >= otherRouteAudio + 5; });
        const auto routed = viewers[0]->Status();
        Check(routed.roomId == beforeRoute.roomId && routed.peerId == beforeRoute.peerId &&
            routed.playback.revision == beforeRoute.playback.revision && routed.playback.health.failures == 0 &&
            host.Status().activePeers == 4);
        // Route a live default host capture while all four viewers keep media.
        const auto savedAudio = host.Status().audio.selected;
        if (savedAudio.kind != AudioKind::System || !savedAudio.deviceId.empty()) {
            auto useDefault = host.SwitchAudioSource({}); Check(Get(useDefault).error == AudioUpdateError::None);
        }
        const auto beforeCaptureRoute = host.Status();
        const auto captureRouteStarts = hostEvidence->audio->captureStarts.load();
        std::array<unsigned,4> captureRouteFrames;
        std::array<uint64_t,4> captureRouteAudio;
        for(size_t i=0;i<4;++i){captureRouteFrames[i]=evidence[i]->frames;captureRouteAudio[i]=evidence[i]->audio->audibleBlocks;}
        ++hostEvidence->audio->captureRoute;
        Wait([&]{
            if(hostEvidence->audio->captureStarts<=captureRouteStarts)return false;
            for(size_t i=0;i<4;++i)if(evidence[i]->frames<captureRouteFrames[i]+5 || evidence[i]->audio->audibleBlocks<captureRouteAudio[i]+5)return false;
            return true;
        });
        const auto afterCaptureRoute=host.Status();
        Check(afterCaptureRoute.audio.revision==beforeCaptureRoute.audio.revision &&
            afterCaptureRoute.audio.health.failures==beforeCaptureRoute.audio.health.failures &&
            afterCaptureRoute.audio.health.state==AudioEndpointState::Running && afterCaptureRoute.roomId==beforeCaptureRoute.roomId &&
            afterCaptureRoute.activePeers==4 && hostEvidence->audio->liveCaptures==1);
        if (savedAudio.kind != AudioKind::System || !savedAudio.deviceId.empty()) {
            auto restoreAudio = host.SwitchAudioSource(savedAudio); Check(Get(restoreAudio).error == AudioUpdateError::None);
        }
        const auto beforeAudioFailure = host.Status();
        hostEvidence->audio->captureUnavailable = true;
        Wait([&] {
            if (host.Status().audio.health.state != AudioEndpointState::Failed) return false;
            for (const auto& value : evidence) if (value->audio->quietStreak < 30) return false;
            return true;
        });
        std::array<unsigned, 4> silentFrames;
        for (size_t i = 0; i < evidence.size(); ++i) silentFrames[i] = evidence[i]->frames;
        Wait([&] { for (size_t i = 0; i < evidence.size(); ++i) if (evidence[i]->frames < silentFrames[i] + 10) return false; return true; });
        Check(host.Status().audio.revision == beforeAudioFailure.audio.revision);
        const auto startsBeforeCaptureRecovery = hostEvidence->audio->captureStarts.load();
        hostEvidence->audio->captureUnavailable = false;
        std::array<uint64_t, 4> quietAudio;
        for (size_t i = 0; i < evidence.size(); ++i) quietAudio[i] = evidence[i]->audio->audibleBlocks;
        if (!hostEvidence->sharedSourceAudio) {
            auto recoveredCapture = host.SwitchAudioSource({}); Check(Get(recoveredCapture).error == AudioUpdateError::None);
        }
        Wait([&] { for (size_t i = 0; i < evidence.size(); ++i) if (evidence[i]->audio->audibleBlocks < quietAudio[i] + 10) return false; return true; });
        Check(hostEvidence->audio->captureStarts > startsBeforeCaptureRecovery && host.Status().audio.health.failures == 1 &&
            host.Status().revision == beforeAudioFailure.revision && host.Status().activePeers == 4 &&
            host.Status().audio.selected.kind == (hostEvidence->sharedSourceAudio ? AudioKind::SharedSource : AudioKind::System));
        if (hostEvidence->sharedSourceAudio) Check(host.Status().audio.revision == beforeAudioFailure.audio.revision);
        const auto beforeOutputFailure = viewers[0]->Status();
        const auto outputStartsBeforeFailure = evidence[0]->audio->outputStarts.load();
        evidence[0]->audio->outputUnavailable = true;
        Wait([&] { return viewers[0]->Status().playback.health.state == AudioEndpointState::Failed; });
        const auto failedOutputFrames = evidence[0]->frames.load();
        const auto healthyAudio = evidence[1]->audio->audibleBlocks.load();
        Wait([&] { return evidence[0]->frames >= failedOutputFrames + 10 && evidence[1]->audio->audibleBlocks >= healthyAudio + 10; });
        Check(host.Status().activePeers == 4 && viewers[0]->Status().playback.revision == beforeOutputFailure.playback.revision);
        evidence[0]->audio->outputUnavailable = false;
        const auto outputBeforeRetry = evidence[0]->audio->audibleBlocks.load();
        // The existing room/output selection must recover without a command.
        // Native PCM and the public status snapshot advance independently.
        Wait([&] { return evidence[0]->audio->audibleBlocks >= outputBeforeRetry + 10 &&
            viewers[0]->Status().playback.health.state == AudioEndpointState::Running; });
        Check(evidence[0]->audio->outputStarts > outputStartsBeforeFailure && viewers[0]->Status().playback.health.failures == 1 &&
            viewers[0]->Status().playback.health.state == AudioEndpointState::Running &&
            viewers[0]->Status().playback.revision == beforeOutputFailure.playback.revision &&
            viewers[0]->Status().peerId == beforeOutputFailure.peerId && viewers[0]->Status().roomId == beforeOutputFailure.roomId);
        const auto beforeAudioSwitch = host.Status();
        if (hostEvidence->sharedSourceAudio) {
            hostEvidence->audio->captureUnavailable = true;
            Wait([&] { return host.Status().audio.health.state == AudioEndpointState::Failed; });
        }
        auto audioSwitch = host.SwitchAudioSource({AudioKind::None});
        Check(Get(audioSwitch).error == AudioUpdateError::None);
        const auto mutedCaptureStarts = hostEvidence->audio->captureStarts.load();
        hostEvidence->audio->captureUnavailable = false;
        // Observe real decoded silence instead of assuming a fixed jitter/codec
        // drain time. This is a correctness deadline, not a gaming latency gate.
        const auto quietDeadline = std::chrono::steady_clock::now() + 3s;
        while (!std::all_of(evidence.begin(), evidence.end(), [](const auto& value) { return value->audio->quietStreak >= 30; })) {
            Check(std::chrono::steady_clock::now() < quietDeadline); std::this_thread::sleep_for(5ms);
        }
        std::array<uint64_t, 4> quietBlocks;
        std::array<unsigned, 4> playingFrames;
        for (size_t i = 0; i < 4; ++i) { quietBlocks[i] = evidence[i]->audio->audibleBlocks; playingFrames[i] = evidence[i]->frames; }
        ++hostEvidence->audio->captureRoute; // A muted default source must not reopen.
        std::this_thread::sleep_for(300ms);
        Check(hostEvidence->audio->liveCaptures == 0 && hostEvidence->audio->captureStarts == mutedCaptureStarts);
        for (size_t i = 0; i < 4; ++i) {
            if (evidence[i]->audio->audibleBlocks != quietBlocks[i] || evidence[i]->frames <= playingFrames[i])
                std::cerr << "Audio switch viewer " << i << " nonzero blocks " << quietBlocks[i] << '/' << evidence[i]->audio->audibleBlocks
                    << " frames " << playingFrames[i] << '/' << evidence[i]->frames << " peak " << evidence[i]->audio->lastPeak << '\n';
            Check(evidence[i]->audio->audibleBlocks == quietBlocks[i] && evidence[i]->frames > playingFrames[i]);
        }
        audioSwitch = host.SwitchAudioSource({}); Check(Get(audioSwitch).error == AudioUpdateError::None);
        Wait([&] { for (size_t i = 0; i < 4; ++i) if (evidence[i]->audio->audibleBlocks < quietBlocks[i] + 10) return false; return true; });
        Check(host.Status().audio.revision == beforeAudioSwitch.audio.revision + 2 && host.Status().activePeers == 4 &&
            host.Status().revision == beforeAudioSwitch.revision && host.Status().stream.requestedRevision == beforeAudioSwitch.stream.requestedRevision);
        Wait([&] { const auto peers = host.Status().stream.peers;
            return peers.size() == 4 && std::all_of(peers.begin(), peers.end(), [](const auto& peer) {
                return peer.transportSendBps.value_or(0) > 0 && peer.receiver.observation && peer.receiver.observation->framesDecoded > 0 && !peer.receiver.stale &&
#ifdef SCREENSHARE_WINDOWS_ROOM_PROOF
                    peer.receiver.observation->decoder == CodecImplementation::MfH264Hardware &&
#else
                    peer.receiver.observation->decoder == CodecImplementation::MfH264Software &&
#endif
                    !peer.receiver.observation->presentation && peer.appliedPreferences && peer.delivery.delivered > 0 &&
                    peer.recovery.state == PeerLifecycleState::Connected;
            });
        });
        StreamPreferences live;
        const auto pausedPeer = viewers[0]->Status().peerId;
        const auto framesBeforeTelemetryPause = evidence[0]->frames.load();
        evidence[0]->pauseAdvance = true;
        try {
            Wait([&] { const auto peers = host.Status().stream.peers;
                return std::any_of(peers.begin(), peers.end(), [&](const auto& peer) {
                    return peer.peerId == pausedPeer && peer.receiver.stale && !peer.receiver.observation;
                });
            });
            Check(evidence[0]->frames > framesBeforeTelemetryPause);
        } catch (...) { evidence[0]->pauseAdvance = false; throw; }
        evidence[0]->pauseAdvance = false;
        Wait([&] { const auto peers = host.Status().stream.peers;
            return peers.size() == 4 && std::all_of(peers.begin(), peers.end(), [](const auto& peer) {
                return peer.receiver.observation && !peer.receiver.stale;
            });
        });
        if (socketReconnect) {
            Check(diagnosticPlaintext);
            auto generation = [](const RoomSession& session, const std::string& peer) {
                for (const auto& connection : session.Status().stream.connections)
                    if (connection.peerId == peer && !connection.retained) return connection.generation;
                return uint64_t(0);
            };
            auto ready = [](RoomSession& session, const std::string& peer) {
                if (auto input = session.Input()) for (const auto& state : input->Read())
                    if (state.peer == peer && state.ready && state.permission) return true;
                return false;
            };
            QNetworkAccessManager network;
            for (const auto& reconnecting : {controllerId, controllerId, hostId}) {
                const auto oldHost = generation(host, controllerId);
                const auto oldViewer = generation(*viewers[0], hostId);
                const auto healthyGeneration = generation(host, viewers[1]->Status().peerId);
                const auto before = evidence[0]->frames.load(), healthyBefore = evidence[1]->frames.load();
                const auto healthyOffers = evidence[1]->offers.load();
                QUrl url(QString::fromLocal8Bit(argv[1]));
                url.setPath(QString::fromStdString("/test/reconnect/" + host.Status().roomId + "/" + reconnecting));
                auto* reply = network.get(QNetworkRequest(url));
                Wait([&] { QCoreApplication::processEvents(); return reply->isFinished(); });
                Check(reply->error() == QNetworkReply::NoError && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 204);
                reply->deleteLater();
                Wait([&] {
                    if (generation(host, controllerId) <= oldHost || generation(*viewers[0], hostId) <= oldViewer ||
                        evidence[0]->frames < before + 30 || evidence[1]->frames < healthyBefore + 30 ||
                        host.Status().activePeers != 4 || !ready(host, controllerId) || !ready(*viewers[0], hostId)) return false;
                    const auto peers = host.Status().stream.peers;
                    return peers.size() == 4 && std::all_of(peers.begin(), peers.end(), [](const auto& peer) {
                        return peer.receiver.observation && !peer.receiver.stale && peer.receiver.observation->framesDecoded > 0;
                    });
                });
                if (reconnecting == controllerId) {
                    Check(generation(host, viewers[1]->Status().peerId) == healthyGeneration && evidence[1]->offers == healthyOffers);
                }
                // Readiness must mean the rebuilt control lane delivers input,
                // not just that RTP resumed after an ICE restart.
                Check(viewers[0]->Input()->Request(hostId, capability));
                Wait([&] { for (const auto& state : host.Input()->Read()) if (state.peer == controllerId && state.requested == capability) return true; return false; });
                Check(host.Input()->Grant(controllerId, capability));
                Wait([&] { return granted(viewers[0]->Input(), hostId) == capability; });
                const auto applied = hostEvidence->input->applied.load();
                Check(viewers[0]->Input()->Submit(hostId, press));
                Wait([&] { return hostEvidence->input->applied > applied; });
                host.Input()->Revoke();
                Wait([&] { return !granted(viewers[0]->Input(), hostId); });
            }
        }
        live.resolution = ResolutionMode::Fixed; live.width = 320; live.height = 180;
        live.preset = StreamPreset::Quality;
        live.fps = 20; live.bitrateMode = SettingMode::Manual; live.bitrateLimitBps = 1000000;
        live.aggregateUploadLimitBps = 4000000;
        auto invalidPreferences = live; invalidPreferences.width = 319;
        const auto originalRevision = host.Status().stream.requestedRevision;
        auto invalidUpdate = host.UpdateStreamPreferences(invalidPreferences);
        Check(Get(invalidUpdate).error == StreamUpdateError::Invalid);
        Check(host.Status().stream.requestedRevision == originalRevision);
        auto viewerUpdate = viewers[0]->UpdateStreamPreferences(live);
        Check(Get(viewerUpdate).error == StreamUpdateError::Unsupported);
        auto update = host.UpdateStreamPreferences(live); const auto accepted = Get(update);
        Check(accepted.error == StreamUpdateError::None && accepted.revision > originalRevision);
        Wait([&] {
            const auto status = host.Status().stream;
            if (status.peers.size() != 4) return false;
            for (const auto& peer : status.peers)
                if (peer.rejected || peer.appliedRevision != accepted.revision || peer.observedRevision != accepted.revision || peer.width != 320 || peer.height != 180 ||
                    peer.allocatedVideoBitrateBps != 672000 || peer.appliedVideoBitrateBps != 672000) return false;
            for (const auto& value : evidence) if (value->smallFrames < 10) return false;
            return true;
        });
        const unsigned restartingBefore = evidence[0]->frames, healthyBefore = evidence[1]->frames;
#ifdef SCREENSHARE_WINDOWS_ROOM_PROOF
        for (const auto& peer : host.Status().stream.peers)
            Check(peer.source.scalingPath == SourceScalingPath::Gpu && peer.source.gpuScaled > 0 &&
                peer.source.gpuReadbackFallbacks == 0 && peer.source.imageWidth == 320 && peer.source.imageHeight == 180);
#endif
        // The reconnect scenario already uses all four permitted offers in a
        // rolling minute. The ordinary scenario separately exercises ICE reuse.
        if (!socketReconnect) {
            evidence[0]->restart = true;
            Wait([&] { return evidence[0]->offers >= 2 && evidence[0]->frames >= restartingBefore + 30 && evidence[1]->frames >= healthyBefore + 30; });
        }
        Wait([&] { const auto peers = host.Status().stream.peers;
            return peers.size() == 4 && std::all_of(peers.begin(), peers.end(), [](const auto& peer) {
                return peer.receiver.observation && peer.receiver.observation->width == 320 && !peer.receiver.stale;
            });
        });
        auto stopViewer = viewers[3]->Stop(); Get(stopViewer);
        Check(evidence[3]->destroyed == 1);
        Wait([&] { return host.Status().activePeers == 3; });
        Wait([&] { const auto status = host.Status().stream;
            return status.peers.size() == 3 && std::all_of(status.peers.begin(), status.peers.end(), [&](const auto& peer) {
                return peer.appliedRevision == status.requestedRevision && peer.appliedVideoBitrateBps == 938666;
            });
        });
        auto retiredEvidence = evidence[3];
        evidence[3] = std::make_shared<Evidence>();
        viewers[3] = std::make_unique<RoomSession>(factory(evidence[3]), diagnosticPlaintext);
        auto rejoining = viewers[3]->Start(options); Check(Get(rejoining).error == RoomError::None);
        Wait([&] { return host.Status().activePeers == 4 && evidence[3]->smallFrames >= 30 && evidence[3]->audio->audibleBlocks >= 20; });
        Wait([&] { const auto status = host.Status().stream;
            return status.peers.size() == 4 && std::all_of(status.peers.begin(), status.peers.end(), [&](const auto& peer) {
                return peer.appliedRevision == status.requestedRevision && peer.appliedVideoBitrateBps == 672000 &&
                    peer.receiver.observation && peer.receiver.observation->width == 320 && !peer.receiver.stale;
            });
        });
        live.width = 640; live.height = 360; live.fps = 30;
        live.preset = StreamPreset::Gaming;
        live.bitrateMode = SettingMode::Auto; live.bitrateLimitBps.reset();
        std::vector<unsigned> framesBeforeRestore;
        for (const auto& value : evidence) framesBeforeRestore.push_back(value->frames.load());
        auto restore = host.UpdateStreamPreferences(live); const auto restored = Get(restore);
        Check(restored.error == StreamUpdateError::None && restored.revision > accepted.revision);
        Wait([&] {
            const auto status = host.Status().stream;
            if (status.peers.size() != 4) return false;
            for (const auto& peer : status.peers)
                if (peer.rejected || peer.appliedRevision != restored.revision || peer.observedRevision != restored.revision || peer.width != 640 ||
                    !peer.receiver.observation || peer.receiver.stale || peer.receiver.observation->width != 640) return false;
            for (size_t i = 0; i < evidence.size(); ++i)
                if (evidence[i]->frames < framesBeforeRestore[i] + 30) return false;
            return true;
        });
        auto leaveAgain = viewers[3]->Stop(); Get(leaveAgain);
        Wait([&] { return host.Status().activePeers == 3; });
        Check(retiredEvidence->destroyed == 1);
#ifndef SCREENSHARE_WINDOWS_ROOM_PROOF
        hostEvidence->failDelivery = true;
        Wait([&] { return host.Status().activePeers == 2 && host.Status().failedPeers == 1; });
        std::array<unsigned, 3> before{};
        for (size_t i = 0; i < before.size(); ++i) before[i] = evidence[i]->frames;
        Wait([&] {
            unsigned progressing = 0;
            for (size_t i = 0; i < before.size(); ++i) if (evidence[i]->frames >= before[i] + 20) ++progressing;
            return progressing == 2;
        });
#endif
        auto stopping = host.Stop(); auto sameStop = host.Stop(); Get(stopping); Get(sameStop);
        Check(hostEvidence->destroyed == 1 && host.Status().phase == RoomPhase::Stopped);
        auto stoppedUpdate = host.UpdateStreamPreferences(live);
        Check(Get(stoppedUpdate).error == StreamUpdateError::Unavailable);
        for (auto& viewer : viewers) { auto done = viewer->Stop(); Get(done); }
        unsigned frames = 0;
        for (auto& value : evidence) { Check(value->invalid == 0 && value->destroyed == 1); frames += value->frames; }
        // Cancellation is ordered even when the admission command has not run.
        RoomSession cancelled(factory(std::make_shared<Evidence>()), diagnosticPlaintext);
        auto pending = cancelled.Start(hostOptions); auto cancelledStop = cancelled.Stop(); Get(cancelledStop);
        Check(Get(pending).error == RoomError::Cancelled);
        std::promise<void> release; auto barrier = release.get_future().share();
        auto heldEvidence = std::make_shared<Evidence>();
        RoomSession held([&](auto, auto) { return std::make_unique<HeldRuntime>(barrier, heldEvidence); }, diagnosticPlaintext);
        hostOptions.publicRoom = false;
        std::shared_future<void> heldStop;
        try {
            auto heldStart = held.Start(hostOptions); Check(Get(heldStart).error == RoomError::None);
            heldStop = held.Stop();
            Wait([&] { return held.Status().phase == RoomPhase::Stopping; });
            Check(heldStop.wait_for(100ms) == std::future_status::timeout && heldEvidence->destroyed == 0);
        } catch (...) { release.set_value(); throw; }
        release.set_value(); Get(heldStop); Check(heldEvidence->destroyed == 1);
        // Production transport rejects the diagnostic plaintext origin.
        RoomSession secure(factory(std::make_shared<Evidence>()));
        auto plaintextOptions = hostOptions;
        plaintextOptions.origin = "http://127.0.0.1:12345";
        auto rejected = secure.Start(plaintextOptions); Check(Get(rejected).error == RoomError::Admission);
        auto secureStop = secure.Stop(); Get(secureStop);
        RoomSession failedCapture([](auto identity, auto send) {
            NativeRoomRuntimeOptions options;
            options.engine = []() -> std::unique_ptr<MediaEngine> { throw std::runtime_error("Engine must not start after failed capture"); };
            options.capture = []() -> std::unique_ptr<ICaptureSource> { throw std::runtime_error("Injected capture startup failure"); };
            options.deliver = [](auto&, const auto&) {};
            return CreateNativeRoomRuntime(identity, std::move(send), std::move(options));
        }, diagnosticPlaintext);
        auto failedStart = failedCapture.Start(hostOptions); Get(failedStart);
        Wait([&] { return failedCapture.Status().phase == RoomPhase::Failed; });
        Check(failedCapture.Status().error == RoomError::Media);
        auto failedStop = failedCapture.Stop(); Get(failedStop);
        std::cout << "{\"passed\":true,\"public_session\":true,\"native_runtime\":true,\"rejoin\":true,\"viewers\":4,\"decoded_frames\":" << frames
            << ",\"socket_reconnect\":" << (socketReconnect ? "true" : "false")
            << ",\"authorized_input\":true,\"input_response_internal_ms\":" << (inputResponseMs ? std::to_string(*inputResponseMs) : "null")
            << ",\"cancel_admission\":true,\"coalesced_stop\":true,\"media_drain_barrier\":true,\"production_tls_required\":true,\"diagnostic_plaintext\":"
            << (diagnosticPlaintext ? "true" : "false") << "}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    webrtc::CleanupSSL(); return result;
}
