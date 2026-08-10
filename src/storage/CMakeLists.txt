add_library(paperbreak_storage STATIC
    src/event_inspector.cpp
    src/event_file_system_win.cpp
    src/metadata_database.cpp
    src/event_store.cpp
    src/nvme_index.cpp
    src/nvme_block_file_win.cpp
    src/nvme_cache.cpp
    src/storage_policy.cpp
    src/module.cpp)
target_link_libraries(paperbreak_storage
    PUBLIC paperbreak_common paperbreak_event paperbreak_compiler_options
    PRIVATE nlohmann_json::nlohmann_json SQLite3::SQLite3 bcrypt)
target_include_directories(paperbreak_storage PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(paperbreak_storage PROPERTIES FOLDER "PaperBreak/Libraries")
install(TARGETS paperbreak_storage ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
