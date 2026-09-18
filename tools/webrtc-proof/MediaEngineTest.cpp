#include "ProofPeer.h"
#include "media/webrtc/MediaEngine.h"
#include "media/webrtc/MediaNetworkPolicy.h"
#include "rtc_base/experiments/rate_control_settings.h"
#include "media/webrtc/MediaPeer.h"
#include "media/SignalingExecutor.h"
using namespace screenshare::media;
using namespace proofmedia;
int main() {
    webrtc::WinsockInitializer winsock;
    if (winsock.error() || !webrtc::InitializeSSL()) return 1;
    int status = 0;
    try {
        MediaNetworkPolicy policy;
        const webrtc::RateControlSettings rates(policy);
        Require(rates.UseCongestionWindow() && rates.UseCongestionWindowPushback() &&
            !rates.UseCongestionWindowDropFrameOnly() && rates.GetCongestionWindowAdditionalTimeMs() == 50 &&
            rates.CongestionWindowMinPushbackTargetBitrateBps() == 30000,
            "Pinned SDK did not parse the bounded in-flight/bitrate-pushback policy");
        SignalingExecutor executor;
        std::exception_ptr failure;
        auto operation = executor.Post([&] {
            try {
                bool rejected = false;
                try { MediaEngine invalid(nullptr, nullptr, nullptr); }
                catch (const std::invalid_argument&) { rejected = true; }
                Require(rejected, "Missing engine dependencies accepted");
                for (bool throws : {false, true}) {
                    auto evidence = std::make_shared<proof::AudioEvidence>();
                    MediaEngine engine(CreatePcmAudioDeviceModule(proof::SyntheticAudio(evidence),
                        std::make_shared<PcmAudioDiagnostics>()),
                        std::make_unique<MfVideoEncoderFactory>(), std::make_unique<MfVideoDecoderFactory>(), {},
                        [throws]() -> std::unique_ptr<webrtc::RtcEventLogOutput> {
                            if (throws) throw std::runtime_error("Injected output failure");
                            return nullptr;
                        });
                    Peer observer;
                    rejected = false;
                    try { engine.CreatePeer(observer); }
                    catch (const std::runtime_error&) { rejected = true; }
                    Require(rejected, "Requested log failure silently accepted");
                }
                for (int cycle = 0; cycle < 10; ++cycle) {
                    auto evidence = std::make_shared<proof::AudioEvidence>();
                    MediaEngine engine(CreatePcmAudioDeviceModule(proof::SyntheticAudio(evidence),
                        std::make_shared<PcmAudioDiagnostics>()),
                        std::make_unique<MfVideoEncoderFactory>(), std::make_unique<MfVideoDecoderFactory>());
                    bool wrongThreadRejected = false;
                    std::thread wrongThread([&] {
                        try { engine.CreateAudioTrack(); }
                        catch (const std::logic_error&) { wrongThreadRejected = true; }
                    });
                    wrongThread.join();
                    Require(wrongThreadRejected, "Engine accepted foreign-thread operation");
                    Peer peer;
                    webrtc::PeerConnectionInterface::RTCConfiguration config;
                    config.type = webrtc::PeerConnectionInterface::kRelay;
                    peer.connection = engine.CreatePeer(peer, config);
                    Require(peer.connection->GetConfiguration().type == config.type, "ICE policy was lost");
                    auto audio = engine.CreateAudioTrack();
                    rejected = false;
                    try { engine.AttachHostMedia(*peer.connection, nullptr, audio); }
                    catch (const std::invalid_argument&) { rejected = true; }
                    Require(rejected && peer.connection->GetSenders().empty(), "Invalid attachment mutated peer");
                    auto source = webrtc::make_ref_counted<SyntheticVideo>();
                    auto sender = engine.AttachHostMedia(*peer.connection, source, audio);
                    Require(sender && peer.connection->GetSenders().size() == 2, "Host media missing");
                    // Reusing the audio track fails after video attachment. The
                    // engine must roll that video back instead of transmitting it.
                    rejected = false;
                    try { engine.AttachHostMedia(*peer.connection, source, audio); }
                    catch (const std::runtime_error&) { rejected = true; }
                    size_t activeTracks = 0;
                    for (auto& attached : peer.connection->GetSenders()) if (attached->track()) ++activeTracks;
                    Require(rejected && activeTracks == 2, "Partial media attachment was not rolled back");
                    auto channels = engine.CreateHostChannels(*peer.connection);
                    Require(channels[0]->label() == "control" && channels[0]->ordered()
                        && !channels[0]->maxRetransmitsOpt().has_value(), "Control channel must be reliable");
                    for (size_t i = 1; i < channels.size(); ++i)
                        Require(!channels[i]->ordered() && channels[i]->maxRetransmitsOpt() == 0,
                            "Transient channel must not queue retransmissions");
                    peer.Shutdown();
                    size_t acceptedChannels = 0;
                    MediaPeer managed(engine, cycle + 1, nullptr, [&](auto) { ++acceptedChannels; });
                    managed.OpenHostChannels(engine);
                    Require(acceptedChannels == 3, "Managed host channels missing");
                    auto& callbacks = static_cast<webrtc::PeerConnectionObserver&>(managed);
                    for (const auto& label : {"unknown", "control", "telemetry"}) {
                        webrtc::DataChannelInit options;
                        auto extra = managed.connection->CreateDataChannelOrError(label, &options);
                        Require(extra.ok(), "Adversarial channel setup failed");
                        auto channel = extra.MoveValue();
                        callbacks.OnDataChannel(channel);
                        Require(channel->state() == webrtc::DataChannelInterface::kClosing ||
                            channel->state() == webrtc::DataChannelInterface::kClosed, "Unexpected channel accepted");
                    }
                    Require(acceptedChannels == 3, "Invalid channel reached application");
                    size_t remoteChannels = 0;
                    MediaPeer remote(engine, cycle + 100, nullptr, [&](auto) { ++remoteChannels; });
                    auto& remoteCallbacks = static_cast<webrtc::PeerConnectionObserver&>(remote);
                    for (const auto& label : {"unknown", "control", "input-state", "telemetry"}) {
                        webrtc::DataChannelInit options;
                        if (std::string(label) == "control") { options.ordered = false; options.maxRetransmits = 0; }
                        auto extra = remote.connection->CreateDataChannelOrError(label, &options);
                        Require(extra.ok(), "Invalid-policy channel setup failed");
                        auto channel = extra.MoveValue();
                        remoteCallbacks.OnDataChannel(channel);
                        Require(channel->state() == webrtc::DataChannelInterface::kClosing ||
                            channel->state() == webrtc::DataChannelInterface::kClosed, "Wrong channel policy accepted");
                    }
                    Require(remoteChannels == 0, "Wrong policy reached application before capacity limit");
                    remote.OpenHostChannels(engine);
                    Require(remoteChannels == 3, "Rejected channels consumed valid channel capacity");
                    managed.Close(); managed.Close();
                    callbacks.OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::kIceConnectionConnected);
                    Require(managed.lifecycle.state() == PeerLifecycleState::Closed, "Late callback revived peer");
                }
            } catch (...) { failure = std::current_exception(); }
        });
        Require(operation.get().error == ExecutorError::None, "Engine test dispatch failed");
        executor.Stop();
        if (failure) std::rethrow_exception(failure);
        std::cout << "{\"passed\":true,\"engine_lifecycles\":10}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; status = 1; }
    webrtc::CleanupSSL();
    return status;
}
