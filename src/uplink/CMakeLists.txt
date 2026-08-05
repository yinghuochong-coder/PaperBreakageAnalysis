add_library(paperbreak_uplink STATIC
    src/module.cpp
    src/protocol.cpp
    src/runtime.cpp
    src/upload_scheduler.cpp)
target_link_libraries(paperbreak_uplink
    PUBLIC paperbreak_common paperbreak_storage paperbreak_compiler_options
    PRIVATE paperbreak_event nlohmann_json::nlohmann_json)
target_include_directories(paperbreak_uplink PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(paperbreak_uplink PROPERTIES FOLDER "PaperBreak/Libraries")
install(TARGETS paperbreak_uplink ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

add_subdirectory(transport)

if(BUILD_TESTING)
    add_subdirectory(mock)
endif()
