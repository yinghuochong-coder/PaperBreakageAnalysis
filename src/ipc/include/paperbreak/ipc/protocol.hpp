#pragma once

#include "paperbreak/common/error.hpp"
#include "paperbreak/common/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace paperbreak::ipc
{

inline constexpr std::uint32_t protocol_version = 1U;
inline constexpr std::string_view default_server_name = "PaperBreakEdgeService.Ipc";
inline constexpr std::size_t frame_prefix_bytes = 8U;
inline constexpr std::size_t maximum_header_bytes = 1024U * 1024U;
inline constexpr std::size_t maximum_binary_bytes = 16U * 1024U * 1024U;

struct Frame final
{
    std::string header_json;
    std::vector<std::byte> binary;
};

struct RequestMessage final
{
    std::string request_id;
    std::string command;
    std::string timestamp;
    std::string payload_json{"{}"};
    std::vector<std::byte> binary;
};

struct CommandResponse final
{
    std::string payload_json{"{}"};
    std::vector<std::byte> binary;
};

struct ResponseMessage final
{
    std::string request_id;
    bool success{};
    std::string timestamp;
    std::string payload_json{"{}"};
    std::optional<Error> error;
    std::vector<std::byte> binary;
};

struct PushMessage final
{
    std::string event_name;
    std::string timestamp;
    std::string payload_json{"{}"};
    std::vector<std::byte> binary;
    std::string coalescing_key;
};

using ServerMessage = std::variant<ResponseMessage, PushMessage>;

class FrameDecoder final
{
  public:
    [[nodiscard]] Result<std::vector<Frame>> append(std::span<const std::byte> bytes);
    [[nodiscard]] bool has_pending_data() const noexcept;
    void reset() noexcept;

  private:
    std::vector<std::byte> buffer_;
};

[[nodiscard]] Result<std::vector<std::byte>> encode_frame(const Frame& frame);
[[nodiscard]] Result<Frame> encode_request(const RequestMessage& request);
[[nodiscard]] Result<RequestMessage> decode_request(const Frame& frame);
[[nodiscard]] Result<Frame> encode_response(const ResponseMessage& response);
[[nodiscard]] Result<ResponseMessage> decode_response(const Frame& frame);
[[nodiscard]] Result<Frame> encode_push(const PushMessage& push);
[[nodiscard]] Result<PushMessage> decode_push(const Frame& frame);
[[nodiscard]] Result<ServerMessage> decode_server_message(const Frame& frame);

} // namespace paperbreak::ipc
