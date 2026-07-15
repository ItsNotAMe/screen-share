#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace screenshare {

struct SignalingCandidate {
    std::string type = "srflx";
    std::string ip;
    uint16_t port = 0;
    std::string protocol = "udp";
};

struct SignalingPeerMetadata {
    std::string name;
    std::string platform;
};

struct SignalingPeer {
    std::string peerId;
    std::vector<SignalingCandidate> candidates;
    SignalingPeerMetadata metadata;
    int64_t lastSeen = 0;
};

struct SignalingPeerState {
    std::string peerId;
    std::vector<SignalingCandidate> candidates;
    SignalingPeerMetadata metadata;
    std::string roomName;
    std::string roomPassword;
    bool passwordProtected = false;
    bool host = false;
};

struct SignalingRoomResponse {
    bool ok = false;
    std::string roomAccessKey;
    std::string roomName;
    bool passwordProtected = false;
    // Server-issued secret returned by Join. It must be presented on subsequent
    // Peers/Heartbeat/Leave/ListenRoomEvents calls to prove ownership of this
    // peerId and to read candidates/the room key.
    std::string peerToken;
    std::vector<SignalingPeer> peers;
};

struct SignalingClientConfig {
    std::string serverUrl;
    std::chrono::milliseconds timeout{5000};
};

class SignalingClient {
public:
    explicit SignalingClient(SignalingClientConfig config);

    void Health();
    // peerToken is empty on the first join (the server issues one, returned in
    // the response) and the issued token on subsequent re-announcements of the
    // same peerId, which the server requires to prove ownership.
    SignalingRoomResponse Join(
        const std::string& roomId,
        const SignalingPeerState& peer,
        const std::string& peerToken = {});
    // peerToken is the secret returned by Join; it authorizes reading candidates
    // and the room key and mutating this peer's state.
    SignalingRoomResponse Peers(
        const std::string& roomId,
        const std::string& peerId,
        const std::string& peerToken);
    void ListenRoomEvents(
        const std::string& roomId,
        const std::string& peerId,
        const std::string& peerToken,
        const std::function<void(const std::string&)>& onEvent,
        const std::function<bool()>& shouldStop);
    void Heartbeat(const std::string& roomId, const std::string& peerId, const std::string& peerToken);
    void Leave(const std::string& roomId, const std::string& peerId, const std::string& peerToken);

    struct ParsedUrl {
        std::wstring host;
        uint16_t port = 0;
        std::wstring pathPrefix;
        bool secure = false;
    };

private:
    [[nodiscard]] std::string Request(
        const wchar_t* method,
        const std::string& pathAndQuery,
        const std::optional<std::string>& jsonBody,
        const std::wstring& extraHeaders = {});

    SignalingClientConfig config_;
    ParsedUrl url_;
};

std::string SignalingJsonEscape(const std::string& text);
void ValidateSignalingRoomId(const std::string& roomId);
void ValidateSignalingPeerId(const std::string& peerId);
void ValidateSignalingCandidate(const SignalingCandidate& candidate);

} // namespace screenshare
