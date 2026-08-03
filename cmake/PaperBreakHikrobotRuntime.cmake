function(paperbreak_resolve_hikrobot_runtime runtime_dir out_core_runtime out_gige_runtime)
    cmake_path(CONVERT "${runtime_dir}" TO_CMAKE_PATH_LIST normalized_runtime_dir NORMALIZE)
    set(core_runtime "${normalized_runtime_dir}/MvCameraControl.dll")
    set(gige_runtime "${normalized_runtime_dir}/MVGigEVisionSDK.dll")

    # Check the dynamically loaded GigE transport first so a partial Runtime installation has a
    # precise configure-time diagnostic instead of failing later as MV_E_LOAD_LIBRARY.
    foreach(required_file IN ITEMS "${gige_runtime}" "${core_runtime}")
        if(NOT EXISTS "${required_file}")
            message(FATAL_ERROR
                "Hikrobot MVS Runtime 4.8.0.3 x64 file is missing: '${required_file}'.")
        endif()
    endforeach()

    foreach(runtime_file IN ITEMS "${core_runtime}" "${gige_runtime}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env
                "PAPERBREAK_MVS_VERSION_FILE=${runtime_file}"
                powershell -NoProfile -NonInteractive -Command
                "[Console]::Write((Get-Item -LiteralPath $env:PAPERBREAK_MVS_VERSION_FILE).VersionInfo.FileVersion)"
            RESULT_VARIABLE version_result
            OUTPUT_VARIABLE runtime_version
            ERROR_VARIABLE version_error
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT version_result EQUAL 0)
            message(FATAL_ERROR
                "Unable to read Hikrobot MVS Runtime file version: ${version_error}")
        endif()
        if(NOT runtime_version STREQUAL "4.8.0.3")
            message(FATAL_ERROR
                "Hikrobot MVS Runtime version '${runtime_version}' is not the approved "
                "4.8.0.3 baseline: '${runtime_file}'.")
        endif()
    endforeach()

    set(${out_core_runtime} "${core_runtime}" PARENT_SCOPE)
    set(${out_gige_runtime} "${gige_runtime}" PARENT_SCOPE)
endfunction()
