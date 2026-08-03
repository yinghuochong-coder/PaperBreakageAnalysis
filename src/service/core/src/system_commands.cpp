#include "paperbreak/service/system_commands.hpp"

#include "paperbreak/common/version.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
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
                                           std::filesystem::path config_directory)
    : repository_(repository), status_(std::move(status)), metrics_(std::move(metrics)),
      alarms_(std::move(alarms)), logging_(std::move(logging)),
      config_directory_(std::move(config_directory))
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
