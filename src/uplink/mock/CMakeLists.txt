add_library(paperbreak_uplink_mock STATIC
    src/mock_transport.cpp)
target_link_libraries(paperbreak_uplink_mock
    PUBLIC
        paperbreak_uplink
        paperbreak_compiler_options)
target_include_directories(paperbreak_uplink_mock PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
set_target_properties(paperbreak_uplink_mock PROPERTIES FOLDER "PaperBreak/Libraries")
