#include "paperbreak/monitoring/monitoring.hpp"
#include "paperbreak/platform/system_metrics.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace std::chrono_literals;

class MutableMetricSource final : public paperbreak::monitoring::IMetricSource
{
  public:
    [[nodiscard]] std::string_view source_name() const noexcept override
    {
        return "mock";
    }

    [[nodiscard]] paperbreak::Result<std::vector<paperbreak::monitoring::MetricPoint>> collect(
        std::stop_token) noexcept override
    {
        samples.fetch_add(1U, std::memory_order_relaxed);
        if (fail.load(std::memory_order_relaxed))
        {
            return paperbreak::Result<std::vector<paperbreak::monitoring::MetricPoint>>::failure(
                paperbreak::make_error("SYS_MONITORING_SAMPLE_FAILED",
                                       paperbreak::Severity::warning, "mock failure", "test",
                                       "test.collect"));
        }
        return paperbreak::Result<std::vector<paperbreak::monitoring::MetricPoint>>::success(
            {{.name = "process.cpu.percent",
              .value = cpu.load(std::memory_order_relaxed),
              .unit = "percent"},
             {.name = "system.memory.used_percent",
              .value = memory.load(std::memory_order_relaxed),
              .unit = "percent"},
             {.name = "disk.event.free_gib",
              .value = disk.load(std::memory_order_relaxed),
              .unit = "GiB"}});
    }

    std::atomic<double> cpu{10.0};
    std::atomic<double> memory{20.0};
    std::atomic<double> disk{500.0};
    std::atomic_bool fail{false};
    std::atomic_uint64_t samples{0U};
};

bool wait_until(const std::function<bool()>& predicate,
                const std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

} // namespace

TEST(MonitoringMetrics, ReplacesSourcesFiltersAndEnforcesCapacity)
{
    paperbreak::monitoring::MetricRegistry registry{3U};
    ASSERT_TRUE(registry.replace_source(
        "system", {{.name = "process.cpu.percent", .value = 12.5, .unit = "percent"},
                   {.name = "system.memory.used_percent", .value = 50.0, .unit = "percent"}}));
    ASSERT_TRUE(registry.replace_source(
        "ipc", {{.name = "ipc.connections.active", .value = std::uint64_t{1U}, .unit = "count"}}));

    const auto filtered = registry.query({.prefixes = {"ipc."}, .limit = 10U});
    ASSERT_EQ(filtered.snapshot.metrics.size(), 1U);
    EXPECT_EQ(filtered.snapshot.metrics.front().name, "ipc.connections.active");

    ASSERT_TRUE(registry.replace_source(
        "system", {{.name = "process.cpu.percent", .value = 22.0, .unit = "percent"}}));
    EXPECT_EQ(registry.size(), 2U);
    auto overflow =
        registry.replace_source("extra", {{.name = "extra.one", .value = true, .unit = "boolean"},
                                          {.name = "extra.two", .value = true, .unit = "boolean"}});
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().business_code, "MONITORING_CAPACITY_EXCEEDED");
}

TEST(MonitoringMetrics, BoundsSourcesAndRedactsStringValues)
{
    paperbreak::monitoring::MetricRegistry registry{8U, 1U};
    ASSERT_TRUE(registry.replace_source(
        "database",
        {{.name = "database.state", .value = std::string{"token=secret"}, .unit = "state"}}));
    const auto snapshot = registry.query();
    ASSERT_EQ(snapshot.snapshot.metrics.size(), 1U);
    EXPECT_EQ(std::get<std::string>(snapshot.snapshot.metrics.front().value), "token=***");

    const auto overflow = registry.replace_source(
        "ipc", {{.name = "ipc.connections.active", .value = std::uint64_t{0U}, .unit = "count"}});
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().business_code, "MONITORING_CAPACITY_EXCEEDED");
}

TEST(MonitoringMetrics, SupportsConcurrentSourceReplacementAndSnapshots)
{
    paperbreak::monitoring::MetricRegistry registry;
    std::atomic_bool failed{false};
    std::jthread writer([&](std::stop_token) {
        for (std::uint64_t index = 0; index < 500U; ++index)
        {
            if (!registry.replace_source(
                    "ipc", {{.name = "ipc.requests.total", .value = index, .unit = "count"}}))
            {
                failed.store(true, std::memory_order_relaxed);
            }
        }
    });
    std::jthread reader([&](std::stop_token) {
        for (int index = 0; index < 500; ++index)
        {
            const auto result = registry.query({.prefixes = {"ipc."}, .limit = 1U});
            if (result.snapshot.metrics.size() > 1U)
            {
                failed.store(true, std::memory_order_relaxed);
            }
        }
    });
    writer.join();
    reader.join();
    EXPECT_FALSE(failed.load(std::memory_order_relaxed));
    EXPECT_GT(registry.query().snapshot.version, 0U);
}

