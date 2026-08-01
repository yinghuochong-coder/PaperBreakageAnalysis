add_library(paperbreak_monitoring STATIC
    src/monitoring.cpp)

target_link_libraries(paperbreak_monitoring
    PUBLIC
        paperbreak_common
        paperbreak_logging
        paperbreak_compiler_options)
target_include_directories(paperbreak_monitoring
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set_target_properties(paperbreak_monitoring PROPERTIES
    OUTPUT_NAME paperbreak_monitoring
    FOLDER "PaperBreak/Libraries")

install(TARGETS paperbreak_monitoring
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
