cmake_minimum_required(VERSION 4.2)
project(PaperBreakMissingHikrobotGigeRuntime NONE)

include("${CMAKE_CURRENT_LIST_DIR}/../../PaperBreakHikrobotRuntime.cmake")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/incomplete-runtime")
paperbreak_resolve_hikrobot_runtime(
    "${CMAKE_CURRENT_BINARY_DIR}/incomplete-runtime"
    core_runtime
    gige_runtime)
