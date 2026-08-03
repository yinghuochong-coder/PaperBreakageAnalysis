#include "paperbreak/console/client_state_store.hpp"
#include "paperbreak/console/navigation_model.hpp"
#include "paperbreak/console/tray_status_model.hpp"
#include "paperbreak/ipc/server.hpp"

#include <QCoreApplication>
#include <QEventLoop>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <stop_token>
#include <string>
#include <thread>

namespace
{

class StateAuthorizer final : public paperbreak::ipc::IPeerAuthorizer
{
  public:
    [[nodiscard]] paperbreak::Result<paperbreak::ipc::PeerIdentity> authorize(
        std::uintptr_t) noexcept override
    {
        return paperbreak::Result<paperbreak::ipc::PeerIdentity>::success(
            {.actor_sid = "S-1-5-21-state-test",
             .local = true,
             .authenticated = true,
             .administrator = false});
    }
};

class StatusHandler final : public paperbreak::ipc::IRequestHandler
{
  public:
    StatusHandler(std::string state, std::string machine,
                  const std::uint64_t malformed_metrics_responses = 0U,
                  const bool malformed_locations = false)
        : state_(std::move(state)), machine_(std::move(machine)),
          malformed_metrics_responses_(malformed_metrics_responses),
          malformed_locations_(malformed_locations)
    {
    }

    [[nodiscard]] std::uint64_t metrics_requests() const noexcept
    {
        return metrics_requests_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t alarm_requests() const noexcept
    {
        return alarm_requests_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] paperbreak::Result<paperbreak::ipc::CommandResponse> handle(
        const paperbreak::ipc::RequestMessage& request, const paperbreak::ipc::PeerIdentity&,
        std::stop_token) override
    {
        if (request.command == "system.getVersion")
        {
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json = R"({"applicationVersion":"4.1.0","gitCommit":"abc123"})",
                 .binary = {}});
        }
        if (request.command == "system.getMetrics")
        {
            const std::uint64_t request_number =
                metrics_requests_.fetch_add(1U, std::memory_order_relaxed) + 1U;
            if (request_number <= malformed_metrics_responses_)
            {
                return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                    {.payload_json = R"({"sampledAt":42,"metrics":[]})", .binary = {}});
            }
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json =
                     R"({"sampledAt":"2026-08-03T01:00:00.000Z","metrics":[{"name":"process.cpu.percent","value":12.5,"unit":"percent","available":true},{"name":"system.memory.used_percent","value":48.0,"unit":"percent","available":true},{"name":"disk.event.free_gib","value":512.25,"unit":"GiB","available":true}],"truncated":false})",
                 .binary = {}});
        }
        if (request.command == "alarm.list")
        {
            alarm_requests_.fetch_add(1U, std::memory_order_relaxed);
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json =
                     R"({"registryRevision":3,"alarms":[{"alarmId":7,"revision":2,"code":"DISK_WARNING","severity":"Warning","source":"storage","firstOccurredAt":"2026-08-03T00:59:00.000Z","lastOccurredAt":"2026-08-03T01:00:00.000Z","active":true,"occurrenceCount":2,"message":"事件盘空间偏低","details":{},"acknowledged":false}],"truncated":false,"nextBeforeAlarmId":null})",
                 .binary = {}});
        }
        if (request.command == "system.getLocations")
        {
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json = malformed_locations_ ? R"({"eventRoot":42})"
                                                      : R"({"eventRoot":"C:/PaperBreak/events"})",
                 .binary = {}});
        }
        return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
            {.payload_json = "{\"serviceState\":\"" + state_ + "\",\"machineId\":\"" + machine_ +
                             "\",\"timestamp\":\"2026-08-01T12:00:00.123Z\","
                             "\"acceptingWrites\":true}",
             .binary = {}});
    }

  private:
    std::string state_;
    std::string machine_;
    std::uint64_t malformed_metrics_responses_{};
    bool malformed_locations_{};
    std::atomic_uint64_t metrics_requests_{};
    std::atomic_uint64_t alarm_requests_{};
};

std::string state_name()
{
    static std::atomic_uint64_t sequence{};
    return "PaperBreakEdgeService.Ipc.StateTest." + std::to_string(++sequence);
}

