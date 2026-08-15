#include "paperbreak/service/time_sync_monitor.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string_view>
#include <vector>

namespace
{
using namespace std::chrono_literals;

std::shared_ptr<const paperbreak::time::ClockModelSnapshot> model(
    const paperbreak::time::ClockSource source, const std::int64_t uncertainty_ns,
    const std::uint64_t revision, std::optional<std::string> camera_id = std::nullopt)
{
    return std::make_shared<const paperbreak::time::ClockModelSnapshot>(
        paperbreak::time::ClockModelSnapshot{
            .model_revision = revision,
            .camera_id = std::move(camera_id),
            .clock_source = source,
            .sync_state = source == paperbreak::time::ClockSource::ptp_hardware
                              ? paperbreak::time::SyncState::synced
                              : paperbreak::time::SyncState::degraded,
            .anchor_monotonic_ns = 1,
            .anchor_utc_ns = 2,
            .offset_ns = 0,
            .uncertainty_ns = uncertainty_ns,
            .valid_from_monotonic_ns = 1});
}

bool contains_alarm(const paperbreak::monitoring::AlarmQueryResult& result,
                    const std::string_view code, const bool active)
{
    return std::ranges::any_of(result.alarms, [code, active](const auto& alarm) {
        return alarm.code == code && alarm.active == active;
    });
}
} // namespace

TEST(TimeSyncAlarmMonitor, RecordsSourceChangesAndRequiresContinuousThresholdDuration)
{
    auto alarms = std::make_shared<paperbreak::monitoring::AlarmRegistry>();
    paperbreak::service::TimeSyncAlarmMonitor monitor{alarms,
                                                      {.warning_threshold_ns = 1'000'000,
                                                       .alarm_threshold_ns = 5'000'000,
                                                       .warning_duration = 1000ms,
                                                       .alarm_duration = 1000ms}};

    monitor.observe(0, model(paperbreak::time::ClockSource::ntp, 2'000'000, 1), {});
    monitor.observe(999'000'000, model(paperbreak::time::ClockSource::ntp, 2'000'000, 2), {});
    EXPECT_TRUE(alarms->query({.active = true}).alarms.empty());

    monitor.observe(1'000'000'000, model(paperbreak::time::ClockSource::ntp, 2'000'000, 3), {});
    EXPECT_TRUE(contains_alarm(alarms->query({.active = true}),
                               "TIME_SYNC_WARNING_THRESHOLD_EXCEEDED", true));

    monitor.observe(1'100'000'000, model(paperbreak::time::ClockSource::ntp, 6'000'000, 4), {});
    monitor.observe(2'099'000'000, model(paperbreak::time::ClockSource::ntp, 6'000'000, 5), {});
    EXPECT_FALSE(contains_alarm(alarms->query({.active = true}),
                                "TIME_SYNC_ALARM_THRESHOLD_EXCEEDED", true));
    monitor.observe(2'100'000'000, model(paperbreak::time::ClockSource::ntp, 6'000'000, 6), {});
    const auto alarm_active = alarms->query({.active = true});
    EXPECT_TRUE(contains_alarm(alarm_active, "TIME_SYNC_ALARM_THRESHOLD_EXCEEDED", true));
    EXPECT_FALSE(contains_alarm(alarm_active, "TIME_SYNC_WARNING_THRESHOLD_EXCEEDED", true));

    monitor.observe(2'200'000'000, model(paperbreak::time::ClockSource::ptp_hardware, 1000, 7), {});
    EXPECT_TRUE(alarms->query({.active = true}).alarms.empty());
    const auto history = alarms->query({.active = false, .limit = 20U});
    EXPECT_TRUE(contains_alarm(history, "TIME_SYNC_SOURCE_CHANGED", false));
    EXPECT_TRUE(contains_alarm(history, "TIME_SYNC_WARNING_THRESHOLD_EXCEEDED", false));
    EXPECT_TRUE(contains_alarm(history, "TIME_SYNC_ALARM_THRESHOLD_EXCEEDED", false));
}

TEST(TimeSyncAlarmMonitor, NormalSamplesBreakContinuityAndStopClearsActiveAlarms)
{
    auto alarms = std::make_shared<paperbreak::monitoring::AlarmRegistry>();
    paperbreak::service::TimeSyncAlarmMonitor monitor{alarms,
                                                      {.warning_threshold_ns = 1'000'000,
                                                       .alarm_threshold_ns = 5'000'000,
                                                       .warning_duration = 100ms,
                                                       .alarm_duration = 100ms}};
    const std::vector<std::shared_ptr<const paperbreak::time::ClockModelSnapshot>> cameras{
        model(paperbreak::time::ClockSource::receive_clock, 10'000'000, 2, std::string{"CAM01"})};
    monitor.observe(0, model(paperbreak::time::ClockSource::receive_clock, 10'000'000, 1), cameras);
    monitor.observe(50'000'000, model(paperbreak::time::ClockSource::ptp_hardware, 1000, 3), {});
    monitor.observe(100'000'000, model(paperbreak::time::ClockSource::receive_clock, 10'000'000, 4),
                    {});
    EXPECT_FALSE(contains_alarm(alarms->query({.active = true}),
                                "TIME_SYNC_ALARM_THRESHOLD_EXCEEDED", true));
    monitor.observe(200'000'000, model(paperbreak::time::ClockSource::receive_clock, 10'000'000, 5),
                    {});
    EXPECT_TRUE(contains_alarm(alarms->query({.active = true}),
                               "TIME_SYNC_ALARM_THRESHOLD_EXCEEDED", true));
    monitor.stop();
    EXPECT_TRUE(alarms->query({.active = true}).alarms.empty());
}

TEST(TimeSyncAlarmMonitor, ReconfigureValidatesAndResetsPendingIntervals)
{
    auto alarms = std::make_shared<paperbreak::monitoring::AlarmRegistry>();
    paperbreak::service::TimeSyncAlarmMonitor monitor{alarms};
    EXPECT_FALSE(monitor.reconfigure({.warning_threshold_ns = 5,
                                      .alarm_threshold_ns = 5,
                                      .warning_duration = 1ms,
                                      .alarm_duration = 1ms}));
    EXPECT_TRUE(monitor.reconfigure({.warning_threshold_ns = 10,
                                     .alarm_threshold_ns = 20,
                                     .warning_duration = 0ms,
                                     .alarm_duration = 0ms}));
    monitor.observe(0, model(paperbreak::time::ClockSource::ntp, 25, 1), {});
    EXPECT_TRUE(contains_alarm(alarms->query({.active = true}),
                               "TIME_SYNC_ALARM_THRESHOLD_EXCEEDED", true));
}
