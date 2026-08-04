add_library(paperbreak_event STATIC
    src/memory_ring.cpp
    src/module.cpp)
target_link_libraries(paperbreak_event
    PUBLIC
        paperbreak_camera
        paperbreak_common
        paperbreak_compiler_options)
target_include_directories(paperbreak_event PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(paperbreak_event PROPERTIES FOLDER "PaperBreak/Libraries")
install(TARGETS paperbreak_event ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
