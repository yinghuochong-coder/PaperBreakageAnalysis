#include "paperbreak/uplink/runtime.hpp"

#include "paperbreak/common/error.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>

namespace paperbreak::uplink
{
namespace
{
using Json = nlohmann::json;
inline constexpr std::size_t maximum_runtime_payload_bytes = maximum_json_message_bytes - 4096U;

Error runtime_error(std::string code, std::string message, std::string operation,
                    const bool retryable = false)
{
    return make_error(std::move(code), Severity::error, std::move(message), "uplink",
                      std::move(operation), retryable);
}

bool has_only_fields(const Json& value, const std::initializer_list<std::string_view> fields)
{
    if (!value.is_object())
        return false;
    const std::set<std::string_view> allowed(fields.begin(), fields.end());
    return std::ranges::all_of(
        value.items(), [&allowed](const auto& item) { return allowed.contains(item.key()); });
}

std::string severity_name(const Severity severity)
{
    switch (severity)
    {
    case Severity::info:
        return "info";
    case Severity::warning:
        return "warning";
    case Severity::error:
        return "error";
    case Severity::critical:
        return "critical";
    }
    return "error";
}

Json error_json(const Error& error)
{
    Json details = Json::object();
    for (const auto& detail : error.details)
        details[detail.key] = detail.value;
    return {{"businessCode", error.business_code},
            {"severity", severity_name(error.severity)},
            {"message", error.message},
            {"module", error.module},
            {"operation", error.operation},
            {"retryable", error.retryable},
            {"timestamp", error.timestamp},
            {"details", std::move(details)}};
}

std::optional<std::chrono::system_clock::time_point> parse_timestamp(const std::string_view value)
{
    // Uplink v1 emits RFC 3339. Keep parsing independent of locale and MSVC chrono I/O support.
    if (value.size() < 20U || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':')
        return std::nullopt;
    const auto number = [value](const std::size_t offset,
                                const std::size_t count) -> std::optional<unsigned> {
        unsigned result = 0U;
        for (std::size_t index = 0; index < count; ++index)
        {
            const char digit = value[offset + index];
            if (digit < '0' || digit > '9')
                return std::nullopt;
            result = result * 10U + static_cast<unsigned>(digit - '0');
        }
        return result;
    };
    const auto year = number(0U, 4U);
    const auto month = number(5U, 2U);
    const auto day = number(8U, 2U);
    const auto hour = number(11U, 2U);
    const auto minute = number(14U, 2U);
    const auto second = number(17U, 2U);
    if (!year || !month || !day || !hour || !minute || !second || *hour > 23U || *minute > 59U ||
        *second > 59U)
        return std::nullopt;

    std::size_t cursor = 19U;
    std::chrono::milliseconds fraction{};
    if (cursor < value.size() && value[cursor] == '.')
    {
        ++cursor;
        std::size_t digits = 0U;
        unsigned milliseconds = 0U;
        while (cursor < value.size() && value[cursor] >= '0' && value[cursor] <= '9')
        {
            if (digits < 3U)
                milliseconds = milliseconds * 10U + static_cast<unsigned>(value[cursor] - '0');
            ++digits;
            ++cursor;
        }
        if (digits == 0U)
            return std::nullopt;
        while (digits < 3U)
        {
            milliseconds *= 10U;
            ++digits;
        }
        fraction = std::chrono::milliseconds{milliseconds};
    }

    int offset_minutes = 0;
    if (cursor < value.size() && value[cursor] == 'Z')
        ++cursor;
    else if (cursor + 6U == value.size() && (value[cursor] == '+' || value[cursor] == '-') &&
             value[cursor + 3U] == ':')
    {
        const auto offset_hour = number(cursor + 1U, 2U);
        const auto offset_minute = number(cursor + 4U, 2U);
        if (!offset_hour || !offset_minute || *offset_hour > 23U || *offset_minute > 59U)
            return std::nullopt;
        offset_minutes = static_cast<int>(*offset_hour * 60U + *offset_minute);
        if (value[cursor] == '-')
            offset_minutes = -offset_minutes;
        cursor += 6U;
    }
    else
        return std::nullopt;
    if (cursor != value.size())
        return std::nullopt;

    const std::chrono::year_month_day date{std::chrono::year{static_cast<int>(*year)},
                                           std::chrono::month{*month}, std::chrono::day{*day}};
    if (!date.ok())
        return std::nullopt;
    return std::chrono::sys_days{date} + std::chrono::hours{*hour} +
           std::chrono::minutes{static_cast<int>(*minute) - offset_minutes} +
           std::chrono::seconds{*second} + fraction;
}

Result<RemoteCommand> parse_remote_command(const MessageEnvelope& envelope)
{
    if (envelope.protocol_version != protocol_version || envelope.message_type != "command")
        return Result<RemoteCommand>::failure(runtime_error(
            "UPLINK_PROTOCOL_ERROR", "收到的消息不是 Uplink v1 command", "uplink.command.parse"));
    auto machine = validate_identifier(envelope.machine_id, "machineId", 64U);
    auto message = validate_identifier(envelope.message_id, "messageId", 128U);
    if (!machine)
        return Result<RemoteCommand>::failure(machine.error());
    if (!message)
        return Result<RemoteCommand>::failure(message.error());
    if (envelope.payload_json.empty() || envelope.payload_json.size() > maximum_json_message_bytes)
        return Result<RemoteCommand>::failure(runtime_error(
            "UPLINK_PROTOCOL_ERROR", "命令 payload 为空或超过上限", "uplink.command.parse"));
    Json payload = Json::parse(envelope.payload_json, nullptr, false);
    if (payload.is_discarded() ||
        !has_only_fields(payload, {"commandId", "commandType", "deadline", "operatorConfirmed",
                                   "body", "extensions"}) ||
        !payload.contains("commandId") || !payload["commandId"].is_string() ||
        !payload.contains("commandType") || !payload["commandType"].is_string() ||
        !payload.contains("deadline") || !payload["deadline"].is_string() ||
        !payload.contains("operatorConfirmed") || !payload["operatorConfirmed"].is_boolean() ||
        !payload.contains("body") || !payload["body"].is_object() ||
        (payload.contains("extensions") && !payload["extensions"].is_object()))
        return Result<RemoteCommand>::failure(runtime_error("UPLINK_PROTOCOL_ERROR",
                                                            "命令字段缺失、类型错误或包含未知字段",
                                                            "uplink.command.parse"));

    RemoteCommand result{.command_id = payload["commandId"].get<std::string>(),
                         .command_type = payload["commandType"].get<std::string>(),
                         .deadline = payload["deadline"].get<std::string>(),
                         .operator_confirmed = payload["operatorConfirmed"].get<bool>(),
                         .body_json = payload["body"].dump()};
    auto command_id = validate_identifier(result.command_id, "commandId", 128U);
    auto command_type = validate_identifier(result.command_type, "commandType", 128U);
    if (!command_id)
        return Result<RemoteCommand>::failure(command_id.error());
    if (!command_type)
        return Result<RemoteCommand>::failure(command_type.error());
    if (!parse_timestamp(result.deadline))
        return Result<RemoteCommand>::failure(runtime_error("UPLINK_PROTOCOL_ERROR",
                                                            "命令 deadline 不是有效 RFC 3339 时间",
                                                            "uplink.command.parse"));
    return Result<RemoteCommand>::success(std::move(result));
}

std::string command_fingerprint(const RemoteCommand& command)
{
    return command.command_type + '\n' + command.deadline + '\n' +
           (command.operator_confirmed ? "1\n" : "0\n") + command.body_json;
}

struct CachedCommandResult final
{
    std::string fingerprint;
    std::string command_type;
    Result<std::string> outcome;
    std::size_t retained_bytes{};
};

std::size_t envelope_bytes(const MessageEnvelope& envelope) noexcept
{
    return envelope.message_type.size() + envelope.message_id.size() + envelope.machine_id.size() +
           envelope.timestamp.size() + envelope.payload_json.size();
}

std::size_t error_bytes(const Error& error) noexcept
{
    std::size_t result = error.business_code.size() + error.message.size() + error.module.size() +
                         error.operation.size() + error.timestamp.size();
    for (const auto& detail : error.details)
        result += detail.key.size() + detail.value.size();
    return result;
}

} // namespace

struct UplinkRuntime::Impl final
{
    std::shared_ptr<IUplinkTransport> transport;
    UplinkRuntimeConfig config;
    UplinkStatusProvider status_provider;
    RemoteCommandExecutor command_executor;
    std::set<std::string> capabilities;

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<MessageEnvelope> commands;
    std::deque<std::string> result_order;
    std::unordered_map<std::string, CachedCommandResult> results;
    UplinkRuntimeSnapshot metrics;
    std::stop_source stop_source;
    std::thread worker;
    bool started{};
    bool accepting_commands{};
    bool finished{true};
    std::uint64_t sequence{};
    std::size_t command_bytes{};
    std::size_t result_bytes{};

