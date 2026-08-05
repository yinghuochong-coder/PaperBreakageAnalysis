if(NOT DEFINED BUILD_DIR OR NOT DEFINED INSTALL_DIR OR NOT DEFINED CONFIG)
    message(FATAL_ERROR "BUILD_DIR, INSTALL_DIR and CONFIG are required")
endif()

file(REMOVE_RECURSE "${INSTALL_DIR}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${INSTALL_DIR}"
            --config "${CONFIG}" --component UplinkSimulator
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR "UplinkSimulator install failed.\n${install_output}\n${install_error}")
endif()

set(program "${INSTALL_DIR}/bin/PaperBreakUplinkSimulator.exe")
if(NOT EXISTS "${program}")
    message(FATAL_ERROR "Standalone simulator executable is missing: ${program}")
endif()
execute_process(
    COMMAND "${program}" --headless --listen 127.0.0.1 --port 0
            --workspace "${INSTALL_DIR}/smoke-workspace" --run-for-ms 50
    WORKING_DIRECTORY "${INSTALL_DIR}/bin"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
    TIMEOUT 10)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "Installed simulator failed to run (${run_result}).\n${run_output}\n${run_error}")
endif()
