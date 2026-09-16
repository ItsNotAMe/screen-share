#include "SyntheticAudio.h"
#include "media/HostMediaSession.h"
#include "media/capture/SyntheticCaptureSource.h"
#include "media/webrtc/CaptureVideoSource.h"
#include "media/webrtc/MfVideoEncoderFactory.h"
#include "media/webrtc/MfVideoDecoderFactory.h"
#include "media/webrtc/PcmAudioDeviceModule.h"
#include "api/make_ref_counted.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/win32_socket_init.h"
#include "rtc_base/logging.h"
#include "media/webrtc/MediaPeer.h"
#include "media/webrtc/RoomPeerNegotiation.h"
#include "media/RoomPeerRoster.h"
#include "media/HostPeerOwner.h"
#include "media/webrtc/RoomManagedPeer.h"
#include "room/qt/RoomSessionCoordinator.h"
#include "room/qt/RoomSignalCodec.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include "media/webrtc/MediaEngine.h"
#include <deque>
#include <array>
#include <atomic>
#include <iostream>
#include <thread>
#include <algorithm>
#include <map>
using namespace screenshare::media;
using screenshare::room::qt::RoomSocket;
using screenshare::room::qt::RoomAdmission;
using screenshare::room::qt::RoomNetwork;
using screenshare::room::qt::RoomSessionCoordinator;
using screenshare::room::qt::EncodeRoomSignal;
using screenshare::room::qt::DecodeRoomSignal;
namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
class Channel final : public webrtc::DataChannelObserver {
public:
    explicit Channel(webrtc::scoped_refptr<webrtc::DataChannelInterface> value)
        : channel(std::move(value)) { channel->RegisterObserver(this); }
    ~Channel() override { channel->UnregisterObserver(); }
    void OnStateChange() override {}
    void OnMessage(const webrtc::DataBuffer& buffer) override {
        received = std::string(buffer.data.cdata<char>(), buffer.data.size()) == "screenshare-proof:" + channel->label();
    }
    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel;
    bool received = false;
};
// Diagnostic waits only. Production coordinators observe these futures without
// blocking; all sockets/admission and their Qt events run in RoomNetwork.
struct ProofRoomSocket {
    RoomSessionCoordinator& coordinator;
    SignalingExecutor& executor;
    uint64_t& next;
    RoomSessionCoordinator::Notify notify;
    uint64_t id = 0;
    bool Start(RoomSocket::Config config) {
        Stop(); std::future<bool> opened;
        Require(executor.Post([&] { id = ++next; opened = coordinator.Open(id, std::move(config), notify); }).get().error == ExecutorError::None, "Socket open command failed");
        return opened.get();
    }
    RoomSocket::SendResult Send(QByteArray command) {
        bool accepted = false;
        auto send = [&] { accepted = coordinator.Send(id, std::move(command)); };
        if (executor.IsCurrent()) send();
        else Require(executor.Post(send).get().error == ExecutorError::None, "Socket send command failed");
        return accepted ? RoomSocket::SendResult::Sent : RoomSocket::SendResult::Backpressure;
    }
    void Stop() {
        if (!id) return;
        std::shared_future<void> closed;
        Require(executor.Post([&] { closed = coordinator.Close(id); id = 0; }).get().error == ExecutorError::None, "Socket close command failed");
        closed.get();
    }
};
struct Packet { size_t viewer; bool fromHost; RoomPeerSignal message; };
// Only evidence collection belongs to the diagnostic. Native callbacks, ICE
// lifecycle, incoming tracks/channel policy and shutdown belong to MediaPeer.
struct ObservedPeer : MediaPeer, webrtc::VideoSinkInterface<webrtc::VideoFrame> {
    ObservedPeer(MediaEngine& engine, uint64_t generation)
        : MediaPeer(engine, generation, this, [this](auto channel) {
            channels.push_back(std::make_unique<Channel>(std::move(channel)));
        }) {}
    ~ObservedPeer() override { Close(); }
    void OnFrame(const webrtc::VideoFrame& frame) override {
        auto buffer = frame.video_frame_buffer();
        auto pixels = buffer->ToI420();
        if (frame.width() != 640 || frame.height() != 360 || !pixels || pixels->DataY()[0] < 35)
            ++invalidFrames;
        ++decodedFrames;
    }
    std::vector<std::unique_ptr<Channel>> channels;
    std::atomic<unsigned> decodedFrames{0}, invalidFrames{0};
};
struct RoomMediaLink {
    RoomMediaLink(MediaEngine& engine, uint64_t& generation)
        : host(engine, ++generation), viewer(engine, ++generation) {}
    ObservedPeer host, viewer;
    webrtc::scoped_refptr<CaptureVideoSource> source = webrtc::make_ref_counted<CaptureVideoSource>();
    webrtc::scoped_refptr<webrtc::RtpSenderInterface> sender;
};
std::unique_ptr<RoomMediaLink> CreateViewer(MediaEngine& engine,
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio, uint64_t& generation) {
    auto link = std::make_unique<RoomMediaLink>(engine, generation);
    link->host.OpenHostChannels(engine);
    link->sender = engine.AttachHostMedia(*link->host.connection, link->source, std::move(audio));
    return link;
}
struct Context {
    std::unique_ptr<MediaEngine> engine;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio;
    std::shared_ptr<proof::AudioEvidence> audioEvidence = std::make_shared<proof::AudioEvidence>();
    uint64_t nextPeerGeneration = 0;
    std::array<std::unique_ptr<RoomMediaLink>, 4> links;
    std::array<std::unique_ptr<RoomPeerNegotiation>, 4> hosts, viewers;
    std::unique_ptr<RoomMediaLink> unansweredLink;
    std::unique_ptr<RoomPeerNegotiation> unansweredPeer;
    HostMediaSession capture;
    std::unique_ptr<HostPeerOwner> owner;
    std::unique_ptr<RoomPeerRoster> membership;
    std::map<std::string, size_t> peerSlots;
    std::array<unsigned, 4> incarnations{};
    std::array<std::atomic<bool>, 4> retired{};
    uint64_t captureGeneration = 0;
    void Close() {
        membership.reset();
        if (owner) { owner->Stop(); owner.reset(); }
        if (captureGeneration) { capture.Stop(captureGeneration).get(); captureGeneration = 0; }
        for (auto& p : hosts) p.reset();
        for (auto& p : viewers) p.reset();
        unansweredPeer.reset(); unansweredLink.reset();
        for (auto& p : links) p.reset();
        audio = nullptr; engine.reset();
    }
    ~Context() { Close(); }
};
void Run(const QUrl& origin) {
    RoomNetwork roomNetwork(true);
    uint64_t nextSocket = 0;
    SignalingExecutor executor;
    std::unique_ptr<RoomSessionCoordinator> coordinator;
    std::unique_ptr<Context> context;
    std::deque<Packet> incoming;
    size_t candidatesSent = 0;
    auto execute = [&](auto action) {
        if (executor.IsCurrent()) { action(); return; }
        std::exception_ptr failure;
        auto done = executor.Post([&] { try { action(); } catch (...) { failure = std::current_exception(); } });
        Require(done.get().error == ExecutorError::None, "Signaling command failed");
        if (failure) std::rethrow_exception(failure);
    };
    std::array<RoomSocket::Config, 4> configs;
    RoomSocket::Config hostConfig;
    std::array<std::unique_ptr<ProofRoomSocket>, 4> sockets;
    std::array<bool, 4> ready{};
    std::array<bool, 4> closed{};
    bool roomClosed = false;
    std::atomic<bool> slow{true};
    std::function<void(size_t, std::string)> attachMedia;
    bool hostReady = false, transportFailed = false;
    QJsonObject roster;
    uint64_t rosterGeneration = 0, rosterRevision = 0, rosterLostGeneration = 0;
    bool rosterDirty = false, rosterLost = false;
    auto receive = [&](const RoomSocket::Event& event, bool toHost, size_t index) {
        if (event.kind == RoomSocket::EventKind::Error || event.kind == RoomSocket::EventKind::Reconnecting) transportFailed = true;
        if (toHost && (event.kind == RoomSocket::EventKind::Error || event.kind == RoomSocket::EventKind::Reconnecting || event.kind == RoomSocket::EventKind::Closed)) {
            rosterLost = true; rosterLostGeneration = event.generation;
        }
        if (event.kind == RoomSocket::EventKind::Snapshot) {
            if (toHost) { hostReady = true; roster = event.value; rosterGeneration = event.generation; Require(event.revision.has_value(), "Missing roster revision"); rosterRevision = *event.revision; rosterDirty = true; } else ready[index] = true;
        }
        if (event.kind == RoomSocket::EventKind::Closed) { if (toHost) roomClosed = true; else closed[index] = true; }
        if (event.kind != RoomSocket::EventKind::Signal) return;
        if (toHost) {
            index = configs.size();
            for (size_t i = 0; i < configs.size(); ++i) if (configs[i].selfPeerId == event.value["fromPeerId"]) index = i;
        }
        if (index >= configs.size() || incoming.size() >= 256) { transportFailed = true; return; }
        auto signal = DecodeRoomSignal(event.value);
        if (!signal) { transportFailed = true; return; }
        incoming.push_back({index, !toHost, std::move(*signal)});
    };
    auto advance = [&] {
        if (context) {
            for (size_t i = 0; i < 4; ++i) if (context->retired[i].exchange(false)) {
                context->hosts[i].reset(); context->viewers[i].reset(); context->links[i].reset();
            }
            if (context->membership && rosterLost) {
                context->membership->TransportLost(rosterLostGeneration); rosterLost = false;
            }
            if (context->membership && rosterDirty) {
                std::vector<std::string> peers;
                for (const auto& member : roster["members"].toArray())
                    if (member.toObject()["role"] == "viewer" && member.toObject()["status"] == "connected") peers.push_back(member.toObject()["peerId"].toString().toStdString());
                Require(context->membership->Apply(rosterGeneration, rosterRevision, std::move(peers)) != RoomPeerRoster::Result::Invalid && context->membership->failedCount() == 0, "Roster media reconciliation failed");
                rosterDirty = false;
            }
            for (auto& packet : incoming) {
                auto& target = packet.fromHost ? context->viewers[packet.viewer] : context->hosts[packet.viewer];
                if (target) Require(target->Receive(std::move(packet.message)), "Authenticated negotiation message rejected");
            }
            incoming.clear();
            for (size_t i = 0; i < 4; ++i) if (context->hosts[i]) {
                const auto status = context->owner->snapshot(i + 1);
                if (status && !status->peerClosed) Require(!context->hosts[i]->closed() && !context->viewers[i]->closed(), "Asynchronous negotiation failed");
            }
        }
    };
    execute([&] { coordinator = std::make_unique<RoomSessionCoordinator>(executor, roomNetwork, advance); });
    ProofRoomSocket host{*coordinator, executor, nextSocket, [&](const auto& event) { receive(event, true, 0); }};
    auto wait = [&](auto predicate, const char* failure) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
        for (;;) {
            bool done = false;
            execute([&] { Require(!coordinator->failed() && !transportFailed, "Room transport failed"); done = predicate(); });
            if (done) return;
            Require(std::chrono::steady_clock::now() < deadline, failure);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    auto admit = [&](RoomAdmission::Request request) {
        auto future = roomNetwork.Admit(std::move(request));
        wait([&] { return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }, "Room admission timed out");
        auto result = future.get(); Require(result.error == RoomAdmission::Error::None && result.membership.has_value(), "Room admission failed"); return *result.membership;
    };
    try {
        RoomAdmission::Request create; create.origin = origin; create.create = true; create.nickname = "MediaHost"; create.name = "Headless media"; create.viewerLimit = 4;
        const auto admittedHost = admit(create); execute([&] { hostConfig = admittedHost; }); Require(host.Start(hostConfig), "Host socket failed");
        wait([&] { return hostReady; }, "Host snapshot timed out");
        for (size_t i = 0; i < 4; ++i) {
            RoomAdmission::Request join; join.origin = origin; join.roomId = hostConfig.roomId; join.nickname = "Viewer" + QString::number(i);
            const auto admittedViewer = admit(join); execute([&] { configs[i] = admittedViewer; });
            sockets[i] = std::make_unique<ProofRoomSocket>(*coordinator, executor, nextSocket, [&, i](const auto& event) { receive(event, false, i); });
            Require(sockets[i]->Start(configs[i]), "Viewer socket failed");
        }
        wait([&] { return std::all_of(ready.begin(), ready.end(), [](bool v) { return v; }) && roster["members"].toArray().size() == 5; }, "Room membership timed out");
        execute([&] {
            context = std::make_unique<Context>();
            context->engine = std::make_unique<MediaEngine>(
                CreatePcmAudioDeviceModule(proof::SyntheticAudio(context->audioEvidence), std::make_shared<PcmAudioDiagnostics>()),
                std::make_unique<MfVideoEncoderFactory>(), std::make_unique<MfVideoDecoderFactory>());
            context->audio = context->engine->CreateAudioTrack();
        });
        auto captureStarted = context->capture.Start([] { return std::make_unique<SyntheticCaptureSource>(640, 360, 30); });
        wait([&] { return captureStarted.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }, "Capture startup timed out");
        const auto captureResult = captureStarted.get();
        Require(captureResult.error == HostOperationError::None, "Capture startup failed");
        execute([&] {
            context->captureGeneration = captureResult.generation;
            context->owner = std::make_unique<HostPeerOwner>(executor, context->capture, context->captureGeneration);
            attachMedia = [&](size_t i, std::string id) {
                context->links[i] = CreateViewer(*context->engine, context->audio, context->nextPeerGeneration);
                auto& link = *context->links[i];
                auto send = [&, i](bool fromHost) { return [&, i, fromHost](RoomPeerSignal signal) {
                    if (signal.kind == RoomPeerSignal::Kind::Candidate) ++candidatesSent;
                    auto& socket = fromHost ? host : *sockets[i];
                    return socket.Send(EncodeRoomSignal(signal, hostConfig.roomId, fromHost ? configs[i].selfPeerId : hostConfig.selfPeerId)) == RoomSocket::SendResult::Sent;
                }; };
                context->hosts[i] = std::make_unique<RoomPeerNegotiation>(link.host.connection, link.host.Negotiation(), link.host.lifecycle.generation(), true, send(true));
                context->viewers[i] = std::make_unique<RoomPeerNegotiation>(link.viewer.connection, link.viewer.Negotiation(), link.viewer.lifecycle.generation(), false, send(false));
                link.host.candidateObserver = [&, i](auto* c) { if (context && context->hosts[i]) context->hosts[i]->LocalCandidate(c); };
                link.viewer.candidateObserver = [&, i](auto* c) { if (context && context->viewers[i]) context->viewers[i]->LocalCandidate(c); };
                auto attachment = context->capture.AddViewer(context->captureGeneration, i + 1, link.host.lifecycle.generation(), [source = link.source, &slow, i](auto sample) {
                    if (i == 3 && slow) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    source->Push(*std::static_pointer_cast<SyntheticCaptureResource>(sample.resource), sample.capturedAt);
                });
                Require(context->owner->Add(i + 1, std::make_unique<RoomManagedPeer>(link.host.lifecycle, *context->hosts[i],
                    std::move(attachment), std::move(id),
                    [i, generation = link.host.lifecycle.generation()](uint64_t revision) { return "room_media_" + std::to_string(i) + "_g" + std::to_string(generation) + "_restart_" + std::to_string(revision); },
                    [state = context.get(), i] { state->retired[i] = true; })), "Scheduled peer ownership failed");
            };
            context->membership = std::make_unique<RoomPeerRoster>([&](const std::string& peer) {
                for (size_t i = 0; i < configs.size(); ++i) if (configs[i].selfPeerId.toStdString() == peer) {
                    attachMedia(i, "room_media_" + std::to_string(i) + "_" + std::to_string(++context->incarnations[i]));
                    context->peerSlots.emplace(peer, i); return true;
                }
                return false;
            }, [state = context.get()](const std::string& peer) {
                const auto slot = state->peerSlots.find(peer);
                if (slot == state->peerSlots.end()) return;
                const auto i = slot->second;
                state->owner->Remove(i + 1, state->links[i]->host.lifecycle.generation());
                state->peerSlots.erase(slot);
            });
            rosterDirty = true;
        });
        auto frames = [&] { for (auto& link : context->links) if (!link || link->viewer.decodedFrames < 60) return false; return context->audioEvidence->audibleBlocks >= 30; };
        wait(frames, "Authenticated four-viewer media timed out");
        execute([&] { Require(context->links[0]->viewer.decodedFrames > context->links[3]->viewer.decodedFrames * 2, "Slow viewer throttled healthy media"); });
        slow = false;
        bool channelsReady = false;
        wait([&] { execute([&] {
            channelsReady = true;
            for (auto& link : context->links) { channelsReady &= link->viewer.channels.size() == 3; for (auto& channel : link->host.channels) channelsReady &= channel->channel->state() == webrtc::DataChannelInterface::kOpen; }
        }); return channelsReady; }, "Encrypted data channels did not open");
        execute([&] { for (auto& link : context->links) for (auto& channel : link->host.channels) Require(channel->channel->Send(webrtc::DataBuffer("screenshare-proof:" + channel->channel->label())), "Encrypted data send failed"); });
        wait([&] { bool received = true; execute([&] { for (auto& link : context->links) for (auto& channel : link->viewer.channels) received &= channel->received; }); return received; }, "Encrypted data delivery timed out");
        // Lose one room socket while the other viewers remain connected. The
        // authoritative reconnecting roster must retire its media incarnation.
        sockets[3]->Stop();
        wait([&] { return !context->links[3]; }, "Socket loss did not retire its media peer");
        execute([&] { ready[3] = false; });
        Require(sockets[3]->Start(configs[3]), "Socket reconnect failed");
        wait([&] { return ready[3] && context->links[3] && context->links[3]->viewer.decodedFrames >= 30; }, "Socket reconnect did not rebuild media");
        unsigned before = 0, healthyBefore = 0;
        std::string expectedRestartId;
        execute([&] {
            before = context->links[3]->viewer.decodedFrames; healthyBefore = context->links[0]->viewer.decodedFrames;
            expectedRestartId = "room_media_3_g" + std::to_string(context->links[3]->host.lifecycle.generation()) + "_restart_1";
        });
        execute([&] { Require(context->owner->RequestRestart(4, context->links[3]->host.lifecycle.generation()), "Restart rejected"); });
        // No scenario pumping or status queries during recovery. The backend
        // must relay offer/answer/ICE and complete negotiation autonomously.
        std::this_thread::sleep_for(std::chrono::seconds(5));
        execute([&] { Require(context->hosts[3]->ready() && context->viewers[3]->connectionId() == expectedRestartId, "Recovery depended on caller pumping"); });
        wait([&] { return context->links[3]->viewer.decodedFrames >= before + 30 && context->links[0]->viewer.decodedFrames >= healthyBefore + 30; }, "Media did not continue through restart");
        bool restartReady = false;
        wait([&] { execute([&] { restartReady = context->hosts[3]->ready() && context->viewers[3]->ready() && context->viewers[3]->connectionId() == expectedRestartId; }); return restartReady; }, "Restart negotiation incomplete");
        execute([&] {
            Require(context->viewers[3]->Receive({RoomPeerSignal::Kind::Candidate, "room_media_3_1", {}, {"retired-invalid-candidate", "0", 0}}), "Retired candidate did not drop safely");
            Require(context->viewers[3]->ready() && !context->hosts[3]->Offer(expectedRestartId, true), "Retired/reused generation changed active negotiation");
        });
        Require(host.Send(QJsonDocument(QJsonObject{{"v", 2}, {"type", "peer.disconnect"}, {"roomId", hostConfig.roomId}, {"requestId", "media-kick"}, {"payload", QJsonObject{{"peerId", configs[3].selfPeerId}}}}).toJson(QJsonDocument::Compact)) == RoomSocket::SendResult::Sent, "Kick failed");
        wait([&] { return closed[3] && roster["members"].toArray().size() == 4 && !context->links[3]; }, "Kick not observed");
        execute([&] {
            Require(!context->links[3] && context->membership->activeCount() == 3, "Roster did not remove kicked media");
        });
        RoomAdmission::Request rejoin; rejoin.origin = origin; rejoin.roomId = hostConfig.roomId; rejoin.nickname = "RejoinedViewer";
        const auto rejoined = admit(rejoin);
        execute([&] { configs[3] = rejoined; ready[3] = false; closed[3] = false; });
        Require(sockets[3]->Start(configs[3]), "Rejoin socket failed");
        wait([&] { return ready[3] && roster["members"].toArray().size() == 5; }, "Rejoin membership failed");
        execute([&] { healthyBefore = context->links[0]->viewer.decodedFrames; });
        wait([&] { return context->links[3] && context->links[3]->viewer.decodedFrames >= 30 && context->links[0]->viewer.decodedFrames >= healthyBefore + 30; }, "Rejoined media failed");
        unsigned total = 0;
        execute([&] { for (auto& link : context->links) { total += link->viewer.decodedFrames; Require(link->viewer.invalidFrames == 0, "Decoded media invalid"); } });
        const auto audioBlocks = context->audioEvidence->audibleBlocks.load();
        // No caller polls this adapter: its own signaling timer must close a
        // peer whose answer never arrives, while the established peers continue.
        const auto unansweredStarted = std::chrono::steady_clock::now();
        execute([&] {
            auto cancelledLink = CreateViewer(*context->engine, context->audio, context->nextPeerGeneration);
            auto cancelled = std::make_unique<RoomPeerNegotiation>(cancelledLink->host.connection,
                cancelledLink->host.Negotiation(), cancelledLink->host.lifecycle.generation(), true,
                [](RoomPeerSignal) { return true; });
            Require(cancelled->Offer("cancelled_before_tick"), "Cancelled offer failed");
            cancelled.reset(); // A queued scheduler callback must not touch freed state.
            context->unansweredLink = CreateViewer(*context->engine, context->audio, context->nextPeerGeneration);
            auto& orphan = context->unansweredLink->host;
            context->unansweredPeer = std::make_unique<RoomPeerNegotiation>(orphan.connection,
                orphan.Negotiation(), orphan.lifecycle.generation(), true, [](RoomPeerSignal) { return true; });
            Require(context->unansweredPeer->Offer("unanswered_offer"), "Unanswered offer failed");
        });
        bool automaticallyClosed = false;
        execute([&] { healthyBefore = context->links[0]->viewer.decodedFrames; });
        wait([&] { execute([&] { automaticallyClosed = context->unansweredPeer->closed(); }); return automaticallyClosed; }, "Automatic negotiation timeout failed");
        Require(std::chrono::steady_clock::now() - unansweredStarted >= std::chrono::seconds(20), "Unanswered peer failed before its negotiation deadline");
        execute([&] { Require(context->links[0]->viewer.decodedFrames > healthyBefore + 30, "Unanswered peer blocked healthy media"); });
        Require(host.Send(QJsonDocument(QJsonObject{{"v", 2}, {"type", "peer.leave"}, {"roomId", hostConfig.roomId}, {"requestId", "media-close"}, {"payload", QJsonObject{}}}).toJson(QJsonDocument::Compact)) == RoomSocket::SendResult::Sent, "Room close failed");
        wait([&] { return roomClosed && std::all_of(closed.begin(), closed.end(), [](bool v) { return v; }); }, "Room closure failed");
        execute([&] { Require(context->membership->activeCount() == 0 && context->peerSlots.empty(), "Room close retained media peers"); });
        for (auto& socket : sockets) socket->Stop(); host.Stop();
        std::shared_future<HostOperationError> mediaStopped;
        execute([&] { mediaStopped = context->owner->BeginStop(); });
        Require(mediaStopped.wait_for(std::chrono::seconds(10)) == std::future_status::ready && mediaStopped.get() == HostOperationError::None, "Asynchronous media shutdown failed");
        RoomSessionCoordinator::Stats dispatchStats;
        std::shared_future<void> transportStopped;
        execute([&] { dispatchStats = coordinator->stats(); transportStopped = coordinator->Stop(); context.reset(); coordinator.reset(); });
        transportStopped.get(); executor.Stop();
        Require(candidatesSent > 0, "Room signaling did not exercise trickle ICE");
        std::cout << "{\"passed\":true,\"room_backed_media\":true,\"viewers\":4,\"decoded_frames\":" << total << ",\"opus_audible_blocks\":" << audioBlocks
                  << ",\"ice_candidates_sent\":" << candidatesSent << ",\"peak_signaling_queue_bytes\":" << dispatchStats.peakSendBytes << ",\"peak_signaling_queue_messages\":" << dispatchStats.peakSendOperations
                  << ",\"autonomous_dispatch\":true,\"owned_network_loop\":true,\"roster_driven_peers\":true,\"socket_reconnect\":true,\"ice_restart\":true,\"kick_rejoin\":true,\"automatic_timeout\":true,\"cancel_before_tick\":true,\"data_channels\":12}\n";
    } catch (...) {
        for (auto& socket : sockets) if (socket) socket->Stop(); host.Stop();
        std::shared_future<void> transportStopped;
        execute([&] { transportStopped = coordinator->Stop(); context.reset(); coordinator.reset(); });
        transportStopped.get(); executor.Stop(); throw;
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
