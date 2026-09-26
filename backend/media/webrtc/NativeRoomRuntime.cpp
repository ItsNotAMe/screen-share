#include "NativeRoomRuntime.h"
#include "RoomManagedPeer.h"
#include "ViewerStreamSettings.h"
#include "TransportSendRate.h"
#include "ReceiverTelemetryChannel.h"
#include "InputChannels.h"
#include "media/capture/SwitchableCaptureSource.h"
#include "media/ProcessDiagnostics.h"
#include "api/make_ref_counted.h"
#include "api/transport/bitrate_settings.h"
#include <map>
#include <set>
#include <atomic>
#include <deque>

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
        uint64_t attemptedRevision = 0;
        bool settingsRejected = false;
        SettingsApplyError settingsError = SettingsApplyError::None;
        bool retired = false, removing = false;
        // Capture callbacks run on a separate delivery worker. Do not scale or
        // submit frames for an unconnected peer on the shared capture device.
        std::shared_ptr<std::atomic<bool>> deliverFrames = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<DiagnosticHistory> events = std::make_shared<DiagnosticHistory>();
        DiagnosticHistory mediaHistory{60, false};
        std::chrono::steady_clock::time_point nextMediaSample{};
        std::shared_ptr<TransportSendRate> sendRate = std::make_shared<TransportSendRate>();
        std::string statsConnection;
        // Destroy/unregister observer before native peer/channel destruction.
        std::unique_ptr<ReceiverTelemetryChannel> telemetry;
        std::unique_ptr<InputChannels> input;
    };
    v2::RoomIdentity identity_;
    v2::RoomSend send_;
    NativeRoomRuntimeOptions options_;
    std::shared_ptr<input::Service> input_;
    std::unique_ptr<MediaEngine> engine_;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio_;
    HostMediaSession capture_;
    std::shared_ptr<CaptureSwitchControl> captureSwitch_;
    std::future<HostOperationResult> starting_;
    uint64_t captureGeneration_ = 0, next_ = 0;
    uint64_t settingsRevision_ = 2;
    size_t allocatedViewers_ = 0;
    std::map<std::string, std::unique_ptr<Entry>> peers_;
    std::set<std::string> failed_;
    std::deque<PeerConnectionStatus> retiredConnections_;
    DiagnosticHistory performance_{60, false};
    std::chrono::steady_clock::time_point lastAdvance_{}, nextPerformanceSample_{};
    double maxAdvanceGapMs_ = 0;
    ProcessDiagnostics processDiagnostics_;
    // Destroy the registry before native entries it references.
    std::unique_ptr<HostPeerRegistry> owner_;
    bool stopping_ = false, stopped_ = false;
    std::chrono::steady_clock::time_point startupDeadline_ = std::chrono::steady_clock::now() + 20s;
    std::promise<void> stoppedPromise_;
    std::shared_future<void> stoppedFuture_ = stoppedPromise_.get_future().share();
