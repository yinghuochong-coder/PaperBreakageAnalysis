#include "paperbreak/uplink/mock_transport.hpp"

#include "paperbreak/common/error.hpp"

#include <algorithm>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace paperbreak::uplink::mock
{
namespace detail
{
struct MockUplinkState final
{
    explicit MockUplinkState(MockUplinkConfig value) : config(std::move(value)) {}

    mutable std::mutex mutex;
    MockUplinkConfig config;
    bool online{true};
    UplinkConnectionState connection_state{UplinkConnectionState::disconnected};
    std::string machine_id;
    std::uint64_t session_sequence{};
    std::deque<MockFaultStep> faults;
    std::deque<MockCallRecord> history;
    CommandHandler command_handler;
    std::uint64_t calls{};
    std::uint64_t successes{};
    std::uint64_t failures{};
    std::uint64_t duplicate_acknowledgements{};
    std::uint64_t checksum_mismatches{};
    std::uint64_t rejected_faults{};
};
} // namespace detail

namespace
{
struct PreparedCall final
{
    std::chrono::milliseconds delay{};
    std::uint32_t acknowledgement_delivery_count{1U};
};

Error mock_error(std::string code, std::string message, std::string operation, const bool retryable)
{
    return make_error(std::move(code), retryable ? Severity::warning : Severity::error,
                      std::move(message), "uplink-mock", std::move(operation), retryable);
}

Error protocol_error(std::string message, std::string operation)
{
    return mock_error("UPLINK_PROTOCOL_ERROR", std::move(message), std::move(operation), false);
}

Error disconnected_error(const std::string_view operation)
{
    return mock_error("UPLINK_DISCONNECTED", "Mock 上位机当前离线或会话未连接",
                      std::string{operation}, true);
}

void append_history_locked(detail::MockUplinkState& state, MockCallRecord record)
{
    if (state.history.size() == state.config.history_capacity)
        state.history.pop_front();
    state.history.push_back(std::move(record));
}

Result<PreparedCall> fail_prepared_locked(detail::MockUplinkState& state,
                                          const UplinkOperation operation,
                                          const std::string_view correlation_id, Error error)
{
    ++state.failures;
    append_history_locked(state, {.operation = operation,
                                  .correlation_id = std::string{correlation_id},
                                  .succeeded = false,
                                  .business_code = error.business_code});
    return Result<PreparedCall>::failure(std::move(error));
}

Result<PreparedCall> prepare_call(const std::shared_ptr<detail::MockUplinkState>& state,
                                  const UplinkOperation operation,
                                  const std::string_view correlation_id,
                                  const bool requires_connection)
{
    std::lock_guard lock{state->mutex};
    ++state->calls;
    if (!state->online ||
        (requires_connection && state->connection_state != UplinkConnectionState::connected))
    {
        return fail_prepared_locked(*state, operation, correlation_id,
                                    disconnected_error("uplink.mock.call"));
    }

    PreparedCall prepared;
    if (!state->faults.empty() && (!state->faults.front().operation.has_value() ||
                                   state->faults.front().operation.value() == operation))
    {
        const auto fault = state->faults.front();
        state->faults.pop_front();
        switch (fault.kind)
        {
        case MockFaultKind::offline:
            state->online = false;
            state->connection_state = UplinkConnectionState::disconnected;
            state->machine_id.clear();
            return fail_prepared_locked(*state, operation, correlation_id,
                                        disconnected_error("uplink.mock.fault.offline"));
        case MockFaultKind::slow_response:
            prepared.delay = fault.delay;
            break;
        case MockFaultKind::retryable_failure:
            return fail_prepared_locked(*state, operation, correlation_id,
                                        mock_error("UPLINK_SERVER_BUSY", "Mock 注入可重试传输失败",
                                                   "uplink.mock.fault.retryable", true));
        case MockFaultKind::permanent_failure:
            return fail_prepared_locked(*state, operation, correlation_id,
                                        mock_error("UPLOAD_REJECTED", "Mock 注入永久拒绝",
                                                   "uplink.mock.fault.permanent", false));
        case MockFaultKind::duplicate_acknowledgement:
            prepared.acknowledgement_delivery_count = 2U;
            break;
        case MockFaultKind::checksum_mismatch:
            ++state->checksum_mismatches;
            return fail_prepared_locked(*state, operation, correlation_id,
                                        mock_error("UPLOAD_CHECKSUM_MISMATCH",
                                                   "Mock 注入文件校验错误",
                                                   "uplink.mock.fault.checksum", true));
        }
    }
    return Result<PreparedCall>::success(prepared);
}

Result<TransportAcknowledgement> finish_acknowledgement(
    const std::shared_ptr<detail::MockUplinkState>& state, const UplinkOperation operation,
    const std::string_view correlation_id, const PreparedCall prepared)
{
    if (prepared.delay.count() > 0)
        std::this_thread::sleep_for(prepared.delay);

    std::lock_guard lock{state->mutex};
    if (!state->online || state->connection_state != UplinkConnectionState::connected)
    {
        ++state->failures;
        auto error = disconnected_error("uplink.mock.complete");
        append_history_locked(*state, {.operation = operation,
                                       .correlation_id = std::string{correlation_id},
                                       .succeeded = false,
                                       .business_code = error.business_code});
        return Result<TransportAcknowledgement>::failure(std::move(error));
    }

    ++state->successes;
    if (prepared.acknowledgement_delivery_count == 2U)
        ++state->duplicate_acknowledgements;
    append_history_locked(
        *state, {.operation = operation,
                 .correlation_id = std::string{correlation_id},
                 .succeeded = true,
                 .acknowledgement_delivery_count = prepared.acknowledgement_delivery_count});
    return Result<TransportAcknowledgement>::success(
        {.correlation_id = std::string{correlation_id},
         .acknowledged_at = current_utc_timestamp(),
         .delivery_count = prepared.acknowledgement_delivery_count});
}

Result<void> validate_message(const MessageEnvelope& message, const std::string_view expected_type,
                              const std::string_view operation)
{
    if (message.protocol_version != protocol_version || message.message_type != expected_type ||
        message.timestamp.empty() || message.payload_json.empty())
        return Result<void>::failure(
            protocol_error("控制消息版本、类型、时间或 payload 无效", std::string{operation}));
    if (auto result = validate_identifier(message.message_id, "messageId", 128U); !result)
        return result;
    if (auto result = validate_identifier(message.machine_id, "machineId", 64U); !result)
        return result;
    return Result<void>::success();
}
} // namespace

MockUplinkTransport::MockUplinkTransport(ValidatedTag, MockUplinkConfig config)
    : state_(std::make_shared<detail::MockUplinkState>(std::move(config)))
{
}

MockUplinkTransport::~MockUplinkTransport() = default;

Result<std::unique_ptr<MockUplinkTransport>> MockUplinkTransport::create(MockUplinkConfig config)
{
    if (config.fault_capacity == 0U || config.fault_capacity > maximum_mock_fault_capacity ||
        config.history_capacity == 0U || config.history_capacity > maximum_mock_history_capacity ||
        config.maximum_delay.count() < 0 || config.maximum_delay > maximum_mock_delay ||
        config.heartbeat_seconds == 0U || config.heartbeat_seconds > 3600U)
    {
        return Result<std::unique_ptr<MockUplinkTransport>>::failure(
            protocol_error("Mock Uplink 容量、延迟或心跳配置超出上限", "uplink.mock.create"));
    }
    return Result<std::unique_ptr<MockUplinkTransport>>::success(
        std::make_unique<MockUplinkTransport>(ValidatedTag{}, std::move(config)));
}

Result<TransportSession> MockUplinkTransport::connect(const SessionHello& hello)
{
    if (auto result = validate_identifier(hello.request_id, "requestId", 128U); !result)
        return Result<TransportSession>::failure(result.error());
    if (auto result = validate_identifier(hello.machine_id, "machineId", 64U); !result)
        return Result<TransportSession>::failure(result.error());
    if (std::ranges::find(hello.supported_protocol_versions, protocol_version) ==
        hello.supported_protocol_versions.end())
    {
        return Result<TransportSession>::failure(mock_error("UPLINK_PROTOCOL_VERSION_UNSUPPORTED",
                                                            "Mock 与客户端没有共同协议版本",
                                                            "uplink.mock.connect", false));
    }

    auto prepared = prepare_call(state_, UplinkOperation::connect, hello.request_id, false);
    if (!prepared)
        return Result<TransportSession>::failure(prepared.error());
    if (prepared.value().delay.count() > 0)
        std::this_thread::sleep_for(prepared.value().delay);

    std::lock_guard lock{state_->mutex};
    if (!state_->online)
    {
        ++state_->failures;
        auto error = disconnected_error("uplink.mock.connect.complete");
        append_history_locked(*state_, {.operation = UplinkOperation::connect,
                                        .correlation_id = hello.request_id,
                                        .succeeded = false,
                                        .business_code = error.business_code});
        return Result<TransportSession>::failure(std::move(error));
    }
    state_->connection_state = UplinkConnectionState::connected;
    state_->machine_id = hello.machine_id;
    ++state_->session_sequence;
    ++state_->successes;
    append_history_locked(*state_, {.operation = UplinkOperation::connect,
                                    .correlation_id = hello.request_id,
                                    .succeeded = true});
    return Result<TransportSession>::success(
        {.session_id = "mock-session-" + std::to_string(state_->session_sequence),
         .machine_id = hello.machine_id,
         .negotiated_protocol_version = protocol_version,
         .heartbeat_seconds = state_->config.heartbeat_seconds});
}

void MockUplinkTransport::disconnect() noexcept
{
    std::lock_guard lock{state_->mutex};
    state_->connection_state = UplinkConnectionState::disconnected;
    state_->machine_id.clear();
}

UplinkConnectionState MockUplinkTransport::connection_state() const noexcept
{
    std::lock_guard lock{state_->mutex};
    return state_->connection_state;
}

Result<TransportAcknowledgement> MockUplinkTransport::send_heartbeat(
    const MessageEnvelope& heartbeat)
{
    if (auto valid = validate_message(heartbeat, "heartbeat", "uplink.mock.heartbeat"); !valid)
        return Result<TransportAcknowledgement>::failure(valid.error());
    auto prepared = prepare_call(state_, UplinkOperation::heartbeat, heartbeat.message_id, true);
    if (!prepared)
        return Result<TransportAcknowledgement>::failure(prepared.error());
    return finish_acknowledgement(state_, UplinkOperation::heartbeat, heartbeat.message_id,
                                  prepared.value());
}

Result<TransportAcknowledgement> MockUplinkTransport::send_control_message(
    const MessageEnvelope& message)
{
    if (message.message_type == "heartbeat")
        return Result<TransportAcknowledgement>::failure(
            protocol_error("heartbeat 必须使用 send_heartbeat", "uplink.mock.control"));
    if (message.protocol_version != protocol_version || message.message_type.empty() ||
        message.timestamp.empty() || message.payload_json.empty())
        return Result<TransportAcknowledgement>::failure(
            protocol_error("控制消息版本、类型、时间或 payload 无效", "uplink.mock.control"));
    if (auto result = validate_identifier(message.message_id, "messageId", 128U); !result)
        return Result<TransportAcknowledgement>::failure(result.error());
    if (auto result = validate_identifier(message.machine_id, "machineId", 64U); !result)
        return Result<TransportAcknowledgement>::failure(result.error());
    auto prepared =
        prepare_call(state_, UplinkOperation::control_message, message.message_id, true);
    if (!prepared)
        return Result<TransportAcknowledgement>::failure(prepared.error());
    return finish_acknowledgement(state_, UplinkOperation::control_message, message.message_id,
                                  prepared.value());
}

Result<TransportAcknowledgement> MockUplinkTransport::send_event_metadata(
    const EventMetadataRequest& event)
{
    if (auto result = validate_identifier(event.request_id, "requestId", 128U); !result)
        return Result<TransportAcknowledgement>::failure(result.error());
    if (auto result = validate_identifier(event.machine_id, "machineId", 64U); !result)
        return Result<TransportAcknowledgement>::failure(result.error());
    if (auto result = validate_identifier(event.event_id, "eventId", 128U); !result)
        return Result<TransportAcknowledgement>::failure(result.error());
    if (event.metadata_json.empty() || event.metadata_json.size() > maximum_json_message_bytes)
        return Result<TransportAcknowledgement>::failure(
            protocol_error("事件元数据为空或超过 1 MiB", "uplink.mock.event"));
    auto prepared = prepare_call(state_, UplinkOperation::event_metadata, event.request_id, true);
    if (!prepared)
        return Result<TransportAcknowledgement>::failure(prepared.error());
    return finish_acknowledgement(state_, UplinkOperation::event_metadata, event.request_id,
                                  prepared.value());
}

Result<TransportAcknowledgement> MockUplinkTransport::upload_file(const UploadFileRequest& request)
{
    if (auto result = validate_identifier(request.machine_id, "machineId", 64U); !result)
        return Result<TransportAcknowledgement>::failure(result.error());
    if (auto result = validate_identifier(request.description.request_id, "requestId", 128U);
        !result)
        return Result<TransportAcknowledgement>::failure(result.error());
    if (auto result = validate_identifier(request.description.event_id, "eventId", 128U); !result)
        return Result<TransportAcknowledgement>::failure(result.error());
    if (auto result =
            validate_identifier(request.description.logical_file_id, "logicalFileId", 128U);
        !result)
        return Result<TransportAcknowledgement>::failure(result.error());
    if (request.source_path.empty() || request.description.total_bytes == 0U ||
        request.description.total_bytes > maximum_file_bytes ||
        request.description.chunk_bytes == 0U ||
        request.description.chunk_bytes > maximum_chunk_bytes ||
        !is_sha256_hex(request.description.sha256))
        return Result<TransportAcknowledgement>::failure(
            protocol_error("文件上传请求的路径、长度、分块或 SHA-256 无效", "uplink.mock.upload"));
    auto prepared =
        prepare_call(state_, UplinkOperation::upload_file, request.description.request_id, true);
    if (!prepared)
        return Result<TransportAcknowledgement>::failure(prepared.error());
    return finish_acknowledgement(state_, UplinkOperation::upload_file,
                                  request.description.request_id, prepared.value());
}

void MockUplinkTransport::set_command_handler(CommandHandler handler)
{
    std::lock_guard lock{state_->mutex};
    state_->command_handler = std::move(handler);
}

Result<void> MockUplinkTransport::enqueue_fault(MockFaultStep fault)
{
    if (fault.delay.count() < 0 || fault.delay > state_->config.maximum_delay ||
        (fault.kind == MockFaultKind::slow_response && fault.delay.count() == 0) ||
        (fault.kind != MockFaultKind::slow_response && fault.delay.count() != 0))
        return Result<void>::failure(
            protocol_error("Mock 故障延迟与类型不匹配或超过上限", "uplink.mock.enqueue"));

    std::lock_guard lock{state_->mutex};
    if (state_->faults.size() == state_->config.fault_capacity)
    {
        ++state_->rejected_faults;
        return Result<void>::failure(
            mock_error("UPLINK_SERVER_BUSY", "Mock 故障脚本队列已满", "uplink.mock.enqueue", true));
    }
    state_->faults.push_back(std::move(fault));
    return Result<void>::success();
}

void MockUplinkTransport::clear_faults() noexcept
{
    std::lock_guard lock{state_->mutex};
    state_->faults.clear();
}

void MockUplinkTransport::set_online(const bool online) noexcept
{
    std::lock_guard lock{state_->mutex};
    state_->online = online;
    if (!online)
    {
        state_->connection_state = UplinkConnectionState::disconnected;
        state_->machine_id.clear();
    }
}

Result<void> MockUplinkTransport::inject_command(const MessageEnvelope& command)
{
    if (auto valid = validate_message(command, "command", "uplink.mock.command"); !valid)
        return valid;

    CommandHandler handler;
    {
        std::lock_guard lock{state_->mutex};
        if (!state_->online || state_->connection_state != UplinkConnectionState::connected)
            return Result<void>::failure(disconnected_error("uplink.mock.command"));
        if (!state_->command_handler)
            return Result<void>::failure(
                protocol_error("尚未注册 Mock 命令处理器", "uplink.mock.command"));
        handler = state_->command_handler;
    }
    handler(command);
    return Result<void>::success();
}

MockUplinkSnapshot MockUplinkTransport::snapshot() const noexcept
{
    std::lock_guard lock{state_->mutex};
    return {.online = state_->online,
            .connection_state = state_->connection_state,
            .queued_faults = state_->faults.size(),
            .retained_history = state_->history.size(),
            .calls = state_->calls,
            .successes = state_->successes,
            .failures = state_->failures,
            .duplicate_acknowledgements = state_->duplicate_acknowledgements,
            .checksum_mismatches = state_->checksum_mismatches,
            .rejected_faults = state_->rejected_faults};
}

std::vector<MockCallRecord> MockUplinkTransport::history() const
{
    std::lock_guard lock{state_->mutex};
    return {state_->history.begin(), state_->history.end()};
}

void MockUplinkTransport::clear_history() noexcept
{
    std::lock_guard lock{state_->mutex};
    state_->history.clear();
}

} // namespace paperbreak::uplink::mock
