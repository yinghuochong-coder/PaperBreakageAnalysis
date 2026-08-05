#pragma once

#include "paperbreak/common/result.hpp"
#include "paperbreak/uplink/protocol.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace paperbreak::uplink
{

enum class UplinkConnectionState
{
    disconnected,
    connected,
};

enum class UplinkOperation
{
    connect,
    heartbeat,
    control_message,
    event_metadata,
    upload_file,
};

struct TransportSession final
{
    std::string session_id;
    std::string machine_id;
    std::uint32_t negotiated_protocol_version{protocol_version};
    std::uint32_t heartbeat_seconds{5U};
};

/// A bounded representation of one or two identical server confirmations.
struct TransportAcknowledgement final
{
    std::string correlation_id;
    std::string acknowledged_at;
    std::uint32_t delivery_count{1U};
};

struct EventMetadataRequest final
{
    std::string request_id;
    std::string machine_id;
    std::string event_id;
    std::string metadata_json;
};

/// A high-level file request. Chunking and checkpoint handling belong to M8-04.
struct UploadFileRequest final
{
    std::string machine_id;
    UploadCreateRequest description;
    std::string source_path;
};

using CommandHandler = std::function<void(const MessageEnvelope& command)>;

/// Transport-independent edge-side uplink port.
///
/// Calls are synchronous and must not be made from a camera acquisition callback. The M8-02/M8-03
/// orchestration layer owns worker threads, retry policy, and persistent queues.
class IUplinkTransport
{
  public:
    virtual ~IUplinkTransport() = default;

    [[nodiscard]] virtual Result<TransportSession> connect(const SessionHello& hello) = 0;
    virtual void disconnect() noexcept = 0;
    [[nodiscard]] virtual UplinkConnectionState connection_state() const noexcept = 0;

    [[nodiscard]] virtual Result<TransportAcknowledgement> send_heartbeat(
        const MessageEnvelope& heartbeat) = 0;
    [[nodiscard]] virtual Result<TransportAcknowledgement> send_control_message(
        const MessageEnvelope& message) = 0;
    [[nodiscard]] virtual Result<TransportAcknowledgement> send_event_metadata(
        const EventMetadataRequest& event) = 0;
    [[nodiscard]] virtual Result<TransportAcknowledgement> upload_file(
        const UploadFileRequest& request) = 0;

    virtual void set_command_handler(CommandHandler handler) = 0;
};

} // namespace paperbreak::uplink
