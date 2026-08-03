add_library(paperbreak_pipeline STATIC src/module.cpp src/pipeline.cpp src/preview.cpp)
target_link_libraries(paperbreak_pipeline
    PUBLIC paperbreak_common paperbreak_camera paperbreak_compiler_options
    PRIVATE opencv_core opencv_imgproc opencv_imgcodecs)
target_include_directories(paperbreak_pipeline PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(paperbreak_pipeline PROPERTIES FOLDER "PaperBreak/Libraries")
install(TARGETS paperbreak_pipeline ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
