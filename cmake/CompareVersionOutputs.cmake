if(NOT DEFINED SERVICE OR NOT DEFINED CONSOLE)
    message(FATAL_ERROR "SERVICE and CONSOLE executable paths are required")
endif()

execute_process(
    COMMAND "${SERVICE}" --version
    RESULT_VARIABLE service_result
    OUTPUT_VARIABLE service_output
    ERROR_VARIABLE service_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
    COMMAND "${CONSOLE}" --version
    RESULT_VARIABLE console_result
    OUTPUT_VARIABLE console_output
    ERROR_VARIABLE console_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT service_result EQUAL 0)
    message(FATAL_ERROR "Service --version failed: ${service_error}")
endif()
if(NOT console_result EQUAL 0)
    message(FATAL_ERROR "Console --version failed: ${console_error}")
endif()
if(NOT service_output STREQUAL console_output)
    message(FATAL_ERROR
        "Version output differs.\nService:\n${service_output}\nConsole:\n${console_output}")
endif()

