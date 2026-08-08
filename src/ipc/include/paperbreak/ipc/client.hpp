#pragma once

#include "paperbreak/common/result.hpp"
#include "paperbreak/common/threading.hpp"
#include "paperbreak/ipc/protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace paperbreak::ipc
{

enum class ClientConnectionState
{
    stopped,
    connecting,
    connected,
    retry_wait,
};

struct ClientConnectionSnapshot final
{
    ClientConnectionState state{ClientConnectionState::stopped};
    std::uint64_t generation{};
    std::size_t reconnect_attempt{};
    std::optional<std::chrono::milliseconds> next_retry_delay;
    std::optional<Error> last_error;
};

struct ClientRequestHandle final
{
    std::string request_id;
    std::uint64_t generation{};

    [[nodiscard]] bool operator==(const ClientRequestHandle&) const = default;
};

using RequestCompletion = std::function<void(ClientRequestHandle, Result<ResponseMessage>)>;
using ConnectionCallback = std::function<void(const ClientConnectionSnapshot&)>;
using PushCallback = std::function<void(std::uint64_t, const PushMessage&)>;

struct IpcClientCallbacks final
{
    ConnectionCallback connection_changed;
    PushCallback push_received;
};

struct IpcClientOptions final
{
    std::string server_name{default_server_name};
    std::size_t maximum_pending_requests{128U};
    std::size_t outbound_byte_capacity{32U * 1024U * 1024U};
    std::chrono::milliseconds connect_timeout{std::chrono::seconds{2}};
    std::chrono::milliseconds default_request_timeout{std::chrono::seconds{5}};
    std::chrono::milliseconds initial_reconnect_delay{std::chrono::milliseconds{250}};
    std::chrono::milliseconds maximum_reconnect_delay{std::chrono::seconds{10}};
    std::chrono::milliseconds stable_connection_reset{std::chrono::seconds{5}};
    double reconnect_jitter_fraction{0.20};
    DebugDiagnosticSink diagnostics;
};

class IpcClient final
{
  public:
    // Construct, call, and destroy the client on one thread with an active Qt event loop.
    // Callbacks are synchronous on that same thread and must remain short.
    explicit IpcClient(IpcClientCallbacks callbacks = {}, IpcClientOptions options = {});
    ~IpcClient();

    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;
    IpcClient(IpcClient&&) = delete;
    IpcClient& operator=(IpcClient&&) = delete;

    [[nodiscard]] Result<void> start();
    void stop() noexcept;
    [[nodiscard]] ClientConnectionSnapshot snapshot() const;

    [[nodiscard]] Result<ClientRequestHandle> send_request(
        std::string command, std::string payload_json, std::vector<std::byte> binary,
        RequestCompletion completion,
        std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());
    [[nodiscard]] bool cancel_request(const ClientRequestHandle& handle);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace paperbreak::ipc