    void enqueue(MessageEnvelope command) noexcept
    {
        try
        {
            std::lock_guard lock{mutex};
            if (!accepting_commands)
                return;
            ++metrics.commands_received;
            const std::size_t bytes = envelope_bytes(command);
            if (command.payload_json.empty() ||
                command.payload_json.size() > maximum_json_message_bytes ||
                bytes > config.command_queue_byte_capacity)
            {
                ++metrics.command_queue_rejections;
                ++metrics.commands_rejected;
                metrics.last_error_code = "UPLINK_PROTOCOL_ERROR";
                return;
            }
            if (commands.size() >= config.command_queue_capacity ||
                command_bytes > config.command_queue_byte_capacity - bytes)
            {
                ++metrics.command_queue_rejections;
                ++metrics.commands_rejected;
                metrics.last_error_code = "UPLINK_SERVER_BUSY";
                return;
            }
            command_bytes += bytes;
            commands.push_back(std::move(command));
            metrics.command_queue_depth = commands.size();
            metrics.command_queue_bytes = command_bytes;
            metrics.command_queue_high_watermark =
                std::max(metrics.command_queue_high_watermark, commands.size());
            metrics.command_queue_byte_high_watermark =
                std::max(metrics.command_queue_byte_high_watermark, command_bytes);
            condition.notify_all();
        }
        catch (...)
        {
            std::lock_guard lock{mutex};
            ++metrics.command_queue_rejections;
            ++metrics.commands_rejected;
            metrics.last_error_code = "SYS_INTERNAL_ERROR";
        }
    }

