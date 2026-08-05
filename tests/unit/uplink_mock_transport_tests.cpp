#include "paperbreak/uplink/mock_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace
{
using namespace std::chrono_literals;
using namespace paperbreak::uplink;
using namespace paperbreak::uplink::mock;

SessionHello hello(const std::string& request_id = "session-001")
{
    return {.request_id = request_id,
            .machine_id = "EDGE-01",
            .production_line_id = "LINE-01",
            .software_version = "0.1.0",
            .supported_protocol_versions = {protocol_version},
            .capabilities = {"system.requestStatus"}};
}

MessageEnvelope message(const std::string& type, const std::string& id)
{
    return {.protocol_version = protocol_version,
            .message_type = type,
            .message_id = id,
            .machine_id = "EDGE-01",
            .sequence = 1U,
            .timestamp = "2026-08-05T01:02:03.004Z",
            .payload_json = R"({"state":"ok"})"};
}

EventMetadataRequest event_request(const std::string& id = "event-request-001")
{
    return {.request_id = id,
            .machine_id = "EDGE-01",
            .event_id = "01989abc-def0-7000-8000-000000000001",
            .metadata_json = R"({"eventId":"01989abc-def0-7000-8000-000000000001"})"};
}

UploadFileRequest upload_request(const std::string& id = "upload-request-001")
{
    return {.machine_id = "EDGE-01",
            .description = {.request_id = id,
                            .event_id = "01989abc-def0-7000-8000-000000000001",
                            .logical_file_id = "manifest",
                            .file_name = "manifest.json",
                            .content_type = "application/json",
                            .total_bytes = 1024U,
                            .chunk_bytes = 1024U,
                            .sha256 = std::string(64U, 'a')},
            .source_path = R"(C:\events\manifest.json)"};
}

std::unique_ptr<MockUplinkTransport> connected_mock(MockUplinkConfig config = {})
{
    auto created = MockUplinkTransport::create(config);
    EXPECT_TRUE(created);
    if (!created)
        return {};
    auto transport = std::move(created).value();
    IUplinkTransport& port = *transport;
    auto connected = port.connect(hello());
    EXPECT_TRUE(connected);
    return transport;
}
} // namespace

TEST(UplinkMockTransport, ValidatesBoundedConfigurationAndProtocolVersion)
{
    auto invalid = MockUplinkTransport::create({.fault_capacity = 0U});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().business_code, "UPLINK_PROTOCOL_ERROR");

    invalid = MockUplinkTransport::create(
        {.fault_capacity = 1U, .history_capacity = 1U, .maximum_delay = 10001ms});
    ASSERT_FALSE(invalid);

    auto created = MockUplinkTransport::create();
    ASSERT_TRUE(created);
    auto transport = std::move(created).value();
    auto unsupported = hello();
    unsupported.supported_protocol_versions = {2U};
    auto result = transport->connect(unsupported);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "UPLINK_PROTOCOL_VERSION_UNSUPPORTED");
    EXPECT_EQ(transport->connection_state(), UplinkConnectionState::disconnected);
}

TEST(UplinkMockTransport, ImplementsTransportPortAndBoundsCallHistory)
{
    auto transport = connected_mock({.fault_capacity = 4U, .history_capacity = 2U});
    ASSERT_NE(transport, nullptr);
    IUplinkTransport& port = *transport;

    ASSERT_TRUE(port.send_heartbeat(message("heartbeat", "heartbeat-001")));
    ASSERT_TRUE(port.send_event_metadata(event_request()));
    ASSERT_TRUE(port.upload_file(upload_request()));

    const auto history = transport->history();
    ASSERT_EQ(history.size(), 2U);
    EXPECT_EQ(history.front().operation, UplinkOperation::event_metadata);
    EXPECT_EQ(history.back().operation, UplinkOperation::upload_file);
    const auto snapshot = transport->snapshot();
    EXPECT_EQ(snapshot.calls, 4U);
    EXPECT_EQ(snapshot.successes, 4U);
    EXPECT_EQ(snapshot.retained_history, 2U);

    port.disconnect();
    EXPECT_EQ(port.connection_state(), UplinkConnectionState::disconnected);
    auto after_disconnect = port.send_heartbeat(message("heartbeat", "heartbeat-002"));
    ASSERT_FALSE(after_disconnect);
    EXPECT_EQ(after_disconnect.error().business_code, "UPLINK_DISCONNECTED");
}

