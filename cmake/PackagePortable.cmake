if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

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

file(GLOB package_executables LIST_DIRECTORIES false
    "${INPUT_DIR}/ScreenShare*.exe"
)
list(SORT package_executables)
foreach(package_executable IN LISTS package_executables)
    copy_package_file("${package_executable}")
endforeach()
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

foreach(runtime_dir IN ITEMS platforms styles imageformats iconengines networkinformation tls generic)
    if(EXISTS "${INPUT_DIR}/${runtime_dir}")
        file(COPY "${INPUT_DIR}/${runtime_dir}" DESTINATION "${STAGING_DIR}")
    endif()
endforeach()

set(runtime_search_dirs "${INPUT_DIR}")
if(DEFINED RUNTIME_SEARCH_DIRS AND NOT "${RUNTIME_SEARCH_DIRS}" STREQUAL "")
    string(REPLACE "|" ";" runtime_search_dirs_arg "${RUNTIME_SEARCH_DIRS}")
    list(APPEND runtime_search_dirs ${runtime_search_dirs_arg})
endif()
list(REMOVE_DUPLICATES runtime_search_dirs)

file(GLOB input_libraries LIST_DIRECTORIES false
    "${INPUT_DIR}/*.dll"
)
file(GLOB_RECURSE input_plugin_libraries LIST_DIRECTORIES false
    "${INPUT_DIR}/platforms/*.dll"
    "${INPUT_DIR}/styles/*.dll"
    "${INPUT_DIR}/imageformats/*.dll"
    "${INPUT_DIR}/iconengines/*.dll"
    "${INPUT_DIR}/networkinformation/*.dll"
    "${INPUT_DIR}/tls/*.dll"
    "${INPUT_DIR}/generic/*.dll"
)
list(APPEND input_libraries ${input_plugin_libraries})
if(package_executables OR input_libraries)
    set(runtime_dependency_inputs)
    if(package_executables)
        list(APPEND runtime_dependency_inputs EXECUTABLES ${package_executables})
    endif()
    if(input_libraries)
        list(APPEND runtime_dependency_inputs LIBRARIES ${input_libraries})
    endif()
    file(GET_RUNTIME_DEPENDENCIES
        RESOLVED_DEPENDENCIES_VAR resolved_runtime_dependencies
        UNRESOLVED_DEPENDENCIES_VAR unresolved_runtime_dependencies
        CONFLICTING_DEPENDENCIES_PREFIX conflicting_runtime_dependencies
        ${runtime_dependency_inputs}
        DIRECTORIES ${runtime_search_dirs}
        PRE_EXCLUDE_REGEXES
            "api-ms-win-.*"
            "ext-ms-.*"
        POST_EXCLUDE_REGEXES
            "^[A-Za-z]:[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss]ystem32[/\\\\].*"
            "^[A-Za-z]:[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss][Yy][Ss][Ww][Oo][Ww]64[/\\\\].*"
    )
    list(SORT resolved_runtime_dependencies)
    foreach(resolved_runtime_dependency IN LISTS resolved_runtime_dependencies)
        copy_package_file("${resolved_runtime_dependency}")
    endforeach()
    foreach(conflicting_runtime_dependency_name IN LISTS conflicting_runtime_dependencies_FILENAMES)
        set(conflicting_runtime_dependency_paths "${conflicting_runtime_dependencies_${conflicting_runtime_dependency_name}}")
        set(preferred_runtime_dependency)
        foreach(conflicting_runtime_dependency_path IN LISTS conflicting_runtime_dependency_paths)
            get_filename_component(conflicting_runtime_dependency_dir "${conflicting_runtime_dependency_path}" DIRECTORY)
            if(conflicting_runtime_dependency_dir STREQUAL INPUT_DIR)
                set(preferred_runtime_dependency "${conflicting_runtime_dependency_path}")
                break()
            endif()
        endforeach()
        if(NOT preferred_runtime_dependency AND conflicting_runtime_dependency_paths)
            list(GET conflicting_runtime_dependency_paths 0 preferred_runtime_dependency)
        endif()
        if(preferred_runtime_dependency)
            copy_package_file("${preferred_runtime_dependency}")
        endif()
    endforeach()
    foreach(unresolved_runtime_dependency IN LISTS unresolved_runtime_dependencies)
        if(NOT unresolved_runtime_dependency MATCHES "^(api-ms-win|ext-ms)-")
            message(WARNING "Could not resolve runtime dependency: ${unresolved_runtime_dependency}")
        endif()
    endforeach()
endif()

file(WRITE "${STAGING_DIR}/ScreenShare-package.txt"
    "ScreenShare portable package\n"
    "\n"
    "Run ScreenShareUi.exe for the desktop app, or ScreenShare.exe for the command-line tools.\n"
    "The DLLs and plugin folders beside them are runtime dependencies staged by the build.\n"
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