paperbreak::ipc::IpcServerOptions server_options(const std::string& name)
{
    paperbreak::ipc::IpcServerOptions options;
    options.server_name = name;
    const std::wstring suffix{name.begin(), name.end()};
    options.instance_guard_name = L"Local\\" + suffix + L".Guard";
    options.shutdown_flush_timeout = std::chrono::milliseconds{10};
    return options;
}

paperbreak::ipc::IpcClientOptions client_options(const std::string& name)
{
    paperbreak::ipc::IpcClientOptions options;
    options.server_name = name;
    options.connect_timeout = std::chrono::milliseconds{50};
    options.initial_reconnect_delay = std::chrono::milliseconds{10};
    options.maximum_reconnect_delay = std::chrono::milliseconds{40};
    options.reconnect_jitter_fraction = 0.0;
    return options;
}

bool wait_until(const std::function<bool()>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return predicate();
}

void stop_server(paperbreak::ipc::IpcServer& server)
{
    server.request_stop();
    ASSERT_TRUE(server.join(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
}

} // namespace

TEST(ClientStateStore, SynchronizesMarksStaleAndRefreshesAfterReconnect)
{
    const std::string name = state_name();
    auto first_handler = std::make_shared<StatusHandler>("running", "EDGE-01");
    auto first = std::make_unique<paperbreak::ipc::IpcServer>(
        first_handler, std::make_unique<StateAuthorizer>(), server_options(name));
    ASSERT_TRUE(first->start());

    paperbreak::console::ClientStateSnapshot latest;
    paperbreak::console::ClientStateStore store([&](const auto& snapshot) { latest = snapshot; },
                                                client_options(name));
    ASSERT_TRUE(store.start());
    ASSERT_TRUE(wait_until([&] {
        const bool ready = latest.service_status.has_value() && !latest.service_status_stale &&
                           latest.version.has_value() && !latest.version_stale &&
                           latest.metrics.has_value() && !latest.metrics_stale &&
                           latest.alarms.has_value() && !latest.alarms_stale &&
                           latest.locations.has_value() && !latest.locations_stale;
        if (!ready)
        {
            store.refresh_dynamic();
        }
        return ready;
    })) << "connection="
        << static_cast<int>(latest.connection.state)
        << ", status=" << latest.service_status.has_value() << '/' << latest.service_status_stale
        << ", version=" << latest.version.has_value() << '/' << latest.version_stale
        << ", metrics=" << latest.metrics.has_value() << '/' << latest.metrics_stale
        << ", alarms=" << latest.alarms.has_value() << '/' << latest.alarms_stale
        << ", metrics requests=" << first_handler->metrics_requests()
        << ", alarm requests=" << first_handler->alarm_requests()
        << ", server responses=" << first->metrics_snapshot().responses_total
        << ", outbound=" << first->metrics_snapshot().outbound_messages;
    EXPECT_EQ(latest.service_status->service_state, "running");
    EXPECT_EQ(latest.service_status->machine_id, "EDGE-01");
    EXPECT_EQ(latest.version->application_version, "4.1.0");
    EXPECT_DOUBLE_EQ(latest.metrics->process_cpu_percent.value(), 12.5);
    EXPECT_DOUBLE_EQ(latest.metrics->system_memory_used_percent.value(), 48.0);
    EXPECT_DOUBLE_EQ(latest.metrics->event_disk_free_gib.value(), 512.25);
    ASSERT_EQ(latest.alarms->recent.size(), 1U);
    EXPECT_EQ(latest.alarms->recent.front().message, "事件盘空间偏低");
    EXPECT_EQ(latest.alarms->highest_severity, "Warning");
    EXPECT_EQ(latest.locations->event_root, "C:/PaperBreak/events");
    const std::uint64_t first_generation = latest.service_status->generation;
    const std::uint64_t metrics_before_burst = first_handler->metrics_requests();
    const std::uint64_t alarms_before_burst = first_handler->alarm_requests();

    for (int iteration = 0; iteration < 20; ++iteration)
    {
        store.refresh_dynamic();
    }
    ASSERT_TRUE(wait_until([&] {
        return first_handler->metrics_requests() > metrics_before_burst &&
               first_handler->alarm_requests() > alarms_before_burst;
    }));
    EXPECT_EQ(first_handler->metrics_requests(), metrics_before_burst + 1U);
    EXPECT_EQ(first_handler->alarm_requests(), alarms_before_burst + 1U);

    ASSERT_TRUE(first->try_publish({.event_name = "alarm.raised",
                                    .timestamp = "2026-08-03T01:00:01.000Z",
                                    .payload_json = "{}",
                                    .binary = {},
                                    .coalescing_key = "alarm.raised"}));
    ASSERT_TRUE(
        wait_until([&] { return first_handler->alarm_requests() >= alarms_before_burst + 2U; }));

    stop_server(*first);
    first.reset();
    ASSERT_TRUE(wait_until([&] { return latest.service_status_stale; }));
    ASSERT_TRUE(latest.service_status.has_value());
    EXPECT_EQ(latest.service_status->service_state, "stop-requested");
    EXPECT_TRUE(latest.version_stale);
    EXPECT_TRUE(latest.metrics_stale);
    EXPECT_TRUE(latest.alarms_stale);
    EXPECT_TRUE(latest.locations_stale);

    auto second = std::make_unique<paperbreak::ipc::IpcServer>(
        std::make_shared<StatusHandler>("degraded", "EDGE-01"), std::make_unique<StateAuthorizer>(),
        server_options(name));
    ASSERT_TRUE(second->start());
    ASSERT_TRUE(wait_until([&] {
        const bool ready = latest.service_status.has_value() && !latest.service_status_stale &&
                           latest.service_status->generation > first_generation;
        if (!ready)
        {
            store.refresh_dynamic();
        }
        return ready;
    }));
    EXPECT_EQ(latest.service_status->service_state, "degraded");

    store.stop();
    stop_server(*second);
}

TEST(ClientStateStore, InvalidMetricsDoNotInvalidateOtherDataAndCanRecover)
{
    const std::string name = state_name();
    auto handler = std::make_shared<StatusHandler>("running", "EDGE-02", 1U);
    paperbreak::ipc::IpcServer server(handler, std::make_unique<StateAuthorizer>(),
                                      server_options(name));
    ASSERT_TRUE(server.start());

    paperbreak::console::ClientStateSnapshot latest;
    bool saw_metrics_protocol_error = false;
    paperbreak::console::ClientStateStore store(
        [&](const auto& snapshot) {
            latest = snapshot;
            saw_metrics_protocol_error =
                saw_metrics_protocol_error ||
                (snapshot.metrics_error.has_value() &&
                 snapshot.metrics_error->business_code == "IPC_PROTOCOL_ERROR");
        },
        client_options(name));
    ASSERT_TRUE(store.start());
    ASSERT_TRUE(wait_until([&] {
        const bool ready = latest.service_status.has_value() && !latest.service_status_stale &&
                           latest.version.has_value() && !latest.version_stale &&
                           latest.alarms.has_value() && !latest.alarms_stale &&
                           latest.metrics.has_value() && !latest.metrics_stale;
        if (!ready)
        {
            store.refresh_dynamic();
        }
        return ready;
    })) << "connection="
        << static_cast<int>(latest.connection.state)
        << ", status=" << latest.service_status.has_value() << '/' << latest.service_status_stale
        << ", version=" << latest.version.has_value() << '/' << latest.version_stale
        << ", metrics=" << latest.metrics.has_value() << '/' << latest.metrics_stale
        << ", alarms=" << latest.alarms.has_value() << '/' << latest.alarms_stale
        << ", metrics requests=" << handler->metrics_requests()
        << ", alarm requests=" << handler->alarm_requests()
        << ", server responses=" << server.metrics_snapshot().responses_total
        << ", outbound=" << server.metrics_snapshot().outbound_messages;
    EXPECT_TRUE(saw_metrics_protocol_error);
    EXPECT_TRUE(latest.metrics.has_value());
    EXPECT_FALSE(latest.synchronization_error.has_value());
    EXPECT_FALSE(latest.version_error.has_value());
    EXPECT_FALSE(latest.alarms_error.has_value());

    EXPECT_FALSE(latest.metrics_error.has_value());

    store.stop();
    stop_server(server);
}

TEST(ClientStateStore, InvalidLocationsStayStaleWithoutInvalidatingStatus)
{
    const std::string name = state_name();
    auto handler = std::make_shared<StatusHandler>("running", "EDGE-03", 0U, true);
    paperbreak::ipc::IpcServer server(handler, std::make_unique<StateAuthorizer>(),
                                      server_options(name));
    ASSERT_TRUE(server.start());

    paperbreak::console::ClientStateSnapshot latest;
    paperbreak::console::ClientStateStore store([&](const auto& snapshot) { latest = snapshot; },
                                                client_options(name));
    ASSERT_TRUE(store.start());
    ASSERT_TRUE(wait_until([&] {
        return latest.service_status.has_value() && !latest.service_status_stale &&
               latest.locations_error.has_value();
    }));
    EXPECT_TRUE(latest.locations_stale);
    EXPECT_FALSE(latest.locations.has_value());
    EXPECT_EQ(latest.locations_error->business_code, "IPC_PROTOCOL_ERROR");
    EXPECT_EQ(latest.service_status->service_state, "running");

    store.stop();
    stop_server(server);
}

TEST(ConsoleNavigationModel, DefinesStableUniquePageOrder)
{
    const auto pages = paperbreak::console::console_pages();
    ASSERT_EQ(pages.size(), 12U);
    EXPECT_EQ(paperbreak::console::default_console_page_index(), 0U);
    EXPECT_EQ(pages.front().id, paperbreak::console::ConsolePageId::overview);
    EXPECT_EQ(pages.front().title, "总览");
    EXPECT_EQ(pages.back().id, paperbreak::console::ConsolePageId::maintenance);

    std::set<std::string_view> keys;
    for (const auto& page : pages)
    {
        EXPECT_TRUE(keys.insert(page.key).second);
        ASSERT_TRUE(paperbreak::console::console_page_index(page.id).has_value());
        EXPECT_EQ(pages[paperbreak::console::console_page_index(page.id).value()].key, page.key);
    }
}

TEST(ConsoleTrayStatusModel, MapsConnectionServiceAndAlarmPriority)
{
    using paperbreak::console::TrayStatusColor;
    paperbreak::console::ClientStateSnapshot snapshot;
    snapshot.connection.state = paperbreak::ipc::ClientConnectionState::retry_wait;
    snapshot.service_status = paperbreak::console::ServiceStatusSummary{.service_state = "running"};
    snapshot.service_status_stale = false;
    snapshot.alarms = paperbreak::console::AlarmOverviewSummary{};
    snapshot.alarms_stale = false;
    EXPECT_EQ(paperbreak::console::tray_status(snapshot).color, TrayStatusColor::gray);

    snapshot.connection.state = paperbreak::ipc::ClientConnectionState::connected;
    snapshot.service_status_stale = true;
    EXPECT_EQ(paperbreak::console::tray_status(snapshot).color, TrayStatusColor::gray);

    snapshot.service_status_stale = false;
    snapshot.service_status->service_state = "starting";
    EXPECT_EQ(paperbreak::console::tray_status(snapshot).color, TrayStatusColor::red);

    snapshot.service_status->service_state = "running";
    snapshot.alarms_stale = true;
    EXPECT_EQ(paperbreak::console::tray_status(snapshot).color, TrayStatusColor::gray);

    snapshot.alarms_stale = false;
    snapshot.alarms->highest_severity = "Info";
    EXPECT_EQ(paperbreak::console::tray_status(snapshot).color, TrayStatusColor::green);
    snapshot.alarms->highest_severity = "Warning";
    EXPECT_EQ(paperbreak::console::tray_status(snapshot).color, TrayStatusColor::yellow);
    snapshot.alarms->highest_severity = "Error";
    EXPECT_EQ(paperbreak::console::tray_status(snapshot).color, TrayStatusColor::red);
    snapshot.alarms->highest_severity = "Critical";
    EXPECT_EQ(paperbreak::console::tray_status(snapshot).color, TrayStatusColor::red);
}