TEST(UplinkMockTransport, ScriptsOperationScopedSlowFailureDuplicateAndChecksumFaults)
{
    auto transport =
        connected_mock({.fault_capacity = 8U, .history_capacity = 16U, .maximum_delay = 50ms});
    ASSERT_NE(transport, nullptr);

    ASSERT_TRUE(transport->enqueue_fault(
        {.kind = MockFaultKind::retryable_failure, .operation = UplinkOperation::event_metadata}));
    EXPECT_TRUE(transport->send_heartbeat(message("heartbeat", "heartbeat-pass")));
    auto retryable = transport->send_event_metadata(event_request("event-retry"));
    ASSERT_FALSE(retryable);
    EXPECT_EQ(retryable.error().business_code, "UPLINK_SERVER_BUSY");
    EXPECT_TRUE(retryable.error().retryable);

    ASSERT_TRUE(transport->enqueue_fault({.kind = MockFaultKind::slow_response,
                                          .operation = UplinkOperation::heartbeat,
                                          .delay = 15ms}));
    const auto started = std::chrono::steady_clock::now();
    EXPECT_TRUE(transport->send_heartbeat(message("heartbeat", "heartbeat-slow")));
    EXPECT_GE(std::chrono::steady_clock::now() - started, 10ms);

    ASSERT_TRUE(transport->enqueue_fault({.kind = MockFaultKind::duplicate_acknowledgement,
                                          .operation = UplinkOperation::upload_file}));
    auto duplicated = transport->upload_file(upload_request("upload-duplicate"));
    ASSERT_TRUE(duplicated);
    EXPECT_EQ(duplicated.value().delivery_count, 2U);

    ASSERT_TRUE(transport->enqueue_fault(
        {.kind = MockFaultKind::checksum_mismatch, .operation = UplinkOperation::upload_file}));
    auto checksum = transport->upload_file(upload_request("upload-checksum"));
    ASSERT_FALSE(checksum);
    EXPECT_EQ(checksum.error().business_code, "UPLOAD_CHECKSUM_MISMATCH");
    EXPECT_TRUE(checksum.error().retryable);

    ASSERT_TRUE(transport->enqueue_fault(
        {.kind = MockFaultKind::permanent_failure, .operation = UplinkOperation::upload_file}));
    auto permanent = transport->upload_file(upload_request("upload-permanent"));
    ASSERT_FALSE(permanent);
    EXPECT_EQ(permanent.error().business_code, "UPLOAD_REJECTED");
    EXPECT_FALSE(permanent.error().retryable);

    const auto snapshot = transport->snapshot();
    EXPECT_EQ(snapshot.duplicate_acknowledgements, 1U);
    EXPECT_EQ(snapshot.checksum_mismatches, 1U);
}

TEST(UplinkMockTransport, OfflineFaultRequiresExplicitRecoveryAndReconnect)
{
    auto transport = connected_mock();
    ASSERT_NE(transport, nullptr);
    ASSERT_TRUE(transport->enqueue_fault(
        {.kind = MockFaultKind::offline, .operation = UplinkOperation::heartbeat}));
    auto offline = transport->send_heartbeat(message("heartbeat", "heartbeat-offline"));
    ASSERT_FALSE(offline);
    EXPECT_EQ(offline.error().business_code, "UPLINK_DISCONNECTED");
    EXPECT_EQ(transport->connection_state(), UplinkConnectionState::disconnected);

    auto still_offline = transport->connect(hello("session-offline"));
    ASSERT_FALSE(still_offline);
    transport->set_online(true);
    ASSERT_TRUE(transport->connect(hello("session-recovered")));
    EXPECT_EQ(transport->connection_state(), UplinkConnectionState::connected);
}

TEST(UplinkMockTransport, RejectsFullFaultQueueAndInvalidDelay)
{
    auto transport =
        connected_mock({.fault_capacity = 1U, .history_capacity = 8U, .maximum_delay = 20ms});
    ASSERT_NE(transport, nullptr);
    ASSERT_TRUE(transport->enqueue_fault({.kind = MockFaultKind::retryable_failure}));
    auto full = transport->enqueue_fault({.kind = MockFaultKind::offline});
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().business_code, "UPLINK_SERVER_BUSY");
    EXPECT_EQ(transport->snapshot().rejected_faults, 1U);

    transport->clear_faults();
    auto invalid = transport->enqueue_fault({.kind = MockFaultKind::slow_response, .delay = 21ms});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().business_code, "UPLINK_PROTOCOL_ERROR");
}

TEST(UplinkMockTransport, DeliversCommandsOutsideStateLock)
{
    auto transport = connected_mock();
    ASSERT_NE(transport, nullptr);
    bool invoked = false;
    transport->set_command_handler([&](const MessageEnvelope& command) {
        invoked = command.message_id == "command-001";
        EXPECT_EQ(transport->snapshot().connection_state, UplinkConnectionState::connected);
    });

    ASSERT_TRUE(transport->inject_command(message("command", "command-001")));
    EXPECT_TRUE(invoked);

    transport->disconnect();
    auto disconnected = transport->inject_command(message("command", "command-002"));
    ASSERT_FALSE(disconnected);
    EXPECT_EQ(disconnected.error().business_code, "UPLINK_DISCONNECTED");
}
