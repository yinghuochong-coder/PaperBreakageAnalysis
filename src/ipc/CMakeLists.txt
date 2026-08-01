add_library(paperbreak_ipc STATIC
    src/client.cpp
    src/protocol.cpp
    src/server.cpp)

target_link_libraries(paperbreak_ipc
    PUBLIC
        paperbreak_common
        paperbreak_compiler_options
    PRIVATE
        paperbreak_platform_windows
        Qt6::Core
        Qt6::Network
        nlohmann_json::nlohmann_json)
target_include_directories(paperbreak_ipc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set_target_properties(paperbreak_ipc PROPERTIES
    OUTPUT_NAME paperbreak_ipc
    FOLDER "PaperBreak/Libraries")

install(TARGETS paperbreak_ipc
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
