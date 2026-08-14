if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(design_path "${SOURCE_DIR}/docs/validation/m7-01/nvme-block-format-v1.json")
file(READ "${design_path}" design)
string(JSON document_type TYPE "${design}")
if(NOT document_type STREQUAL "OBJECT")
    message(FATAL_ERROR "M7-01 design must contain a JSON object")
endif()

string(JSON schemaVersion GET "${design}" schemaVersion)
string(JSON format_formatVersion GET "${design}" format formatVersion)
string(JSON format_blockDurationMs GET "${design}" format blockDurationMs)
string(JSON format_pageAlignmentBytes GET "${design}" format pageAlignmentBytes)
string(JSON format_headerBytes GET "${design}" format headerBytes)
string(JSON format_indexEntryBytes GET "${design}" format indexEntryBytes)
string(JSON format_footerBytes GET "${design}" format footerBytes)

if(NOT schemaVersion EQUAL 1 OR NOT format_formatVersion EQUAL 1)
    message(FATAL_ERROR "Unexpected M7-01 schema or block format version")
endif()
if(NOT format_blockDurationMs EQUAL 1000)
    message(FATAL_ERROR "NVMe v1 must use one-second blocks")
endif()
if(NOT format_pageAlignmentBytes EQUAL 4096 OR
   NOT format_headerBytes EQUAL 4096 OR
   NOT format_footerBytes EQUAL 4096 OR
   NOT format_indexEntryBytes EQUAL 96)
    message(FATAL_ERROR "NVMe v1 page or index constants changed unexpectedly")
endif()

string(JSON byte_order GET "${design}" format byteOrder)
string(JSON compression GET "${design}" format compression)
string(JSON crc_algorithm GET "${design}" integrity algorithm)
string(JSON crc_check GET "${design}" integrity checkValueHex)
string(JSON design_status GET "${design}" designStatus)
string(JSON hardware_status GET "${design}" hardwareValidationStatus)
string(JSON rolling_share GET "${design}" capacityPolicy maximumRollingShareOfMeasuredSustainedWritePercent)
string(JSON reserve_slots GET "${design}" capacityPolicy minimumInProgressBlockReserveSlots)
if(NOT byte_order STREQUAL "little-endian" OR NOT compression STREQUAL "none")
    message(FATAL_ERROR "NVMe v1 must remain explicit little-endian raw storage")
endif()
if(NOT crc_algorithm STREQUAL "CRC-32C" OR NOT crc_check STREQUAL "E3069283")
    message(FATAL_ERROR "NVMe v1 CRC-32C parameters changed unexpectedly")
endif()
if(NOT design_status STREQUAL "accepted" OR NOT hardware_status STREQUAL "not-validated")
    message(FATAL_ERROR "The design is accepted but target hardware must remain not validated")
endif()
if(NOT rolling_share EQUAL 80 OR NOT reserve_slots EQUAL 1)
    message(FATAL_ERROR "NVMe capacity or bandwidth reserve policy changed unexpectedly")
endif()

string(JSON mono8_id GET "${design}" stableIds pixelFormats Mono8)
string(JSON mono10_id GET "${design}" stableIds pixelFormats Mono10LE16)
string(JSON mono12_id GET "${design}" stableIds pixelFormats Mono12LE16)
string(JSON bayer_id GET "${design}" stableIds pixelFormats BayerRG8)
string(JSON header_crc_offset GET "${design}" layout headerCrc32cOffset)
string(JSON payload_crc_offset GET "${design}" layout indexPayloadCrc32cOffset)
string(JSON entry_crc_offset GET "${design}" layout indexEntryCrc32cOffset)
string(JSON footer_crc_offset GET "${design}" layout footerCrc32cOffsetWithinFooter)
string(JSON commit_offset GET "${design}" layout commitMarkerOffsetWithinFooter)
if(NOT mono8_id EQUAL 1 OR NOT mono10_id EQUAL 2 OR
   NOT mono12_id EQUAL 3 OR NOT bayer_id EQUAL 4)
    message(FATAL_ERROR "NVMe v1 stable pixel format identifiers changed unexpectedly")
