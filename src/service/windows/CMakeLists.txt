add_library(paperbreak_windows_service STATIC
    src/console_control.cpp
    src/scm.cpp
    src/scm_host.cpp
    src/scm_win32.cpp)

target_link_libraries(paperbreak_windows_service
    PUBLIC
        paperbreak_common
        paperbreak_service_core
        paperbreak_compiler_options)
target_include_directories(paperbreak_windows_service
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set_target_properties(paperbreak_windows_service PROPERTIES
    OUTPUT_NAME paperbreak_windows_service
    FOLDER "PaperBreak/Libraries")

install(TARGETS paperbreak_windows_service
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
