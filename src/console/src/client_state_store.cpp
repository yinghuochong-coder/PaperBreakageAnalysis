#include "paperbreak/console/client_state_store.hpp"

#include <nlohmann/json.hpp>

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

Result<ServiceStatusSummary> parse_status(const ipc::ResponseMessage& response,
                                          const std::uint64_t generation)
{
    if (!response.success)
    {
        if (response.error.has_value())
        {
            return Result<ServiceStatusSummary>::failure(response.error.value());
        }
        return Result<ServiceStatusSummary>::failure(
            state_error("服务状态查询失败但未携带错误", "console.status.parse"));
    }
    const Json payload = Json::parse(response.payload_json, nullptr, false);
    if (payload.is_discarded() || !payload.is_object() || !payload.contains("serviceState") ||
        !payload["serviceState"].is_string() ||
        payload["serviceState"].get_ref<const std::string&>().empty() ||
        !payload.contains("machineId") || !payload["machineId"].is_string() ||
        !payload.contains("timestamp") || !payload["timestamp"].is_string() ||
        !payload.contains("acceptingWrites") || !payload["acceptingWrites"].is_boolean())
    {
        return Result<ServiceStatusSummary>::failure(
            state_error("system.getStatus 响应结构无效", "console.status.parse"));
    }
    return Result<ServiceStatusSummary>::success(
        {.service_state = payload["serviceState"].get<std::string>(),
         .machine_id = payload["machineId"].get<std::string>(),
         .service_timestamp = payload["timestamp"].get<std::string>(),
         .accepting_writes = payload["acceptingWrites"].get<bool>(),
         .generation = generation});
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
        status_request_.reset();
        notify();
        return;
    }

    if (!new_connection)
    {
        notify();
        return;
    }
    snapshot_.service_status_stale = true;
    snapshot_.synchronization_error.reset();
    status_request_.reset();
    notify();
    synchronize_status(connection.generation);
}

void ClientStateStore::push_received(const std::uint64_t generation, const ipc::PushMessage& push)
{
    if (generation != snapshot_.connection.generation ||
        snapshot_.connection.state != ipc::ClientConnectionState::connected ||
        push.event_name != "status.changed")
    {
        return;
    }
    const Json payload = Json::parse(push.payload_json, nullptr, false);
    if (payload.is_discarded() || !payload.is_object() || !payload.contains("serviceState") ||
        !payload["serviceState"].is_string() || !snapshot_.service_status.has_value())
    {
        return;
    }
    snapshot_.service_status->service_state = payload["serviceState"].get<std::string>();
    snapshot_.service_status->generation = generation;
    notify();
}

void ClientStateStore::synchronize_status(const std::uint64_t generation)
{
    auto request = client_->send_request(
        "system.getStatus", "{}", {},
        [this](ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result) {
            status_completed(std::move(handle), std::move(result));
        },
        std::chrono::seconds{2});
    if (!request)
    {
        if (generation == snapshot_.connection.generation &&
            snapshot_.connection.state == ipc::ClientConnectionState::connected)
        {
            snapshot_.synchronization_error = request.error();
            snapshot_.service_status_stale = true;
            notify();
        }
        return;
    }
    status_request_ = std::move(request).value();
}

void ClientStateStore::status_completed(ipc::ClientRequestHandle handle,
                                        Result<ipc::ResponseMessage> result)
{
    if (handle.generation != snapshot_.connection.generation ||
        snapshot_.connection.state != ipc::ClientConnectionState::connected ||
        !status_request_.has_value() || status_request_.value() != handle)
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
