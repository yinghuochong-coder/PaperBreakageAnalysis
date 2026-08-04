add_library(paperbreak_algorithm_mock STATIC
    src/mock_trigger_detector.cpp)
target_link_libraries(paperbreak_algorithm_mock
    PUBLIC
        paperbreak_algorithm
        paperbreak_compiler_options)
target_include_directories(paperbreak_algorithm_mock PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(paperbreak_algorithm_mock PROPERTIES FOLDER "PaperBreak/Libraries")
install(TARGETS paperbreak_algorithm_mock ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