    bool stop_requested() const noexcept
    {
        return stop_source.stop_requested();
    }

    void set_state(const UplinkRuntimeState state)
    {
        const auto connection_state = transport->connection_state();
        std::lock_guard lock{mutex};
        metrics.state = state;
        metrics.transport_state = connection_state;
    }

    void record_error(const Error& error)
    {
        std::lock_guard lock{mutex};
        metrics.last_error_code = error.business_code;
    }

    bool interruptible_wait(const std::chrono::milliseconds duration, const bool wake_for_command)
    {
        std::unique_lock lock{mutex};
        condition.wait_for(lock, duration, [&] {
            return stop_requested() || (wake_for_command && !commands.empty());
        });
        return !stop_requested();
    }

    MessageEnvelope make_message(const std::string_view type, std::string payload)
    {
        const auto next = ++sequence;
        return {.protocol_version = protocol_version,
                .message_type = std::string{type},
                .message_id = "edge-" + std::to_string(next),
                .machine_id = config.session_hello.machine_id,
                .sequence = next,
                .timestamp = current_utc_timestamp(),
                .payload_json = std::move(payload)};
    }

    bool send_status()
    {
        Result<std::string> status = Result<std::string>::failure(
            runtime_error("SYS_INTERNAL_ERROR", "状态提供器异常", "uplink.status.provider"));
        try
        {
            status = status_provider();
        }
        catch (...)
        {
        }
        if (!status)
        {
            record_error(status.error());
            return false;
        }
        Json payload = Json::parse(status.value(), nullptr, false);
        if (payload.is_discarded() || !payload.is_object() ||
            status.value().size() > maximum_runtime_payload_bytes)
        {
            record_error(runtime_error("UPLINK_PROTOCOL_ERROR", "状态提供器必须返回有界 JSON 对象",
                                       "uplink.status.validate"));
            return false;
        }
        auto sent = transport->send_control_message(make_message("status", payload.dump()));
        if (!sent)
        {
            std::lock_guard lock{mutex};
            ++metrics.send_failures;
            metrics.last_error_code = sent.error().business_code;
            return false;
        }
        std::lock_guard lock{mutex};
        ++metrics.statuses_sent;
        metrics.last_success_at = sent.value().acknowledged_at;
        return true;
    }

