#include "paperbreak/ipc/protocol.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace
{

using Json = nlohmann::json;

paperbreak::ipc::Frame request_frame(const int version = 1)
{
    Json header{{"protocolVersion", version},
                {"messageType", "request"},
                {"requestId", "019870f2-6c80-7a31-9b52-6e3b9ca1d88f"},
                {"command", "system.getStatus"},
                {"timestamp", "2026-08-01T12:00:00.123Z"},
                {"payload", Json::object()}};
    return {.header_json = header.dump(), .binary = {}};
}

} // namespace

TEST(IpcProtocol, DecodesEveryByteFragmentAndPreservesBinary)
{
    auto frame = request_frame();
    frame.binary = {std::byte{0x00}, std::byte{0x7f}, std::byte{0xff}};
    auto encoded = paperbreak::ipc::encode_frame(frame);
    ASSERT_TRUE(encoded);

    paperbreak::ipc::FrameDecoder decoder;
    std::vector<paperbreak::ipc::Frame> decoded;
    for (const std::byte value : encoded.value())
    {
        const std::array one{value};
        auto result = decoder.append(one);
        ASSERT_TRUE(result);
        decoded.insert(decoded.end(), result.value().begin(), result.value().end());
    }

    ASSERT_EQ(decoded.size(), 1U);
    EXPECT_EQ(decoded.front().header_json, frame.header_json);
    EXPECT_EQ(decoded.front().binary, frame.binary);
    EXPECT_FALSE(decoder.has_pending_data());
}

TEST(IpcProtocol, DecodesStickyFrames)
{
    auto first = paperbreak::ipc::encode_frame(request_frame());
    auto second = paperbreak::ipc::encode_frame(request_frame());
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    std::vector<std::byte> combined = first.value();
    combined.insert(combined.end(), second.value().begin(), second.value().end());

    paperbreak::ipc::FrameDecoder decoder;
    auto decoded = decoder.append(combined);

    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().size(), 2U);
}

TEST(IpcProtocol, RejectsZeroAndOversizedLengthsBeforePayloadAllocation)
{
    paperbreak::ipc::FrameDecoder decoder;
    const std::array<std::byte, 8> zero{};
    auto zero_result = decoder.append(zero);
    ASSERT_FALSE(zero_result);
    EXPECT_EQ(zero_result.error().business_code, "IPC_PROTOCOL_ERROR");

    const std::uint32_t too_large =
        static_cast<std::uint32_t>(paperbreak::ipc::maximum_header_bytes + 1U);
    const std::array<std::byte, 8> prefix{static_cast<std::byte>((too_large >> 24U) & 0xffU),
                                          static_cast<std::byte>((too_large >> 16U) & 0xffU),
                                          static_cast<std::byte>((too_large >> 8U) & 0xffU),
                                          static_cast<std::byte>(too_large & 0xffU),
                                          std::byte{},
                                          std::byte{},
                                          std::byte{},
                                          std::byte{}};
    auto large_result = decoder.append(prefix);
    ASSERT_FALSE(large_result);
    EXPECT_EQ(large_result.error().business_code, "IPC_MESSAGE_TOO_LARGE");

    paperbreak::ipc::Frame maximum_header{
        .header_json = std::string(paperbreak::ipc::maximum_header_bytes, 'x'), .binary = {}};
    EXPECT_TRUE(paperbreak::ipc::encode_frame(maximum_header));
    paperbreak::ipc::Frame maximum_binary{
        .header_json = "{}",
        .binary = std::vector<std::byte>(paperbreak::ipc::maximum_binary_bytes)};
    EXPECT_TRUE(paperbreak::ipc::encode_frame(maximum_binary));
}

TEST(IpcProtocol, RejectsInvalidJsonUnknownFieldsAndUnsupportedVersion)
{
    auto invalid_json = paperbreak::ipc::decode_request({.header_json = "not-json", .binary = {}});
    ASSERT_FALSE(invalid_json);
    EXPECT_EQ(invalid_json.error().business_code, "IPC_PROTOCOL_ERROR");

    auto unknown = request_frame();
    Json unknown_header = Json::parse(unknown.header_json);
    unknown_header["surprise"] = true;
    unknown.header_json = unknown_header.dump();
    auto unknown_result = paperbreak::ipc::decode_request(unknown);
    ASSERT_FALSE(unknown_result);
    EXPECT_EQ(unknown_result.error().business_code, "IPC_REQUEST_INVALID");
    EXPECT_TRUE(unknown_result.error().correlation_id.has_value());

    auto version_result = paperbreak::ipc::decode_request(request_frame(2));
    ASSERT_FALSE(version_result);
    EXPECT_EQ(version_result.error().business_code, "IPC_PROTOCOL_VERSION_UNSUPPORTED");
    EXPECT_TRUE(version_result.error().correlation_id.has_value());

    auto invalid_timestamp = request_frame();
    Json timestamp_header = Json::parse(invalid_timestamp.header_json);
    timestamp_header["timestamp"] = "xxxx-99-99T99:99:99Z";
    invalid_timestamp.header_json = timestamp_header.dump();
    auto timestamp_result = paperbreak::ipc::decode_request(invalid_timestamp);
    ASSERT_FALSE(timestamp_result);
    EXPECT_EQ(timestamp_result.error().business_code, "IPC_REQUEST_INVALID");

    auto extension = request_frame();
    Json extension_header = Json::parse(extension.header_json);
    extension_header["extensions"] = Json{{"futureField", true}};
    extension.header_json = extension_header.dump();
    EXPECT_TRUE(paperbreak::ipc::decode_request(extension));
}

TEST(IpcProtocol, SerializesStableNestedErrorAndPushWithoutRequestId)
{
    auto error = paperbreak::make_error("IPC_BUSY", paperbreak::Severity::warning, "busy", "ipc",
                                        "ipc.test", true);
    paperbreak::ipc::ResponseMessage response{.request_id = "019870f2-6c80-7a31-9b52-6e3b9ca1d88f",
                                              .success = false,
                                              .timestamp = "2026-08-01T12:00:00.123Z",
                                              .payload_json = "{}",
                                              .error = error,
                                              .binary = {}};
    auto encoded_response = paperbreak::ipc::encode_response(response);
    ASSERT_TRUE(encoded_response);
    const Json response_json = Json::parse(encoded_response.value().header_json);
    EXPECT_EQ(response_json.at("error").at("businessCode"), "IPC_BUSY");
    EXPECT_FALSE(response_json.at("success").get<bool>());

    auto encoded_push =
        paperbreak::ipc::encode_push({.event_name = "status.changed",
                                      .timestamp = "2026-08-01T12:00:00.123Z",
                                      .payload_json = R"({"serviceState":"running"})",
                                      .binary = {},
                                      .coalescing_key = "status.changed"});
    ASSERT_TRUE(encoded_push);
    const Json push_json = Json::parse(encoded_push.value().header_json);
    EXPECT_EQ(push_json.at("messageType"), "push");
    EXPECT_FALSE(push_json.contains("requestId"));
}
