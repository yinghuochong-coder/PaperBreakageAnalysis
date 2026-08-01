function(paperbreak_generate_version output_source output_include_dir)
    string(TIMESTAMP PAPERBREAK_BUILD_TIME_UTC "%Y-%m-%dT%H:%M:%S.000Z" UTC)
    set(PAPERBREAK_GIT_COMMIT "unknown")
    set(PAPERBREAK_GIT_DIRTY "false")

    find_package(Git QUIET)
    if(Git_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            RESULT_VARIABLE git_result
            OUTPUT_VARIABLE git_output
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(git_result EQUAL 0 AND NOT git_output STREQUAL "")
            set(PAPERBREAK_GIT_COMMIT "${git_output}")
        endif()

        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            RESULT_VARIABLE git_status_result
            OUTPUT_VARIABLE git_status_output
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(git_status_result EQUAL 0 AND NOT git_status_output STREQUAL "")
            set(PAPERBREAK_GIT_DIRTY "true")
        endif()
    endif()

    set(version_include_dir "${PROJECT_BINARY_DIR}/generated/include")
    set(version_source_dir "${PROJECT_BINARY_DIR}/generated/src")
    file(MAKE_DIRECTORY "${version_include_dir}/paperbreak/common" "${version_source_dir}")

    configure_file(
        "${PROJECT_SOURCE_DIR}/cmake/templates/version.hpp.in"
        "${version_include_dir}/paperbreak/common/version.hpp"
        @ONLY)
    configure_file(
        "${PROJECT_SOURCE_DIR}/cmake/templates/version.cpp.in"
        "${version_source_dir}/version.cpp"
        @ONLY)

    set(${output_source} "${version_source_dir}/version.cpp" PARENT_SCOPE)
    set(${output_include_dir} "${version_include_dir}" PARENT_SCOPE)
endfunction()

