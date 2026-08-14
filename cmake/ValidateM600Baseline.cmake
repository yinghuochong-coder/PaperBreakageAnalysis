if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(baseline_dir "${SOURCE_DIR}/docs/validation/m6-00")
foreach(file_name IN ITEMS
        algorithm-acceptance-baseline-v1.json
        dataset-manifest-v1.schema.json
        dataset-manifest.template.json)
    file(READ "${baseline_dir}/${file_name}" json_text)
    string(JSON document_type TYPE "${json_text}")
    if(NOT document_type STREQUAL "OBJECT")
        message(FATAL_ERROR "${file_name} must contain a JSON object")
    endif()
endforeach()

file(READ "${baseline_dir}/algorithm-acceptance-baseline-v1.json" baseline)
string(JSON schema_version GET "${baseline}" schemaVersion)
string(JSON status GET "${baseline}" status)
string(JSON decision GET "${baseline}" gateDecision)
string(JSON prototype_only GET "${baseline}" prototypeOnly)
string(JSON camera_count GET "${baseline}" performanceGate cameraCount)
string(JSON camera_fps GET "${baseline}" performanceGate configuredFramesPerSecondPerCamera)
string(JSON required_fps GET "${baseline}" performanceGate requiredSustainedInputFramesPerSecond)
string(JSON theoretical_mean_ms GET "${baseline}" performanceGate singleWorkerTheoreticalMeanServiceTimeMs)
string(JSON blocking_count LENGTH "${baseline}" blockingItems)

if(NOT schema_version EQUAL 1)
    message(FATAL_ERROR "Unexpected M6-00 baseline schema version")
endif()
if(NOT status STREQUAL "blocked" OR NOT decision STREQUAL "not-approved")
    message(FATAL_ERROR "The M6-00 gate must remain blocked until external approvals exist")
endif()
if(NOT prototype_only)
    message(FATAL_ERROR "Algorithms must remain prototype-only while the M6-00 gate is blocked")
endif()
math(EXPR calculated_fps "${camera_count} * ${camera_fps}")
if(NOT camera_count EQUAL 6 OR NOT camera_fps EQUAL 60 OR NOT required_fps EQUAL calculated_fps OR
   NOT theoretical_mean_ms STREQUAL "2.7778")
    message(FATAL_ERROR "The six-camera workload calculation is inconsistent")
endif()
if(NOT blocking_count EQUAL 5)
    message(FATAL_ERROR "The M6-00 baseline must enumerate all five blocking evidence groups")
endif()

foreach(quality_threshold IN ITEMS
        minimumEventRecall
        minimumEventPrecision
        maximumFalseAlarmsPerCameraHour
        maximumP95DetectionDelayMs
        minimumPositiveEventsInFrozenTest
        minimumNegativeCameraHoursInFrozenTest)
    string(JSON threshold_type TYPE "${baseline}" qualityGate "${quality_threshold}")
    if(NOT threshold_type STREQUAL "NULL")
        message(FATAL_ERROR "Unapproved quality threshold ${quality_threshold} must remain null")
    endif()
endforeach()

foreach(performance_threshold IN ITEMS
        maximumMeanFrameTimeMs
        maximumP95FrameTimeMs
        maximumP99FrameTimeMs
        minimumSustainedProcessedFramesPerSecond
        maximumAlgorithmCpuPercent
        maximumAlgorithmWorkingSetMiB)
    string(JSON threshold_type TYPE "${baseline}" performanceGate "${performance_threshold}")
    if(NOT threshold_type STREQUAL "NULL")
        message(FATAL_ERROR "Unapproved performance threshold ${performance_threshold} must remain null")
    endif()
endforeach()

file(READ "${baseline_dir}/dataset-manifest-v1.schema.json" manifest_schema)
string(JSON manifest_schema_version GET "${manifest_schema}" properties schemaVersion const)
file(READ "${baseline_dir}/dataset-manifest.template.json" manifest_template)
string(JSON template_schema_version GET "${manifest_template}" schemaVersion)
string(JSON template_status GET "${manifest_template}" status)
if(NOT manifest_schema_version EQUAL 1 OR NOT template_schema_version EQUAL 1)
    message(FATAL_ERROR "Dataset manifest schema versions are inconsistent")
endif()
if(NOT template_status STREQUAL "draft")
    message(FATAL_ERROR "The dataset template must never present itself as frozen evidence")
endif()

message(STATUS "M6-00 acceptance baseline is internally consistent and correctly blocked")
