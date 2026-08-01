#include "paperbreak/console/client_state_store.hpp"
#include "paperbreak/ipc/server.hpp"

#include <QCoreApplication>
#include <QEventLoop>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
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
    StatusHandler(std::string state, std::string machine)
        : state_(std::move(state)), machine_(std::move(machine))
    {
    }

    [[nodiscard]] paperbreak::Result<paperbreak::ipc::CommandResponse> handle(
        const paperbreak::ipc::RequestMessage&, const paperbreak::ipc::PeerIdentity&,
        std::stop_token) override
    {
        return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
            {.payload_json = "{\"serviceState\":\"" + state_ + "\",\"machineId\":\"" + machine_ +
                             "\",\"timestamp\":\"2026-08-01T12:00:00.123Z\","
                             "\"acceptingWrites\":true}",
             .binary = {}});
    }

  private:
    std::string state_;
    std::string machine_;
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
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
    auto first = std::make_unique<paperbreak::ipc::IpcServer>(
        std::make_shared<StatusHandler>("running", "EDGE-01"), std::make_unique<StateAuthorizer>(),
        server_options(name));
    ASSERT_TRUE(first->start());

    paperbreak::console::ClientStateSnapshot latest;
    paperbreak::console::ClientStateStore store([&](const auto& snapshot) { latest = snapshot; },
                                                client_options(name));
    ASSERT_TRUE(store.start());
    ASSERT_TRUE(wait_until(
        [&] { return latest.service_status.has_value() && !latest.service_status_stale; }));
    EXPECT_EQ(latest.service_status->service_state, "running");
    EXPECT_EQ(latest.service_status->machine_id, "EDGE-01");
    const std::uint64_t first_generation = latest.service_status->generation;

    stop_server(*first);
    first.reset();
    ASSERT_TRUE(wait_until([&] { return latest.service_status_stale; }));
    ASSERT_TRUE(latest.service_status.has_value());
    EXPECT_EQ(latest.service_status->service_state, "stop-requested");

    auto second = std::make_unique<paperbreak::ipc::IpcServer>(
        std::make_shared<StatusHandler>("degraded", "EDGE-01"), std::make_unique<StateAuthorizer>(),
        server_options(name));
    ASSERT_TRUE(second->start());
    ASSERT_TRUE(wait_until([&] {
        return latest.service_status.has_value() && !latest.service_status_stale &&
               latest.service_status->generation > first_generation;
    }));
    EXPECT_EQ(latest.service_status->service_state, "degraded");

    store.stop();
    stop_server(*second);
}
