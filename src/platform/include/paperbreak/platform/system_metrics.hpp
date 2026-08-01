#pragma once

#include "paperbreak/monitoring/monitoring.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace paperbreak::platform
{

struct DiskMetricPath final
{
    std::string label;
    std::filesystem::path path;
};

/// Creates the Windows process/system/disk snapshot source used by HealthMonitor.
[[nodiscard]] std::shared_ptr<monitoring::IMetricSource> make_windows_system_metric_source(
    std::vector<DiskMetricPath> disks);

/// Returns the Windows system volume root without exposing Win32 types.
[[nodiscard]] Result<std::filesystem::path> windows_system_volume() noexcept;

} // namespace paperbreak::platform
