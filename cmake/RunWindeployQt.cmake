if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

if(NOT DEFINED WINDEPLOYQT_TARGET OR "${WINDEPLOYQT_TARGET}" STREQUAL "")
    message(WARNING "Qt deployment target was not provided; skipping Qt runtime deployment")
    return()
endif()

get_filename_component(target_dir "${WINDEPLOYQT_TARGET}" DIRECTORY)

function(copy_if_exists source_path destination_dir copied_any_var)
    if(EXISTS "${source_path}")
        get_filename_component(source_name "${source_path}" NAME)
        file(COPY_FILE "${source_path}" "${destination_dir}/${source_name}" ONLY_IF_DIFFERENT)
        set(${copied_any_var} TRUE PARENT_SCOPE)
    endif()
endfunction()

function(copy_qt_plugin_dir plugin_root plugin_name destination_dir dependency_files_var)
    set(local_dependencies)
    set(source_dir "${plugin_root}/${plugin_name}")
    if(EXISTS "${source_dir}")
        file(MAKE_DIRECTORY "${destination_dir}/${plugin_name}")
        file(GLOB plugin_files LIST_DIRECTORIES false "${source_dir}/*.dll")
        foreach(plugin_file IN LISTS plugin_files)
            get_filename_component(plugin_file_name "${plugin_file}" NAME)
            set(destination_file "${destination_dir}/${plugin_name}/${plugin_file_name}")
            file(COPY_FILE "${plugin_file}" "${destination_file}" ONLY_IF_DIFFERENT)
            list(APPEND local_dependencies "${destination_file}")
        endforeach()
    endif()
    set(${dependency_files_var} "${${dependency_files_var}};${local_dependencies}" PARENT_SCOPE)
endfunction()

function(repair_qt_runtime_deploy qt_bin_dir)
    if(NOT EXISTS "${qt_bin_dir}")
        message(WARNING "Qt runtime repair could not find Qt bin directory: ${qt_bin_dir}")
        return()
    endif()

    get_filename_component(qt_prefix "${qt_bin_dir}" DIRECTORY)
    set(qt_plugin_root "${qt_prefix}/share/qt6/plugins")
    set(runtime_inputs)
    set(copied_core_runtime FALSE)

    foreach(runtime_name IN ITEMS
        Qt6Core.dll
        Qt6Gui.dll
        Qt6Widgets.dll
    )
        set(runtime_path "${qt_bin_dir}/${runtime_name}")
        copy_if_exists("${runtime_path}" "${target_dir}" copied_core_runtime)
        if(EXISTS "${target_dir}/${runtime_name}")
            list(APPEND runtime_inputs "${target_dir}/${runtime_name}")
        endif()
    endforeach()

    foreach(plugin_name IN ITEMS
        platforms
        styles
        imageformats
    )
        copy_qt_plugin_dir("${qt_plugin_root}" "${plugin_name}" "${target_dir}" runtime_inputs)
    endforeach()

    if(NOT EXISTS "${target_dir}/platforms/qwindows.dll")
        message(WARNING "Qt runtime repair did not find platforms/qwindows.dll; the UI may not open")
    endif()

    if(copied_core_runtime OR runtime_inputs)
        file(GET_RUNTIME_DEPENDENCIES
            RESOLVED_DEPENDENCIES_VAR resolved_runtime_dependencies
            UNRESOLVED_DEPENDENCIES_VAR unresolved_runtime_dependencies
            CONFLICTING_DEPENDENCIES_PREFIX conflicting_runtime_dependencies
            EXECUTABLES "${WINDEPLOYQT_TARGET}"
            LIBRARIES ${runtime_inputs}
            DIRECTORIES "${target_dir}" "${qt_bin_dir}"
            PRE_EXCLUDE_REGEXES
                "api-ms-win-.*"
                "ext-ms-.*"
            POST_EXCLUDE_REGEXES
                "^[A-Za-z]:[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss]ystem32[/\\\\].*"
                "^[A-Za-z]:[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss][Yy][Ss][Ww][Oo][Ww]64[/\\\\].*"
        )
        foreach(resolved_runtime_dependency IN LISTS resolved_runtime_dependencies)
            get_filename_component(dependency_name "${resolved_runtime_dependency}" NAME)
            if(EXISTS "${qt_bin_dir}/${dependency_name}")
                copy_if_exists("${qt_bin_dir}/${dependency_name}" "${target_dir}" copied_dependency)
            else()
                copy_if_exists("${resolved_runtime_dependency}" "${target_dir}" copied_dependency)
            endif()
        endforeach()
        foreach(conflicting_runtime_dependency_name IN LISTS conflicting_runtime_dependencies_FILENAMES)
            if(EXISTS "${qt_bin_dir}/${conflicting_runtime_dependency_name}")
                copy_if_exists("${qt_bin_dir}/${conflicting_runtime_dependency_name}" "${target_dir}" copied_dependency)
            else()
                set(conflicting_runtime_dependency_paths "${conflicting_runtime_dependencies_${conflicting_runtime_dependency_name}}")
                if(conflicting_runtime_dependency_paths)
                    list(GET conflicting_runtime_dependency_paths 0 conflicting_runtime_dependency_path)
                    copy_if_exists("${conflicting_runtime_dependency_path}" "${target_dir}" copied_dependency)
                endif()
            endif()
        endforeach()
        foreach(unresolved_runtime_dependency IN LISTS unresolved_runtime_dependencies)
            if(NOT unresolved_runtime_dependency MATCHES "^(api-ms-win|ext-ms)-")
                message(WARNING "Qt runtime repair could not resolve runtime dependency: ${unresolved_runtime_dependency}")
            endif()
        endforeach()
    endif()

    message(STATUS "Qt runtime dependencies verified next to ${WINDEPLOYQT_TARGET}")
endfunction()

if(DEFINED WINDEPLOYQT_EXECUTABLE AND NOT "${WINDEPLOYQT_EXECUTABLE}" STREQUAL "")
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
        get_filename_component(windeployqt_bin_dir "${WINDEPLOYQT_EXECUTABLE}" DIRECTORY)
        repair_qt_runtime_deploy("${windeployqt_bin_dir}")
        message(STATUS "Qt runtime deployed next to ${WINDEPLOYQT_TARGET}")
        return()
    endif()

    get_filename_component(windeployqt_bin_dir "${WINDEPLOYQT_EXECUTABLE}" DIRECTORY)
    message(STATUS "windeployqt failed (${windeployqt_result}); using fallback Qt runtime deployment")
    repair_qt_runtime_deploy("${windeployqt_bin_dir}")
    return()
endif()

message(STATUS "windeployqt executable was not provided; using fallback Qt runtime deployment")
repair_qt_runtime_deploy("C:/msys64/ucrt64/bin")
