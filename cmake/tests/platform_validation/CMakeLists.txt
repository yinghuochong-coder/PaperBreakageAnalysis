cmake_minimum_required(VERSION 4.2)
project(PaperBreakPlatformValidation NONE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../..")
include(PaperBreakPlatform)

set(WIN32 TRUE)
set(MSVC TRUE)
set(MSVC_VERSION 1951)
set(CMAKE_CXX_COMPILER_ID "MSVC")
set(CMAKE_SIZEOF_VOID_P 8)

if(FAILURE_CASE STREQUAL "architecture")
    set(CMAKE_SIZEOF_VOID_P 4)
elseif(FAILURE_CASE STREQUAL "compiler")
    set(MSVC FALSE)
    set(CMAKE_CXX_COMPILER_ID "GNU")
endif()

paperbreak_validate_platform()

