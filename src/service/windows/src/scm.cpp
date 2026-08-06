#include "paperbreak/service/windows/scm.hpp"

#include <thread>
#include <utility>

namespace paperbreak::service::windows
{

namespace
{

Error operation_error(std::string business_code, std::string message, std::string operation)
{
    return make_error(std::move(business_code), Severity::error, std::move(message), "service",
                      std::move(operation));
}

Error operation_failure(Error cause, std::string business_code, std::string operation)
{
    if (cause.business_code != business_code)
    {
        cause.details.push_back(
            {.key = "causeBusinessCode", .value = std::move(cause.business_code)});
    }
    cause.business_code = std::move(business_code);
    cause.operation = std::move(operation);
    return cause;
}

} // namespace

ServiceManager::ServiceManager(IServiceManagerApi& api) noexcept : api_(api) {}

Result<InstallOutcome> ServiceManager::install(const ServiceDefinition& definition)
{
    auto access_result = api_.verify_management_access();
    if (!access_result)
    {
        return Result<InstallOutcome>::failure(
            operation_failure(access_result.error(), "SYS_SERVICE_INSTALL_FAILED",
                              "service.scm.install.verifyAdministrator"));
    }

    auto query_result = api_.query(definition.name);
    if (!query_result)
    {
        return Result<InstallOutcome>::failure(operation_failure(
            query_result.error(), "SYS_SERVICE_INSTALL_FAILED", "service.scm.install.query"));
    }

    if (query_result.value() == ServicePresence::marked_for_delete)
    {
        return Result<InstallOutcome>::failure(operation_error("SYS_SERVICE_INSTALL_FAILED",
                                                               "服务正等待删除，暂时无法安装",
                                                               "service.scm.install.query"));
    }

    const bool created = query_result.value() == ServicePresence::missing;
    auto base_result = created ? api_.create(definition) : api_.update(definition);
    if (!base_result)
    {
        return Result<InstallOutcome>::failure(operation_failure(
            base_result.error(), "SYS_SERVICE_INSTALL_FAILED",
            created ? "service.scm.install.create" : "service.scm.install.update"));
    }

    auto configure_result = api_.configure(definition);
    if (!configure_result)
    {
        Error error = operation_failure(configure_result.error(), "SYS_SERVICE_INSTALL_FAILED",
                                        "service.scm.install.configure");
        if (created)
        {
            auto rollback_result = api_.remove(definition.name);
            error.details.push_back(
                {.key = "rollback", .value = rollback_result ? "removed" : "remove_failed"});
            if (!rollback_result)
            {
                error.details.push_back({.key = "rollbackBusinessCode",
                                         .value = rollback_result.error().business_code});
            }
        }
        return Result<InstallOutcome>::failure(std::move(error));
    }

    auto runtime_access_result = api_.configure_runtime_access(definition.name);
    if (!runtime_access_result)
    {
        Error error = operation_failure(runtime_access_result.error(), "SYS_SERVICE_INSTALL_FAILED",
                                        "service.scm.install.runtimeAccess");
        if (created)
        {
            auto rollback_result = api_.remove(definition.name);
            error.details.push_back(
                {.key = "rollback", .value = rollback_result ? "removed" : "remove_failed"});
            if (!rollback_result)
            {
                error.details.push_back({.key = "rollbackBusinessCode",
                                         .value = rollback_result.error().business_code});
            }
        }
        return Result<InstallOutcome>::failure(std::move(error));
    }

    return Result<InstallOutcome>::success(created ? InstallOutcome::created
                                                   : InstallOutcome::converged);
}

Result<UninstallOutcome> ServiceManager::uninstall(const std::string_view name)
{
    auto access_result = api_.verify_management_access();
    if (!access_result)
    {
        return Result<UninstallOutcome>::failure(
            operation_failure(access_result.error(), "SYS_SERVICE_UNINSTALL_FAILED",
                              "service.scm.uninstall.verifyAdministrator"));
    }

    auto query_result = api_.query(name);
    if (!query_result)
    {
        return Result<UninstallOutcome>::failure(operation_failure(
            query_result.error(), "SYS_SERVICE_UNINSTALL_FAILED", "service.scm.uninstall.query"));
    }
    if (query_result.value() == ServicePresence::missing ||
        query_result.value() == ServicePresence::marked_for_delete)
    {
        return Result<UninstallOutcome>::success(UninstallOutcome::already_absent);
    }

    auto state_result = api_.query_state(name);
    if (!state_result)
    {
        return Result<UninstallOutcome>::failure(
            operation_failure(state_result.error(), "SYS_SERVICE_UNINSTALL_FAILED",
                              "service.scm.uninstall.queryState"));
    }
    if (state_result.value() != ManagedServiceState::stopped)
    {
        auto stop_result = api_.request_stop(name);
        if (!stop_result)
        {
            return Result<UninstallOutcome>::failure(operation_failure(
                stop_result.error(), "SYS_SERVICE_UNINSTALL_FAILED", "service.scm.uninstall.stop"));
        }
        auto wait_result = api_.wait_for_stopped(name, service_stop_timeout);
        if (!wait_result)
        {
            return Result<UninstallOutcome>::failure(
                operation_failure(wait_result.error(), "SYS_SERVICE_UNINSTALL_FAILED",
                                  "service.scm.uninstall.waitForStopped"));
        }
        if (!wait_result.value())
        {
            return Result<UninstallOutcome>::failure(operation_error(
                "SYS_SERVICE_UNINSTALL_FAILED", "服务未在 30 秒内停止，保留服务注册",
                "service.scm.uninstall.waitForStopped"));
        }
    }

    auto remove_result = api_.remove(name);
    if (!remove_result)
    {
        return Result<UninstallOutcome>::failure(operation_failure(
            remove_result.error(), "SYS_SERVICE_UNINSTALL_FAILED", "service.scm.uninstall.delete"));
    }
    return Result<UninstallOutcome>::success(UninstallOutcome::removed);
}

Result<void> ServiceManager::restart(const std::string_view name,
                                     const std::chrono::milliseconds timeout,
                                     const std::stop_token stop_token)
{
    auto presence = api_.query(name);
    if (!presence)
        return Result<void>::failure(operation_failure(
            presence.error(), "SYS_SERVICE_RESTART_FAILED", "service.scm.restart.query"));
    if (presence.value() != ServicePresence::present)
        return Result<void>::failure(operation_error(
            "SYS_SERVICE_RESTART_FAILED", "后台服务未安装或正在删除", "service.scm.restart.query"));

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto state = api_.query_state(name);
    if (!state)
        return Result<void>::failure(operation_failure(state.error(), "SYS_SERVICE_RESTART_FAILED",
                                                       "service.scm.restart.queryState"));
    if (state.value() != ManagedServiceState::stopped)
    {
        auto stopped = api_.request_stop(name);
        if (!stopped)
            return Result<void>::failure(operation_failure(
                stopped.error(), "SYS_SERVICE_RESTART_FAILED", "service.scm.restart.stop"));
        while (!stop_token.stop_requested() && std::chrono::steady_clock::now() < deadline)
        {
            state = api_.query_state(name);
            if (!state)
                return Result<void>::failure(
                    operation_failure(state.error(), "SYS_SERVICE_RESTART_FAILED",
                                      "service.scm.restart.waitForStopped"));
            if (state.value() == ManagedServiceState::stopped)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        if (stop_token.stop_requested())
            return Result<void>::failure(operation_error("SYS_SERVICE_RESTART_CANCELLED",
                                                         "后台服务重启已取消",
                                                         "service.scm.restart.waitForStopped"));
        if (state.value() != ManagedServiceState::stopped)
            return Result<void>::failure(operation_error("SYS_SERVICE_RESTART_FAILED",
                                                         "后台服务未在截止时间内停止",
                                                         "service.scm.restart.waitForStopped"));
    }

    auto started = api_.request_start(name);
    if (!started)
        return Result<void>::failure(operation_failure(
            started.error(), "SYS_SERVICE_RESTART_FAILED", "service.scm.restart.start"));
    while (!stop_token.stop_requested() && std::chrono::steady_clock::now() < deadline)
    {
        state = api_.query_state(name);
        if (!state)
            return Result<void>::failure(operation_failure(
                state.error(), "SYS_SERVICE_RESTART_FAILED", "service.scm.restart.waitForRunning"));
        if (state.value() == ManagedServiceState::running)
            return Result<void>::success();
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    if (stop_token.stop_requested())
        return Result<void>::failure(operation_error("SYS_SERVICE_RESTART_CANCELLED",
                                                     "后台服务重启已取消",
                                                     "service.scm.restart.waitForRunning"));
    return Result<void>::failure(operation_error("SYS_SERVICE_RESTART_FAILED",
                                                 "后台服务未在截止时间内恢复运行",
                                                 "service.scm.restart.waitForRunning"));
}

std::wstring quote_windows_argument(const std::wstring_view argument)
{
    std::wstring quoted;
    quoted.reserve(argument.size() + 2U);
    quoted.push_back(L'"');

    std::size_t backslashes = 0;
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (character == L'"')
        {
            quoted.append(backslashes * 2U + 1U, L'\\');
            quoted.push_back(L'"');
        }
        else
        {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
        }
        backslashes = 0;
    }

    quoted.append(backslashes * 2U, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring build_service_command_line(const std::filesystem::path& executable,
                                        const std::filesystem::path& config)
{
    return quote_windows_argument(executable.native()) + L" --service --config " +
           quote_windows_argument(config.native());
}

} // namespace paperbreak::service::windows
