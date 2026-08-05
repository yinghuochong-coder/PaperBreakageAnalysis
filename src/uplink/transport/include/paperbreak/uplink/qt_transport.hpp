#pragma once

#include "paperbreak/uplink/transport.hpp"
#include "paperbreak/uplink/upload_scheduler.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace paperbreak::uplink
{

inline constexpr std::uint32_t default_upload_chunk_bytes = 1024U * 1024U;
inline constexpr std::uint64_t default_upload_limit_bytes_per_second = 20ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t maximum_upload_limit_bytes_per_second = 1024ULL * 1024ULL * 1024ULL;

struct QtUplinkTransportConfig final
{
    std::string server_url;
    std::chrono::milliseconds io_timeout{std::chrono::seconds{10}};
    std::uint32_t chunk_bytes{default_upload_chunk_bytes};
    std::uint64_t upload_limit_bytes_per_second{default_upload_limit_bytes_per_second};
};

/// Production Uplink v1 adapter backed by Qt Network and Qt WebSockets.
/// Public headers intentionally expose no Qt types.
class QtUplinkTransport final : public IUplinkTransport
{
  private:
    struct Impl;
    struct ValidatedTag final
    {
    };

  public:
    ~QtUplinkTransport() override;

    QtUplinkTransport(const QtUplinkTransport&) = delete;
    QtUplinkTransport& operator=(const QtUplinkTransport&) = delete;

    [[nodiscard]] static Result<std::unique_ptr<QtUplinkTransport>> create(
        QtUplinkTransportConfig config);
    QtUplinkTransport(ValidatedTag, std::shared_ptr<Impl> impl);

    [[nodiscard]] Result<TransportSession> connect(const SessionHello& hello) override;
    void disconnect() noexcept override;
    [[nodiscard]] UplinkConnectionState connection_state() const noexcept override;
    [[nodiscard]] Result<TransportAcknowledgement> send_heartbeat(
        const MessageEnvelope& heartbeat) override;
    [[nodiscard]] Result<TransportAcknowledgement> send_control_message(
        const MessageEnvelope& message) override;
    [[nodiscard]] Result<TransportAcknowledgement> send_event_metadata(
        const EventMetadataRequest& event) override;
    [[nodiscard]] Result<TransportAcknowledgement> upload_file(
        const UploadFileRequest& request) override;
    void set_command_handler(CommandHandler handler) override;

  private:
    std::shared_ptr<Impl> impl_;
};

struct ChunkedUploadExecutorConfig final
{
    std::filesystem::path event_root;
    std::string machine_id;
    std::uint32_t chunk_bytes{default_upload_chunk_bytes};
};

/// Adapts persisted M8-03 jobs to Uplink v1 alarm or resumable file calls.
[[nodiscard]] Result<UploadJobExecutor> make_chunked_upload_executor(
    std::shared_ptr<IUplinkTransport> transport, ChunkedUploadExecutorConfig config);

} // namespace paperbreak::uplink
