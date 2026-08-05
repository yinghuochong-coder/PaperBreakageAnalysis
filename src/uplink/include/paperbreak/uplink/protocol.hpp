#pragma once

#include "paperbreak/common/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace paperbreak::uplink
{

inline constexpr std::uint32_t protocol_version = 1U;
inline constexpr std::size_t maximum_json_message_bytes = 1024U * 1024U;
inline constexpr std::size_t maximum_preview_header_bytes = 64U * 1024U;
inline constexpr std::size_t maximum_preview_jpeg_bytes = 2U * 1024U * 1024U;
inline constexpr std::size_t maximum_chunk_bytes = 4U * 1024U * 1024U;
inline constexpr std::uint64_t maximum_file_bytes = 64ULL * 1024ULL * 1024ULL * 1024ULL;

struct SessionHello final
{
    std::string request_id;
    std::string machine_id;
    std::string production_line_id;
    std::string software_version;
    std::vector<std::uint32_t> supported_protocol_versions;
    std::vector<std::string> capabilities;
};

struct MessageEnvelope final
{
    std::uint32_t protocol_version{};
    std::string message_type;
    std::string message_id;
    std::string machine_id;
    std::uint64_t sequence{};
    std::string timestamp;
    std::string payload_json;
};

struct UploadCreateRequest final
{
    std::string request_id;
    std::string event_id;
    std::string logical_file_id;
    std::string file_name;
    std::string content_type;
    std::uint64_t total_bytes{};
    std::uint32_t chunk_bytes{};
    std::string sha256;
};

struct PreviewFrame final
{
    std::string machine_id;
    std::string camera_id;
    std::string message_id;
    std::uint64_t sequence{};
    std::string timestamp;
    std::vector<std::byte> jpeg;
};

/// Validates a path-safe external identifier with an explicit byte limit.
[[nodiscard]] Result<void> validate_identifier(std::string_view value, std::string_view field,
                                               std::size_t maximum_bytes) noexcept;

/// Parses a strict Uplink v1 session request. Unknown fields are rejected except `extensions`.
[[nodiscard]] Result<SessionHello> parse_session_hello(std::string_view json) noexcept;

/// Parses a strict WebSocket text envelope and retains its payload as canonical JSON.
[[nodiscard]] Result<MessageEnvelope> parse_message_envelope(std::string_view json) noexcept;

/// Serializes a v1 text envelope with a parsed JSON payload.
[[nodiscard]] Result<std::string> serialize_message_envelope(
    const MessageEnvelope& envelope) noexcept;

/// Parses a strict upload creation request.
[[nodiscard]] Result<UploadCreateRequest> parse_upload_create(std::string_view json) noexcept;

/// Encodes/decodes a binary preview message: 4-byte little-endian header size, JSON header, JPEG.
[[nodiscard]] Result<std::vector<std::byte>> encode_preview_frame(
    const PreviewFrame& frame) noexcept;
[[nodiscard]] Result<PreviewFrame> decode_preview_frame(
    std::span<const std::byte> message) noexcept;

/// Returns true only for lower-case, 64-character SHA-256 hex strings.
[[nodiscard]] bool is_sha256_hex(std::string_view value) noexcept;

} // namespace paperbreak::uplink
