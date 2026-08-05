add_library(paperbreak_uplink_transport STATIC
    src/qt_transport.cpp)
target_link_libraries(paperbreak_uplink_transport
    PUBLIC paperbreak_uplink paperbreak_compiler_options
    PRIVATE Qt6::Core Qt6::Network Qt6::WebSockets nlohmann_json::nlohmann_json)
target_include_directories(paperbreak_uplink_transport PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(paperbreak_uplink_transport PROPERTIES FOLDER "PaperBreak/Libraries")
install(TARGETS paperbreak_uplink_transport ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
