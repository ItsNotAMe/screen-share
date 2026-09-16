# Shared by the application and native media proofs, using the pinned SDK.
include("${CMAKE_CURRENT_LIST_DIR}/MediaAdapters.cmake")
if(NOT TARGET ScreenShareNegotiation)
    add_library(ScreenShareNegotiation STATIC
        "${CMAKE_CURRENT_LIST_DIR}/../backend/media/webrtc/MediaEngine.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../backend/media/webrtc/NativeRoomRuntime.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../backend/media/webrtc/WindowsRoomRuntime.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../backend/media/webrtc/MediaPeer.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../backend/media/webrtc/PeerNegotiation.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../backend/media/webrtc/RoomPeerNegotiation.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../backend/media/webrtc/SignalingExecutor.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../backend/media/webrtc/HostPeerOwner.cpp")
    target_include_directories(ScreenShareNegotiation PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../backend")
    target_compile_features(ScreenShareNegotiation PUBLIC cxx_std_20)
    target_compile_options(ScreenShareNegotiation PRIVATE /EHsc /GR)
    target_link_libraries(ScreenShareNegotiation PUBLIC ScreenShare::WebRTC ScreenShareMediaAdapters)
endif()
