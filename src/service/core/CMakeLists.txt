add_library(paperbreak_service_core STATIC
    src/event_runtime.cpp
    src/runtime.cpp
    src/system_commands.cpp)

target_link_libraries(paperbreak_service_core
    PUBLIC
        paperbreak_common
        paperbreak_config
        paperbreak_camera
        paperbreak_algorithm_classical
        paperbreak_algorithm_mock
        paperbreak_event_codec
        paperbreak_ipc
        paperbreak_logging
        paperbreak_monitoring
        paperbreak_pipeline
        paperbreak_storage
        paperbreak_compiler_options)
target_link_libraries(paperbreak_service_core PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(paperbreak_service_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set_target_properties(paperbreak_service_core PROPERTIES
    OUTPUT_NAME paperbreak_service_core
    FOLDER "PaperBreak/Libraries")

install(TARGETS paperbreak_service_core
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