public:
    NativeRoomRuntime(v2::RoomIdentity identity, v2::RoomSend send, NativeRoomRuntimeOptions options)
        : identity_(std::move(identity)), send_(std::move(send)), options_(std::move(options)) {
        if (!options_.engine || !send_ || (identity_.host && (!options_.capture || !options_.deliver)))
            throw std::invalid_argument("Native room runtime requires media dependencies");
        ValidateStreamPreferences(options_.preferences);
        if (!options_.diagnostics) options_.diagnostics = std::make_shared<DiagnosticHistory>();
        input_ = std::make_shared<input::Service>(identity_.host, options_.inputSink);
        input_->Configure(options_.initialCapture.kind == CaptureKind::Window ? uint8_t(input::Mouse | input::Gamepad) : uint8_t(7), 0);
        if (identity_.host) {
            if (options_.captureForSelection) {
                captureSwitch_ = std::make_shared<CaptureSwitchControl>(options_.initialCapture);
                starting_ = capture_.Start([initial = options_.capture, control = captureSwitch_] {
                    return std::make_unique<SwitchableCaptureSource>(initial, control);
                });
            } else starting_ = capture_.Start(options_.capture);
        } else {
            engine_ = options_.engine();
            if (!engine_) throw std::runtime_error("Native media engine creation failed");
            owner_ = std::make_unique<HostPeerRegistry>();
        }
    }
    ~NativeRoomRuntime() override {
        input_->Close();
        if (owner_) owner_->Stop(); // Final teardown fallback; normal Stop already drained.
        owner_.reset(); peers_.clear(); audio_ = nullptr; engine_.reset();
    }
    bool Ready(const std::string& id) override {
        return !stopping_ && engine_ && owner_ && !peers_.contains(id);
    }
    bool Add(const std::string& id) override {
        if (!Ready(id) || peers_.size() >= (identity_.host ? 63u : 1u)) return false;
        failed_.erase(id);
        std::erase_if(retiredConnections_, [&](const auto& value) { return value.peerId == id; });
        auto entry = std::make_unique<Entry>(); entry->generation = ++next_;
        const auto trace = entry->events;
        const char* constructionStage = "peer-setup";
        try {
        entry->telemetry = std::make_unique<ReceiverTelemetryChannel>(identity_.host, options_.presentation);
        if (!options_.channel) entry->input = std::make_unique<InputChannels>(input_, id);
        auto* raw = entry.get();
        entry->events->Event("peer-added", next_);
        constructionStage = "peer-connection-create";
        entry->peer = std::make_unique<MediaPeer>(*engine_, next_, options_.frames.get(),
            [this, id, raw](auto channel) {
                if (channel->label() == "telemetry") raw->telemetry->Attach(std::move(channel));
                else if (raw->input) raw->input->Attach(std::move(channel));
                else if (options_.channel) options_.channel(id, std::move(channel));
            }, options_.connection, entry->events);
        auto send = [this, id](auto signal) { return send_(id, std::move(signal)); };
        entry->negotiation = std::make_unique<RoomPeerNegotiation>(entry->peer->connection,
            entry->peer->Negotiation(), next_, identity_.host, send, entry->events);
        entry->peer->candidateObserver = [raw](auto* candidate) { raw->negotiation->LocalCandidate(candidate); };
        if (identity_.host) {
            constructionStage = "host-track-attach";
            entry->source = webrtc::make_ref_counted<CaptureVideoSource>();
            entry->source->Configure(options_.preferences, settingsRevision_ - 1);
            entry->peer->OpenHostChannels(*engine_);
            entry->sender = engine_->AttachHostMedia(*entry->peer->connection, entry->source, audio_);
        }
        peers_.emplace(id, std::move(entry));
        constructionStage = "capture-subscribe";
        // The map pins native references through registry/capture retirement.
        if (identity_.host) {
            auto attachment = capture_.AddViewer(captureGeneration_, next_, next_,
                [source = raw->source, enabled = raw->deliverFrames, deliver = options_.deliver](auto sample) {
                    if (enabled->load(std::memory_order_relaxed)) deliver(*source, sample);
                });
            const auto prefix = "media_" + std::to_string(next_);
            return owner_->Add(next_, std::make_unique<RoomManagedPeer>(raw->peer->lifecycle,
                *raw->negotiation, std::move(attachment), prefix,
                [prefix](uint64_t revision) { return prefix + "_restart_" + std::to_string(revision); },
                [raw] { raw->retired = true; }));
        }
        return owner_->Add(next_, std::make_unique<ViewerPeer>(*raw->peer, *raw->negotiation,
            std::move(send), [raw] { raw->retired = true; }));
        } catch (...) {
            trace->Event(constructionStage, -1);
            PeerConnectionStatus failed;
            failed.peerId = id; failed.generation = next_; failed.retained = true;
            failed.recovery.state = PeerLifecycleState::Failed; failed.recovery.operationFailed = true;
            failed.current.labels["failureStage"] = constructionStage; failed.events = trace->Read();
            if (retiredConnections_.size() == 63) retiredConnections_.pop_front();
            retiredConnections_.push_back(std::move(failed));
            // No native owner was installed on an exception. Capture callbacks
            // own their source/gate independently and are removed asynchronously.
            if (peers_.contains(id)) {
                if (identity_.host) capture_.RemoveViewer(captureGeneration_, next_, next_);
                peers_.erase(id);
            }
            return false;
        }
    }
    void Remove(const std::string& id) noexcept override {
        failed_.erase(id);
        const auto found = peers_.find(id);
        if (found == peers_.end() || found->second->removing) return;
        auto& entry = *found->second;
        entry.deliverFrames->store(false, std::memory_order_relaxed);
        try {
            auto last = ConnectionStatus(id, entry);
            last.retained = true;
            if (last.recovery.state != PeerLifecycleState::Failed) last.recovery.state = PeerLifecycleState::Closed;
            // Bound historical evidence across arbitrary join/leave churn.
            if (retiredConnections_.size() == 63) retiredConnections_.pop_front();
            retiredConnections_.push_back(std::move(last));
        } catch (...) { /* Diagnostics must never prevent noexcept peer cleanup. */ }
        entry.removing = true;
        input_->Remove(id);
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
    std::future<CaptureUpdateResult> SwitchCaptureSource(media::CaptureSelection selection) override {
        if (!identity_.host || !captureSwitch_) return CaptureUpdateReady(CaptureUpdateError::Unsupported);
        if (stopping_) return CaptureUpdateReady(CaptureUpdateError::Unavailable);
        try {
            ValidateCaptureSelection(selection);
            auto factory = options_.captureForSelection(selection);
            // Revoke every grant. The Windows sink additionally requires fresh
            // captured geometry and refuses keyboard control on window sources.
            input_->Configure(7, 0);
            return captureSwitch_->Submit(selection, std::move(factory));
        }
        catch (...) { return CaptureUpdateReady(CaptureUpdateError::Invalid); }
    }
    CaptureSelectionStatus CaptureSelection() const override { return captureSwitch_ ? captureSwitch_->Status() : CaptureSelectionStatus{}; }
    std::future<AudioUpdateResult> SwitchAudioSource(media::AudioSelection selection) override {
        if (!identity_.host || !options_.audioSwitch || (selection.kind != AudioKind::None && !options_.audioForSelection)) return CaptureUpdateReady(AudioUpdateError::Unsupported);
        try { ValidateAudioSelection(selection); return options_.audioSwitch->Submit(selection,
            selection.kind == AudioKind::None ? AudioSwitchControl::Factory{} : options_.audioForSelection(selection)); }
        catch (...) { return CaptureUpdateReady(AudioUpdateError::Invalid); }
    }
    AudioSelectionStatus AudioSelection() const override { return options_.audioSwitch ? options_.audioSwitch->Status() : AudioSelectionStatus{}; }
    std::future<AudioUpdateResult> UpdatePlayback(PlaybackSelection selection) override {
        if (identity_.host || !options_.playback || !options_.playbackForSelection) return CaptureUpdateReady(AudioUpdateError::Unsupported);
        try { ValidatePlaybackSelection(selection); return options_.playback->Submit(selection, options_.playbackForSelection(selection)); }
        catch (...) { return CaptureUpdateReady(AudioUpdateError::Invalid); }
    }
    PlaybackStatus Playback() const override { return options_.playback ? options_.playback->Status() : PlaybackStatus{}; }
    std::shared_ptr<input::Port> Input() const override { return input_; }
    v2::StreamUpdateResult UpdateStreamPreferences(const StreamPreferences& preferences) override {
        if (!identity_.host) return {v2::StreamUpdateError::Unsupported};
        if (stopping_) return {v2::StreamUpdateError::Unavailable};
        try { ValidateStreamPreferences(preferences); }
        catch (const std::invalid_argument&) { return {v2::StreamUpdateError::Invalid}; }
        options_.preferences = preferences;
        return {v2::StreamUpdateError::None, ++settingsRevision_};
    }
    v2::StreamStatus StreamSettings() const override {
        v2::StreamStatus result;
        result.mediaEvents = options_.diagnostics->Read();
        result.performance = performance_.Read();
        result.connections.assign(retiredConnections_.begin(), retiredConnections_.end());
        for (const auto& [id, entry] : peers_) if (!entry->removing && !entry->retired)
            result.connections.push_back(ConnectionStatus(id, *entry));
        if (!identity_.host) {
            for(const auto& [id,entry]:peers_) if(!entry->removing && !entry->retired) {
                const auto sample=entry->sendRate->Read();
                result.receiveBps=sample.receiveBps;result.receiveRttMs=sample.sender.rttMs;
                break;
            }
            return result;
        }
        result.requestedRevision = settingsRevision_; result.preferences = options_.preferences;
        const auto capture = capture_.snapshot();
        result.capture = {capture.state, capture.captureFailure, capture.sourceGeneration, capture.captureSource};
        if (options_.codecStatus) result.codec = options_.codecStatus();
        for (const auto& [id, entry] : peers_) {
            if (entry->removing || entry->retired) continue;
            const auto stats = entry->source->settingsStats();
            result.peers.push_back({id, entry->settings.revision(), stats.observedRevision,
                entry->attemptedRevision == settingsRevision_ && entry->settingsRejected, stats.width, stats.height,
                AllocateViewerVideo(options_.preferences, allocatedViewers_), entry->settings.appliedVideoBitrateBps()});
            const auto sample = entry->sendRate->Read();
            auto& peer = result.peers.back();
            peer.transportSampleStale = sample.stale;
            peer.transportSendBps = sample.bitsPerSecond;
            peer.receiver = entry->telemetry->Status();
            peer.sender = sample.sender;
            peer.source = stats;
            peer.appliedPreferences = entry->settings.preferences();
            peer.settingsError = entry->attemptedRevision == settingsRevision_ ? entry->settingsError : SettingsApplyError::None;
            if (owner_) if (const auto lifecycle = owner_->snapshot(entry->generation))
                peer.recovery = {lifecycle->state, lifecycle->failure, lifecycle->restartRevision,
                    lifecycle->restartDispatchFailed, lifecycle->operationFailed};
            for (const auto& viewer : capture.viewers) if (viewer.viewer == entry->generation)
                peer.delivery = viewer.delivery;
        }
        return result;
    }
    void Advance() override {
        if (stopped_) return;
        const auto now = std::chrono::steady_clock::now();
        if (lastAdvance_ != std::chrono::steady_clock::time_point{})
            maxAdvanceGapMs_ = std::max(maxAdvanceGapMs_, std::chrono::duration<double, std::milli>(now - lastAdvance_).count());
        lastAdvance_ = now;
        if (now >= nextPerformanceSample_) {
            DiagnosticRecord sample;
            sample.numbers = {{"maxAdvanceGapMs", maxAdvanceGapMs_}, {"nativePeers", double(peers_.size())},
                {"failedPeers", double(failed_.size())}};
            processDiagnostics_.Sample(sample);
            if (identity_.host) {
                const auto capture = capture_.snapshot();
                sample.numbers["captureState"] = int(capture.state);
                sample.numbers["captureFailure"] = int(capture.captureFailure);
                sample.numbers["captureGeneration"] = double(capture.sourceGeneration);
            }
            const auto audio = AudioSelection(); const auto playback = Playback();
            sample.numbers["audioCaptureFailures"] = double(audio.health.failures);
            sample.numbers["audioPlaybackFailures"] = double(playback.health.failures);
            if (options_.presentation) if (const auto presentation = options_.presentation->Read()) {
                sample.numbers["presented"] = double(presentation->presented);
                sample.numbers["presentationDropped"] = double(presentation->dropped);
                sample.numbers["presentationQueued"] = presentation->queued;
                sample.numbers["presentationOutcome"] = presentation->outcome;
            }
            performance_.Add(std::move(sample)); maxAdvanceGapMs_ = 0; nextPerformanceSample_ = now + 1s;
        }
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
            if (captureStatus.state == HostMediaState::SourceClosed) throw std::runtime_error("Selected capture source closed");
            if (captureStatus.state == HostMediaState::Failed || captureStatus.state == HostMediaState::Stopped)
                throw std::runtime_error("Capture source stopped");
            if (!engine_) {
                if (captureStatus.state == HostMediaState::Minimized) {
                    startupDeadline_ = std::chrono::steady_clock::now() + 20s;
                    return;
                }
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
        const size_t viewers = std::count_if(peers_.begin(), peers_.end(), [](const auto& item) {
            return !item.second->removing && !item.second->retired;
        });
        if (viewers != allocatedViewers_) {
            allocatedViewers_ = viewers;
            if (identity_.host && options_.preferences.aggregateUploadLimitBps) ++settingsRevision_;
        }
        for (auto it = peers_.begin(); it != peers_.end();) {
            auto& entry = *it->second;
            if (entry.retired) { it = peers_.erase(it); continue; }
            if (entry.statsConnection != entry.negotiation->connectionId()) {
                entry.statsConnection = entry.negotiation->connectionId();
                entry.sendRate = std::make_shared<TransportSendRate>();
            }
            if (!entry.removing) entry.telemetry->Advance(entry.negotiation->connectionId(), *entry.peer->connection, entry.negotiation->ready());
            auto status = owner_->snapshot(entry.generation);
            if (entry.input) entry.input->Advance(entry.negotiation->connectionId(),
                !entry.removing && entry.negotiation->ready() && status && !status->peerClosed &&
                status->state == PeerLifecycleState::Connected);
            if (status && status->peerClosed && !entry.removing) failed_.insert(it->first);
            if (identity_.host && !entry.removing && !entry.negotiation->connectionId().empty() &&
                std::none_of(delivery.viewers.begin(), delivery.viewers.end(), [&](const auto& viewer) {
                    return viewer.viewer == entry.generation && viewer.connectionGeneration == entry.generation;
                })) failed_.insert(it->first);
            if (!entry.removing && identity_.host && entry.negotiation->ready() && entry.attemptedRevision != settingsRevision_) {
                entry.attemptedRevision = settingsRevision_;
                entry.settingsError = entry.settings.Apply(*entry.sender, *entry.source, options_.preferences, settingsRevision_,
                    AllocateViewerVideo(options_.preferences, allocatedViewers_));
                if (entry.settingsError == SettingsApplyError::None) {
                    // RTP limits alone leave GoogCC's probing maximum at its
                    // 5 Mbps fallback. Match the connection budget to this
                    // viewer's video allowance plus the reserved audio budget.
                    // Never reset its start estimate or impose a bitrate floor.
                    webrtc::BitrateSettings transport;
                    transport.max_bitrate_bps = entry.settings.appliedVideoBitrateBps() + kViewerAudioAllowanceBps;
                    if (!entry.peer->connection->SetBitrate(transport).ok()) {
                        // RTP/source settings have committed: do not continue
                        // with a partially applied transport contract.
                        entry.settingsError = SettingsApplyError::SenderRejected;
                        failed_.insert(it->first);
                    }
                }
                entry.settingsRejected = entry.settingsError != SettingsApplyError::None;
                // An RTP update rejection preserves the previously working sender.
                // An initial rejection cannot satisfy the initial stream contract.
                if (entry.settingsRejected && !entry.settings.revision()) failed_.insert(it->first);
            }
            entry.deliverFrames->store(identity_.host && !entry.removing && !failed_.contains(it->first) &&
                entry.negotiation->ready() && status && !status->peerClosed &&
                status->state == PeerLifecycleState::Connected && entry.settings.revision() != 0 &&
                entry.settings.appliedVideoBitrateBps() > 0, std::memory_order_relaxed);
            if (!entry.removing && !entry.negotiation->closed()) {
                bool request = false;
                { std::lock_guard lock(entry.sendRate->mutex);
                  const auto now = std::chrono::steady_clock::now();
                  if (!entry.sendRate->pending && now >= entry.sendRate->next) {
                      entry.sendRate->pending = true; entry.sendRate->next = now + 1s; request = true;
                  } }
                if (request) entry.peer->connection->GetStats(webrtc::make_ref_counted<TransportSendRateCallback>(entry.sendRate).get());
            }
            if (!entry.removing && now >= entry.nextMediaSample) {
                DiagnosticRecord sample;
                sample.numbers["deliveryEnabled"] = entry.deliverFrames->load();
                sample.numbers["lifecycle"] = status ? int(status->state) : -1;
                sample.numbers["negotiated"] = entry.negotiation->negotiated();
                sample.numbers["allocatedVideoBps"] = AllocateViewerVideo(options_.preferences, allocatedViewers_);
                sample.numbers["appliedVideoBps"] = entry.settings.appliedVideoBitrateBps();
                if (entry.source) {
                    const auto source = entry.source->settingsStats();
                    sample.numbers["sourceDropped"] = double(source.dropped);
                    sample.numbers["sourceScaled"] = double(source.scaled);
                    sample.numbers["gpuBusyDrops"] = double(source.gpuBusyDrops);
                    sample.numbers["gpuReadbacks"] = double(source.gpuReadbackFallbacks);
                    sample.numbers["sourceWidth"] = source.width; sample.numbers["sourceHeight"] = source.height;
                }
                for (const auto& viewer : delivery.viewers) if (viewer.viewer == entry.generation) {
                    sample.numbers["captureDelivered"] = double(viewer.delivery.delivered);
                    sample.numbers["captureReplaced"] = double(viewer.delivery.replaced);
                    sample.numbers["maxCaptureHandoffMs"] = double(viewer.delivery.maxHandoffAge.count()) / 1e6;
                }
                const auto receiver = entry.telemetry->Status();
                if (receiver.observation && !receiver.stale) {
                    sample.numbers["remoteDecoded"] = double(receiver.observation->framesDecoded);
                    if (receiver.observation->fpsMilli) sample.numbers["remoteDecodeFps"] = *receiver.observation->fpsMilli / 1000.0;
                    if (receiver.observation->presentation) {
                        sample.numbers["remotePresented"] = double(receiver.observation->presentation->presented);
                        sample.numbers["remotePresentationDropped"] = double(receiver.observation->presentation->dropped);
                    }
                }
                entry.mediaHistory.Add(std::move(sample)); entry.nextMediaSample = now + 1s;
            }
            ++it;
        }
    }
    std::shared_future<void> BeginStop() override {
        for (auto& [id, entry] : peers_) entry->deliverFrames->store(false, std::memory_order_relaxed);
        input_->Close();
        if (options_.playback) options_.playback->Close();
        if (options_.audioSwitch) options_.audioSwitch->Close();
        stopping_ = true;
        if (owner_) owner_->BeginStop();
        return stoppedFuture_;
    }
private:
    PeerConnectionStatus ConnectionStatus(const std::string& id, const Entry& entry) const {
        PeerConnectionStatus result;
        result.peerId = id; result.negotiated = entry.negotiation->negotiated();
        result.generation = entry.generation; result.ageMs = entry.events->elapsedMs();
        result.current = entry.peer->Diagnostics();
        result.current.labels["negotiationStage"] = entry.negotiation->stageName();
        result.current.labels["negotiationFailure"] = entry.negotiation->failure();
        result.events = entry.events->Read(); result.transportHistory = entry.sendRate->history.Read();
        result.mediaHistory = entry.mediaHistory.Read();
        {
            std::lock_guard lock(entry.sendRate->mutex);
            if (entry.sendRate->sampled != std::chrono::steady_clock::time_point{})
                result.statsAgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - entry.sendRate->sampled).count();
        }
        result.localCandidates = entry.negotiation->localCandidates();
        result.remoteCandidates = entry.negotiation->remoteCandidates();
        if (owner_) if (const auto state = owner_->snapshot(entry.generation))
            result.recovery = {state->state, state->failure, state->restartRevision,
                state->restartDispatchFailed, state->operationFailed};
        return result;
    }
};
}
std::unique_ptr<v2::RoomRuntime> CreateNativeRoomRuntime(v2::RoomIdentity identity, v2::RoomSend send, NativeRoomRuntimeOptions options) {
    return std::make_unique<NativeRoomRuntime>(std::move(identity), std::move(send), std::move(options));
}
}
