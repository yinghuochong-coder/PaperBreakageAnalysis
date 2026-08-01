add_library(paperbreak_platform INTERFACE)
target_link_libraries(paperbreak_platform
    INTERFACE
        paperbreak_common
        paperbreak_compiler_options)
target_include_directories(paperbreak_platform
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

add_library(paperbreak_platform_windows STATIC
    windows/atomic_file_win.cpp
    windows/local_ipc_security_win.cpp
    windows/system_metrics_win.cpp)
target_link_libraries(paperbreak_platform_windows
    PUBLIC
        paperbreak_platform
        paperbreak_common
        paperbreak_monitoring
        paperbreak_compiler_options)
target_link_libraries(paperbreak_platform_windows PRIVATE advapi32 psapi)
target_include_directories(paperbreak_platform_windows
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(paperbreak_platform_windows PROPERTIES
    OUTPUT_NAME paperbreak_platform_windows
    FOLDER "PaperBreak/Libraries")

install(TARGETS paperbreak_platform paperbreak_platform_windows
    EXPORT PaperBreakPlatformTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
