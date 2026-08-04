add_library(paperbreak_algorithm_classical STATIC
    src/classical_vision_detector.cpp)
target_link_libraries(paperbreak_algorithm_classical
    PUBLIC
        paperbreak_algorithm
        paperbreak_compiler_options
    PRIVATE
        opencv_core
        opencv_imgproc)
target_include_directories(paperbreak_algorithm_classical PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(paperbreak_algorithm_classical PROPERTIES
    FOLDER "PaperBreak/Libraries")
install(TARGETS paperbreak_algorithm_classical ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