TEST(MonitoringAlarm, MergesAcknowledgesClearsAndStartsANewLifecycle)
{
    paperbreak::monitoring::AlarmRegistry registry{2U, 2U};
    std::vector<paperbreak::monitoring::AlarmChangeKind> changes;
    registry.set_observer([&changes](const paperbreak::monitoring::AlarmChange& change) {
        changes.push_back(change.kind);
    });

    auto first = registry.raise_alarm({.code = "CAMERA_DISCONNECTED",
                                       .severity = paperbreak::Severity::warning,
                                       .source = "CAM01",
                                       .message = "offline"});
    ASSERT_TRUE(first);
    auto duplicate = registry.raise_alarm({.code = "CAMERA_DISCONNECTED",
                                           .severity = paperbreak::Severity::error,
                                           .source = "CAM01",
                                           .message = "still offline"});
    ASSERT_TRUE(duplicate);
    EXPECT_EQ(duplicate.value().alarm_id, first.value().alarm_id);
    EXPECT_EQ(duplicate.value().occurrence_count, 2U);

    auto acknowledged = registry.acknowledge(first.value().alarm_id);
    ASSERT_TRUE(acknowledged);
    EXPECT_TRUE(acknowledged.value().acknowledged);
    auto reraised = registry.raise_alarm({.code = "CAMERA_DISCONNECTED",
                                          .severity = paperbreak::Severity::error,
                                          .source = "CAM01",
                                          .message = "again"});
    ASSERT_TRUE(reraised);
    EXPECT_FALSE(reraised.value().acknowledged);

    auto cleared = registry.clear("CAMERA_DISCONNECTED", "CAM01");
    ASSERT_TRUE(cleared);
    ASSERT_TRUE(cleared.value().has_value());
    EXPECT_FALSE(cleared.value()->active);
    auto next = registry.raise_alarm({.code = "CAMERA_DISCONNECTED",
                                      .severity = paperbreak::Severity::warning,
                                      .source = "CAM01",
                                      .message = "new incident"});
    ASSERT_TRUE(next);
    EXPECT_NE(next.value().alarm_id, first.value().alarm_id);
    EXPECT_EQ(next.value().occurrence_count, 1U);
    EXPECT_EQ(changes.back(), paperbreak::monitoring::AlarmChangeKind::raised);
}

TEST(MonitoringAlarm, BoundsHistoryAndSupportsConcurrentSnapshots)
{
    paperbreak::monitoring::AlarmRegistry registry{8U, 1U};
    for (int index = 0; index < 3; ++index)
    {
        const std::string source = "source-" + std::to_string(index);
        ASSERT_TRUE(registry.raise_alarm({.code = "SYS_TIME_JUMP_DETECTED",
                                          .severity = paperbreak::Severity::warning,
                                          .source = source,
                                          .message = "clock"}));
        ASSERT_TRUE(registry.clear("SYS_TIME_JUMP_DETECTED", source));
    }
    EXPECT_EQ(registry.query().alarms.size(), 1U);

    std::atomic_bool failed{false};
    std::jthread writer([&](std::stop_token) {
        for (int index = 0; index < 100; ++index)
        {
            auto raised = registry.raise_alarm({.code = "SYS_CPU_USAGE_HIGH",
                                                .severity = paperbreak::Severity::warning,
                                                .source = "process",
                                                .message = "high"});
            if (!raised)
                failed.store(true);
        }
    });
    std::jthread reader([&](std::stop_token) {
        for (int index = 0; index < 100; ++index)
        {
            if (registry.query({.active = true, .limit = 10U}).alarms.size() > 1U)
                failed.store(true);
        }
    });
    writer.join();
    reader.join();
    EXPECT_FALSE(failed.load());
}

TEST(MonitoringAlarm, EnforcesActiveCapacityAndAcknowledgesHistoryIdempotently)
{
    paperbreak::monitoring::AlarmRegistry registry{1U, 2U};
    auto first = registry.raise_alarm({.code = "SYS_CPU_USAGE_HIGH",
                                       .severity = paperbreak::Severity::warning,
                                       .source = "process",
                                       .message = "high"});
    ASSERT_TRUE(first);
    const auto overflow = registry.raise_alarm({.code = "SYS_MEMORY_USAGE_HIGH",
                                                .severity = paperbreak::Severity::warning,
                                                .source = "system",
                                                .message = "high"});
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().business_code, "MONITORING_CAPACITY_EXCEEDED");
    ASSERT_TRUE(registry.clear("SYS_CPU_USAGE_HIGH", "process"));

    auto acknowledged = registry.acknowledge(first.value().alarm_id);
    ASSERT_TRUE(acknowledged);
    EXPECT_TRUE(acknowledged.value().acknowledged);
    const auto revision = acknowledged.value().revision;
    acknowledged = registry.acknowledge(first.value().alarm_id);
    ASSERT_TRUE(acknowledged);
    EXPECT_EQ(acknowledged.value().revision, revision);
}

