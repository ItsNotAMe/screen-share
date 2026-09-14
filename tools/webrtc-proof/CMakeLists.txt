cmake_minimum_required(VERSION 3.24)
project(ScreenShareWebRTCProof LANGUAGES CXX)
include(CTest)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/WebRTC.cmake")
screenshare_import_webrtc()
add_executable(WebRTCProof main.cpp)
target_link_libraries(WebRTCProof PRIVATE ScreenShare::WebRTC)
add_test(NAME webrtc-direct-h264-and-data COMMAND WebRTCProof)
set_tests_properties(webrtc-direct-h264-and-data PROPERTIES TIMEOUT 45)

# Probe existing Windows primitives independently before adapting them to WebRTC.
set(nativeRoot "${CMAKE_CURRENT_LIST_DIR}/../../src")
set_property(SOURCE "${nativeRoot}/capture/DesktopCapturer.cpp" APPEND PROPERTY COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:Clang>:/clang:-mcx16>")
add_library(MfWebRTCCodecs STATIC
    "${nativeRoot}/render/Nv12D3D11Presenter.cpp"
    "${nativeRoot}/media/webrtc/PcmAudioDeviceModule.cpp"
    "${nativeRoot}/media/audio/WasapiPcmEndpoint.cpp"
    "${nativeRoot}/audio/WasapiCapture.cpp"
    "${nativeRoot}/media/webrtc/D3dVideoFrameBuffer.cpp"
    "${nativeRoot}/media/webrtc/MfVideoEncoderFactory.cpp"
    "${nativeRoot}/media/webrtc/MfVideoDecoderFactory.cpp"
    "${nativeRoot}/codec/H264StreamEncoder.cpp"
    "${nativeRoot}/codec/H264Bitstream.cpp"
    "${nativeRoot}/video/Nv12Convert.cpp"
    "${nativeRoot}/capture/DesktopCapturer.cpp"
    "${nativeRoot}/codec/H264StreamDecoder.cpp")
target_include_directories(MfWebRTCCodecs PUBLIC "${nativeRoot}")
target_link_libraries(MfWebRTCCodecs PUBLIC ScreenShare::WebRTC
    d3d11 d3dcompiler dxgi dxguid mf mfplat mfuuid wmcodecdspuuid ole32 user32 windowsapp)
target_link_libraries(WebRTCProof PRIVATE MfWebRTCCodecs)
add_executable(MfDecoderAdapterTest MfDecoderAdapterTest.cpp)
target_link_libraries(MfDecoderAdapterTest PRIVATE MfWebRTCCodecs)
add_test(NAME mf-webrtc-decoder-lifecycle COMMAND MfDecoderAdapterTest)
set_tests_properties(mf-webrtc-decoder-lifecycle PROPERTIES TIMEOUT 30)
add_executable(MfEncoderAdapterTest MfEncoderAdapterTest.cpp)
target_link_libraries(MfEncoderAdapterTest PRIVATE MfWebRTCCodecs)
add_test(NAME mf-webrtc-encoder-lifecycle COMMAND MfEncoderAdapterTest)
set_tests_properties(mf-webrtc-encoder-lifecycle PROPERTIES TIMEOUT 30)
add_executable(HardwareFrameWaitTest HardwareFrameWaitTest.cpp)
target_link_libraries(HardwareFrameWaitTest PRIVATE MfWebRTCCodecs)
add_test(NAME mf-hardware-output-deadline COMMAND HardwareFrameWaitTest)
set_tests_properties(mf-hardware-output-deadline PROPERTIES TIMEOUT 10)
add_executable(MfHardwareAdapterTest MfHardwareAdapterTest.cpp)
target_link_libraries(MfHardwareAdapterTest PRIVATE MfWebRTCCodecs)
add_executable(PcmAudioDeviceTest PcmAudioDeviceTest.cpp)
target_link_libraries(PcmAudioDeviceTest PRIVATE MfWebRTCCodecs)
add_test(NAME pcm-adm-lifecycle COMMAND PcmAudioDeviceTest)
set_tests_properties(pcm-adm-lifecycle PROPERTIES TIMEOUT 20)
option(SCREENSHARE_TEST_AUDIO_DEVICE "Run WASAPI process-loopback/playback proof with a quiet tone" OFF)
if(SCREENSHARE_TEST_AUDIO_DEVICE)
    add_test(NAME wasapi-adm-lifecycle COMMAND PcmAudioDeviceTest --wasapi)
    add_test(NAME webrtc-wasapi-opus COMMAND WebRTCProof --wasapi)
    set_tests_properties(webrtc-wasapi-opus PROPERTIES TIMEOUT 30)
    set_tests_properties(wasapi-adm-lifecycle PROPERTIES TIMEOUT 30)
endif()
option(SCREENSHARE_TEST_HARDWARE "Run proofs requiring a supported D3D11/MF GPU" OFF)
add_executable(LiveCaptureTest LiveCaptureTest.cpp)
add_executable(CaptureProcessExitTest CaptureProcessExitTest.cpp)
target_link_libraries(CaptureProcessExitTest PRIVATE MfWebRTCCodecs gdi32)
add_executable(PresentationQueueTest PresentationQueueTest.cpp)
add_executable(PresentationRecoveryTest PresentationRecoveryTest.cpp)
target_link_libraries(PresentationRecoveryTest PRIVATE MfWebRTCCodecs gdi32)
add_test(NAME presentation-device-recovery COMMAND PresentationRecoveryTest)
set_tests_properties(presentation-device-recovery PROPERTIES TIMEOUT 20)
target_link_libraries(PresentationQueueTest PRIVATE MfWebRTCCodecs)
add_test(NAME presentation-latest-frame COMMAND PresentationQueueTest)
set_tests_properties(presentation-latest-frame PROPERTIES TIMEOUT 10)
target_link_libraries(LiveCaptureTest PRIVATE MfWebRTCCodecs gdi32)
target_link_libraries(WebRTCProof PRIVATE gdi32)
option(SCREENSHARE_TEST_LIVE_CAPTURE "Capture a generated test window through WGC" OFF)
if(SCREENSHARE_TEST_LIVE_CAPTURE)
    add_test(NAME presentation-gpu-recovery COMMAND PresentationRecoveryTest --gpu)
    set_tests_properties(presentation-gpu-recovery PROPERTIES TIMEOUT 20)
    add_test(NAME wgc-source-process-exit COMMAND CaptureProcessExitTest)
    set_tests_properties(wgc-source-process-exit PROPERTIES TIMEOUT 45)
    add_test(NAME wgc-owned-hardware-capture COMMAND LiveCaptureTest --repeat)
    add_test(NAME webrtc-live-wgc-h264 COMMAND WebRTCProof --live-capture)
    set_tests_properties(webrtc-live-wgc-h264 PROPERTIES TIMEOUT 45)
    set_tests_properties(wgc-owned-hardware-capture PROPERTIES TIMEOUT 45)
endif()
if(SCREENSHARE_TEST_HARDWARE)
    add_test(NAME mf-hardware-fallback-and-gpu-lifetime COMMAND MfHardwareAdapterTest)
    add_test(NAME webrtc-direct-hardware-h264 COMMAND WebRTCProof --hardware)
    add_test(NAME mf-hardware-encoder-lifecycle COMMAND MfEncoderAdapterTest --hardware)
    set_tests_properties(mf-hardware-fallback-and-gpu-lifetime webrtc-direct-hardware-h264 mf-hardware-encoder-lifecycle PROPERTIES TIMEOUT 45)
endif()
add_executable(MfCodecProbe MfCodecProbe.cpp
    "${nativeRoot}/capture/DesktopCapturer.cpp"
    "${nativeRoot}/codec/H264Bitstream.cpp"
    "${nativeRoot}/codec/H264StreamEncoder.cpp"
    "${nativeRoot}/codec/H264StreamDecoder.cpp"
    "${nativeRoot}/video/Nv12Convert.cpp")
target_compile_features(MfCodecProbe PRIVATE cxx_std_20)
target_include_directories(MfCodecProbe PRIVATE "${nativeRoot}")
target_compile_definitions(MfCodecProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(MfCodecProbe PRIVATE /EHsc /GR)
target_link_libraries(MfCodecProbe PRIVATE d3d11 d3dcompiler dxgi dxguid mf mfplat mfuuid wmcodecdspuuid ole32 user32 windowsapp)
add_test(NAME mf-software-codec COMMAND MfCodecProbe)
set_tests_properties(mf-software-codec PROPERTIES TIMEOUT 30)
