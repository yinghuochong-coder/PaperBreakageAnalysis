#include "paperbreak/ipc/protocol.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace paperbreak::ipc
{
namespace
{

using Json = nlohmann::json;

Error ipc_error(std::string code, std::string message, std::string operation,
                const std::optional<std::string>& request_id = std::nullopt)
{
    Error error = make_error(std::move(code), Severity::error, std::move(message), "ipc",
                             std::move(operation));
    error.correlation_id = request_id;
    return error;
}

std::uint32_t read_u32_be(const std::byte* bytes) noexcept
{
    return (std::to_integer<std::uint32_t>(bytes[0]) << 24U) |
           (std::to_integer<std::uint32_t>(bytes[1]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[2]) << 8U) |
           std::to_integer<std::uint32_t>(bytes[3]);
}

void append_u32_be(std::vector<std::byte>& output, const std::uint32_t value)
{
    output.push_back(static_cast<std::byte>((value >> 24U) & 0xffU));
    output.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::byte>(value & 0xffU));
}

bool is_hex(const char value) noexcept
{
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

bool is_canonical_uuid(const std::string_view value) noexcept
{
    if (value.size() != 36U)
    {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index)
    {
        const bool separator = index == 8U || index == 13U || index == 18U || index == 23U;
        if ((separator && value[index] != '-') || (!separator && !is_hex(value[index])))
        {
            return false;
        }
    }
    return true;
}

bool is_rfc3339_timestamp(const std::string_view value) noexcept
{
    if (value.size() < 20U || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':')
    {
        return false;
    }

    constexpr std::array<std::size_t, 14> digit_positions{0U, 1U,  2U,  3U,  5U,  6U,  8U,
                                                          9U, 11U, 12U, 14U, 15U, 17U, 18U};
    if (!std::ranges::all_of(digit_positions, [&value](const std::size_t position) {
            return value[position] >= '0' && value[position] <= '9';
        }))
    {
        return false;
    }
    const auto two_digits = [&value](const std::size_t position) {
        return static_cast<unsigned>(value[position] - '0') * 10U +
               static_cast<unsigned>(value[position + 1U] - '0');
    };
    if (two_digits(5U) < 1U || two_digits(5U) > 12U || two_digits(8U) < 1U ||
        two_digits(8U) > 31U || two_digits(11U) > 23U || two_digits(14U) > 59U ||
        two_digits(17U) > 59U)
    {
        return false;
    }

    const auto zone = value.find_first_of("Z+-", 19U);
    if (zone == std::string_view::npos)
    {
        return false;
    }
    if (zone > 19U)
    {
        if (value[19] != '.' || zone == 20U ||
            !std::ranges::all_of(value.substr(20U, zone - 20U), [](const char character) {
                return character >= '0' && character <= '9';
            }))
        {
            return false;
        }
    }
    if (value[zone] == 'Z')
    {
        return zone + 1U == value.size();
    }
    return zone + 6U == value.size() && value[zone + 3U] == ':' && value[zone + 1U] >= '0' &&
           value[zone + 1U] <= '9' && value[zone + 2U] >= '0' && value[zone + 2U] <= '9' &&
           value[zone + 4U] >= '0' && value[zone + 4U] <= '9' && value[zone + 5U] >= '0' &&
           value[zone + 5U] <= '9' && two_digits(zone + 1U) <= 23U && two_digits(zone + 4U) <= 59U;
}

std::string severity_name(const Severity severity)
{
    switch (severity)
    {
    case Severity::info:
        return "Info";
    case Severity::warning:
        return "Warning";
    case Severity::error:
        return "Error";
    case Severity::critical:
        return "Critical";
    }
    return "Error";
}

Result<Json> parse_payload_object(const std::string_view payload, const std::string_view operation)
{
    Json document = Json::parse(payload, nullptr, false);
    if (document.is_discarded() || !document.is_object())
    {
        return Result<Json>::failure(ipc_error(
            "IPC_REQUEST_INVALID", "IPC payload 必须是 JSON 对象", std::string{operation}));
    }
    return Result<Json>::success(std::move(document));
}

Json public_error(const Error& error)
{
    Json details = Json::object();
    constexpr std::size_t maximum_details = 32U;
    constexpr std::size_t maximum_key_bytes = 64U;
    constexpr std::size_t maximum_value_bytes = 512U;
    const std::size_t count = std::min(error.details.size(), maximum_details);
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto& detail = error.details[index];
        details[detail.key.substr(0U, maximum_key_bytes)] =
            detail.value.substr(0U, maximum_value_bytes);
    }
    Json document{{"businessCode", error.business_code},
                  {"severity", severity_name(error.severity)},
                  {"message", error.message},
                  {"module", error.module},
                  {"operation", error.operation},
                  {"details", std::move(details)},
                  {"retryable", error.retryable},
                  {"timestamp", error.timestamp}};
    if (error.source_id.has_value())
    {
        document["sourceId"] = error.source_id.value();
    }
    if (error.correlation_id.has_value())
    {
        document["correlationId"] = error.correlation_id.value();
    }
    return document;
}

Result<Frame> encode_json_message(Json header, const std::vector<std::byte>& binary)
{
    const std::string text = header.dump();
    if (text.empty() || text.size() > maximum_header_bytes || binary.size() > maximum_binary_bytes)
    {
        return Result<Frame>::failure(
            ipc_error("IPC_MESSAGE_TOO_LARGE", "IPC 消息超过固定大小上限", "ipc.encode"));
    }
    return Result<Frame>::success({.header_json = text, .binary = binary});
}

} // namespace

