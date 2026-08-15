#pragma once

#include "paperbreak/time/time_sync_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>

namespace paperbreak::platform
{

struct WindowsClockObservation final
{
    bool time_service_installed{};
    bool time_service_running{};
    std::int64_t sample_monotonic_ns{};
    std::int64_t sample_utc_ns{};
    std::int64_t system_time_increment_ns{};
};

class IWindowsClockProbeBackend
{
  public:
    virtual ~IWindowsClockProbeBackend() = default;
    [[nodiscard]] virtual Result<WindowsClockObservation> observe(
        std::stop_token stop_token, std::chrono::steady_clock::time_point deadline) = 0;
};

class WindowsSystemClockProbe final : public time::ISystemClockProbe
{
  public:
    explicit WindowsSystemClockProbe(std::unique_ptr<IWindowsClockProbeBackend> backend,
                                     std::int64_t uncertainty_floor_ns = 50'000'000);

    [[nodiscard]] Result<time::SystemClockProbeSample> sample(
        std::stop_token stop_token, std::chrono::steady_clock::time_point deadline) override;

  private:
    std::unique_ptr<IWindowsClockProbeBackend> backend_;
    std::int64_t uncertainty_floor_ns_{};
};

/// Creates the read-only production Win32 probe. It never changes W32Time or PTP configuration.
[[nodiscard]] std::unique_ptr<time::ISystemClockProbe> create_windows_system_clock_probe(
    std::int64_t uncertainty_floor_ns = 50'000'000);

} // namespace paperbreak::platform
