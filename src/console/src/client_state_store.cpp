#include "paperbreak/console/client_state_store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <utility>

namespace paperbreak::console
{
namespace
{

using Json = nlohmann::json;

Error state_error(std::string message, std::string operation)
{
    return make_error("IPC_PROTOCOL_ERROR", Severity::error, std::move(message), "console",
                      std::move(operation));
}

Result<Json> response_payload(const ipc::ResponseMessage& response, std::string operation)
{
    if (!response.success)
    {
        if (response.error.has_value())
        {
            return Result<Json>::failure(response.error.value());
        }
        return Result<Json>::failure(state_error("服务查询失败但未携带错误", std::move(operation)));
    }
    Json payload = Json::parse(response.payload_json, nullptr, false);
    if (payload.is_discarded() || !payload.is_object())
    {
        return Result<Json>::failure(
            state_error("服务响应不是有效 JSON 对象", std::move(operation)));
    }
    return Result<Json>::success(std::move(payload));
}

Result<ServiceStatusSummary> parse_status(const ipc::ResponseMessage& response,
                                          const std::uint64_t generation)
{
    auto parsed = response_payload(response, "console.status.parse");
    if (!parsed)
    {
        return Result<ServiceStatusSummary>::failure(parsed.error());
    }
    const Json& payload = parsed.value();
    if (!payload.contains("serviceState") || !payload["serviceState"].is_string() ||
        payload["serviceState"].get_ref<const std::string&>().empty() ||
        !payload.contains("machineId") || !payload["machineId"].is_string() ||
        !payload.contains("timestamp") || !payload["timestamp"].is_string() ||
        !payload.contains("acceptingWrites") || !payload["acceptingWrites"].is_boolean() ||
        (payload.contains("loggingLevel") && !payload["loggingLevel"].is_string()))
    {
        return Result<ServiceStatusSummary>::failure(
            state_error("system.getStatus 响应结构无效", "console.status.parse"));
    }
    return Result<ServiceStatusSummary>::success(
        {.service_state = payload["serviceState"].get<std::string>(),
         .machine_id = payload["machineId"].get<std::string>(),
         .service_timestamp = payload["timestamp"].get<std::string>(),
         .logging_level = payload.value("loggingLevel", std::string{"info"}),
         .accepting_writes = payload["acceptingWrites"].get<bool>(),
         .generation = generation});
}

Result<VersionSummary> parse_version(const ipc::ResponseMessage& response,
                                     const std::uint64_t generation)
{
    auto parsed = response_payload(response, "console.version.parse");
    if (!parsed)
    {
        return Result<VersionSummary>::failure(parsed.error());
    }
    const Json& payload = parsed.value();
    if (!payload.contains("applicationVersion") || !payload["applicationVersion"].is_string() ||
        payload["applicationVersion"].get_ref<const std::string&>().empty() ||
        !payload.contains("gitCommit") || !payload["gitCommit"].is_string())
    {
        return Result<VersionSummary>::failure(
            state_error("system.getVersion 响应结构无效", "console.version.parse"));
    }
    return Result<VersionSummary>::success(
        {.application_version = payload["applicationVersion"].get<std::string>(),
         .git_commit = payload["gitCommit"].get<std::string>(),
         .generation = generation});
}

std::optional<double> numeric_metric(const Json& value)
{
    if (!value.is_number())
    {
        return std::nullopt;
    }
    return value.get<double>();
}

Result<SystemMetricsSummary> parse_metrics(const ipc::ResponseMessage& response,
                                           const std::uint64_t generation)
{
    auto parsed = response_payload(response, "console.metrics.parse");
    if (!parsed)
    {
        return Result<SystemMetricsSummary>::failure(parsed.error());
    }
    const Json& payload = parsed.value();
    if (!payload.contains("sampledAt") || !payload["sampledAt"].is_string() ||
        !payload.contains("metrics") || !payload["metrics"].is_array() ||
        !payload.contains("truncated") || !payload["truncated"].is_boolean())
    {
        return Result<SystemMetricsSummary>::failure(
            state_error("system.getMetrics 响应结构无效", "console.metrics.parse"));
    }

    SystemMetricsSummary summary;
    summary.sampled_at = payload["sampledAt"].get<std::string>();
    summary.generation = generation;
    for (const auto& metric : payload["metrics"])
    {
        if (!metric.is_object() || !metric.contains("name") || !metric["name"].is_string() ||
            !metric.contains("value") || !metric.contains("unit") || !metric["unit"].is_string() ||
            !metric.contains("available") || !metric["available"].is_boolean())
        {
            return Result<SystemMetricsSummary>::failure(
                state_error("system.getMetrics 指标结构无效", "console.metrics.parse"));
        }
        if (!metric["available"].get<bool>())
        {
            continue;
        }
        const std::string& name = metric["name"].get_ref<const std::string&>();
        if (name == "uplink.state" && metric["value"].is_string())
        {
            summary.uplink_state = metric["value"].get<std::string>();
            continue;
        }
        const auto value = numeric_metric(metric["value"]);
        if (!value.has_value())
        {
            continue;
        }
        if (name == "process.cpu.percent")
        {
            summary.process_cpu_percent = value;
        }
        else if (name == "system.memory.used_percent")
        {
            summary.system_memory_used_percent = value;
        }
        else if (name == "disk.event.free_gib")
        {
            summary.event_disk_free_gib = value;
        }
        else if (name == "uplink.pending_upload_tasks" && *value >= 0.0)
        {
            summary.pending_upload_tasks = static_cast<std::uint64_t>(*value);
        }
    }
    return Result<SystemMetricsSummary>::success(std::move(summary));
}

Result<AlarmOverviewSummary> parse_alarms(const ipc::ResponseMessage& response,
                                          const std::uint64_t generation)
{
    auto parsed = response_payload(response, "console.alarms.parse");
    if (!parsed)
    {
        return Result<AlarmOverviewSummary>::failure(parsed.error());
    }
    const Json& payload = parsed.value();
    if (!payload.contains("alarms") || !payload["alarms"].is_array() ||
        !payload.contains("truncated") || !payload["truncated"].is_boolean())
    {
        return Result<AlarmOverviewSummary>::failure(
            state_error("alarm.list 响应结构无效", "console.alarms.parse"));
    }

    AlarmOverviewSummary summary;
    summary.active_count = payload["alarms"].size();
    summary.count_truncated = payload["truncated"].get<bool>();
    summary.generation = generation;
    summary.recent.reserve(std::min<std::size_t>(5U, summary.active_count));
    int highest_rank = 0;
    for (const auto& alarm : payload["alarms"])
    {
        if (!alarm.is_object() || !alarm.contains("alarmId") ||
            !alarm["alarmId"].is_number_unsigned() || !alarm.contains("severity") ||
            !alarm["severity"].is_string() || !alarm.contains("source") ||
            !alarm["source"].is_string() || !alarm.contains("lastOccurredAt") ||
            !alarm["lastOccurredAt"].is_string() || !alarm.contains("active") ||
            !alarm["active"].is_boolean() || !alarm.contains("message") ||
            !alarm["message"].is_string() || !alarm.contains("acknowledged") ||
            !alarm["acknowledged"].is_boolean())
        {
            return Result<AlarmOverviewSummary>::failure(
                state_error("alarm.list 报警结构无效", "console.alarms.parse"));
        }
        if (!alarm["active"].get<bool>())
        {
            continue;
        }
        const std::string severity = alarm["severity"].get<std::string>();
        int rank = 0;
        if (severity == "Warning")
            rank = 1;
        else if (severity == "Error")
            rank = 2;
        else if (severity == "Critical")
            rank = 3;
        else if (severity != "Info")
            return Result<AlarmOverviewSummary>::failure(
                state_error("alarm.list 包含未知报警等级", "console.alarms.parse"));
        if (rank > highest_rank)
        {
            highest_rank = rank;
            summary.highest_severity = severity;
        }
        if (summary.recent.size() < 5U)
        {
            summary.recent.push_back(
                {.alarm_id = alarm["alarmId"].get<std::uint64_t>(),
                 .severity = severity,
                 .source = alarm["source"].get<std::string>(),
                 .last_occurred_at = alarm["lastOccurredAt"].get<std::string>(),
                 .message = alarm["message"].get<std::string>(),
                 .acknowledged = alarm["acknowledged"].get<bool>()});
        }
    }
    return Result<AlarmOverviewSummary>::success(std::move(summary));
}

Result<ServiceLocationsSummary> parse_locations(const ipc::ResponseMessage& response,
                                                const std::uint64_t generation)
{
    auto parsed = response_payload(response, "console.locations.parse");
    if (!parsed)
        return Result<ServiceLocationsSummary>::failure(parsed.error());
    const Json& payload = parsed.value();
    if (payload.size() != 1U || !payload.contains("eventRoot") ||
        !payload["eventRoot"].is_string() ||
        payload["eventRoot"].get_ref<const std::string&>().empty())
    {
        return Result<ServiceLocationsSummary>::failure(
            state_error("system.getLocations 响应结构无效", "console.locations.parse"));
    }
    return Result<ServiceLocationsSummary>::success(
        {.event_root = payload["eventRoot"].get<std::string>(), .generation = generation});
}

bool current_request(const ipc::ClientRequestHandle& handle,
                     const std::optional<ipc::ClientRequestHandle>& expected,
                     const ClientStateSnapshot& snapshot)
{
    return handle.generation == snapshot.connection.generation &&
           snapshot.connection.state == ipc::ClientConnectionState::connected &&
           expected.has_value() && expected.value() == handle;
}

} // namespace

ClientStateStore::ClientStateStore(ClientStateObserver observer, ipc::IpcClientOptions options)
    : observer_(std::move(observer))
{
    ipc::IpcClientCallbacks callbacks;
    callbacks.connection_changed = [this](const ipc::ClientConnectionSnapshot& connection) {
        connection_changed(connection);
    };
    callbacks.push_received = [this](const std::uint64_t generation, const ipc::PushMessage& push) {
        push_received(generation, push);
    };
    client_ = std::make_unique<ipc::IpcClient>(std::move(callbacks), std::move(options));
}

ClientStateStore::~ClientStateStore()
{
    stop();
}

Result<void> ClientStateStore::start()
{
    return client_->start();
}

void ClientStateStore::stop() noexcept
{
    client_->stop();
    status_request_.reset();
    version_request_.reset();
    metrics_request_.reset();
    alarms_request_.reset();
    locations_request_.reset();
    alarm_push_refresh_pending_ = false;
}

void ClientStateStore::refresh_dynamic()
{
    if (snapshot_.connection.state != ipc::ClientConnectionState::connected)
    {
        return;
    }
    if (snapshot_.service_status_stale)
    {
        synchronize_status(snapshot_.connection.generation);
    }
    if (snapshot_.version_stale)
    {
        synchronize_version(snapshot_.connection.generation);
    }
    synchronize_metrics(snapshot_.connection.generation);
    synchronize_alarms(snapshot_.connection.generation);
    if (snapshot_.locations_stale)
        synchronize_locations(snapshot_.connection.generation);
}

const ClientStateSnapshot& ClientStateStore::snapshot() const noexcept
{
    return snapshot_;
}

void ClientStateStore::connection_changed(const ipc::ClientConnectionSnapshot& connection)
{
    const bool new_connection =
        connection.state == ipc::ClientConnectionState::connected &&
        (snapshot_.connection.state != ipc::ClientConnectionState::connected ||
         snapshot_.connection.generation != connection.generation);
    snapshot_.connection = connection;
    if (connection.state != ipc::ClientConnectionState::connected)
    {
        snapshot_.service_status_stale = true;
        snapshot_.version_stale = true;
        snapshot_.metrics_stale = true;
        snapshot_.alarms_stale = true;
        snapshot_.locations_stale = true;
        status_request_.reset();
        version_request_.reset();
        metrics_request_.reset();
        alarms_request_.reset();
        locations_request_.reset();
        alarm_push_refresh_pending_ = false;
        notify();
        return;
    }

    if (!new_connection)
    {
        notify();
        return;
    }
    snapshot_.service_status_stale = true;
    snapshot_.version_stale = true;
    snapshot_.metrics_stale = true;
    snapshot_.alarms_stale = true;
    snapshot_.locations_stale = true;
    snapshot_.synchronization_error.reset();
    snapshot_.version_error.reset();
    snapshot_.metrics_error.reset();
    snapshot_.alarms_error.reset();
    snapshot_.locations_error.reset();
    status_request_.reset();
    version_request_.reset();
    metrics_request_.reset();
    alarms_request_.reset();
    locations_request_.reset();
    alarm_push_refresh_pending_ = false;
    notify();
    synchronize_status(connection.generation);
    synchronize_version(connection.generation);
    synchronize_metrics(connection.generation);
    synchronize_alarms(connection.generation);
    synchronize_locations(connection.generation);
}

void ClientStateStore::push_received(const std::uint64_t generation, const ipc::PushMessage& push)
{
    if (generation != snapshot_.connection.generation ||
        snapshot_.connection.state != ipc::ClientConnectionState::connected)
    {
        return;
    }
    if (push.event_name == "status.changed")
    {
        const Json payload = Json::parse(push.payload_json, nullptr, false);
        if (payload.is_discarded() || !payload.is_object() || !payload.contains("serviceState") ||
            !payload["serviceState"].is_string() || !snapshot_.service_status.has_value())
        {
            return;
        }
        snapshot_.service_status->service_state = payload["serviceState"].get<std::string>();
        snapshot_.service_status->generation = generation;
        notify();
        return;
    }
    if (push.event_name == "alarm.raised" || push.event_name == "alarm.cleared" ||
        push.event_name == "alarm.acknowledged")
    {
        if (alarms_request_.has_value())
        {
            alarm_push_refresh_pending_ = true;
        }
        else
        {
            synchronize_alarms(generation);
        }
    }
}

void ClientStateStore::synchronize_status(const std::uint64_t generation)
{
    if (generation != snapshot_.connection.generation ||
        snapshot_.connection.state != ipc::ClientConnectionState::connected ||
        status_request_.has_value())
    {
        return;
    }
    auto request = client_->send_request(
        "system.getStatus", "{}", {},
        [this](ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result) {
            status_completed(std::move(handle), std::move(result));
        },
        std::chrono::seconds{5});
    if (!request)
    {
        snapshot_.synchronization_error = request.error();
        snapshot_.service_status_stale = true;
        notify();
        return;
    }
    status_request_ = std::move(request).value();
}

void ClientStateStore::synchronize_version(const std::uint64_t generation)
{
    if (generation != snapshot_.connection.generation ||
        snapshot_.connection.state != ipc::ClientConnectionState::connected ||
        version_request_.has_value())
    {
        return;
    }
    auto request = client_->send_request(
        "system.getVersion", "{}", {},
        [this](ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result) {
            version_completed(std::move(handle), std::move(result));
        },
        std::chrono::seconds{5});
    if (!request)
    {
        snapshot_.version_error = request.error();
        snapshot_.version_stale = true;
        notify();
        return;
    }
    version_request_ = std::move(request).value();
}

void ClientStateStore::synchronize_metrics(const std::uint64_t generation)
{
    if (generation != snapshot_.connection.generation ||
        snapshot_.connection.state != ipc::ClientConnectionState::connected ||
        metrics_request_.has_value())
    {
        return;
    }
    auto request = client_->send_request(
        "system.getMetrics",
        R"({"prefixes":["process.cpu.","system.memory.","disk.event.","uplink."],"limit":64})", {},
        [this](ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result) {
            metrics_completed(std::move(handle), std::move(result));
        },
        std::chrono::seconds{5});
    if (!request)
    {
        snapshot_.metrics_error = request.error();
        snapshot_.metrics_stale = true;
        notify();
        return;
    }
    metrics_request_ = std::move(request).value();
}

void ClientStateStore::synchronize_alarms(const std::uint64_t generation)
{
    if (generation != snapshot_.connection.generation ||
        snapshot_.connection.state != ipc::ClientConnectionState::connected ||
        alarms_request_.has_value())
    {
        return;
    }
    auto request = client_->send_request(
        "alarm.list", R"({"active":true,"limit":200})", {},
        [this](ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result) {
            alarms_completed(std::move(handle), std::move(result));
        },
        std::chrono::seconds{5});
    if (!request)
    {
        snapshot_.alarms_error = request.error();
        snapshot_.alarms_stale = true;
        notify();
        return;
    }
    alarms_request_ = std::move(request).value();
}

void ClientStateStore::synchronize_locations(const std::uint64_t generation)
{
    if (generation != snapshot_.connection.generation ||
        snapshot_.connection.state != ipc::ClientConnectionState::connected ||
        locations_request_.has_value())
        return;
    auto request = client_->send_request(
        "system.getLocations", "{}", {},
        [this](ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result) {
            locations_completed(std::move(handle), std::move(result));
        },
        std::chrono::seconds{5});
    if (!request)
    {
        snapshot_.locations_error = request.error();
        snapshot_.locations_stale = true;
        notify();
        return;
    }
    locations_request_ = std::move(request).value();
}

void ClientStateStore::status_completed(ipc::ClientRequestHandle handle,
                                        Result<ipc::ResponseMessage> result)
{
    if (!current_request(handle, status_request_, snapshot_))
    {
        return;
    }
    status_request_.reset();
    if (!result)
    {
        snapshot_.synchronization_error = result.error();
        snapshot_.service_status_stale = true;
        notify();
        return;
    }
    auto status = parse_status(result.value(), handle.generation);
    if (!status)
    {
        snapshot_.synchronization_error = status.error();
        snapshot_.service_status_stale = true;
        notify();
        return;
    }
    snapshot_.service_status = std::move(status).value();
    snapshot_.service_status_stale = false;
    snapshot_.synchronization_error.reset();
    notify();
}

void ClientStateStore::version_completed(ipc::ClientRequestHandle handle,
                                         Result<ipc::ResponseMessage> result)
{
    if (!current_request(handle, version_request_, snapshot_))
    {
        return;
    }
    version_request_.reset();
    if (!result)
    {
        snapshot_.version_error = result.error();
        snapshot_.version_stale = true;
        notify();
        return;
    }
    auto version = parse_version(result.value(), handle.generation);
    if (!version)
    {
        snapshot_.version_error = version.error();
        snapshot_.version_stale = true;
        notify();
        return;
    }
    snapshot_.version = std::move(version).value();
    snapshot_.version_stale = false;
    snapshot_.version_error.reset();
    notify();
}

void ClientStateStore::metrics_completed(ipc::ClientRequestHandle handle,
                                         Result<ipc::ResponseMessage> result)
{
    if (!current_request(handle, metrics_request_, snapshot_))
    {
        return;
    }
    metrics_request_.reset();
    if (!result)
    {
        snapshot_.metrics_error = result.error();
        snapshot_.metrics_stale = true;
        notify();
        return;
    }
    auto metrics = parse_metrics(result.value(), handle.generation);
    if (!metrics)
    {
        snapshot_.metrics_error = metrics.error();
        snapshot_.metrics_stale = true;
        notify();
        return;
    }
    snapshot_.metrics = std::move(metrics).value();
    snapshot_.metrics_stale = false;
    snapshot_.metrics_error.reset();
    notify();
}

void ClientStateStore::alarms_completed(ipc::ClientRequestHandle handle,
                                        Result<ipc::ResponseMessage> result)
{
    if (!current_request(handle, alarms_request_, snapshot_))
    {
        return;
    }
    alarms_request_.reset();
    const auto refresh_after_push = [this, generation = handle.generation] {
        if (alarm_push_refresh_pending_ &&
            snapshot_.connection.state == ipc::ClientConnectionState::connected &&
            snapshot_.connection.generation == generation)
        {
            alarm_push_refresh_pending_ = false;
            synchronize_alarms(generation);
        }
    };
    if (!result)
    {
        snapshot_.alarms_error = result.error();
        snapshot_.alarms_stale = true;
        notify();
        refresh_after_push();
        return;
    }
    auto alarms = parse_alarms(result.value(), handle.generation);
    if (!alarms)
    {
        snapshot_.alarms_error = alarms.error();
        snapshot_.alarms_stale = true;
        notify();
        refresh_after_push();
        return;
    }
    snapshot_.alarms = std::move(alarms).value();
    snapshot_.alarms_stale = false;
    snapshot_.alarms_error.reset();
    notify();
    refresh_after_push();
}

void ClientStateStore::locations_completed(ipc::ClientRequestHandle handle,
                                           Result<ipc::ResponseMessage> result)
{
    if (!current_request(handle, locations_request_, snapshot_))
        return;
    locations_request_.reset();
    if (!result)
    {
        snapshot_.locations_error = result.error();
        snapshot_.locations_stale = true;
        notify();
        return;
    }
    auto locations = parse_locations(result.value(), handle.generation);
    if (!locations)
    {
        snapshot_.locations_error = locations.error();
        snapshot_.locations_stale = true;
        notify();
        return;
    }
    snapshot_.locations = std::move(locations).value();
    snapshot_.locations_stale = false;
    snapshot_.locations_error.reset();
    notify();
}

void ClientStateStore::notify() const noexcept
{
    if (!observer_)
    {
        return;
    }
    try
    {
        observer_(snapshot_);
    }
    catch (...)
    {
    }
}

} // namespace paperbreak::console
