add_library(paperbreak_config STATIC
    src/basic_config.cpp)

target_link_libraries(paperbreak_config
    PUBLIC
        paperbreak_common
        paperbreak_compiler_options
    PRIVATE
        nlohmann_json::nlohmann_json)
target_include_directories(paperbreak_config
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set_target_properties(paperbreak_config PROPERTIES
    OUTPUT_NAME paperbreak_config
    FOLDER "PaperBreak/Libraries")

install(TARGETS paperbreak_config
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
