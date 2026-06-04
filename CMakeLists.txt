cmake_minimum_required(VERSION 3.24)

project(ScreenShareNative VERSION 0.1.0 LANGUAGES CXX)

option(SCREENSHARE_STATIC_MINGW_RUNTIME "Statically link MinGW GCC/C++/pthread runtime libraries" ON)
option(SCREENSHARE_COPY_RUNTIME_DLLS "Copy remaining runtime DLLs next to ScreenShare.exe after build" ON)
option(SCREENSHARE_PACKAGE_PORTABLE_ON_BUILD "Create the portable zip package after the normal ScreenShare build" ON)
option(SCREENSHARE_BUILD_QT_UI "Build the Qt desktop control UI when Qt 6 Widgets and Svg are available" ON)
option(SCREENSHARE_ENABLE_RELEASE_LTO "Enable compiler-supported link-time optimization for Release builds" ON)
set(SCREENSHARE_UPDATE_MANIFEST_URL
    "https://github.com/ItsNotAMe/screen-share/releases/latest/download/screenshare-update.json"
    CACHE STRING
    "HTTPS URL for the ScreenShare auto-update manifest")

if(SCREENSHARE_ENABLE_RELEASE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT SCREENSHARE_RELEASE_LTO_SUPPORTED OUTPUT SCREENSHARE_RELEASE_LTO_OUTPUT)
    if(SCREENSHARE_RELEASE_LTO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        message(STATUS "Release LTO enabled")
    else()
        message(WARNING "Release LTO requested but not supported by this toolchain: ${SCREENSHARE_RELEASE_LTO_OUTPUT}")
    endif()
endif()

function(screenshare_configure_release_lto_warnings target_name)
    if(SCREENSHARE_ENABLE_RELEASE_LTO AND SCREENSHARE_RELEASE_LTO_SUPPORTED AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # GCC's LTO ODR checker can warn on equivalent Windows API declarations from
        # C++/WinRT and MinGW headers, such as GetProcAddress/FARPROC at executable link time.
        target_link_options(${target_name} PRIVATE $<$<CONFIG:Release>:-Wno-odr>)
    endif()
endfunction()

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

add_library(ScreenShareCore STATIC
    src/audio/OpusCodec.cpp
    src/audio/WasapiCapture.cpp
    src/audio/WasapiRenderer.cpp
    src/capture/DesktopCapturer.cpp
    src/codec/H264Bitstream.cpp
    src/codec/H264FileEncoder.cpp
    src/codec/H264EncoderProbe.cpp
    src/codec/H264StreamDecoder.cpp
    src/codec/H264StreamEncoder.cpp
    src/core/ScreenShareSession.cpp
    src/core/SessionCommand.cpp
    src/core/SessionRuntimeControl.cpp
    src/render/Nv12D3D11Presenter.cpp
    src/render/ReceiverPreviewWindow.cpp
    src/transport/NatTraversal.cpp
    src/transport/SignalingClient.cpp
    src/transport/StunClient.cpp
    src/transport/UdpCrypto.cpp
    src/transport/LanDiscovery.cpp
    src/transport/UdpProtocol.cpp
    src/transport/UdpReceiver.cpp
    src/transport/UdpSender.cpp
    src/video/Nv12Convert.cpp
)

target_compile_features(ScreenShareCore PUBLIC cxx_std_20)

target_include_directories(ScreenShareCore PUBLIC
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
        target_include_directories(ScreenShareCore PUBLIC "${_cppwinrt_include_dir}")
    endif()
endif()

target_compile_definitions(ScreenShareCore PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(ScreenShareCore PRIVATE /W4 /permissive- /EHsc)
endif()

target_link_libraries(ScreenShareCore PUBLIC
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
    bcrypt
    iphlpapi
    winhttp
    ws2_32
)

add_library(ScreenShareAPI STATIC
    src/api/ScreenShareAPI.cpp
    src/runtime/ScreenShareRuntimeExecution.cpp
    src/runtime/ScreenShareRuntimeSupport.cpp
    src/runtime/ScreenShareSessionOptions.cpp
    src/runtime/ScreenShareSessionRunner.cpp
)

target_compile_features(ScreenShareAPI PUBLIC cxx_std_20)

target_include_directories(ScreenShareAPI PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(ScreenShareAPI PUBLIC
    ScreenShareCore
)

if(MSVC)
    target_compile_options(ScreenShareAPI PRIVATE /W4 /permissive- /EHsc)
endif()

add_executable(ScreenShare
    src/cli/ScreenShareMain.cpp
    src/cli/ScreenShareCLI.cpp
)

target_compile_features(ScreenShare PRIVATE cxx_std_20)

if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND SCREENSHARE_STATIC_MINGW_RUNTIME)
    target_link_options(ScreenShare PRIVATE -static)
endif()

screenshare_configure_release_lto_warnings(ScreenShare)

target_link_libraries(ScreenShare PRIVATE
    ScreenShareAPI
)

if(MSVC)
    target_compile_options(ScreenShare PRIVATE /W4 /permissive- /EHsc)
endif()

add_executable(ScreenShareUpdater WIN32
    src/updater/ScreenShareUpdater.cpp
)

target_compile_features(ScreenShareUpdater PRIVATE cxx_std_20)
target_compile_definitions(ScreenShareUpdater PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND SCREENSHARE_STATIC_MINGW_RUNTIME)
    target_link_options(ScreenShareUpdater PRIVATE -static)
endif()

screenshare_configure_release_lto_warnings(ScreenShareUpdater)

target_link_libraries(ScreenShareUpdater PRIVATE
    shell32
)

if(MSVC)
    target_compile_options(ScreenShareUpdater PRIVATE /W4 /permissive- /EHsc)
endif()

if(SCREENSHARE_BUILD_QT_UI)
    if(WIN32 AND EXISTS "C:/msys64/ucrt64/lib/cmake/Qt6")
        list(APPEND CMAKE_PREFIX_PATH "C:/msys64/ucrt64")
    endif()

    find_package(Qt6 QUIET COMPONENTS Network Svg Widgets)
    if(Qt6_FOUND)
        set(SCREENSHARE_UI_ICON_RC)
        if(WIN32)
            set(SCREENSHARE_UI_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/assets/brand/screenshare.ico")
            set(SCREENSHARE_UI_ICON_RC "${CMAKE_CURRENT_BINARY_DIR}/ScreenShareUiIcon.rc")
            configure_file(
                "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ScreenShareUiIcon.rc.in"
                "${SCREENSHARE_UI_ICON_RC}"
                @ONLY
            )
            if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
                find_program(SCREENSHARE_WINDRES_EXECUTABLE
                    NAMES windres windres.exe
                    HINTS
                        "C:/msys64/ucrt64/bin"
                        "C:/msys64/usr/bin"
                )
                if(NOT SCREENSHARE_WINDRES_EXECUTABLE)
                    message(FATAL_ERROR "windres is required to embed the ScreenShareUi Windows icon with MinGW")
                endif()

                set(SCREENSHARE_UI_ICON_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/ScreenShareUiIcon.res.obj")
                add_custom_command(
                    OUTPUT "${SCREENSHARE_UI_ICON_OBJECT}"
                    COMMAND "${SCREENSHARE_WINDRES_EXECUTABLE}"
                        -O coff
                        -i "${SCREENSHARE_UI_ICON_RC}"
                        -o "${SCREENSHARE_UI_ICON_OBJECT}"
                    DEPENDS
                        "${SCREENSHARE_UI_ICON_RC}"
                        "${SCREENSHARE_UI_ICON_FILE}"
                    COMMENT "Compiling ScreenShareUi Windows icon resource"
                    VERBATIM
                )
                set_source_files_properties("${SCREENSHARE_UI_ICON_OBJECT}" PROPERTIES
                    EXTERNAL_OBJECT TRUE
                    GENERATED TRUE
                )
                set(SCREENSHARE_UI_ICON_RC "${SCREENSHARE_UI_ICON_OBJECT}")
            else()
                enable_language(RC)
            endif()
        endif()

        add_executable(ScreenShareUi WIN32
            src/ui/main.cpp
            src/ui/QtSessionBackend.cpp
            src/ui/ActiveShareWindow.cpp
            src/ui/ActiveWatchWindow.cpp
            src/ui/AppShellWindow.cpp
            src/ui/CreateRoomWindow.cpp
            src/ui/HomeWindow.cpp
            src/ui/JoinRoomWindow.cpp
            src/ui/RoomAccessCheck.cpp
            src/ui/UiStyle.cpp
            src/ui/UpdateManager.cpp
            src/ui/VideoFrameWidget.cpp
            src/ui/resources.qrc
            ${SCREENSHARE_UI_ICON_RC}
        )
        set_property(TARGET ScreenShareUi PROPERTY AUTORCC ON)
        target_compile_features(ScreenShareUi PRIVATE cxx_std_20)
        target_include_directories(ScreenShareUi PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        target_link_libraries(ScreenShareUi PRIVATE ScreenShareAPI Qt6::Network Qt6::Svg Qt6::Widgets shell32)
        target_compile_definitions(ScreenShareUi PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            SCREENSHARE_APP_VERSION="${PROJECT_VERSION}"
            SCREENSHARE_UPDATE_MANIFEST_URL="${SCREENSHARE_UPDATE_MANIFEST_URL}"
        )
        screenshare_configure_release_lto_warnings(ScreenShareUi)
        add_dependencies(ScreenShareUi ScreenShare)

        if(MSVC)
            target_compile_options(ScreenShareUi PRIVATE /W4 /permissive- /EHsc)
        endif()

        find_program(SCREENSHARE_WINDEPLOYQT_EXECUTABLE
            NAMES windeployqt6 windeployqt-qt6 windeployqt
            HINTS
                "C:/msys64/ucrt64/bin"
        )
        if(WIN32 AND SCREENSHARE_WINDEPLOYQT_EXECUTABLE)
            add_custom_command(TARGET ScreenShareUi POST_BUILD
                COMMAND "${CMAKE_COMMAND}"
                    "-DWINDEPLOYQT_EXECUTABLE=${SCREENSHARE_WINDEPLOYQT_EXECUTABLE}"
                    "-DWINDEPLOYQT_TARGET=$<TARGET_FILE:ScreenShareUi>"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunWindeployQt.cmake"
                COMMENT "Deploying Qt runtime next to ScreenShareUi.exe"
                VERBATIM
            )
        endif()
    else()
        message(WARNING "Qt 6 Network, Svg, and Widgets were not found; ScreenShareUi target will not be built")
    endif()
endif()

if(WIN32 AND SCREENSHARE_COPY_RUNTIME_DLLS AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(SCREENSHARE_RUNTIME_DLL_NAMES
        libopus-0.dll
    )
    set(SCREENSHARE_STATIC_RUNTIME_DLL_NAMES
        libgcc_s_seh-1.dll
        libstdc++-6.dll
        libwinpthread-1.dll
    )
    if(NOT SCREENSHARE_STATIC_MINGW_RUNTIME)
        list(APPEND SCREENSHARE_RUNTIME_DLL_NAMES ${SCREENSHARE_STATIC_RUNTIME_DLL_NAMES})
    endif()

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(SCREENSHARE_WINDOWS_REDIST_ARCH x64)
    else()
        set(SCREENSHARE_WINDOWS_REDIST_ARCH x86)
    endif()

    get_filename_component(SCREENSHARE_COMPILER_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(SCREENSHARE_RUNTIME_DLL_DIRS "${SCREENSHARE_COMPILER_BIN_DIR}")

    if(DEFINED OPUS_PREFIX AND OPUS_PREFIX)
        list(APPEND SCREENSHARE_RUNTIME_DLL_DIRS "${OPUS_PREFIX}/bin")
    endif()
    if(DEFINED SCREENSHARE_OPUS_LIBRARY AND SCREENSHARE_OPUS_LIBRARY)
        get_filename_component(SCREENSHARE_OPUS_LIB_DIR "${SCREENSHARE_OPUS_LIBRARY}" DIRECTORY)
        get_filename_component(SCREENSHARE_OPUS_PREFIX "${SCREENSHARE_OPUS_LIB_DIR}" DIRECTORY)
        list(APPEND SCREENSHARE_RUNTIME_DLL_DIRS "${SCREENSHARE_OPUS_PREFIX}/bin")
    endif()
    list(REMOVE_DUPLICATES SCREENSHARE_RUNTIME_DLL_DIRS)

    foreach(SCREENSHARE_RUNTIME_DLL_NAME IN LISTS SCREENSHARE_RUNTIME_DLL_NAMES)
        string(MAKE_C_IDENTIFIER "${SCREENSHARE_RUNTIME_DLL_NAME}" SCREENSHARE_RUNTIME_DLL_ID)
        set(SCREENSHARE_RUNTIME_DLL_VAR "SCREENSHARE_RUNTIME_DLL_${SCREENSHARE_RUNTIME_DLL_ID}")
        find_file(${SCREENSHARE_RUNTIME_DLL_VAR}
            NAMES "${SCREENSHARE_RUNTIME_DLL_NAME}"
            PATHS ${SCREENSHARE_RUNTIME_DLL_DIRS}
            NO_DEFAULT_PATH
        )
        if(${SCREENSHARE_RUNTIME_DLL_VAR})
            list(APPEND SCREENSHARE_RUNTIME_DLLS "${${SCREENSHARE_RUNTIME_DLL_VAR}}")
        else()
            message(WARNING "Could not find ${SCREENSHARE_RUNTIME_DLL_NAME}; portable builds may require it next to ScreenShare.exe")
        endif()
    endforeach()

    set(SCREENSHARE_WINDOWS_KITS_REDIST_ROOT "C:/Program Files (x86)/Windows Kits/10/Redist")
    file(GLOB SCREENSHARE_UCRT_REDIST_DIRS LIST_DIRECTORIES true
        "${SCREENSHARE_WINDOWS_KITS_REDIST_ROOT}/*/ucrt/DLLs/${SCREENSHARE_WINDOWS_REDIST_ARCH}"
        "${SCREENSHARE_WINDOWS_KITS_REDIST_ROOT}/ucrt/DLLs/${SCREENSHARE_WINDOWS_REDIST_ARCH}"
    )
    if(SCREENSHARE_UCRT_REDIST_DIRS)
        list(SORT SCREENSHARE_UCRT_REDIST_DIRS)
        list(REVERSE SCREENSHARE_UCRT_REDIST_DIRS)
        list(GET SCREENSHARE_UCRT_REDIST_DIRS 0 SCREENSHARE_UCRT_REDIST_DIR)
        file(GLOB SCREENSHARE_UCRT_RUNTIME_DLLS
            "${SCREENSHARE_UCRT_REDIST_DIR}/api-ms-win-crt-*.dll"
            "${SCREENSHARE_UCRT_REDIST_DIR}/ucrtbase.dll"
        )
        list(APPEND SCREENSHARE_RUNTIME_DLLS ${SCREENSHARE_UCRT_RUNTIME_DLLS})
    else()
        message(WARNING "Could not find Windows SDK UCRT redist DLLs; older Windows installs may need the Universal C Runtime installed")
    endif()

    file(GLOB SCREENSHARE_D3D_COMPILER_DLLS
        "${SCREENSHARE_WINDOWS_KITS_REDIST_ROOT}/D3D/${SCREENSHARE_WINDOWS_REDIST_ARCH}/d3dcompiler_47.dll"
        "${SCREENSHARE_WINDOWS_KITS_REDIST_ROOT}/*/D3D/${SCREENSHARE_WINDOWS_REDIST_ARCH}/d3dcompiler_47.dll"
    )
    if(SCREENSHARE_D3D_COMPILER_DLLS)
        list(SORT SCREENSHARE_D3D_COMPILER_DLLS)
        list(REVERSE SCREENSHARE_D3D_COMPILER_DLLS)
        list(GET SCREENSHARE_D3D_COMPILER_DLLS 0 SCREENSHARE_D3D_COMPILER_DLL)
        list(APPEND SCREENSHARE_RUNTIME_DLLS "${SCREENSHARE_D3D_COMPILER_DLL}")
    else()
        message(WARNING "Could not find d3dcompiler_47.dll in the Windows SDK redist")
    endif()

    list(REMOVE_DUPLICATES SCREENSHARE_RUNTIME_DLLS)

    if(SCREENSHARE_RUNTIME_DLLS)
        string(REPLACE ";" "\n" SCREENSHARE_RUNTIME_DLL_REPORT "${SCREENSHARE_RUNTIME_DLLS}")
        add_custom_command(TARGET ScreenShare POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -f
                $<$<BOOL:${SCREENSHARE_STATIC_MINGW_RUNTIME}>:$<TARGET_FILE_DIR:ScreenShare>/libgcc_s_seh-1.dll>
                $<$<BOOL:${SCREENSHARE_STATIC_MINGW_RUNTIME}>:$<TARGET_FILE_DIR:ScreenShare>/libstdc++-6.dll>
                $<$<BOOL:${SCREENSHARE_STATIC_MINGW_RUNTIME}>:$<TARGET_FILE_DIR:ScreenShare>/libwinpthread-1.dll>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SCREENSHARE_RUNTIME_DLLS}
                $<TARGET_FILE_DIR:ScreenShare>
            COMMAND_EXPAND_LISTS
            COMMENT "Copying runtime DLLs next to ScreenShare.exe"
            VERBATIM
        )
        file(GENERATE
            OUTPUT "$<TARGET_FILE_DIR:ScreenShare>/ScreenShare-runtime-dependencies.txt"
            CONTENT "${SCREENSHARE_RUNTIME_DLL_REPORT}\n"
        )
    endif()
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(SCREENSHARE_PACKAGE_ARCH x64)
else()
    set(SCREENSHARE_PACKAGE_ARCH x86)
endif()
string(TOLOWER "${CMAKE_SYSTEM_NAME}" SCREENSHARE_PACKAGE_SYSTEM)
if(CMAKE_BUILD_TYPE)
    string(TOLOWER "${CMAKE_BUILD_TYPE}" SCREENSHARE_PACKAGE_CONFIG)
else()
    set(SCREENSHARE_PACKAGE_CONFIG build)
endif()
set(SCREENSHARE_PACKAGE_BASENAME
    "ScreenShare-${SCREENSHARE_PACKAGE_CONFIG}-${SCREENSHARE_PACKAGE_SYSTEM}-${SCREENSHARE_PACKAGE_ARCH}"
)
set(SCREENSHARE_PACKAGE_RUNTIME_SEARCH_DIRS "$<TARGET_FILE_DIR:ScreenShare>")
if(CMAKE_CXX_COMPILER)
    get_filename_component(SCREENSHARE_PACKAGE_COMPILER_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    list(APPEND SCREENSHARE_PACKAGE_RUNTIME_SEARCH_DIRS "${SCREENSHARE_PACKAGE_COMPILER_BIN_DIR}")
endif()
if(WIN32 AND EXISTS "C:/msys64/ucrt64/bin")
    list(APPEND SCREENSHARE_PACKAGE_RUNTIME_SEARCH_DIRS "C:/msys64/ucrt64/bin")
endif()
list(REMOVE_DUPLICATES SCREENSHARE_PACKAGE_RUNTIME_SEARCH_DIRS)
string(REPLACE ";" "|" SCREENSHARE_PACKAGE_RUNTIME_SEARCH_DIRS_ARG "${SCREENSHARE_PACKAGE_RUNTIME_SEARCH_DIRS}")
set(SCREENSHARE_PACKAGE_ARGS
    -D "INPUT_DIR=$<TARGET_FILE_DIR:ScreenShare>"
    -D "EXECUTABLE_NAME=$<TARGET_FILE_NAME:ScreenShare>"
    -D "SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    -D "STAGING_DIR=${CMAKE_BINARY_DIR}/package/${SCREENSHARE_PACKAGE_BASENAME}"
    -D "OUTPUT_ZIP=${CMAKE_BINARY_DIR}/${SCREENSHARE_PACKAGE_BASENAME}.zip"
    -D "RUNTIME_SEARCH_DIRS=${SCREENSHARE_PACKAGE_RUNTIME_SEARCH_DIRS_ARG}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackagePortable.cmake"
)
set(SCREENSHARE_PACKAGE_DEPENDS ScreenShare ScreenShareUpdater)
if(TARGET ScreenShareUi)
    list(APPEND SCREENSHARE_PACKAGE_DEPENDS ScreenShareUi)
endif()

if(SCREENSHARE_PACKAGE_PORTABLE_ON_BUILD)
    add_custom_target(package-portable ALL
        COMMAND ${CMAKE_COMMAND} ${SCREENSHARE_PACKAGE_ARGS}
        DEPENDS ${SCREENSHARE_PACKAGE_DEPENDS}
        COMMENT "Creating portable ScreenShare zip package"
        VERBATIM
    )
else()
    add_custom_target(package-portable
        COMMAND ${CMAKE_COMMAND} ${SCREENSHARE_PACKAGE_ARGS}
        DEPENDS ${SCREENSHARE_PACKAGE_DEPENDS}
        COMMENT "Creating portable ScreenShare zip package"
        VERBATIM
    )
endif()
