cmake_minimum_required(VERSION 3.24)

project(ScreenShareNative LANGUAGES CXX)

add_executable(ScreenShare
    src/app/main.cpp
    src/capture/DesktopCapturer.cpp
    src/codec/H264FileEncoder.cpp
    src/codec/H264StreamDecoder.cpp
    src/codec/H264StreamEncoder.cpp
    src/render/ReceiverPreviewWindow.cpp
    src/transport/UdpReceiver.cpp
    src/transport/UdpSender.cpp
    src/video/Nv12Convert.cpp
)

target_compile_features(ScreenShare PRIVATE cxx_std_20)

target_include_directories(ScreenShare PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(ScreenShare PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(ScreenShare PRIVATE /W4 /permissive- /EHsc)
endif()

target_link_libraries(ScreenShare PRIVATE
    d3d11
    d3dcompiler
    dxgi
    dxguid
    mf
    mfplat
    mfreadwrite
    mfuuid
    user32
    ws2_32
)
