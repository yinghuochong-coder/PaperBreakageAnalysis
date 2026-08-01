#include "paperbreak/service/system_commands.hpp"

#include "paperbreak/common/version.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

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

bool has_only_field(const Json& object, const std::string_view field)
{
    return object.size() == 1U && object.contains(std::string{field});
}

} // namespace

void ServiceStatusStore::set_state(const ServiceState state)
{
    if (state == ServiceState::starting)
    {
        std::scoped_lock lock{mutex_};
        if (started_at_.empty())
        {
            started_at_ = current_utc_timestamp();
        }
    }
    state_.store(state, std::memory_order_release);
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
                                           std::shared_ptr<ServiceStatusStore> status)
    : repository_(repository), status_(std::move(status))
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
