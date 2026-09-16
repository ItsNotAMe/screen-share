#include "NativeRoomRuntime.h"
#include "RoomManagedPeer.h"
#include "ViewerStreamSettings.h"
#include "api/make_ref_counted.h"
#include <map>
#include <set>

namespace screenshare::media {
using namespace std::chrono_literals;
namespace {
class ViewerPeer final : public IMediaPeer {
    MediaPeer& peer_;
    RoomPeerNegotiation& negotiation_;
    std::function<bool(RoomPeerSignal)> send_;
    std::function<void()> retire_;
public:
    ViewerPeer(MediaPeer& peer, RoomPeerNegotiation& negotiation, std::function<bool(RoomPeerSignal)> send,
               std::function<void()> retire)
        : peer_(peer), negotiation_(negotiation), send_(std::move(send)), retire_(std::move(retire)) {}
    ~ViewerPeer() override { Close(); retire_(); }
    PeerConnectionLifecycle& lifecycle() noexcept override { return peer_.lifecycle; }
    bool Poll() override { return !negotiation_.closed(); }
    bool RequestIceRestart(uint64_t) override {
        return !negotiation_.connectionId().empty() && send_({RoomPeerSignal::Kind::RestartRequest, negotiation_.connectionId(), {}, {}});
    }
    void Close() noexcept override { negotiation_.Close(); }
};
class NativeRoomRuntime final : public v2::RoomRuntime {
    struct Entry {
        uint64_t generation;
        std::unique_ptr<MediaPeer> peer;
        std::unique_ptr<RoomPeerNegotiation> negotiation;
        webrtc::scoped_refptr<CaptureVideoSource> source;
        webrtc::scoped_refptr<webrtc::RtpSenderInterface> sender;
        ViewerStreamSettings settings;
        bool retired = false, removing = false;
    };
    v2::RoomIdentity identity_;
    v2::RoomSend send_;
    NativeRoomRuntimeOptions options_;
    std::unique_ptr<MediaEngine> engine_;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio_;
    HostMediaSession capture_;
    std::future<HostOperationResult> starting_;
    uint64_t captureGeneration_ = 0, next_ = 0;
    std::map<std::string, std::unique_ptr<Entry>> peers_;
    std::set<std::string> failed_;
    // Destroy the registry before native entries it references.
    std::unique_ptr<HostPeerRegistry> owner_;
    bool stopping_ = false, stopped_ = false;
    const std::chrono::steady_clock::time_point startupDeadline_ = std::chrono::steady_clock::now() + 20s;
    std::promise<void> stoppedPromise_;
    std::shared_future<void> stoppedFuture_ = stoppedPromise_.get_future().share();
public:
    NativeRoomRuntime(v2::RoomIdentity identity, v2::RoomSend send, NativeRoomRuntimeOptions options)
        : identity_(std::move(identity)), send_(std::move(send)), options_(std::move(options)) {
        if (!options_.engine || !send_ || (identity_.host && (!options_.capture || !options_.deliver)))
            throw std::invalid_argument("Native room runtime requires media dependencies");
        ValidateStreamPreferences(options_.preferences);
        if (identity_.host) {
            starting_ = capture_.Start(options_.capture);
        } else {
            engine_ = options_.engine();
            if (!engine_) throw std::runtime_error("Native media engine creation failed");
            owner_ = std::make_unique<HostPeerRegistry>();
        }
    }
    ~NativeRoomRuntime() override {
        if (owner_) owner_->Stop(); // Final teardown fallback; normal Stop already drained.
        owner_.reset(); peers_.clear(); audio_ = nullptr; engine_.reset();
    }
    bool Ready(const std::string& id) override {
        return !stopping_ && engine_ && owner_ && !peers_.contains(id);
    }
    bool Add(const std::string& id) override {
        if (!Ready(id) || peers_.size() >= (identity_.host ? 63u : 1u)) return false;
        failed_.erase(id);
        auto entry = std::make_unique<Entry>(); entry->generation = ++next_;
        entry->peer = std::make_unique<MediaPeer>(*engine_, next_, options_.frames.get(),
            [this, id](auto channel) { if (options_.channel) options_.channel(id, std::move(channel)); }, options_.connection);
        auto* raw = entry.get();
        auto send = [this, id](auto signal) { return send_(id, std::move(signal)); };
        entry->negotiation = std::make_unique<RoomPeerNegotiation>(entry->peer->connection,
            entry->peer->Negotiation(), next_, identity_.host, send);
        entry->peer->candidateObserver = [raw](auto* candidate) { raw->negotiation->LocalCandidate(candidate); };
        if (identity_.host) {
            entry->source = webrtc::make_ref_counted<CaptureVideoSource>();
            entry->source->Configure(options_.preferences, 1);
            entry->peer->OpenHostChannels(*engine_);
            entry->sender = engine_->AttachHostMedia(*entry->peer->connection, entry->source, audio_);
        }
        peers_.emplace(id, std::move(entry));
        // The map pins native references through registry/capture retirement.
        if (identity_.host) {
            auto attachment = capture_.AddViewer(captureGeneration_, next_, next_,
                [source = raw->source, deliver = options_.deliver](auto sample) { deliver(*source, sample); });
            const auto prefix = "media_" + std::to_string(next_);
            return owner_->Add(next_, std::make_unique<RoomManagedPeer>(raw->peer->lifecycle,
                *raw->negotiation, std::move(attachment), prefix,
                [prefix](uint64_t revision) { return prefix + "_restart_" + std::to_string(revision); },
                [raw] { raw->retired = true; }));
        }
        return owner_->Add(next_, std::make_unique<ViewerPeer>(*raw->peer, *raw->negotiation,
            std::move(send), [raw] { raw->retired = true; }));
    }
    void Remove(const std::string& id) noexcept override {
        failed_.erase(id);
        const auto found = peers_.find(id);
        if (found == peers_.end() || found->second->removing) return;
        auto& entry = *found->second; entry.removing = true;
        if (owner_) owner_->Remove(entry.generation, entry.generation);
    }
    bool Receive(const std::string& id, RoomPeerSignal signal) override {
        const auto found = peers_.find(id);
        if (found == peers_.end() || found->second->removing || found->second->retired) return false;
        auto& entry = *found->second;
        if (signal.kind == RoomPeerSignal::Kind::RestartRequest && identity_.host) {
            if (signal.connectionId != entry.negotiation->connectionId()) return true;
            return owner_->RequestRestart(entry.generation, entry.generation, PeerConnectionLifecycle::Clock::now());
        }
        return entry.negotiation->Receive(std::move(signal));
    }
    std::vector<std::string> FailedPeers() const override { return {failed_.begin(), failed_.end()}; }
    void Advance() override {
        if (stopped_) return;
        if (starting_.valid() && starting_.wait_for(0ms) == std::future_status::ready) {
            const auto result = starting_.get();
            if (result.error != HostOperationError::None) throw std::runtime_error("Capture startup failed");
            captureGeneration_ = result.generation;
            owner_ = std::make_unique<HostPeerRegistry>(capture_, captureGeneration_);
        }
        if (stopping_) {
            if (starting_.valid()) return;
            if (owner_) {
                owner_->BeginStop(); owner_->Tick(PeerConnectionLifecycle::Clock::now());
                if (owner_->stopError() != HostOperationError::None && owner_->stopError() != HostOperationError::StaleGeneration) {
                    stopped_ = true; stoppedPromise_.set_exception(std::make_exception_ptr(std::runtime_error("Capture shutdown failed"))); return;
                }
                if (!owner_->stopped()) return;
            }
            owner_.reset(); peers_.clear(); audio_ = nullptr; engine_.reset();
            stopped_ = true; stoppedPromise_.set_value(); return;
        }
        if (!owner_) return;
        if (identity_.host) {
            const auto captureStatus = capture_.snapshot();
            if (captureStatus.state == HostMediaState::Failed || captureStatus.state == HostMediaState::Stopped)
                throw std::runtime_error("Capture source stopped");
            if (!engine_) {
                if (options_.engineReady && !options_.engineReady()) {
                    if (std::chrono::steady_clock::now() >= startupDeadline_) throw std::runtime_error("Capture device startup timed out");
                    return;
                }
                engine_ = options_.engine();
                if (!engine_) throw std::runtime_error("Native media engine creation failed");
                audio_ = engine_->CreateAudioTrack();
            }
        }
        owner_->Tick(PeerConnectionLifecycle::Clock::now());
        const auto delivery = identity_.host ? capture_.snapshot() : HostMediaSnapshot{};
        for (auto it = peers_.begin(); it != peers_.end();) {
            auto& entry = *it->second;
            if (entry.retired) { it = peers_.erase(it); continue; }
            auto status = owner_->snapshot(entry.generation);
            if (status && status->peerClosed && !entry.removing) failed_.insert(it->first);
            if (identity_.host && !entry.removing && !entry.negotiation->connectionId().empty() &&
                std::none_of(delivery.viewers.begin(), delivery.viewers.end(), [&](const auto& viewer) {
                    return viewer.viewer == entry.generation && viewer.connectionGeneration == entry.generation;
                })) failed_.insert(it->first);
            if (!entry.removing && identity_.host && entry.negotiation->ready() && !entry.settings.revision()) {
                if (entry.settings.Apply(*entry.sender, *entry.source, options_.preferences, 2) != SettingsApplyError::None)
                    failed_.insert(it->first);
            }
            ++it;
        }
    }
    std::shared_future<void> BeginStop() override {
        stopping_ = true;
        if (owner_) owner_->BeginStop();
        return stoppedFuture_;
    }
};
}
std::unique_ptr<v2::RoomRuntime> CreateNativeRoomRuntime(v2::RoomIdentity identity, v2::RoomSend send, NativeRoomRuntimeOptions options) {
    return std::make_unique<NativeRoomRuntime>(std::move(identity), std::move(send), std::move(options));
}
}
