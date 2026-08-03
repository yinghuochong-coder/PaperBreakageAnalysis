add_executable(PaperBreakTests
    test_main.cpp
    unit/camera_acquisition_tests.cpp
    unit/camera_model_tests.cpp
    unit/camera_state_tests.cpp
    unit/common_tests.cpp
    unit/basic_config_tests.cpp
    unit/client_state_store_tests.cpp
    unit/ipc_client_tests.cpp
    unit/ipc_protocol_tests.cpp
    unit/ipc_server_tests.cpp
    unit/logging_tests.cpp
    unit/monitoring_tests.cpp
    unit/mock_camera_tests.cpp
    unit/pipeline_tests.cpp
    unit/service_runtime_tests.cpp
    unit/system_command_tests.cpp
    unit/windows_control_tests.cpp
    unit/windows_scm_tests.cpp
    simulation/simulation_baseline_tests.cpp
    hardware/hardware_baseline_tests.cpp)
target_link_libraries(PaperBreakTests
    PRIVATE
        paperbreak_common
        paperbreak_camera
        paperbreak_camera_mock
        paperbreak_console_model
        paperbreak_config
        paperbreak_platform_windows
        paperbreak_logging
        paperbreak_monitoring
        paperbreak_pipeline
        paperbreak_ipc
        paperbreak_service_core
        paperbreak_windows_service
        paperbreak_compiler_options
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        nlohmann_json::nlohmann_json
        GTest::gtest)
