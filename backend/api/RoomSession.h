#pragma once
#include "media/RoomPeerSignal.h"
#include "media/StreamPreferences.h"
#include "media/CaptureSelection.h"
#include "media/AudioSelection.h"
#include "media/PlaybackSelection.h"
#include "media/ReceiverVideoStatus.h"
#include "media/SenderVideoStatus.h"
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace screenshare::v2 {
struct RoomOptions {
    std::string origin, roomId, nickname, name, password;
    bool host = false, publicRoom = true;
    int viewerLimit = 4;
};
enum class RoomPhase { Idle, Admitting, Connecting, Active, Reconnecting, Stopping, Stopped, Failed };
enum class RoomError { None, Busy, Cancelled, Admission, Transport, Media };
struct RoomResult { RoomError error = RoomError::None; bool outcomeUnconfirmed = false; };
struct RoomPolicy { std::string name; bool publicRoom = true; int viewerLimit = 4; };
struct RoomMember { std::string peerId, nickname; bool host = false; };
enum class RoomUpdateError { None, Invalid, Busy, Unavailable, Forbidden, Conflict, Rejected, Unconfirmed };
struct RoomUpdateResult { RoomUpdateError error = RoomUpdateError::None; uint64_t currentRevision = 0; };
enum class StreamUpdateError { None, Invalid, Busy, Unavailable, Unsupported, Rejected };
struct StreamUpdateResult { StreamUpdateError error = StreamUpdateError::None; uint64_t revision = 0; };
struct PeerStreamStatus {
    std::string peerId;
    uint64_t appliedRevision = 0, observedRevision = 0;
    bool rejected = false;
    int width = 0, height = 0;
    int allocatedVideoBitrateBps = 0, appliedVideoBitrateBps = 0;
    // Sampled WebRTC transport bytes, not physical-interface/IP overhead.
    std::optional<uint64_t> transportSendBps;
    // A stale sample is deliberately withheld; absent is not measured zero.
    bool transportSampleStale = false;
    media::ReceiverVideoStatus receiver;
    media::SenderVideoObservation sender;
};
struct StreamStatus {
    uint64_t requestedRevision = 0;
    media::StreamPreferences preferences;
    std::vector<PeerStreamStatus> peers;
};
struct RoomStatus {
    RoomPhase phase = RoomPhase::Idle;
    RoomError error = RoomError::None;
    uint64_t generation = 0;
    std::string roomId, peerId;
    size_t activePeers = 0, failedPeers = 0, pendingPeers = 0;
    StreamStatus stream;
    uint64_t revision = 0;
    RoomPolicy policy;
    std::vector<RoomMember> members;
    media::CaptureSelectionStatus capture;
    media::AudioSelectionStatus audio;
    media::PlaybackStatus playback;
};
// Private media implementations are injected without leaking Qt/WebRTC types
// into the public control API. All runtime methods execute on owned signaling.
// BeginStop must drain capture/delivery asynchronously before destruction.
// It must return a valid shared future. Advance continues during that drain;
// Ready and Remove must not throw. RoomSend is signaling-only and reports queue
// acceptance; subsequent transport failure is handled by the session owner.
class RoomRuntime {
public:
    virtual ~RoomRuntime() = default;
    virtual void Advance() = 0;
    virtual bool Ready(const std::string&) = 0;
    virtual bool Add(const std::string&) = 0;
    virtual void Remove(const std::string&) noexcept = 0;
    virtual bool Receive(const std::string&, media::RoomPeerSignal) = 0;
    virtual std::vector<std::string> FailedPeers() const { return {}; }
    virtual StreamUpdateResult UpdateStreamPreferences(const media::StreamPreferences&) { return {StreamUpdateError::Unsupported}; }
    virtual StreamStatus StreamSettings() const { return {}; }
    virtual std::future<media::CaptureUpdateResult> SwitchCaptureSource(media::CaptureSelection) { return media::CaptureUpdateReady(media::CaptureUpdateError::Unsupported); }
    virtual media::CaptureSelectionStatus CaptureSelection() const { return {}; }
    virtual std::future<media::AudioUpdateResult> SwitchAudioSource(media::AudioSelection) { return media::CaptureUpdateReady(media::AudioUpdateError::Unsupported); }
    virtual media::AudioSelectionStatus AudioSelection() const { return {}; }
    virtual std::future<media::AudioUpdateResult> UpdatePlayback(media::PlaybackSelection) { return media::CaptureUpdateReady(media::AudioUpdateError::Unsupported); }
    virtual media::PlaybackStatus Playback() const { return {}; }
    virtual std::shared_future<void> BeginStop() = 0;
};
struct RoomIdentity { bool host; std::string roomId, peerId; };
using RoomSend = std::function<bool(const std::string&, media::RoomPeerSignal)>;
using RoomRuntimeFactory = std::function<std::unique_ptr<RoomRuntime>(const RoomIdentity&, RoomSend)>;
// One session incarnation. Requires process QCoreApplication and application
// media/SSL initialization. Start/Stop/Status may be called from external threads.
// Stop is coalesced and completes after networking and media drain. Destroy on
// an external owner thread after command producers stop; never in runtime hooks.
// Credentials remain private. Create a new object for a new admission; ambiguous
// admission is never retried automatically.
// Start completes on the first authenticated room snapshot, not first media.
class RoomSession final {
public:
    explicit RoomSession(RoomRuntimeFactory, bool diagnosticLoopback = false);
    ~RoomSession();
    RoomSession(const RoomSession&) = delete;
    RoomSession& operator=(const RoomSession&) = delete;
    std::future<RoomResult> Start(RoomOptions);
    // Host-only. Completion means accepted, not decoded/displayed remotely.
    // Status exposes per-peer sender application and source-frame observation.
    // At most one command is queued; callers may retry Busy with their latest value.
    std::future<StreamUpdateResult> UpdateStreamPreferences(media::StreamPreferences);
    // Host-only, one pending switch. Success means the replacement produced its
    // first frame; it is not a remote-display acknowledgement. Failure keeps the
    // previous source when it remains healthy. Stop cancels pending completion.
    std::future<media::CaptureUpdateResult> SwitchCaptureSource(media::CaptureSelection);
    // Host capture only, one pending handover. Requires an active recording
    // endpoint (normally at least one viewer). Success means first PCM received.
    std::future<media::AudioUpdateResult> SwitchAudioSource(media::AudioSelection);
    // Viewer-local output settings; success means the endpoint accepted a block,
    // not physical playback. One pending command, cancelled by Stop.
    std::future<media::AudioUpdateResult> UpdatePlayback(media::PlaybackSelection);
    // One in-flight room/profile mutation. Completion is the server acknowledgement;
    // authoritative state arrives independently through pushed snapshots. Never
    // automatically retry Conflict or Unconfirmed (the server may have committed).
    std::future<RoomUpdateResult> UpdateNickname(std::string, uint64_t expectedRevision);
    std::future<RoomUpdateResult> UpdateRoomPolicy(RoomPolicy, uint64_t expectedRevision);
    std::shared_future<void> Stop();
    RoomStatus Status() const;
private:
    std::future<RoomUpdateResult> SubmitUpdate(std::optional<std::string>, std::optional<RoomPolicy>, uint64_t);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
