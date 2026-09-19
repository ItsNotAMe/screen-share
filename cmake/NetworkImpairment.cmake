include_guard(GLOBAL)
set(SCREENSHARE_WEBRTC_TEST_SOURCE_DIR "" CACHE PATH "Optional pinned WebRTC checkout for network impairment proofs")

function(screenshare_add_network_model target)
    set(webrtcSource "${SCREENSHARE_WEBRTC_TEST_SOURCE_DIR}")
    find_package(Git REQUIRED)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${webrtcSource}" rev-parse HEAD
        OUTPUT_VARIABLE testRevision OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/dependencies/webrtc-source.json" testLock)
    string(JSON expectedTestRevision GET "${testLock}" commit)
    if(NOT testRevision STREQUAL expectedTestRevision)
        message(FATAL_ERROR "Network impairment source must match the pinned WebRTC revision")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${webrtcSource}" diff --exit-code HEAD --
        test/network/simulated_network.cc test/network/simulated_network.h api/test/simulated_network.h
        api/test/network_emulation/leaky_bucket_network_queue.cc api/test/network_emulation/leaky_bucket_network_queue.h
        api/test/network_emulation/network_queue.h COMMAND_ERROR_IS_FATAL ANY)
    target_sources(${target} PRIVATE "${webrtcSource}/test/network/simulated_network.cc"
        "${webrtcSource}/api/test/network_emulation/leaky_bucket_network_queue.cc")
    target_include_directories(${target} SYSTEM PRIVATE "${webrtcSource}")
endfunction()
