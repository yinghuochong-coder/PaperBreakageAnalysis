add_library(paperbreak_uplink_simulator_core STATIC
    src/simulator.cpp
    src/workspace.cpp)
target_link_libraries(paperbreak_uplink_simulator_core
    PUBLIC
        paperbreak_common
        paperbreak_uplink
        paperbreak_compiler_options
    PRIVATE
        Qt6::Core
        Qt6::Network
        Qt6::HttpServer
        Qt6::WebSockets
        nlohmann_json::nlohmann_json
        SQLite3::SQLite3)
target_include_directories(paperbreak_uplink_simulator_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(paperbreak_uplink_simulator_core PROPERTIES
    FOLDER "PaperBreak/Tools")

add_executable(PaperBreakUplinkSimulator WIN32
    main.cpp
    src/main_window.cpp)
target_link_libraries(PaperBreakUplinkSimulator
    PRIVATE
        paperbreak_uplink_simulator_core
        paperbreak_common
        paperbreak_compiler_options
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets)
target_include_directories(PaperBreakUplinkSimulator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(PaperBreakUplinkSimulator PROPERTIES
    OUTPUT_NAME PaperBreakUplinkSimulator
    WIN32_EXECUTABLE TRUE
    FOLDER "PaperBreak/Tools")

install(TARGETS PaperBreakUplinkSimulator
    RUNTIME_DEPENDENCY_SET paperbreak_uplink_simulator_runtime
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT UplinkSimulator
    EXCLUDE_FROM_ALL)
install(RUNTIME_DEPENDENCY_SET paperbreak_uplink_simulator_runtime
    DIRECTORIES
        "$<TARGET_FILE_DIR:Qt6::Core>"
        "$<TARGET_FILE_DIR:spdlog::spdlog>"
        "$<TARGET_FILE_DIR:SQLite3::SQLite3>"
    PRE_EXCLUDE_REGEXES
        "api-ms-.*"
        "ext-ms-.*"
    POST_EXCLUDE_REGEXES
        ".*[\\/]system32[\\/].*\\.dll"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT UplinkSimulator
    EXCLUDE_FROM_ALL)
qt_generate_deploy_app_script(
    TARGET PaperBreakUplinkSimulator
    OUTPUT_SCRIPT paperbreak_uplink_simulator_deploy_script
    NO_TRANSLATIONS)
install(SCRIPT "${paperbreak_uplink_simulator_deploy_script}"
    COMPONENT UplinkSimulator
    EXCLUDE_FROM_ALL)