endif()
if(NOT header_crc_offset EQUAL 128 OR NOT payload_crc_offset EQUAL 76 OR
   NOT entry_crc_offset EQUAL 80 OR NOT footer_crc_offset EQUAL 4084 OR
   NOT commit_offset EQUAL 4088)
    message(FATAL_ERROR "NVMe v1 CRC or commit offsets changed unexpectedly")
endif()

foreach(field IN ITEMS
        cameraCount
        widthPixels
        heightPixels
        strideBytes
        maximumAppliedFramesPerSecond
        maxFrameBytes
        indexCapacity
        indexRegionBytes
        dataRegionBytes
        maximumBlockBytes
        rawPayloadBytesPerSecondAllCameras
        metadataBytesPerSecondAllCameras
        rollingWriteBytesPerSecondAllCameras
        minimumMeasuredSustainedWriteBytesPerSecond
        defaultRollingCacheWriteLimitMiBps
        configuredFramePoolCapacityPerCamera
        framePoolBytesAllCameras
        maximumCacheStorageGiBExample
        physicalSlotCount
        usableCommittedSlotCount
        balancedRetentionSeconds
        rawPayloadBytesPerDayAllCameras)
    string(JSON value GET "${design}" referenceWorkload "${field}")
    set("reference_${field}" "${value}")
endforeach()

math(EXPR calculated_frame_bytes
    "${reference_strideBytes} * ${reference_heightPixels}")
math(EXPR calculated_index_capacity
    "${reference_maximumAppliedFramesPerSecond} + 2")
math(EXPR raw_index_bytes
    "${calculated_index_capacity} * ${format_indexEntryBytes}")
math(EXPR calculated_index_region
    "((${raw_index_bytes} + ${format_pageAlignmentBytes} - 1) / ${format_pageAlignmentBytes}) * ${format_pageAlignmentBytes}")
math(EXPR raw_data_region
    "${calculated_index_capacity} * ${calculated_frame_bytes}")
math(EXPR calculated_data_region
    "((${raw_data_region} + ${format_pageAlignmentBytes} - 1) / ${format_pageAlignmentBytes}) * ${format_pageAlignmentBytes}")
math(EXPR calculated_block_bytes
    "${format_headerBytes} + ${calculated_index_region} + ${calculated_data_region} + ${format_footerBytes}")
math(EXPR calculated_raw_bps
    "${calculated_frame_bytes} * ${reference_maximumAppliedFramesPerSecond} * ${reference_cameraCount}")
math(EXPR calculated_metadata_bps
    "(${reference_maximumAppliedFramesPerSecond} * ${format_indexEntryBytes} + ${format_headerBytes} + ${format_footerBytes}) * ${reference_cameraCount}")
math(EXPR calculated_rolling_bps
    "${calculated_raw_bps} + ${calculated_metadata_bps}")
math(EXPR calculated_minimum_disk_bps
    "(${calculated_rolling_bps} * 100 + ${rolling_share} - 1) / ${rolling_share}")
math(EXPR calculated_default_write_limit_bps
    "${reference_defaultRollingCacheWriteLimitMiBps} * 1048576")
math(EXPR calculated_frame_pool_bytes
    "${reference_maxFrameBytes} * ${reference_configuredFramePoolCapacityPerCamera} * ${reference_cameraCount}")

