if(NOT DEFINED PAPERBREAK_MVS_ROOT OR PAPERBREAK_MVS_ROOT STREQUAL "")
    if(DEFINED ENV{PAPERBREAK_MVS_ROOT} AND NOT "$ENV{PAPERBREAK_MVS_ROOT}" STREQUAL "")
        set(PAPERBREAK_MVS_ROOT "$ENV{PAPERBREAK_MVS_ROOT}")
    elseif(DEFINED ENV{MVCAM_COMMON_RUNENV} AND NOT "$ENV{MVCAM_COMMON_RUNENV}" STREQUAL "")
        set(PAPERBREAK_MVS_ROOT "$ENV{MVCAM_COMMON_RUNENV}")
    else()
        message(FATAL_ERROR
            "PAPERBREAK_ENABLE_HIKROBOT=ON requires PAPERBREAK_MVS_ROOT. "
            "Install Hikrobot MVS Development SDK 4.8.0.3 and inject its root via "
            "CMakeUserPresets.json or the PAPERBREAK_MVS_ROOT environment variable.")
    endif()
endif()
cmake_path(CONVERT "${PAPERBREAK_MVS_ROOT}" TO_CMAKE_PATH_LIST PAPERBREAK_MVS_ROOT NORMALIZE)
if(NOT IS_DIRECTORY "${PAPERBREAK_MVS_ROOT}")
    message(FATAL_ERROR
        "PAPERBREAK_MVS_ROOT does not name an existing directory: '${PAPERBREAK_MVS_ROOT}'.")
endif()

if(EXISTS "${PAPERBREAK_MVS_ROOT}/Development/Includes/MvCameraControl.h")
    set(paperbreak_mvs_development_dir "${PAPERBREAK_MVS_ROOT}/Development")
else()
    set(paperbreak_mvs_development_dir "${PAPERBREAK_MVS_ROOT}")
endif()

set(paperbreak_mvs_include_dir "${paperbreak_mvs_development_dir}/Includes")
set(paperbreak_mvs_library
    "${paperbreak_mvs_development_dir}/Libraries/win64/MvCameraControl.lib")
foreach(required_file
        "${paperbreak_mvs_include_dir}/MvCameraControl.h"
        "${paperbreak_mvs_include_dir}/MvErrorDefine.h"
        "${paperbreak_mvs_library}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR
            "Hikrobot MVS Development SDK 4.8.0.3 x64 file is missing: '${required_file}'.")
    endif()
endforeach()

if(NOT DEFINED PAPERBREAK_MVS_RUNTIME_DIR OR PAPERBREAK_MVS_RUNTIME_DIR STREQUAL "")
    if(DEFINED ENV{PAPERBREAK_MVS_RUNTIME_DIR}
            AND NOT "$ENV{PAPERBREAK_MVS_RUNTIME_DIR}" STREQUAL "")
        set(PAPERBREAK_MVS_RUNTIME_DIR "$ENV{PAPERBREAK_MVS_RUNTIME_DIR}")
    else()
        message(FATAL_ERROR
            "PAPERBREAK_ENABLE_HIKROBOT=ON requires PAPERBREAK_MVS_RUNTIME_DIR to name "
            "the MVS 4.8.0.3 x64 Runtime directory containing MvCameraControl.dll.")
    endif()
endif()
cmake_path(CONVERT "${PAPERBREAK_MVS_RUNTIME_DIR}" TO_CMAKE_PATH_LIST
    PAPERBREAK_MVS_RUNTIME_DIR NORMALIZE)
set(paperbreak_mvs_runtime "${PAPERBREAK_MVS_RUNTIME_DIR}/MvCameraControl.dll")
if(NOT EXISTS "${paperbreak_mvs_runtime}")
    message(FATAL_ERROR
        "Hikrobot MVS Runtime 4.8.0.3 x64 DLL is missing: '${paperbreak_mvs_runtime}'.")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "PAPERBREAK_MVS_VERSION_FILE=${paperbreak_mvs_runtime}"
        powershell -NoProfile -NonInteractive -Command
        "[Console]::Write((Get-Item -LiteralPath $env:PAPERBREAK_MVS_VERSION_FILE).VersionInfo.FileVersion)"
    RESULT_VARIABLE paperbreak_mvs_version_result
    OUTPUT_VARIABLE paperbreak_mvs_runtime_version
    ERROR_VARIABLE paperbreak_mvs_version_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT paperbreak_mvs_version_result EQUAL 0)
    message(FATAL_ERROR
        "Unable to read Hikrobot MVS Runtime file version: ${paperbreak_mvs_version_error}")
endif()
if(NOT paperbreak_mvs_runtime_version STREQUAL "4.8.0.3")
    message(FATAL_ERROR
        "Hikrobot MVS Runtime version '${paperbreak_mvs_runtime_version}' is not the approved "
        "4.8.0.3 baseline: '${paperbreak_mvs_runtime}'.")
endif()

add_library(Hikrobot::MVS SHARED IMPORTED)
set_target_properties(Hikrobot::MVS PROPERTIES
    IMPORTED_IMPLIB "${paperbreak_mvs_library}"
    IMPORTED_LOCATION "${paperbreak_mvs_runtime}"
    INTERFACE_INCLUDE_DIRECTORIES "${paperbreak_mvs_include_dir}")

add_library(paperbreak_camera_hikrobot STATIC
    src/mvs_lifecycle.cpp)
target_link_libraries(paperbreak_camera_hikrobot
    PRIVATE
        Hikrobot::MVS
        paperbreak_compiler_options
    PUBLIC
        paperbreak_camera
        paperbreak_common)
target_include_directories(paperbreak_camera_hikrobot PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/private")
target_include_directories(paperbreak_camera_hikrobot PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(paperbreak_camera_hikrobot PROPERTIES
    FOLDER "PaperBreak/Libraries")

install(TARGETS paperbreak_camera_hikrobot
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "$<TARGET_FILE:Hikrobot::MVS>"
    DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(BUILD_TESTING)
    add_executable(PaperBreakHikrobotTests
        tests/mvs_lifecycle_tests.cpp)
    target_link_libraries(PaperBreakHikrobotTests
        PRIVATE
            Hikrobot::MVS
            paperbreak_camera_hikrobot
            paperbreak_compiler_options
            GTest::gtest
            GTest::gtest_main)
    target_include_directories(PaperBreakHikrobotTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/private")
    set_target_properties(PaperBreakHikrobotTests PROPERTIES
        FOLDER "PaperBreak/Tests")
    add_custom_command(TARGET PaperBreakHikrobotTests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:Hikrobot::MVS>"
            "$<TARGET_FILE_DIR:PaperBreakHikrobotTests>"
        VERBATIM)
    add_test(NAME hikrobot_adapter_unit COMMAND PaperBreakHikrobotTests)
    set_tests_properties(hikrobot_adapter_unit PROPERTIES LABELS "unit;hikrobot-sdk")
endif()
