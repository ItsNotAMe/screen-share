# Shared native adapters; codec/capture primitives are supplied by the core target
# (or the proof's primitive target), never by diagnostic implementation code.
if(NOT TARGET ScreenShareMediaAdapters)
    add_library(ScreenShareMediaAdapters STATIC
        "${CMAKE_CURRENT_LIST_DIR}/../backend/media/webrtc/PcmAudioDeviceModule.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../backend/media/webrtc/MicrophoneCapture.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../backend/media/audio/WasapiPcmEndpoint.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../backend/media/webrtc/D3dVideoFrameBuffer.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../backend/media/webrtc/D3dNv12Scaler.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../backend/media/webrtc/MfVideoEncoderFactory.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../backend/media/webrtc/MfVideoDecoderFactory.cpp")
    target_include_directories(ScreenShareMediaAdapters PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../backend")
    target_compile_features(ScreenShareMediaAdapters PUBLIC cxx_std_20)
    target_compile_options(ScreenShareMediaAdapters PRIVATE /EHsc /GR)
    target_link_libraries(ScreenShareMediaAdapters PUBLIC ScreenShare::WebRTC
        d3d11 d3dcompiler dxgi dxguid mf mfplat mfuuid wmcodecdspuuid ole32 user32 windowsapp CoreMessaging)
endif()
