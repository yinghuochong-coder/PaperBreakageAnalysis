#include "paperbreak/service/system_commands.hpp"

#include "paperbreak/common/camera_slots.hpp"
#include "paperbreak/common/version.hpp"
#include "paperbreak/service/camera_startup.hpp"

#include <nlohmann/json.hpp>

#include "paperbreak/camera/control.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace paperbreak::service
{
namespace
{

using Json = nlohmann::json;

constexpr std::size_t maximum_diagnostic_bytes = 8U * 1024U * 1024U;

Error command_error(std::string code, Severity severity, std::string message, std::string operation,
                    bool retryable = false);

struct ZipEntry final
{
    std::string name;
    std::string content;
};

void append_u16(std::vector<std::byte>& output, const std::uint16_t value)
{
    output.push_back(static_cast<std::byte>(value & 0xffU));
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::byte>& output, const std::uint32_t value)
{
    append_u16(output, static_cast<std::uint16_t>(value & 0xffffU));
    append_u16(output, static_cast<std::uint16_t>((value >> 16U) & 0xffffU));
}

void append_text(std::vector<std::byte>& output, const std::string_view value)
{
    output.reserve(output.size() + value.size());
    for (const unsigned char character : value)
        output.push_back(static_cast<std::byte>(character));
}

std::uint32_t crc32(const std::string_view value) noexcept
{
    std::uint32_t crc = 0xffffffffU;
    for (const unsigned char character : value)
    {
        crc ^= character;
        for (unsigned bit = 0U; bit < 8U; ++bit)
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

Result<std::vector<std::byte>> make_zip(const std::vector<ZipEntry>& entries)
{
    struct CentralEntry final
    {
        const ZipEntry* entry{};
        std::uint32_t crc{};
        std::uint32_t offset{};
    };
    std::vector<std::byte> output;
    std::vector<CentralEntry> central;
    central.reserve(entries.size());
    for (const auto& entry : entries)
    {
        if (entry.name.empty() || entry.name.size() > (std::numeric_limits<std::uint16_t>::max)() ||
            entry.content.size() > (std::numeric_limits<std::uint32_t>::max)() ||
            output.size() > (std::numeric_limits<std::uint32_t>::max)())
            return Result<std::vector<std::byte>>::failure(
                command_error("SYS_DIAGNOSTIC_TOO_LARGE", Severity::error, "诊断包条目超过允许范围",
                              "ipc.system.exportDiagnostics"));
        const auto size = static_cast<std::uint32_t>(entry.content.size());
        const auto checksum = crc32(entry.content);
        central.push_back({.entry = &entry,
                           .crc = checksum,
                           .offset = static_cast<std::uint32_t>(output.size())});
        append_u32(output, 0x04034b50U);
        append_u16(output, 20U);
        append_u16(output, 0x0800U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u32(output, checksum);
        append_u32(output, size);
        append_u32(output, size);
        append_u16(output, static_cast<std::uint16_t>(entry.name.size()));
        append_u16(output, 0U);
        append_text(output, entry.name);
        append_text(output, entry.content);
        if (output.size() > maximum_diagnostic_bytes)
            return Result<std::vector<std::byte>>::failure(
                command_error("SYS_DIAGNOSTIC_TOO_LARGE", Severity::error,
                              "诊断包超过 8 MiB 内部上限", "ipc.system.exportDiagnostics"));
    }

    const auto central_offset = static_cast<std::uint32_t>(output.size());
    for (const auto& item : central)
    {
        const auto size = static_cast<std::uint32_t>(item.entry->content.size());
        append_u32(output, 0x02014b50U);
        append_u16(output, 20U);
        append_u16(output, 20U);
        append_u16(output, 0x0800U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u32(output, item.crc);
        append_u32(output, size);
        append_u32(output, size);
        append_u16(output, static_cast<std::uint16_t>(item.entry->name.size()));
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u32(output, 0U);
        append_u32(output, item.offset);
        append_text(output, item.entry->name);
    }
    const auto central_size = static_cast<std::uint32_t>(output.size() - central_offset);
    if (central.size() > (std::numeric_limits<std::uint16_t>::max)())
        return Result<std::vector<std::byte>>::failure(
            command_error("SYS_DIAGNOSTIC_TOO_LARGE", Severity::error, "诊断包条目数量超过允许范围",
                          "ipc.system.exportDiagnostics"));
    append_u32(output, 0x06054b50U);
    append_u16(output, 0U);
    append_u16(output, 0U);
    append_u16(output, static_cast<std::uint16_t>(central.size()));
    append_u16(output, static_cast<std::uint16_t>(central.size()));
    append_u32(output, central_size);
    append_u32(output, central_offset);
    append_u16(output, 0U);
    if (output.size() > maximum_diagnostic_bytes)
        return Result<std::vector<std::byte>>::failure(
            command_error("SYS_DIAGNOSTIC_TOO_LARGE", Severity::error, "诊断包超过 8 MiB 内部上限",
                          "ipc.system.exportDiagnostics"));
    return Result<std::vector<std::byte>>::success(std::move(output));
}

bool sensitive_key(std::string key)
{
    std::ranges::transform(key, key.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    const auto has = [&key](const std::string_view fragment) {
        return key.find(fragment) != std::string::npos;
    };
    return has("password") || has("token") || has("secret") || has("privatekey") ||
           has("private-key") || has("private_key") || has("credentialreference") ||
           has("certificatereference");
}

void redact_json(Json& value)
{
    if (value.is_object())
    {
        for (auto& [key, child] : value.items())
        {
            if (sensitive_key(key))
                child = child.is_string() && child.get_ref<const std::string&>().empty()
                            ? ""
                            : "<redacted>";
            else
                redact_json(child);
        }
    }
    else if (value.is_array())
    {
        for (auto& child : value)
            redact_json(child);
    }
    else if (value.is_string())
        value = logging::redact_sensitive(value.get_ref<const std::string&>());
}

Error command_error(std::string code, const Severity severity, std::string message,
                    std::string operation, const bool retryable)
{
    return make_error(std::move(code), severity, std::move(message), "ipc", std::move(operation),
                      retryable);
}

Error camera_provider_required(std::string operation)
{
    return command_error("SYS_NOT_SUPPORTED", Severity::warning,
                         "当前服务未装配 Hikrobot MVS 相机提供者；请检查部署完整性",
                         std::move(operation));
}

Result<Json> request_payload(const ipc::RequestMessage& request)
{
    if (!request.binary.empty())
    {
        return Result<Json>::failure(command_error("IPC_REQUEST_INVALID", Severity::error,
                                                   "当前 system 命令不接受二进制负载",
                                                   "ipc.system.payload"));
    }
    Json payload = Json::parse(request.payload_json, nullptr, false);
    if (payload.is_discarded() || !payload.is_object())
    {
        return Result<Json>::failure(command_error("IPC_REQUEST_INVALID", Severity::error,
                                                   "system 命令 payload 必须是对象",
                                                   "ipc.system.payload"));
    }
    return Result<Json>::success(std::move(payload));
}

Json config_summary(const config::ConfigSnapshot& snapshot)
{
    return {{"configSchemaVersion", snapshot.stored->config_schema_version},
            {"storedConfigRevision", snapshot.stored_config_revision},
            {"effectiveConfigRevision", snapshot.effective_config_revision},
            {"pendingRestartPaths", snapshot.pending_restart_paths},
            {"recoveredFromHistory", snapshot.recovered_from_history}};
}

logging::Level configured_logging_level(const config::LogLevel level)
{
    switch (level)
    {
    case config::LogLevel::trace:
        return logging::Level::trace;
    case config::LogLevel::debug:
        return logging::Level::debug;
    case config::LogLevel::info:
        return logging::Level::info;
    case config::LogLevel::warning:
        return logging::Level::warning;
    case config::LogLevel::error:
        return logging::Level::error;
    case config::LogLevel::critical:
        return logging::Level::critical;
    }
    return logging::Level::info;
}

Result<ipc::CommandResponse> status_response(config::ConfigRepository& repository,
                                             const ServiceStatusStore& status_store,
                                             const logging::LoggingRuntime* logging_runtime)
{
    auto configuration = repository.snapshot();
    if (!configuration)
    {
        return Result<ipc::CommandResponse>::failure(configuration.error());
    }
    const ServiceStatusSnapshot status = status_store.snapshot();
    Json payload = config_summary(configuration.value());
    payload["serviceState"] = service_state_name(status.state);
    payload["acceptingWrites"] = status.accepting_writes;
    payload["startedAt"] = status.started_at;
    payload["timestamp"] = current_utc_timestamp();
    payload["machineId"] = configuration.value().effective->system.machine_id;
    payload["loggingLevel"] = std::string{
        logging_runtime != nullptr ? logging::level_name(logging_runtime->minimum_level())
                                   : logging::level_name(configured_logging_level(
                                         configuration.value().effective->logging.level))};
    return Result<ipc::CommandResponse>::success({.payload_json = payload.dump(), .binary = {}});
}

Json version_json()
{
    const auto& version = version_info();
    return {{"applicationVersion", version.application_version},
            {"gitCommit", version.git_commit},
            {"gitDirty", version.git_dirty},
            {"buildTimeUtc", version.build_time_utc},
            {"compiler", version.compiler},
            {"dependencies",
             {{"qt", version.qt_version},
              {"opencv", version.opencv_version},
              {"spdlog", version.spdlog_version},
              {"nlohmannJson", version.json_version},
              {"sqlite", version.sqlite_version}}}};
}

Result<ipc::CommandResponse> version_response()
{
    return Result<ipc::CommandResponse>::success(
        {.payload_json = version_json().dump(), .binary = {}});
}

std::filesystem::path path_from_utf8(const std::string_view value)
{
    std::u8string converted;
    converted.reserve(value.size());
    for (const unsigned char byte : value)
        converted.push_back(static_cast<char8_t>(byte));
    return std::filesystem::path{converted};
}

std::string path_to_utf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

Result<ipc::CommandResponse> locations_response(config::ConfigRepository& repository,
                                                const std::filesystem::path& config_directory)
{
    auto configuration = repository.snapshot();
    if (!configuration)
        return Result<ipc::CommandResponse>::failure(configuration.error());
    auto event_root = path_from_utf8(configuration.value().effective->storage.event_root);
    if (event_root.is_relative())
        event_root = config_directory / event_root;
    std::error_code path_error;
    event_root = std::filesystem::absolute(event_root, path_error).lexically_normal();
    if (path_error)
        return Result<ipc::CommandResponse>::failure(command_error(
            "SYS_INTERNAL_ERROR", Severity::error, "无法解析事件目录", "ipc.system.getLocations"));
    return Result<ipc::CommandResponse>::success(
        {.payload_json = Json{{"eventRoot", path_to_utf8(event_root)}}.dump(), .binary = {}});
}

bool has_only_field(const Json& object, const std::string_view field)
{
    return object.size() == 1U && object.contains(std::string{field});
}

bool has_only_fields(const Json& object, const std::initializer_list<std::string_view> fields)
{
    for (auto iterator = object.begin(); iterator != object.end(); ++iterator)
    {
        const std::string key = iterator.key();
        if (std::none_of(fields.begin(), fields.end(),
                         [&key](const std::string_view allowed) { return key == allowed; }))
        {
            return false;
        }
    }
    return true;
}

Json event_config_json(const config::EventConfig& value)
{
    return {{"preEventSeconds", value.pre_event_seconds},
            {"postEventSeconds", value.post_event_seconds},
            {"maxEventSeconds", value.max_event_seconds},
            {"mergeGapSeconds", value.merge_gap_seconds},
            {"keyFrameCount", value.key_frame_count},
            {"saveRaw", value.save_raw},
            {"generatePreviewVideo", value.generate_preview_video},
            {"uploadPolicy", value.upload_policy},
            {"retentionDays", value.retention_days}};
}

Json storage_config_json(const config::StorageConfig& value)
{
    return {{"eventRoot", value.event_root},
            {"cacheRoot", value.cache_root},
            {"rollingCacheEnabled", value.rolling_cache_enabled},
            {"maximumCacheStorageGiB", value.maximum_cache_storage_gib},
            {"rollingCacheWriteLimitMiBps", value.rolling_cache_write_limit_mibps},
            {"rollingCacheIoTimeoutMs", value.rolling_cache_io_timeout_ms},
            {"warningFreeSpaceGiB", value.warning_free_space_gib},
            {"criticalFreeSpaceGiB", value.critical_free_space_gib},
            {"stopFreeSpaceGiB", value.stop_free_space_gib},
            {"maximumEventStorageGiB", value.maximum_event_storage_gib}};
}

Json uplink_config_json(const config::UplinkConfig& value)
{
    return {{"enabled", value.enabled},
            {"serverUrl", value.server_url},
            {"heartbeatSeconds", value.heartbeat_seconds},
            {"chunkBytes", value.chunk_bytes},
            {"ioTimeoutMs", value.io_timeout_ms},
            {"uploadLimitMiBps", value.upload_limit_mibps},
            {"credentialReference", value.credential_reference},
            {"certificateReference", value.certificate_reference}};
}

Json algorithm_config_json(const config::AlgorithmConfig& value)
{
    const auto downsample_mode = [&] {
        switch (value.downsample_mode)
        {
        case config::AlgorithmDownsampleMode::disabled:
            return "disabled";
        case config::AlgorithmDownsampleMode::half:
            return "half";
        case config::AlgorithmDownsampleMode::quarter:
            return "quarter";
        }
        return "disabled";
    }();
    return {{"enabled", value.enabled},
            {"type", value.type},
            {"roi",
             {{"width", value.roi.width},
              {"height", value.roi.height},
              {"offsetX", value.roi.offset_x},
              {"offsetY", value.roi.offset_y}}},
            {"downsampleMode", downsample_mode},
            {"processingFps", static_cast<std::uint32_t>(value.processing_fps)},
            {"candidateThreshold", value.candidate_threshold},
            {"confirmationThreshold", value.confirmation_threshold},
            {"confirmationDurationMs", value.confirmation_duration_ms},
            {"cooldownMs", value.cooldown_ms},
            {"rearmDurationMs", value.rearm_duration_ms},
            {"modelReference", value.model_reference},
            {"modelVersion", value.model_version},
            {"device", value.device},
            {"debugOverlay", value.debug_overlay}};
}

Json detector_info_json(const algorithm::DetectorInfo& value)
{
    return {{"pluginId", value.plugin_id},
            {"displayName", value.display_name},
            {"implementationVersion", value.implementation_version},
            {"modelVersion", value.model_version},
            {"supportsHotUpdate", value.supports_hot_update},
            {"prototypeOnly", value.prototype_only}};
}

std::string_view candidate_type_name(const algorithm::DetectionCandidateType value) noexcept
{
    using Type = algorithm::DetectionCandidateType;
    switch (value)
    {
    case Type::none:
        return "none";
    case Type::paper_break:
        return "paper-break";
    case Type::paper_missing:
        return "paper-missing";
    case Type::obstruction:
        return "obstruction";
    case Type::flicker:
        return "flicker";
    case Type::indeterminate:
        return "indeterminate";
    }
    return "unknown";
}

Json detection_json(const algorithm::DetectionResult& value)
{
    Json debug_metrics = Json::array();
    for (const auto& metric : value.debug_metrics)
        debug_metrics.push_back({{"name", metric.name}, {"value", metric.value}});
    return {{"triggered", value.triggered},
            {"anomalous", value.anomalous},
            {"triggerSource", algorithm::to_string(value.trigger_source)},
            {"candidateType", candidate_type_name(value.candidate_type)},
            {"cameraId", value.camera_id},
            {"sequenceNumber", value.sequence_number},
            {"cameraFrameNumber", value.camera_frame_number},
            {"evaluatedRegion",
             {{"offsetX", value.evaluated_region.offset_x},
              {"offsetY", value.evaluated_region.offset_y},
              {"width", value.evaluated_region.width},
              {"height", value.evaluated_region.height}}},
            {"meanGrayscale", value.mean_grayscale},
            {"meanGrayscaleChange", value.mean_grayscale_change},
            {"paperRatio", value.paper_ratio},
            {"confidence", value.confidence},
            {"areaRatio", value.area_ratio},
            {"changeScore", value.change_score},
            {"processingTimeUs", value.processing_time.count()},
            {"detectorVersion", value.detector_version},
            {"modelVersion", value.model_version},
            {"reason", value.reason},
            {"debugMetrics", std::move(debug_metrics)}};
}

Json algorithm_runtime_json(const AlgorithmRuntimeSnapshot& value)
{
    const auto& metrics = value.metrics;
    Json result{{"cameraId", value.camera_id},
                {"configRevision", value.config_revision},
                {"state", to_string(value.state)},
                {"hasCurrentFrame", value.has_current_frame},
                {"latestSequenceNumber", value.latest_sequence_number},
                {"metrics",
                 {{"queueDepth", metrics.frame_queue_depth},
                  {"queueCapacity", metrics.frame_queue_capacity},
                  {"queueHighWatermark", metrics.frame_queue_high_watermark},
                  {"submittedFrames", metrics.submitted_frames},
                  {"processedFrames", metrics.processed_frames},
                  {"skippedFrames", metrics.skipped_frames},
                  {"sampledSkippedFrames", metrics.sampled_skipped_frames},
                  {"missedProcessingSlots", metrics.missed_processing_slots},
                  {"configuredProcessingFps", metrics.configured_processing_fps},
                  {"detectorFailures", metrics.detector_failures},
                  {"consecutiveDetectorFailures", metrics.consecutive_detector_failures},
                  {"consecutiveBacklogEvents", metrics.consecutive_backlog_events},
                  {"backlogActive", metrics.backlog_active},
                  {"consecutiveBadBacklogWindows", metrics.consecutive_bad_backlog_windows},
                  {"consecutiveHealthyBacklogWindows", metrics.consecutive_healthy_backlog_windows},
                  {"resultQueueRejected", metrics.result_queue_rejected},
                  {"rearmPending", metrics.rearm_pending},
                  {"rearmSuppressedResults", metrics.rearm_suppressed_results},
                  {"processCalls", metrics.detector_process_calls},
                  {"lastProcessingTimeUs", metrics.last_algorithm_processing_time.count()},
                  {"averageProcessingTimeUs", metrics.average_algorithm_processing_time.count()},
                  {"maximumProcessingTimeUs", metrics.maximum_algorithm_processing_time.count()},
                  {"lastQueueWaitTimeUs", metrics.last_queue_wait_time.count()},
                  {"averageQueueWaitTimeUs", metrics.average_queue_wait_time.count()},
                  {"maximumQueueWaitTimeUs", metrics.maximum_queue_wait_time.count()},
                  {"lastEndToEndTimeUs", metrics.last_end_to_end_time.count()},
                  {"averageEndToEndTimeUs", metrics.average_end_to_end_time.count()},
                  {"maximumEndToEndTimeUs", metrics.maximum_end_to_end_time.count()},
                  {"inputFps", metrics.input_fps},
                  {"processedFps", metrics.processed_fps},
                  {"skippedRatio", metrics.skipped_ratio},
                  {"candidatesCreated", metrics.candidates_created},
                  {"confirmedEvents", metrics.confirmed_events},
                  {"rejectedCandidates", metrics.rejected_candidates}}}};
    result["detector"] = value.detector_info ? detector_info_json(*value.detector_info) : Json{};
    return result;
}

Json event_record_json(const storage::EventMetadataRecord& event)
{
    return {
        {"eventId", event.event_id},
        {"eventSchemaVersion", event.event_schema_version},
        {"eventState", event.event_state},
        {"decisionState", event.decision_state},
        {"persistenceState", event.persistence_state},
        {"reviewState", event.review_state},
        {"reviewDecision", event.review_decision ? Json(*event.review_decision) : Json(nullptr)},
        {"artifactsAvailable", event.artifacts_available},
        {"triggerCount", event.trigger_count},
        {"reviewRevision", event.review_revision},
        {"reviewedAtUtcMs",
         event.reviewed_at_utc_ms ? Json(*event.reviewed_at_utc_ms) : Json(nullptr)},
        {"reviewedBy", event.reviewed_by},
        {"candidateTimeUtcMs", event.candidate_time_utc_ms},
        {"confirmedTimeUtcMs",
         event.confirmed_time_utc_ms ? Json(*event.confirmed_time_utc_ms) : Json(nullptr)},
        {"startTimeUtcMs", event.start_time_utc_ms},
        {"endTimeUtcMs", event.end_time_utc_ms},
        {"cameraIds", event.camera_ids},
        {"triggerCameraId", event.trigger_camera_id},
        {"triggerFrameNumber", event.trigger_frame_number},
        {"triggerReason", event.trigger_reason},
        {"confidence", event.confidence},
        {"uploadState", event.upload_state},
        {"storageState", event.storage_state},
        {"integrityState", event.integrity_state},
        {"integrityCheckedAtUtcMs", event.integrity_checked_at_utc_ms
                                        ? Json(*event.integrity_checked_at_utc_ms)
                                        : Json(nullptr)},
        {"integrityErrorCode", event.integrity_error_code},
        {"retentionLocked", event.retention_locked},
        {"deletionAllowed", event.deletion_allowed},
        {"deletionState", event.deletion_state},
        {"relativeDirectory", path_to_utf8(event.relative_directory)},
        {"thumbnailAvailable", event.artifacts_available}};
}

Result<std::size_t> event_size_field(const Json& payload, const std::string_view key,
                                     const std::size_t default_value, const std::size_t maximum,
                                     const std::string_view operation)
{
    const auto found = payload.find(std::string{key});
    if (found == payload.end())
        return Result<std::size_t>::success(default_value);
    if (!found->is_number_unsigned())
        return Result<std::size_t>::failure(command_error("IPC_REQUEST_INVALID", Severity::error,
                                                          std::string{key} + " 必须是非负整数",
                                                          std::string{operation}));
    const auto value = found->get<std::uint64_t>();
    if (value > maximum)
        return Result<std::size_t>::failure(command_error("IPC_REQUEST_INVALID", Severity::error,
                                                          std::string{key} + " 超出允许范围",
                                                          std::string{operation}));
    return Result<std::size_t>::success(static_cast<std::size_t>(value));
}

Result<storage::EventQuery> event_query(const Json& payload)
{
    if (!has_only_fields(payload, {"startTimeUtcMs", "endTimeUtcMs", "eventState", "decisionState",
                                   "persistenceState", "reviewState", "reviewDecision", "cameraId",
                                   "offset", "limit"}))
        return Result<storage::EventQuery>::failure(command_error(
            "IPC_REQUEST_INVALID", Severity::error, "event.list 包含未知字段", "ipc.event.list"));
    storage::EventQuery query;
    auto offset = event_size_field(payload, "offset", 0U, 10'000'000U, "ipc.event.list");
    auto limit = event_size_field(payload, "limit", storage::database_default_page_size,
                                  storage::database_maximum_page_size, "ipc.event.list");
    if (!offset)
        return Result<storage::EventQuery>::failure(std::move(offset).error());
    if (!limit || limit.value() == 0U)
        return Result<storage::EventQuery>::failure(
            limit ? command_error("IPC_REQUEST_INVALID", Severity::error, "limit 不能为零",
                                  "ipc.event.list")
                  : std::move(limit).error());
    query.offset = offset.value();
    query.limit = limit.value();
    const auto time_field = [&](const std::string_view key,
                                std::optional<std::int64_t>& destination) -> Result<void> {
        const auto found = payload.find(std::string{key});
        if (found == payload.end())
            return Result<void>::success();
        if (!found->is_number_integer() || found->get<std::int64_t>() < 0)
            return Result<void>::failure(command_error("IPC_REQUEST_INVALID", Severity::error,
                                                       std::string{key} + " 必须是非负整数",
                                                       "ipc.event.list"));
        destination = found->get<std::int64_t>();
        return Result<void>::success();
    };
    auto start = time_field("startTimeUtcMs", query.start_time_utc_ms);
    auto end = time_field("endTimeUtcMs", query.end_time_utc_ms);
    if (!start)
        return Result<storage::EventQuery>::failure(std::move(start).error());
    if (!end)
        return Result<storage::EventQuery>::failure(std::move(end).error());
    const auto text_field = [&](const std::string_view key,
                                std::optional<std::string>& destination) -> Result<void> {
        const auto found = payload.find(std::string{key});
        if (found == payload.end())
            return Result<void>::success();
        if (!found->is_string() || found->get_ref<const std::string&>().empty() ||
            found->get_ref<const std::string&>().size() > 128U)
            return Result<void>::failure(command_error("IPC_REQUEST_INVALID", Severity::error,
                                                       std::string{key} + " 无效",
                                                       "ipc.event.list"));
        destination = found->get<std::string>();
        return Result<void>::success();
    };
    auto state = text_field("eventState", query.event_state);
    auto decision_state = text_field("decisionState", query.decision_state);
    auto persistence_state = text_field("persistenceState", query.persistence_state);
    auto review_state = text_field("reviewState", query.review_state);
    auto review_decision = text_field("reviewDecision", query.review_decision);
    auto camera = text_field("cameraId", query.camera_id);
    if (!state)
        return Result<storage::EventQuery>::failure(std::move(state).error());
    if (!decision_state)
        return Result<storage::EventQuery>::failure(std::move(decision_state).error());
    if (!persistence_state)
        return Result<storage::EventQuery>::failure(std::move(persistence_state).error());
    if (!review_state)
        return Result<storage::EventQuery>::failure(std::move(review_state).error());
    if (!review_decision)
        return Result<storage::EventQuery>::failure(std::move(review_decision).error());
    if (!camera)
        return Result<storage::EventQuery>::failure(std::move(camera).error());
    return Result<storage::EventQuery>::success(std::move(query));
}

Result<std::string> required_event_id(const Json& payload, const std::string_view operation,
                                      const bool allow_expected_revision)
{
    if (!has_only_fields(
            payload,
            allow_expected_revision
                ? std::initializer_list<std::string_view>{"eventId", "expectedReviewRevision"}
                : std::initializer_list<std::string_view>{"eventId"}) ||
        !payload.contains("eventId") || !payload["eventId"].is_string() ||
        payload["eventId"].get_ref<const std::string&>().empty() ||
        payload["eventId"].get_ref<const std::string&>().size() > 128U)
        return Result<std::string>::failure(command_error("IPC_REQUEST_INVALID", Severity::error,
                                                          "事件命令需要有效 eventId",
                                                          std::string{operation}));
    return Result<std::string>::success(payload["eventId"].get<std::string>());
}

std::int64_t current_utc_milliseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Json metric_value_json(const monitoring::MetricValue& value)
{
    return std::visit([](const auto& item) { return Json(item); }, value);
}

Json metric_json(const monitoring::MetricPoint& point)
{
    return {{"name", point.name},
            {"value", metric_value_json(point.value)},
            {"unit", point.unit},
            {"available", point.available}};
}

Json details_json(const std::vector<ErrorDetail>& details)
{
    Json result = Json::object();
    for (const auto& detail : details)
    {
        result[detail.key] = detail.value;
    }
    return result;
}

Json alarm_json(const monitoring::AlarmRecord& alarm)
{
    return {{"alarmId", alarm.alarm_id},
            {"revision", alarm.revision},
            {"code", alarm.code},
            {"severity", monitoring::severity_name(alarm.severity)},
            {"source", alarm.source},
            {"firstOccurredAt", alarm.first_occurred_at},
            {"lastOccurredAt", alarm.last_occurred_at},
            {"active", alarm.active},
            {"occurrenceCount", alarm.occurrence_count},
            {"message", alarm.message},
            {"details", details_json(alarm.details)},
            {"acknowledged", alarm.acknowledged}};
}

Json log_json(const logging::RecentLogRecord& record)
{
    return {{"sequence", record.sequence},
            {"timestamp", record.timestamp},
            {"threadId", record.thread_id},
            {"threadName", record.thread_name},
            {"category", logging::category_name(record.category)},
            {"level", logging::level_name(record.level)},
            {"message", record.message}};
}

Result<std::size_t> bounded_limit(const Json& payload, const std::size_t default_value,
                                  const std::size_t maximum, const std::string_view operation)
{
    if (!payload.contains("limit"))
    {
        return Result<std::size_t>::success(default_value);
    }
    const Json& value = payload["limit"];
    if (!(value.is_number_unsigned() || value.is_number_integer()))
    {
        return Result<std::size_t>::failure(command_error(
            "IPC_REQUEST_INVALID", Severity::error, "limit 必须是正整数", std::string{operation}));
    }
    const auto signed_value = value.is_number_unsigned() ? 1 : value.get<std::int64_t>();
    const std::uint64_t parsed = value.is_number_unsigned()
                                     ? value.get<std::uint64_t>()
                                     : static_cast<std::uint64_t>(signed_value);
    if ((!value.is_number_unsigned() && signed_value <= 0) || parsed == 0U || parsed > maximum)
    {
        return Result<std::size_t>::failure(command_error(
            "IPC_REQUEST_INVALID", Severity::error, "limit 超出允许范围", std::string{operation}));
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(parsed));
}

Result<ipc::CommandResponse> metrics_response(const Json& payload,
                                              monitoring::MetricRegistry& registry)
{
    if (!has_only_fields(payload, {"prefixes", "limit"}))
    {
        return Result<ipc::CommandResponse>::failure(
            command_error("IPC_REQUEST_INVALID", Severity::error, "system.getMetrics 包含未知字段",
                          "ipc.system.getMetrics"));
    }
    monitoring::MetricQuery query;
    auto limit = bounded_limit(payload, 256U, 256U, "ipc.system.getMetrics");
    if (!limit)
        return Result<ipc::CommandResponse>::failure(limit.error());
    query.limit = limit.value();
    if (payload.contains("prefixes"))
    {
        const Json& prefixes = payload["prefixes"];
        if (!prefixes.is_array() || prefixes.size() > 16U)
        {
            return Result<ipc::CommandResponse>::failure(
                command_error("IPC_REQUEST_INVALID", Severity::error,
                              "prefixes 必须是最多 16 项的数组", "ipc.system.getMetrics"));
        }
        for (const auto& prefix : prefixes)
        {
            if (!prefix.is_string() || prefix.get_ref<const std::string&>().empty() ||
                prefix.get_ref<const std::string&>().size() > 64U)
            {
                return Result<ipc::CommandResponse>::failure(
                    command_error("IPC_REQUEST_INVALID", Severity::error, "指标前缀无效",
                                  "ipc.system.getMetrics"));
            }
            query.prefixes.push_back(prefix.get<std::string>());
        }
    }
    const auto result = registry.query(query);
    Json metrics = Json::array();
    for (const auto& point : result.snapshot.metrics)
        metrics.push_back(metric_json(point));
    Json response{{"snapshotVersion", result.snapshot.version},
                  {"sampledAt", result.snapshot.sampled_at},
                  {"metrics", std::move(metrics)},
                  {"truncated", result.truncated}};
    return Result<ipc::CommandResponse>::success({.payload_json = response.dump(), .binary = {}});
}

Result<ipc::CommandResponse> alarm_list_response(const Json& payload,
                                                 monitoring::AlarmRegistry& registry)
{
    if (!has_only_fields(payload,
                         {"active", "minimumSeverity", "source", "beforeAlarmId", "limit"}))
    {
        return Result<ipc::CommandResponse>::failure(command_error(
            "IPC_REQUEST_INVALID", Severity::error, "alarm.list 包含未知字段", "ipc.alarm.list"));
    }
    monitoring::AlarmQuery query;
    auto limit = bounded_limit(payload, 100U, 200U, "ipc.alarm.list");
    if (!limit)
        return Result<ipc::CommandResponse>::failure(limit.error());
    query.limit = limit.value();
    if (payload.contains("active"))
    {
        if (!payload["active"].is_boolean())
            return Result<ipc::CommandResponse>::failure(command_error(
                "IPC_REQUEST_INVALID", Severity::error, "active 必须是布尔值", "ipc.alarm.list"));
        query.active = payload["active"].get<bool>();
    }
    if (payload.contains("minimumSeverity"))
    {
        if (!payload["minimumSeverity"].is_string())
            return Result<ipc::CommandResponse>::failure(
                command_error("IPC_REQUEST_INVALID", Severity::error,
                              "minimumSeverity 必须是字符串", "ipc.alarm.list"));
        query.minimum_severity =
            monitoring::parse_severity(payload["minimumSeverity"].get<std::string>());
        if (!query.minimum_severity.has_value())
            return Result<ipc::CommandResponse>::failure(command_error(
                "IPC_REQUEST_INVALID", Severity::error, "minimumSeverity 无效", "ipc.alarm.list"));
    }
    if (payload.contains("source"))
    {
        if (!payload["source"].is_string() ||
            payload["source"].get_ref<const std::string&>().empty() ||
            payload["source"].get_ref<const std::string&>().size() > 128U)
            return Result<ipc::CommandResponse>::failure(command_error(
                "IPC_REQUEST_INVALID", Severity::error, "source 无效", "ipc.alarm.list"));
        query.source = payload["source"].get<std::string>();
    }
    if (payload.contains("beforeAlarmId"))
    {
        if (!payload["beforeAlarmId"].is_number_unsigned())
            return Result<ipc::CommandResponse>::failure(
                command_error("IPC_REQUEST_INVALID", Severity::error,
                              "beforeAlarmId 必须是无符号整数", "ipc.alarm.list"));
        query.before_alarm_id = payload["beforeAlarmId"].get<std::uint64_t>();
    }
    const auto result = registry.query(query);
    Json alarms = Json::array();
    for (const auto& alarm : result.alarms)
        alarms.push_back(alarm_json(alarm));
    Json response{{"registryRevision", result.registry_revision},
                  {"alarms", std::move(alarms)},
                  {"truncated", result.truncated}};
    response["nextBeforeAlarmId"] = result.next_before_alarm_id.has_value()
                                        ? Json(result.next_before_alarm_id.value())
                                        : Json{};
    return Result<ipc::CommandResponse>::success({.payload_json = response.dump(), .binary = {}});
}

Result<ipc::CommandResponse> log_tail_response(const Json& payload,
                                               const logging::LoggingRuntime& runtime)
{
    if (!has_only_fields(payload,
                         {"afterSequence", "categories", "minimumLevel", "threadName", "limit"}))
    {
        return Result<ipc::CommandResponse>::failure(command_error(
            "IPC_REQUEST_INVALID", Severity::error, "log.tail 包含未知字段", "ipc.log.tail"));
    }
    logging::RecentLogQuery query;
    auto limit = bounded_limit(payload, 100U, 200U, "ipc.log.tail");
    if (!limit)
        return Result<ipc::CommandResponse>::failure(limit.error());
    query.limit = limit.value();
    if (payload.contains("afterSequence"))
    {
        if (!payload["afterSequence"].is_number_unsigned())
            return Result<ipc::CommandResponse>::failure(
                command_error("IPC_REQUEST_INVALID", Severity::error,
                              "afterSequence 必须是无符号整数", "ipc.log.tail"));
        query.after_sequence = payload["afterSequence"].get<std::uint64_t>();
    }
    if (payload.contains("categories"))
    {
        const Json& categories = payload["categories"];
        if (!categories.is_array() || categories.size() > 10U)
            return Result<ipc::CommandResponse>::failure(
                command_error("IPC_REQUEST_INVALID", Severity::error,
                              "categories 必须是最多 10 项的数组", "ipc.log.tail"));
        for (const auto& item : categories)
        {
            if (!item.is_string())
                return Result<ipc::CommandResponse>::failure(command_error(
                    "IPC_REQUEST_INVALID", Severity::error, "日志分类无效", "ipc.log.tail"));
            const auto parsed = logging::parse_category(item.get<std::string>());
            if (!parsed.has_value())
                return Result<ipc::CommandResponse>::failure(command_error(
                    "IPC_REQUEST_INVALID", Severity::error, "日志分类无效", "ipc.log.tail"));
            query.categories.push_back(parsed.value());
        }
    }
    if (payload.contains("minimumLevel"))
    {
        if (!payload["minimumLevel"].is_string())
            return Result<ipc::CommandResponse>::failure(
                command_error("IPC_REQUEST_INVALID", Severity::error, "minimumLevel 必须是字符串",
                              "ipc.log.tail"));
        query.minimum_level = logging::parse_level(payload["minimumLevel"].get<std::string>());
        if (!query.minimum_level.has_value())
            return Result<ipc::CommandResponse>::failure(command_error(
                "IPC_REQUEST_INVALID", Severity::error, "minimumLevel 无效", "ipc.log.tail"));
    }
    if (payload.contains("threadName"))
    {
        if (!payload["threadName"].is_string() ||
            !logging::valid_thread_name(payload["threadName"].get_ref<const std::string&>()))
            return Result<ipc::CommandResponse>::failure(command_error(
                "IPC_REQUEST_INVALID", Severity::error, "threadName 无效", "ipc.log.tail"));
        query.thread_name = payload["threadName"].get<std::string>();
    }
    const auto result = runtime.tail(query);
    Json records = Json::array();
    for (const auto& record : result.records)
        records.push_back(log_json(record));
    Json response{{"firstAvailableSequence", result.first_available_sequence},
                  {"latestSequence", result.latest_sequence},
                  {"records", std::move(records)},
                  {"truncated", result.truncated}};
    return Result<ipc::CommandResponse>::success({.payload_json = response.dump(), .binary = {}});
}

bool is_camera_write_command(const std::string_view command)
{
    return command == "camera.connect" || command == "camera.disconnect" ||
           command == "camera.start" || command == "camera.stop" || command == "camera.bind" ||
           command == "camera.updateConfig" || command == "camera.captureSnapshot" ||
           command == "camera.softwareTrigger";
}

bool is_read_only_query_command(const std::string_view command) noexcept
{
    constexpr std::array commands{std::string_view{"uplink.getConfig"},
                                  std::string_view{"storage.getConfig"},
                                  std::string_view{"algorithm.getConfig"},
                                  std::string_view{"event.getConfig"},
                                  std::string_view{"event.list"},
                                  std::string_view{"event.get"},
                                  std::string_view{"event.getSummary"},
                                  std::string_view{"event.getManifest"},
                                  std::string_view{"camera.discover"},
                                  std::string_view{"camera.list"},
                                  std::string_view{"camera.getConfig"},
                                  std::string_view{"system.getStatus"},
                                  std::string_view{"system.getVersion"},
                                  std::string_view{"system.getLocations"},
                                  std::string_view{"system.getMetrics"},
                                  std::string_view{"alarm.list"},
                                  std::string_view{"log.tail"}};
    return std::ranges::find(commands, command) != commands.end();
}

bool topology_restart_required(const config::ConfigSnapshot& snapshot)
{
    return std::find(snapshot.pending_restart_paths.begin(), snapshot.pending_restart_paths.end(),
                     "/cameras") != snapshot.pending_restart_paths.end();
}

Result<std::string> camera_id(const Json& payload, const std::string_view operation)
{
    if (!payload.contains("cameraId") || !payload["cameraId"].is_string())
        return Result<std::string>::failure(command_error("IPC_REQUEST_INVALID", Severity::error,
                                                          "cameraId 必须是字符串",
                                                          std::string{operation}));
    const auto value = payload["cameraId"].get<std::string>();
    if (!is_canonical_camera_id(value))
        return Result<std::string>::failure(command_error("IPC_REQUEST_INVALID", Severity::error,
                                                          "cameraId 必须为 CAM01 至 CAM06",
                                                          std::string{operation}));
    return Result<std::string>::success(value);
}

Result<ipc::CommandResponse> algorithm_configuration_response(config::ConfigRepository& repository,
                                                              EventRuntime& runtime,
                                                              const std::string_view camera_id)
{
    auto configuration = repository.snapshot();
    if (!configuration)
        return Result<ipc::CommandResponse>::failure(configuration.error());
    auto actual = runtime.algorithm_snapshot(camera_id);
    if (!actual)
        return Result<ipc::CommandResponse>::failure(actual.error());
    return Result<ipc::CommandResponse>::success(
        {.payload_json =
             Json{{"algorithm", algorithm_config_json(configuration.value().stored->algorithm)},
                  {"effectiveAlgorithm",
                   algorithm_config_json(configuration.value().effective->algorithm)},
                  {"storedConfigRevision", configuration.value().stored_config_revision},
                  {"effectiveConfigRevision", configuration.value().effective_config_revision},
                  {"runtime", algorithm_runtime_json(actual.value())}}
                 .dump(),
         .binary = {}});
}

template <typename T> Json stepped_range_json(const camera::SteppedRange<T>& value)
{
    return {{"minimum", value.minimum}, {"maximum", value.maximum}, {"increment", value.increment}};
}

std::string_view exposure_auto_mode_name(const camera::ExposureAutoMode value) noexcept
{
    switch (value)
    {
    case camera::ExposureAutoMode::off:
        return "Off";
    case camera::ExposureAutoMode::once:
        return "Once";
    case camera::ExposureAutoMode::continuous:
        return "Continuous";
    }
    return "Off";
}

std::string_view exposure_auto_mode_name(const config::ExposureAutoMode value) noexcept
{
    switch (value)
    {
    case config::ExposureAutoMode::off:
        return "Off";
    case config::ExposureAutoMode::once:
        return "Once";
    case config::ExposureAutoMode::continuous:
        return "Continuous";
    }
    return "Off";
}

Json camera_snapshot_json(const camera::CameraControlSnapshot& value)
{
    Json result{{"cameraId", value.camera_id},
                {"serialNumber", value.serial_number},
                {"state", camera::camera_control_state_name(value.state)}};
    if (value.device)
        result["device"] = {{"model", value.device->model_name},
                            {"serialNumber", value.device->serial_number},
                            {"ip", value.device->ip_address},
                            {"networkInterface", value.device->network_interface}};
    if (value.capabilities && value.capabilities->roi)
    {
        const auto& roi = *value.capabilities->roi;
        result["capabilities"]["roi"] = {{"sensorWidth", roi.sensor_width},
                                         {"sensorHeight", roi.sensor_height},
                                         {"width", stepped_range_json(roi.width)},
                                         {"height", stepped_range_json(roi.height)},
                                         {"offsetX", stepped_range_json(roi.offset_x)},
                                         {"offsetY", stepped_range_json(roi.offset_y)}};
    }
    if (value.capabilities)
    {
        Json modes = Json::array();
        for (const auto mode : value.capabilities->exposure_auto_modes)
            modes.push_back(exposure_auto_mode_name(mode));
        result["capabilities"]["autoExposureModes"] = std::move(modes);
        const auto& line = value.capabilities->line_io;
        Json line_io{{"alarmInputSupported", line.alarm_input_supported},
                     {"risingEdgeSupported", line.line0_rising_edge_supported},
                     {"fallingEdgeSupported", line.line0_falling_edge_supported},
                     {"strobeOutputSupported", line.strobe_output_supported},
                     {"unsupportedReason", line.unsupported_reason}};
        if (line.strobe_duration_us)
            line_io["strobeDurationUs"] = stepped_range_json(*line.strobe_duration_us);
        if (line.strobe_pre_delay_us)
            line_io["strobePreDelayUs"] = stepped_range_json(*line.strobe_pre_delay_us);
        if (line.strobe_post_delay_us)
            line_io["strobePostDelayUs"] = stepped_range_json(*line.strobe_post_delay_us);
        result["capabilities"]["lineIo"] = std::move(line_io);
    }
    if (value.actual)
    {
        const auto& p = *value.actual;
        Json actual;
        if (p.exposure_us)
            actual["exposureUs"] = *p.exposure_us;
        if (p.exposure_auto_mode)
            actual["autoExposure"] = exposure_auto_mode_name(*p.exposure_auto_mode);
        if (p.gain_db)
            actual["gainDb"] = *p.gain_db;
        if (p.frame_rate)
            actual["frameRate"] = *p.frame_rate;
        if (p.roi)
            actual["roi"] = {{"width", p.roi->width},
                             {"height", p.roi->height},
                             {"offsetX", p.roi->offset_x},
                             {"offsetY", p.roi->offset_y}};
        if (p.reverse_x)
            actual["reverseX"] = *p.reverse_x;
        if (p.reverse_y)
            actual["reverseY"] = *p.reverse_y;
        if (p.pixel_format)
        {
            switch (*p.pixel_format)
            {
            case camera::PixelFormat::mono8:
                actual["pixelFormat"] = "Mono8";
                break;
            case camera::PixelFormat::mono10:
                actual["pixelFormat"] = "Mono10";
                break;
            case camera::PixelFormat::mono12:
                actual["pixelFormat"] = "Mono12";
                break;
            case camera::PixelFormat::bayer_rg8:
                actual["pixelFormat"] = "BayerRG8";
                break;
            }
        }
        if (p.trigger_mode)
        {
            switch (*p.trigger_mode)
            {
            case camera::TriggerMode::continuous:
                actual["triggerMode"] = "Continuous";
                break;
            case camera::TriggerMode::hardware:
                actual["triggerMode"] = "Hardware";
                break;
            case camera::TriggerMode::software:
                actual["triggerMode"] = "Software";
                break;
            }
        }
        if (p.trigger_source)
            actual["triggerSource"] = *p.trigger_source;
        if (p.trigger_delay_us)
            actual["triggerDelayUs"] = *p.trigger_delay_us;
        if (p.packet_size_bytes)
            actual["packetSizeBytes"] = *p.packet_size_bytes;
        if (p.inter_packet_delay_ns)
            actual["interPacketDelayNs"] = *p.inter_packet_delay_ns;
        if (p.line_io)
            actual["lineIo"] = {{"alarmInputEnabled", p.line_io->alarm_input_enabled},
                                {"strobeOutputEnabled", p.line_io->strobe_output_enabled},
                                {"strobeDurationUs", p.line_io->strobe_duration_us},
                                {"strobePreDelayUs", p.line_io->strobe_pre_delay_us},
                                {"strobePostDelayUs", p.line_io->strobe_post_delay_us}};
        if (p.line_input)
            result["lineInput"] = {
                {"enabled", p.line_input->enabled},
                {"rawLevel", p.line_input->raw_level},
                {"revision", p.line_input->revision},
                {"timestampUtcMs", p.line_input->timestamp_utc_ms},
                {"stale", value.state == camera::CameraControlState::disconnected}};
        result["actual"] = std::move(actual);
    }
    if (value.last_error)
        result["lastError"] = {{"code", value.last_error->business_code},
                               {"message", value.last_error->message}};
    return result;
}

Json saved_camera_json(const config::CameraConfig& value)
{
    Json result{
        {"exposureUs", value.exposure_us},
        {"autoExposure", exposure_auto_mode_name(value.exposure_auto_mode)},
        {"gainDb", value.gain_db},
        {"frameRate", value.frame_rate},
        {"roi",
         {{"width", value.roi.width},
          {"height", value.roi.height},
          {"offsetX", value.roi.offset_x},
          {"offsetY", value.roi.offset_y}}},
        {"reverseX", value.reverse_x},
        {"reverseY", value.reverse_y},
        {"triggerSource", value.trigger_source},
        {"triggerDelayUs", value.trigger_delay_us},
        {"packetSizeBytes", value.packet_size_bytes},
        {"interPacketDelayNs", value.inter_packet_delay_ns},
        {"lineIo",
         {{"alarmInputEnabled", value.line_io.alarm_input_enabled},
          {"alarmActiveLevel",
           value.line_io.alarm_active_level == config::AlarmActiveLevel::high ? "High" : "Low"},
          {"strobeOutputEnabled", value.line_io.strobe_output_enabled},
          {"strobeDurationUs", value.line_io.strobe_duration_us},
          {"strobePreDelayUs", value.line_io.strobe_pre_delay_us},
          {"strobePostDelayUs", value.line_io.strobe_post_delay_us}}}};
    switch (value.pixel_format)
    {
    case config::PixelFormat::mono8:
        result["pixelFormat"] = "Mono8";
        break;
    case config::PixelFormat::mono10:
        result["pixelFormat"] = "Mono10";
        break;
    case config::PixelFormat::mono12:
        result["pixelFormat"] = "Mono12";
        break;
    case config::PixelFormat::bayer_rg8:
        result["pixelFormat"] = "BayerRG8";
        break;
    }
    switch (value.trigger_mode)
    {
    case config::TriggerMode::continuous:
        result["triggerMode"] = "Continuous";
        break;
    case config::TriggerMode::hardware:
        result["triggerMode"] = "Hardware";
        break;
    case config::TriggerMode::software:
        result["triggerMode"] = "Software";
        break;
    }
    return result;
}

void add_alarm_active(Json& value, const config::CameraConfig& configuration)
{
    if (!value.contains("lineInput") || !value["lineInput"].is_object() ||
        !value["lineInput"].contains("rawLevel") || !value["lineInput"]["rawLevel"].is_boolean())
        return;
    const bool raw = value["lineInput"]["rawLevel"].get<bool>();
    value["lineInput"]["alarmActive"] =
        configuration.line_io.alarm_active_level == config::AlarmActiveLevel::high ? raw : !raw;
}

Result<Json> bound_camera_json(const std::string& id, const std::string& serial,
                               const std::string& location,
                               const camera::CameraControlSnapshot& snapshot)
{
    if (!snapshot.actual)
    {
        return Result<Json>::failure(command_error(
            "CAMERA_PARAMETER_READ_FAILED", Severity::error,
            snapshot.last_error ? snapshot.last_error->message : "无法读取相机当前参数",
            "ipc.camera.bind"));
    }
    const auto& actual = *snapshot.actual;
    if (!actual.exposure_us || !actual.exposure_auto_mode || !actual.gain_db ||
        !actual.frame_rate || !actual.roi || !actual.pixel_format || !actual.trigger_mode ||
        !actual.trigger_delay_us || !actual.packet_size_bytes || !actual.inter_packet_delay_ns)
    {
        return Result<Json>::failure(command_error("CAMERA_PARAMETER_READ_FAILED", Severity::error,
                                                   "相机没有返回创建配置所需的完整参数",
                                                   "ipc.camera.bind"));
    }

    std::string pixel_format;
    switch (*actual.pixel_format)
    {
    case camera::PixelFormat::mono8:
        pixel_format = "Mono8";
        break;
    case camera::PixelFormat::mono10:
        pixel_format = "Mono10";
        break;
    case camera::PixelFormat::mono12:
        pixel_format = "Mono12";
        break;
    case camera::PixelFormat::bayer_rg8:
        pixel_format = "BayerRG8";
        break;
    }

    std::string trigger_mode;
    switch (*actual.trigger_mode)
    {
    case camera::TriggerMode::continuous:
        trigger_mode = "Continuous";
        break;
    case camera::TriggerMode::hardware:
        trigger_mode = "Hardware";
        break;
    case camera::TriggerMode::software:
        trigger_mode = "Software";
        break;
    }

    return Result<Json>::success(
        {{"id", id},
         {"enabled", true},
         {"serialNumber", serial},
         {"location", location},
         {"exposureUs", *actual.exposure_us},
         {"autoExposure", exposure_auto_mode_name(*actual.exposure_auto_mode)},
         {"gainDb", *actual.gain_db},
         {"frameRate", *actual.frame_rate},
         {"roi",
          {{"width", actual.roi->width},
           {"height", actual.roi->height},
           {"offsetX", actual.roi->offset_x},
           {"offsetY", actual.roi->offset_y}}},
         {"reverseX", actual.reverse_x.value_or(false)},
         {"reverseY", actual.reverse_y.value_or(false)},
         {"pixelFormat", std::move(pixel_format)},
         {"triggerMode", std::move(trigger_mode)},
         {"triggerSource", actual.trigger_source.value_or("")},
         {"triggerDelayUs", *actual.trigger_delay_us},
         {"packetSizeBytes", *actual.packet_size_bytes},
         {"interPacketDelayNs", *actual.inter_packet_delay_ns},
         {"lineIo",
          {{"alarmInputEnabled", false},
           {"alarmActiveLevel", "High"},
           {"strobeOutputEnabled", false},
           {"strobeDurationUs", 0U},
           {"strobePreDelayUs", 0U},
           {"strobePostDelayUs", 0U}}}});
}

Result<ipc::CommandResponse> diagnostics_response(
    const Json& payload, config::ConfigRepository& repository,
    const ServiceStatusStore& status_store,
    const std::shared_ptr<monitoring::MetricRegistry>& metrics,
    const std::shared_ptr<monitoring::AlarmRegistry>& alarms,
    const std::shared_ptr<logging::LoggingRuntime>& logging,
    const std::shared_ptr<camera::CameraControlRuntime>& cameras, const std::string_view actor)
{
    if (!payload.empty())
        return Result<ipc::CommandResponse>::failure(command_error(
            "IPC_REQUEST_INVALID", Severity::error, "system.exportDiagnostics payload 必须为空",
            "ipc.system.exportDiagnostics"));
    if (!metrics || !alarms || !logging)
        return Result<ipc::CommandResponse>::failure(
            command_error("SYS_INTERNAL_ERROR", Severity::error, "诊断数据源未完整装配",
                          "ipc.system.exportDiagnostics"));

    auto configuration = repository.snapshot();
    if (!configuration)
        return Result<ipc::CommandResponse>::failure(configuration.error());

    Json redacted_config =
        Json::parse(config::serialize_config(*configuration.value().stored), nullptr, false);
    if (redacted_config.is_discarded())
        return Result<ipc::CommandResponse>::failure(
            command_error("SYS_INTERNAL_ERROR", Severity::error, "无法序列化诊断配置快照",
                          "ipc.system.exportDiagnostics"));
    redact_json(redacted_config);

    const auto metric_result = metrics->query({.limit = 256U});
    Json metric_items = Json::array();
    for (const auto& metric : metric_result.snapshot.metrics)
        metric_items.push_back(metric_json(metric));
    Json metric_document{{"snapshotVersion", metric_result.snapshot.version},
                         {"sampledAt", metric_result.snapshot.sampled_at},
                         {"metrics", std::move(metric_items)},
                         {"truncated", metric_result.truncated}};

    const auto alarm_result = alarms->query({.limit = 200U});
    Json alarm_items = Json::array();
    for (const auto& alarm : alarm_result.alarms)
        alarm_items.push_back(alarm_json(alarm));
    Json alarm_document{{"registryRevision", alarm_result.registry_revision},
                        {"alarms", std::move(alarm_items)},
                        {"truncated", alarm_result.truncated}};

    const auto log_result = logging->tail({.limit = 200U});
    Json log_items = Json::array();
    for (const auto& record : log_result.records)
        log_items.push_back(log_json(record));
    Json log_document{{"firstAvailableSequence", log_result.first_available_sequence},
                      {"latestSequence", log_result.latest_sequence},
                      {"records", std::move(log_items)},
                      {"truncated", log_result.truncated}};

    Json camera_items = Json::array();
    for (const auto& configured : configuration.value().stored->cameras)
    {
        Json item{{"cameraId", configured.id},
                  {"serialNumber", configured.serial_number},
                  {"location", configured.location},
                  {"enabled", configured.enabled}};
        if (cameras)
        {
            auto current = cameras->get(configured.id, configured.serial_number);
            if (current)
                item["runtime"] = camera_snapshot_json(current.value());
            else
                item["runtimeError"] = {{"code", current.error().business_code},
                                        {"message", current.error().message}};
        }
        else
            item["runtime"] = {{"available", false}, {"state", "not-initialized"}};
        camera_items.push_back(std::move(item));
    }

    const auto status = status_store.snapshot();
    Json system_document{
        {"generatedAt", current_utc_timestamp()},
        {"serviceState", service_state_name(status.state)},
        {"acceptingWrites", status.accepting_writes},
        {"startedAt", status.started_at},
        {"machineId", configuration.value().effective->system.machine_id},
        {"configSchemaVersion", configuration.value().stored->config_schema_version},
        {"storedConfigRevision", configuration.value().stored_config_revision},
        {"effectiveConfigRevision", configuration.value().effective_config_revision}};
    Json network_document{
        {"localIpc", "enabled"},
        {"uplink", configuration.value().effective->uplink.enabled ? "configured" : "disabled"},
        {"plantIo", configuration.value().effective->plant_io.enabled ? "configured" : "disabled"}};

    std::vector<ZipEntry> entries{
        {"config-redacted.json", redacted_config.dump(2)},
        {"system.json", system_document.dump(2)},
        {"metrics.json", metric_document.dump(2)},
        {"cameras.json", Json{{"cameras", std::move(camera_items)}}.dump(2)},
        {"network.json", network_document.dump(2)},
        {"alarms.json", alarm_document.dump(2)},
        {"recent-logs.json", log_document.dump(2)},
        {"version.json", version_json().dump(2)}};
    Json entry_names = Json::array();
    for (const auto& entry : entries)
        entry_names.push_back(entry.name);
    entry_names.push_back("manifest.json");
    entries.push_back({"manifest.json", Json{{"formatVersion", 1},
                                             {"generatedAt", current_utc_timestamp()},
                                             {"redacted", true},
                                             {"alarmHistoryScope", "process-memory"},
                                             {"entries", std::move(entry_names)}}
                                            .dump(2)});
    auto archive = make_zip(entries);
    if (!archive)
        return Result<ipc::CommandResponse>::failure(archive.error());

    static_cast<void>(logging->log(logging::Category::audit, logging::Level::info,
                                   "Local diagnostic package exported by actor=" +
                                       logging::redact_sensitive(actor)));
    const std::string generated = current_utc_timestamp();
    std::string compact;
    compact.reserve(generated.size());
    for (const char value : generated)
        if ((value >= '0' && value <= '9') || value == 'T' || value == 'Z')
            compact.push_back(value);
    return Result<ipc::CommandResponse>::success(
        {.payload_json = Json{{"fileName", "PaperBreakEdge-diagnostics-" + compact + ".zip"},
                              {"contentType", "application/zip"},
                              {"size", archive.value().size()},
                              {"redacted", true}}
                             .dump(),
         .binary = std::move(archive).value()});
}

} // namespace

void ServiceStatusStore::set_state(const ServiceState state)
{
    std::function<void(const ServiceStatusSnapshot&)> observer;
    if (state == ServiceState::starting)
    {
        std::scoped_lock lock{mutex_};
        if (started_at_.empty())
        {
            started_at_ = current_utc_timestamp();
        }
    }
    state_.store(state, std::memory_order_release);
    {
        std::scoped_lock lock{mutex_};
        observer = observer_;
    }
    if (observer)
    {
        try
        {
            observer(snapshot());
        }
        catch (...)
        {
        }
    }
}

void ServiceStatusStore::set_observer(std::function<void(const ServiceStatusSnapshot&)> observer)
{
    std::scoped_lock lock{mutex_};
    observer_ = std::move(observer);
}

ServiceStatusSnapshot ServiceStatusStore::snapshot() const
{
    ServiceStatusSnapshot result;
    result.state = state_.load(std::memory_order_acquire);
    result.accepting_writes = result.state == ServiceState::running;
    {
        std::scoped_lock lock{mutex_};
        result.started_at = started_at_;
    }
    return result;
}

SystemCommandService::SystemCommandService(
    config::ConfigRepository& repository, std::shared_ptr<ServiceStatusStore> status,
    std::shared_ptr<monitoring::MetricRegistry> metrics,
    std::shared_ptr<monitoring::AlarmRegistry> alarms,
    std::shared_ptr<logging::LoggingRuntime> logging, std::filesystem::path config_directory,
    std::shared_ptr<pipeline::PreviewRuntime> preview,
    std::shared_ptr<camera::CameraControlRuntime> cameras,
    std::shared_ptr<EventRuntime> event_runtime,
    std::shared_ptr<storage::EventMetadataDatabase> event_database,
    std::shared_ptr<storage::EventInspector> event_inspector,
    std::function<void(const storage::EventMetadataRecord&)> event_review_observer)
    : repository_(repository), status_(std::move(status)), metrics_(std::move(metrics)),
      alarms_(std::move(alarms)), logging_(std::move(logging)),
      config_directory_(std::move(config_directory)), preview_(std::move(preview)),
      cameras_(std::move(cameras)), event_runtime_(std::move(event_runtime)),
      event_database_(std::move(event_database)), event_inspector_(std::move(event_inspector)),
      event_review_observer_(std::move(event_review_observer))
{
}

ipc::IRequestHandler::ExecutionClass SystemCommandService::execution_class(
    const ipc::RequestMessage& request) const noexcept
{
    return is_read_only_query_command(request.command)
               ? ipc::IRequestHandler::ExecutionClass::read_only_query
               : ipc::IRequestHandler::ExecutionClass::serial_control;
}

Result<ipc::CommandResponse> SystemCommandService::handle(const ipc::RequestMessage& request,
                                                          const ipc::PeerIdentity& peer,
                                                          const std::stop_token stop_token)
{
    if (!peer.local || !peer.authenticated)
        return Result<ipc::CommandResponse>::failure(
            command_error("IPC_UNAUTHORIZED", Severity::error, "本机 IPC 功能只允许已认证本机用户",
                          "ipc.dispatch"));
    return handle_with_source(request, peer, stop_token, config::ConfigChangeSource::local_ipc);
}

Result<std::string> SystemCommandService::handle_uplink_command(
    const uplink::RemoteCommand& command, const std::stop_token stop_token)
{
    auto command_id = uplink::validate_identifier(command.command_id, "commandId", 128U);
    auto command_type = uplink::validate_identifier(command.command_type, "commandType", 128U);
    if (!command_id)
        return Result<std::string>::failure(command_id.error());
    if (!command_type)
        return Result<std::string>::failure(command_type.error());

    const bool mutating = command.command_type != "system.requestStatus";
    if (mutating && !command.operator_confirmed)
        return Result<std::string>::failure(
            command_error("UPLINK_COMMAND_NOT_CONFIRMED", Severity::error,
                          "远程变更命令缺少操作员确认", "uplink.command.confirmation"));
    if (mutating && stop_token.stop_requested())
        return Result<std::string>::failure(command_error("SYS_SERVICE_STOPPING", Severity::warning,
                                                          "服务正在停止，拒绝远程变更命令",
                                                          "uplink.command.dispatch", true));
    if (mutating && !logging_)
        return Result<std::string>::failure(command_error("SYS_NOT_SUPPORTED", Severity::error,
                                                          "远程变更命令要求已装配审计日志",
                                                          "uplink.command.audit"));
    if (mutating)
    {
        auto audited = logging_->log(logging::Category::audit, logging::Level::info,
                                     "Uplink command requested type=" + command.command_type +
                                         " commandId=" + command.command_id);
        if (!audited)
            return Result<std::string>::failure(audited.error());
    }

    const auto audit_outcome = [&](const bool success, const std::string_view code) {
        if (!mutating || !logging_)
            return;
        static_cast<void>(logging_->log(
            logging::Category::audit, success ? logging::Level::info : logging::Level::warning,
            "Uplink command completed type=" + command.command_type +
                " commandId=" + command.command_id + " success=" + (success ? "true" : "false") +
                " code=" + std::string{code}));
    };

    Json body = Json::parse(command.body_json, nullptr, false);
    if (body.is_discarded() || !body.is_object() ||
        command.body_json.size() > uplink::maximum_json_message_bytes)
    {
        auto error = command_error("UPLINK_PROTOCOL_ERROR", Severity::error,
                                   "远程命令 body 必须是有界 JSON 对象", "uplink.command.body");
        audit_outcome(false, error.business_code);
        return Result<std::string>::failure(std::move(error));
    }

    if (command.command_type == "service.restart")
    {
        auto error =
            command_error("SYS_NOT_SUPPORTED", Severity::warning, "当前会话未提供远程服务重启能力",
                          "uplink.command.serviceRestart");
        audit_outcome(false, error.business_code);
        return Result<std::string>::failure(std::move(error));
    }
    if (command.command_type == "config.replace")
    {
        if (!has_only_fields(body, {"expectedConfigRevision", "config"}) ||
            !body.contains("expectedConfigRevision") ||
            !body["expectedConfigRevision"].is_number_unsigned() || !body.contains("config") ||
            !body["config"].is_object())
        {
            auto error =
                command_error("IPC_REQUEST_INVALID", Severity::error,
                              "config.replace 需要 expectedConfigRevision 和完整 config 对象",
                              "uplink.command.configReplace");
            audit_outcome(false, error.business_code);
            return Result<std::string>::failure(std::move(error));
        }
        auto updated = repository_.update(body["config"].dump(),
                                          body["expectedConfigRevision"].get<std::uint64_t>(),
                                          {.source = config::ConfigChangeSource::uplink,
                                           .actor = "uplink:" + command.command_id,
                                           .correlation_id = command.command_id});
        if (!updated)
        {
            audit_outcome(false, updated.error().business_code);
            return Result<std::string>::failure(updated.error());
        }
        audit_outcome(true, "OK");
        return Result<std::string>::success(config_summary(updated.value()).dump());
    }

    std::string mapped_command;
    if (command.command_type == "system.requestStatus")
        mapped_command = "system.getStatus";
    else if (command.command_type == "event.retryUpload")
        mapped_command = "event.retryUpload";
    else if (command.command_type == "event.review")
    {
        if (!has_only_fields(body, {"eventId", "expectedReviewRevision", "decision"}) ||
            !body.contains("decision") || !body["decision"].is_string())
        {
            auto error =
                command_error("IPC_REQUEST_INVALID", Severity::error,
                              "event.review 需要 eventId、expectedReviewRevision 和 decision",
                              "uplink.command.eventReview");
            audit_outcome(false, error.business_code);
            return Result<std::string>::failure(std::move(error));
        }
        const std::string decision = body["decision"].get<std::string>();
        if (decision != "confirmed" && decision != "rejected")
        {
            auto error = command_error("IPC_REQUEST_INVALID", Severity::error,
                                       "event.review decision 必须是 confirmed 或 rejected",
                                       "uplink.command.eventReview");
            audit_outcome(false, error.business_code);
            return Result<std::string>::failure(std::move(error));
        }
        mapped_command = decision == "confirmed" ? "event.confirm" : "event.reject";
        body.erase("decision");
    }
    else if (constexpr std::array<std::string_view, 9U> camera_commands{
                 "camera.discover", "camera.bind", "camera.connect", "camera.disconnect",
                 "camera.start", "camera.stop", "camera.updateConfig", "camera.captureSnapshot",
                 "camera.softwareTrigger"};
             std::ranges::find(camera_commands, command.command_type) != camera_commands.end())
        mapped_command = command.command_type;
    else
    {
        auto error = command_error("SYS_NOT_SUPPORTED", Severity::warning,
                                   "不支持的 Uplink 命令类型", "uplink.command.dispatch");
        audit_outcome(false, error.business_code);
        return Result<std::string>::failure(std::move(error));
    }

    ipc::RequestMessage request{.request_id = command.command_id,
                                .command = std::move(mapped_command),
                                .timestamp = current_utc_timestamp(),
                                .payload_json = body.dump(),
                                .binary = {}};
    const ipc::PeerIdentity peer{.actor_sid = "uplink:" + command.command_id,
                                 .connection_id = 0U,
                                 .local = true,
                                 .authenticated = true};
    auto handled =
        handle_with_source(request, peer, stop_token, config::ConfigChangeSource::uplink);
    if (!handled)
    {
        audit_outcome(false, handled.error().business_code);
        return Result<std::string>::failure(handled.error());
    }
    Json response = Json::parse(handled.value().payload_json, nullptr, false);
    if (response.is_discarded() || !response.is_object())
    {
        auto error = command_error("SYS_INTERNAL_ERROR", Severity::error, "服务命令返回了无效 JSON",
                                   "uplink.command.response");
        audit_outcome(false, error.business_code);
        return Result<std::string>::failure(std::move(error));
    }
    if (!handled.value().binary.empty())
    {
        response["binaryOmitted"] = true;
        response["binaryBytes"] = handled.value().binary.size();
    }
    audit_outcome(true, "OK");
    return Result<std::string>::success(response.dump());
}

#if defined(_MSC_VER)
// The dispatcher holds temporaries for mutually exclusive, bounded command branches. Its
// analyzed frame is small relative to the fixed Windows worker-thread stack.
#pragma warning(suppress : 6262)
#endif
Result<ipc::CommandResponse> SystemCommandService::handle_with_source(
    const ipc::RequestMessage& request, const ipc::PeerIdentity& peer,
    const std::stop_token stop_token, const config::ConfigChangeSource config_source)
{
    auto payload = request_payload(request);
    if (!payload)
    {
        return Result<ipc::CommandResponse>::failure(payload.error());
    }

    if (request.command.starts_with("uplink."))
    {
        if (!peer.local || !peer.authenticated)
            return Result<ipc::CommandResponse>::failure(
                command_error("IPC_UNAUTHORIZED", Severity::error,
                              "上位机配置读取只允许已认证本机用户", "ipc.uplink.dispatch"));
        const bool write_command = request.command == "uplink.updateConfig";
        if (write_command && stop_token.stop_requested())
            return Result<ipc::CommandResponse>::failure(
                command_error("SYS_SERVICE_STOPPING", Severity::warning,
                              "服务正在停止，拒绝上位机配置修改", "ipc.uplink.dispatch", true));

        if (request.command == "uplink.getConfig")
        {
            if (!payload.value().empty())
                return Result<ipc::CommandResponse>::failure(
                    command_error("IPC_REQUEST_INVALID", Severity::error,
                                  "uplink.getConfig payload 必须为空", "ipc.uplink.getConfig"));
            auto snapshot = repository_.snapshot();
            if (!snapshot)
                return Result<ipc::CommandResponse>::failure(snapshot.error());
            return Result<ipc::CommandResponse>::success(
                {.payload_json =
                     Json{{"uplink", uplink_config_json(snapshot.value().stored->uplink)},
                          {"effectiveUplink",
                           uplink_config_json(snapshot.value().effective->uplink)},
                          {"storedConfigRevision", snapshot.value().stored_config_revision},
                          {"effectiveConfigRevision", snapshot.value().effective_config_revision},
                          {"pendingRestartPaths", snapshot.value().pending_restart_paths}}
                         .dump(),
                 .binary = {}});
        }
        if (request.command == "uplink.updateConfig")
        {
            if (!has_only_fields(payload.value(), {"expectedConfigRevision", "uplink"}) ||
                !payload.value().contains("expectedConfigRevision") ||
                !payload.value()["expectedConfigRevision"].is_number_unsigned() ||
                !payload.value().contains("uplink") || !payload.value()["uplink"].is_object())
                return Result<ipc::CommandResponse>::failure(command_error(
                    "IPC_REQUEST_INVALID", Severity::error,
                    "uplink.updateConfig 需要 expectedConfigRevision 和完整 uplink 对象",
                    "ipc.uplink.updateConfig"));
            auto current = repository_.snapshot();
            if (!current)
                return Result<ipc::CommandResponse>::failure(current.error());
            Json document = Json::parse(config::serialize_config(*current.value().stored));
            document["uplink"] = payload.value()["uplink"];
            auto updated = repository_.update(
                document.dump(), payload.value()["expectedConfigRevision"].get<std::uint64_t>(),
                {.source = config_source,
                 .actor = peer.actor_sid,
                 .correlation_id = request.request_id});
            if (!updated)
                return Result<ipc::CommandResponse>::failure(updated.error());
            return Result<ipc::CommandResponse>::success(
                {.payload_json =
                     Json{
                         {"uplink", uplink_config_json(updated.value().stored->uplink)},
                         {"effectiveUplink", uplink_config_json(updated.value().effective->uplink)},
                         {"storedConfigRevision", updated.value().stored_config_revision},
                         {"effectiveConfigRevision", updated.value().effective_config_revision},
                         {"applied", updated.value().pending_restart_paths.empty()},
                         {"pendingRestartPaths", updated.value().pending_restart_paths}}
                         .dump(),
                 .binary = {}});
        }
        return Result<ipc::CommandResponse>::failure(command_error(
            "IPC_COMMAND_UNKNOWN", Severity::error, "未知 uplink 命令", "ipc.uplink.dispatch"));
    }

    if (request.command.starts_with("storage."))
    {
        if (!peer.local || !peer.authenticated)
            return Result<ipc::CommandResponse>::failure(
                command_error("IPC_UNAUTHORIZED", Severity::error,
                              "存储配置读取只允许已认证本机用户", "ipc.storage.dispatch"));
        const bool write_command = request.command == "storage.updateConfig";
        if (write_command && stop_token.stop_requested())
            return Result<ipc::CommandResponse>::failure(
                command_error("SYS_SERVICE_STOPPING", Severity::warning,
                              "服务正在停止，拒绝存储配置修改", "ipc.storage.dispatch", true));

        if (request.command == "storage.getConfig")
        {
            if (!payload.value().empty())
                return Result<ipc::CommandResponse>::failure(
                    command_error("IPC_REQUEST_INVALID", Severity::error,
                                  "storage.getConfig payload 必须为空", "ipc.storage.getConfig"));
            auto snapshot = repository_.snapshot();
            if (!snapshot)
                return Result<ipc::CommandResponse>::failure(snapshot.error());
            return Result<ipc::CommandResponse>::success(
                {.payload_json =
                     Json{{"storage", storage_config_json(snapshot.value().stored->storage)},
                          {"effectiveStorage",
                           storage_config_json(snapshot.value().effective->storage)},
                          {"storedConfigRevision", snapshot.value().stored_config_revision},
                          {"effectiveConfigRevision", snapshot.value().effective_config_revision},
                          {"pendingRestartPaths", snapshot.value().pending_restart_paths}}
                         .dump(),
                 .binary = {}});
        }
        if (request.command == "storage.updateConfig")
        {
            if (!has_only_fields(payload.value(), {"expectedConfigRevision", "storage"}) ||
                !payload.value().contains("expectedConfigRevision") ||
                !payload.value()["expectedConfigRevision"].is_number_unsigned() ||
                !payload.value().contains("storage") || !payload.value()["storage"].is_object())
                return Result<ipc::CommandResponse>::failure(command_error(
                    "IPC_REQUEST_INVALID", Severity::error,
                    "storage.updateConfig 需要 expectedConfigRevision 和完整 storage 对象",
                    "ipc.storage.updateConfig"));
            auto current = repository_.snapshot();
            if (!current)
                return Result<ipc::CommandResponse>::failure(current.error());
            Json document = Json::parse(config::serialize_config(*current.value().stored));
            document["storage"] = payload.value()["storage"];
            auto updated = repository_.update(
                document.dump(), payload.value()["expectedConfigRevision"].get<std::uint64_t>(),
                {.source = config_source,
                 .actor = peer.actor_sid,
                 .correlation_id = request.request_id});
            if (!updated)
                return Result<ipc::CommandResponse>::failure(updated.error());
            return Result<ipc::CommandResponse>::success(
                {.payload_json =
                     Json{{"storage", storage_config_json(updated.value().stored->storage)},
                          {"effectiveStorage",
                           storage_config_json(updated.value().effective->storage)},
                          {"storedConfigRevision", updated.value().stored_config_revision},
                          {"effectiveConfigRevision", updated.value().effective_config_revision},
                          {"applied", updated.value().pending_restart_paths.empty()},
                          {"pendingRestartPaths", updated.value().pending_restart_paths}}
                         .dump(),
                 .binary = {}});
        }
        return Result<ipc::CommandResponse>::failure(command_error(
            "IPC_COMMAND_UNKNOWN", Severity::error, "未知 storage 命令", "ipc.storage.dispatch"));
    }

    if (request.command.starts_with("algorithm."))
    {
        const bool write_command = request.command == "algorithm.updateConfig" ||
                                   request.command == "algorithm.testCurrentFrame";
        if (write_command && stop_token.stop_requested())
            return Result<ipc::CommandResponse>::failure(command_error(
                "SYS_SERVICE_STOPPING", Severity::warning, "服务正在停止，拒绝算法配置和调试操作",
                "ipc.algorithm.dispatch", true));
        if (!event_runtime_)
            return Result<ipc::CommandResponse>::failure(
                command_error("SYS_NOT_SUPPORTED", Severity::warning, "算法运行时未装配",
                              "ipc.algorithm.dispatch"));

        if (request.command == "algorithm.getConfig")
        {
            if (!has_only_field(payload.value(), "cameraId"))
                return Result<ipc::CommandResponse>::failure(command_error(
                    "IPC_REQUEST_INVALID", Severity::error, "algorithm.getConfig 只接受 cameraId",
                    "ipc.algorithm.getConfig"));
            auto id = camera_id(payload.value(), "ipc.algorithm.getConfig");
            if (!id)
                return Result<ipc::CommandResponse>::failure(id.error());
            return algorithm_configuration_response(repository_, *event_runtime_, id.value());
        }
        if (request.command == "algorithm.updateConfig")
        {
            if (!has_only_fields(payload.value(),
                                 {"cameraId", "expectedConfigRevision", "algorithm"}) ||
                !payload.value().contains("expectedConfigRevision") ||
                !payload.value()["expectedConfigRevision"].is_number_unsigned() ||
                !payload.value().contains("algorithm") || !payload.value()["algorithm"].is_object())
                return Result<ipc::CommandResponse>::failure(
                    command_error("IPC_REQUEST_INVALID", Severity::error,
                                  "algorithm.updateConfig 需要 cameraId、expectedConfigRevision 和 "
                                  "algorithm 对象",
                                  "ipc.algorithm.updateConfig"));
            auto id = camera_id(payload.value(), "ipc.algorithm.updateConfig");
            if (!id)
                return Result<ipc::CommandResponse>::failure(id.error());
            auto current_runtime = event_runtime_->algorithm_snapshot(id.value());
            if (!current_runtime)
                return Result<ipc::CommandResponse>::failure(current_runtime.error());
            auto current = repository_.snapshot();
            if (!current)
                return Result<ipc::CommandResponse>::failure(current.error());
            Json document = Json::parse(config::serialize_config(*current.value().stored));
            document["algorithm"] = payload.value()["algorithm"];
            auto updated = repository_.update(
                document.dump(), payload.value()["expectedConfigRevision"].get<std::uint64_t>(),
                {.source = config_source,
                 .actor = peer.actor_sid,
                 .correlation_id = request.request_id});
            if (!updated)
                return Result<ipc::CommandResponse>::failure(updated.error());
            return algorithm_configuration_response(repository_, *event_runtime_, id.value());
        }
        if (request.command == "algorithm.testCurrentFrame")
        {
            if (!has_only_field(payload.value(), "cameraId"))
                return Result<ipc::CommandResponse>::failure(
                    command_error("IPC_REQUEST_INVALID", Severity::error,
                                  "algorithm.testCurrentFrame 只接受 cameraId",
                                  "ipc.algorithm.testCurrentFrame"));
            auto id = camera_id(payload.value(), "ipc.algorithm.testCurrentFrame");
            if (!id)
                return Result<ipc::CommandResponse>::failure(id.error());
            auto tested = event_runtime_->test_current_frame(id.value());
            if (!tested)
                return Result<ipc::CommandResponse>::failure(tested.error());
            return Result<ipc::CommandResponse>::success(
                {.payload_json =
                     Json{{"detector", detector_info_json(tested.value().detector_info)},
                          {"result", detection_json(tested.value().detection)},
                          {"isolated", true},
                          {"candidateCreated", false},
                          {"previewFormat", "jpeg"},
                          {"previewSourceWidth", tested.value().source_width},
                          {"previewSourceHeight", tested.value().source_height},
                          {"previewBytes", tested.value().preview_jpeg.size()}}
                         .dump(),
                 .binary = std::move(tested).value().preview_jpeg});
        }
        return Result<ipc::CommandResponse>::failure(
            command_error("IPC_COMMAND_UNKNOWN", Severity::error, "未知 algorithm 命令",
                          "ipc.algorithm.dispatch"));
    }

    if (request.command.starts_with("event."))
    {
        const bool write_command =
            request.command == "event.manualTrigger" || request.command == "event.confirm" ||
            request.command == "event.reject" || request.command == "event.updateConfig" ||
            request.command == "event.export" || request.command == "event.retryUpload";
        if (write_command && stop_token.stop_requested())
            return Result<ipc::CommandResponse>::failure(
                command_error("SYS_SERVICE_STOPPING", Severity::warning,
                              "服务正在停止，拒绝事件写操作", "ipc.event.dispatch", true));
        if (request.command == "event.getConfig")
        {
            if (!payload.value().empty())
                return Result<ipc::CommandResponse>::failure(
                    command_error("IPC_REQUEST_INVALID", Severity::error,
                                  "event.getConfig payload 必须为空", "ipc.event.getConfig"));
            auto snapshot = repository_.snapshot();
            if (!snapshot)
                return Result<ipc::CommandResponse>::failure(snapshot.error());
            return Result<ipc::CommandResponse>::success(
                {.payload_json =
                     Json{{"event", event_config_json(snapshot.value().stored->event)},
                          {"storedConfigRevision", snapshot.value().stored_config_revision},
                          {"effectiveConfigRevision", snapshot.value().effective_config_revision},
                          {"previewVideoGenerationAvailable", false},
                          {"uploadRuntimeAvailable", false}}
                         .dump(),
                 .binary = {}});
        }
        if (request.command == "event.updateConfig")
        {
            if (!has_only_fields(payload.value(), {"expectedConfigRevision", "event"}) ||
                !payload.value().contains("expectedConfigRevision") ||
                !payload.value()["expectedConfigRevision"].is_number_unsigned() ||
                !payload.value().contains("event") || !payload.value()["event"].is_object())
                return Result<ipc::CommandResponse>::failure(
                    command_error("IPC_REQUEST_INVALID", Severity::error,
                                  "event.updateConfig 需要 expectedConfigRevision 和 event 对象",
                                  "ipc.event.updateConfig"));
            auto current = repository_.snapshot();
            if (!current)
                return Result<ipc::CommandResponse>::failure(current.error());
            Json document = Json::parse(config::serialize_config(*current.value().stored));
            document["event"] = payload.value()["event"];
            auto updated = repository_.update(
                document.dump(), payload.value()["expectedConfigRevision"].get<std::uint64_t>(),
                {.source = config_source,
                 .actor = peer.actor_sid,
                 .correlation_id = request.request_id});
            if (!updated)
                return Result<ipc::CommandResponse>::failure(updated.error());
            return Result<ipc::CommandResponse>::success(
                {.payload_json =
                     Json{{"event", event_config_json(updated.value().stored->event)},
                          {"storedConfigRevision", updated.value().stored_config_revision},
                          {"effectiveConfigRevision", updated.value().effective_config_revision},
                          {"applied", updated.value().pending_restart_paths.empty()},
                          {"pendingRestartPaths", updated.value().pending_restart_paths}}
                         .dump(),
                 .binary = {}});
        }
        if (request.command == "event.retryUpload")
        {
            if (!has_only_field(payload.value(), "eventId") ||
                !payload.value()["eventId"].is_string())
                return Result<ipc::CommandResponse>::failure(command_error(
                    "IPC_REQUEST_INVALID", Severity::error,
                    "event.retryUpload 必须且只能包含 eventId", "ipc.event.retryUpload"));
            if (!event_database_)
                return Result<ipc::CommandResponse>::failure(
                    command_error("SYS_NOT_SUPPORTED", Severity::warning, "持久上传仓库尚未装配",
                                  "ipc.event.retryUpload"));
            const auto event_id = payload.value()["eventId"].get<std::string>();
            auto event = event_database_->get_event(event_id);
            if (!event)
                return Result<ipc::CommandResponse>::failure(event.error());
            if (!event.value().artifacts_available)
            {
                if (event.value().integrity_state == "Failed")
                    return Result<ipc::CommandResponse>::failure(command_error(
                        "EVENT_INTEGRITY_FAILED", Severity::critical,
                        "事件证据完整性校验已失败，禁止重试上传", "ipc.event.retryUpload"));
                return Result<ipc::CommandResponse>::failure(
                    command_error("EVENT_NOT_COMMITTED", Severity::warning,
                                  "事件证据尚未提交，不能重试上传", "ipc.event.retryUpload"));
            }
            auto retried = event_database_->retry_event_upload_jobs(
                event_id, std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count());
            if (!retried)
                return Result<ipc::CommandResponse>::failure(retried.error());
            return Result<ipc::CommandResponse>::success(
                {.payload_json =
                     Json{{"eventId", event_id}, {"requeuedJobs", retried.value()}}.dump(),
                 .binary = {}});
        }
        if (request.command == "event.manualTrigger")
        {
            if (!has_only_field(payload.value(), "cameraId") ||
                !payload.value()["cameraId"].is_string() ||
                payload.value()["cameraId"].get_ref<const std::string&>().empty() ||
                payload.value()["cameraId"].get_ref<const std::string&>().size() > 32U)
                return Result<ipc::CommandResponse>::failure(command_error(
                    "IPC_REQUEST_INVALID", Severity::error,
                    "event.manualTrigger 必须且只能包含 cameraId", "ipc.event.manualTrigger"));
            if (!event_runtime_)
                return Result<ipc::CommandResponse>::failure(
                    command_error("SYS_NOT_SUPPORTED", Severity::warning, "事件运行时尚未装配",
                                  "ipc.event.manualTrigger"));
            auto triggered = event_runtime_->request_manual_trigger(
                payload.value()["cameraId"].get_ref<const std::string&>());
            if (!triggered)
                return Result<ipc::CommandResponse>::failure(triggered.error());
            return Result<ipc::CommandResponse>::success(
                {.payload_json =
                     Json{{"accepted", triggered.value()}, {"alreadyPending", !triggered.value()}}
                         .dump(),
                 .binary = {}});
        }
        if (!event_database_)
            return Result<ipc::CommandResponse>::failure(
                command_error("SYS_NOT_SUPPORTED", Severity::warning, "事件数据库尚未装配",
                              "ipc.event.dispatch"));
        if (request.command == "event.list")
        {
            auto query = event_query(payload.value());
            if (!query)
                return Result<ipc::CommandResponse>::failure(query.error());
            auto page = event_database_->query_events(query.value());
            if (!page)
                return Result<ipc::CommandResponse>::failure(page.error());
            Json events = Json::array();
            for (const auto& event : page.value().events)
                events.push_back(event_record_json(event));
            const auto runtime =
                event_runtime_ ? std::optional{event_runtime_->snapshot()} : std::nullopt;
            const auto candidate_decisions =
                runtime ? runtime->candidates_created : page.value().summary.decision_candidates;
            const auto automatic_confirmations =
                runtime ? runtime->confirmed_events : page.value().summary.decision_confirmed;
            return Result<ipc::CommandResponse>::success(
                {.payload_json =
                     Json{{"events", std::move(events)},
                          {"total", page.value().total},
                          {"offset", page.value().offset},
                          {"limit", page.value().limit},
                          {"summary",
                           {{"decisionCandidates", candidate_decisions},
                            {"decisionConfirmed", automatic_confirmations},
                            {"decisionRejected", page.value().summary.decision_rejected},
                            {"decisionTimeout", page.value().summary.decision_timeout},
                            {"persistenceCollecting", page.value().summary.persistence_collecting},
                            {"persistenceEncoding", page.value().summary.persistence_encoding},
                            {"persistenceQueued", page.value().summary.persistence_queued},
                            {"persistenceWriting", page.value().summary.persistence_writing},
                            {"persistenceCommitted", page.value().summary.persistence_committed},
                            {"persistenceIncomplete", page.value().summary.persistence_incomplete},
                            {"reviewUnreviewed", page.value().summary.review_unreviewed},
                            {"reviewConfirmed", page.value().summary.review_confirmed},
                            {"reviewRejected", page.value().summary.review_rejected}}}}
                         .dump(),
                 .binary = {}});
        }
        if (request.command == "event.get")
        {
            auto event_id = required_event_id(payload.value(), "ipc.event.get", false);
            if (!event_id)
                return Result<ipc::CommandResponse>::failure(event_id.error());
            auto record = event_database_->get_event(event_id.value());
            if (!record)
                return Result<ipc::CommandResponse>::failure(record.error());
            if (!record.value().artifacts_available)
            {
                if (record.value().integrity_state == "Failed")
                    return Result<ipc::CommandResponse>::failure(
                        command_error("EVENT_INTEGRITY_FAILED", Severity::critical,
                                      "事件证据完整性校验已失败，详情内容不可用", "ipc.event.get"));
                Json response{{"event", event_record_json(record.value())},
                              {"committedDirectory", nullptr},
                              {"rawFrameCount", 0U},
                              {"keyFrameCount", 0U},
                              {"observedSequenceGaps", 0U},
                              {"keyFramesTraceable", false},
                              {"manifestBytes", 0U},
                              {"thumbnailBytes", 0U}};
                return Result<ipc::CommandResponse>::success(
                    {.payload_json = response.dump(), .binary = {}});
            }
            if (!event_inspector_)
                return Result<ipc::CommandResponse>::failure(command_error(
                    "SYS_NOT_SUPPORTED", Severity::warning, "事件检查器尚未装配", "ipc.event.get"));
            auto inspected = event_inspector_->inspect(record.value().relative_directory);
            if (!inspected)
            {
                const auto failure = inspected.error();
                static_cast<void>(event_database_->mark_event_integrity_failed(
                    event_id.value(), failure.business_code, current_utc_milliseconds()));
                if (alarms_)
                    static_cast<void>(
                        alarms_->raise_alarm({.code = "EVENT_INTEGRITY_FAILED",
                                              .severity = Severity::critical,
                                              .source = event_id.value(),
                                              .message = "事件证据完整性校验失败",
                                              .details = {{"errorCode", failure.business_code}}}));
                return Result<ipc::CommandResponse>::failure(failure);
            }
            auto marked = event_database_->mark_event_integrity_verified(
                event_id.value(), current_utc_milliseconds());
            if (!marked)
                return Result<ipc::CommandResponse>::failure(marked.error());
            record = event_database_->get_event(event_id.value());
            if (!record)
                return Result<ipc::CommandResponse>::failure(record.error());
            Json response{
                {"event", event_record_json(record.value())},
                {"committedDirectory", path_to_utf8(inspected.value().committed_directory)},
                {"rawFrameCount", inspected.value().raw_frames.size()},
                {"keyFrameCount", inspected.value().key_frames.size()},
                {"observedSequenceGaps", inspected.value().observed_sequence_gaps},
                {"keyFramesTraceable", inspected.value().key_frames_traceable},
                {"manifestBytes", inspected.value().manifest_json.size()},
                {"thumbnailBytes", inspected.value().thumbnail_jpeg.size()}};
            return Result<ipc::CommandResponse>::success(
                {.payload_json = response.dump(),
                 .binary = std::move(inspected).value().thumbnail_jpeg});
        }
        if (request.command == "event.getSummary")
        {
            auto event_id = required_event_id(payload.value(), "ipc.event.getSummary", false);
            if (!event_id)
                return Result<ipc::CommandResponse>::failure(event_id.error());
            auto record = event_database_->get_event(event_id.value());
            if (!record)
                return Result<ipc::CommandResponse>::failure(record.error());
            if (!record.value().artifacts_available)
            {
                if (record.value().integrity_state == "Failed")
                    return Result<ipc::CommandResponse>::failure(command_error(
                        "EVENT_INTEGRITY_FAILED", Severity::critical,
                        "事件证据完整性校验已失败，详情内容不可用", "ipc.event.getSummary"));
                Json response{{"event", event_record_json(record.value())},
                              {"committedDirectory", nullptr},
                              {"rawFrameCount", 0U},
                              {"keyFrameCount", 0U},
                              {"observedSequenceGaps", 0U},
                              {"keyFramesTraceable", false},
                              {"manifestBytes", 0U},
                              {"thumbnailBytes", 0U}};
                return Result<ipc::CommandResponse>::success(
                    {.payload_json = response.dump(), .binary = {}});
            }
            if (!event_inspector_)
                return Result<ipc::CommandResponse>::failure(
                    command_error("SYS_NOT_SUPPORTED", Severity::warning, "事件检查器尚未装配",
                                  "ipc.event.getSummary"));
            auto inspected = event_inspector_->inspect_summary(record.value().relative_directory);
            if (!inspected)
            {
                const auto failure = inspected.error();
                static_cast<void>(event_database_->mark_event_integrity_failed(
                    event_id.value(), failure.business_code, current_utc_milliseconds()));
                if (alarms_)
                    static_cast<void>(
                        alarms_->raise_alarm({.code = "EVENT_INTEGRITY_FAILED",
                                              .severity = Severity::critical,
                                              .source = event_id.value(),
                                              .message = "事件证据结构或缩略图校验失败",
                                              .details = {{"errorCode", failure.business_code}}}));
                return Result<ipc::CommandResponse>::failure(failure);
            }
            Json response{
                {"event", event_record_json(record.value())},
                {"committedDirectory", path_to_utf8(inspected.value().committed_directory)},
                {"rawFrameCount", inspected.value().raw_frame_count},
                {"keyFrameCount", inspected.value().key_frame_count},
                {"observedSequenceGaps", inspected.value().observed_sequence_gaps},
                {"keyFramesTraceable", inspected.value().key_frames_traceable},
                {"manifestBytes", inspected.value().manifest_bytes},
                {"thumbnailBytes", inspected.value().thumbnail_jpeg.size()}};
            return Result<ipc::CommandResponse>::success(
                {.payload_json = response.dump(),
                 .binary = std::move(inspected).value().thumbnail_jpeg});
        }
        if (request.command == "event.getManifest")
        {
            auto event_id = required_event_id(payload.value(), "ipc.event.getManifest", false);
            if (!event_id)
                return Result<ipc::CommandResponse>::failure(event_id.error());
            auto record = event_database_->get_event(event_id.value());
            if (!record)
                return Result<ipc::CommandResponse>::failure(record.error());
            if (!record.value().artifacts_available)
            {
                if (record.value().integrity_state == "Failed")
                    return Result<ipc::CommandResponse>::failure(
                        command_error("EVENT_INTEGRITY_FAILED", Severity::critical,
                                      "事件证据完整性校验已失败，manifest 不可用于内容访问",
                                      "ipc.event.getManifest"));
                return Result<ipc::CommandResponse>::failure(command_error(
                    "EVENT_NOT_COMMITTED", Severity::warning,
                    "事件证据尚未提交，manifest 当前不可用", "ipc.event.getManifest"));
            }
            if (!event_inspector_)
                return Result<ipc::CommandResponse>::failure(
                    command_error("SYS_NOT_SUPPORTED", Severity::warning, "事件检查器尚未装配",
                                  "ipc.event.getManifest"));
            auto manifest_text = event_inspector_->get_manifest(record.value().relative_directory);
            if (!manifest_text)
                return Result<ipc::CommandResponse>::failure(manifest_text.error());
            std::vector<std::byte> manifest;
            manifest.reserve(manifest_text.value().size());
            for (const unsigned char byte : manifest_text.value())
                manifest.push_back(static_cast<std::byte>(byte));
            return Result<ipc::CommandResponse>::success(
                {.payload_json = Json{{"eventId", event_id.value()},
                                      {"contentType", "application/json; charset=utf-8"},
                                      {"size", manifest.size()},
                                      {"verified", record.value().integrity_state == "Verified"},
                                      {"integrityState", record.value().integrity_state}}
                                     .dump(),
                 .binary = std::move(manifest)});
        }
        if (request.command == "event.confirm" || request.command == "event.reject")
        {
            auto event_id = required_event_id(payload.value(), "ipc.event.review", true);
            if (!event_id || !payload.value().contains("expectedReviewRevision") ||
                !payload.value()["expectedReviewRevision"].is_number_unsigned() ||
                payload.value()["expectedReviewRevision"].get<std::uint64_t>() == 0U)
                return Result<ipc::CommandResponse>::failure(
                    event_id ? command_error("IPC_REQUEST_INVALID", Severity::error,
                                             "事件复核需要正整数 expectedReviewRevision",
                                             "ipc.event.review")
                             : event_id.error());
            auto reviewed = event_database_->review_event(
                event_id.value(), payload.value()["expectedReviewRevision"].get<std::uint64_t>(),
                request.command == "event.confirm" ? storage::EventReviewDecision::confirmed
                                                   : storage::EventReviewDecision::rejected,
                current_utc_milliseconds(), peer.actor_sid);
            if (!reviewed)
                return Result<ipc::CommandResponse>::failure(reviewed.error());
            if (event_review_observer_)
                event_review_observer_(reviewed.value().event);
            return Result<ipc::CommandResponse>::success(
                {.payload_json = Json{{"event", event_record_json(reviewed.value().event)},
                                      {"duplicate", reviewed.value().duplicate}}
                                     .dump(),
                 .binary = {}});
        }
        if (request.command == "event.export")
        {
            auto event_id = required_event_id(payload.value(), "ipc.event.export", false);
            if (!event_id)
                return Result<ipc::CommandResponse>::failure(event_id.error());
            auto record = event_database_->get_event(event_id.value());
            if (!record)
                return Result<ipc::CommandResponse>::failure(record.error());
            if (!record.value().artifacts_available)
            {
                if (record.value().integrity_state == "Failed")
                    return Result<ipc::CommandResponse>::failure(
                        command_error("EVENT_INTEGRITY_FAILED", Severity::critical,
                                      "事件证据完整性校验已失败，禁止导出", "ipc.event.export"));
                return Result<ipc::CommandResponse>::failure(
                    command_error("EVENT_NOT_COMMITTED", Severity::warning,
                                  "事件证据尚未提交，不能导出", "ipc.event.export"));
            }
            if (!event_inspector_)
                return Result<ipc::CommandResponse>::failure(
                    command_error("SYS_NOT_SUPPORTED", Severity::warning, "事件检查器尚未装配",
                                  "ipc.event.export"));
            auto configuration = repository_.snapshot();
            if (!configuration)
                return Result<ipc::CommandResponse>::failure(configuration.error());
            auto cache_root = path_from_utf8(configuration.value().effective->storage.cache_root);
            if (cache_root.is_relative())
                cache_root = config_directory_ / cache_root;
            std::error_code path_error;
            cache_root = std::filesystem::absolute(cache_root, path_error).lexically_normal();
            if (path_error)
                return Result<ipc::CommandResponse>::failure(
                    command_error("SYS_INTERNAL_ERROR", Severity::error, "无法解析事件导出缓存目录",
                                  "ipc.event.export.path"));
            const auto destination =
                cache_root / ".event-exports" /
                (event_id.value() + "-" + std::to_string(current_utc_milliseconds()) + "-" +
                 std::to_string(std::hash<std::string>{}(request.request_id)) + ".zip");
            auto archive =
                event_inspector_->export_zip_file(record.value().relative_directory, destination);
            if (!archive)
            {
                const auto failure = archive.error();
                static_cast<void>(event_database_->mark_event_integrity_failed(
                    event_id.value(), failure.business_code, current_utc_milliseconds()));
                if (alarms_)
                    static_cast<void>(
                        alarms_->raise_alarm({.code = "EVENT_INTEGRITY_FAILED",
                                              .severity = Severity::critical,
                                              .source = event_id.value(),
                                              .message = "事件证据完整性校验失败，导出已取消",
                                              .details = {{"errorCode", failure.business_code}}}));
                return Result<ipc::CommandResponse>::failure(failure);
            }
            auto marked = event_database_->mark_event_integrity_verified(
                event_id.value(), current_utc_milliseconds());
            if (!marked)
                return Result<ipc::CommandResponse>::failure(marked.error());
            return Result<ipc::CommandResponse>::success(
                {.payload_json = Json{{"eventId", archive.value().event_id},
                                      {"fileName", archive.value().file_name},
                                      {"contentType", "application/zip"},
                                      {"size", archive.value().size_bytes},
                                      {"sourceFileCount", archive.value().source_file_count},
                                      {"exportSourcePath", path_to_utf8(archive.value().path)},
                                      {"verified", true}}
                                     .dump(),
                 .binary = {}});
        }
        return Result<ipc::CommandResponse>::failure(command_error(
            "IPC_REQUEST_INVALID", Severity::error, "未知事件 IPC 命令", "ipc.event.dispatch"));
    }

    if (request.command.starts_with("camera."))
    {
        if (!cameras_)
            return Result<ipc::CommandResponse>::failure(
                camera_provider_required("ipc.camera.dispatch"));
        if (!peer.local || !peer.authenticated)
            return Result<ipc::CommandResponse>::failure(
                command_error("IPC_UNAUTHORIZED", Severity::error, "相机操作只允许已认证本机用户",
                              "ipc.camera.dispatch"));
        if (is_camera_write_command(request.command) && stop_token.stop_requested())
            return Result<ipc::CommandResponse>::failure(
                command_error("SYS_SERVICE_STOPPING", Severity::warning,
                              "服务正在停止，拒绝相机控制操作", "ipc.camera.dispatch", true));
        if (request.command == "camera.discover")
        {
            if (!payload.value().empty())
                return Result<ipc::CommandResponse>::failure(
                    command_error("IPC_REQUEST_INVALID", Severity::error,
                                  "camera.discover payload 必须为空", "ipc.camera.discover"));
            auto discovered = cameras_->discover();
            if (!discovered)
            {
                if (discovered.error().business_code == "SYS_NOT_SUPPORTED")
                    return Result<ipc::CommandResponse>::failure(
                        camera_provider_required("ipc.camera.discover"));
                return Result<ipc::CommandResponse>::failure(discovered.error());
            }
            Json devices = Json::array();
            for (const auto& d : discovered.value())
                devices.push_back({{"model", d.model_name},
                                   {"serialNumber", d.serial_number},
                                   {"ip", d.ip_address},
                                   {"networkInterface", d.network_interface},
                                   {"exclusiveAccessAvailable", d.exclusive_access_available}});
            return Result<ipc::CommandResponse>::success(
                {.payload_json = Json{{"devices", std::move(devices)}}.dump(), .binary = {}});
        }
        const bool no_payload = request.command == "camera.list";
        if (no_payload)
        {
            if (!payload.value().empty())
                return Result<ipc::CommandResponse>::failure(
                    command_error("IPC_REQUEST_INVALID", Severity::error,
                                  "camera.list payload 必须为空", "ipc.camera.list"));
            auto config = repository_.snapshot();
            if (!config)
                return Result<ipc::CommandResponse>::failure(config.error());
            Json list = Json::array();
            for (const auto& item : config.value().stored->cameras)
            {
                auto current = cameras_->get(item.id, item.serial_number);
                if (!current)
                    return Result<ipc::CommandResponse>::failure(current.error());
                auto json = camera_snapshot_json(current.value());
                add_alarm_active(json, item);
                json["enabled"] = item.enabled;
                json["location"] = item.location;
                json["savedConfigRevision"] = config.value().stored_config_revision;
                json["saved"] = saved_camera_json(item);
                list.push_back(std::move(json));
            }
            return Result<ipc::CommandResponse>::success(
                {.payload_json =
                     Json{{"cameras", std::move(list)},
                          {"storedConfigRevision", config.value().stored_config_revision},
                          {"topologyRestartRequired", topology_restart_required(config.value())}}
                         .dump(),
                 .binary = {}});
        }
        auto id = camera_id(payload.value(), "ipc.camera");
        if (!id)
            return Result<ipc::CommandResponse>::failure(id.error());
        if (request.command == "camera.bind")
        {
            if (!has_only_fields(payload.value(), {"cameraId", "serialNumber", "location",
                                                   "expectedConfigRevision"}) ||
                !payload.value().contains("serialNumber") ||
                !payload.value()["serialNumber"].is_string() ||
                !payload.value().contains("location") || !payload.value()["location"].is_string() ||
                !payload.value().contains("expectedConfigRevision") ||
                !payload.value()["expectedConfigRevision"].is_number_unsigned())
            {
                return Result<ipc::CommandResponse>::failure(command_error(
                    "IPC_REQUEST_INVALID", Severity::error,
                    "camera.bind 需要 cameraId、serialNumber、location 和 expectedConfigRevision",
                    "ipc.camera.bind"));
            }
            const std::string serial = payload.value()["serialNumber"].get<std::string>();
            const std::string location = payload.value()["location"].get<std::string>();
            if (!is_canonical_camera_id(id.value()) || serial.empty() || serial.size() > 128U ||
                location.empty() || location.size() > 128U ||
                location.find_first_not_of(" \t\r\n") == std::string::npos)
            {
                return Result<ipc::CommandResponse>::failure(
                    command_error("IPC_REQUEST_INVALID", Severity::error,
                                  "逻辑槽位、相机序列号或安装位置无效", "ipc.camera.bind"));
            }

            auto config = repository_.snapshot();
            if (!config)
                return Result<ipc::CommandResponse>::failure(config.error());
            if (payload.value()["expectedConfigRevision"].get<std::uint64_t>() !=
                config.value().stored_config_revision)
            {
                Error error = command_error("SYS_CONFIG_VERSION_CONFLICT", Severity::warning,
                                            "配置修订与当前版本冲突", "ipc.camera.bind");
                error.details.push_back({"currentConfigRevision",
                                         std::to_string(config.value().stored_config_revision)});
                return Result<ipc::CommandResponse>::failure(std::move(error));
            }
            if (config.value().stored->cameras.size() >= camera_slot_count ||
                std::ranges::any_of(config.value().stored->cameras,
                                    [&](const auto& item) { return item.id == id.value(); }))
            {
                return Result<ipc::CommandResponse>::failure(
                    command_error("CAMERA_CONFIG_FAILED", Severity::error,
                                  "逻辑相机槽位已被占用或已达到六路上限", "ipc.camera.bind"));
            }
            if (std::ranges::any_of(config.value().stored->cameras,
                                    [&](const auto& item) { return item.serial_number == serial; }))
            {
                return Result<ipc::CommandResponse>::failure(
                    command_error("CAMERA_CONFIG_FAILED", Severity::error,
                                  "该序列号已经绑定到其他逻辑相机", "ipc.camera.bind"));
            }

            auto discovered = cameras_->discover();
            if (!discovered)
            {
                if (discovered.error().business_code == "SYS_NOT_SUPPORTED")
                    return Result<ipc::CommandResponse>::failure(
                        camera_provider_required("ipc.camera.bind"));
                return Result<ipc::CommandResponse>::failure(discovered.error());
            }
            auto device = camera::find_device_by_serial(discovered.value(), serial);
            if (!device)
                return Result<ipc::CommandResponse>::failure(device.error());
            if (device.value().model_name != "MV-CS020-60GM")
            {
                return Result<ipc::CommandResponse>::failure(
                    command_error("CAMERA_CONFIG_FAILED", Severity::error,
                                  "仅允许绑定已批准型号 MV-CS020-60GM", "ipc.camera.bind"));
            }
            if (!device.value().exclusive_access_available)
            {
                return Result<ipc::CommandResponse>::failure(command_error(
                    "CAMERA_ACCESS_DENIED", Severity::error,
                    "相机正被其他程序占用，无法读取参数并完成绑定", "ipc.camera.bind", true));
            }

            auto connected = cameras_->connect(id.value(), serial);
            if (!connected)
                return Result<ipc::CommandResponse>::failure(connected.error());
            auto candidate = bound_camera_json(id.value(), serial, location, connected.value());
            auto disconnected = cameras_->disconnect(id.value());
            if (!disconnected)
                return Result<ipc::CommandResponse>::failure(disconnected.error());
            if (!candidate)
                return Result<ipc::CommandResponse>::failure(candidate.error());

            Json document = Json::parse(config::serialize_config(*config.value().stored));
            document["cameras"].push_back(std::move(candidate).value());
            std::sort(document["cameras"].begin(), document["cameras"].end(),
                      [](const Json& left, const Json& right) {
                          return left.at("id").get<std::string>() <
                                 right.at("id").get<std::string>();
                      });
            auto saved = repository_.update(
                document.dump(), payload.value()["expectedConfigRevision"].get<std::uint64_t>(),
                {.source = config_source,
                 .actor = peer.actor_sid,
                 .correlation_id = request.request_id});
            if (!saved)
                return Result<ipc::CommandResponse>::failure(saved.error());
            return Result<ipc::CommandResponse>::success(
                {.payload_json =
                     Json{{"saved", true},
                          {"dispatched", false},
                          {"applied", false},
                          {"restartRequired", true},
                          {"storedConfigRevision", saved.value().stored_config_revision}}
                         .dump(),
                 .binary = {}});
        }
        if (request.command != "camera.updateConfig" &&
            !has_only_field(payload.value(), "cameraId"))
            return Result<ipc::CommandResponse>::failure(command_error(
                "IPC_REQUEST_INVALID", Severity::error, "相机命令包含未知字段", "ipc.camera"));
        if (request.command == "camera.updateConfig" &&
            !has_only_fields(payload.value(), {"cameraId", "expectedConfigRevision", "parameters"}))
            return Result<ipc::CommandResponse>::failure(
                command_error("IPC_REQUEST_INVALID", Severity::error,
                              "camera.updateConfig 包含未知字段", "ipc.camera.updateConfig"));
        auto config = repository_.snapshot();
        if (!config)
            return Result<ipc::CommandResponse>::failure(config.error());
        const auto found = std::find_if(config.value().stored->cameras.begin(),
                                        config.value().stored->cameras.end(),
                                        [&](const auto& c) { return c.id == id.value(); });
        if (found == config.value().stored->cameras.end())
            return Result<ipc::CommandResponse>::failure(
                command_error("CAMERA_NOT_FOUND", Severity::error, "逻辑相机未配置", "ipc.camera"));
        Result<camera::CameraControlSnapshot> result =
            Result<camera::CameraControlSnapshot>::failure(command_error(
                "IPC_REQUEST_INVALID", Severity::error, "未知相机命令", "ipc.camera"));
        if (request.command == "camera.connect")
        {
            auto connected = cameras_->connect(id.value(), found->serial_number);
            if (!connected)
                return Result<ipc::CommandResponse>::failure(connected.error());

            auto applied = cameras_->update(id.value(), configured_camera_parameters(*found));
            if (!applied)
            {
                if (applied.error().business_code != "CAMERA_CONFIG_FAILED")
                    return Result<ipc::CommandResponse>::failure(applied.error());
                Json response = camera_snapshot_json(connected.value());
                response["saved"] = false;
                response["dispatched"] = false;
                response["applied"] = false;
                response["restartRequired"] = false;
                response["applyError"] = {{"code", applied.error().business_code},
                                          {"message", applied.error().message}};
                return Result<ipc::CommandResponse>::success(
                    {.payload_json = response.dump(), .binary = {}});
            }
            result = std::move(applied);
        }
        else if (request.command == "camera.disconnect")
            result = cameras_->disconnect(id.value());
        else if (request.command == "camera.start")
            result = cameras_->start(id.value());
        else if (request.command == "camera.stop")
            result = cameras_->stop(id.value());
        else if (request.command == "camera.getConfig")
            result = cameras_->get(id.value(), found->serial_number);
        else if (request.command == "camera.updateConfig")
        {
            if (!payload.value().contains("expectedConfigRevision") ||
                !payload.value()["expectedConfigRevision"].is_number_unsigned() ||
                !payload.value().contains("parameters") ||
                !payload.value()["parameters"].is_object())
                return Result<ipc::CommandResponse>::failure(command_error(
                    "IPC_REQUEST_INVALID", Severity::error,
                    "camera.updateConfig 需要 expectedConfigRevision 和 parameters 对象",
                    "ipc.camera.updateConfig"));
            const Json& parameters = payload.value()["parameters"];
            static constexpr std::array<std::string_view, 14U> allowed{"exposureUs",
                                                                       "autoExposure",
                                                                       "gainDb",
                                                                       "frameRate",
                                                                       "roi",
                                                                       "pixelFormat",
                                                                       "triggerMode",
                                                                       "triggerSource",
                                                                       "triggerDelayUs",
                                                                       "packetSizeBytes",
                                                                       "interPacketDelayNs",
                                                                       "reverseX",
                                                                       "reverseY",
                                                                       "lineIo"};
            for (auto it = parameters.begin(); it != parameters.end(); ++it)
                if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end())
                    return Result<ipc::CommandResponse>::failure(command_error(
                        "IPC_REQUEST_INVALID", Severity::error, "camera.updateConfig 包含未知参数",
                        "ipc.camera.updateConfig"));
            Json document = Json::parse(config::serialize_config(*config.value().stored));
            Json* target = nullptr;
            for (auto& candidate : document["cameras"])
                if (candidate["id"] == id.value())
                {
                    target = &candidate;
                    break;
                }
            if (!target)
                return Result<ipc::CommandResponse>::failure(
                    command_error("CAMERA_NOT_FOUND", Severity::error, "逻辑相机未配置",
                                  "ipc.camera.updateConfig"));
            for (auto it = parameters.begin(); it != parameters.end(); ++it)
                (*target)[it.key()] = it.value();

            auto candidate = config::parse_config(document.dump(), config_directory_);
            if (!candidate)
                return Result<ipc::CommandResponse>::failure(candidate.error());
            const auto candidate_camera =
                std::find_if(candidate.value().cameras.begin(), candidate.value().cameras.end(),
                             [&](const auto& item) { return item.id == id.value(); });
            if (candidate_camera == candidate.value().cameras.end())
                return Result<ipc::CommandResponse>::failure(
                    command_error("CAMERA_NOT_FOUND", Severity::error, "候选配置中逻辑相机不存在",
                                  "ipc.camera.updateConfig"));

            auto current = cameras_->get(id.value(), found->serial_number);
            if (!current)
                return Result<ipc::CommandResponse>::failure(current.error());
            if (current.value().capabilities)
            {
                auto validated = camera::validate_parameters(
                    *current.value().capabilities, configured_camera_parameters(*candidate_camera));
                if (!validated)
                    return Result<ipc::CommandResponse>::failure(validated.error());
            }

            auto saved = repository_.update(
                document.dump(), payload.value()["expectedConfigRevision"].get<std::uint64_t>(),
                {.source = config_source,
                 .actor = peer.actor_sid,
                 .correlation_id = request.request_id});
            if (!saved)
                return Result<ipc::CommandResponse>::failure(saved.error());
            const auto updated = std::find_if(
                saved.value().stored->cameras.begin(), saved.value().stored->cameras.end(),
                [&](const auto& item) { return item.id == id.value(); });
            if (updated == saved.value().stored->cameras.end())
                return Result<ipc::CommandResponse>::failure(
                    command_error("CAMERA_NOT_FOUND", Severity::error, "保存后逻辑相机不存在",
                                  "ipc.camera.updateConfig"));
            auto applied = cameras_->update(id.value(), configured_camera_parameters(*updated));
            if (applied)
                result = std::move(applied);
            else
            {
                Json response{{"saved", true},
                              {"dispatched", false},
                              {"applied", false},
                              {"restartRequired", !saved.value().pending_restart_paths.empty()},
                              {"storedConfigRevision", saved.value().stored_config_revision},
                              {"applyError",
                               {{"code", applied.error().business_code},
                                {"message", applied.error().message}}}};
                return Result<ipc::CommandResponse>::success(
                    {.payload_json = response.dump(), .binary = {}});
            }
            if (!result)
                return Result<ipc::CommandResponse>::failure(result.error());
            Json response = camera_snapshot_json(result.value());
            add_alarm_active(response, *updated);
            response["saved"] = true;
            response["dispatched"] = true;
            response["applied"] = true;
            response["restartRequired"] = !saved.value().pending_restart_paths.empty();
            response["storedConfigRevision"] = saved.value().stored_config_revision;
            return Result<ipc::CommandResponse>::success(
                {.payload_json = response.dump(), .binary = {}});
        }
        else if (request.command == "camera.softwareTrigger")
        {
            auto triggered = cameras_->software_trigger(id.value());
            if (!triggered)
                return Result<ipc::CommandResponse>::failure(triggered.error());
            return Result<ipc::CommandResponse>::success(
                {.payload_json = R"({"triggered":true})", .binary = {}});
        }
        else if (request.command == "camera.captureSnapshot")
        {
            auto frame = cameras_->capture_snapshot(id.value());
            if (!frame)
                return Result<ipc::CommandResponse>::failure(frame.error());
            return Result<ipc::CommandResponse>::success(
                {.payload_json = Json{{"cameraFrameNumber", frame.value().camera_frame_number},
                                      {"width", frame.value().geometry.width},
                                      {"height", frame.value().geometry.height}}
                                     .dump(),
                 .binary = {}});
        }
        if (!result)
            return Result<ipc::CommandResponse>::failure(result.error());
        Json response = camera_snapshot_json(result.value());
        add_alarm_active(response, *found);
        return Result<ipc::CommandResponse>::success(
            {.payload_json = response.dump(), .binary = {}});
    }

    if (request.command == "system.getStatus")
    {
        if (!payload.value().empty())
        {
            return Result<ipc::CommandResponse>::failure(
                command_error("IPC_REQUEST_INVALID", Severity::error,
                              "system.getStatus payload 必须为空", "ipc.system.getStatus"));
        }
        return status_response(repository_, *status_, logging_.get());
    }
    if (request.command == "system.getVersion")
    {
        if (!payload.value().empty())
        {
            return Result<ipc::CommandResponse>::failure(
                command_error("IPC_REQUEST_INVALID", Severity::error,
                              "system.getVersion payload 必须为空", "ipc.system.getVersion"));
        }
        return version_response();
    }
    if (request.command == "system.getLocations")
    {
        if (!payload.value().empty())
            return Result<ipc::CommandResponse>::failure(
                command_error("IPC_REQUEST_INVALID", Severity::error,
                              "system.getLocations payload 必须为空", "ipc.system.getLocations"));
        return locations_response(repository_, config_directory_);
    }
    if (request.command == "system.exportDiagnostics")
    {
        if (!peer.local || !peer.authenticated)
            return Result<ipc::CommandResponse>::failure(command_error(
                "IPC_UNAUTHORIZED", Severity::error,
                "system.exportDiagnostics 只允许已认证本机用户", "ipc.system.exportDiagnostics"));
        return diagnostics_response(payload.value(), repository_, *status_, metrics_, alarms_,
                                    logging_, cameras_, peer.actor_sid);
    }
    if (request.command == "system.getMetrics")
    {
        if (!metrics_)
            return Result<ipc::CommandResponse>::failure(command_error(
                "SYS_INTERNAL_ERROR", Severity::error, "指标服务未装配", "ipc.system.getMetrics"));
        return metrics_response(payload.value(), *metrics_);
    }
    if (request.command == "alarm.list")
    {
        if (!alarms_)
            return Result<ipc::CommandResponse>::failure(command_error(
                "SYS_INTERNAL_ERROR", Severity::error, "报警服务未装配", "ipc.alarm.list"));
        return alarm_list_response(payload.value(), *alarms_);
    }
    if (request.command == "log.tail")
    {
        if (!logging_)
            return Result<ipc::CommandResponse>::failure(command_error(
                "SYS_INTERNAL_ERROR", Severity::error, "日志服务未装配", "ipc.log.tail"));
        return log_tail_response(payload.value(), *logging_);
    }
    if (request.command == "preview.subscribe")
    {
        if (!preview_)
            return Result<ipc::CommandResponse>::failure(
                command_error("SYS_NOT_SUPPORTED", Severity::warning, "预览运行时尚未装配",
                              "ipc.preview.subscribe"));
        if (!has_only_fields(payload.value(), {"cameraIds", "fps"}) ||
            !payload.value().contains("cameraIds") || !payload.value()["cameraIds"].is_array() ||
            payload.value()["cameraIds"].empty() ||
            payload.value()["cameraIds"].size() > camera_slot_count)
            return Result<ipc::CommandResponse>::failure(command_error(
                "IPC_REQUEST_INVALID", Severity::error,
                "preview.subscribe 需要 1 至 6 个 cameraIds", "ipc.preview.subscribe"));
        std::vector<std::string> camera_ids;
        camera_ids.reserve(payload.value()["cameraIds"].size());
        for (const auto& value : payload.value()["cameraIds"])
        {
            if (!value.is_string() || !is_canonical_camera_id(value.get_ref<const std::string&>()))
                return Result<ipc::CommandResponse>::failure(
                    command_error("IPC_REQUEST_INVALID", Severity::error,
                                  "cameraIds 包含无效相机编号", "ipc.preview.subscribe"));
            camera_ids.push_back(value.get<std::string>());
        }
        std::optional<double> frames_per_second;
        if (payload.value().contains("fps"))
        {
            if (!payload.value()["fps"].is_number())
                return Result<ipc::CommandResponse>::failure(
                    command_error("IPC_REQUEST_INVALID", Severity::error, "fps 必须为数值",
                                  "ipc.preview.subscribe"));
            frames_per_second = payload.value()["fps"].get<double>();
            if (!std::isfinite(frames_per_second.value()) ||
                frames_per_second.value() < pipeline::preview_minimum_frames_per_second ||
                frames_per_second.value() > pipeline::preview_maximum_frames_per_second)
                return Result<ipc::CommandResponse>::failure(
                    command_error("IPC_REQUEST_INVALID", Severity::error,
                                  "fps 必须在 0.1 至 30 之间", "ipc.preview.subscribe"));
        }
        auto subscribed = preview_->subscribe(peer.connection_id, camera_ids, frames_per_second);
        if (!subscribed)
            return Result<ipc::CommandResponse>::failure(subscribed.error());
        return Result<ipc::CommandResponse>::success(
            {.payload_json =
                 Json{{"subscribed", true}, {"cameraIds", camera_ids}, {"fps", subscribed.value()}}
                     .dump(),
             .binary = {}});
    }
    if (request.command == "preview.unsubscribe")
    {
        if (!preview_)
            return Result<ipc::CommandResponse>::failure(
                command_error("SYS_NOT_SUPPORTED", Severity::warning, "预览运行时尚未装配",
                              "ipc.preview.unsubscribe"));
        if (!payload.value().empty())
            return Result<ipc::CommandResponse>::failure(
                command_error("IPC_REQUEST_INVALID", Severity::error,
                              "preview.unsubscribe payload 必须为空", "ipc.preview.unsubscribe"));
        preview_->unsubscribe(peer.connection_id);
        return Result<ipc::CommandResponse>::success(
            {.payload_json = R"({"subscribed":false})", .binary = {}});
    }
    if (request.command == "alarm.acknowledge")
    {
        if (stop_token.stop_requested())
            return Result<ipc::CommandResponse>::failure(
                command_error("SYS_SERVICE_STOPPING", Severity::warning,
                              "服务正在停止，拒绝报警确认", "ipc.alarm.acknowledge", true));
        if (!has_only_field(payload.value(), "alarmId") ||
            !payload.value()["alarmId"].is_number_unsigned() ||
            payload.value()["alarmId"].get<std::uint64_t>() == 0U)
            return Result<ipc::CommandResponse>::failure(command_error(
                "IPC_REQUEST_INVALID", Severity::error,
                "alarm.acknowledge 必须且只能包含正整数 alarmId", "ipc.alarm.acknowledge"));
        if (!alarms_)
            return Result<ipc::CommandResponse>::failure(command_error(
                "SYS_INTERNAL_ERROR", Severity::error, "报警服务未装配", "ipc.alarm.acknowledge"));
        auto acknowledged = alarms_->acknowledge(payload.value()["alarmId"].get<std::uint64_t>());
        if (!acknowledged)
            return Result<ipc::CommandResponse>::failure(acknowledged.error());
        return Result<ipc::CommandResponse>::success(
            {.payload_json = alarm_json(acknowledged.value()).dump(), .binary = {}});
    }
    if (request.command != "system.reloadConfig")
    {
        return Result<ipc::CommandResponse>::failure(command_error(
            "IPC_REQUEST_INVALID", Severity::error, "未知 IPC 命令", "ipc.system.dispatch"));
    }
    if (stop_token.stop_requested())
    {
        return Result<ipc::CommandResponse>::failure(
            command_error("SYS_SERVICE_STOPPING", Severity::warning, "服务正在停止，拒绝配置重载",
                          "ipc.system.reloadConfig", true));
    }
    if (!has_only_field(payload.value(), "expectedConfigRevision") ||
        !(payload.value()["expectedConfigRevision"].is_number_unsigned() ||
          payload.value()["expectedConfigRevision"].is_number_integer()))
    {
        return Result<ipc::CommandResponse>::failure(
            command_error("IPC_REQUEST_INVALID", Severity::error,
                          "system.reloadConfig 必须且只能包含整数 expectedConfigRevision",
                          "ipc.system.reloadConfig"));
    }
    const Json& revision_value = payload.value()["expectedConfigRevision"];
    const bool unsigned_revision = revision_value.is_number_unsigned();
    const std::int64_t signed_revision = unsigned_revision ? 0 : revision_value.get<std::int64_t>();
    if (!unsigned_revision && signed_revision < 0)
    {
        return Result<ipc::CommandResponse>::failure(
            command_error("IPC_REQUEST_INVALID", Severity::error,
                          "expectedConfigRevision 不能为负数", "ipc.system.reloadConfig"));
    }

    config::ConfigChangeContext context;
    context.source = config_source;
    context.actor = peer.actor_sid;
    context.correlation_id = request.request_id;
    const std::uint64_t revision = unsigned_revision ? revision_value.get<std::uint64_t>()
                                                     : static_cast<std::uint64_t>(signed_revision);
    auto reloaded = repository_.reload(revision, context);
    if (!reloaded)
    {
        return Result<ipc::CommandResponse>::failure(reloaded.error());
    }
    return Result<ipc::CommandResponse>::success(
        {.payload_json = config_summary(reloaded.value()).dump(), .binary = {}});
}

} // namespace paperbreak::service
