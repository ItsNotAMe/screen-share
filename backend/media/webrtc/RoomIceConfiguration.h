#pragma once
#include "api/peer_connection_interface.h"

namespace screenshare::media {
inline webrtc::PeerConnectionInterface::RTCConfiguration RoomIceConfiguration(
    webrtc::PeerConnectionInterface::RTCConfiguration config, bool useDefaultStun) {
    // Match the legacy client's public STUN provider. STUN discovers public
    // addresses; it does not relay media or replace a TURN service.
    // Explicit ICE servers and relay-only embedding policies are never replaced.
    if (useDefaultStun && config.servers.empty() && config.type == webrtc::PeerConnectionInterface::kAll) {
        webrtc::PeerConnectionInterface::IceServer server;
        server.urls = {"stun:stun.l.google.com:19302", "stun:stun1.l.google.com:19302"};
        config.servers.push_back(std::move(server));
    }
    return config;
}
}
