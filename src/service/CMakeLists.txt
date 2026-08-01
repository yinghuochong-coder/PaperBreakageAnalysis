add_executable(PaperBreakEdgeService main.cpp)
target_link_libraries(PaperBreakEdgeService
    PRIVATE
        paperbreak_common
        paperbreak_logging
        paperbreak_camera
        paperbreak_pipeline
        paperbreak_algorithm
        paperbreak_event
        paperbreak_storage
        paperbreak_uplink
        paperbreak_compiler_options)
set_target_properties(PaperBreakEdgeService PROPERTIES
    OUTPUT_NAME PaperBreakEdgeService
    FOLDER "PaperBreak/Applications")
install(TARGETS PaperBreakEdgeService
    RUNTIME_DEPENDENCY_SET paperbreak_service_runtime
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
