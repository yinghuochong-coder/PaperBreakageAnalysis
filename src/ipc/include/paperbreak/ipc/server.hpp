#pragma once

#include "paperbreak/common/result.hpp"
#include "paperbreak/ipc/protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>

namespace paperbreak::ipc
{

struct PeerIdentity final
{
    std::string actor_sid;
    std::uint64_t connection_id{};
    bool local{};
    bool authenticated{};
    bool administrator{};
};

class IPeerAuthorizer
{
  public:
    virtual ~IPeerAuthorizer() = default;
    [[nodiscard]] virtual Result<PeerIdentity> authorize(
        std::uintptr_t native_descriptor) noexcept = 0;
};

[[nodiscard]] std::unique_ptr<IPeerAuthorizer> make_windows_peer_authorizer();

class IRequestHandler
{
  public:
    virtual ~IRequestHandler() = default;
    [[nodiscard]] virtual Result<CommandResponse> handle(const RequestMessage& request,
                                                         const PeerIdentity& peer,
                                                         std::stop_token stop_token) = 0;
};

enum class PushPolicy
{
    drop_newest,
    coalesce_latest,
};

struct IpcServerOptions final
{
    std::string server_name{default_server_name};
    std::wstring instance_guard_name{L"Global\\PaperBreakEdgeService.Ipc.ServerGuard"};
    std::size_t maximum_connections{4U};
    std::size_t maximum_in_flight_per_connection{16U};
    std::size_t recent_request_ids_per_connection{1024U};
    std::size_t command_queue_capacity{64U};
    std::size_t outbound_message_capacity{128U};
    std::size_t push_queue_capacity{32U};
    std::size_t outbound_byte_capacity{32U * 1024U * 1024U};
    std::size_t publish_ingress_capacity{64U};
    std::chrono::milliseconds incomplete_frame_timeout{std::chrono::seconds{5}};
    std::chrono::milliseconds startup_timeout{std::chrono::seconds{5}};
    std::chrono::milliseconds shutdown_flush_timeout{std::chrono::milliseconds{250}};
};

struct IpcServerMetrics final
{
    std::uint64_t active_connections{};
    std::uint64_t in_flight_requests{};
    std::uint64_t command_queue_depth{};
    std::uint64_t command_queue_high_watermark{};
    std::uint64_t publish_queue_depth{};
    std::uint64_t publish_queue_high_watermark{};
    std::uint64_t outbound_messages{};
    std::uint64_t outbound_bytes{};
    std::uint64_t requests_total{};
    std::uint64_t responses_total{};
    std::uint64_t protocol_errors_total{};
    std::uint64_t pushes_dropped_total{};
    double average_request_duration_ms{};
    double maximum_request_duration_ms{};
};

class IpcServer final
{
  public:
    IpcServer(std::shared_ptr<IRequestHandler> handler,
              std::unique_ptr<IPeerAuthorizer> authorizer = make_windows_peer_authorizer(),
              IpcServerOptions options = {});
    ~IpcServer();

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;
    IpcServer(IpcServer&&) = delete;
    IpcServer& operator=(IpcServer&&) = delete;

    [[nodiscard]] Result<void> start();
    void request_stop() noexcept;
    [[nodiscard]] Result<void> join(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] bool try_publish(PushMessage push,
                                   PushPolicy policy = PushPolicy::drop_newest) noexcept;
    [[nodiscard]] IpcServerMetrics metrics_snapshot() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace paperbreak::ipc
