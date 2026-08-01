if(NOT DEFINED PROGRAM OR NOT DEFINED EXPECTED_EXIT)
    message(FATAL_ERROR "PROGRAM and EXPECTED_EXIT are required")
endif()

set(arguments "")
if(DEFINED PROGRAM_ARGUMENTS)
    string(REPLACE "|" ";" arguments "${PROGRAM_ARGUMENTS}")
endif()

execute_process(
    COMMAND "${PROGRAM}" ${arguments}
    RESULT_VARIABLE actual_exit
    OUTPUT_VARIABLE standard_output
    ERROR_VARIABLE standard_error)

if(NOT "${actual_exit}" STREQUAL "${EXPECTED_EXIT}")
    message(FATAL_ERROR
        "Expected exit ${EXPECTED_EXIT}, got ${actual_exit}\nstdout: ${standard_output}\nstderr: ${standard_error}")
endif()

if(DEFINED EXPECTED_TEXT)
    string(CONCAT combined_output "${standard_output}" "${standard_error}")
    string(FIND "${combined_output}" "${EXPECTED_TEXT}" match_position)
    if(match_position EQUAL -1)
        message(FATAL_ERROR
            "Expected text '${EXPECTED_TEXT}' was not found\nstdout: ${standard_output}\nstderr: ${standard_error}")
    endif()
endif()
