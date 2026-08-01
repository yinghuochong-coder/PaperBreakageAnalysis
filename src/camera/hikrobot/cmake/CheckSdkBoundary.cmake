if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

file(GLOB_RECURSE candidate_files
    "${SOURCE_DIR}/src/*.cpp"
    "${SOURCE_DIR}/src/*.hpp"
    "${SOURCE_DIR}/src/*.h"
    "${SOURCE_DIR}/src/CMakeLists.txt")

set(violations)
set(adapter_dir "${SOURCE_DIR}/src/camera/hikrobot")
foreach(candidate IN LISTS candidate_files)
    cmake_path(IS_PREFIX adapter_dir "${candidate}" NORMALIZE is_adapter)
    if(is_adapter)
        continue()
    endif()
    file(READ "${candidate}" contents)
    if(contents MATCHES "MvCameraControl|MvErrorDefine|MV_CC_|MV_E_")
        list(APPEND violations "${candidate}")
    endif()
endforeach()

if(violations)
    list(JOIN violations "\n  " violation_text)
    message(FATAL_ERROR
        "Hikrobot MVS SDK reference escaped src/camera/hikrobot:\n  ${violation_text}")
endif()

message(STATUS "Hikrobot MVS SDK references are isolated to src/camera/hikrobot")
