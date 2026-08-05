#include "paperbreak/common/error.hpp"
#include "paperbreak/uplink/mock_transport.hpp"
#include "paperbreak/uplink/runtime.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using Json = nlohmann::json;

template <typename Predicate>
bool wait_until(Predicate predicate, const std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

paperbreak::uplink::UplinkRuntimeConfig runtime_config(std::vector<std::string> capabilities = {
                                                           "system.requestStatus"})
{
    return {.session_hello = {.request_id = "session-runtime-1",
                              .machine_id = "EDGE-TEST",
                              .production_line_id = "PM-TEST",
                              .software_version = "0.1.0",
                              .supported_protocol_versions = {1U},
                              .capabilities = std::move(capabilities)},
            .initial_reconnect_delay = 10ms,
            .maximum_reconnect_delay = 40ms,
            .command_queue_capacity = 8U,
            .command_deduplication_capacity = 16U};
}

paperbreak::uplink::MessageEnvelope command_envelope(
    std::string command_id, std::string command_type, Json body = Json::object(),
    const bool confirmed = true, std::string deadline = "2999-01-01T00:00:00.000Z")
{
    const std::string message_id = "message-" + command_id;
    return {.protocol_version = paperbreak::uplink::protocol_version,
            .message_type = "command",
            .message_id = message_id,
            .machine_id = "EDGE-TEST",
            .sequence = 1U,
            .timestamp = paperbreak::current_utc_timestamp(),
            .payload_json = Json{{"commandId", std::move(command_id)},
                                 {"commandType", std::move(command_type)},
                                 {"deadline", std::move(deadline)},
                                 {"operatorConfirmed", confirmed},
                                 {"body", std::move(body)}}
                                .dump()};
}

struct RuntimeFixture final
{
    explicit RuntimeFixture(paperbreak::uplink::UplinkRuntimeConfig config = runtime_config(),
                            paperbreak::uplink::RemoteCommandExecutor executor = {})
    {
        auto created =
            paperbreak::uplink::mock::MockUplinkTransport::create({.fault_capacity = 32U,
                                                                   .history_capacity = 256U,
                                                                   .maximum_delay = 100ms,
                                                                   .heartbeat_seconds = 1U});
        if (!created)
            throw std::runtime_error{created.error().message};
        transport = std::shared_ptr<paperbreak::uplink::mock::MockUplinkTransport>{
            std::move(created).value()};
        if (!executor)
            executor = [](const paperbreak::uplink::RemoteCommand& command, std::stop_token) {
                return paperbreak::Result<std::string>::success(
                    Json{{"commandId", command.command_id}, {"accepted", true}}.dump());
            };
        auto runtime_created = paperbreak::uplink::UplinkRuntime::create(
            transport, std::move(config),
            [] { return paperbreak::Result<std::string>::success(R"({"service":"running"})"); },
            std::move(executor));
        if (!runtime_created)
            throw std::runtime_error{runtime_created.error().message};
        runtime = std::move(runtime_created).value();
    }

    ~RuntimeFixture()
    {
        runtime->request_stop();
        EXPECT_TRUE(runtime->join(std::chrono::steady_clock::now() + 2s));
    }

    void start_and_wait_connected()
    {
        ASSERT_TRUE(runtime->start());
        ASSERT_TRUE(wait_until([&] {
            return runtime->snapshot().state == paperbreak::uplink::UplinkRuntimeState::connected;
        }));
    }

    std::shared_ptr<paperbreak::uplink::mock::MockUplinkTransport> transport;
    std::unique_ptr<paperbreak::uplink::UplinkRuntime> runtime;
};

TEST(UplinkRuntime, ValidatesBoundedConfigurationAndSessionV1)
{
    auto transport_result = paperbreak::uplink::mock::MockUplinkTransport::create();
    ASSERT_TRUE(transport_result);
    std::shared_ptr<paperbreak::uplink::IUplinkTransport> transport{
        std::move(transport_result).value()};
    auto config = runtime_config();
    config.command_queue_capacity = 0U;
    auto invalid = paperbreak::uplink::UplinkRuntime::create(
        transport, config, [] { return paperbreak::Result<std::string>::success("{}"); },
        [](const paperbreak::uplink::RemoteCommand&, std::stop_token) {
            return paperbreak::Result<std::string>::success("{}");
        });
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().business_code, "SYS_CONFIG_INVALID");

    config = runtime_config();
    config.session_hello.supported_protocol_versions = {2U};
    invalid = paperbreak::uplink::UplinkRuntime::create(
        transport, config, [] { return paperbreak::Result<std::string>::success("{}"); },
        [](const paperbreak::uplink::RemoteCommand&, std::stop_token) {
            return paperbreak::Result<std::string>::success("{}");
        });
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().business_code, "SYS_CONFIG_INVALID");
}

TEST(UplinkRuntime, ConnectsAndPublishesImmediateStatusAndPeriodicHeartbeat)
{
    RuntimeFixture fixture;
    fixture.start_and_wait_connected();
    ASSERT_TRUE(wait_until([&] { return fixture.runtime->snapshot().statuses_sent >= 1U; }));
    ASSERT_TRUE(
        wait_until([&] { return fixture.runtime->snapshot().heartbeats_sent >= 1U; }, 1500ms));

    const auto snapshot = fixture.runtime->snapshot();
    EXPECT_EQ(snapshot.successful_connections, 1U);
    EXPECT_GE(snapshot.statuses_sent, 2U);
    EXPECT_FALSE(snapshot.last_heartbeat_at.empty());
    const auto history = fixture.transport->history();
    EXPECT_NE(std::ranges::find_if(history,
                                   [](const auto& call) {
                                       return call.operation ==
                                              paperbreak::uplink::UplinkOperation::heartbeat;
                                   }),
              history.end());
}

