add_executable(PaperBreakEdgeConsole WIN32 main.cpp)
target_link_libraries(PaperBreakEdgeConsole
    PRIVATE
        paperbreak_common
        paperbreak_logging
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
