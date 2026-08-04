add_library(paperbreak_algorithm STATIC
    src/module.cpp
    src/trigger.cpp)
target_link_libraries(paperbreak_algorithm
    PUBLIC
        paperbreak_camera
        paperbreak_common
        paperbreak_compiler_options)
target_include_directories(paperbreak_algorithm PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(paperbreak_algorithm PROPERTIES FOLDER "PaperBreak/Libraries")
install(TARGETS paperbreak_algorithm ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

add_subdirectory(mock)
