function(paperbreak_validate_platform)
    if(NOT WIN32)
        message(FATAL_ERROR
            "PaperBreakEdge supports only Windows 10/11 x64. "
            "Configure on Windows with Visual Studio 2026 MSVC.")
    endif()

    if(NOT MSVC)
        message(FATAL_ERROR
            "PaperBreakEdge supports only the MSVC compiler from Visual Studio 2026. "
            "The detected compiler is '${CMAKE_CXX_COMPILER_ID}'.")
    endif()

    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR
            "PaperBreakEdge supports only x64 builds. "
            "Select the x64 architecture or use VCPKG_TARGET_TRIPLET=x64-windows.")
    endif()

    if(MSVC_VERSION LESS 1950)
        message(FATAL_ERROR
            "PaperBreakEdge requires Visual Studio 2026 MSVC v145 (MSVC 19.50 or newer). "
            "Detected MSVC_VERSION=${MSVC_VERSION}.")
    endif()
endfunction()

