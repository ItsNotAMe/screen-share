#include "MultiViewerScenario.h"
#include "media/webrtc/RoomPeerNegotiation.h"
#include "room/qt/RoomAdmission.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <deque>
#include <mutex>
using namespace screenshare::media;
using screenshare::room::qt::RoomSocket;
using screenshare::room::qt::RoomAdmission;
using namespace proofmedia;
namespace {
struct Packet { size_t viewer; bool fromHost; RoomPeerSignal message; };
struct Context {
    std::unique_ptr<webrtc::Thread> network = webrtc::Thread::CreateWithSocketServer(), worker = webrtc::Thread::Create();
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio;
    std::shared_ptr<proof::AudioEvidence> audioEvidence = std::make_shared<proof::AudioEvidence>();
    std::array<std::unique_ptr<MediaLink>, 4> links;
    std::array<std::unique_ptr<RoomPeerNegotiation>, 4> hosts, viewers;
    std::unique_ptr<MediaLink> unansweredLink;
    std::unique_ptr<RoomPeerNegotiation> unansweredPeer;
    HostMediaSession capture;
    uint64_t captureGeneration = 0;
    void Close() {
        if (captureGeneration) { capture.Stop(captureGeneration).get(); captureGeneration = 0; }
        for (auto& p : hosts) p.reset();
        for (auto& p : viewers) p.reset();
        unansweredPeer.reset(); unansweredLink.reset();
        for (auto& p : links) p.reset();
        audio = nullptr; factory = nullptr;
    }
    ~Context() { Close(); }
};
QByteArray Wire(const Packet& packet, const QString& room, const QString& target) {
    const auto& signal = packet.message;
    QString type;
    QJsonObject payload;
    switch (signal.kind) {
    case RoomPeerSignal::Kind::Offer: type = "signal.offer"; payload["sdp"] = QString::fromStdString(signal.sdp); break;
    case RoomPeerSignal::Kind::Answer: type = "signal.answer"; payload["sdp"] = QString::fromStdString(signal.sdp); break;
    case RoomPeerSignal::Kind::Candidate: type = "signal.candidate"; payload = {{"candidate", QString::fromStdString(signal.ice.candidate)}, {"sdpMid", QString::fromStdString(signal.ice.mid)}, {"sdpMLineIndex", signal.ice.line}}; break;
    default: type = "signal.restart_request"; break;
    }
    return QJsonDocument(QJsonObject{{"v", 2}, {"type", type}, {"roomId", room}, {"connectionId", QString::fromStdString(signal.connectionId)}, {"toPeerId", target}, {"payload", payload}}).toJson(QJsonDocument::Compact);
}
RoomPeerSignal Decode(const QJsonObject& event) {
    RoomPeerSignal signal;
    const auto type = event["type"].toString();
    signal.kind = type == "signal.offer" ? RoomPeerSignal::Kind::Offer : type == "signal.answer" ? RoomPeerSignal::Kind::Answer :
        type == "signal.candidate" ? RoomPeerSignal::Kind::Candidate : RoomPeerSignal::Kind::RestartRequest;
    signal.connectionId = event["connectionId"].toString().toStdString();
    const auto payload = event["payload"].toObject(); signal.sdp = payload["sdp"].toString().toStdString();
    signal.ice = {payload["candidate"].toString().toStdString(), payload["sdpMid"].toString().toStdString(), payload["sdpMLineIndex"].toInt()};
    return signal;
}
void Run(const QUrl& origin) {
    SignalingExecutor executor;
    std::unique_ptr<Context> context;
    std::mutex mutex;
    std::deque<Packet> outgoing, incoming;
    size_t queuedBytes = 0, peakQueuedBytes = 0, peakQueuedMessages = 0, candidatesSent = 0;
    auto execute = [&](auto action) {
        std::exception_ptr failure;
        auto done = executor.Post([&] { try { action(); } catch (...) { failure = std::current_exception(); } });
        Require(done.get().error == ExecutorError::None, "Signaling command failed");
        if (failure) std::rethrow_exception(failure);
    };
    std::array<RoomSocket::Config, 4> configs;
    RoomSocket::Config hostConfig;
    std::array<std::unique_ptr<RoomSocket>, 4> sockets;
    std::array<bool, 4> ready{};
    std::array<bool, 4> closed{};
    bool roomClosed = false;
    std::atomic<bool> slow{true};
    std::function<void(size_t, std::string)> attachMedia;
    bool hostReady = false, transportFailed = false;
    QJsonObject roster;
    auto receive = [&](const RoomSocket::Event& event, bool toHost, size_t index) {
        if (event.kind == RoomSocket::EventKind::Error || event.kind == RoomSocket::EventKind::Reconnecting) transportFailed = true;
        if (event.kind == RoomSocket::EventKind::Snapshot) {
            if (toHost) { hostReady = true; roster = event.value; } else ready[index] = true;
        }
        if (event.kind == RoomSocket::EventKind::Closed) { if (toHost) roomClosed = true; else closed[index] = true; }
        if (event.kind != RoomSocket::EventKind::Signal) return;
        if (toHost) {
            index = configs.size();
            for (size_t i = 0; i < configs.size(); ++i) if (configs[i].selfPeerId == event.value["fromPeerId"]) index = i;
        }
        if (index >= configs.size() || incoming.size() >= 256) { transportFailed = true; return; }
        incoming.push_back({index, !toHost, Decode(event.value)});
    };
    RoomSocket host([&](const auto& event) { receive(event, true, 0); }, true);
    auto pump = [&] {
        QCoreApplication::processEvents();
        Require(!transportFailed, "Room transport failed");
        if (context) execute([&] {
            for (auto& packet : incoming) {
                auto& target = packet.fromHost ? context->viewers[packet.viewer] : context->hosts[packet.viewer];
                if (target) Require(target->Receive(std::move(packet.message)), "Authenticated negotiation message rejected");
            }
            incoming.clear();
            for (size_t i = 0; i < 4; ++i) if (context->hosts[i]) Require(!context->hosts[i]->closed() && !context->viewers[i]->closed(), "Asynchronous negotiation failed");
        });
        std::deque<Packet> packets;
        { std::lock_guard lock(mutex); packets.swap(outgoing); queuedBytes = 0; }
        for (const auto& packet : packets) {
            if (packet.message.kind == RoomPeerSignal::Kind::Candidate) ++candidatesSent;
            auto& socket = packet.fromHost ? host : *sockets[packet.viewer];
            Require(socket.Send(Wire(packet, hostConfig.roomId, packet.fromHost ? configs[packet.viewer].selfPeerId : hostConfig.selfPeerId)) == RoomSocket::SendResult::Sent, "Room rejected outbound media signaling");
        }
    };
    auto wait = [&](auto predicate, const char* failure) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
        while (!predicate()) { Require(std::chrono::steady_clock::now() < deadline, failure); pump(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    };
    RoomAdmission admission(true);
    auto admit = [&](RoomAdmission::Request request) {
        auto future = admission.Start(std::move(request));
        wait([&] { return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }, "Room admission timed out");
        auto result = future.get(); Require(result.error == RoomAdmission::Error::None && result.membership.has_value(), "Room admission failed"); return *result.membership;
    };
    try {
        RoomAdmission::Request create; create.origin = origin; create.create = true; create.nickname = "MediaHost"; create.name = "Headless media"; create.viewerLimit = 4;
        hostConfig = admit(create); Require(host.Start(hostConfig), "Host socket failed");
        wait([&] { return hostReady; }, "Host snapshot timed out");
        for (size_t i = 0; i < 4; ++i) {
            RoomAdmission::Request join; join.origin = origin; join.roomId = hostConfig.roomId; join.nickname = "Viewer" + QString::number(i);
            configs[i] = admit(join);
            sockets[i] = std::make_unique<RoomSocket>([&, i](const auto& e) { receive(e, false, i); }, true);
            Require(sockets[i]->Start(configs[i]), "Viewer socket failed");
        }
        wait([&] { return std::all_of(ready.begin(), ready.end(), [](bool v) { return v; }) && roster["members"].toArray().size() == 5; }, "Room membership timed out");
        execute([&] {
            context = std::make_unique<Context>();
            Require(context->network->Start() && context->worker->Start(), "Media threads failed");
            webrtc::PeerConnectionFactoryDependencies dependencies;
            dependencies.env = webrtc::CreateEnvironment(); dependencies.network_thread = context->network.get(); dependencies.worker_thread = context->worker.get(); dependencies.signaling_thread = webrtc::Thread::Current();
            dependencies.adm = CreatePcmAudioDeviceModule(proof::SyntheticAudio(context->audioEvidence), std::make_shared<PcmAudioDiagnostics>());
            dependencies.audio_encoder_factory = webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus>();
            dependencies.audio_decoder_factory = webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus>();
            dependencies.video_encoder_factory = std::make_unique<MfVideoEncoderFactory>(); dependencies.video_decoder_factory = std::make_unique<MfVideoDecoderFactory>();
            webrtc::EnableMedia(dependencies); context->factory = webrtc::CreateModularPeerConnectionFactory(std::move(dependencies));
            Require(context->factory != nullptr, "Media factory failed");
            webrtc::AudioOptions options; options.echo_cancellation = options.auto_gain_control = options.noise_suppression = false;
            auto source = context->factory->CreateAudioSource(options); context->audio = context->factory->CreateAudioTrack("room-audio", source.get());
            context->captureGeneration = context->capture.Start([] { return std::make_unique<SyntheticCaptureSource>(640, 360, 30); }).get().generation;
            attachMedia = [&](size_t i, std::string id) {
                context->links[i] = CreateViewer(*context->factory, context->audio);
                auto& link = *context->links[i];
                auto send = [&, i](bool fromHost) { return [&, i, fromHost](RoomPeerSignal signal) {
                    const auto bytes = signal.sdp.size() + signal.ice.candidate.size() + 512;
                    std::lock_guard lock(mutex);
                    if (outgoing.size() >= 256 || queuedBytes + bytes > 256 * 1024) return false;
                    queuedBytes += bytes; outgoing.push_back({i, fromHost, std::move(signal)});
                    peakQueuedBytes = std::max(peakQueuedBytes, queuedBytes); peakQueuedMessages = std::max(peakQueuedMessages, outgoing.size()); return true;
                }; };
                context->hosts[i] = std::make_unique<RoomPeerNegotiation>(link.host.connection, link.host.Negotiation(), link.host.lifecycle.generation(), true, send(true));
                context->viewers[i] = std::make_unique<RoomPeerNegotiation>(link.viewer.connection, link.viewer.Negotiation(), link.viewer.lifecycle.generation(), false, send(false));
                link.host.candidateObserver = [&, i](auto* c) { if (context && context->hosts[i]) context->hosts[i]->LocalCandidate(c); };
                link.viewer.candidateObserver = [&, i](auto* c) { if (context && context->viewers[i]) context->viewers[i]->LocalCandidate(c); };
                Require(context->capture.AddViewer(context->captureGeneration, i + 1, link.host.lifecycle.generation(), [source = link.source, &slow, i](auto sample) {
                    if (i == 3 && slow) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    source->Push(*std::static_pointer_cast<SyntheticCaptureResource>(sample.resource), sample.capturedAt);
                }).get().error == HostOperationError::None, "Capture subscription failed");
                Require(context->hosts[i]->Offer(std::move(id)), "Offer startup failed");
            };
            for (size_t i = 0; i < 4; ++i) attachMedia(i, "room_media_" + std::to_string(i) + "_initial");
        });
        auto frames = [&] { for (auto& link : context->links) if (link->viewer.decodedFrames < 60) return false; return context->audioEvidence->audibleBlocks >= 30; };
        wait(frames, "Authenticated four-viewer media timed out");
        Require(context->links[0]->viewer.decodedFrames > context->links[3]->viewer.decodedFrames * 2, "Slow viewer throttled healthy media");
        slow = false;
        bool channelsReady = false;
        wait([&] { execute([&] {
            channelsReady = true;
            for (auto& link : context->links) { channelsReady &= link->viewer.channels.size() == 3; for (auto& channel : link->host.channels) channelsReady &= channel->channel->state() == webrtc::DataChannelInterface::kOpen; }
        }); return channelsReady; }, "Encrypted data channels did not open");
        execute([&] { for (auto& link : context->links) for (auto& channel : link->host.channels) Require(channel->channel->Send(webrtc::DataBuffer("screenshare-proof:" + channel->channel->label())), "Encrypted data send failed"); });
        wait([&] { bool received = true; execute([&] { for (auto& link : context->links) for (auto& channel : link->viewer.channels) received &= channel->received; }); return received; }, "Encrypted data delivery timed out");
        unsigned before = context->links[3]->viewer.decodedFrames, healthyBefore = context->links[0]->viewer.decodedFrames;
        execute([&] { Require(context->hosts[3]->Offer("room_media_3_restart", true), "Restart rejected"); });
        wait([&] { return context->links[3]->viewer.decodedFrames >= before + 30 && context->links[0]->viewer.decodedFrames >= healthyBefore + 30; }, "Media did not continue through restart");
        bool restartReady = false;
        wait([&] { execute([&] { restartReady = context->hosts[3]->ready() && context->viewers[3]->ready() && context->viewers[3]->connectionId() == "room_media_3_restart"; }); return restartReady; }, "Restart negotiation incomplete");
        execute([&] {
            Require(context->viewers[3]->Receive({RoomPeerSignal::Kind::Candidate, "room_media_3_initial", {}, {"retired-invalid-candidate", "0", 0}}), "Retired candidate did not drop safely");
            Require(context->viewers[3]->ready() && !context->hosts[3]->Offer("room_media_3_restart", true), "Retired/reused generation changed active negotiation");
        });
        Require(host.Send(QJsonDocument(QJsonObject{{"v", 2}, {"type", "peer.disconnect"}, {"roomId", hostConfig.roomId}, {"requestId", "media-kick"}, {"payload", QJsonObject{{"peerId", configs[3].selfPeerId}}}}).toJson(QJsonDocument::Compact)) == RoomSocket::SendResult::Sent, "Kick failed");
        wait([&] { return closed[3] && roster["members"].toArray().size() == 4; }, "Kick not observed");
        execute([&] {
            Require(context->capture.RemoveViewer(context->captureGeneration, 4, context->links[3]->host.lifecycle.generation()).get().error == HostOperationError::None, "Capture removal failed");
            context->hosts[3].reset(); context->viewers[3].reset(); context->links[3].reset();
        });
        RoomAdmission::Request rejoin; rejoin.origin = origin; rejoin.roomId = hostConfig.roomId; rejoin.nickname = "RejoinedViewer";
        configs[3] = admit(rejoin); ready[3] = false; closed[3] = false;
        Require(sockets[3]->Start(configs[3]), "Rejoin socket failed");
        wait([&] { return ready[3] && roster["members"].toArray().size() == 5; }, "Rejoin membership failed");
        healthyBefore = context->links[0]->viewer.decodedFrames;
        execute([&] { attachMedia(3, "room_media_3_rejoined"); });
        wait([&] { return context->links[3]->viewer.decodedFrames >= 30 && context->links[0]->viewer.decodedFrames >= healthyBefore + 30; }, "Rejoined media failed");
        unsigned total = 0;
        for (auto& link : context->links) { total += link->viewer.decodedFrames; Require(link->viewer.invalidFrames == 0, "Decoded media invalid"); }
        const auto audioBlocks = context->audioEvidence->audibleBlocks.load();
        // No caller polls this adapter: its own signaling timer must close a
        // peer whose answer never arrives, while the established peers continue.
        const auto unansweredStarted = std::chrono::steady_clock::now();
        execute([&] {
            auto cancelledLink = CreateViewer(*context->factory, context->audio);
            auto cancelled = std::make_unique<RoomPeerNegotiation>(cancelledLink->host.connection,
                cancelledLink->host.Negotiation(), cancelledLink->host.lifecycle.generation(), true,
                [](RoomPeerSignal) { return true; });
            Require(cancelled->Offer("cancelled_before_tick"), "Cancelled offer failed");
            cancelled.reset(); // A queued scheduler callback must not touch freed state.
            context->unansweredLink = CreateViewer(*context->factory, context->audio);
            auto& orphan = context->unansweredLink->host;
            context->unansweredPeer = std::make_unique<RoomPeerNegotiation>(orphan.connection,
                orphan.Negotiation(), orphan.lifecycle.generation(), true, [](RoomPeerSignal) { return true; });
            Require(context->unansweredPeer->Offer("unanswered_offer"), "Unanswered offer failed");
        });
        bool automaticallyClosed = false;
        healthyBefore = context->links[0]->viewer.decodedFrames;
        wait([&] { execute([&] { automaticallyClosed = context->unansweredPeer->closed(); }); return automaticallyClosed; }, "Automatic negotiation timeout failed");
        Require(std::chrono::steady_clock::now() - unansweredStarted >= std::chrono::seconds(20), "Unanswered peer failed before its negotiation deadline");
        Require(context->links[0]->viewer.decodedFrames > healthyBefore + 30, "Unanswered peer blocked healthy media");
        Require(host.Send(QJsonDocument(QJsonObject{{"v", 2}, {"type", "peer.leave"}, {"roomId", hostConfig.roomId}, {"requestId", "media-close"}, {"payload", QJsonObject{}}}).toJson(QJsonDocument::Compact)) == RoomSocket::SendResult::Sent, "Room close failed");
        wait([&] { return roomClosed && std::all_of(closed.begin(), closed.end(), [](bool v) { return v; }); }, "Room closure failed");
        for (auto& socket : sockets) socket->Stop(); host.Stop();
        execute([&] { context.reset(); }); executor.Stop();
        Require(candidatesSent > 0, "Room signaling did not exercise trickle ICE");
        std::cout << "{\"passed\":true,\"room_backed_media\":true,\"viewers\":4,\"decoded_frames\":" << total << ",\"opus_audible_blocks\":" << audioBlocks
                  << ",\"ice_candidates_sent\":" << candidatesSent << ",\"peak_signaling_queue_bytes\":" << peakQueuedBytes << ",\"peak_signaling_queue_messages\":" << peakQueuedMessages
                  << ",\"ice_restart\":true,\"kick_rejoin\":true,\"automatic_timeout\":true,\"cancel_before_tick\":true,\"data_channels\":12}\n";
    } catch (...) {
        for (auto& socket : sockets) if (socket) socket->Stop(); host.Stop();
        execute([&] { context.reset(); }); executor.Stop(); throw;
    }
}
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
    webrtc::InitializeLogging(std::move(logging)); webrtc::WinsockInitializer winsock;
    if (winsock.error() || !webrtc::InitializeSSL()) return 1;
    int result = 0;
    try { Require(argc == 2, "Expected loopback origin"); const QUrl origin(QString::fromLocal8Bit(argv[1])); Require(origin.scheme() == "http" && origin.host() == "127.0.0.1", "Expected numeric loopback"); Run(origin); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    webrtc::CleanupSSL(); return result;
}
