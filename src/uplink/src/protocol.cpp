#include "paperbreak/uplink/protocol.hpp"

#include "paperbreak/common/error.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace paperbreak::uplink
{
namespace
{
using Json = nlohmann::json;

Error protocol_error(std::string message, std::string operation, std::string field = {})
{
    auto error = make_error("UPLINK_PROTOCOL_ERROR", Severity::error, std::move(message), "uplink",
                            std::move(operation), false);
    if (!field.empty())
        error.details.push_back({.key = "field", .value = std::move(field)});
    return error;
}

bool has_only_fields(const Json& value, const std::initializer_list<std::string_view> fields)
{
    if (!value.is_object())
        return false;
    const std::set<std::string_view> allowed(fields.begin(), fields.end());
    return std::ranges::all_of(
        value.items(), [&allowed](const auto& item) { return allowed.contains(item.key()); });
}

Result<Json> parse_json(const std::string_view text, const std::string_view operation)
{
    if (text.empty() || text.size() > maximum_json_message_bytes)
        return Result<Json>::failure(
            protocol_error("JSON 消息为空或超过 1 MiB 上限", std::string{operation}, "message"));
    auto value = Json::parse(text, nullptr, false);
    if (value.is_discarded() || !value.is_object())
        return Result<Json>::failure(
            protocol_error("JSON 消息必须是有效对象", std::string{operation}, "message"));
    return Result<Json>::success(std::move(value));
}

Result<std::string> required_string(const Json& value, const std::string_view name,
                                    const std::size_t maximum, const std::string_view operation,
                                    const bool identifier = false)
{
    const auto iterator = value.find(name);
    if (iterator == value.end() || !iterator->is_string())
        return Result<std::string>::failure(
            protocol_error("缺少必需字符串字段", std::string{operation}, std::string{name}));
    auto result = iterator->get<std::string>();
    if (result.empty() || result.size() > maximum)
        return Result<std::string>::failure(
            protocol_error("字符串字段为空或超过上限", std::string{operation}, std::string{name}));
    if (identifier)
    {
        auto valid = validate_identifier(result, name, maximum);
        if (!valid)
            return Result<std::string>::failure(valid.error());
    }
    return Result<std::string>::success(std::move(result));
}

Result<std::uint64_t> required_unsigned(const Json& value, const std::string_view name,
                                        const std::uint64_t maximum,
                                        const std::string_view operation)
{
    const auto iterator = value.find(name);
    if (iterator == value.end() || !iterator->is_number_unsigned())
        return Result<std::uint64_t>::failure(
            protocol_error("缺少必需无符号整数字段", std::string{operation}, std::string{name}));
    const auto result = iterator->get<std::uint64_t>();
    if (result > maximum)
        return Result<std::uint64_t>::failure(
            protocol_error("无符号整数字段超过上限", std::string{operation}, std::string{name}));
    return Result<std::uint64_t>::success(result);
}

Result<void> validate_extensions(const Json& value, const std::string_view operation)
{
    const auto iterator = value.find("extensions");
    if (iterator != value.end() && !iterator->is_object())
        return Result<void>::failure(
            protocol_error("extensions 必须是对象", std::string{operation}, "extensions"));
    return Result<void>::success();
}

template <typename T> Result<T> propagate(const Result<std::string>& result)
{
    return Result<T>::failure(result.error());
}

} // namespace

Result<void> validate_identifier(const std::string_view value, const std::string_view field,
                                 const std::size_t maximum_bytes) noexcept
{
    try
    {
        if (value.empty() || value.size() > maximum_bytes)
            return Result<void>::failure(
                protocol_error("标识符为空或超过上限", "uplink.validate", std::string{field}));
        if (!std::isalnum(static_cast<unsigned char>(value.front())))
            return Result<void>::failure(protocol_error("标识符必须以字母或数字开头",
                                                        "uplink.validate", std::string{field}));
        const bool valid = std::ranges::all_of(value, [](const char character) {
            const auto byte = static_cast<unsigned char>(character);
            return std::isalnum(byte) || character == '.' || character == '_' || character == '-';
        });
        if (!valid || value == "." || value == ".." || value.find("..") != std::string_view::npos)
            return Result<void>::failure(protocol_error("标识符包含非法或路径逃逸字符",
                                                        "uplink.validate", std::string{field}));
        return Result<void>::success();
    }
    catch (...)
    {
        return Result<void>::failure(
            protocol_error("标识符校验异常", "uplink.validate", std::string{field}));
    }
}

Result<SessionHello> parse_session_hello(const std::string_view json) noexcept
{
    try
    {
        auto parsed = parse_json(json, "uplink.session.parse");
        if (!parsed)
            return Result<SessionHello>::failure(parsed.error());
        const auto& value = parsed.value();
        if (!has_only_fields(value,
                             {"requestId", "machineId", "productionLineId", "softwareVersion",
                              "supportedProtocolVersions", "capabilities", "extensions"}))
            return Result<SessionHello>::failure(
                protocol_error("会话请求包含未知字段", "uplink.session.parse", "request"));
        auto extensions = validate_extensions(value, "uplink.session.parse");
        if (!extensions)
            return Result<SessionHello>::failure(extensions.error());

        auto request_id = required_string(value, "requestId", 128U, "uplink.session.parse", true);
        auto machine_id = required_string(value, "machineId", 64U, "uplink.session.parse", true);
        auto line_id =
            required_string(value, "productionLineId", 64U, "uplink.session.parse", true);
        auto version = required_string(value, "softwareVersion", 128U, "uplink.session.parse");
        if (!request_id)
            return propagate<SessionHello>(request_id);
        if (!machine_id)
            return propagate<SessionHello>(machine_id);
        if (!line_id)
            return propagate<SessionHello>(line_id);
        if (!version)
            return propagate<SessionHello>(version);

        const auto protocol_iterator = value.find("supportedProtocolVersions");
        const auto capabilities_iterator = value.find("capabilities");
        if (protocol_iterator == value.end() || !protocol_iterator->is_array() ||
            protocol_iterator->empty() || protocol_iterator->size() > 16U)
            return Result<SessionHello>::failure(
                protocol_error("supportedProtocolVersions 必须是 1～16 项数组",
                               "uplink.session.parse", "supportedProtocolVersions"));
        if (capabilities_iterator == value.end() || !capabilities_iterator->is_array() ||
            capabilities_iterator->size() > 64U)
            return Result<SessionHello>::failure(protocol_error(
                "capabilities 必须是不超过 64 项的数组", "uplink.session.parse", "capabilities"));

        SessionHello result{.request_id = std::move(request_id).value(),
                            .machine_id = std::move(machine_id).value(),
                            .production_line_id = std::move(line_id).value(),
                            .software_version = std::move(version).value()};
        std::set<std::uint32_t> protocol_versions;
        for (const auto& item : *protocol_iterator)
        {
            if (!item.is_number_unsigned() || item.get<std::uint64_t>() == 0U ||
                item.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
                return Result<SessionHello>::failure(protocol_error(
                    "协议版本必须是正整数", "uplink.session.parse", "supportedProtocolVersions"));
            protocol_versions.insert(item.get<std::uint32_t>());
        }
        result.supported_protocol_versions.assign(protocol_versions.begin(),
                                                  protocol_versions.end());

        std::set<std::string> capabilities;
        for (const auto& item : *capabilities_iterator)
        {
            if (!item.is_string())
                return Result<SessionHello>::failure(
                    protocol_error("能力名称必须是字符串", "uplink.session.parse", "capabilities"));
            const auto capability = item.get<std::string>();
            auto valid = validate_identifier(capability, "capabilities", 128U);
            if (!valid)
                return Result<SessionHello>::failure(valid.error());
            capabilities.insert(capability);
        }
        result.capabilities.assign(capabilities.begin(), capabilities.end());
        return Result<SessionHello>::success(std::move(result));
    }
    catch (...)
    {
        return Result<SessionHello>::failure(
            protocol_error("会话请求解析异常", "uplink.session.parse"));
    }
}

Result<MessageEnvelope> parse_message_envelope(const std::string_view json) noexcept
{
    try
    {
        auto parsed = parse_json(json, "uplink.message.parse");
        if (!parsed)
            return Result<MessageEnvelope>::failure(parsed.error());
        const auto& value = parsed.value();
        if (!has_only_fields(value, {"protocolVersion", "messageType", "messageId", "machineId",
                                     "sequence", "timestamp", "payload", "extensions"}))
            return Result<MessageEnvelope>::failure(
                protocol_error("消息信封包含未知字段", "uplink.message.parse", "message"));
        auto extensions = validate_extensions(value, "uplink.message.parse");
        if (!extensions)
            return Result<MessageEnvelope>::failure(extensions.error());
        auto version =
            required_unsigned(value, "protocolVersion", std::numeric_limits<std::uint32_t>::max(),
                              "uplink.message.parse");
        if (!version)
            return Result<MessageEnvelope>::failure(version.error());
        if (version.value() != protocol_version)
        {
            auto error =
                make_error("UPLINK_PROTOCOL_VERSION_UNSUPPORTED", Severity::error,
                           "不支持收到的 Uplink 协议版本", "uplink", "uplink.message.parse", false);
            error.details.push_back(
                {.key = "receivedVersion", .value = std::to_string(version.value())});
            error.details.push_back(
                {.key = "supportedVersion", .value = std::to_string(protocol_version)});
            return Result<MessageEnvelope>::failure(std::move(error));
        }
        auto type = required_string(value, "messageType", 128U, "uplink.message.parse", true);
        auto id = required_string(value, "messageId", 128U, "uplink.message.parse", true);
        auto machine = required_string(value, "machineId", 64U, "uplink.message.parse", true);
        auto sequence = required_unsigned(
            value, "sequence", std::numeric_limits<std::uint64_t>::max(), "uplink.message.parse");
        auto timestamp = required_string(value, "timestamp", 64U, "uplink.message.parse");
        if (!type)
            return propagate<MessageEnvelope>(type);
        if (!id)
            return propagate<MessageEnvelope>(id);
        if (!machine)
            return propagate<MessageEnvelope>(machine);
        if (!sequence)
            return Result<MessageEnvelope>::failure(sequence.error());
        if (!timestamp)
            return propagate<MessageEnvelope>(timestamp);
        const auto payload = value.find("payload");
        if (payload == value.end() || !payload->is_object())
            return Result<MessageEnvelope>::failure(
                protocol_error("payload 必须是对象", "uplink.message.parse", "payload"));
        return Result<MessageEnvelope>::success({.protocol_version = protocol_version,
                                                 .message_type = std::move(type).value(),
                                                 .message_id = std::move(id).value(),
                                                 .machine_id = std::move(machine).value(),
                                                 .sequence = sequence.value(),
                                                 .timestamp = std::move(timestamp).value(),
                                                 .payload_json = payload->dump()});
    }
    catch (...)
    {
        return Result<MessageEnvelope>::failure(
            protocol_error("消息信封解析异常", "uplink.message.parse"));
    }
}

Result<std::string> serialize_message_envelope(const MessageEnvelope& envelope) noexcept
{
    try
    {
        auto machine = validate_identifier(envelope.machine_id, "machineId", 64U);
        auto type = validate_identifier(envelope.message_type, "messageType", 128U);
        auto id = validate_identifier(envelope.message_id, "messageId", 128U);
        if (!machine)
            return Result<std::string>::failure(machine.error());
        if (!type)
            return Result<std::string>::failure(type.error());
        if (!id)
            return Result<std::string>::failure(id.error());
        if (envelope.protocol_version != protocol_version || envelope.timestamp.empty())
            return Result<std::string>::failure(
                protocol_error("消息信封版本或时间无效", "uplink.message.serialize"));
        auto payload = Json::parse(envelope.payload_json, nullptr, false);
        if (payload.is_discarded() || !payload.is_object())
            return Result<std::string>::failure(protocol_error(
                "消息 payload 不是有效 JSON 对象", "uplink.message.serialize", "payload"));
        const std::string result =
            Json{{"protocolVersion", protocol_version}, {"messageType", envelope.message_type},
                 {"messageId", envelope.message_id},    {"machineId", envelope.machine_id},
                 {"sequence", envelope.sequence},       {"timestamp", envelope.timestamp},
                 {"payload", std::move(payload)}}
                .dump();
        if (result.size() > maximum_json_message_bytes)
            return Result<std::string>::failure(
                protocol_error("序列化消息超过 1 MiB 上限", "uplink.message.serialize"));
        return Result<std::string>::success(result);
    }
    catch (...)
    {
        return Result<std::string>::failure(
            protocol_error("消息信封序列化异常", "uplink.message.serialize"));
    }
}

bool is_sha256_hex(const std::string_view value) noexcept
{
    return value.size() == 64U && std::ranges::all_of(value, [](const char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

Result<UploadCreateRequest> parse_upload_create(const std::string_view json) noexcept
{
    try
    {
        auto parsed = parse_json(json, "upload.create.parse");
        if (!parsed)
            return Result<UploadCreateRequest>::failure(parsed.error());
        const auto& value = parsed.value();
        if (!has_only_fields(value,
                             {"requestId", "eventId", "logicalFileId", "fileName", "contentType",
                              "totalBytes", "chunkBytes", "sha256", "extensions"}))
            return Result<UploadCreateRequest>::failure(
                protocol_error("上传创建请求包含未知字段", "upload.create.parse", "request"));
        auto extensions = validate_extensions(value, "upload.create.parse");
        if (!extensions)
            return Result<UploadCreateRequest>::failure(extensions.error());
        auto request = required_string(value, "requestId", 128U, "upload.create.parse", true);
        auto event = required_string(value, "eventId", 128U, "upload.create.parse", true);
        auto logical = required_string(value, "logicalFileId", 128U, "upload.create.parse", true);
        auto file_name = required_string(value, "fileName", 255U, "upload.create.parse");
        auto content = required_string(value, "contentType", 128U, "upload.create.parse");
        auto total =
            required_unsigned(value, "totalBytes", maximum_file_bytes, "upload.create.parse");
        auto chunk =
            required_unsigned(value, "chunkBytes", maximum_chunk_bytes, "upload.create.parse");
        auto digest = required_string(value, "sha256", 64U, "upload.create.parse");
        if (!request)
            return propagate<UploadCreateRequest>(request);
        if (!event)
            return propagate<UploadCreateRequest>(event);
        if (!logical)
            return propagate<UploadCreateRequest>(logical);
        if (!file_name)
            return propagate<UploadCreateRequest>(file_name);
        if (!content)
            return propagate<UploadCreateRequest>(content);
        if (!total)
            return Result<UploadCreateRequest>::failure(total.error());
        if (!chunk)
            return Result<UploadCreateRequest>::failure(chunk.error());
        if (total.value() == 0U || chunk.value() == 0U || !is_sha256_hex(digest.value()))
            return Result<UploadCreateRequest>::failure(protocol_error(
                "上传长度、分块长度或 SHA-256 无效", "upload.create.parse", "request"));
        return Result<UploadCreateRequest>::success(
            {.request_id = std::move(request).value(),
             .event_id = std::move(event).value(),
             .logical_file_id = std::move(logical).value(),
             .file_name = std::move(file_name).value(),
             .content_type = std::move(content).value(),
             .total_bytes = total.value(),
             .chunk_bytes = static_cast<std::uint32_t>(chunk.value()),
             .sha256 = std::move(digest).value()});
    }
    catch (...)
    {
        return Result<UploadCreateRequest>::failure(
            protocol_error("上传创建请求解析异常", "upload.create.parse"));
    }
}

Result<std::vector<std::byte>> encode_preview_frame(const PreviewFrame& frame) noexcept
{
    try
    {
        auto machine = validate_identifier(frame.machine_id, "machineId", 64U);
        auto camera = validate_identifier(frame.camera_id, "cameraId", 64U);
        auto message = validate_identifier(frame.message_id, "messageId", 128U);
        if (!machine)
            return Result<std::vector<std::byte>>::failure(machine.error());
        if (!camera)
            return Result<std::vector<std::byte>>::failure(camera.error());
        if (!message)
            return Result<std::vector<std::byte>>::failure(message.error());
        if (frame.timestamp.empty() || frame.jpeg.empty() ||
            frame.jpeg.size() > maximum_preview_jpeg_bytes)
            return Result<std::vector<std::byte>>::failure(
                protocol_error("预览时间或 JPEG 长度无效", "uplink.preview.encode", "preview"));
        const std::string header =
            Json{{"protocolVersion", protocol_version}, {"messageType", "preview.frame"},
                 {"messageId", frame.message_id},       {"machineId", frame.machine_id},
                 {"cameraId", frame.camera_id},         {"sequence", frame.sequence},
                 {"timestamp", frame.timestamp},        {"jpegBytes", frame.jpeg.size()}}
                .dump();
        if (header.size() > maximum_preview_header_bytes)
            return Result<std::vector<std::byte>>::failure(
                protocol_error("预览头超过上限", "uplink.preview.encode", "header"));
        const auto header_size = static_cast<std::uint32_t>(header.size());
        std::vector<std::byte> result(4U + header.size() + frame.jpeg.size());
        result[0] = static_cast<std::byte>(header_size & 0xffU);
        result[1] = static_cast<std::byte>((header_size >> 8U) & 0xffU);
        result[2] = static_cast<std::byte>((header_size >> 16U) & 0xffU);
        result[3] = static_cast<std::byte>((header_size >> 24U) & 0xffU);
        std::ranges::transform(header, result.begin() + 4, [](const char character) {
            return static_cast<std::byte>(static_cast<unsigned char>(character));
        });
        std::ranges::copy(frame.jpeg, result.begin() + 4 + header.size());
        return Result<std::vector<std::byte>>::success(std::move(result));
    }
    catch (...)
    {
        return Result<std::vector<std::byte>>::failure(
            protocol_error("预览编码异常", "uplink.preview.encode"));
    }
}

Result<PreviewFrame> decode_preview_frame(const std::span<const std::byte> message) noexcept
{
    try
    {
        if (message.size() < 5U ||
            message.size() > 4U + maximum_preview_header_bytes + maximum_preview_jpeg_bytes)
            return Result<PreviewFrame>::failure(
                protocol_error("预览二进制消息长度无效", "uplink.preview.decode", "message"));
        const auto header_size = std::to_integer<std::uint32_t>(message[0]) |
                                 (std::to_integer<std::uint32_t>(message[1]) << 8U) |
                                 (std::to_integer<std::uint32_t>(message[2]) << 16U) |
                                 (std::to_integer<std::uint32_t>(message[3]) << 24U);
        if (header_size == 0U || header_size > maximum_preview_header_bytes ||
            4ULL + header_size >= message.size())
            return Result<PreviewFrame>::failure(
                protocol_error("预览头长度无效", "uplink.preview.decode", "header"));
        const auto* header_data = reinterpret_cast<const char*>(message.data() + 4U);
        auto header = Json::parse(header_data, header_data + header_size, nullptr, false);
        if (header.is_discarded() || !header.is_object() ||
            !has_only_fields(header,
                             {"protocolVersion", "messageType", "messageId", "machineId",
                              "cameraId", "sequence", "timestamp", "jpegBytes", "extensions"}))
            return Result<PreviewFrame>::failure(
                protocol_error("预览头不是有效 v1 对象", "uplink.preview.decode", "header"));
        if (header.value("protocolVersion", 0U) != protocol_version ||
            header.value("messageType", std::string{}) != "preview.frame")
            return Result<PreviewFrame>::failure(
                protocol_error("预览协议版本或类型无效", "uplink.preview.decode", "header"));
        auto machine = required_string(header, "machineId", 64U, "uplink.preview.decode", true);
        auto camera = required_string(header, "cameraId", 64U, "uplink.preview.decode", true);
        auto id = required_string(header, "messageId", 128U, "uplink.preview.decode", true);
        auto sequence = required_unsigned(
            header, "sequence", std::numeric_limits<std::uint64_t>::max(), "uplink.preview.decode");
        auto timestamp = required_string(header, "timestamp", 64U, "uplink.preview.decode");
        auto jpeg_bytes = required_unsigned(header, "jpegBytes", maximum_preview_jpeg_bytes,
                                            "uplink.preview.decode");
        if (!machine)
            return propagate<PreviewFrame>(machine);
        if (!camera)
            return propagate<PreviewFrame>(camera);
        if (!id)
            return propagate<PreviewFrame>(id);
        if (!sequence)
            return Result<PreviewFrame>::failure(sequence.error());
        if (!timestamp)
            return propagate<PreviewFrame>(timestamp);
        if (!jpeg_bytes || jpeg_bytes.value() == 0U ||
            jpeg_bytes.value() != message.size() - 4U - header_size)
            return Result<PreviewFrame>::failure(
                protocol_error("预览 JPEG 长度与消息不一致", "uplink.preview.decode", "jpegBytes"));
        std::vector<std::byte> jpeg(message.begin() + 4U + header_size, message.end());
        return Result<PreviewFrame>::success({.machine_id = std::move(machine).value(),
                                              .camera_id = std::move(camera).value(),
                                              .message_id = std::move(id).value(),
                                              .sequence = sequence.value(),
                                              .timestamp = std::move(timestamp).value(),
                                              .jpeg = std::move(jpeg)});
    }
    catch (...)
    {
        return Result<PreviewFrame>::failure(
            protocol_error("预览解码异常", "uplink.preview.decode"));
    }
}

} // namespace paperbreak::uplink
