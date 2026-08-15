add_library(paperbreak_time STATIC
    src/time_model.cpp
    src/time_sync_runtime.cpp)
target_link_libraries(paperbreak_time PUBLIC paperbreak_common paperbreak_compiler_options)
target_include_directories(paperbreak_time PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(paperbreak_time PROPERTIES FOLDER "PaperBreak/Libraries")
install(TARGETS paperbreak_time ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