    bool send_heartbeat(const TransportSession& session)
    {
        auto sent = transport->send_heartbeat(make_message(
            "heartbeat", Json{{"sessionId", session.session_id}, {"state", "running"}}.dump()));
        if (!sent)
        {
            std::lock_guard lock{mutex};
            ++metrics.send_failures;
            metrics.last_error_code = sent.error().business_code;
            return false;
        }
        std::lock_guard lock{mutex};
        ++metrics.heartbeats_sent;
        metrics.last_heartbeat_at = sent.value().acknowledged_at;
        metrics.last_success_at = sent.value().acknowledged_at;
        return true;
    }

    Result<std::string> rejected_command(const std::string& code, const std::string& message,
                                         const std::string& operation)
    {
        return Result<std::string>::failure(runtime_error(code, message, operation));
    }

    Result<std::string> execute_command(const RemoteCommand& command)
    {
        if (!capabilities.contains(command.command_type))
            return rejected_command("SYS_NOT_SUPPORTED", "命令未在会话能力中声明",
                                    "uplink.command.capability");
        const auto deadline = parse_timestamp(command.deadline);
        if (!deadline || std::chrono::system_clock::now() > *deadline)
            return rejected_command("UPLINK_COMMAND_EXPIRED", "命令已超过截止时间",
                                    "uplink.command.deadline");
        if (command.command_type != "system.requestStatus" && !command.operator_confirmed)
            return rejected_command("UPLINK_COMMAND_NOT_CONFIRMED", "变更命令缺少操作员确认",
                                    "uplink.command.confirmation");
        try
        {
            auto outcome = command_executor(command, stop_source.get_token());
            if (outcome &&
                (outcome.value().empty() || outcome.value().size() > maximum_runtime_payload_bytes))
                return rejected_command("UPLINK_PROTOCOL_ERROR",
                                        "远程命令执行器返回为空或超过运行时上限",
                                        "uplink.command.execute");
            if (outcome)
            {
                const Json value = Json::parse(outcome.value(), nullptr, false);
                if (value.is_discarded() || !value.is_object())
                    return rejected_command("UPLINK_PROTOCOL_ERROR",
                                            "远程命令执行器必须返回 JSON 对象",
                                            "uplink.command.execute");
            }
            else if (error_bytes(outcome.error()) > maximum_runtime_payload_bytes)
                return rejected_command("UPLINK_PROTOCOL_ERROR",
                                        "远程命令执行器返回的错误超过运行时上限",
                                        "uplink.command.execute");
            return outcome;
        }
        catch (...)
        {
            return rejected_command("SYS_INTERNAL_ERROR", "远程命令执行器异常",
                                    "uplink.command.execute");
        }
    }

