#pragma once

#include "paperbreak/monitoring/monitoring.hpp"
#include "paperbreak/time/time_model.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace paperbreak::service
{

struct TimeSyncAlarmOptions final
{
    std::int64_t warning_threshold_ns{1'000'000};
    std::int64_t alarm_threshold_ns{5'000'000};
    std::chrono::milliseconds warning_duration{3000};
    std::chrono::milliseconds alarm_duration{3000};
};

class TimeSyncAlarmMonitor final
{
  public:
    explicit TimeSyncAlarmMonitor(std::shared_ptr<monitoring::AlarmRegistry> alarms,
                                  TimeSyncAlarmOptions options = {});

    [[nodiscard]] Result<void> reconfigure(TimeSyncAlarmOptions options);
    void observe(
        std::int64_t monotonic_ns,
        const std::shared_ptr<const time::ClockModelSnapshot>& system_model,
        const std::vector<std::shared_ptr<const time::ClockModelSnapshot>>& camera_models) noexcept;
    void stop() noexcept;

  private:
    struct EntityState final
    {
        std::optional<time::ClockSource> source;
        std::optional<std::int64_t> warning_since_ns;
        std::optional<std::int64_t> alarm_since_ns;
        bool warning_active{};
        bool alarm_active{};
    };

    void observe_one(std::string source_id, std::int64_t monotonic_ns,
                     const std::shared_ptr<const time::ClockModelSnapshot>& model) noexcept;
    void clear_thresholds(const std::string& source_id, EntityState& state) noexcept;

    std::shared_ptr<monitoring::AlarmRegistry> alarms_;
    mutable std::mutex mutex_;
    TimeSyncAlarmOptions options_;
    std::unordered_map<std::string, EntityState> states_;
};

} // namespace paperbreak::service