Result<std::vector<Frame>> FrameDecoder::append(const std::span<const std::byte> bytes)
{
    constexpr std::size_t maximum_buffer_bytes =
        frame_prefix_bytes + maximum_header_bytes + maximum_binary_bytes;
    if (bytes.size() > maximum_buffer_bytes - std::min(buffer_.size(), maximum_buffer_bytes))
    {
        reset();
        return Result<std::vector<Frame>>::failure(
            ipc_error("IPC_MESSAGE_TOO_LARGE", "IPC 接收缓冲超过固定上限", "ipc.decode.length"));
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());

    std::vector<Frame> decoded;
    while (buffer_.size() >= frame_prefix_bytes)
    {
        const std::uint32_t header_length = read_u32_be(buffer_.data());
        const std::uint32_t binary_length = read_u32_be(buffer_.data() + 4U);
        if (header_length == 0U)
        {
            reset();
            return Result<std::vector<Frame>>::failure(ipc_error(
                "IPC_PROTOCOL_ERROR", "IPC JSON header 长度不能为零", "ipc.decode.length"));
        }
        if (header_length > maximum_header_bytes || binary_length > maximum_binary_bytes)
        {
            reset();
            return Result<std::vector<Frame>>::failure(ipc_error(
                "IPC_MESSAGE_TOO_LARGE", "IPC 帧声明长度超过固定上限", "ipc.decode.length"));
        }
        const std::size_t frame_size = frame_prefix_bytes + header_length + binary_length;
        if (buffer_.size() < frame_size)
        {
            break;
        }

        const auto header_begin = buffer_.begin() + static_cast<std::ptrdiff_t>(frame_prefix_bytes);
        const auto binary_begin = header_begin + static_cast<std::ptrdiff_t>(header_length);
        Frame frame;
        frame.header_json.assign(reinterpret_cast<const char*>(&*header_begin), header_length);
        frame.binary.assign(binary_begin,
                            binary_begin + static_cast<std::ptrdiff_t>(binary_length));
        decoded.push_back(std::move(frame));
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(frame_size));
    }
    return Result<std::vector<Frame>>::success(std::move(decoded));
}

bool FrameDecoder::has_pending_data() const noexcept
{
    return !buffer_.empty();
}