    void retain_result(const RemoteCommand& command, Result<std::string> outcome)
    {
        const std::string fingerprint = command_fingerprint(command);
        const std::size_t retained =
            command.command_id.size() + command.command_type.size() + fingerprint.size() +
            (outcome ? outcome.value().size() : error_bytes(outcome.error()));
        while (!result_order.empty() &&
               (result_order.size() >= config.command_deduplication_capacity ||
                result_bytes > config.command_deduplication_byte_capacity - retained))
        {
            const auto existing = results.find(result_order.front());
            if (existing != results.end())
            {
                result_bytes -= existing->second.retained_bytes;
                results.erase(existing);
            }
            result_order.pop_front();
        }
        const std::string id = command.command_id;
        result_order.push_back(id);
        results.emplace(id, CachedCommandResult{.fingerprint = std::move(fingerprint),
                                                .command_type = command.command_type,
                                                .outcome = std::move(outcome),
                                                .retained_bytes = retained});
        result_bytes += retained;
        std::lock_guard lock{mutex};
        metrics.retained_command_results = results.size();
        metrics.retained_command_result_bytes = result_bytes;
    }

    bool send_command_result(const std::string& command_id, const std::string& command_type,
                             const Result<std::string>& outcome, const bool duplicate)
    {
        Json payload{{"commandId", command_id},
                     {"commandType", command_type},
                     {"success", static_cast<bool>(outcome)},
                     {"duplicate", duplicate}};
        if (outcome)
        {
            Json result = Json::parse(outcome.value(), nullptr, false);
            payload["result"] = result.is_discarded() ? Json::object() : std::move(result);
        }
        else
            payload["error"] = error_json(outcome.error());
        auto sent = transport->send_control_message(make_message("command.result", payload.dump()));
        if (sent)
        {
            std::lock_guard lock{mutex};
            metrics.last_success_at = sent.value().acknowledged_at;
            return true;
        }
        std::lock_guard lock{mutex};
        ++metrics.send_failures;
        metrics.last_error_code = sent.error().business_code;
        return false;
    }

    bool process_one_command()
    {
        MessageEnvelope envelope;
        {
            std::lock_guard lock{mutex};
            if (commands.empty())
                return true;
            envelope = std::move(commands.front());
            commands.pop_front();
            command_bytes -= envelope_bytes(envelope);
            metrics.command_queue_depth = commands.size();
            metrics.command_queue_bytes = command_bytes;
        }

        auto parsed = parse_remote_command(envelope);
        if (parsed && envelope.machine_id != config.session_hello.machine_id)
            parsed = Result<RemoteCommand>::failure(runtime_error("UPLINK_PROTOCOL_ERROR",
                                                                  "命令 machineId 与当前会话不匹配",
                                                                  "uplink.command.machine"));
        if (!parsed || parsed.value().command_id.empty())
        {
            const Error error = parsed ? runtime_error("UPLINK_PROTOCOL_ERROR", "命令 ID 为空",
                                                       "uplink.command.parse")
                                       : parsed.error();
            {
                std::lock_guard lock{mutex};
                ++metrics.commands_rejected;
                metrics.last_error_code = error.business_code;
            }
            return send_command_result(envelope.message_id, "invalid",
                                       Result<std::string>::failure(error), false);
        }

        const auto existing = results.find(parsed.value().command_id);
        if (existing != results.end())
        {
            if (existing->second.fingerprint != command_fingerprint(parsed.value()))
            {
                auto conflict =
                    rejected_command("UPLINK_COMMAND_CONFLICT", "相同 commandId 携带了不同命令内容",
                                     "uplink.command.deduplicate");
                {
                    std::lock_guard lock{mutex};
                    ++metrics.commands_conflicted;
                    ++metrics.commands_rejected;
                }
                return send_command_result(parsed.value().command_id, parsed.value().command_type,
                                           conflict, true);
            }
            {
                std::lock_guard lock{mutex};
                ++metrics.commands_replayed;
            }
            return send_command_result(parsed.value().command_id, existing->second.command_type,
                                       existing->second.outcome, true);
        }

        auto outcome = execute_command(parsed.value());
        {
            std::lock_guard lock{mutex};
            if (outcome)
                ++metrics.commands_executed;
            else
            {
                ++metrics.commands_rejected;
                metrics.last_error_code = outcome.error().business_code;
            }
        }
        retain_result(parsed.value(), outcome);
        return send_command_result(parsed.value().command_id, parsed.value().command_type,
                                   results.at(parsed.value().command_id).outcome, false);
    }

