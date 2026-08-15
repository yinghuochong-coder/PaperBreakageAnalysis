#include "paperbreak/platform/windows_clock_probe.hpp"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace paperbreak::platform
{
namespace
{
constexpr std::uint64_t filetime_unix_epoch_100ns = 116'444'736'000'000'000ULL;

Error probe_error(std::string code, std::string message, std::string operation,
                  const bool retryable, const std::optional<DWORD> native = std::nullopt)
{
    auto error = make_error(std::move(code), Severity::warning, std::move(message), "time",
                            std::move(operation), retryable);
    if (native)
    {
        error.native_domain = "win32";
        error.native_code = std::to_string(*native);
    }
    return error;
}

Result<std::int64_t> filetime_to_unix_ns(const FILETIME value)
{
    ULARGE_INTEGER encoded{};
    encoded.LowPart = value.dwLowDateTime;
    encoded.HighPart = value.dwHighDateTime;
    if (encoded.QuadPart < filetime_unix_epoch_100ns)
        return Result<std::int64_t>::failure(probe_error("TIME_PROBE_UNAVAILABLE",
                                                         "Windows 返回的系统时间早于 Unix epoch",
                                                         "time.windows.readClock", true));
    const auto unix_100ns = encoded.QuadPart - filetime_unix_epoch_100ns;
    if (unix_100ns >
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) / 100ULL)
        return Result<std::int64_t>::failure(probe_error("TIME_PROBE_UNAVAILABLE",
                                                         "Windows 系统时间超出纳秒表示范围",
                                                         "time.windows.readClock", true));
    return Result<std::int64_t>::success(static_cast<std::int64_t>(unix_100ns * 100ULL));
}

class ProductionWindowsClockBackend final : public IWindowsClockProbeBackend
{
  public:
    Result<WindowsClockObservation> observe(
        const std::stop_token stop_token,
        const std::chrono::steady_clock::time_point deadline) override
    {
        if (stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline)
            return Result<WindowsClockObservation>::failure(
                probe_error("TIME_PROBE_UNAVAILABLE", "Windows 时间探针已取消或超过截止时间",
                            "time.windows.observe", true));

        const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (manager == nullptr)
            return Result<WindowsClockObservation>::failure(
                probe_error("TIME_PROBE_UNAVAILABLE", "无法打开 Windows 服务控制管理器",
                            "time.windows.openScm", true, GetLastError()));
        const auto close_manager =
            std::unique_ptr<void, decltype([](void* value) {
                                if (value != nullptr)
                                    CloseServiceHandle(static_cast<SC_HANDLE>(value));
                            })>{manager};

        const SC_HANDLE service = OpenServiceW(manager, L"W32Time", SERVICE_QUERY_STATUS);
        if (service == nullptr)
        {
            const DWORD native = GetLastError();
            if (native == ERROR_SERVICE_DOES_NOT_EXIST)
                return Result<WindowsClockObservation>::success({.time_service_installed = false});
            return Result<WindowsClockObservation>::failure(
                probe_error("TIME_PROBE_UNAVAILABLE", "无法查询 Windows Time 服务",
                            "time.windows.openW32Time", true, native));
        }
        const auto close_service =
            std::unique_ptr<void, decltype([](void* value) {
                                if (value != nullptr)
                                    CloseServiceHandle(static_cast<SC_HANDLE>(value));
                            })>{service};

        SERVICE_STATUS_PROCESS status{};
        DWORD bytes_needed{};
        if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status),
                                 sizeof(status), &bytes_needed) == FALSE)
            return Result<WindowsClockObservation>::failure(
                probe_error("TIME_PROBE_UNAVAILABLE", "无法读取 Windows Time 服务状态",
                            "time.windows.queryW32Time", true, GetLastError()));

        FILETIME utc_filetime{};
        GetSystemTimePreciseAsFileTime(&utc_filetime);
        auto utc = filetime_to_unix_ns(utc_filetime);
        if (!utc)
            return Result<WindowsClockObservation>::failure(utc.error());

        DWORD adjustment{};
        DWORD increment_100ns{};
        BOOL adjustment_disabled{};
        if (GetSystemTimeAdjustment(&adjustment, &increment_100ns, &adjustment_disabled) == FALSE)
            return Result<WindowsClockObservation>::failure(
                probe_error("TIME_PROBE_UNAVAILABLE", "无法读取 Windows 系统时间精度",
                            "time.windows.queryAdjustment", true, GetLastError()));
        static_cast<void>(adjustment);
        static_cast<void>(adjustment_disabled);
        const auto maximum_increment =
            static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) / 100ULL;
        const auto increment_ns = increment_100ns > maximum_increment
                                      ? (std::numeric_limits<std::int64_t>::max)()
                                      : static_cast<std::int64_t>(increment_100ns * 100ULL);
        const auto monotonic_ns =
            time::monotonic_time_to_nanoseconds(std::chrono::steady_clock::now()).value_or(0);
        if (stop_token.stop_requested() || std::chrono::steady_clock::now() > deadline)
            return Result<WindowsClockObservation>::failure(
                probe_error("TIME_PROBE_UNAVAILABLE", "Windows 时间探针已取消或超过截止时间",
                            "time.windows.observe", true));
        return Result<WindowsClockObservation>::success(
            {.time_service_installed = true,
             .time_service_running = status.dwCurrentState == SERVICE_RUNNING,
             .sample_monotonic_ns = monotonic_ns,
             .sample_utc_ns = utc.value(),
             .system_time_increment_ns = increment_ns});
    }
};
} // namespace

