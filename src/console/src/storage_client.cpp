#include "paperbreak/console/storage_client.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace paperbreak::console
{
namespace
{
using Json = nlohmann::json;

Error client_error(std::string code, std::string message, std::string operation,
                   const bool retryable = false)
{
    return make_error(std::move(code), Severity::warning, std::move(message), "console",
                      std::move(operation), retryable);
}

Result<Json> response_payload(const Result<ipc::ResponseMessage>& result,
                              const std::string_view operation)
{
    if (!result)
        return Result<Json>::failure(result.error());
    if (!result.value().success)
        return Result<Json>::failure(result.value().error.value_or(
            client_error("IPC_PROTOCOL_ERROR", "后台服务返回未知失败", std::string{operation})));
    auto payload = Json::parse(result.value().payload_json, nullptr, false);
    if (payload.is_discarded() || !payload.is_object())
        return Result<Json>::failure(
            client_error("IPC_PROTOCOL_ERROR", "存储配置响应不是有效对象", std::string{operation}));
    return Result<Json>::success(std::move(payload));
}

StorageConfigurationValue configuration_value(const Json& value)
{
    return {.event_root = value.value("eventRoot", std::string{}),
            .cache_root = value.value("cacheRoot", std::string{}),
            .rolling_cache_enabled = value.value("rollingCacheEnabled", false),
            .maximum_cache_storage_gib = value.value("maximumCacheStorageGiB", 1000U),
            .rolling_cache_write_limit_mibps = value.value("rollingCacheWriteLimitMiBps", 600U),
            .rolling_cache_io_timeout_ms = value.value("rollingCacheIoTimeoutMs", 10000U),
            .warning_free_space_gib = value.value("warningFreeSpaceGiB", 200U),
            .critical_free_space_gib = value.value("criticalFreeSpaceGiB", 100U),
            .stop_free_space_gib = value.value("stopFreeSpaceGiB", 20U),
            .maximum_event_storage_gib = value.value("maximumEventStorageGiB", 1000U)};
}

Json configuration_json(const StorageConfigurationValue& value)
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

} // namespace

StorageClient::StorageClient(StorageClientObserver observer, ipc::IpcClientOptions options)
    : observer_(std::move(observer))
{
    client_ = std::make_unique<ipc::IpcClient>(
        ipc::IpcClientCallbacks{
            .connection_changed = [this](const auto& value) { connection_changed(value); },
            .push_received = {}},
        std::move(options));
}

StorageClient::~StorageClient()
{
    stop();
}

Result<void> StorageClient::start()
{
    return client_->start();
}

void StorageClient::stop() noexcept
{
    if (client_)
        client_->stop();
    config_request_.reset();
    update_request_.reset();
    snapshot_.stale = true;
    snapshot_.operation_pending = false;
    notify();
}

void StorageClient::connection_changed(const ipc::ClientConnectionSnapshot& connection)
{
    snapshot_.connection = connection;
    if (connection.state == ipc::ClientConnectionState::connected)
        refresh();
    else
    {
        config_request_.reset();
        update_request_.reset();
        snapshot_.stale = true;
        snapshot_.operation_pending = false;
    }
    notify();
}

void StorageClient::refresh()
{
    if (snapshot_.connection.state != ipc::ClientConnectionState::connected || config_request_)
        return;
    auto sent =
        client_->send_request("storage.getConfig", "{}", {}, [this](auto handle, auto result) {
            config_completed(handle, std::move(result));
        });
    if (sent)
        config_request_ = sent.value();
    else
        snapshot_.error = sent.error();
    notify();
}

Result<void> StorageClient::update_configuration(StorageConfigurationValue value)
{
    if (snapshot_.connection.state != ipc::ClientConnectionState::connected)
        return Result<void>::failure(client_error("IPC_NOT_CONNECTED", "后台服务尚未连接",
                                                  "console.storage.updateConfig", true));
    if (snapshot_.stale)
        return Result<void>::failure(client_error("STORAGE_CONFIG_STALE",
                                                  "存储配置尚未同步，不能保存",
                                                  "console.storage.updateConfig", true));
    if (update_request_)
        return Result<void>::failure(
            client_error("IPC_BUSY", "存储配置更新正在执行", "console.storage.updateConfig", true));
    const Json payload{{"expectedConfigRevision", snapshot_.stored_config_revision},
                       {"storage", configuration_json(value)}};
    auto sent = client_->send_request(
        "storage.updateConfig", payload.dump(), {},
        [this](auto handle, auto result) { update_completed(handle, std::move(result)); });
    if (!sent)
        return Result<void>::failure(sent.error());
    update_request_ = sent.value();
    snapshot_.operation_pending = true;
    snapshot_.error.reset();
    notify();
    return Result<void>::success();
}

const StorageClientSnapshot& StorageClient::snapshot() const noexcept
{
    return snapshot_;
}

void StorageClient::config_completed(const ipc::ClientRequestHandle handle,
                                     Result<ipc::ResponseMessage> result)
{
    if (!config_request_ || *config_request_ != handle)
        return;
    config_request_.reset();
    auto payload = response_payload(result, "console.storage.getConfig");
    if (!payload)
        snapshot_.error = payload.error();
    else
    {
        snapshot_.configuration = configuration_value(payload.value().at("storage"));
        snapshot_.effective_configuration =
            configuration_value(payload.value().at("effectiveStorage"));
        snapshot_.stored_config_revision =
            payload.value().value("storedConfigRevision", std::uint64_t{});
        snapshot_.effective_config_revision =
            payload.value().value("effectiveConfigRevision", std::uint64_t{});
        snapshot_.pending_restart_paths =
            payload.value().value("pendingRestartPaths", std::vector<std::string>{});
        snapshot_.stale = false;
        snapshot_.error.reset();
    }
    notify();
}

void StorageClient::update_completed(const ipc::ClientRequestHandle handle,
                                     Result<ipc::ResponseMessage> result)
{
    if (!update_request_ || *update_request_ != handle)
        return;
    update_request_.reset();
    snapshot_.operation_pending = false;
    auto payload = response_payload(result, "console.storage.updateConfig");
    if (!payload)
        snapshot_.error = payload.error();
    else
    {
        snapshot_.configuration = configuration_value(payload.value().at("storage"));
        snapshot_.effective_configuration =
            configuration_value(payload.value().at("effectiveStorage"));
        snapshot_.stored_config_revision =
            payload.value().value("storedConfigRevision", std::uint64_t{});
        snapshot_.effective_config_revision =
            payload.value().value("effectiveConfigRevision", std::uint64_t{});
        snapshot_.pending_restart_paths =
            payload.value().value("pendingRestartPaths", std::vector<std::string>{});
        snapshot_.stale = false;
        snapshot_.error.reset();
    }
    notify();
}

void StorageClient::notify() const noexcept
{
    try
    {
        if (observer_)
            observer_(snapshot_);
    }
    catch (...)
    {
    }
}

} // namespace paperbreak::console