target_compile_definitions(PaperBreakTests PRIVATE
    PAPERBREAK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(PaperBreakTests PROPERTIES FOLDER "PaperBreak/Tests")

set(paperbreak_test_data_dir "${CMAKE_CURRENT_BINARY_DIR}/data")
file(MAKE_DIRECTORY "${paperbreak_test_data_dir}")
configure_file(data/basic-config-valid.json
    "${paperbreak_test_data_dir}/basic-config-valid.json" COPYONLY)
configure_file(data/basic-config-invalid.json
    "${paperbreak_test_data_dir}/basic-config-invalid.json" COPYONLY)

add_executable(PaperBreakOpenCvSmoke integration/opencv_smoke.cpp)
target_link_libraries(PaperBreakOpenCvSmoke
    PRIVATE
        paperbreak_compiler_options
        opencv_core
        opencv_imgproc
        opencv_imgcodecs)
set_target_properties(PaperBreakOpenCvSmoke PROPERTIES FOLDER "PaperBreak/Tests")

add_test(NAME unit
    COMMAND PaperBreakTests --gtest_filter=Camera*:Pipeline*:Common*:Result*:Version*:Logging*:Monitoring*:HealthMonitor*:BasicConfig*:ConfigRepository*:IpcProtocol*:IpcServer*:IpcClient*:ClientStateStore*:PreviewClient*:ConsoleNavigationModel*:ConsoleTrayStatusModel*:SystemCommand*:ServiceRuntime*:WindowsControl*:WindowsScm*)
set_tests_properties(unit PROPERTIES
    LABELS "unit"
    ENVIRONMENT "PATH=$<TARGET_FILE_DIR:Qt6::Core>\\;$<TARGET_FILE_DIR:opencv_core>\\;$ENV{PATH}")

add_test(NAME simulation
    COMMAND PaperBreakTests --gtest_filter=Simulation*)
set_tests_properties(simulation PROPERTIES LABELS "simulation")

add_test(NAME hardware_baseline
    COMMAND PaperBreakTests --gtest_filter=HardwareBaseline.*)
set_tests_properties(hardware_baseline PROPERTIES LABELS "hardware-integration")

add_test(NAME service_console_smoke
    COMMAND PaperBreakEdgeService
        --console
        --config "${paperbreak_test_data_dir}/basic-config-valid.json"
        --run-for-ms 25)
set_tests_properties(service_console_smoke PROPERTIES
    LABELS "integration"
    TIMEOUT 10)

add_test(NAME service_validate_config
    COMMAND PaperBreakEdgeService
        --validate-config
        --config "${paperbreak_test_data_dir}/basic-config-valid.json")
set_tests_properties(service_validate_config PROPERTIES LABELS "integration")

add_test(NAME service_rejects_invalid_config
    COMMAND "${CMAKE_COMMAND}"
        "-DPROGRAM=$<TARGET_FILE:PaperBreakEdgeService>"
        "-DPROGRAM_ARGUMENTS=--validate-config|--config|${paperbreak_test_data_dir}/basic-config-invalid.json"
        -DEXPECTED_EXIT=2
        -DEXPECTED_TEXT=SYS_CONFIG_INVALID
        -P "${PROJECT_SOURCE_DIR}/cmake/RunAndCheckExit.cmake")
set_tests_properties(service_rejects_invalid_config PROPERTIES LABELS "integration")

add_test(NAME service_requires_config_path
    COMMAND "${CMAKE_COMMAND}"
        "-DPROGRAM=$<TARGET_FILE:PaperBreakEdgeService>"
        "-DPROGRAM_ARGUMENTS=--validate-config"
        -DEXPECTED_EXIT=2
        -DEXPECTED_TEXT=SYS_CONFIG_INVALID
        -P "${PROJECT_SOURCE_DIR}/cmake/RunAndCheckExit.cmake")
set_tests_properties(service_requires_config_path PROPERTIES LABELS "integration")

add_test(NAME service_rejects_conflicting_modes
    COMMAND "${CMAKE_COMMAND}"
        "-DPROGRAM=$<TARGET_FILE:PaperBreakEdgeService>"
        "-DPROGRAM_ARGUMENTS=--console|--validate-config|--config|${paperbreak_test_data_dir}/basic-config-valid.json"
        -DEXPECTED_EXIT=2
        -DEXPECTED_TEXT=SYS_CONFIG_INVALID
        -P "${PROJECT_SOURCE_DIR}/cmake/RunAndCheckExit.cmake")
set_tests_properties(service_rejects_conflicting_modes PROPERTIES LABELS "integration")

add_test(NAME service_install_requires_config
    COMMAND "${CMAKE_COMMAND}"
        "-DPROGRAM=$<TARGET_FILE:PaperBreakEdgeService>"
        "-DPROGRAM_ARGUMENTS=--install"
        -DEXPECTED_EXIT=2
        -DEXPECTED_TEXT=SYS_CONFIG_INVALID
        -P "${PROJECT_SOURCE_DIR}/cmake/RunAndCheckExit.cmake")
set_tests_properties(service_install_requires_config PROPERTIES LABELS "integration")

add_test(NAME service_rejects_install_mode_conflict
    COMMAND "${CMAKE_COMMAND}"
        "-DPROGRAM=$<TARGET_FILE:PaperBreakEdgeService>"
        "-DPROGRAM_ARGUMENTS=--install|--uninstall|--config|${paperbreak_test_data_dir}/basic-config-valid.json"
        -DEXPECTED_EXIT=2
        -DEXPECTED_TEXT=SYS_CONFIG_INVALID
        -P "${PROJECT_SOURCE_DIR}/cmake/RunAndCheckExit.cmake")
set_tests_properties(service_rejects_install_mode_conflict PROPERTIES LABELS "integration")

add_test(NAME service_internal_mode_requires_scm
    COMMAND "${CMAKE_COMMAND}"
        "-DPROGRAM=$<TARGET_FILE:PaperBreakEdgeService>"
        "-DPROGRAM_ARGUMENTS=--service|--config|${paperbreak_test_data_dir}/basic-config-valid.json"
        -DEXPECTED_EXIT=1
        -DEXPECTED_TEXT=SYS_SERVICE_CONTROL_FAILED
        -P "${PROJECT_SOURCE_DIR}/cmake/RunAndCheckExit.cmake")
set_tests_properties(service_internal_mode_requires_scm PROPERTIES LABELS "integration")

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

add_test(NAME missing_hikrobot_gige_runtime_diagnostic
    COMMAND "${CMAKE_COMMAND}"
        "-DTEST_SOURCE=${PROJECT_SOURCE_DIR}/cmake/tests/hikrobot_runtime"
        "-DTEST_BINARY=${PROJECT_BINARY_DIR}/configure-tests/hikrobot-runtime"
        "-DFAILURE_CASE=hikrobot-runtime"
        "-DEXPECTED_TEXT=MVGigEVisionSDK.dll"
        -P "${PROJECT_SOURCE_DIR}/cmake/ExpectConfigureFailure.cmake")
set_tests_properties(missing_hikrobot_gige_runtime_diagnostic PROPERTIES LABELS "integration")

add_test(NAME hikrobot_sdk_boundary
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/src/camera/hikrobot/cmake/CheckSdkBoundary.cmake")
set_tests_properties(hikrobot_sdk_boundary PROPERTIES LABELS "integration")

set(path_scan_inputs "$ENV{PAPERBREAK_QT_ROOT}|${OpenCV_DIR}|$ENV{VCPKG_ROOT}")
if(DEFINED PAPERBREAK_MVS_ROOT AND NOT PAPERBREAK_MVS_ROOT STREQUAL "")
    set(path_scan_mvs_root "${PAPERBREAK_MVS_ROOT}")
else()
    set(path_scan_mvs_root "$ENV{PAPERBREAK_MVS_ROOT}")
endif()
if(DEFINED PAPERBREAK_MVS_RUNTIME_DIR AND NOT PAPERBREAK_MVS_RUNTIME_DIR STREQUAL "")
    set(path_scan_mvs_runtime "${PAPERBREAK_MVS_RUNTIME_DIR}")
else()
    set(path_scan_mvs_runtime "$ENV{PAPERBREAK_MVS_RUNTIME_DIR}")
endif()
string(APPEND path_scan_inputs "|${path_scan_mvs_root}|${path_scan_mvs_runtime}")
set(required_install_files
    "${CMAKE_INSTALL_BINDIR}/MvCameraControl.dll|${CMAKE_INSTALL_BINDIR}/MVGigEVisionSDK.dll")
add_test(NAME install_tree_no_sdk_path_leaks
    COMMAND "${CMAKE_COMMAND}"
        "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
        "-DINSTALL_DIR=${PROJECT_BINARY_DIR}/test-install"
        "-DCONFIG=$<CONFIG>"
        "-DFORBIDDEN_PATHS=${path_scan_inputs}"
        "-DREQUIRED_FILES=${required_install_files}"
        "-DSERVICE_RELATIVE=${CMAKE_INSTALL_BINDIR}/PaperBreakEdgeService.exe"
        "-DCONSOLE_RELATIVE=${CMAKE_INSTALL_BINDIR}/PaperBreakEdgeConsole.exe"
        -P "${PROJECT_SOURCE_DIR}/cmake/InstallAndScan.cmake")
set_tests_properties(install_tree_no_sdk_path_leaks PROPERTIES LABELS "integration")
