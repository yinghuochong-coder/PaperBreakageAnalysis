add_executable(PaperBreakTests
    unit/common_tests.cpp
    unit/logging_tests.cpp
    simulation/simulation_baseline_tests.cpp
    hardware/hardware_baseline_tests.cpp)
target_link_libraries(PaperBreakTests
    PRIVATE
        paperbreak_common
        paperbreak_logging
        paperbreak_compiler_options
        GTest::gtest_main)
set_target_properties(PaperBreakTests PROPERTIES FOLDER "PaperBreak/Tests")

add_executable(PaperBreakOpenCvSmoke integration/opencv_smoke.cpp)
target_link_libraries(PaperBreakOpenCvSmoke
    PRIVATE
        paperbreak_compiler_options
        opencv_core
        opencv_imgproc
        opencv_imgcodecs)
set_target_properties(PaperBreakOpenCvSmoke PROPERTIES FOLDER "PaperBreak/Tests")

add_test(NAME unit
    COMMAND PaperBreakTests --gtest_filter=Common*:Result*:Version*:Logging*)
set_tests_properties(unit PROPERTIES LABELS "unit")

add_test(NAME simulation
    COMMAND PaperBreakTests --gtest_filter=SimulationBaseline.*)
set_tests_properties(simulation PROPERTIES LABELS "simulation")

add_test(NAME hardware_baseline
    COMMAND PaperBreakTests --gtest_filter=HardwareBaseline.*)
set_tests_properties(hardware_baseline PROPERTIES LABELS "hardware-integration")

add_test(NAME service_console_smoke
    COMMAND PaperBreakEdgeService --console --run-for-ms 25)
set_tests_properties(service_console_smoke PROPERTIES
    LABELS "integration"
    TIMEOUT 10)

add_test(NAME qt_console_smoke
    COMMAND "${CMAKE_COMMAND}" -E env
        "QT_QPA_PLATFORM=offscreen"
        "PATH=$<TARGET_FILE_DIR:Qt6::Core>;$ENV{PATH}"
        "$<TARGET_FILE:PaperBreakEdgeConsole>" --smoke-test)
set_tests_properties(qt_console_smoke PROPERTIES
    LABELS "integration"
    TIMEOUT 10)

add_test(NAME version_output_matches
    COMMAND "${CMAKE_COMMAND}"
        "-DSERVICE=$<TARGET_FILE:PaperBreakEdgeService>"
        "-DCONSOLE=$<TARGET_FILE:PaperBreakEdgeConsole>"
        -P "${PROJECT_SOURCE_DIR}/cmake/CompareVersionOutputs.cmake")
set_tests_properties(version_output_matches PROPERTIES LABELS "integration")

add_test(NAME opencv_link_smoke
    COMMAND "${CMAKE_COMMAND}" -E env
        "PATH=$<TARGET_FILE_DIR:opencv_core>;$ENV{PATH}"
        "$<TARGET_FILE:PaperBreakOpenCvSmoke>")
set_tests_properties(opencv_link_smoke PROPERTIES LABELS "integration")

add_test(NAME reject_x86_configuration
    COMMAND "${CMAKE_COMMAND}"
        "-DTEST_SOURCE=${PROJECT_SOURCE_DIR}/cmake/tests/platform_validation"
        "-DTEST_BINARY=${PROJECT_BINARY_DIR}/configure-tests/x86"
        "-DFAILURE_CASE=architecture"
        "-DEXPECTED_TEXT=supports only x64 builds"
        -P "${PROJECT_SOURCE_DIR}/cmake/ExpectConfigureFailure.cmake")
set_tests_properties(reject_x86_configuration PROPERTIES LABELS "integration")

add_test(NAME reject_non_msvc_configuration
    COMMAND "${CMAKE_COMMAND}"
        "-DTEST_SOURCE=${PROJECT_SOURCE_DIR}/cmake/tests/platform_validation"
        "-DTEST_BINARY=${PROJECT_BINARY_DIR}/configure-tests/compiler"
        "-DFAILURE_CASE=compiler"
        "-DEXPECTED_TEXT=supports only the MSVC compiler"
        -P "${PROJECT_SOURCE_DIR}/cmake/ExpectConfigureFailure.cmake")
set_tests_properties(reject_non_msvc_configuration PROPERTIES LABELS "integration")

add_test(NAME missing_dependency_diagnostic
    COMMAND "${CMAKE_COMMAND}"
        "-DTEST_SOURCE=${PROJECT_SOURCE_DIR}/cmake/tests/missing_dependency"
        "-DTEST_BINARY=${PROJECT_BINARY_DIR}/configure-tests/missing-dependency"
        "-DFAILURE_CASE=dependency"
        "-DEXPECTED_TEXT=PaperBreakDeliberatelyMissing"
        -P "${PROJECT_SOURCE_DIR}/cmake/ExpectConfigureFailure.cmake")
set_tests_properties(missing_dependency_diagnostic PROPERTIES LABELS "integration")

set(path_scan_inputs "$ENV{PAPERBREAK_QT_ROOT}|${OpenCV_DIR}|$ENV{VCPKG_ROOT}")
add_test(NAME install_tree_no_sdk_path_leaks
    COMMAND "${CMAKE_COMMAND}"
        "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
        "-DINSTALL_DIR=${PROJECT_BINARY_DIR}/test-install"
        "-DCONFIG=$<CONFIG>"
        "-DFORBIDDEN_PATHS=${path_scan_inputs}"
        "-DSERVICE_RELATIVE=${CMAKE_INSTALL_BINDIR}/PaperBreakEdgeService.exe"
        "-DCONSOLE_RELATIVE=${CMAKE_INSTALL_BINDIR}/PaperBreakEdgeConsole.exe"
        -P "${PROJECT_SOURCE_DIR}/cmake/InstallAndScan.cmake")
set_tests_properties(install_tree_no_sdk_path_leaks PROPERTIES LABELS "integration")