void FrameDecoder::reset() noexcept
{
    buffer_.clear();
}

Result<std::vector<std::byte>> encode_frame(const Frame& frame)
{
    if (frame.header_json.empty())
    {
        return Result<std::vector<std::byte>>::failure(
            ipc_error("IPC_PROTOCOL_ERROR", "IPC JSON header 不能为空", "ipc.encode.length"));
    }
    if (frame.header_json.size() > maximum_header_bytes ||
        frame.binary.size() > maximum_binary_bytes ||
        frame.header_json.size() > std::numeric_limits<std::uint32_t>::max() ||
        frame.binary.size() > std::numeric_limits<std::uint32_t>::max())
    {
        return Result<std::vector<std::byte>>::failure(
            ipc_error("IPC_MESSAGE_TOO_LARGE", "IPC 帧超过固定大小上限", "ipc.encode.length"));
    }

    std::vector<std::byte> output;
    output.reserve(frame_prefix_bytes + frame.header_json.size() + frame.binary.size());
    append_u32_be(output, static_cast<std::uint32_t>(frame.header_json.size()));
    append_u32_be(output, static_cast<std::uint32_t>(frame.binary.size()));
    for (const char value : frame.header_json)
    {
        output.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    output.insert(output.end(), frame.binary.begin(), frame.binary.end());
    return Result<std::vector<std::byte>>::success(std::move(output));
}

Result<RequestMessage> decode_request(const Frame& frame)
{
    Json header = Json::parse(frame.header_json, nullptr, false);
    if (header.is_discarded() || !header.is_object())
    {
        return Result<RequestMessage>::failure(
            ipc_error("IPC_PROTOCOL_ERROR", "IPC header 不是有效 JSON 对象", "ipc.decode.header"));
    }

    std::optional<std::string> request_id;
    if (header.contains("requestId") && header["requestId"].is_string())
    {
        const std::string candidate = header["requestId"].get<std::string>();
        if (is_canonical_uuid(candidate))
        {
            request_id = candidate;
        }
    }

    static const std::unordered_set<std::string> allowed_fields{
        "protocolVersion", "messageType", "requestId", "command",
        "timestamp",       "payload",     "extensions"};
    for (const auto& [key, value] : header.items())
    {
        static_cast<void>(value);
        if (!allowed_fields.contains(key))
        {
            return Result<RequestMessage>::failure(ipc_error("IPC_REQUEST_INVALID",
                                                             "IPC 请求包含未知顶层字段",
                                                             "ipc.decode.request", request_id));
        }
    }
    if (!header.contains("protocolVersion") || !(header["protocolVersion"].is_number_unsigned() ||
                                                 header["protocolVersion"].is_number_integer()))
    {
        return Result<RequestMessage>::failure(ipc_error("IPC_PROTOCOL_ERROR",
                                                         "IPC 请求缺少整数 protocolVersion",
                                                         "ipc.decode.version", request_id));
    }
    const bool unsigned_version = header["protocolVersion"].is_number_unsigned();
    const std::uint64_t positive_version =
        unsigned_version ? header["protocolVersion"].get<std::uint64_t>() : 0U;
    const std::int64_t signed_version =
        unsigned_version ? 0 : header["protocolVersion"].get<std::int64_t>();
    const bool supported = unsigned_version
                               ? positive_version == protocol_version
                               : signed_version == static_cast<std::int64_t>(protocol_version);
    if (!supported)
    {
        Error error = ipc_error("IPC_PROTOCOL_VERSION_UNSUPPORTED", "IPC 协议版本不受支持",
                                "ipc.decode.version", request_id);
        error.details.push_back({"supportedMinimum", std::to_string(protocol_version)});
        error.details.push_back({"supportedMaximum", std::to_string(protocol_version)});
        error.details.push_back({"receivedVersion", unsigned_version
                                                        ? std::to_string(positive_version)
                                                        : std::to_string(signed_version)});
        return Result<RequestMessage>::failure(std::move(error));
    }
    if (!header.contains("messageType") || !header["messageType"].is_string() ||
        header["messageType"].get<std::string>() != "request")
    {
        return Result<RequestMessage>::failure(ipc_error("IPC_PROTOCOL_ERROR",
                                                         "IPC messageType 必须是 request",
                                                         "ipc.decode.messageType", request_id));
    }
    if (!request_id.has_value())
    {
        return Result<RequestMessage>::failure(ipc_error(
            "IPC_REQUEST_INVALID", "IPC requestId 必须是规范 UUID", "ipc.decode.requestId"));
    }
    if (!header.contains("command") || !header["command"].is_string() ||
        header["command"].get_ref<const std::string&>().empty() ||
        header["command"].get_ref<const std::string&>().size() > 128U)
    {
        return Result<RequestMessage>::failure(
            ipc_error("IPC_REQUEST_INVALID", "IPC command 无效", "ipc.decode.command", request_id));
    }
    if (!header.contains("timestamp") || !header["timestamp"].is_string() ||
        !is_rfc3339_timestamp(header["timestamp"].get_ref<const std::string&>()))
    {
        return Result<RequestMessage>::failure(ipc_error("IPC_REQUEST_INVALID",
                                                         "IPC timestamp 必须是 RFC 3339",
                                                         "ipc.decode.timestamp", request_id));
    }
    if (!header.contains("payload") || !header["payload"].is_object())
    {
        return Result<RequestMessage>::failure(ipc_error("IPC_REQUEST_INVALID",
                                                         "IPC payload 必须是 JSON 对象",
                                                         "ipc.decode.payload", request_id));
    }
    if (header.contains("extensions") && !header["extensions"].is_object())
    {
        return Result<RequestMessage>::failure(ipc_error("IPC_REQUEST_INVALID",
                                                         "IPC extensions 必须是 JSON 对象",
                                                         "ipc.decode.extensions", request_id));
    }

    return Result<RequestMessage>::success({.request_id = request_id.value(),
                                            .command = header["command"].get<std::string>(),
                                            .timestamp = header["timestamp"].get<std::string>(),
                                            .payload_json = header["payload"].dump(),
                                            .binary = frame.binary});
}

Result<Frame> encode_response(const ResponseMessage& response)
{
    if (!is_canonical_uuid(response.request_id) ||
        (response.success && response.error.has_value()) ||
        (!response.success && !response.error.has_value()))
    {
        return Result<Frame>::failure(ipc_error(
            "SYS_INTERNAL_ERROR", "IPC 响应 DTO 不满足协议不变量", "ipc.encode.response"));
    }
    auto payload = parse_payload_object(response.payload_json, "ipc.encode.response");
    if (!payload)
    {
        return Result<Frame>::failure(payload.error());
    }
    Json header{{"protocolVersion", protocol_version}, {"messageType", "response"},
                {"requestId", response.request_id},    {"timestamp", response.timestamp},
                {"success", response.success},         {"payload", std::move(payload).value()}};
    header["error"] = response.error.has_value() ? public_error(response.error.value()) : Json{};
    return encode_json_message(std::move(header), response.binary);
}

Result<Frame> encode_push(const PushMessage& push)
{
    if (push.event_name.empty() || push.event_name.size() > 128U)
    {
        return Result<Frame>::failure(
            ipc_error("IPC_REQUEST_INVALID", "IPC 推送 eventName 无效", "ipc.encode.push"));
    }
    auto payload = parse_payload_object(push.payload_json, "ipc.encode.push");
    if (!payload)
    {
        return Result<Frame>::failure(payload.error());
    }
    Json header{{"protocolVersion", protocol_version},
                {"messageType", "push"},
                {"eventName", push.event_name},
                {"timestamp", push.timestamp},
                {"payload", std::move(payload).value()}};
    return encode_json_message(std::move(header), push.binary);
}

} // namespace paperbreak::ipc
