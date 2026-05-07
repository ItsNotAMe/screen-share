cmake_minimum_required(VERSION 3.24)

project(ScreenShareNative LANGUAGES CXX)

add_executable(ScreenShare
    src/app/main.cpp
    src/capture/DesktopCapturer.cpp
    src/codec/H264FileEncoder.cpp
    src/codec/H264EncoderProbe.cpp
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

if(WIN32)
    file(GLOB _cppwinrt_include_dirs LIST_DIRECTORIES true
        "C:/Program Files (x86)/Windows Kits/10/Include/*/cppwinrt"
    )
    if(_cppwinrt_include_dirs)
        list(SORT _cppwinrt_include_dirs)
        list(REVERSE _cppwinrt_include_dirs)
        list(GET _cppwinrt_include_dirs 0 _cppwinrt_include_dir)
        target_include_directories(ScreenShare PRIVATE "${_cppwinrt_include_dir}")
    endif()
endif()

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
    windowsapp
    ws2_32
)
