#pragma once

#include "paperbreak/uplink/transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>

namespace paperbreak::uplink
{

inline constexpr std::size_t maximum_uplink_command_capacity = 4096U;
inline constexpr std::size_t maximum_uplink_deduplication_capacity = 4096U;
inline constexpr std::size_t maximum_uplink_runtime_byte_capacity = 64U * 1024U * 1024U;

enum class UplinkRuntimeState
{
    stopped,
    connecting,
    connected,
    backing_off,
    stop_requested,
};

struct RemoteCommand final
{
    std::string command_id;
    std::string command_type;
    std::string deadline;
    bool operator_confirmed{};
    std::string body_json{"{}"};
};

using UplinkStatusProvider = std::function<Result<std::string>()>;
using RemoteCommandExecutor =
    std::function<Result<std::string>(const RemoteCommand&, std::stop_token)>;

struct UplinkRuntimeConfig final
{
    SessionHello session_hello;
    std::chrono::milliseconds initial_reconnect_delay{std::chrono::seconds{1}};
    std::chrono::milliseconds maximum_reconnect_delay{std::chrono::minutes{1}};
    std::size_t command_queue_capacity{64U};
    std::size_t command_queue_byte_capacity{8U * 1024U * 1024U};
    std::size_t command_deduplication_capacity{1024U};
    std::size_t command_deduplication_byte_capacity{16U * 1024U * 1024U};
};

struct UplinkRuntimeSnapshot final
{
    UplinkRuntimeState state{UplinkRuntimeState::stopped};
    UplinkConnectionState transport_state{UplinkConnectionState::disconnected};
    std::uint64_t connection_attempts{};
    std::uint64_t successful_connections{};
    std::uint64_t reconnect_failures{};
    std::uint64_t heartbeats_sent{};
    std::uint64_t statuses_sent{};
    std::uint64_t send_failures{};
    std::uint64_t commands_received{};
    std::uint64_t commands_executed{};
    std::uint64_t commands_replayed{};
    std::uint64_t commands_rejected{};
    std::uint64_t commands_conflicted{};
    std::uint64_t command_queue_rejections{};
    std::size_t command_queue_depth{};
    std::size_t command_queue_bytes{};
    std::size_t command_queue_high_watermark{};
    std::size_t command_queue_byte_high_watermark{};
    std::size_t retained_command_results{};
    std::size_t retained_command_result_bytes{};
    std::chrono::milliseconds current_reconnect_delay{};
    std::string last_heartbeat_at;
    std::string last_success_at;
    std::string last_error_code;
};

/// Owns the single uplink worker that performs all synchronous transport calls.
///
/// The transport command callback only copies into a bounded queue. Service commands and network
/// sends always run on the worker, never on a transport callback or camera acquisition thread.
class UplinkRuntime final
{
  private:
    struct Impl;
    struct ValidatedTag final
    {
    };

  public:
    ~UplinkRuntime();

    UplinkRuntime(const UplinkRuntime&) = delete;
    UplinkRuntime& operator=(const UplinkRuntime&) = delete;
    UplinkRuntime(UplinkRuntime&&) = delete;
    UplinkRuntime& operator=(UplinkRuntime&&) = delete;

    [[nodiscard]] static Result<std::unique_ptr<UplinkRuntime>> create(
        std::shared_ptr<IUplinkTransport> transport, UplinkRuntimeConfig config,
        UplinkStatusProvider status_provider, RemoteCommandExecutor command_executor);

    /// Public only for std::make_unique; ValidatedTag is private so callers must use create().
    UplinkRuntime(ValidatedTag, std::shared_ptr<Impl> impl);

    [[nodiscard]] Result<void> start();
    void request_stop() noexcept;
    [[nodiscard]] Result<void> join(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] UplinkRuntimeSnapshot snapshot() const noexcept;

  private:
    std::shared_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view uplink_runtime_state_name(UplinkRuntimeState state) noexcept;

} // namespace paperbreak::uplink