    void disconnect_for_retry()
    {
        transport->disconnect();
        std::lock_guard lock{mutex};
        metrics.transport_state = UplinkConnectionState::disconnected;
    }

    void run() noexcept
    {
        auto reconnect_delay = config.initial_reconnect_delay;
        try
        {
            while (!stop_requested())
            {
                set_state(UplinkRuntimeState::connecting);
                {
                    std::lock_guard lock{mutex};
                    ++metrics.connection_attempts;
                }
                auto connected = transport->connect(config.session_hello);
                if (!connected ||
                    connected.value().negotiated_protocol_version != protocol_version ||
                    connected.value().machine_id != config.session_hello.machine_id ||
                    connected.value().heartbeat_seconds == 0U ||
                    connected.value().heartbeat_seconds > 3600U)
                {
                    Error error =
                        !connected ? connected.error()
                                   : runtime_error("UPLINK_PROTOCOL_VERSION_UNSUPPORTED",
                                                   "会话协商结果无效", "uplink.connect.validate");
                    transport->disconnect();
                    {
                        std::lock_guard lock{mutex};
                        ++metrics.reconnect_failures;
                        metrics.state = UplinkRuntimeState::backing_off;
                        metrics.transport_state = UplinkConnectionState::disconnected;
                        metrics.current_reconnect_delay = reconnect_delay;
                        metrics.last_error_code = error.business_code;
                    }
                    if (!interruptible_wait(reconnect_delay, false))
                        break;
                    reconnect_delay = std::min(config.maximum_reconnect_delay, reconnect_delay * 2);
                    continue;
                }

                const TransportSession session = connected.value();
                reconnect_delay = config.initial_reconnect_delay;
                {
                    std::lock_guard lock{mutex};
                    ++metrics.successful_connections;
                    metrics.state = UplinkRuntimeState::connected;
                    metrics.transport_state = UplinkConnectionState::connected;
                    metrics.current_reconnect_delay = reconnect_delay;
                    metrics.last_success_at = current_utc_timestamp();
                }
                if (!send_status())
                {
                    disconnect_for_retry();
                    {
                        std::lock_guard lock{mutex};
                        ++metrics.reconnect_failures;
                        metrics.state = UplinkRuntimeState::backing_off;
                        metrics.current_reconnect_delay = reconnect_delay;
                    }
                    if (!interruptible_wait(reconnect_delay, false))
                        break;
                    reconnect_delay = std::min(config.maximum_reconnect_delay, reconnect_delay * 2);
                    continue;
                }

                const auto heartbeat_interval = std::chrono::seconds{session.heartbeat_seconds};
                auto next_heartbeat = std::chrono::steady_clock::now() + heartbeat_interval;
                bool retry = false;
                while (!stop_requested() && !retry)
                {
                    bool has_command = false;
                    {
                        std::lock_guard lock{mutex};
                        has_command = !commands.empty();
                    }
                    if (has_command && !process_one_command())
                    {
                        retry = true;
                        break;
                    }
                    if (std::chrono::steady_clock::now() >= next_heartbeat)
                    {
                        if (!send_heartbeat(session) || !send_status())
                        {
                            retry = true;
                            break;
                        }
                        next_heartbeat = std::chrono::steady_clock::now() + heartbeat_interval;
                    }
                    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                        next_heartbeat - std::chrono::steady_clock::now());
                    if (!interruptible_wait(std::max(remaining, std::chrono::milliseconds{0}),
                                            true))
                        break;
                    if (transport->connection_state() != UplinkConnectionState::connected)
                        retry = true;
                }
                disconnect_for_retry();
                if (!stop_requested())
                {
                    {
                        std::lock_guard lock{mutex};
                        ++metrics.reconnect_failures;
                        metrics.state = UplinkRuntimeState::backing_off;
                        metrics.current_reconnect_delay = reconnect_delay;
                    }
                    if (!interruptible_wait(reconnect_delay, false))
                        break;
                    reconnect_delay = std::min(config.maximum_reconnect_delay, reconnect_delay * 2);
                }
            }
        }
        catch (...)
        {
            std::lock_guard lock{mutex};
            metrics.last_error_code = "SYS_INTERNAL_ERROR";
        }
        transport->set_command_handler({});
        transport->disconnect();
        {
            std::lock_guard lock{mutex};
            accepting_commands = false;
            commands.clear();
            command_bytes = 0U;
            metrics.command_queue_depth = 0U;
            metrics.command_queue_bytes = 0U;
            metrics.state = UplinkRuntimeState::stopped;
            metrics.transport_state = UplinkConnectionState::disconnected;
            finished = true;
        }
        condition.notify_all();
    }
};

