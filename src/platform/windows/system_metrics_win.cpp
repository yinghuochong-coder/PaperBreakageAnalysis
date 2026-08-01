#include "paperbreak/platform/system_metrics.hpp"

#include <Windows.h>

#include <Psapi.h>
#include <TlHelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace paperbreak::platform
{
namespace
{

using monitoring::MetricPoint;

Error windows_error(std::string message, std::string operation)
{
    const DWORD code = GetLastError();
    Error error = make_error("SYS_MONITORING_SAMPLE_FAILED", Severity::warning, std::move(message),
                             "monitoring", std::move(operation), true);
    error.native_domain = "win32";
    error.native_code = std::to_string(code);
    return error;
}

std::uint64_t file_time_value(const FILETIME& value) noexcept
{
    ULARGE_INTEGER converted{};
    converted.LowPart = value.dwLowDateTime;
    converted.HighPart = value.dwHighDateTime;
    return converted.QuadPart;
}

Result<std::uint32_t> current_process_thread_count() noexcept
{
    const DWORD process_id = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0U);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return Result<std::uint32_t>::failure(
            windows_error("无法创建线程快照", "monitoring.windows.threadSnapshot"));
    }
    const std::unique_ptr<void, decltype(&CloseHandle)> snapshot_guard{snapshot, &CloseHandle};
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    std::uint32_t count = 0U;
    if (Thread32First(snapshot, &entry) == FALSE)
    {
        return Result<std::uint32_t>::failure(
            windows_error("无法读取线程快照", "monitoring.windows.threadSnapshot"));
    }
    do
    {
        if (entry.th32OwnerProcessID == process_id)
        {
            ++count;
        }
    } while (Thread32Next(snapshot, &entry) != FALSE);
    return Result<std::uint32_t>::success(count);
}

std::filesystem::path nearest_existing_path(std::filesystem::path path)
{
    std::error_code error;
    path = std::filesystem::absolute(path, error);
    if (error)
    {
        return {};
    }
    while (!path.empty() && !std::filesystem::exists(path, error))
    {
        error.clear();
        const auto parent = path.parent_path();
        if (parent == path)
        {
            break;
        }
        path = parent;
    }
    return path;
}

class WindowsSystemMetricSource final : public monitoring::IMetricSource
{
  public:
    explicit WindowsSystemMetricSource(std::vector<DiskMetricPath> disks)
        : disks_(std::move(disks)), started_(std::chrono::steady_clock::now())
    {
    }

    [[nodiscard]] std::string_view source_name() const noexcept override
    {
        return "windows.system";
    }

