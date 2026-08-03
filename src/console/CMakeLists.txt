add_library(paperbreak_console_model STATIC
    src/client_state_store.cpp
    src/navigation_model.cpp
    src/preview_client.cpp
    src/tray_status_model.cpp)
target_link_libraries(paperbreak_console_model
    PUBLIC
        paperbreak_common
        paperbreak_ipc
        paperbreak_compiler_options
    PRIVATE
        Qt6::Gui
        nlohmann_json::nlohmann_json)
target_include_directories(paperbreak_console_model
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
set_target_properties(paperbreak_console_model PROPERTIES
    OUTPUT_NAME paperbreak_console_model
    FOLDER "PaperBreak/Libraries")

add_executable(PaperBreakEdgeConsole WIN32
    main.cpp
    src/main_window.cpp
    src/system_tray_controller.cpp)
target_link_libraries(PaperBreakEdgeConsole
    PRIVATE
        paperbreak_common
        paperbreak_console_model
        paperbreak_logging
        paperbreak_windows_service_control
        paperbreak_compiler_options
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets)
set_target_properties(PaperBreakEdgeConsole PROPERTIES
    OUTPUT_NAME PaperBreakEdgeConsole
    WIN32_EXECUTABLE TRUE
    FOLDER "PaperBreak/Applications")
install(TARGETS PaperBreakEdgeConsole RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

qt_generate_deploy_app_script(
    TARGET PaperBreakEdgeConsole
    OUTPUT_SCRIPT paperbreak_console_deploy_script
    NO_TRANSLATIONS)
install(SCRIPT "${paperbreak_console_deploy_script}")
