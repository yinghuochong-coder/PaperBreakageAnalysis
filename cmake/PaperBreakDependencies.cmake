macro(paperbreak_resolve_dependencies)
    if(DEFINED ENV{PAPERBREAK_QT_ROOT} AND NOT "$ENV{PAPERBREAK_QT_ROOT}" STREQUAL "")
        list(PREPEND CMAKE_PREFIX_PATH "$ENV{PAPERBREAK_QT_ROOT}")
    endif()

    find_package(Qt6 6.10.2 EXACT REQUIRED COMPONENTS Core Gui Widgets Network)
    find_package(OpenCV 4.12.0 EXACT REQUIRED COMPONENTS core imgproc imgcodecs)
    find_package(spdlog 1.17.0 CONFIG REQUIRED)
    find_package(nlohmann_json 3.12.0 CONFIG REQUIRED)
    find_package(SQLite3 3.53.4 REQUIRED)

    if(BUILD_TESTING)
        find_package(GTest 1.17.0 CONFIG REQUIRED)
    endif()

    if(PAPERBREAK_ENABLE_HIKROBOT)
        if(NOT DEFINED PAPERBREAK_MVS_ROOT OR PAPERBREAK_MVS_ROOT STREQUAL "")
            if(DEFINED ENV{PAPERBREAK_MVS_ROOT} AND NOT "$ENV{PAPERBREAK_MVS_ROOT}" STREQUAL "")
                set(PAPERBREAK_MVS_ROOT "$ENV{PAPERBREAK_MVS_ROOT}")
            else()
                message(FATAL_ERROR
                    "PAPERBREAK_ENABLE_HIKROBOT=ON requires PAPERBREAK_MVS_ROOT. "
                    "Install Hikrobot MVS Development SDK 4.8.0.3 and inject its root via "
                    "CMakeUserPresets.json or the PAPERBREAK_MVS_ROOT environment variable.")
            endif()
        endif()
        if(NOT IS_DIRECTORY "${PAPERBREAK_MVS_ROOT}")
            message(FATAL_ERROR
                "PAPERBREAK_MVS_ROOT does not name an existing directory: '${PAPERBREAK_MVS_ROOT}'.")
        endif()
        message(FATAL_ERROR
            "The Hikrobot adapter belongs to M3 and is intentionally unavailable in M0. "
            "Use PAPERBREAK_ENABLE_HIKROBOT=OFF for the M0 Mock-only build.")
    endif()

    set(PAPERBREAK_QT_VERSION "${Qt6_VERSION}")
    set(PAPERBREAK_OPENCV_VERSION "${OpenCV_VERSION}")
    set(PAPERBREAK_SPDLOG_VERSION "${spdlog_VERSION}")
    set(PAPERBREAK_JSON_VERSION "${nlohmann_json_VERSION}")
    set(PAPERBREAK_SQLITE_VERSION "${SQLite3_VERSION}")
endmacro()
