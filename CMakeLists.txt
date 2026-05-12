cmake_minimum_required(VERSION 3.24)

project(ScreenShareNative LANGUAGES CXX)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(OPUS QUIET IMPORTED_TARGET opus)
endif()
if(OPUS_FOUND)
    set(SCREENSHARE_OPUS_TARGET PkgConfig::OPUS)
else()
    find_path(SCREENSHARE_OPUS_INCLUDE_DIR
        NAMES opus/opus.h
        PATHS ENV OPUS_ROOT
        PATH_SUFFIXES include
    )
    find_library(SCREENSHARE_OPUS_LIBRARY
        NAMES opus
        PATHS ENV OPUS_ROOT
        PATH_SUFFIXES lib
    )
    if(NOT SCREENSHARE_OPUS_INCLUDE_DIR OR NOT SCREENSHARE_OPUS_LIBRARY)
        message(FATAL_ERROR "Opus development files are required. Install mingw-w64-ucrt-x86_64-opus with MSYS2 or set OPUS_ROOT.")
    endif()
    add_library(ScreenShareOpus UNKNOWN IMPORTED)
    set_target_properties(ScreenShareOpus PROPERTIES
        IMPORTED_LOCATION "${SCREENSHARE_OPUS_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SCREENSHARE_OPUS_INCLUDE_DIR}"
    )
    set(SCREENSHARE_OPUS_TARGET ScreenShareOpus)
endif()

add_executable(ScreenShare
    src/app/main.cpp
    src/audio/OpusCodec.cpp
    src/audio/WasapiCapture.cpp
    src/audio/WasapiRenderer.cpp
    src/capture/DesktopCapturer.cpp
    src/codec/H264Bitstream.cpp
    src/codec/H264FileEncoder.cpp
    src/codec/H264EncoderProbe.cpp
    src/codec/H264StreamDecoder.cpp
    src/codec/H264StreamEncoder.cpp
    src/render/ReceiverPreviewWindow.cpp
    src/transport/UdpProtocol.cpp
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
    ${SCREENSHARE_OPUS_TARGET}
    d3d11
    d3dcompiler
    dxgi
    dxguid
    ksuser
    mf
    mfplat
    mfreadwrite
    mfuuid
    mmdevapi
    ole32
    propsys
    user32
    uuid
    windowsapp
    ws2_32
)
