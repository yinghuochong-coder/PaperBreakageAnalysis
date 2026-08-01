add_library(paperbreak_common STATIC
    src/error.cpp
    "${PAPERBREAK_VERSION_SOURCE}")

target_link_libraries(paperbreak_common PUBLIC paperbreak_compiler_options)
target_include_directories(paperbreak_common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PAPERBREAK_GENERATED_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set_target_properties(paperbreak_common PROPERTIES
    OUTPUT_NAME paperbreak_common
    FOLDER "PaperBreak/Libraries")

install(TARGETS paperbreak_common
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

