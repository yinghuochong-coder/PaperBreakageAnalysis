add_library(paperbreak_logging STATIC
    src/logging.cpp)

target_link_libraries(paperbreak_logging
    PUBLIC
        paperbreak_common
        paperbreak_compiler_options
    PRIVATE
        spdlog::spdlog)
target_include_directories(paperbreak_logging
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set_target_properties(paperbreak_logging PROPERTIES
    OUTPUT_NAME paperbreak_logging
    FOLDER "PaperBreak/Libraries")

install(TARGETS paperbreak_logging
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

