#include "paperbreak/uplink/protocol.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace
{
using paperbreak::uplink::MessageEnvelope;
using paperbreak::uplink::PreviewFrame;

TEST(UplinkProtocol, ParsesStrictSessionAndSortsCapabilities)
{
    const auto result = paperbreak::uplink::parse_session_hello(R"({
        "requestId":"req-1",
        "machineId":"EDGE-01",
        "productionLineId":"LINE-01",
        "softwareVersion":"0.1.0",
        "supportedProtocolVersions":[2,1,1],
        "capabilities":["camera.start","event.review","camera.start"],
        "extensions":{"vendor":"test"}
    })");
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().machine_id, "EDGE-01");
    EXPECT_EQ(result.value().supported_protocol_versions, (std::vector<std::uint32_t>{1U, 2U}));
    EXPECT_EQ(result.value().capabilities,
              (std::vector<std::string>{"camera.start", "event.review"}));
}

TEST(UplinkProtocol, RejectsUnknownFieldAndPathEscapingIdentifier)
{
    auto unknown = paperbreak::uplink::parse_session_hello(R"({
        "requestId":"req-1","machineId":"EDGE-01","productionLineId":"LINE-01",
        "softwareVersion":"0.1.0","supportedProtocolVersions":[1],"capabilities":[],
        "unexpected":true
    })");
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().business_code, "UPLINK_PROTOCOL_ERROR");

    auto path = paperbreak::uplink::validate_identifier("EDGE..01", "machineId", 64U);
    ASSERT_FALSE(path);
    EXPECT_EQ(path.error().business_code, "UPLINK_PROTOCOL_ERROR");
}

TEST(UplinkProtocol, RoundTripsTextEnvelopeAndRejectsHigherVersion)
{
    MessageEnvelope source{.protocol_version = 1U,
                           .message_type = "status.update",
                           .message_id = "msg-1",
                           .machine_id = "EDGE-01",
                           .sequence = 7U,
                           .timestamp = "2026-08-05T01:02:03.004Z",
                           .payload_json = R"({"streaming":true})"};
    auto encoded = paperbreak::uplink::serialize_message_envelope(source);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = paperbreak::uplink::parse_message_envelope(encoded.value());
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value().message_type, source.message_type);
    EXPECT_EQ(decoded.value().sequence, 7U);

    auto higher = paperbreak::uplink::parse_message_envelope(R"({
        "protocolVersion":2,"messageType":"heartbeat","messageId":"msg-2",
        "machineId":"EDGE-01","sequence":8,"timestamp":"2026-08-05T01:02:03.004Z",
        "payload":{}
    })");
    ASSERT_FALSE(higher);
    EXPECT_EQ(higher.error().business_code, "UPLINK_PROTOCOL_VERSION_UNSUPPORTED");
}

TEST(UplinkProtocol, ValidatesUploadDescriptorAndDigest)
{
    const std::string digest(64U, 'a');
    auto upload = paperbreak::uplink::parse_upload_create(
        std::string{R"({"requestId":"req-2","eventId":"EVT-1","logicalFileId":"manifest-1",)"} +
        R"("fileName":"manifest.json","contentType":"application/json","totalBytes":12,)" +
        R"("chunkBytes":12,"sha256":")" + digest + R"("})");
    ASSERT_TRUE(upload) << upload.error().message;
    EXPECT_EQ(upload.value().total_bytes, 12U);
    EXPECT_TRUE(paperbreak::uplink::is_sha256_hex(digest));
    EXPECT_FALSE(paperbreak::uplink::is_sha256_hex(std::string(64U, 'A')));
}

TEST(UplinkProtocol, RoundTripsBoundedBinaryPreview)
{
    PreviewFrame source{
        .machine_id = "EDGE-01",
        .camera_id = "CAM01",
        .message_id = "preview-1",
        .sequence = 9U,
        .timestamp = "2026-08-05T01:02:03.004Z",
        .jpeg = {std::byte{0xff}, std::byte{0xd8}, std::byte{0xff}, std::byte{0xd9}}};
    auto encoded = paperbreak::uplink::encode_preview_frame(source);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = paperbreak::uplink::decode_preview_frame(encoded.value());
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value().machine_id, source.machine_id);
    EXPECT_EQ(decoded.value().camera_id, source.camera_id);
    EXPECT_EQ(decoded.value().jpeg, source.jpeg);

    encoded.value()[0] = std::byte{0xff};
    EXPECT_FALSE(paperbreak::uplink::decode_preview_frame(encoded.value()));
}

} // namespace