WindowsSystemClockProbe::WindowsSystemClockProbe(std::unique_ptr<IWindowsClockProbeBackend> backend,
                                                 const std::int64_t uncertainty_floor_ns)
    : backend_(std::move(backend)), uncertainty_floor_ns_(uncertainty_floor_ns)
{
}

Result<time::SystemClockProbeSample> WindowsSystemClockProbe::sample(
    const std::stop_token stop_token, const std::chrono::steady_clock::time_point deadline)
{
    if (!backend_ || uncertainty_floor_ns_ < 0)
        return Result<time::SystemClockProbeSample>::failure(probe_error(
            "TIME_PROBE_UNAVAILABLE", "Windows 时间探针配置无效", "time.windows.sample", false));
    auto observed = backend_->observe(stop_token, deadline);
    if (!observed)
        return Result<time::SystemClockProbeSample>::failure(observed.error());
    if (!observed.value().time_service_installed)
        return Result<time::SystemClockProbeSample>::failure(probe_error(
            "TIME_PROBE_NOT_SUPPORTED", "Windows Time 服务未安装", "time.windows.sample", false));
    if (!observed.value().time_service_running)
        return Result<time::SystemClockProbeSample>::failure(probe_error(
            "TIME_PROBE_UNAVAILABLE", "Windows Time 服务未运行", "time.windows.sample", true));
    if (observed.value().sample_monotonic_ns < 0 || observed.value().sample_utc_ns <= 0 ||
        observed.value().system_time_increment_ns < 0)
        return Result<time::SystemClockProbeSample>::failure(probe_error(
            "TIME_PROBE_UNAVAILABLE", "Windows 时间观测值无效", "time.windows.sample", true));

    return Result<time::SystemClockProbeSample>::success(
        {.clock_source = time::ClockSource::ntp,
         .sync_state = time::SyncState::syncing,
         .sample_monotonic_ns = observed.value().sample_monotonic_ns,
         .sample_utc_ns = observed.value().sample_utc_ns,
         .offset_ns = std::nullopt,
         .uncertainty_ns =
             std::max(uncertainty_floor_ns_, observed.value().system_time_increment_ns),
         .maximum_observed_offset_ns = std::nullopt,
         .last_synchronized_utc_ns = std::nullopt,
         .grandmaster_identity = std::nullopt,
         .last_error_code = "TIME_SYNC_SOURCE_UNVERIFIED"});
}

std::unique_ptr<time::ISystemClockProbe> create_windows_system_clock_probe(
    const std::int64_t uncertainty_floor_ns)
{
    return std::make_unique<WindowsSystemClockProbe>(
        std::make_unique<ProductionWindowsClockBackend>(), uncertainty_floor_ns);
}

} // namespace paperbreak::platform