UplinkRuntime::UplinkRuntime(ValidatedTag, std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

UplinkRuntime::~UplinkRuntime()
{
    request_stop();
    if (impl_->worker.joinable())
        impl_->worker.join();
}

Result<std::unique_ptr<UplinkRuntime>> UplinkRuntime::create(
    std::shared_ptr<IUplinkTransport> transport, UplinkRuntimeConfig config,
    UplinkStatusProvider status_provider, RemoteCommandExecutor command_executor)
{
    if (!transport || !status_provider || !command_executor ||
        config.initial_reconnect_delay.count() <= 0 ||
        config.maximum_reconnect_delay < config.initial_reconnect_delay ||
        config.maximum_reconnect_delay > std::chrono::hours{1} ||
        config.command_queue_capacity == 0U ||
        config.command_queue_capacity > maximum_uplink_command_capacity ||
        config.command_queue_byte_capacity < maximum_json_message_bytes ||
        config.command_queue_byte_capacity > maximum_uplink_runtime_byte_capacity ||
        config.command_deduplication_capacity == 0U ||
        config.command_deduplication_capacity > maximum_uplink_deduplication_capacity ||
        config.command_deduplication_byte_capacity < 2U * maximum_json_message_bytes + 4096U ||
        config.command_deduplication_byte_capacity > maximum_uplink_runtime_byte_capacity)
        return Result<std::unique_ptr<UplinkRuntime>>::failure(runtime_error(
            "SYS_CONFIG_INVALID", "Uplink 运行时容量、退避或依赖无效", "uplink.runtime.create"));
    auto machine = validate_identifier(config.session_hello.machine_id, "machineId", 64U);
    auto request = validate_identifier(config.session_hello.request_id, "requestId", 128U);
    auto line =
        validate_identifier(config.session_hello.production_line_id, "productionLineId", 64U);
    if (!machine || !request || !line || config.session_hello.software_version.empty() ||
        std::ranges::find(config.session_hello.supported_protocol_versions, protocol_version) ==
            config.session_hello.supported_protocol_versions.end())
        return Result<std::unique_ptr<UplinkRuntime>>::failure(
            !machine   ? machine.error()
            : !request ? request.error()
            : !line    ? line.error()
                       : runtime_error("SYS_CONFIG_INVALID", "会话必须声明 Uplink v1 和软件版本",
                                       "uplink.runtime.create"));
    std::set<std::string> capabilities;
    for (const auto& capability : config.session_hello.capabilities)
    {
        auto valid = validate_identifier(capability, "capabilities", 128U);
        if (!valid)
            return Result<std::unique_ptr<UplinkRuntime>>::failure(valid.error());
        capabilities.insert(capability);
    }
    auto impl = std::make_shared<Impl>();
    impl->transport = std::move(transport);
    impl->config = std::move(config);
    impl->status_provider = std::move(status_provider);
    impl->command_executor = std::move(command_executor);
    impl->capabilities = std::move(capabilities);
    return Result<std::unique_ptr<UplinkRuntime>>::success(
        std::make_unique<UplinkRuntime>(ValidatedTag{}, std::move(impl)));
}

Result<void> UplinkRuntime::start()
{
    {
        std::lock_guard lock{impl_->mutex};
        if (impl_->started)
            return Result<void>::failure(runtime_error("SYS_INVALID_STATE", "Uplink 运行时已经启动",
                                                       "uplink.runtime.start"));
        impl_->started = true;
        impl_->finished = false;
        impl_->accepting_commands = true;
    }
    const std::weak_ptr<Impl> weak = impl_;
    impl_->transport->set_command_handler([weak](const MessageEnvelope& command) {
        if (const auto state = weak.lock())
            state->enqueue(command);
    });
    try
    {
        impl_->worker = std::thread{[state = impl_] { state->run(); }};
    }
    catch (...)
    {
        impl_->transport->set_command_handler({});
        {
            std::lock_guard lock{impl_->mutex};
            impl_->started = false;
            impl_->finished = true;
            impl_->accepting_commands = false;
        }
        return Result<void>::failure(runtime_error("SYS_INTERNAL_ERROR", "无法创建 Uplink 工作线程",
                                                   "uplink.runtime.start"));
    }
    return Result<void>::success();
}

void UplinkRuntime::request_stop() noexcept
{
    {
        std::lock_guard lock{impl_->mutex};
        if (!impl_->started || impl_->finished)
            return;
        impl_->accepting_commands = false;
        impl_->metrics.state = UplinkRuntimeState::stop_requested;
        impl_->stop_source.request_stop();
    }
    impl_->condition.notify_all();
    impl_->transport->disconnect();
}

Result<void> UplinkRuntime::join(const std::chrono::steady_clock::time_point deadline)
{
    {
        std::unique_lock lock{impl_->mutex};
        if (!impl_->started)
            return Result<void>::success();
        if (!impl_->condition.wait_until(lock, deadline, [&] { return impl_->finished; }))
            return Result<void>::failure(runtime_error("SYS_SHUTDOWN_TIMEOUT",
                                                       "Uplink 工作线程未在截止时间内停止",
                                                       "uplink.runtime.join", true));
    }
    if (impl_->worker.joinable())
        impl_->worker.join();
    return Result<void>::success();
}

UplinkRuntimeSnapshot UplinkRuntime::snapshot() const noexcept
{
    UplinkRuntimeSnapshot result;
    {
        std::lock_guard lock{impl_->mutex};
        result = impl_->metrics;
        result.command_queue_depth = impl_->commands.size();
        result.command_queue_bytes = impl_->command_bytes;
    }
    result.transport_state = impl_->transport->connection_state();
    return result;
}

std::string_view uplink_runtime_state_name(const UplinkRuntimeState state) noexcept
{
    switch (state)
    {
    case UplinkRuntimeState::stopped:
        return "stopped";
    case UplinkRuntimeState::connecting:
        return "connecting";
    case UplinkRuntimeState::connected:
        return "connected";
    case UplinkRuntimeState::backing_off:
        return "backing-off";
    case UplinkRuntimeState::stop_requested:
        return "stop-requested";
    }
    return "unknown";
}

} // namespace paperbreak::uplink
