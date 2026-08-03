#include "paperbreak/service/windows/scm.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace paperbreak::service::windows
{

namespace
{

class ServiceHandle final
{
  public:
    explicit ServiceHandle(SC_HANDLE handle = nullptr) noexcept : handle_(handle) {}

    ~ServiceHandle()
    {
        if (handle_ != nullptr)
        {
            static_cast<void>(CloseServiceHandle(handle_));
        }
    }

    ServiceHandle(const ServiceHandle&) = delete;
    ServiceHandle& operator=(const ServiceHandle&) = delete;

    ServiceHandle(ServiceHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr))
    {
    }

    ServiceHandle& operator=(ServiceHandle&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_ != nullptr)
            {
                static_cast<void>(CloseServiceHandle(handle_));
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] SC_HANDLE get() const noexcept
    {
        return handle_;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return handle_ != nullptr;
    }

  private:
    SC_HANDLE handle_{nullptr};
};

Error win32_error(std::string business_code, std::string message, std::string operation,
                  const DWORD native_code)
{
    if (native_code == ERROR_ACCESS_DENIED)
    {
        message += "；该操作需要提升的管理员权限";
    }
    Error error = make_error(std::move(business_code), Severity::error, std::move(message),
                             "service", std::move(operation));
    error.native_domain = "win32";
    error.native_code = std::to_string(native_code);
    return error;
}

Result<std::wstring> to_wide(const std::string_view text, std::string operation)
{
    if (text.empty())
    {
        return Result<std::wstring>::success({});
    }
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return Result<std::wstring>::failure(
            make_error("SYS_SERVICE_CONTROL_FAILED", Severity::error, "服务字符串超过 Win32 上限",
                       "service", std::move(operation)));
    }
    const int input_size = static_cast<int>(text.size());
    const int count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), input_size, nullptr, 0);
    if (count <= 0)
    {
        return Result<std::wstring>::failure(win32_error("SYS_SERVICE_CONTROL_FAILED",
                                                         "服务字符串不是合法 UTF-8",
                                                         std::move(operation), GetLastError()));
    }
    std::wstring value(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), input_size, value.data(),
                            count) != count)
    {
        return Result<std::wstring>::failure(win32_error("SYS_SERVICE_CONTROL_FAILED",
                                                         "无法转换服务字符串", std::move(operation),
                                                         GetLastError()));
    }
    return Result<std::wstring>::success(std::move(value));
}

Result<ServiceHandle> open_manager(const DWORD access, const std::string_view business_code,
                                   const std::string_view operation)
{
    ServiceHandle manager{OpenSCManagerW(nullptr, nullptr, access)};
    if (!manager)
    {
        return Result<ServiceHandle>::failure(win32_error(std::string{business_code},
                                                          "无法打开 Windows 服务控制管理器",
                                                          std::string{operation}, GetLastError()));
    }
    return Result<ServiceHandle>::success(std::move(manager));
}

Result<ServiceHandle> open_service(const ServiceHandle& manager, const std::string_view name,
                                   const DWORD access, const std::string_view business_code,
                                   const std::string_view operation)
{
    auto wide_name = to_wide(name, std::string{operation});
    if (!wide_name)
    {
        return Result<ServiceHandle>::failure(wide_name.error());
    }
    ServiceHandle service{OpenServiceW(manager.get(), wide_name.value().c_str(), access)};
    if (!service)
    {
        return Result<ServiceHandle>::failure(win32_error(std::string{business_code},
                                                          "无法打开 Windows 服务",
                                                          std::string{operation}, GetLastError()));
    }
    return Result<ServiceHandle>::success(std::move(service));
}

