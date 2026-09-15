# Shared by the application and native media proofs, using the pinned SDK.
if(NOT TARGET ScreenShareNegotiation)
    add_library(ScreenShareNegotiation STATIC
        "${CMAKE_CURRENT_LIST_DIR}/../src/media/webrtc/PeerNegotiation.cpp")
    target_include_directories(ScreenShareNegotiation PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../src")
    target_compile_features(ScreenShareNegotiation PUBLIC cxx_std_20)
    target_compile_options(ScreenShareNegotiation PRIVATE /EHsc /GR)
    target_link_libraries(ScreenShareNegotiation PUBLIC ScreenShare::WebRTC)
endif()
