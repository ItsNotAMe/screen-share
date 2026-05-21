if(NOT DEFINED WINDEPLOYQT_EXECUTABLE OR "${WINDEPLOYQT_EXECUTABLE}" STREQUAL "")
    message(WARNING "windeployqt executable was not provided; skipping Qt runtime deployment")
    return()
endif()

if(NOT DEFINED WINDEPLOYQT_TARGET OR "${WINDEPLOYQT_TARGET}" STREQUAL "")
    message(WARNING "windeployqt target was not provided; skipping Qt runtime deployment")
    return()
endif()

execute_process(
    COMMAND "${WINDEPLOYQT_EXECUTABLE}"
        --no-translations
        --no-system-d3d-compiler
        --no-system-dxc-compiler
        --no-opengl-sw
        --compiler-runtime
        "${WINDEPLOYQT_TARGET}"
    RESULT_VARIABLE windeployqt_result
    OUTPUT_VARIABLE windeployqt_output
    ERROR_VARIABLE windeployqt_error
)

if(windeployqt_result EQUAL 0)
    message(STATUS "Qt runtime deployed next to ${WINDEPLOYQT_TARGET}")
    return()
endif()

string(STRIP "${windeployqt_output}" windeployqt_output)
string(STRIP "${windeployqt_error}" windeployqt_error)

message(WARNING
    "windeployqt failed (${windeployqt_result}); ${WINDEPLOYQT_TARGET} was still built.\n"
    "The UI can run from a configured development shell, but a portable copy may need Qt runtime DLLs "
    "from the package target or a repaired Qt installation.\n"
    "${windeployqt_output}\n"
    "${windeployqt_error}"
)