    [[nodiscard]] Result<std::vector<MetricPoint>> collect(
        std::stop_token stop_token) noexcept override
    {
        if (stop_token.stop_requested())
        {
            return Result<std::vector<MetricPoint>>::success({});
        }
        try
        {
            std::vector<MetricPoint> metrics;
            metrics.reserve(8U + disks_.size() * 3U);
            const auto uptime = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_);
            metrics.push_back({.name = "service.uptime.seconds",
                               .value = static_cast<double>(uptime.count()) / 1000.0,
                               .unit = "seconds"});

            FILETIME created{};
            FILETIME exited{};
            FILETIME kernel{};
            FILETIME user{};
            if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) == FALSE)
            {
                return Result<std::vector<MetricPoint>>::failure(
                    windows_error("无法读取进程 CPU 时间", "monitoring.windows.processTimes"));
            }
            const std::uint64_t process_ticks = file_time_value(kernel) + file_time_value(user);
            const auto now = std::chrono::steady_clock::now();
            double cpu_percent = 0.0;
            bool cpu_available = false;
            {
                std::scoped_lock lock{cpu_mutex_};
                if (previous_wall_.has_value() && process_ticks >= previous_process_ticks_)
                {
                    const double wall_100ns =
                        std::chrono::duration<double>(now - previous_wall_.value()).count() *
                        10'000'000.0;
                    const DWORD processors =
                        std::max<DWORD>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), 1U);
                    if (wall_100ns > 0.0)
                    {
                        cpu_percent = static_cast<double>(process_ticks - previous_process_ticks_) /
                                      wall_100ns * 100.0 / static_cast<double>(processors);
                        cpu_percent = std::clamp(cpu_percent, 0.0, 100.0);
                        cpu_available = true;
                    }
                }
                previous_wall_ = now;
                previous_process_ticks_ = process_ticks;
            }
            metrics.push_back({.name = "process.cpu.percent",
                               .value = cpu_percent,
                               .unit = "percent",
                               .available = cpu_available});

            PROCESS_MEMORY_COUNTERS_EX process_memory{};
            process_memory.cb = sizeof(process_memory);
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                                     reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process_memory),
                                     sizeof(process_memory)) == FALSE)
            {
                return Result<std::vector<MetricPoint>>::failure(
                    windows_error("无法读取进程内存", "monitoring.windows.processMemory"));
            }
            metrics.push_back({.name = "process.memory.working_set_bytes",
                               .value = static_cast<std::uint64_t>(process_memory.WorkingSetSize),
                               .unit = "bytes"});

            MEMORYSTATUSEX memory{};
            memory.dwLength = sizeof(memory);
            if (GlobalMemoryStatusEx(&memory) == FALSE)
            {
                return Result<std::vector<MetricPoint>>::failure(
                    windows_error("无法读取系统内存", "monitoring.windows.systemMemory"));
            }
            metrics.push_back({.name = "system.memory.used_percent",
                               .value = static_cast<double>(memory.dwMemoryLoad),
                               .unit = "percent"});
            metrics.push_back({.name = "system.memory.total_bytes",
                               .value = static_cast<std::uint64_t>(memory.ullTotalPhys),
                               .unit = "bytes"});

            DWORD handle_count = 0U;
            if (GetProcessHandleCount(GetCurrentProcess(), &handle_count) == FALSE)
            {
                return Result<std::vector<MetricPoint>>::failure(
                    windows_error("无法读取进程句柄数", "monitoring.windows.handleCount"));
            }
            metrics.push_back({.name = "process.handles.count",
                               .value = static_cast<std::uint64_t>(handle_count),
                               .unit = "count"});
            auto thread_count = current_process_thread_count();
            if (!thread_count)
            {
                return Result<std::vector<MetricPoint>>::failure(thread_count.error());
            }
            metrics.push_back({.name = "process.threads.count",
                               .value = static_cast<std::uint64_t>(thread_count.value()),
                               .unit = "count"});

            constexpr double bytes_per_gib = 1024.0 * 1024.0 * 1024.0;
            for (const auto& disk : disks_)
            {
                const auto existing = nearest_existing_path(disk.path);
                ULARGE_INTEGER available{};
                ULARGE_INTEGER total{};
                ULARGE_INTEGER free{};
                const bool ok =
                    !existing.empty() &&
                    GetDiskFreeSpaceExW(existing.c_str(), &available, &total, &free) != FALSE;
                if (!ok)
                {
                    return Result<std::vector<MetricPoint>>::failure(
                        windows_error("无法读取磁盘容量", "monitoring.windows.diskSpace"));
                }
                const std::string prefix = "disk." + disk.label + '.';
                metrics.push_back({.name = prefix + "total_bytes",
                                   .value = static_cast<std::uint64_t>(total.QuadPart),
                                   .unit = "bytes",
                                   .available = ok});
                metrics.push_back({.name = prefix + "free_bytes",
                                   .value = static_cast<std::uint64_t>(available.QuadPart),
                                   .unit = "bytes",
                                   .available = ok});
                metrics.push_back({.name = prefix + "free_gib",
                                   .value = static_cast<double>(available.QuadPart) / bytes_per_gib,
                                   .unit = "GiB",
                                   .available = ok});
            }
            return Result<std::vector<MetricPoint>>::success(std::move(metrics));
        }
        catch (...)
        {
            return Result<std::vector<MetricPoint>>::failure(
                make_error("SYS_MONITORING_SAMPLE_FAILED", Severity::warning,
                           "Windows 健康指标采样发生未预期错误", "monitoring",
                           "monitoring.windows.collect", true));
        }
    }

  private:
    std::vector<DiskMetricPath> disks_;
    std::chrono::steady_clock::time_point started_;
    std::mutex cpu_mutex_;
    std::optional<std::chrono::steady_clock::time_point> previous_wall_;
    std::uint64_t previous_process_ticks_{};
};

} // namespace

std::shared_ptr<monitoring::IMetricSource> make_windows_system_metric_source(
    std::vector<DiskMetricPath> disks)
{
    return std::make_shared<WindowsSystemMetricSource>(std::move(disks));
}

Result<std::filesystem::path> windows_system_volume() noexcept
{
    std::vector<wchar_t> buffer(MAX_PATH);
    const UINT size = GetWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
    if (size == 0U || size >= buffer.size())
    {
        return Result<std::filesystem::path>::failure(
            windows_error("无法确定 Windows 系统目录", "monitoring.windows.systemVolume"));
    }
    std::filesystem::path path{buffer.data()};
    return Result<std::filesystem::path>::success(path.root_path());
}

} // namespace paperbreak::platform
