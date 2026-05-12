foreach(required_var IN ITEMS INPUT_DIR EXECUTABLE_NAME SOURCE_DIR STAGING_DIR OUTPUT_ZIP)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} is required")
    endif()
endforeach()

set(executable_path "${INPUT_DIR}/${EXECUTABLE_NAME}")
if(NOT EXISTS "${executable_path}")
    message(FATAL_ERROR "Executable not found: ${executable_path}")
endif()

get_filename_component(staging_parent "${STAGING_DIR}" DIRECTORY)
get_filename_component(package_dir_name "${STAGING_DIR}" NAME)

file(REMOVE_RECURSE "${STAGING_DIR}")
file(MAKE_DIRECTORY "${STAGING_DIR}")

function(copy_package_file file_path)
    if(EXISTS "${file_path}")
        file(COPY "${file_path}" DESTINATION "${STAGING_DIR}")
    endif()
endfunction()

copy_package_file("${executable_path}")
copy_package_file("${SOURCE_DIR}/README.md")
copy_package_file("${SOURCE_DIR}/LICENSE")

file(GLOB runtime_files LIST_DIRECTORIES false
    "${INPUT_DIR}/*.dll"
    "${INPUT_DIR}/ScreenShare-runtime-dependencies.txt"
)
list(SORT runtime_files)
foreach(runtime_file IN LISTS runtime_files)
    copy_package_file("${runtime_file}")
endforeach()

file(WRITE "${STAGING_DIR}/ScreenShare-package.txt"
    "ScreenShare portable package\n"
    "\n"
    "Run ScreenShare.exe from this folder. The DLLs beside it are runtime dependencies staged by the build.\n"
    "If Windows reports a loader error, delete the old copied folder on the target machine and unpack this zip again.\n"
)

file(REMOVE "${OUTPUT_ZIP}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${OUTPUT_ZIP}" --format=zip -- "${package_dir_name}"
    WORKING_DIRECTORY "${staging_parent}"
    RESULT_VARIABLE package_result
)
if(NOT package_result EQUAL 0)
    message(FATAL_ERROR "Failed to create package: ${OUTPUT_ZIP}")
endif()

message(STATUS "Created ${OUTPUT_ZIP}")
