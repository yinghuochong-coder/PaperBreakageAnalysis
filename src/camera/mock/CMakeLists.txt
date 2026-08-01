add_library(paperbreak_camera_mock STATIC
    src/mock_camera.cpp)
target_link_libraries(paperbreak_camera_mock
    PUBLIC
        paperbreak_camera
        paperbreak_common
        paperbreak_compiler_options)
target_include_directories(paperbreak_camera_mock PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(paperbreak_camera_mock PROPERTIES FOLDER "PaperBreak/Libraries")
install(TARGETS paperbreak_camera_mock ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
