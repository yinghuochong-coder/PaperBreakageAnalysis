if(NOT DEFINED BUILD_DIR OR NOT DEFINED INSTALL_DIR)
    message(FATAL_ERROR "BUILD_DIR and INSTALL_DIR are required")
endif()

file(REMOVE_RECURSE "${INSTALL_DIR}")
set(install_command "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${INSTALL_DIR}")
if(DEFINED CONFIG AND NOT CONFIG STREQUAL "")
    list(APPEND install_command --config "${CONFIG}")
endif()
execute_process(
    COMMAND ${install_command}
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "Install failed.\n${install_output}\n${install_error}")
endif()

string(REPLACE "|" ";" forbidden_paths "${FORBIDDEN_PATHS}")
file(GLOB_RECURSE installed_files LIST_DIRECTORIES FALSE "${INSTALL_DIR}/*")
foreach(installed_file IN LISTS installed_files)
    # MSVC Debug static/import libraries contain compiler include paths just like PDB/ILK files.
    # They are development artifacts; deployment path-leak validation applies to runtime and data
    # files that are loaded on the target machine.
    if(installed_file MATCHES "\\.(pdb|ilk|lib)$")
        continue()
    endif()
    file(STRINGS "${installed_file}" file_strings LIMIT_COUNT 100000)
    string(JOIN "\n" file_content ${file_strings})
    foreach(forbidden_path IN LISTS forbidden_paths)
        if(forbidden_path STREQUAL "")
            continue()
        endif()
        file(TO_CMAKE_PATH "${forbidden_path}" normalized_path)
        string(FIND "${file_content}" "${forbidden_path}" native_position)
        string(FIND "${file_content}" "${normalized_path}" normalized_position)
        if(NOT native_position EQUAL -1 OR NOT normalized_position EQUAL -1)
            message(FATAL_ERROR
                "Installed artifact '${installed_file}' leaks dependency path '${forbidden_path}'.")
        endif()
    endforeach()
endforeach()

if(DEFINED SERVICE_RELATIVE AND DEFINED CONSOLE_RELATIVE)
    set(service_path "${INSTALL_DIR}/${SERVICE_RELATIVE}")
    set(console_path "${INSTALL_DIR}/${CONSOLE_RELATIVE}")
    get_filename_component(runtime_directory "${service_path}" DIRECTORY)
    execute_process(
        COMMAND "${service_path}" --version
        WORKING_DIRECTORY "${runtime_directory}"
        RESULT_VARIABLE service_result
        OUTPUT_VARIABLE service_output
        ERROR_VARIABLE service_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(
        COMMAND "${console_path}" --version
        WORKING_DIRECTORY "${runtime_directory}"
        RESULT_VARIABLE console_result
        OUTPUT_VARIABLE console_output
        ERROR_VARIABLE console_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT service_result EQUAL 0)
        message(FATAL_ERROR "Installed service failed to launch: ${service_error}")
    endif()
    if(NOT console_result EQUAL 0)
        message(FATAL_ERROR "Installed console failed to launch: ${console_error}")
    endif()
    if(NOT service_output STREQUAL console_output)
        message(FATAL_ERROR "Installed service and console version output differs.")
    endif()
endif()
