#include "paperbreak/console/uplink_client.hpp"

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
        return Result<Json>::failure(client_error(
            "IPC_PROTOCOL_ERROR", "上位机配置响应不是有效对象", std::string{operation}));
    return Result<Json>::success(std::move(payload));
}

UplinkConfigurationValue configuration_value(const Json& value)
{
    return {.enabled = value.value("enabled", false),
            .server_url = value.value("serverUrl", std::string{}),
            .heartbeat_seconds = value.value("heartbeatSeconds", 5U),
            .chunk_bytes = value.value("chunkBytes", 1024U * 1024U),
            .io_timeout_ms = value.value("ioTimeoutMs", 10000U),
            .upload_limit_mibps = value.value("uploadLimitMiBps", 20U),
            .credential_reference = value.value("credentialReference", std::string{}),
            .certificate_reference = value.value("certificateReference", std::string{})};
}

Json configuration_json(const UplinkConfigurationValue& value)
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

void apply_payload(UplinkClientSnapshot& snapshot, const Json& payload)
{
    snapshot.configuration = configuration_value(payload.at("uplink"));
    snapshot.effective_configuration = configuration_value(payload.at("effectiveUplink"));
    snapshot.stored_config_revision = payload.value("storedConfigRevision", std::uint64_t{});
    snapshot.effective_config_revision = payload.value("effectiveConfigRevision", std::uint64_t{});
    snapshot.pending_restart_paths =
        payload.value("pendingRestartPaths", std::vector<std::string>{});
    snapshot.stale = false;
    snapshot.error.reset();
}

} // namespace

UplinkClient::UplinkClient(UplinkClientObserver observer, ipc::IpcClientOptions options)
    : observer_(std::move(observer))
{
    client_ = std::make_unique<ipc::IpcClient>(
        ipc::IpcClientCallbacks{
            .connection_changed = [this](const auto& value) { connection_changed(value); },
            .push_received = {}},
        std::move(options));
}

UplinkClient::~UplinkClient()
{
    stop();
}

Result<void> UplinkClient::start()
{
    return client_->start();
}

void UplinkClient::stop() noexcept
{
    if (client_)
        client_->stop();
    config_request_.reset();
    update_request_.reset();
    snapshot_.stale = true;
    snapshot_.operation_pending = false;
    notify();
}

void UplinkClient::connection_changed(const ipc::ClientConnectionSnapshot& connection)
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

void UplinkClient::refresh()
{
    if (snapshot_.connection.state != ipc::ClientConnectionState::connected || config_request_)
        return;
    auto sent =
        client_->send_request("uplink.getConfig", "{}", {}, [this](auto handle, auto result) {
            config_completed(handle, std::move(result));
        });
    if (sent)
        config_request_ = sent.value();
    else
        snapshot_.error = sent.error();
    notify();
}

Result<void> UplinkClient::update_configuration(UplinkConfigurationValue value)
{
    if (snapshot_.connection.state != ipc::ClientConnectionState::connected)
        return Result<void>::failure(client_error("IPC_NOT_CONNECTED", "后台服务尚未连接",
                                                  "console.uplink.updateConfig", true));
    if (snapshot_.stale)
        return Result<void>::failure(client_error("UPLINK_CONFIG_STALE",
                                                  "上位机配置尚未同步，不能保存",
                                                  "console.uplink.updateConfig", true));
    if (update_request_)
        return Result<void>::failure(client_error("IPC_BUSY", "上位机配置更新正在执行",
                                                  "console.uplink.updateConfig", true));
    const Json payload{{"expectedConfigRevision", snapshot_.stored_config_revision},
                       {"uplink", configuration_json(value)}};
    auto sent = client_->send_request(
        "uplink.updateConfig", payload.dump(), {},
        [this](auto handle, auto result) { update_completed(handle, std::move(result)); });
    if (!sent)
        return Result<void>::failure(sent.error());
    update_request_ = sent.value();
    snapshot_.operation_pending = true;
    snapshot_.error.reset();
    notify();
    return Result<void>::success();
}

const UplinkClientSnapshot& UplinkClient::snapshot() const noexcept
{
    return snapshot_;
}

void UplinkClient::config_completed(const ipc::ClientRequestHandle handle,
                                    Result<ipc::ResponseMessage> result)
{
    if (!config_request_ || *config_request_ != handle)
        return;
    config_request_.reset();
    auto payload = response_payload(result, "console.uplink.getConfig");
    if (!payload)
        snapshot_.error = payload.error();
    else
        apply_payload(snapshot_, payload.value());
    notify();
}

void UplinkClient::update_completed(const ipc::ClientRequestHandle handle,
                                    Result<ipc::ResponseMessage> result)
{
    if (!update_request_ || *update_request_ != handle)
        return;
    update_request_.reset();
    snapshot_.operation_pending = false;
    auto payload = response_payload(result, "console.uplink.updateConfig");
    if (!payload)
        snapshot_.error = payload.error();
    else
        apply_payload(snapshot_, payload.value());
    notify();
}

void UplinkClient::notify() const noexcept
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
