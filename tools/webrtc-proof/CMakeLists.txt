cmake_minimum_required(VERSION 3.24)
project(ScreenShareWebRTCProof LANGUAGES CXX)
include(CTest)
find_program(SCREENSHARE_POWERSHELL_EXECUTABLE NAMES powershell pwsh)
if(SCREENSHARE_POWERSHELL_EXECUTABLE)
    add_test(NAME room-live-timing-evidence COMMAND "${SCREENSHARE_POWERSHELL_EXECUTABLE}"
        -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_LIST_DIR}/../../tests/RoomLiveEvidenceTests.ps1")
    set_tests_properties(room-live-timing-evidence PROPERTIES TIMEOUT 20)
endif()
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME capture-stress-evidence COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/../../tests/LiveCaptureStressTests.py")
    set_tests_properties(capture-stress-evidence PROPERTIES TIMEOUT 20)
    add_test(NAME capture-handle-trace-evidence COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/../../tests/CaptureHandleTraceTests.py")
    set_tests_properties(capture-handle-trace-evidence PROPERTIES TIMEOUT 20)
    add_test(NAME room-lifecycle-evidence COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/../../tests/RoomLifecycleStressTests.py")
    set_tests_properties(room-lifecycle-evidence PROPERTIES TIMEOUT 20)
    add_test(NAME room-impairment-evidence COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/../../tests/RoomImpairmentTests.py")
    set_tests_properties(room-impairment-evidence PROPERTIES TIMEOUT 20)
    add_test(NAME rtc-probe-evidence COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/../../tests/RtcProbeEvidenceTests.py")
    set_tests_properties(rtc-probe-evidence PROPERTIES TIMEOUT 20)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/WebRTC.cmake")
screenshare_import_webrtc()
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/MediaNegotiation.cmake")
add_executable(RtcEventLogTest RtcEventLogTest.cpp)
target_link_libraries(RtcEventLogTest PRIVATE ScreenShareNegotiation)
add_test(NAME rtc-event-log-bounds COMMAND RtcEventLogTest)
set_tests_properties(rtc-event-log-bounds PROPERTIES TIMEOUT 10)
add_executable(WebRTCProof main.cpp)
add_executable(SignalingExecutorTest SignalingExecutorTest.cpp)
target_link_libraries(SignalingExecutorTest PRIVATE ScreenShareNegotiation)
add_test(NAME signaling-executor-lifecycle COMMAND SignalingExecutorTest)
set_tests_properties(signaling-executor-lifecycle PROPERTIES TIMEOUT 15)
target_link_libraries(WebRTCProof PRIVATE ScreenShare::WebRTC)
add_test(NAME webrtc-direct-h264-and-data COMMAND WebRTCProof)
set_tests_properties(webrtc-direct-h264-and-data PROPERTIES TIMEOUT 45)
add_test(NAME webrtc-four-viewer-isolation COMMAND WebRTCProof --multi-viewer)
set_tests_properties(webrtc-four-viewer-isolation PROPERTIES TIMEOUT 60)
add_test(NAME webrtc-negotiation-lifecycle COMMAND WebRTCProof --negotiation-only)
set_tests_properties(webrtc-negotiation-lifecycle PROPERTIES TIMEOUT 20)

# Probe existing Windows primitives independently before adapting them to WebRTC.
set(nativeRoot "${CMAKE_CURRENT_LIST_DIR}/../../backend")
set_property(SOURCE "${nativeRoot}/capture/DesktopCapturer.cpp" APPEND PROPERTY COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:Clang>:/clang:-mcx16>")
add_library(MfWebRTCCodecs STATIC
    "${nativeRoot}/media/HostMediaSession.cpp"
    "${nativeRoot}/media/capture/CaptureSession.cpp"
    "${nativeRoot}/media/capture/CaptureDistributor.cpp"
    "${nativeRoot}/render/Nv12D3D11Presenter.cpp"
    "${nativeRoot}/audio/WasapiCapture.cpp"
    "${nativeRoot}/codec/H264StreamEncoder.cpp"
    "${nativeRoot}/codec/H264Bitstream.cpp"
    "${nativeRoot}/video/Nv12Convert.cpp"
    "${nativeRoot}/capture/DesktopCapturer.cpp"
    "${nativeRoot}/capture/DxgiCursor.cpp"
    "${nativeRoot}/input/RemoteInputInjector.cpp"
    "${nativeRoot}/input/v2/DesktopSink.cpp"
    "${nativeRoot}/input/v2/GamepadSink.cpp"
    "${nativeRoot}/input/VirtualGamepadBackend.cpp"
    "${nativeRoot}/input/XInputGamepad.cpp"
    "${nativeRoot}/codec/H264StreamDecoder.cpp")
target_include_directories(MfWebRTCCodecs PUBLIC "${nativeRoot}")
target_link_libraries(MfWebRTCCodecs PUBLIC ScreenShare::WebRTC ScreenShareMediaAdapters
    d3d11 d3dcompiler dxgi dxguid mf mfplat mfuuid wmcodecdspuuid ole32 user32 dwmapi windowsapp CoreMessaging)
target_link_libraries(WebRTCProof PRIVATE MfWebRTCCodecs ScreenShareNegotiation)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/RoomTransport.cmake")
add_executable(RoomNetworkTest RoomNetworkTest.cpp)
target_link_libraries(RoomNetworkTest PRIVATE ScreenShareRoom)
add_custom_command(TARGET RoomNetworkTest POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE:Qt6::Network>" "$<TARGET_FILE:Qt6::WebSockets>" "$<TARGET_FILE_DIR:RoomNetworkTest>" VERBATIM)
add_test(NAME room-network-ownership COMMAND RoomNetworkTest)
set_tests_properties(room-network-ownership PROPERTIES TIMEOUT 30)
add_executable(RoomMediaProof RoomMediaProof.cpp)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/RoomSession.cmake")
add_executable(RoomSessionCoordinatorTest RoomSessionCoordinatorTest.cpp)
target_link_libraries(RoomSessionCoordinatorTest PRIVATE ScreenShareRoomSession)
add_custom_command(TARGET RoomSessionCoordinatorTest POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE:Qt6::Network>" "$<TARGET_FILE:Qt6::WebSockets>" "$<TARGET_FILE_DIR:RoomSessionCoordinatorTest>" VERBATIM)
add_test(NAME room-session-coordinator COMMAND RoomSessionCoordinatorTest)
  set_tests_properties(room-session-coordinator PROPERTIES TIMEOUT 20)
  add_executable(RoomMediaSessionTest RoomMediaSessionTest.cpp)
  target_link_libraries(RoomMediaSessionTest PRIVATE ScreenShareRoomSession)
  add_custom_command(TARGET RoomMediaSessionTest POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE:Qt6::Network>" "$<TARGET_FILE:Qt6::WebSockets>"
      "$<TARGET_FILE_DIR:RoomMediaSessionTest>" VERBATIM)
  add_test(NAME room-media-session COMMAND RoomMediaSessionTest)
  set_tests_properties(room-media-session PROPERTIES TIMEOUT 20)
target_link_libraries(RoomMediaProof PRIVATE MfWebRTCCodecs ScreenShareNegotiation ScreenShareRoomSession)
add_custom_command(TARGET RoomMediaProof POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE:Qt6::Network>" "$<TARGET_FILE:Qt6::WebSockets>" "$<TARGET_FILE_DIR:RoomMediaProof>" VERBATIM)
find_program(SCREENSHARE_NODE_EXECUTABLE node)
set(SCREENSHARE_WEBRTC_TEST_SOURCE_DIR "" CACHE PATH "Optional pinned WebRTC checkout for network impairment proof")
if(SCREENSHARE_WEBRTC_TEST_SOURCE_DIR)
    set(webrtcSource "${SCREENSHARE_WEBRTC_TEST_SOURCE_DIR}")
    find_package(Git REQUIRED)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${webrtcSource}" rev-parse HEAD OUTPUT_VARIABLE testRevision OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/../../refactor/webrtc-source.json" testLock)
    string(JSON expectedTestRevision GET "${testLock}" commit)
    if(NOT testRevision STREQUAL expectedTestRevision)
        message(FATAL_ERROR "Network impairment source must match the pinned WebRTC revision")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${webrtcSource}" diff --exit-code HEAD --
        test/network/simulated_network.cc test/network/simulated_network.h api/test/simulated_network.h
        api/test/network_emulation/leaky_bucket_network_queue.cc api/test/network_emulation/leaky_bucket_network_queue.h
        api/test/network_emulation/network_queue.h COMMAND_ERROR_IS_FATAL ANY)
    add_executable(RoomImpairmentProof RoomImpairmentProof.cpp
        "${webrtcSource}/test/network/simulated_network.cc"
        "${webrtcSource}/api/test/network_emulation/leaky_bucket_network_queue.cc")
    target_link_libraries(RoomImpairmentProof PRIVATE MfWebRTCCodecs ScreenShareRoomSession)
    target_include_directories(RoomImpairmentProof SYSTEM PRIVATE "${webrtcSource}")
    add_custom_command(TARGET RoomImpairmentProof POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE:Qt6::Network>" "$<TARGET_FILE:Qt6::WebSockets>"
        "$<TARGET_FILE_DIR:RoomImpairmentProof>" VERBATIM)
endif()
  if(SCREENSHARE_NODE_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../signaling-worker/node_modules/miniflare/package.json")
      add_executable(PublicRoomSessionProof PublicRoomSessionProof.cpp)
      target_link_libraries(PublicRoomSessionProof PRIVATE MfWebRTCCodecs ScreenShareRoomSession)
      get_filename_component(proofQtBin "${Qt6_DIR}/../../../bin" ABSOLUTE)
      find_program(PROOF_WINDEPLOYQT_EXECUTABLE NAMES windeployqt6 windeployqt
          PATHS "${proofQtBin}" NO_DEFAULT_PATH REQUIRED)
      add_executable(CrossMachineRoomProof CrossMachineRoomProof.cpp)
      target_link_libraries(CrossMachineRoomProof PRIVATE MfWebRTCCodecs ScreenShareRoomSession)
      add_custom_command(TARGET CrossMachineRoomProof POST_BUILD COMMAND "${CMAKE_COMMAND}"
          "-DWINDEPLOYQT_EXECUTABLE=${PROOF_WINDEPLOYQT_EXECUTABLE}"
          "-DWINDEPLOYQT_TARGET=$<TARGET_FILE:CrossMachineRoomProof>"
          "-DWINDEPLOYQT_DEBUG=$<CONFIG:Debug>"
          -DWINDEPLOYQT_HEADLESS=ON
          -P "${CMAKE_CURRENT_LIST_DIR}/../../cmake/RunWindeployQt.cmake" VERBATIM)
      add_custom_command(TARGET PublicRoomSessionProof POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE:Qt6::Network>" "$<TARGET_FILE:Qt6::WebSockets>"
          "$<TARGET_FILE_DIR:PublicRoomSessionProof>" VERBATIM)
      add_custom_command(TARGET PublicRoomSessionProof POST_BUILD COMMAND "${CMAKE_COMMAND}"
          "-DWINDEPLOYQT_EXECUTABLE=${PROOF_WINDEPLOYQT_EXECUTABLE}"
          "-DWINDEPLOYQT_TARGET=$<TARGET_FILE:PublicRoomSessionProof>"
          "-DWINDEPLOYQT_DEBUG=$<CONFIG:Debug>"
          -DWINDEPLOYQT_HEADLESS=ON
          -P "${CMAKE_CURRENT_LIST_DIR}/../../cmake/RunWindeployQt.cmake" VERBATIM)
      add_test(NAME public-room-session-media COMMAND "${SCREENSHARE_NODE_EXECUTABLE}"
          "${CMAKE_CURRENT_LIST_DIR}/../../signaling-worker/tests/run-native-service.mjs"
          "$<TARGET_FILE:PublicRoomSessionProof>" "${CMAKE_CURRENT_BINARY_DIR}/public-room-evidence" media)
      set_tests_properties(public-room-session-media PROPERTIES TIMEOUT 90)
    add_test(NAME room-backed-four-peer-media COMMAND "${SCREENSHARE_NODE_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/../../signaling-worker/tests/run-native-service.mjs"
        "$<TARGET_FILE:RoomMediaProof>" "${CMAKE_CURRENT_BINARY_DIR}/room-media-evidence" media)
    set_tests_properties(room-backed-four-peer-media PROPERTIES TIMEOUT 90)
endif()
add_executable(HostPeerOwnerTest HostPeerOwnerTest.cpp)
add_executable(MediaEngineTest MediaEngineTest.cpp)
target_link_libraries(MediaEngineTest PRIVATE ScreenShareNegotiation MfWebRTCCodecs)
add_test(NAME media-engine-lifecycle COMMAND MediaEngineTest)
set_tests_properties(media-engine-lifecycle PROPERTIES TIMEOUT 30)
target_link_libraries(HostPeerOwnerTest PRIVATE ScreenShareNegotiation MfWebRTCCodecs)
add_test(NAME scheduled-peer-owner COMMAND HostPeerOwnerTest)
set_tests_properties(scheduled-peer-owner PROPERTIES TIMEOUT 20)
add_executable(CaptureSessionTest CaptureSessionTest.cpp)
target_link_libraries(CaptureSessionTest PRIVATE MfWebRTCCodecs)
add_test(NAME capture-session-headless COMMAND CaptureSessionTest)
set_tests_properties(capture-session-headless PROPERTIES TIMEOUT 20)
add_executable(CaptureDistributorTest CaptureDistributorTest.cpp)
target_link_libraries(CaptureDistributorTest PRIVATE MfWebRTCCodecs)
add_test(NAME capture-distributor-headless COMMAND CaptureDistributorTest)
set_tests_properties(capture-distributor-headless PROPERTIES TIMEOUT 15)
add_executable(StreamSettingsTest StreamSettingsTest.cpp)
target_link_libraries(StreamSettingsTest PRIVATE MfWebRTCCodecs)
add_test(NAME stream-settings-and-adaptation COMMAND StreamSettingsTest)
set_tests_properties(stream-settings-and-adaptation PROPERTIES TIMEOUT 15)
add_executable(HostMediaSessionTest HostMediaSessionTest.cpp)
target_link_libraries(HostMediaSessionTest PRIVATE MfWebRTCCodecs)
add_test(NAME host-media-session-coordinator COMMAND HostMediaSessionTest)
set_tests_properties(host-media-session-coordinator PROPERTIES TIMEOUT 30)
add_executable(MfDecoderAdapterTest MfDecoderAdapterTest.cpp)
target_link_libraries(MfDecoderAdapterTest PRIVATE MfWebRTCCodecs)
add_test(NAME mf-webrtc-decoder-lifecycle COMMAND MfDecoderAdapterTest)
set_tests_properties(mf-webrtc-decoder-lifecycle PROPERTIES TIMEOUT 30)
add_executable(MfEncoderAdapterTest MfEncoderAdapterTest.cpp)
add_executable(CaptureDispatcherTest CaptureDispatcherTest.cpp)
target_link_libraries(CaptureDispatcherTest PRIVATE MfWebRTCCodecs)
add_test(NAME capture-dispatcher-lifecycle COMMAND CaptureDispatcherTest)
set_tests_properties(capture-dispatcher-lifecycle PROPERTIES TIMEOUT 15)
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
add_executable(WindowsCaptureLifecycleTest WindowsCaptureLifecycleTest.cpp)
target_link_libraries(WindowsCaptureLifecycleTest PRIVATE MfWebRTCCodecs gdi32 rpcrt4)
add_executable(CaptureProcessExitTest CaptureProcessExitTest.cpp)
add_executable(CaptureRecoveryTest CaptureRecoveryTest.cpp)
add_executable(CaptureBackendTest CaptureBackendTest.cpp)
target_link_libraries(CaptureBackendTest PRIVATE MfWebRTCCodecs gdi32)
add_test(NAME capture-backend-cursor-policy COMMAND CaptureBackendTest)
set_tests_properties(capture-backend-cursor-policy PROPERTIES TIMEOUT 20)
target_link_libraries(CaptureRecoveryTest PRIVATE MfWebRTCCodecs gdi32)
add_test(NAME capture-recovery-policy COMMAND CaptureRecoveryTest)
set_tests_properties(capture-recovery-policy PROPERTIES TIMEOUT 10)
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
if(SCREENSHARE_NODE_EXECUTABLE AND TARGET PublicRoomSessionProof)
    add_executable(WindowsRoomSessionProof PublicRoomSessionProof.cpp)
    target_compile_definitions(WindowsRoomSessionProof PRIVATE SCREENSHARE_WINDOWS_ROOM_PROOF)
    target_link_libraries(WindowsRoomSessionProof PRIVATE MfWebRTCCodecs ScreenShareRoomSession gdi32)
    add_custom_command(TARGET WindowsRoomSessionProof POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE:Qt6::Network>" "$<TARGET_FILE:Qt6::WebSockets>"
        "$<TARGET_FILE_DIR:WindowsRoomSessionProof>" VERBATIM)
    if(SCREENSHARE_TEST_LIVE_CAPTURE)
        add_test(NAME windows-room-session-media COMMAND "${SCREENSHARE_NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/../../signaling-worker/tests/run-native-service.mjs"
            "$<TARGET_FILE:WindowsRoomSessionProof>" "${CMAKE_CURRENT_BINARY_DIR}/windows-room-evidence" windows-media)
        set_tests_properties(windows-room-session-media PROPERTIES TIMEOUT 90)
    endif()
endif()
if(SCREENSHARE_TEST_LIVE_CAPTURE)
    add_test(NAME capture-automatic-recovery COMMAND CaptureRecoveryTest --live)
    add_test(NAME capture-window-lifecycle COMMAND CaptureBackendTest --live)
    set_tests_properties(capture-window-lifecycle PROPERTIES TIMEOUT 30)
    set_tests_properties(capture-automatic-recovery PROPERTIES TIMEOUT 45)
    add_test(NAME presentation-gpu-recovery COMMAND PresentationRecoveryTest --gpu)
    set_tests_properties(presentation-gpu-recovery PROPERTIES TIMEOUT 20)
    add_test(NAME wgc-source-process-exit COMMAND CaptureProcessExitTest)
    set_tests_properties(wgc-source-process-exit PROPERTIES TIMEOUT 45)
    add_test(NAME wgc-owned-hardware-capture COMMAND LiveCaptureTest --repeat)
    add_test(NAME wgc-production-owner-lifecycle COMMAND WindowsCaptureLifecycleTest)
    set_tests_properties(wgc-production-owner-lifecycle PROPERTIES TIMEOUT 45)
    add_test(NAME webrtc-live-wgc-h264 COMMAND WebRTCProof --live-capture)
    set_tests_properties(webrtc-live-wgc-h264 PROPERTIES TIMEOUT 45)
    set_tests_properties(wgc-owned-hardware-capture PROPERTIES TIMEOUT 45)
endif()
if(SCREENSHARE_TEST_HARDWARE)
    add_test(NAME mf-hardware-decoder-recovery COMMAND MfDecoderAdapterTest --gpu)
    set_tests_properties(mf-hardware-decoder-recovery PROPERTIES TIMEOUT 30)
    add_test(NAME mf-hardware-fallback-and-gpu-lifetime COMMAND MfHardwareAdapterTest)
    add_test(NAME webrtc-direct-hardware-h264 COMMAND WebRTCProof --hardware)
    add_test(NAME mf-hardware-encoder-lifecycle COMMAND MfEncoderAdapterTest --hardware)
    add_test(NAME mf-hardware-owned-gpu-lifecycle COMMAND MfEncoderAdapterTest --hardware --gpu-input)
    set_tests_properties(mf-hardware-owned-gpu-lifecycle PROPERTIES TIMEOUT 45)
    set_tests_properties(mf-hardware-fallback-and-gpu-lifetime webrtc-direct-hardware-h264 mf-hardware-encoder-lifecycle PROPERTIES TIMEOUT 45)
endif()
add_executable(MfCodecProbe MfCodecProbe.cpp
    "${nativeRoot}/capture/DesktopCapturer.cpp"
    "${nativeRoot}/capture/DxgiCursor.cpp"
    "${nativeRoot}/codec/H264Bitstream.cpp"
    "${nativeRoot}/codec/H264StreamEncoder.cpp"
    "${nativeRoot}/codec/H264StreamDecoder.cpp"
    "${nativeRoot}/video/Nv12Convert.cpp")
target_compile_features(MfCodecProbe PRIVATE cxx_std_20)
target_include_directories(MfCodecProbe PRIVATE "${nativeRoot}")
target_compile_definitions(MfCodecProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(MfCodecProbe PRIVATE /EHsc /GR)
target_link_libraries(MfCodecProbe PRIVATE d3d11 d3dcompiler dxgi dxguid mf mfplat mfuuid wmcodecdspuuid ole32 user32 windowsapp CoreMessaging)
add_test(NAME mf-software-codec COMMAND MfCodecProbe)
set_tests_properties(mf-software-codec PROPERTIES TIMEOUT 30)

add_executable(IceCandidateHandoffTest IceCandidateHandoffTest.cpp)
target_link_libraries(IceCandidateHandoffTest PRIVATE MfWebRTCCodecs)
add_test(NAME ice-candidate-handoff COMMAND IceCandidateHandoffTest)
set_tests_properties(ice-candidate-handoff PROPERTIES TIMEOUT 10)

add_executable(PeerConnectionLifecycleTest PeerConnectionLifecycleTest.cpp)
target_link_libraries(PeerConnectionLifecycleTest PRIVATE MfWebRTCCodecs)
add_test(NAME peer-connection-lifecycle COMMAND PeerConnectionLifecycleTest)
set_tests_properties(peer-connection-lifecycle PROPERTIES TIMEOUT 10)

add_executable(HostPeerRegistryTest HostPeerRegistryTest.cpp)
target_link_libraries(HostPeerRegistryTest PRIVATE MfWebRTCCodecs)
add_test(NAME host-peer-ownership COMMAND HostPeerRegistryTest)
set_tests_properties(host-peer-ownership PROPERTIES TIMEOUT 10)