if(NOT reference_cameraCount EQUAL 6 OR
   NOT reference_maxFrameBytes EQUAL calculated_frame_bytes OR
   NOT reference_indexCapacity EQUAL calculated_index_capacity OR
   NOT reference_indexRegionBytes EQUAL calculated_index_region OR
   NOT reference_dataRegionBytes EQUAL calculated_data_region OR
   NOT reference_maximumBlockBytes EQUAL calculated_block_bytes OR
   NOT reference_rawPayloadBytesPerSecondAllCameras EQUAL calculated_raw_bps OR
   NOT reference_metadataBytesPerSecondAllCameras EQUAL calculated_metadata_bps OR
   NOT reference_rollingWriteBytesPerSecondAllCameras EQUAL calculated_rolling_bps OR
   NOT reference_minimumMeasuredSustainedWriteBytesPerSecond EQUAL calculated_minimum_disk_bps OR
   NOT reference_framePoolBytesAllCameras EQUAL calculated_frame_pool_bytes OR
   NOT calculated_default_write_limit_bps LESS calculated_rolling_bps)
    message(FATAL_ERROR "M7-01 reference block or bandwidth arithmetic is inconsistent")
endif()

string(JSON default_limit_sufficient GET "${design}" referenceWorkload defaultRollingCacheWriteLimitSufficient)
string(JSON frame_pool_gib GET "${design}" referenceWorkload framePoolGiBAllCamerasApprox)
if(default_limit_sufficient OR NOT frame_pool_gib STREQUAL "21.82")
    message(FATAL_ERROR "M7-01 default write-limit or frame-pool memory baseline is inconsistent")
endif()

math(EXPR maximum_cache_bytes
    "${reference_maximumCacheStorageGiBExample} * 1073741824")
math(EXPR calculated_slots
    "${maximum_cache_bytes} / ${calculated_block_bytes}")
math(EXPR calculated_usable_slots
    "${calculated_slots} - ${reserve_slots}")
math(EXPR calculated_retention_seconds
    "${calculated_usable_slots} / ${reference_cameraCount}")
math(EXPR calculated_daily_bytes
    "${calculated_raw_bps} * 86400")
if(NOT reference_physicalSlotCount EQUAL calculated_slots OR
   NOT reference_usableCommittedSlotCount EQUAL calculated_usable_slots OR
   NOT reference_balancedRetentionSeconds EQUAL calculated_retention_seconds OR
   NOT reference_rawPayloadBytesPerDayAllCameras EQUAL calculated_daily_bytes)
    message(FATAL_ERROR "M7-01 reference capacity arithmetic is inconsistent")
endif()

string(JSON evidence_count LENGTH "${design}" deploymentEvidenceRequired)
if(NOT evidence_count EQUAL 5)
    message(FATAL_ERROR "M7-01 must keep all five target deployment evidence groups")
endif()

set(buffered_design_path "${SOURCE_DIR}/docs/validation/m7-01/nvme-block-format-v2.json")
file(READ "${buffered_design_path}" buffered_design)
string(JSON buffered_schema GET "${buffered_design}" schemaVersion)
string(JSON buffered_version GET "${buffered_design}" format formatVersion)
string(JSON buffered_magic GET "${buffered_design}" format headerMagicAsciiEscaped)
string(JSON buffered_flush GET "${buffered_design}" writePolicy flushFileBuffers)
string(JSON buffered_recovery GET "${buffered_design}" rollingCachePolicy crossRestartRecovery)
string(JSON buffered_limit_scope GET "${buffered_design}" rollingCachePolicy maximumCacheStorageGiBScope)
string(JSON buffered_minimum_mibps GET "${buffered_design}" performanceAcceptance minimumCommitMiBps)
if(NOT buffered_schema EQUAL 2 OR NOT buffered_version EQUAL 2 OR
   NOT buffered_magic STREQUAL "PBNVME2\\0" OR buffered_flush OR buffered_recovery OR
   NOT buffered_limit_scope STREQUAL "current-session" OR NOT buffered_minimum_mibps EQUAL 100)
    message(FATAL_ERROR "ADR-017 NVMe v2 buffered/session contract is inconsistent")
endif()

message(STATUS "M7-01 NVMe v1 compatibility and ADR-017 v2 buffered design are internally consistent")
