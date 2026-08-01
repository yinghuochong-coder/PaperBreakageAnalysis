find_program(CLANG_FORMAT_EXECUTABLE
    NAMES clang-format clang-format.exe
    HINTS
        "$ENV{VSINSTALLDIR}/VC/Tools/Llvm/x64/bin"
        "$ENV{VSINSTALLDIR}/VC/Tools/Llvm/bin")
if(NOT CLANG_FORMAT_EXECUTABLE)
    message(FATAL_ERROR
        "clang-format was not found. Install it with the Visual Studio C++ workload "
        "or add it to PATH before running the format-check target.")
endif()

if(DEFINED FILE_LIST)
    if(NOT EXISTS "${FILE_LIST}")
        message(FATAL_ERROR "clang-format file list does not exist: ${FILE_LIST}")
    endif()
    file(STRINGS "${FILE_LIST}" format_files ENCODING UTF-8)
else()
    string(REPLACE "|" ";" format_files "${FILES}")
endif()
foreach(format_file IN LISTS format_files)
    execute_process(
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror "${format_file}"
        RESULT_VARIABLE format_result
        OUTPUT_VARIABLE format_output
        ERROR_VARIABLE format_error)
    if(NOT format_result EQUAL 0)
        message(FATAL_ERROR
            "clang-format rejected '${format_file}'.\n${format_output}${format_error}")
    endif()
endforeach()
