add_library(paperbreak_event STATIC
    src/candidate_event.cpp
    src/event_window.cpp
    src/key_frame.cpp
    src/memory_ring.cpp
    src/module.cpp)
target_link_libraries(paperbreak_event
    PUBLIC
        paperbreak_algorithm
        paperbreak_camera
        paperbreak_common
        paperbreak_compiler_options)
target_include_directories(paperbreak_event PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(paperbreak_event PROPERTIES FOLDER "PaperBreak/Libraries")
install(TARGETS paperbreak_event ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

add_library(paperbreak_event_codec STATIC src/key_frame_opencv.cpp)
target_link_libraries(paperbreak_event_codec
    PUBLIC paperbreak_event
    PRIVATE opencv_core opencv_imgproc opencv_imgcodecs paperbreak_compiler_options)
target_include_directories(paperbreak_event_codec PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(paperbreak_event_codec PROPERTIES FOLDER "PaperBreak/Libraries")
install(TARGETS paperbreak_event_codec ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
