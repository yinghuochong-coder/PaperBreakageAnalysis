add_executable(PaperBreakCameraHardwareTest
    hikrobot_hardware_test.cpp)
target_link_libraries(PaperBreakCameraHardwareTest
    PRIVATE
        paperbreak_camera_hikrobot
        paperbreak_camera
        paperbreak_common
        paperbreak_compiler_options
        nlohmann_json::nlohmann_json
        iphlpapi
        psapi)
target_compile_definitions(PaperBreakCameraHardwareTest PRIVATE _WIN32_WINNT=0x0A00)
set_target_properties(PaperBreakCameraHardwareTest PROPERTIES
    FOLDER "PaperBreak/Tools")
paperbreak_stage_hikrobot_runtime(PaperBreakCameraHardwareTest)
install(TARGETS PaperBreakCameraHardwareTest
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

add_test(NAME hikrobot_hardware_tool_help
    COMMAND PaperBreakCameraHardwareTest --help)
set_tests_properties(hikrobot_hardware_tool_help PROPERTIES LABELS "unit;hikrobot-sdk")

add_test(NAME hikrobot_hardware_tool_rejects_missing_plan
    COMMAND "${CMAKE_COMMAND}"
        "-DPROGRAM=$<TARGET_FILE:PaperBreakCameraHardwareTest>"
        "-DPROGRAM_ARGUMENTS=--run"
        -DEXPECTED_EXIT=2
        -DEXPECTED_TEXT=HW_PLAN_INVALID
        -P "${PROJECT_SOURCE_DIR}/cmake/RunAndCheckExit.cmake")
set_tests_properties(hikrobot_hardware_tool_rejects_missing_plan PROPERTIES
    LABELS "unit;hikrobot-sdk")

add_test(NAME hikrobot_hardware_tool_rejects_unbounded_plan
    COMMAND "${CMAKE_COMMAND}"
        "-DPROGRAM=$<TARGET_FILE:PaperBreakCameraHardwareTest>"
        "-DPROGRAM_ARGUMENTS=--run|--plan|${PROJECT_SOURCE_DIR}/tests/data/hardware-plan-unbounded.json|--output|${PROJECT_BINARY_DIR}/must-not-exist.json"
        -DEXPECTED_EXIT=2
        -DEXPECTED_TEXT=HW_PLAN_INVALID
        -P "${PROJECT_SOURCE_DIR}/cmake/RunAndCheckExit.cmake")
set_tests_properties(hikrobot_hardware_tool_rejects_unbounded_plan PROPERTIES
    LABELS "unit;hikrobot-sdk")

add_test(NAME hikrobot_hardware_tool_accepts_line_io_plan
    COMMAND PaperBreakCameraHardwareTest
        --validate-plan
        --plan "${PROJECT_SOURCE_DIR}/tests/data/hardware-plan-line-io.json")
set_tests_properties(hikrobot_hardware_tool_accepts_line_io_plan PROPERTIES
    LABELS "unit;hikrobot-sdk")

add_test(NAME hikrobot_hardware_tool_accepts_six_camera_plan
    COMMAND PaperBreakCameraHardwareTest
        --validate-plan
        --plan "${PROJECT_SOURCE_DIR}/tests/data/hardware-plan-six-cameras.json")
set_tests_properties(hikrobot_hardware_tool_accepts_six_camera_plan PROPERTIES
    LABELS "unit;hikrobot-sdk")

add_test(NAME hikrobot_hardware_tool_rejects_seventh_camera
    COMMAND "${CMAKE_COMMAND}"
        "-DPROGRAM=$<TARGET_FILE:PaperBreakCameraHardwareTest>"
        "-DPROGRAM_ARGUMENTS=--validate-plan|--plan|${PROJECT_SOURCE_DIR}/tests/data/hardware-plan-seven-cameras.json"
        -DEXPECTED_EXIT=2
        -DEXPECTED_TEXT=HW_PLAN_INVALID
        -P "${PROJECT_SOURCE_DIR}/cmake/RunAndCheckExit.cmake")
set_tests_properties(hikrobot_hardware_tool_rejects_seventh_camera PROPERTIES
    LABELS "unit;hikrobot-sdk")
