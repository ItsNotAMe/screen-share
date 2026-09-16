#include "api/RoomSession.h"
#include "media/HostMediaSession.h"
#include "media/webrtc/MediaEngine.h"
#include "media/webrtc/MediaPeer.h"
#include "media/webrtc/RoomPeerNegotiation.h"
#include "media/webrtc/CaptureVideoSource.h"
#include "media/webrtc/MfVideoEncoderFactory.h"
#include "media/webrtc/MfVideoDecoderFactory.h"
#include "media/webrtc/PcmAudioDeviceModule.h"
#include "SyntheticAudio.h"
#include "api/make_ref_counted.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/win32_socket_init.h"
#include "rtc_base/logging.h"
#include <QCoreApplication>
#include <array>
#include <map>
#include <iostream>
using namespace screenshare::media;
using namespace screenshare::v2;
using namespace std::chrono_literals;
void Check(bool ok) { if (!ok) throw std::runtime_error("Public session proof failed"); }
struct Evidence : webrtc::VideoSinkInterface<webrtc::VideoFrame> {
    std::atomic<unsigned> frames{0}, invalid{0}, destroyed{0};
    std::shared_ptr<proof::AudioEvidence> audio = std::make_shared<proof::AudioEvidence>();
    void OnFrame(const webrtc::VideoFrame& frame) override {
        auto pixels = frame.video_frame_buffer()->ToI420();
        if (frame.width() != 640 || frame.height() != 360 || !pixels || pixels->DataY()[0] < 35) ++invalid;
        ++frames;
    }
};
// Diagnostic source/sink composition; the public owner drives every operation.
// No caller executor access, transport pumping or SDP relay is available here.
class Runtime final : public RoomRuntime {
    struct Entry {
        uint64_t generation;
        std::unique_ptr<MediaPeer> peer;
        std::unique_ptr<RoomPeerNegotiation> negotiation;
        webrtc::scoped_refptr<CaptureVideoSource> source;
        std::future<HostOperationResult> attaching, removing;
        bool retired = false;
    };
    RoomIdentity identity_;
    RoomSend send_;
    std::shared_ptr<Evidence> evidence_;
    std::unique_ptr<MediaEngine> engine_;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio_;
    HostMediaSession capture_;
    std::future<HostOperationResult> starting_, stoppingCapture_;
    uint64_t captureGeneration_ = 0, next_ = 0;
    std::map<std::string, std::unique_ptr<Entry>> peers_;
    bool stopping_ = false, stopped_ = false;
    std::promise<void> stoppedPromise_;
    std::shared_future<void> stoppedFuture_ = stoppedPromise_.get_future().share();
public:
    Runtime(RoomIdentity identity, RoomSend send, std::shared_ptr<Evidence> evidence)
        : identity_(std::move(identity)), send_(std::move(send)), evidence_(std::move(evidence)) {
        engine_ = std::make_unique<MediaEngine>(CreatePcmAudioDeviceModule(proof::SyntheticAudio(evidence_->audio),
            std::make_shared<PcmAudioDiagnostics>()), std::make_unique<MfVideoEncoderFactory>(), std::make_unique<MfVideoDecoderFactory>());
        audio_ = engine_->CreateAudioTrack();
        if (identity_.host) starting_ = capture_.Start([] { return std::make_unique<SyntheticCaptureSource>(640, 360, 30); });
    }
    ~Runtime() override { ++evidence_->destroyed; }
    bool Ready(const std::string& id) override { return !stopping_ && !peers_.contains(id) && (!identity_.host || captureGeneration_); }
    bool Add(const std::string& id) override {
        auto entry = std::make_unique<Entry>();
        entry->generation = ++next_;
        entry->peer = std::make_unique<MediaPeer>(*engine_, next_, evidence_.get(), MediaPeer::ChannelReady{});
        auto* raw = entry.get();
        entry->negotiation = std::make_unique<RoomPeerNegotiation>(entry->peer->connection, entry->peer->Negotiation(), next_, identity_.host,
            [this, id](auto signal) { return send_(id, std::move(signal)); });
        entry->peer->candidateObserver = [raw](auto* candidate) { raw->negotiation->LocalCandidate(candidate); };
        if (identity_.host) {
            entry->source = webrtc::make_ref_counted<CaptureVideoSource>();
            entry->peer->OpenHostChannels(*engine_);
            engine_->AttachHostMedia(*entry->peer->connection, entry->source, audio_);
            entry->attaching = capture_.AddViewer(captureGeneration_, next_, next_, [source = entry->source](auto sample) {
                source->Push(*std::static_pointer_cast<SyntheticCaptureResource>(sample.resource), sample.capturedAt);
            });
        }
        return peers_.emplace(id, std::move(entry)).second;
    }
    void Remove(const std::string& id) noexcept override {
        auto found = peers_.find(id); if (found == peers_.end() || found->second->retired) return;
        auto& entry = *found->second; entry.retired = true; entry.negotiation->Close();
        if (identity_.host) entry.removing = capture_.RemoveViewer(captureGeneration_, entry.generation, entry.generation);
    }
    bool Receive(const std::string& id, RoomPeerSignal signal) override {
        auto found = peers_.find(id);
        return found != peers_.end() && !found->second->retired && found->second->negotiation->Receive(std::move(signal));
    }
    void Advance() override {
        if (stopped_) return;
        if (starting_.valid() && starting_.wait_for(0ms) == std::future_status::ready) {
            auto result = starting_.get(); Check(result.error == HostOperationError::None); captureGeneration_ = result.generation;
        }
        if (stopping_) {
            if (starting_.valid()) return;
            if (captureGeneration_ && !stoppingCapture_.valid()) stoppingCapture_ = capture_.Stop(captureGeneration_);
            if (stoppingCapture_.valid()) {
                if (stoppingCapture_.wait_for(0ms) != std::future_status::ready) return;
                Check(stoppingCapture_.get().error == HostOperationError::None); captureGeneration_ = 0;
            }
            peers_.clear(); audio_ = nullptr; engine_.reset(); stopped_ = true; stoppedPromise_.set_value(); return;
        }
        for (auto it = peers_.begin(); it != peers_.end();) {
            auto& entry = *it->second;
            if (entry.retired) {
                if (!entry.removing.valid() || entry.removing.wait_for(0ms) == std::future_status::ready) { it = peers_.erase(it); continue; }
            } else if (entry.attaching.valid() && entry.attaching.wait_for(0ms) == std::future_status::ready) {
                Check(entry.attaching.get().error == HostOperationError::None);
                Check(entry.negotiation->Offer("session_peer_" + std::to_string(entry.generation)));
            }
            ++it;
        }
    }
    std::shared_future<void> BeginStop() override {
        stopping_ = true;
        for (auto& [id, entry] : peers_) entry->negotiation->Close();
        return stoppedFuture_;
    }
};
template<class Predicate> void Wait(Predicate condition) {
    auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!condition()) { Check(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(5ms); }
}
template<class Future> auto Get(Future& future) { Check(future.wait_for(20s) == std::future_status::ready); return future.get(); }
class HeldRuntime final : public RoomRuntime {
    std::shared_future<void> barrier_;
    std::shared_ptr<Evidence> evidence_;
public:
    HeldRuntime(std::shared_future<void> barrier, std::shared_ptr<Evidence> evidence)
        : barrier_(std::move(barrier)), evidence_(std::move(evidence)) {}
    ~HeldRuntime() override { ++evidence_->destroyed; }
    void Advance() override {}
    bool Ready(const std::string&) override { return true; }
    bool Add(const std::string&) override { return true; }
    void Remove(const std::string&) noexcept override {}
    bool Receive(const std::string&, RoomPeerSignal) override { return false; }
    std::shared_future<void> BeginStop() override { return barrier_; }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    webrtc::LoggingConfig logging; logging.set_min_severity(webrtc::LS_NONE); logging.set_debug_severity(webrtc::LS_NONE); logging.set_log_to_stderr(false);
    webrtc::InitializeLogging(std::move(logging)); webrtc::WinsockInitializer winsock;
    if (winsock.error() || !webrtc::InitializeSSL()) return 1;
    int result = 0;
    try {
        Check(argc == 2);
        auto hostEvidence = std::make_shared<Evidence>();
        auto factory = [](auto evidence) { return [evidence](auto identity, auto send) { return std::make_unique<Runtime>(identity, std::move(send), evidence); }; };
        RoomOptions hostOptions; hostOptions.origin = argv[1]; hostOptions.host = true; hostOptions.nickname = "FacadeHost"; hostOptions.name = "Facade media";
        RoomSession host(factory(hostEvidence), true);
        auto start = host.Start(hostOptions); auto duplicate = host.Start(hostOptions);
        Check(Get(duplicate).error == RoomError::Busy); Check(Get(start).error == RoomError::None);
        std::array<std::unique_ptr<RoomSession>, 4> viewers;
        std::array<std::shared_ptr<Evidence>, 4> evidence;
        auto options = hostOptions; options.host = false; options.roomId = host.Status().roomId;
        for (size_t i = 0; i < viewers.size(); ++i) {
            evidence[i] = std::make_shared<Evidence>(); options.nickname = "FacadeViewer" + std::to_string(i);
            viewers[i] = std::make_unique<RoomSession>(factory(evidence[i]), true);
            auto joining = viewers[i]->Start(options); Check(Get(joining).error == RoomError::None);
        }
        Wait([&] { for (auto& value : evidence) if (value->frames < 45 || value->audio->audibleBlocks < 20) return false; return host.Status().activePeers == 4; });
        auto stopViewer = viewers[3]->Stop(); Get(stopViewer);
        Check(evidence[3]->destroyed == 1);
        Wait([&] { return host.Status().activePeers == 3; });
        auto stopping = host.Stop(); auto sameStop = host.Stop(); Get(stopping); Get(sameStop);
        Check(hostEvidence->destroyed == 1 && host.Status().phase == RoomPhase::Stopped);
        for (auto& viewer : viewers) { auto done = viewer->Stop(); Get(done); }
        unsigned frames = 0;
        for (auto& value : evidence) { Check(value->invalid == 0 && value->destroyed == 1); frames += value->frames; }
        // Cancellation is ordered even when the admission command has not run.
        RoomSession cancelled(factory(std::make_shared<Evidence>()), true);
        auto pending = cancelled.Start(hostOptions); auto cancelledStop = cancelled.Stop(); Get(cancelledStop);
        Check(Get(pending).error == RoomError::Cancelled);
        std::promise<void> release; auto barrier = release.get_future().share();
        auto heldEvidence = std::make_shared<Evidence>();
        RoomSession held([&](auto, auto) { return std::make_unique<HeldRuntime>(barrier, heldEvidence); }, true);
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
        auto rejected = secure.Start(hostOptions); Check(Get(rejected).error == RoomError::Admission);
        auto secureStop = secure.Stop(); Get(secureStop);
        std::cout << "{\"passed\":true,\"public_session\":true,\"viewers\":4,\"decoded_frames\":" << frames << ",\"cancel_admission\":true,\"coalesced_stop\":true,\"media_drain_barrier\":true,\"production_tls_required\":true}\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    webrtc::CleanupSSL(); return result;
}