TEST(HealthMonitor, SamplesThresholdsFailureRecoveryAndStopsDeterministically)
{
    auto metrics = std::make_shared<paperbreak::monitoring::MetricRegistry>();
    auto alarms = std::make_shared<paperbreak::monitoring::AlarmRegistry>();
    paperbreak::monitoring::HealthMonitorOptions options;
    options.sample_interval = 100ms;
    options.cpu_warning_percent = 80.0;
    options.memory_warning_percent = 80.0;
    options.disks = {{.metric_name = "disk.event.free_gib",
                      .source = "event",
                      .warning_free_gib = 200.0,
                      .critical_free_gib = 100.0,
                      .stop_free_gib = 20.0}};
    paperbreak::monitoring::HealthMonitor monitor{metrics, alarms, options};
    auto source = std::make_shared<MutableMetricSource>();
    ASSERT_TRUE(monitor.register_source(source));
    ASSERT_TRUE(monitor.start());
    ASSERT_TRUE(wait_until([&] { return source->samples.load() >= 1U; }));

    source->cpu.store(95.0);
    source->memory.store(90.0);
    source->disk.store(50.0);
    ASSERT_TRUE(monitor.reconfigure(options));
    ASSERT_TRUE(wait_until(
        [&] { return alarms->query({.active = true, .limit = 10U}).alarms.size() == 3U; }));

    source->cpu.store(10.0);
    source->memory.store(20.0);
    source->disk.store(500.0);
    ASSERT_TRUE(monitor.reconfigure(options));
    ASSERT_TRUE(
        wait_until([&] { return alarms->query({.active = true, .limit = 10U}).alarms.empty(); }));

    source->fail.store(true);
    ASSERT_TRUE(monitor.reconfigure(options));
    ASSERT_TRUE(wait_until(
        [&] { return alarms->query({.active = true, .limit = 10U}).alarms.size() == 1U; }));
    source->fail.store(false);
    ASSERT_TRUE(monitor.reconfigure(options));
    ASSERT_TRUE(
        wait_until([&] { return alarms->query({.active = true, .limit = 10U}).alarms.empty(); }));

    const auto before_stop = source->samples.load();
    monitor.request_stop();
    ASSERT_TRUE(monitor.join(std::chrono::steady_clock::now() + 1s));
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(source->samples.load(), before_stop);
}

TEST(HealthMonitor, AppliesIntervalAndThresholdChangesImmediately)
{
    auto metrics = std::make_shared<paperbreak::monitoring::MetricRegistry>();
    auto alarms = std::make_shared<paperbreak::monitoring::AlarmRegistry>();
    paperbreak::monitoring::HealthMonitorOptions options;
    options.sample_interval = 1s;
    options.cpu_warning_percent = 99.0;
    options.memory_warning_percent = 99.0;
    paperbreak::monitoring::HealthMonitor monitor{metrics, alarms, options};
    auto source = std::make_shared<MutableMetricSource>();
    source->cpu.store(90.0);
    ASSERT_TRUE(monitor.register_source(source));
    ASSERT_TRUE(monitor.start());
    ASSERT_TRUE(wait_until([&] { return source->samples.load() >= 1U; }));
    EXPECT_TRUE(alarms->query({.active = true}).alarms.empty());

    options.sample_interval = 100ms;
    options.cpu_warning_percent = 80.0;
    ASSERT_TRUE(monitor.reconfigure(options));
    ASSERT_TRUE(
        wait_until([&] { return !alarms->query({.active = true, .limit = 10U}).alarms.empty(); }));
    const auto first_count = source->samples.load();
    ASSERT_TRUE(wait_until([&] { return source->samples.load() > first_count; }, 500ms));

    monitor.request_stop();
    ASSERT_TRUE(monitor.join(std::chrono::steady_clock::now() + 1s));
}

TEST(MonitoringWindows, ReturnsBoundedProcessSystemAndDiskSnapshotWithoutHardware)
{
    auto source = paperbreak::platform::make_windows_system_metric_source(
        {{.label = "temp", .path = std::filesystem::temp_directory_path()}});
    auto first = source->collect({});
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_GE(first.value().size(), 9U);
    EXPECT_NE(std::find_if(first.value().begin(), first.value().end(),
                           [](const auto& point) {
                               return point.name == "process.memory.working_set_bytes";
                           }),
              first.value().end());
    EXPECT_NE(std::find_if(first.value().begin(), first.value().end(),
                           [](const auto& point) {
                               return point.name == "disk.temp.free_gib" && point.available;
                           }),
              first.value().end());
}
