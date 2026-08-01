cmake_minimum_required(VERSION 4.2)

project(PaperBreakageAnalysis VERSION 0.1.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PaperBreakPlatform)
paperbreak_validate_platform()

include(CTest)
include(GNUInstallDirs)

option(PAPERBREAK_ENABLE_HIKROBOT "Enable the Hikrobot MVS production adapter" OFF)
option(PAPERBREAK_ENABLE_STATIC_ANALYSIS "Enable MSVC native static analysis" OFF)
option(PAPERBREAK_WARNINGS_AS_ERRORS "Treat project compiler warnings as errors" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(paperbreak_compiler_options INTERFACE)
target_compile_features(paperbreak_compiler_options INTERFACE cxx_std_20)
target_compile_options(paperbreak_compiler_options INTERFACE
    /utf-8
    /W4
    /permissive-
    /EHsc
    /Zc:__cplusplus
    $<$<BOOL:${PAPERBREAK_WARNINGS_AS_ERRORS}>:/WX>
    $<$<BOOL:${PAPERBREAK_ENABLE_STATIC_ANALYSIS}>:/analyze>
    $<$<BOOL:${PAPERBREAK_ENABLE_STATIC_ANALYSIS}>:/analyze:external->)
target_compile_definitions(paperbreak_compiler_options INTERFACE
    NOMINMAX
    WIN32_LEAN_AND_MEAN)

include(PaperBreakDependencies)
paperbreak_resolve_dependencies()

include(PaperBreakVersion)
paperbreak_generate_version(PAPERBREAK_VERSION_SOURCE PAPERBREAK_GENERATED_INCLUDE_DIR)

add_subdirectory(src/common)
add_subdirectory(src/platform)
add_subdirectory(src/logging)
add_subdirectory(src/config)
add_subdirectory(src/camera)
add_subdirectory(src/algorithm)
add_subdirectory(src/event)
add_subdirectory(src/pipeline)
add_subdirectory(src/storage)
add_subdirectory(src/uplink)
add_subdirectory(src/ipc)
add_subdirectory(src/service)
add_subdirectory(src/console)

file(GLOB_RECURSE PAPERBREAK_FORMAT_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp")
list(JOIN PAPERBREAK_FORMAT_FILES "|" PAPERBREAK_FORMAT_FILES_ENCODED)
add_custom_target(format-check
    COMMAND "${CMAKE_COMMAND}"
        "-DFILES=${PAPERBREAK_FORMAT_FILES_ENCODED}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunClangFormat.cmake"
    COMMENT "Checking C++ formatting with clang-format"
    VERBATIM)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(RUNTIME_DEPENDENCY_SET paperbreak_service_runtime
    DIRECTORIES
        "$<TARGET_FILE_DIR:spdlog::spdlog>"
        "$<TARGET_FILE_DIR:Qt6::Core>"
    PRE_EXCLUDE_REGEXES
        "api-ms-.*"
        "ext-ms-.*"
    POST_EXCLUDE_REGEXES
        ".*[\\/]system32[\\/].*\\.dll"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

install(FILES
    "${PAPERBREAK_GENERATED_INCLUDE_DIR}/paperbreak/common/version.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/paperbreak/common")

install(FILES config/default-config.json
    DESTINATION "${CMAKE_INSTALL_DATADIR}/PaperBreakEdge/config")
install(FILES config/schemas/edge-config-v1.schema.json
    DESTINATION "${CMAKE_INSTALL_DATADIR}/PaperBreakEdge/config/schemas")

set(CPACK_PACKAGE_NAME "PaperBreakEdge")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "PaperBreakEdge")
include(CPack)
