#pragma once
#include "media/webrtc/MediaEngine.h"
#include "media/webrtc/PeerNegotiation.h"
#include "media/PeerConnectionLifecycle.h"
#include "media/DiagnosticHistory.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_frame.h"
#include <functional>
#include <vector>

namespace screenshare::media {
// One native peer incarnation, used only on its engine's signaling executor.
// The frame sink runs on WebRTC's delivery thread and must not block signaling
// or destroy the peer. Other callbacks run on signaling, must not throw/reenter
// destruction, and are disconnected by Close. Engine and sinks outlive this peer.
class MediaPeer : public webrtc::PeerConnectionObserver {
public:
    using ChannelReady = std::function<void(webrtc::scoped_refptr<webrtc::DataChannelInterface>)>;
    MediaPeer(MediaEngine& engine, uint64_t generation,
        webrtc::VideoSinkInterface<webrtc::VideoFrame>* frames,
        ChannelReady channelReady,
        webrtc::PeerConnectionInterface::RTCConfiguration config = {}, std::shared_ptr<DiagnosticHistory> diagnostics = {});
    ~MediaPeer() override;
    MediaPeer(const MediaPeer&) = delete;
    MediaPeer& operator=(const MediaPeer&) = delete;
    void Close() noexcept;
    void OpenHostChannels(MediaEngine& engine);
    PeerNegotiation& Negotiation() { return *negotiation_; }
    PeerConnectionLifecycle lifecycle;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> connection;
    std::function<void(const webrtc::IceCandidate*)> candidateObserver;
    DiagnosticRecord Diagnostics() const;
private:
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override;
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override;
    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState) override;
    void OnIceCandidateError(const std::string&, int, const std::string&, int, const std::string&) override;
    void OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState) override;
    void OnIceCandidate(const webrtc::IceCandidate*) override;
    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>) override;
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override;
    webrtc::Thread* signaling_;
    std::unique_ptr<PeerNegotiation> negotiation_;
    webrtc::VideoSinkInterface<webrtc::VideoFrame>* frames_;
    ChannelReady channelReady_;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_;
    std::vector<webrtc::scoped_refptr<webrtc::DataChannelInterface>> channels_;
    bool closed_ = false;
    std::shared_ptr<DiagnosticHistory> diagnostics_;
    DiagnosticRecord observed_;
};
}
