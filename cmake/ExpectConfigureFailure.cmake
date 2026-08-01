if(NOT DEFINED TEST_SOURCE OR NOT DEFINED TEST_BINARY OR NOT DEFINED EXPECTED_TEXT)
    message(FATAL_ERROR "TEST_SOURCE, TEST_BINARY and EXPECTED_TEXT are required")
endif()

file(REMOVE_RECURSE "${TEST_BINARY}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${TEST_SOURCE}" -B "${TEST_BINARY}"
        "-DFAILURE_CASE=${FAILURE_CASE}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error)
set(all_output "${configure_output}\n${configure_error}")
if(configure_result EQUAL 0)
    message(FATAL_ERROR "Configuration unexpectedly succeeded.\n${all_output}")
endif()
string(FIND "${all_output}" "${EXPECTED_TEXT}" expected_position)
if(expected_position EQUAL -1)
    message(FATAL_ERROR
        "Configuration failed without the expected diagnostic '${EXPECTED_TEXT}'.\n${all_output}")
endif()