class WindowsServiceManagerApi final : public IServiceManagerApi
{
  public:
    [[nodiscard]] Result<void> verify_management_access() override
    {
        auto manager = open_manager(SC_MANAGER_CREATE_SERVICE, "SYS_SERVICE_CONTROL_FAILED",
                                    "service.scm.verifyAdministrator");
        if (!manager)
        {
            return Result<void>::failure(manager.error());
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<ServicePresence> query(const std::string_view name) override
    {
        auto manager = open_manager(SC_MANAGER_CONNECT, "SYS_SERVICE_CONTROL_FAILED",
                                    "service.scm.query.openManager");
        if (!manager)
        {
            return Result<ServicePresence>::failure(manager.error());
        }
        auto wide_name = to_wide(name, "service.scm.query.name");
        if (!wide_name)
        {
            return Result<ServicePresence>::failure(wide_name.error());
        }
        ServiceHandle service{
            OpenServiceW(manager.value().get(), wide_name.value().c_str(), SERVICE_QUERY_STATUS)};
        if (service)
        {
            return Result<ServicePresence>::success(ServicePresence::present);
        }
        const DWORD native_code = GetLastError();
        if (native_code == ERROR_SERVICE_DOES_NOT_EXIST)
        {
            return Result<ServicePresence>::success(ServicePresence::missing);
        }
        if (native_code == ERROR_SERVICE_MARKED_FOR_DELETE)
        {
            return Result<ServicePresence>::success(ServicePresence::marked_for_delete);
        }
        return Result<ServicePresence>::failure(win32_error("SYS_SERVICE_CONTROL_FAILED",
                                                            "无法查询 Windows 服务",
                                                            "service.scm.query", native_code));
    }

    [[nodiscard]] Result<void> create(const ServiceDefinition& definition) override
    {
        auto manager = open_manager(SC_MANAGER_CREATE_SERVICE, "SYS_SERVICE_INSTALL_FAILED",
                                    "service.scm.install.openManager");
        if (!manager)
        {
            return Result<void>::failure(manager.error());
        }
        auto name = to_wide(definition.name, "service.scm.install.name");
        auto display_name = to_wide(definition.display_name, "service.scm.install.displayName");
        auto account = to_wide(definition.account, "service.scm.install.account");
        if (!name || !display_name || !account)
        {
            return Result<void>::failure(!name           ? name.error()
                                         : !display_name ? display_name.error()
                                                         : account.error());
        }

        ServiceHandle service{CreateServiceW(
            manager.value().get(), name.value().c_str(), display_name.value().c_str(),
            SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE,
            SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
            definition.command_line.c_str(), nullptr, nullptr, nullptr, account.value().c_str(),
            L"")};
        if (!service)
        {
            return Result<void>::failure(win32_error("SYS_SERVICE_INSTALL_FAILED",
                                                     "无法创建 Windows 服务",
                                                     "service.scm.install.create", GetLastError()));
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> update(const ServiceDefinition& definition) override
    {
        auto manager = open_manager(SC_MANAGER_CONNECT, "SYS_SERVICE_INSTALL_FAILED",
                                    "service.scm.install.openManager");
        if (!manager)
        {
            return Result<void>::failure(manager.error());
        }
        auto service = open_service(manager.value(), definition.name, SERVICE_CHANGE_CONFIG,
                                    "SYS_SERVICE_INSTALL_FAILED", "service.scm.install.open");
        if (!service)
        {
            return Result<void>::failure(service.error());
        }
        auto display_name = to_wide(definition.display_name, "service.scm.install.displayName");
        auto account = to_wide(definition.account, "service.scm.install.account");
        if (!display_name || !account)
        {
            return Result<void>::failure(!display_name ? display_name.error() : account.error());
        }
        if (ChangeServiceConfigW(
                service.value().get(), SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                SERVICE_ERROR_NORMAL, definition.command_line.c_str(), nullptr, nullptr, nullptr,
                account.value().c_str(), L"", display_name.value().c_str()) == FALSE)
        {
            return Result<void>::failure(win32_error("SYS_SERVICE_INSTALL_FAILED",
                                                     "无法更新 Windows 服务基本配置",
                                                     "service.scm.install.update", GetLastError()));
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> configure(const ServiceDefinition& definition) override
    {
        auto manager = open_manager(SC_MANAGER_CONNECT, "SYS_SERVICE_INSTALL_FAILED",
                                    "service.scm.install.openManager");
        if (!manager)
        {
            return Result<void>::failure(manager.error());
        }
        auto service = open_service(manager.value(), definition.name, SERVICE_CHANGE_CONFIG,
                                    "SYS_SERVICE_INSTALL_FAILED", "service.scm.install.open");
        if (!service)
        {
            return Result<void>::failure(service.error());
        }

        auto description_text = to_wide(definition.description, "service.scm.install.description");
        if (!description_text)
        {
            return Result<void>::failure(description_text.error());
        }
        SERVICE_DESCRIPTIONW description{description_text.value().data()};
        if (ChangeServiceConfig2W(service.value().get(), SERVICE_CONFIG_DESCRIPTION,
                                  &description) == FALSE)
        {
            return Result<void>::failure(
                win32_error("SYS_SERVICE_INSTALL_FAILED", "无法配置服务描述",
                            "service.scm.install.description", GetLastError()));
        }

        if (definition.restart_delays.empty() || definition.restart_delays.size() > 16U)
        {
            return Result<void>::failure(make_error("SYS_SERVICE_INSTALL_FAILED", Severity::error,
                                                    "恢复动作数量必须为 1 到 16", "service",
                                                    "service.scm.install.failureActions"));
        }
        std::vector<SC_ACTION> actions;
        actions.reserve(definition.restart_delays.size());
        for (const auto delay : definition.restart_delays)
        {
            const auto bounded =
                std::clamp<std::int64_t>(delay.count(), 0, std::numeric_limits<DWORD>::max());
            actions.push_back({.Type = SC_ACTION_RESTART, .Delay = static_cast<DWORD>(bounded)});
        }
        SERVICE_FAILURE_ACTIONSW failure_actions{
            .dwResetPeriod = static_cast<DWORD>(definition.failure_reset.count()),
            .lpRebootMsg = nullptr,
            .lpCommand = nullptr,
            .cActions = static_cast<DWORD>(actions.size()),
            .lpsaActions = actions.data()};
        if (ChangeServiceConfig2W(service.value().get(), SERVICE_CONFIG_FAILURE_ACTIONS,
                                  &failure_actions) == FALSE)
        {
            return Result<void>::failure(
                win32_error("SYS_SERVICE_INSTALL_FAILED", "无法配置服务恢复动作",
                            "service.scm.install.failureActions", GetLastError()));
        }

        SERVICE_FAILURE_ACTIONS_FLAG failure_flag{definition.restart_on_non_crash_failure ? TRUE
                                                                                          : FALSE};
        if (ChangeServiceConfig2W(service.value().get(), SERVICE_CONFIG_FAILURE_ACTIONS_FLAG,
                                  &failure_flag) == FALSE)
        {
            return Result<void>::failure(
                win32_error("SYS_SERVICE_INSTALL_FAILED", "无法配置服务失败动作标志",
                            "service.scm.install.failureActionsFlag", GetLastError()));
        }

        SERVICE_PRESHUTDOWN_INFO preshutdown{
            static_cast<DWORD>(definition.preshutdown_wait.count())};
        if (ChangeServiceConfig2W(service.value().get(), SERVICE_CONFIG_PRESHUTDOWN_INFO,
                                  &preshutdown) == FALSE)
        {
            return Result<void>::failure(
                win32_error("SYS_SERVICE_INSTALL_FAILED", "无法配置服务预关机时限",
                            "service.scm.install.preshutdown", GetLastError()));
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<ManagedServiceState> query_state(const std::string_view name) override
    {
        auto service_result =
            open_named_service(name, SERVICE_QUERY_STATUS, "SYS_SERVICE_UNINSTALL_FAILED",
                               "service.scm.uninstall.queryState");
        if (!service_result)
        {
            return Result<ManagedServiceState>::failure(service_result.error());
        }
        SERVICE_STATUS_PROCESS status{};
        DWORD bytes_needed = 0;
        if (QueryServiceStatusEx(service_result.value().get(), SC_STATUS_PROCESS_INFO,
                                 reinterpret_cast<BYTE*>(&status), sizeof(status),
                                 &bytes_needed) == FALSE)
        {
            return Result<ManagedServiceState>::failure(
                win32_error("SYS_SERVICE_UNINSTALL_FAILED", "无法查询服务运行状态",
                            "service.scm.uninstall.queryState", GetLastError()));
        }
        if (status.dwCurrentState == SERVICE_STOPPED)
        {
            return Result<ManagedServiceState>::success(ManagedServiceState::stopped);
        }
        if (status.dwCurrentState == SERVICE_RUNNING)
        {
            return Result<ManagedServiceState>::success(ManagedServiceState::running);
        }
        return Result<ManagedServiceState>::success(ManagedServiceState::pending);
    }

    [[nodiscard]] Result<void> request_stop(const std::string_view name) override
    {
        auto service_result =
            open_named_service(name, SERVICE_STOP | SERVICE_QUERY_STATUS,
                               "SYS_SERVICE_UNINSTALL_FAILED", "service.scm.uninstall.stop");
        if (!service_result)
        {
            return Result<void>::failure(service_result.error());
        }
        SERVICE_STATUS status{};
        if (ControlService(service_result.value().get(), SERVICE_CONTROL_STOP, &status) == FALSE)
        {
            const DWORD native_code = GetLastError();
            if (native_code != ERROR_SERVICE_NOT_ACTIVE &&
                native_code != ERROR_SERVICE_CANNOT_ACCEPT_CTRL)
            {
                return Result<void>::failure(
                    win32_error("SYS_SERVICE_UNINSTALL_FAILED", "无法请求 Windows 服务停止",
                                "service.scm.uninstall.stop", native_code));
            }
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> request_start(const std::string_view name) override
    {
        auto service_result =
            open_named_service(name, SERVICE_START | SERVICE_QUERY_STATUS,
                               "SYS_SERVICE_RESTART_FAILED", "service.scm.restart.start");
        if (!service_result)
            return Result<void>::failure(service_result.error());
        if (StartServiceW(service_result.value().get(), 0, nullptr) == FALSE)
        {
            const DWORD native_code = GetLastError();
            if (native_code != ERROR_SERVICE_ALREADY_RUNNING)
                return Result<void>::failure(win32_error("SYS_SERVICE_RESTART_FAILED",
                                                         "无法启动 Windows 服务",
                                                         "service.scm.restart.start", native_code));
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<bool> wait_for_stopped(const std::string_view name,
                                                const std::chrono::milliseconds timeout) override
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto state = query_state(name);
            if (!state)
            {
                return Result<bool>::failure(state.error());
            }
            if (state.value() == ManagedServiceState::stopped)
            {
                return Result<bool>::success(true);
            }
            if (state.value() == ManagedServiceState::running)
            {
                auto stop_result = request_stop(name);
                if (!stop_result)
                {
                    return Result<bool>::failure(stop_result.error());
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }
        return Result<bool>::success(false);
    }

    [[nodiscard]] Result<void> remove(const std::string_view name) override
    {
        auto service_result = open_named_service(name, DELETE, "SYS_SERVICE_UNINSTALL_FAILED",
                                                 "service.scm.uninstall.delete");
        if (!service_result)
        {
            if (service_result.error().native_code ==
                    std::optional<std::string>{std::to_string(ERROR_SERVICE_DOES_NOT_EXIST)} ||
                service_result.error().native_code ==
                    std::optional<std::string>{std::to_string(ERROR_SERVICE_MARKED_FOR_DELETE)})
            {
                return Result<void>::success();
            }
            return Result<void>::failure(service_result.error());
        }
        if (DeleteService(service_result.value().get()) == FALSE)
        {
            const DWORD native_code = GetLastError();
            if (native_code != ERROR_SERVICE_MARKED_FOR_DELETE)
            {
                return Result<void>::failure(
                    win32_error("SYS_SERVICE_UNINSTALL_FAILED", "无法删除 Windows 服务",
                                "service.scm.uninstall.delete", native_code));
            }
        }
        return Result<void>::success();
    }

  private:
    [[nodiscard]] Result<ServiceHandle> open_named_service(const std::string_view name,
                                                           const DWORD access,
                                                           const std::string_view business_code,
                                                           const std::string_view operation)
    {
        auto manager = open_manager(SC_MANAGER_CONNECT, business_code, operation);
        if (!manager)
        {
            return Result<ServiceHandle>::failure(manager.error());
        }
        return open_service(manager.value(), name, access, business_code, operation);
    }
};

} // namespace

Result<std::filesystem::path> current_executable_path()
{
    std::vector<wchar_t> buffer(32'768U, L'\0');
    const DWORD count =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (count == 0 || count >= buffer.size())
    {
        return Result<std::filesystem::path>::failure(
            win32_error("SYS_SERVICE_INSTALL_FAILED", "无法确定当前服务可执行文件路径",
                        "service.scm.install.executablePath", GetLastError()));
    }
    return Result<std::filesystem::path>::success(
        std::filesystem::path{std::wstring_view{buffer.data(), count}});
}

std::unique_ptr<IServiceManagerApi> make_windows_service_manager_api()
{
    return std::make_unique<WindowsServiceManagerApi>();
}

} // namespace paperbreak::service::windows
