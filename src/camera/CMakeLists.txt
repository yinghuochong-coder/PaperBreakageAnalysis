add_library(paperbreak_camera STATIC
    src/acquisition.cpp
    src/camera.cpp
    src/frame.cpp
    src/frame_pool.cpp
    src/state.cpp
    src/module.cpp)
target_link_libraries(paperbreak_camera PUBLIC paperbreak_common paperbreak_compiler_options)
target_include_directories(paperbreak_camera PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(paperbreak_camera PROPERTIES FOLDER "PaperBreak/Libraries")
install(TARGETS paperbreak_camera ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
