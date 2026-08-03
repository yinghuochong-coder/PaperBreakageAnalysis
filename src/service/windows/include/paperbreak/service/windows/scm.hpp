#pragma once

#include "paperbreak/common/result.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace paperbreak::service::windows
{

inline constexpr std::string_view service_name = "PaperBreakEdgeService";
inline constexpr std::string_view service_display_name = "PaperBreakEdgeService";
inline constexpr std::string_view service_account = "NT AUTHORITY\\LocalService";
inline constexpr std::chrono::seconds service_stop_timeout{30};
inline constexpr std::chrono::seconds failure_reset_period{24 * 60 * 60};
inline constexpr std::chrono::milliseconds preshutdown_timeout{30'000};

struct ServiceDefinition final
{
    std::string name{service_name};
    std::string display_name{service_display_name};
    std::string account{service_account};
    std::wstring command_line;
    std::string description{"纸机断纸分析边缘后台服务"};
    std::vector<std::chrono::milliseconds> restart_delays{
        std::chrono::seconds{5}, std::chrono::seconds{15}, std::chrono::seconds{60}};
    std::chrono::seconds failure_reset{failure_reset_period};
    std::chrono::milliseconds preshutdown_wait{preshutdown_timeout};
    bool restart_on_non_crash_failure{true};
};

enum class ServicePresence
{
    missing,
    present,
    marked_for_delete,
};

enum class ManagedServiceState
{
    stopped,
    running,
    pending,
};

enum class InstallOutcome
{
    created,
    converged,
};

enum class UninstallOutcome
{
    removed,
    already_absent,
};

class IServiceManagerApi
{
  public:
    virtual ~IServiceManagerApi() = default;

    [[nodiscard]] virtual Result<void> verify_management_access() = 0;
    [[nodiscard]] virtual Result<ServicePresence> query(std::string_view name) = 0;
    [[nodiscard]] virtual Result<void> create(const ServiceDefinition& definition) = 0;
    [[nodiscard]] virtual Result<void> update(const ServiceDefinition& definition) = 0;
    [[nodiscard]] virtual Result<void> configure(const ServiceDefinition& definition) = 0;
    [[nodiscard]] virtual Result<ManagedServiceState> query_state(std::string_view name) = 0;
    [[nodiscard]] virtual Result<void> request_stop(std::string_view name) = 0;
    [[nodiscard]] virtual Result<void> request_start(std::string_view name) = 0;
    [[nodiscard]] virtual Result<bool> wait_for_stopped(std::string_view name,
                                                        std::chrono::milliseconds timeout) = 0;
    [[nodiscard]] virtual Result<void> remove(std::string_view name) = 0;
};

class ServiceManager final
{
  public:
    explicit ServiceManager(IServiceManagerApi& api) noexcept;

    [[nodiscard]] Result<InstallOutcome> install(const ServiceDefinition& definition);
    [[nodiscard]] Result<UninstallOutcome> uninstall(std::string_view name = service_name);
    [[nodiscard]] Result<void> restart(std::string_view name = service_name,
                                       std::chrono::milliseconds timeout = service_stop_timeout,
                                       std::stop_token stop_token = {});

  private:
    IServiceManagerApi& api_;
};

/// Quotes one argument according to the CommandLineToArgvW parsing rules.
[[nodiscard]] std::wstring quote_windows_argument(std::wstring_view argument);

/// Builds the SCM ImagePath command line with an explicit internal service mode and config path.
[[nodiscard]] std::wstring build_service_command_line(const std::filesystem::path& executable,
                                                      const std::filesystem::path& config);

/// Returns the current executable path using the Win32 module API.
[[nodiscard]] Result<std::filesystem::path> current_executable_path();

/// Creates the production Win32 SCM adapter. The returned object owns no service handle.
[[nodiscard]] std::unique_ptr<IServiceManagerApi> make_windows_service_manager_api();

} // namespace paperbreak::service::windows
