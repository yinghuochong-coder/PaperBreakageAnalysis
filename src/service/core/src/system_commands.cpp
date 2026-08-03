#include "paperbreak/service/system_commands.hpp"

#include "paperbreak/common/version.hpp"

#include <nlohmann/json.hpp>

#include "paperbreak/camera/control.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
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

Error command_error(std::string code, const Severity severity, std::string message,
                    std::string operation, const bool retryable = false)
{
    return make_error(std::move(code), severity, std::move(message), "ipc", std::move(operation),
                      retryable);
}

Error hikrobot_build_required(std::string operation)
{
    return command_error("SYS_NOT_SUPPORTED", Severity::warning,
                         "当前服务为 Mock-only 构建，未启用 Hikrobot MVS；请使用 "
                         "windows-vs2026-hikrobot-debug 或 windows-vs2026-hikrobot-release "
                         "预设重新构建并部署服务",
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

Result<ipc::CommandResponse> status_response(config::ConfigRepository& repository,
                                             const ServiceStatusStore& status_store)
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
    return Result<ipc::CommandResponse>::success({.payload_json = payload.dump(), .binary = {}});
}

Result<ipc::CommandResponse> version_response()
{
    const auto& version = version_info();
    Json payload{{"applicationVersion", version.application_version},
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
    return Result<ipc::CommandResponse>::success({.payload_json = payload.dump(), .binary = {}});
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
    if (!has_only_fields(payload, {"afterSequence", "categories", "minimumLevel", "limit"}))
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
    if (value.size() != 5U || !value.starts_with("CAM0") || value[4] < '1' || value[4] > '4')
        return Result<std::string>::failure(command_error("IPC_REQUEST_INVALID", Severity::error,
                                                          "cameraId 必须为 CAM01 至 CAM04",
                                                          std::string{operation}));
    return Result<std::string>::success(value);
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
    if (value.actual)
    {
        const auto& p = *value.actual;
        Json actual;
        if (p.exposure_us)
            actual["exposureUs"] = *p.exposure_us;
        if (p.gain_db)
            actual["gainDb"] = *p.gain_db;
        if (p.frame_rate)
            actual["frameRate"] = *p.frame_rate;
        if (p.roi)
            actual["roi"] = {{"width", p.roi->width},
                             {"height", p.roi->height},
                             {"offsetX", p.roi->offset_x},
                             {"offsetY", p.roi->offset_y}};
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
        result["actual"] = std::move(actual);
    }
    if (value.last_error)
        result["lastError"] = {{"code", value.last_error->business_code},
                               {"message", value.last_error->message}};
    return result;
}

Json saved_camera_json(const config::CameraConfig& value)
{
    Json result{{"exposureUs", value.exposure_us},
                {"gainDb", value.gain_db},
                {"frameRate", value.frame_rate},
                {"roi",
                 {{"width", value.roi.width},
                  {"height", value.roi.height},
                  {"offsetX", value.roi.offset_x},
                  {"offsetY", value.roi.offset_y}}},
                {"triggerSource", value.trigger_source},
                {"triggerDelayUs", value.trigger_delay_us},
                {"packetSizeBytes", value.packet_size_bytes},
                {"interPacketDelayNs", value.inter_packet_delay_ns}};
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

camera::CameraParameterSnapshot camera_parameters(const config::CameraConfig& value)
{
    camera::PixelFormat pixel = camera::PixelFormat::mono8;
    switch (value.pixel_format)
    {
    case config::PixelFormat::mono8:
        pixel = camera::PixelFormat::mono8;
        break;
    case config::PixelFormat::mono10:
        pixel = camera::PixelFormat::mono10;
        break;
    case config::PixelFormat::mono12:
        pixel = camera::PixelFormat::mono12;
        break;
    case config::PixelFormat::bayer_rg8:
        pixel = camera::PixelFormat::bayer_rg8;
        break;
    }
    camera::TriggerMode trigger = camera::TriggerMode::continuous;
    switch (value.trigger_mode)
    {
    case config::TriggerMode::continuous:
        trigger = camera::TriggerMode::continuous;
        break;
    case config::TriggerMode::hardware:
        trigger = camera::TriggerMode::hardware;
        break;
    case config::TriggerMode::software:
        trigger = camera::TriggerMode::software;
        break;
    }
    camera::CameraParameterSnapshot result{
        .exposure_us = value.exposure_us,
        .gain_db = value.gain_db,
        .frame_rate = value.frame_rate,
        .roi =
            camera::Roi{value.roi.width, value.roi.height, value.roi.offset_x, value.roi.offset_y},
        .pixel_format = pixel,
        .trigger_mode = trigger,
        .trigger_delay_us = value.trigger_delay_us,
        .packet_size_bytes = value.packet_size_bytes,
        .inter_packet_delay_ns = value.inter_packet_delay_ns};
    if (trigger == camera::TriggerMode::hardware)
        result.trigger_source = value.trigger_source;
    return result;
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
    if (!actual.exposure_us || !actual.gain_db || !actual.frame_rate || !actual.roi ||
        !actual.pixel_format || !actual.trigger_mode || !actual.trigger_delay_us ||
        !actual.packet_size_bytes || !actual.inter_packet_delay_ns)
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

    return Result<Json>::success({{"id", id},
                                  {"enabled", true},
                                  {"serialNumber", serial},
                                  {"location", location},
                                  {"exposureUs", *actual.exposure_us},
                                  {"gainDb", *actual.gain_db},
                                  {"frameRate", *actual.frame_rate},
                                  {"roi",
                                   {{"width", actual.roi->width},
                                    {"height", actual.roi->height},
                                    {"offsetX", actual.roi->offset_x},
                                    {"offsetY", actual.roi->offset_y}}},
                                  {"pixelFormat", std::move(pixel_format)},
                                  {"triggerMode", std::move(trigger_mode)},
                                  {"triggerSource", actual.trigger_source.value_or("")},
                                  {"triggerDelayUs", *actual.trigger_delay_us},
                                  {"packetSizeBytes", *actual.packet_size_bytes},
                                  {"interPacketDelayNs", *actual.inter_packet_delay_ns}});
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

SystemCommandService::SystemCommandService(config::ConfigRepository& repository,
                                           std::shared_ptr<ServiceStatusStore> status,
                                           std::shared_ptr<monitoring::MetricRegistry> metrics,
                                           std::shared_ptr<monitoring::AlarmRegistry> alarms,
                                           std::shared_ptr<logging::LoggingRuntime> logging,
                                           std::filesystem::path config_directory,
                                           std::shared_ptr<pipeline::PreviewRuntime> preview,
                                           std::shared_ptr<camera::CameraControlRuntime> cameras)
    : repository_(repository), status_(std::move(status)), metrics_(std::move(metrics)),
      alarms_(std::move(alarms)), logging_(std::move(logging)),
      config_directory_(std::move(config_directory)), preview_(std::move(preview)),
      cameras_(std::move(cameras))
{
}

Result<ipc::CommandResponse> SystemCommandService::handle(const ipc::RequestMessage& request,
                                                          const ipc::PeerIdentity& peer,
                                                          const std::stop_token stop_token)
{
    auto payload = request_payload(request);
    if (!payload)
    {
        return Result<ipc::CommandResponse>::failure(payload.error());
    }

    if (request.command.starts_with("camera."))
    {
        if (!cameras_)
            return Result<ipc::CommandResponse>::failure(
                hikrobot_build_required("ipc.camera.dispatch"));
        if (is_camera_write_command(request.command) &&
            (!peer.local || !peer.authenticated || !peer.administrator))
            return Result<ipc::CommandResponse>::failure(
                command_error("IPC_UNAUTHORIZED", Severity::error,
                              "相机控制操作要求提升后的本机管理员身份", "ipc.camera.dispatch"));
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
                        hikrobot_build_required("ipc.camera.discover"));
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
            constexpr std::array valid_slots{"CAM01", "CAM02", "CAM03", "CAM04"};
            if (std::ranges::find(valid_slots, id.value()) == valid_slots.end() || serial.empty() ||
                serial.size() > 128U || location.empty() || location.size() > 128U ||
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
            if (config.value().stored->cameras.size() >= 4U ||
                std::ranges::any_of(config.value().stored->cameras,
                                    [&](const auto& item) { return item.id == id.value(); }))
            {
                return Result<ipc::CommandResponse>::failure(
                    command_error("CAMERA_CONFIG_FAILED", Severity::error,
                                  "逻辑相机槽位已被占用或已达到四路上限", "ipc.camera.bind"));
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
                        hikrobot_build_required("ipc.camera.bind"));
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
                {.source = config::ConfigChangeSource::local_ipc,
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
            result = cameras_->connect(id.value(), found->serial_number);
            if (result)
                result = cameras_->update(id.value(), camera_parameters(*found));
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
            static constexpr std::array<std::string_view, 9U> allowed{
                "exposureUs",    "gainDb",         "frameRate",
                "roi",           "pixelFormat",    "triggerMode",
                "triggerSource", "triggerDelayUs", "packetSizeBytes"};
            for (auto it = parameters.begin(); it != parameters.end(); ++it)
                if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end() &&
                    it.key() != "interPacketDelayNs")
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
            auto saved = repository_.update(
                document.dump(), payload.value()["expectedConfigRevision"].get<std::uint64_t>(),
                {.source = config::ConfigChangeSource::local_ipc,
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
            auto applied = cameras_->update(id.value(), camera_parameters(*updated));
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
        return Result<ipc::CommandResponse>::success(
            {.payload_json = camera_snapshot_json(result.value()).dump(), .binary = {}});
    }

    if (request.command == "system.getStatus")
    {
        if (!payload.value().empty())
        {
            return Result<ipc::CommandResponse>::failure(
                command_error("IPC_REQUEST_INVALID", Severity::error,
                              "system.getStatus payload 必须为空", "ipc.system.getStatus"));
        }
        return status_response(repository_, *status_);
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
        if (!has_only_field(payload.value(), "cameraIds") ||
            !payload.value()["cameraIds"].is_array() || payload.value()["cameraIds"].empty() ||
            payload.value()["cameraIds"].size() > 4U)
            return Result<ipc::CommandResponse>::failure(command_error(
                "IPC_REQUEST_INVALID", Severity::error,
                "preview.subscribe 需要 1 至 4 个 cameraIds", "ipc.preview.subscribe"));
        std::vector<std::string> camera_ids;
        camera_ids.reserve(payload.value()["cameraIds"].size());
        for (const auto& value : payload.value()["cameraIds"])
        {
            if (!value.is_string() || value.get_ref<const std::string&>().empty() ||
                value.get_ref<const std::string&>().size() > 32U)
                return Result<ipc::CommandResponse>::failure(
                    command_error("IPC_REQUEST_INVALID", Severity::error,
                                  "cameraIds 包含无效相机编号", "ipc.preview.subscribe"));
            camera_ids.push_back(value.get<std::string>());
        }
        auto subscribed = preview_->subscribe(peer.connection_id, camera_ids);
        if (!subscribed)
            return Result<ipc::CommandResponse>::failure(subscribed.error());
        return Result<ipc::CommandResponse>::success(
            {.payload_json = Json{{"subscribed", true}, {"cameraIds", camera_ids}}.dump(),
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
        if (!peer.local || !peer.authenticated || !peer.administrator)
            return Result<ipc::CommandResponse>::failure(command_error(
                "IPC_UNAUTHORIZED", Severity::error, "alarm.acknowledge 要求提升后的本机管理员身份",
                "ipc.alarm.acknowledge"));
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
    if (!peer.local || !peer.authenticated || !peer.administrator)
    {
        return Result<ipc::CommandResponse>::failure(command_error(
            "IPC_UNAUTHORIZED", Severity::error, "system.reloadConfig 要求提升后的本机管理员身份",
            "ipc.system.reloadConfig"));
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
    context.source = config::ConfigChangeSource::local_ipc;
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
