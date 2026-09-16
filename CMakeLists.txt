cmake_minimum_required(VERSION 3.24)

project(ScreenShareNative VERSION 0.3.4 LANGUAGES CXX)

# C++/WinRT factory-cache clearing uses a 128-bit interlocked operation on x64.
set_property(SOURCE backend/capture/DesktopCapturer.cpp APPEND PROPERTY COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:Clang>:/clang:-mcx16>")

include(CTest)

option(SCREENSHARE_STATIC_MINGW_RUNTIME "Statically link MinGW GCC/C++/pthread runtime libraries" ON)
option(SCREENSHARE_COPY_RUNTIME_DLLS "Copy remaining runtime DLLs next to ScreenShare.exe after build" ON)
option(SCREENSHARE_PACKAGE_PORTABLE_ON_BUILD "Create the portable zip package after the normal ScreenShare build" ON)
option(SCREENSHARE_BUILD_INSTALLER "Create an elevated ScreenShare Setup that provisions controller support" OFF)
option(SCREENSHARE_BUILD_QT_UI "Build the Qt desktop control UI when Qt 6 Widgets and Svg are available" ON)
option(SCREENSHARE_ENABLE_RELEASE_LTO "Enable compiler-supported link-time optimization for Release builds" ON)
option(SCREENSHARE_BUILD_PINNED_VIGEMCLIENT "Build the pinned ViGEm user-mode client from source" ON)
set(SCREENSHARE_VIGEMCLIENT_DLL "" CACHE FILEPATH
    "Optional prebuilt ViGEmClient.dll override; normally the pinned client is built from source")
set(SCREENSHARE_VIGEMCLIENT_LICENSE "" CACHE FILEPATH
    "License file accompanying a prebuilt SCREENSHARE_VIGEMCLIENT_DLL")
set(SCREENSHARE_VIGEMCLIENT_SOURCE_DIR "" CACHE PATH
    "Optional local ViGEmClient v1.16.18.0 source tree; otherwise CMake downloads the hash-pinned source archive")
set(SCREENSHARE_CONTROLLER_DRIVER_SETUP "" CACHE FILEPATH
    "Signed controller driver setup embedded in ScreenShare Setup and executed only during installation/update")
set(SCREENSHARE_CONTROLLER_DRIVER_SETUP_SHA256
    "89220A7865076B342892F98865F3499FB7C4CFD673159E89D352C360FD014C6A"
    CACHE STRING
    "Required SHA-256 of SCREENSHARE_CONTROLLER_DRIVER_SETUP")
set(SCREENSHARE_INNO_SETUP_COMPILER "" CACHE FILEPATH
    "Optional path to Inno Setup ISCC.exe used for package-installer")
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

include(cmake/WebRTC.cmake)
if(SCREENSHARE_WEBRTC_ARTIFACT_DIR)
    screenshare_import_webrtc()
    include(cmake/MediaNegotiation.cmake)
    # The complete WebRTC archive includes its DEPS-pinned Opus implementation.
    # The legacy audio adapter uses it during the build proof; v2 uses the ADM.
    set(SCREENSHARE_OPUS_TARGET ScreenShare::WebRTC)
elseif(NOT MSVC)
    find_package(PkgConfig QUIET)
endif()
if(NOT SCREENSHARE_OPUS_TARGET AND PkgConfig_FOUND)
    pkg_check_modules(OPUS QUIET IMPORTED_TARGET opus)
endif()
if(SCREENSHARE_OPUS_TARGET)
    # Verified native artifact selected above.
elseif(OPUS_FOUND)
    set(SCREENSHARE_OPUS_TARGET PkgConfig::OPUS)
else()
    find_path(SCREENSHARE_OPUS_INCLUDE_DIR
        NAMES opus.h
        PATHS ENV OPUS_ROOT
        PATH_SUFFIXES include/opus include
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

if(SCREENSHARE_VIGEMCLIENT_DLL AND SCREENSHARE_BUILD_PINNED_VIGEMCLIENT)
    message(FATAL_ERROR
        "Choose either SCREENSHARE_VIGEMCLIENT_DLL or SCREENSHARE_BUILD_PINNED_VIGEMCLIENT, not both")
endif()

if(SCREENSHARE_BUILD_PINNED_VIGEMCLIENT)
    if(SCREENSHARE_VIGEMCLIENT_SOURCE_DIR)
        set(_screenshare_vigemclient_source "${SCREENSHARE_VIGEMCLIENT_SOURCE_DIR}")
        if(NOT EXISTS "${_screenshare_vigemclient_source}/src/ViGEmClient.cpp" OR
           NOT EXISTS "${_screenshare_vigemclient_source}/include/ViGEmClient.h")
            message(FATAL_ERROR
                "SCREENSHARE_VIGEMCLIENT_SOURCE_DIR is not a ViGEmClient source tree: "
                "${_screenshare_vigemclient_source}")
        endif()
    else()
        include(FetchContent)
        FetchContent_Declare(screenshare_vigemclient_source
            URL "https://github.com/nefarius/ViGEmClient/archive/refs/tags/v1.16.18.0.zip"
            URL_HASH "SHA256=72256A77B6B68E4D57DF91052A2862BC07FBC6B292E21BD4492225DB21B12ACA"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_GetProperties(screenshare_vigemclient_source)
        if(NOT screenshare_vigemclient_source_POPULATED)
            FetchContent_Populate(screenshare_vigemclient_source)
        endif()
        set(_screenshare_vigemclient_source "${screenshare_vigemclient_source_SOURCE_DIR}")
    endif()

    add_library(ScreenShareViGEmClient SHARED
        "${_screenshare_vigemclient_source}/src/ViGEmClient.cpp"
    )
    target_compile_features(ScreenShareViGEmClient PRIVATE cxx_std_17)
    target_compile_definitions(ScreenShareViGEmClient PRIVATE
        VIGEM_DYNAMIC
        VIGEM_EXPORTS
        _LIB
        NOMINMAX
    )
    if(MSVC)
        target_compile_options(ScreenShareViGEmClient PRIVATE /FIthread)
    else()
        target_compile_options(ScreenShareViGEmClient PRIVATE -include thread)
    endif()
    target_include_directories(ScreenShareViGEmClient PRIVATE
        "${_screenshare_vigemclient_source}/include"
    )
    target_link_libraries(ScreenShareViGEmClient PRIVATE setupapi)
    if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND SCREENSHARE_STATIC_MINGW_RUNTIME)
        target_link_options(ScreenShareViGEmClient PRIVATE -static)
    endif()
    set_target_properties(ScreenShareViGEmClient PROPERTIES
        OUTPUT_NAME ViGEmClient
        PREFIX ""
    )
    set(SCREENSHARE_BUILT_VIGEMCLIENT_LICENSE
        "${_screenshare_vigemclient_source}/LICENSE")
endif()

add_library(ScreenShareCore STATIC
    backend/media/HostMediaSession.cpp
    backend/media/capture/CaptureSession.cpp
    backend/media/capture/CaptureDistributor.cpp
    backend/audio/OpusCodec.cpp
    backend/audio/WasapiCapture.cpp
    backend/audio/WasapiRenderer.cpp
    backend/capture/DesktopCapturer.cpp
    backend/codec/H264Bitstream.cpp
    backend/codec/H264FileEncoder.cpp
    backend/codec/H264EncoderProbe.cpp
    backend/codec/H264StreamDecoder.cpp
    backend/codec/H264StreamEncoder.cpp
    backend/core/ScreenShareSession.cpp
    backend/core/SessionCommand.cpp
    backend/core/SessionRuntimeControl.cpp
    backend/input/RemoteInputInjector.cpp
    backend/input/ViewerGamepad.cpp
    backend/input/VirtualGamepadBackend.cpp
    backend/input/XInputGamepad.cpp
    backend/render/Nv12D3D11Presenter.cpp
    backend/render/FramePresentationBackend.cpp
    backend/render/ReceiverPreviewWindow.cpp
    backend/transport/NatTraversal.cpp
    backend/transport/SignalingClient.cpp
    backend/transport/StunClient.cpp
    backend/transport/UdpCrypto.cpp
    backend/transport/LanDiscovery.cpp
    backend/transport/UdpProtocol.cpp
    backend/transport/UdpReceiver.cpp
    backend/transport/UdpSender.cpp
    backend/updater/UpdateSignature.cpp
    backend/video/Nv12Convert.cpp
)

target_compile_features(ScreenShareCore PUBLIC cxx_std_20)

target_include_directories(ScreenShareCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/backend
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
    CoreMessaging
    bcrypt
    iphlpapi
    hid
    setupapi
    winhttp
    ws2_32
)
if(MSVC)
    target_link_libraries(ScreenShareCore PUBLIC wmcodecdspuuid)
endif()
if(TARGET ScreenShareNegotiation)
    target_link_libraries(ScreenShareCore PRIVATE ScreenShareNegotiation)
endif()
if(MSVC)
    # Room transport is a backend dependency, including CLI-only builds.
    include(cmake/RoomTransport.cmake)
    target_link_libraries(ScreenShareCore PRIVATE ScreenShareRoom)
    if(TARGET ScreenShareNegotiation)
        include(cmake/RoomSession.cmake)
        target_link_libraries(ScreenShareCore PRIVATE ScreenShareRoomSession)
        if(BUILD_TESTING)
            add_executable(NativeRoomRuntimeTests tests/NativeRoomRuntimeTests.cpp)
            target_link_libraries(NativeRoomRuntimeTests PRIVATE ScreenShareCore ScreenShareRoomSession)
            add_test(NAME native-room-runtime-link COMMAND NativeRoomRuntimeTests)
            set_tests_properties(native-room-runtime-link PROPERTIES TIMEOUT 15)
        endif()
    endif()
    if(BUILD_TESTING)
        add_executable(RoomSocketTests tests/RoomSocketTests.cpp)
        add_executable(RoomAdmissionTests tests/RoomAdmissionTests.cpp)
        add_executable(RoomServiceTests tests/RoomServiceTests.cpp)
        target_link_libraries(RoomServiceTests PRIVATE ScreenShareRoom)
        add_dependencies(RoomServiceTests RoomSocketTests)
        find_program(SCREENSHARE_NODE_EXECUTABLE node)
        if(SCREENSHARE_NODE_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/signaling-worker/node_modules/miniflare/package.json")
            add_test(NAME room-v2-native-worker COMMAND "${SCREENSHARE_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/signaling-worker/tests/run-native-service.mjs"
                "$<TARGET_FILE:RoomServiceTests>" "${CMAKE_CURRENT_BINARY_DIR}/room-service-evidence")
            set_tests_properties(room-v2-native-worker PROPERTIES TIMEOUT 90)
        else()
            message(STATUS "Native Worker integration test requires Node and signaling-worker npm dependencies")
        endif()
        target_link_libraries(RoomAdmissionTests PRIVATE ScreenShareRoom)
        add_test(NAME room-v2-admission COMMAND RoomAdmissionTests)
        set_tests_properties(room-v2-admission PROPERTIES TIMEOUT 30)
        # RoomSocketTests stages Qt DLLs into the shared test output directory.
        add_dependencies(RoomAdmissionTests RoomSocketTests)
        target_link_libraries(RoomSocketTests PRIVATE ScreenShareRoom)
        add_custom_command(TARGET RoomSocketTests POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE:Qt6::Network>" "$<TARGET_FILE:Qt6::WebSockets>"
                "$<TARGET_FILE_DIR:RoomSocketTests>"
            VERBATIM)
        add_test(NAME room-v2-live-socket COMMAND RoomSocketTests)
        set_tests_properties(room-v2-live-socket PROPERTIES TIMEOUT 75)
    endif()
endif()

add_library(ScreenShareAPI STATIC
    backend/api/ScreenShareAPI.cpp
    backend/runtime/ScreenShareRuntimeExecution.cpp
    backend/runtime/ScreenShareRuntimeSupport.cpp
    backend/runtime/ScreenShareSessionOptions.cpp
    backend/runtime/ScreenShareSessionRunner.cpp
)

target_compile_features(ScreenShareAPI PUBLIC cxx_std_20)

target_include_directories(ScreenShareAPI PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/backend
)

target_link_libraries(ScreenShareAPI PUBLIC
    ScreenShareCore
)

if(BUILD_TESTING)
    add_executable(ControllerProtocolTests
        tests/ControllerProtocolTests.cpp
    )
    target_compile_features(ControllerProtocolTests PRIVATE cxx_std_20)
    target_link_libraries(ControllerProtocolTests PRIVATE ScreenShareCore)
    add_test(NAME controller-protocol COMMAND ControllerProtocolTests)

    add_executable(ViewerGamepadReportTests
        tests/ViewerGamepadReportTests.cpp
    )
    target_compile_features(ViewerGamepadReportTests PRIVATE cxx_std_20)
    target_link_libraries(ViewerGamepadReportTests PRIVATE ScreenShareCore)
    add_test(NAME viewer-gamepad-reports COMMAND ViewerGamepadReportTests)

    add_executable(UdpSenderPacingTests
        tests/UdpSenderPacingTests.cpp
    )
    target_compile_features(UdpSenderPacingTests PRIVATE cxx_std_20)
    target_link_libraries(UdpSenderPacingTests PRIVATE ScreenShareCore)
    add_test(NAME udp-sender-pacing COMMAND UdpSenderPacingTests)

    add_executable(AvSyncDiagnosticsTests
        tests/AvSyncDiagnosticsTests.cpp
    )
    target_compile_features(AvSyncDiagnosticsTests PRIVATE cxx_std_20)
    target_link_libraries(AvSyncDiagnosticsTests PRIVATE ScreenShareCore)
    add_test(NAME av-sync-diagnostics COMMAND AvSyncDiagnosticsTests)

    find_program(SCREENSHARE_POWERSHELL_EXECUTABLE NAMES pwsh.exe pwsh powershell.exe powershell)
    if(SCREENSHARE_POWERSHELL_EXECUTABLE)
        add_test(NAME update-manifest-script
            COMMAND "${SCREENSHARE_POWERSHELL_EXECUTABLE}" -NoProfile -File
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/UpdateManifestScriptTests.ps1"
                -GeneratorScript "${CMAKE_CURRENT_SOURCE_DIR}/scripts/create-update-manifest.ps1"
        )
    endif()
endif()

if(MSVC)
    target_compile_options(ScreenShareAPI PRIVATE /W4 /permissive- /EHsc)
endif()

add_executable(ScreenShare
    frontend/cli/ScreenShareMain.cpp
    frontend/cli/ScreenShareCLI.cpp
)

target_compile_features(ScreenShare PRIVATE cxx_std_20)
target_include_directories(ScreenShare PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/frontend")
if(TARGET ScreenShareRoomSession)
    add_library(ScreenShareRoomFrontend STATIC frontend/shared/RoomSessionConfig.cpp frontend/shared/RoomProfile.cpp)
    target_include_directories(ScreenShareRoomFrontend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/frontend")
    target_link_libraries(ScreenShareRoomFrontend PUBLIC ScreenShareCore ScreenShareRoomSession)
    add_library(ScreenShareRoomCli STATIC frontend/cli/RoomCli.cpp)
    target_include_directories(ScreenShareRoomCli PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/frontend")
    target_link_libraries(ScreenShareRoomCli PUBLIC ScreenShareRoomFrontend)
    target_link_libraries(ScreenShare PRIVATE ScreenShareRoomCli)
    target_compile_definitions(ScreenShare PRIVATE SCREENSHARE_HAS_ROOM_V2_CLI)
    get_filename_component(_screenshare_cli_qt_bin "${Qt6_DIR}/../../../bin" ABSOLUTE)
    find_program(SCREENSHARE_CLI_WINDEPLOYQT_EXECUTABLE NAMES windeployqt6 windeployqt
        PATHS "${_screenshare_cli_qt_bin}" NO_DEFAULT_PATH REQUIRED)
    add_custom_command(TARGET ScreenShare POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DWINDEPLOYQT_EXECUTABLE=${SCREENSHARE_CLI_WINDEPLOYQT_EXECUTABLE}"
            "-DWINDEPLOYQT_TARGET=$<TARGET_FILE:ScreenShare>"
            "-DWINDEPLOYQT_DEBUG=$<CONFIG:Debug>"
            -DWINDEPLOYQT_HEADLESS=ON
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunWindeployQt.cmake"
        COMMENT "Deploying Qt networking runtime next to ScreenShare.exe" VERBATIM)
    if(BUILD_TESTING)
        add_executable(RoomCliTests tests/RoomCliTests.cpp)
        add_test(NAME room-v2-cli-entry COMMAND "${CMAKE_COMMAND}"
            "-DEXECUTABLE=$<TARGET_FILE:ScreenShare>"
            "-DOUTPUT_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/room-cli-entry"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/RoomCliEntryTests.cmake")
        set_tests_properties(room-v2-cli-entry PROPERTIES TIMEOUT 30)
        add_test(NAME room-v2-cli-deployment COMMAND "${CMAKE_COMMAND}"
            "-DEXECUTABLE=$<TARGET_FILE:ScreenShare>"
            "-DOUTPUT_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/room-cli-deployment"
            "-DWINDEPLOYQT_EXECUTABLE=${SCREENSHARE_CLI_WINDEPLOYQT_EXECUTABLE}"
            "-DWINDEPLOYQT_DEBUG=$<CONFIG:Debug>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/RoomCliDeploymentTests.cmake")
        set_tests_properties(room-v2-cli-deployment PROPERTIES TIMEOUT 60)
        target_link_libraries(RoomCliTests PRIVATE ScreenShareRoomCli)
        add_dependencies(RoomCliTests RoomSocketTests)
        # Explicit generated-window check; routine CTest stays headless and silent.
        add_executable(RoomCliWindowsTests tests/RoomCliTests.cpp)
        target_compile_definitions(RoomCliWindowsTests PRIVATE SCREENSHARE_WINDOWS_CLI_PROOF)
        target_link_libraries(RoomCliWindowsTests PRIVATE ScreenShareRoomCli gdi32)
        add_dependencies(RoomCliWindowsTests RoomSocketTests)
        if(SCREENSHARE_NODE_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/signaling-worker/node_modules/miniflare/package.json")
            add_test(NAME room-v2-cli-media COMMAND "${SCREENSHARE_NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/signaling-worker/tests/run-native-service.mjs"
                "$<TARGET_FILE:RoomCliTests>" "${CMAKE_CURRENT_BINARY_DIR}/room-cli-evidence" media)
            set_tests_properties(room-v2-cli-media PROPERTIES TIMEOUT 90)
        endif()
    endif()
endif()

if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND SCREENSHARE_STATIC_MINGW_RUNTIME)
    target_link_options(ScreenShare PRIVATE -static)
endif()

screenshare_configure_release_lto_warnings(ScreenShare)

target_link_libraries(ScreenShare PRIVATE
    ScreenShareAPI
)

if(TARGET ScreenShareViGEmClient)
    add_dependencies(ScreenShare ScreenShareViGEmClient)
    add_custom_command(TARGET ScreenShare POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:ScreenShareViGEmClient>"
            "$<TARGET_FILE_DIR:ScreenShare>/ViGEmClient.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SCREENSHARE_BUILT_VIGEMCLIENT_LICENSE}"
            "$<TARGET_FILE_DIR:ScreenShare>/ViGEmClient-LICENSE.txt"
        COMMENT "Copying the pinned ViGEm user-mode client"
        VERBATIM
    )
elseif(SCREENSHARE_VIGEMCLIENT_DLL)
    if(NOT EXISTS "${SCREENSHARE_VIGEMCLIENT_DLL}")
        message(FATAL_ERROR "SCREENSHARE_VIGEMCLIENT_DLL does not exist: ${SCREENSHARE_VIGEMCLIENT_DLL}")
    endif()
    add_custom_command(TARGET ScreenShare POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SCREENSHARE_VIGEMCLIENT_DLL}"
            "$<TARGET_FILE_DIR:ScreenShare>/ViGEmClient.dll"
        COMMENT "Copying optional ViGEm user-mode client"
        VERBATIM
    )
    if(SCREENSHARE_VIGEMCLIENT_LICENSE)
        if(NOT EXISTS "${SCREENSHARE_VIGEMCLIENT_LICENSE}")
            message(FATAL_ERROR "SCREENSHARE_VIGEMCLIENT_LICENSE does not exist: ${SCREENSHARE_VIGEMCLIENT_LICENSE}")
        endif()
        add_custom_command(TARGET ScreenShare POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SCREENSHARE_VIGEMCLIENT_LICENSE}"
                "$<TARGET_FILE_DIR:ScreenShare>/ViGEmClient-LICENSE.txt"
            VERBATIM
        )
    endif()
endif()

if(MSVC)
    target_compile_options(ScreenShare PRIVATE /W4 /permissive- /EHsc)
endif()

add_executable(ScreenShareUpdater WIN32
    frontend/updater/ScreenShareUpdater.cpp
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
    bcrypt
)

if(MSVC)
    target_compile_options(ScreenShareUpdater PRIVATE /W4 /permissive- /EHsc)
endif()

if(SCREENSHARE_BUILD_QT_UI)
    if(WIN32 AND NOT MSVC AND EXISTS "C:/msys64/ucrt64/lib/cmake/Qt6")
        list(APPEND CMAKE_PREFIX_PATH "C:/msys64/ucrt64")
    endif()

    if(MSVC)
        find_package(Qt6 REQUIRED COMPONENTS Network WebSockets Svg Widgets)
    else()
        find_package(Qt6 QUIET COMPONENTS Network Svg Widgets)
    endif()
    if(Qt6_FOUND)
        set(SCREENSHARE_UI_ICON_RC)
        if(WIN32)
            set(SCREENSHARE_UI_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/assets/brand/screenshare.ico")
            set(SCREENSHARE_UI_ICON_RC "${CMAKE_CURRENT_BINARY_DIR}/ScreenShareUiIcon.rc")
            configure_file(
                "${CMAKE_CURRENT_SOURCE_DIR}/frontend/ui/ScreenShareUiIcon.rc.in"
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
            frontend/ui/main.cpp
            frontend/ui/QtSessionBackend.cpp
            frontend/ui/ActiveShareWindow.cpp
            frontend/ui/ActiveWatchWindow.cpp
            frontend/ui/AppShellWindow.cpp
            frontend/ui/CreateRoomWindow.cpp
            frontend/ui/HomeWindow.cpp
            frontend/ui/JoinRoomWindow.cpp
            frontend/ui/RoomAccessCheck.cpp
            frontend/ui/ScreenAwakeGuard.cpp
            frontend/ui/Toast.cpp
            frontend/ui/UiStyle.cpp
            frontend/ui/UpdateManager.cpp
            frontend/ui/UpdatePackageSelection.cpp
            frontend/ui/UiReportPath.cpp
            frontend/ui/VideoFrameWidget.cpp
            frontend/ui/resources.qrc
            ${SCREENSHARE_UI_ICON_RC}
        )
        set_property(TARGET ScreenShareUi PROPERTY AUTORCC ON)
        target_compile_features(ScreenShareUi PRIVATE cxx_std_20)
        target_include_directories(ScreenShareUi PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/frontend
        )
        target_link_libraries(ScreenShareUi PRIVATE ScreenShareAPI Qt6::Network Qt6::Svg Qt6::Widgets shell32)
        if(TARGET ScreenShareRoomFrontend)
            add_library(ScreenShareRoomUi STATIC frontend/ui/QtRoomSession.cpp frontend/ui/RoomSessionWindow.cpp frontend/ui/RoomBrowserWindow.cpp)
            target_link_libraries(ScreenShareRoomUi PUBLIC ScreenShareRoomFrontend Qt6::Widgets)
            target_link_libraries(ScreenShareUi PRIVATE ScreenShareRoomUi)
            target_compile_definitions(ScreenShareUi PRIVATE SCREENSHARE_HAS_ROOM_V2_UI)
            if(BUILD_TESTING)
                foreach(ui_proof IN ITEMS RoomUiTests RoomUiWindowsTests)
                    add_executable(${ui_proof} tests/RoomUiTests.cpp frontend/ui/VideoFrameWidget.cpp frontend/ui/UiStyle.cpp)
                    target_link_libraries(${ui_proof} PRIVATE ScreenShareRoomUi gdi32)
                    add_dependencies(${ui_proof} ScreenShareUi)
                endforeach()
                target_compile_definitions(RoomUiWindowsTests PRIVATE SCREENSHARE_WINDOWS_UI_PROOF)
                if(SCREENSHARE_NODE_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/signaling-worker/node_modules/miniflare/package.json")
                    add_test(NAME room-v2-qt-ui COMMAND "${SCREENSHARE_NODE_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/signaling-worker/tests/run-native-service.mjs"
                        "$<TARGET_FILE:RoomUiTests>" "${CMAKE_CURRENT_BINARY_DIR}/room-ui-evidence" media)
                    set_tests_properties(room-v2-qt-ui PROPERTIES TIMEOUT 90 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
                    add_test(NAME room-v2-mutation-ack-recovery COMMAND "${SCREENSHARE_NODE_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/signaling-worker/tests/run-native-service.mjs"
                        "$<TARGET_FILE:RoomUiTests>" "${CMAKE_CURRENT_BINARY_DIR}/room-mutation-evidence" media mutation-ack-delay)
                    set_tests_properties(room-v2-mutation-ack-recovery PROPERTIES TIMEOUT 90 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
                endif()
            endif()
        endif()
        target_compile_definitions(ScreenShareUi PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            SCREENSHARE_APP_VERSION="${PROJECT_VERSION}"
            SCREENSHARE_UPDATE_MANIFEST_URL="${SCREENSHARE_UPDATE_MANIFEST_URL}"
        )
        screenshare_configure_release_lto_warnings(ScreenShareUi)
        add_dependencies(ScreenShareUi ScreenShare)

        if(BUILD_TESTING)
            add_executable(RoomProtocolTests
                tests/RoomProtocolTests.cpp
            )
            if(TARGET ScreenShareRoom)
                target_link_libraries(RoomProtocolTests PRIVATE ScreenShareRoom)
            else()
                target_sources(RoomProtocolTests PRIVATE
                    backend/room/protocol/RoomProtocol.cpp backend/room/protocol/StateSubscription.cpp)
            endif()
            target_compile_features(RoomProtocolTests PRIVATE cxx_std_20)
            target_include_directories(RoomProtocolTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/backend)
            target_compile_definitions(RoomProtocolTests PRIVATE
                ROOM_V2_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/room-v2")
            target_link_libraries(RoomProtocolTests PRIVATE Qt6::Core)
            add_test(NAME room-v2-protocol COMMAND RoomProtocolTests)

            add_executable(UpdatePackageSelectionTests
                tests/UpdatePackageSelectionTests.cpp
                frontend/ui/UpdatePackageSelection.cpp
            )
            target_compile_features(UpdatePackageSelectionTests PRIVATE cxx_std_20)
            target_include_directories(UpdatePackageSelectionTests PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/frontend
            )
            target_link_libraries(UpdatePackageSelectionTests PRIVATE Qt6::Core)
            add_test(NAME update-package-selection COMMAND UpdatePackageSelectionTests)

            add_executable(UiReportPathTests
                tests/UiReportPathTests.cpp
                frontend/ui/UiReportPath.cpp
            )
            target_compile_features(UiReportPathTests PRIVATE cxx_std_20)
            target_include_directories(UiReportPathTests PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/frontend
            )
            target_link_libraries(UiReportPathTests PRIVATE Qt6::Core)
            add_test(NAME ui-report-path COMMAND UiReportPathTests)

            add_executable(VideoFrameInputTests
                tests/VideoFrameInputTests.cpp
                frontend/ui/VideoFrameWidget.cpp
            )
            target_compile_features(VideoFrameInputTests PRIVATE cxx_std_20)
            target_include_directories(VideoFrameInputTests PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/frontend
            )
            target_link_libraries(VideoFrameInputTests PRIVATE ScreenShareCore Qt6::Widgets)
            add_test(NAME video-frame-input COMMAND VideoFrameInputTests)
            get_filename_component(_screenshare_qt_plugins "${Qt6_DIR}/../../../plugins" ABSOLUTE)
            set_tests_properties(video-frame-input PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_screenshare_qt_plugins}"
                TIMEOUT 30
            )
        endif()

        if(MSVC)
            target_compile_options(ScreenShareUi PRIVATE /W4 /permissive- /EHsc)
        endif()

        find_program(SCREENSHARE_WINDEPLOYQT_EXECUTABLE
            NAMES windeployqt6 windeployqt-qt6 windeployqt
            HINTS
                "C:/msys64/ucrt64/bin"
        )
        if(MSVC)
            get_filename_component(_screenshare_native_qt_bin "${Qt6_DIR}/../../../bin" ABSOLUTE)
            find_program(SCREENSHARE_NATIVE_WINDEPLOYQT_EXECUTABLE NAMES windeployqt6 windeployqt
                PATHS "${_screenshare_native_qt_bin}" NO_DEFAULT_PATH REQUIRED)
            set(SCREENSHARE_WINDEPLOYQT_EXECUTABLE "${SCREENSHARE_NATIVE_WINDEPLOYQT_EXECUTABLE}")
        endif()
        if(WIN32 AND SCREENSHARE_WINDEPLOYQT_EXECUTABLE)
            add_custom_command(TARGET ScreenShareUi POST_BUILD
                COMMAND "${CMAKE_COMMAND}"
                    "-DWINDEPLOYQT_EXECUTABLE=${SCREENSHARE_WINDEPLOYQT_EXECUTABLE}"
                    "-DWINDEPLOYQT_TARGET=$<TARGET_FILE:ScreenShareUi>"
                    "-DWINDEPLOYQT_DEBUG=$<CONFIG:Debug>"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunWindeployQt.cmake"
                COMMENT "Deploying Qt runtime next to ScreenShareUi.exe"
                VERBATIM
            )
        endif()
        if(BUILD_TESTING)
            add_test(NAME ui-self-test COMMAND ScreenShareUi --self-test)
            set_tests_properties(ui-self-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
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
if(MSVC)
    # Qt's deployment helper may not recognize a newer installed MSVC toolset.
    # Stage the matching redistributable directly, including vcruntime140_1.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    foreach(runtime_file IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        add_custom_command(TARGET ScreenShare POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${runtime_file}" "$<TARGET_FILE_DIR:ScreenShare>"
            VERBATIM)
    endforeach()
endif()
set(SCREENSHARE_PACKAGE_BASENAME
    "ScreenShare-${SCREENSHARE_PACKAGE_CONFIG}-${SCREENSHARE_PACKAGE_SYSTEM}-${SCREENSHARE_PACKAGE_ARCH}"
)
set(SCREENSHARE_PACKAGE_RUNTIME_SEARCH_DIRS "$<TARGET_FILE_DIR:ScreenShare>")
if(CMAKE_CXX_COMPILER)
    get_filename_component(SCREENSHARE_PACKAGE_COMPILER_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    list(APPEND SCREENSHARE_PACKAGE_RUNTIME_SEARCH_DIRS "${SCREENSHARE_PACKAGE_COMPILER_BIN_DIR}")
endif()
if(WIN32 AND NOT MSVC AND EXISTS "C:/msys64/ucrt64/bin")
    list(APPEND SCREENSHARE_PACKAGE_RUNTIME_SEARCH_DIRS "C:/msys64/ucrt64/bin")
endif()
list(REMOVE_DUPLICATES SCREENSHARE_PACKAGE_RUNTIME_SEARCH_DIRS)
string(REPLACE ";" "|" SCREENSHARE_PACKAGE_RUNTIME_SEARCH_DIRS_ARG "${SCREENSHARE_PACKAGE_RUNTIME_SEARCH_DIRS}")
get_property(SCREENSHARE_WEBRTC_PACKAGE_NOTICES GLOBAL PROPERTY SCREENSHARE_WEBRTC_NOTICES)
if(Qt6_DIR)
    get_filename_component(SCREENSHARE_QT_SBOM_DIR "${Qt6_DIR}/../../../sbom" ABSOLUTE)
endif()
set(SCREENSHARE_PACKAGE_ARGS
    -D "INPUT_DIR=$<TARGET_FILE_DIR:ScreenShare>"
    -D "EXECUTABLE_NAME=$<TARGET_FILE_NAME:ScreenShare>"
    -D "SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    -D "STAGING_DIR=${CMAKE_BINARY_DIR}/package/${SCREENSHARE_PACKAGE_BASENAME}"
    -D "OUTPUT_ZIP=${CMAKE_BINARY_DIR}/${SCREENSHARE_PACKAGE_BASENAME}.zip"
    -D "RUNTIME_SEARCH_DIRS=${SCREENSHARE_PACKAGE_RUNTIME_SEARCH_DIRS_ARG}"
    -D "REQUIRE_NATIVE_NOTICES=${MSVC}"
    -D "WEBRTC_NOTICES_DIR=${SCREENSHARE_WEBRTC_PACKAGE_NOTICES}"
    -D "QT_SBOM_DIR=${SCREENSHARE_QT_SBOM_DIR}"
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

if(SCREENSHARE_BUILD_INSTALLER)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "ScreenShare Setup currently supports only 64-bit Windows builds")
    endif()
    if(NOT SCREENSHARE_CONTROLLER_DRIVER_SETUP OR
       NOT EXISTS "${SCREENSHARE_CONTROLLER_DRIVER_SETUP}")
        message(FATAL_ERROR
            "SCREENSHARE_BUILD_INSTALLER requires SCREENSHARE_CONTROLLER_DRIVER_SETUP. "
            "Run scripts/fetch-controller-runtime.ps1 or provide the signed setup explicitly.")
    endif()
    string(LENGTH "${SCREENSHARE_CONTROLLER_DRIVER_SETUP_SHA256}"
        _screenshare_controller_setup_sha256_length)
    if(NOT _screenshare_controller_setup_sha256_length EQUAL 64 OR
       NOT SCREENSHARE_CONTROLLER_DRIVER_SETUP_SHA256 MATCHES "^[0-9A-Fa-f]+$")
        message(FATAL_ERROR "SCREENSHARE_CONTROLLER_DRIVER_SETUP_SHA256 must contain 64 hexadecimal characters")
    endif()
    file(SHA256 "${SCREENSHARE_CONTROLLER_DRIVER_SETUP}" _screenshare_controller_setup_sha256)
    string(TOUPPER "${_screenshare_controller_setup_sha256}" _screenshare_controller_setup_sha256)
    string(TOUPPER "${SCREENSHARE_CONTROLLER_DRIVER_SETUP_SHA256}" _screenshare_expected_controller_setup_sha256)
    if(NOT _screenshare_controller_setup_sha256 STREQUAL _screenshare_expected_controller_setup_sha256)
        message(FATAL_ERROR
            "Controller driver setup SHA-256 mismatch. Expected "
            "${_screenshare_expected_controller_setup_sha256}, got ${_screenshare_controller_setup_sha256}")
    endif()

    if(NOT SCREENSHARE_INNO_SETUP_COMPILER)
        find_program(SCREENSHARE_INNO_SETUP_COMPILER
            NAMES ISCC.exe ISCC
            PATHS
                "C:/Program Files/Inno Setup 7"
                "C:/Program Files (x86)/Inno Setup 7"
                "C:/Program Files (x86)/Inno Setup 6"
                "C:/Program Files/Inno Setup 6"
        )
    endif()
    if(NOT SCREENSHARE_INNO_SETUP_COMPILER OR
       NOT EXISTS "${SCREENSHARE_INNO_SETUP_COMPILER}")
        message(FATAL_ERROR
            "Inno Setup 6 or 7 is required for SCREENSHARE_BUILD_INSTALLER. "
            "Set SCREENSHARE_INNO_SETUP_COMPILER to ISCC.exe.")
    endif()

    get_filename_component(SCREENSHARE_CONTROLLER_DRIVER_SETUP_NAME
        "${SCREENSHARE_CONTROLLER_DRIVER_SETUP}" NAME)
    file(TO_CMAKE_PATH "${SCREENSHARE_CONTROLLER_DRIVER_SETUP}"
        SCREENSHARE_CONTROLLER_DRIVER_SETUP_INNO)
    file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}/package/${SCREENSHARE_PACKAGE_BASENAME}"
        SCREENSHARE_INSTALLER_APP_STAGE)
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/brand/screenshare.ico"
        SCREENSHARE_INSTALLER_ICON)
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        SCREENSHARE_INSTALLER_LICENSE)
    file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}"
        SCREENSHARE_INSTALLER_OUTPUT_DIR)
    set(SCREENSHARE_INSTALLER_OUTPUT_BASENAME
        "ScreenShare-Setup-${PROJECT_VERSION}-windows-${SCREENSHARE_PACKAGE_ARCH}")
    set(SCREENSHARE_INSTALLER_SCRIPT
        "${CMAKE_BINARY_DIR}/installer/ScreenShare.iss")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/installer")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/installer/ScreenShare.iss.in"
        "${SCREENSHARE_INSTALLER_SCRIPT}"
        @ONLY
    )

    add_custom_target(package-installer ALL
        COMMAND "${SCREENSHARE_INNO_SETUP_COMPILER}" /Qp "${SCREENSHARE_INSTALLER_SCRIPT}"
        DEPENDS package-portable
        COMMENT "Creating ScreenShare Setup with install-time controller provisioning"
        VERBATIM
    )
endif()