TEST(UplinkRuntime, UsesCappedExponentialBackoffAndReconnectsAfterRecovery)
{
    RuntimeFixture fixture;
    fixture.transport->set_online(false);
    ASSERT_TRUE(fixture.runtime->start());
    ASSERT_TRUE(wait_until([&] { return fixture.runtime->snapshot().reconnect_failures >= 3U; }));
    const auto offline = fixture.runtime->snapshot();
    EXPECT_EQ(offline.state, paperbreak::uplink::UplinkRuntimeState::backing_off);
    EXPECT_LE(offline.current_reconnect_delay, 40ms);
    EXPECT_GE(offline.reconnect_failures, 3U);

    fixture.transport->set_online(true);
    ASSERT_TRUE(
        wait_until([&] { return fixture.runtime->snapshot().successful_connections >= 1U; }));
    EXPECT_EQ(fixture.runtime->snapshot().state, paperbreak::uplink::UplinkRuntimeState::connected);
}

TEST(UplinkRuntime, ExecutesOnceReplaysDuplicateAndRejectsConflictingContent)
{
    std::atomic_uint64_t executions{};
    RuntimeFixture fixture{runtime_config({"camera.start"}),
                           [&](const paperbreak::uplink::RemoteCommand&, std::stop_token) {
                               ++executions;
                               return paperbreak::Result<std::string>::success(
                                   R"({"started":true})");
                           }};
    fixture.start_and_wait_connected();

    const auto first = command_envelope("command-1", "camera.start", {{"cameraId", "CAM01"}});
    ASSERT_TRUE(fixture.transport->inject_command(first));
    ASSERT_TRUE(fixture.transport->inject_command(first));
    ASSERT_TRUE(wait_until([&] { return fixture.runtime->snapshot().commands_replayed == 1U; }));
    EXPECT_EQ(executions.load(), 1U);

    ASSERT_TRUE(fixture.transport->inject_command(
        command_envelope("command-1", "camera.start", {{"cameraId", "CAM02"}})));
    ASSERT_TRUE(wait_until([&] { return fixture.runtime->snapshot().commands_conflicted == 1U; }));
    EXPECT_EQ(executions.load(), 1U);
    EXPECT_EQ(fixture.runtime->snapshot().retained_command_results, 1U);
}

TEST(UplinkRuntime, RejectsUnconfirmedExpiredAndUndeclaredCommandsWithoutExecution)
{
    std::atomic_uint64_t executions{};
    RuntimeFixture fixture{runtime_config({"camera.start"}),
                           [&](const paperbreak::uplink::RemoteCommand&, std::stop_token) {
                               ++executions;
                               return paperbreak::Result<std::string>::success("{}");
                           }};
    fixture.start_and_wait_connected();
    ASSERT_TRUE(fixture.transport->inject_command(
        command_envelope("unconfirmed", "camera.start", Json::object(), false)));
    ASSERT_TRUE(fixture.transport->inject_command(command_envelope(
        "expired", "camera.start", Json::object(), true, "2000-01-01T00:00:00.000Z")));
    ASSERT_TRUE(fixture.transport->inject_command(
        command_envelope("undeclared", "camera.stop", Json::object(), true)));
    ASSERT_TRUE(
        wait_until([&] { return fixture.runtime->snapshot().retained_command_results >= 3U; }));
    EXPECT_EQ(executions.load(), 0U);
    EXPECT_EQ(fixture.runtime->snapshot().commands_rejected, 3U);
    EXPECT_EQ(fixture.runtime->snapshot().retained_command_results, 3U);
}

TEST(UplinkRuntime, RejectsNewestWhenCommandQueueIsFullAndStopsDeterministically)
{
    std::atomic_bool entered{};
    std::atomic_bool release{};
    auto config = runtime_config({"camera.start"});
    config.command_queue_capacity = 2U;
    RuntimeFixture fixture{std::move(config),
                           [&](const paperbreak::uplink::RemoteCommand&, std::stop_token token) {
                               entered = true;
                               while (!release.load() && !token.stop_requested())
                                   std::this_thread::sleep_for(1ms);
                               return paperbreak::Result<std::string>::success("{}");
                           }};
    fixture.start_and_wait_connected();
    ASSERT_TRUE(fixture.transport->inject_command(command_envelope("blocking", "camera.start")));
    ASSERT_TRUE(wait_until([&] { return entered.load(); }));
    ASSERT_TRUE(fixture.transport->inject_command(command_envelope("queued-1", "camera.start")));
    ASSERT_TRUE(fixture.transport->inject_command(command_envelope("queued-2", "camera.start")));
    ASSERT_TRUE(fixture.transport->inject_command(command_envelope("rejected", "camera.start")));
    ASSERT_TRUE(
        wait_until([&] { return fixture.runtime->snapshot().command_queue_rejections == 1U; }));
    EXPECT_EQ(fixture.runtime->snapshot().command_queue_high_watermark, 2U);
    release = true;
    ASSERT_TRUE(wait_until([&] { return fixture.runtime->snapshot().commands_executed == 3U; }));
}

} // namespace
