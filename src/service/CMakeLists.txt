add_subdirectory(core)
add_subdirectory(windows)

add_executable(PaperBreakEdgeService main.cpp)
target_link_libraries(PaperBreakEdgeService
    PRIVATE
        paperbreak_common
        paperbreak_config
        paperbreak_logging
        paperbreak_monitoring
        paperbreak_ipc
        paperbreak_platform_windows
        paperbreak_service_core
        paperbreak_windows_service
        paperbreak_compiler_options
        Qt6::Core
        nlohmann_json::nlohmann_json)
set_target_properties(PaperBreakEdgeService PROPERTIES
    OUTPUT_NAME PaperBreakEdgeService
    FOLDER "PaperBreak/Applications")
install(TARGETS PaperBreakEdgeService
    RUNTIME_DEPENDENCY_SET paperbreak_service_runtime
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
