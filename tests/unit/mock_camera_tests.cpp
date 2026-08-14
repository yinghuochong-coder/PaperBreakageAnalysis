#include "paperbreak/camera/mock_camera.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using namespace paperbreak;
using namespace paperbreak::camera;
using namespace paperbreak::camera::mock;

MockCameraConfig camera_config(const std::string& serial = "MOCK-0001",
                               const TriggerMode mode = TriggerMode::continuous)
{
    return {.descriptor = {"PaperBreak Mock", serial, "192.0.2.1", "mock0"},
            .width = 4U,
            .height = 3U,
            .frame_rate = 100.0,
            .pixel_format = PixelFormat::mono8,
            .trigger_mode = mode,
            .pattern = MockFramePattern::gradient,
            .random_seed = 42U,
            .maximum_payload_bytes = 64U,
            .fault_queue_capacity = 2U,
            .trigger_capacity = 2U};
}

struct OpenedCamera final
{
    std::unique_ptr<MockCameraProvider> provider;
    std::unique_ptr<ICameraDevice> device;
    MockCameraControl control;
};

OpenedCamera open_camera(MockCameraConfig config)
{
    const auto serial = config.descriptor.serial_number;
    auto provider_result = MockCameraProvider::create({std::move(config)});
    EXPECT_TRUE(provider_result);
    auto provider = std::move(provider_result).value();
    auto control_result = provider->control(serial);
    EXPECT_TRUE(control_result);
    auto device_result = provider->create_device(serial);
    EXPECT_TRUE(device_result);
    auto device = std::move(device_result).value();
    EXPECT_TRUE(device->connect());
    EXPECT_TRUE(device->start_acquisition());
    return {std::move(provider), std::move(device), std::move(control_result).value()};
}

CapturedFrameMetadata capture(ICameraDevice& device, FrameBuffer& buffer)
{
    auto result = device.capture_into(buffer, 250ms);
    EXPECT_TRUE(result);
    return result ? result.value() : CapturedFrameMetadata{};
}
} // namespace

TEST(CameraMockProvider, ValidatesCountUniqueSerialAndConfiguration)
{
    auto result = MockCameraProvider::create({});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().details.back().value, "camera-count-out-of-range");

    std::vector<MockCameraConfig> seven;
    for (std::size_t index = 0U; index < 7U; ++index)
    {
        seven.push_back(camera_config("MOCK-000" + std::to_string(index)));
    }
    result = MockCameraProvider::create(std::move(seven));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().details.back().value, "camera-count-out-of-range");

    result = MockCameraProvider::create({camera_config(), camera_config()});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().details.back().value, "duplicate-serial-number");

    auto invalid = camera_config();
    invalid.frame_rate = 0.0;
    result = MockCameraProvider::create({invalid});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().details.back().value, "frame-rate-out-of-range-or-step");

    invalid = camera_config();
    invalid.fault_queue_capacity = 0U;
    result = MockCameraProvider::create({invalid});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().details.back().value, "fault-capacity-out-of-range");
}

TEST(CameraMockProvider, EnumeratesSixDevicesAndRejectsUnknownOrDuplicateOpen)
{
    std::vector<MockCameraConfig> configs;
    for (std::size_t index = 1U; index <= 6U; ++index)
    {
        configs.push_back(camera_config("MOCK-000" + std::to_string(index)));
    }
    auto result = MockCameraProvider::create(std::move(configs));
    ASSERT_TRUE(result);
    auto provider = std::move(result).value();
    auto devices = provider->enumerate_devices();
    ASSERT_TRUE(devices);
    EXPECT_EQ(devices.value().size(), 6U);

    auto missing = provider->create_device("MISSING");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().business_code, "CAMERA_NOT_FOUND");

    auto first = provider->create_device("MOCK-0001");
    ASSERT_TRUE(first);
    auto duplicate = provider->create_device("MOCK-0001");
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().business_code, "CAMERA_INVALID_STATE_TRANSITION");
    first.value().reset();
    EXPECT_TRUE(provider->create_device("MOCK-0001"));
}

TEST(CameraMockFrames, ProducesAllFormatsAndDeterministicPatterns)
{
    constexpr std::array formats = {PixelFormat::mono8, PixelFormat::mono10, PixelFormat::mono12,
                                    PixelFormat::bayer_rg8};
    constexpr std::array patterns = {MockFramePattern::gradient, MockFramePattern::checkerboard,
                                     MockFramePattern::noise};
    for (const auto format : formats)
    {
        for (const auto pattern : patterns)
        {
            auto config = camera_config();
            config.pixel_format = format;
            config.pattern = pattern;
            auto first = open_camera(config);
            auto second = open_camera(config);
            FrameBuffer first_buffer{64U};
            FrameBuffer second_buffer{64U};
            const auto first_metadata = capture(*first.device, first_buffer);
            const auto second_metadata = capture(*second.device, second_buffer);
            EXPECT_EQ(first_metadata, second_metadata);
            EXPECT_TRUE(std::ranges::equal(first_buffer.bytes(), second_buffer.bytes()));
            const auto expected_size =
                format == PixelFormat::mono10 || format == PixelFormat::mono12 ? 24U : 12U;
            EXPECT_EQ(first_buffer.size(), expected_size);
        }
    }
}

TEST(CameraMockFrames, AppliesHorizontalAndVerticalMirroringToAcquiredPixels)
{
    auto normal = open_camera(camera_config("MOCK-NORMAL"));
    auto mirrored = open_camera(camera_config("MOCK-MIRRORED"));
    ASSERT_TRUE(mirrored.device->stop_acquisition());
    auto applied = mirrored.device->apply_parameters({.reverse_x = true, .reverse_y = true});
    ASSERT_TRUE(applied);
    EXPECT_EQ(applied.value().reverse_x, true);
    EXPECT_EQ(applied.value().reverse_y, true);
    ASSERT_TRUE(mirrored.device->start_acquisition());

    FrameBuffer normal_buffer{64U};
    FrameBuffer mirrored_buffer{64U};
    capture(*normal.device, normal_buffer);
    capture(*mirrored.device, mirrored_buffer);
    ASSERT_EQ(normal_buffer.size(), 12U);
    ASSERT_EQ(mirrored_buffer.size(), normal_buffer.size());
    const auto normal_bytes = normal_buffer.bytes();
    const auto mirrored_bytes = mirrored_buffer.bytes();
    for (std::size_t y = 0U; y < 3U; ++y)
        for (std::size_t x = 0U; x < 4U; ++x)
            EXPECT_EQ(mirrored_bytes[y * 4U + x], normal_bytes[(2U - y) * 4U + (3U - x)]);
}

TEST(CameraMockLifecycle, AppliesAndReadsParametersAndRejectsIllegalStates)
{
    auto provider_result = MockCameraProvider::create({camera_config()});
    ASSERT_TRUE(provider_result);
    auto provider = std::move(provider_result).value();
    auto device_result = provider->create_device("MOCK-0001");
    ASSERT_TRUE(device_result);
    auto device = std::move(device_result).value();

    EXPECT_FALSE(device->start_acquisition());
    EXPECT_TRUE(device->connect());
    EXPECT_FALSE(device->connect());
    auto applied = device->apply_parameters({.exposure_auto_mode = ExposureAutoMode::continuous,
                                             .frame_rate = 50.0,
                                             .roi = Roi{2U, 2U, 0U, 0U},
                                             .reverse_x = true,
                                             .reverse_y = false,
                                             .pixel_format = PixelFormat::mono10,
                                             .trigger_mode = TriggerMode::software});
    ASSERT_TRUE(applied);
    EXPECT_EQ(applied.value().exposure_auto_mode, ExposureAutoMode::continuous);
    EXPECT_EQ(applied.value().frame_rate, 50.0);
    EXPECT_EQ(applied.value().roi, (Roi{2U, 2U, 0U, 0U}));
    EXPECT_EQ(applied.value().reverse_x, true);
    EXPECT_EQ(applied.value().reverse_y, false);
    EXPECT_FALSE(applied.value().trigger_source);
    EXPECT_EQ(device->read_parameters().value(), applied.value());

    EXPECT_TRUE(device->start_acquisition());
    EXPECT_FALSE(device->start_acquisition());
    EXPECT_FALSE(device->apply_parameters({.frame_rate = 60.0}));
    EXPECT_TRUE(device->stop_acquisition());
    EXPECT_FALSE(device->stop_acquisition());
    EXPECT_TRUE(device->restore_defaults());
    EXPECT_TRUE(device->disconnect());
    EXPECT_FALSE(device->disconnect());
}

TEST(CameraMockTriggers, SupportsContinuousSoftwareAndHardwareModes)
{
    auto continuous_config = camera_config();
    continuous_config.frame_rate = 10.0;
    auto continuous = open_camera(continuous_config);
    FrameBuffer buffer{64U};
    EXPECT_TRUE(continuous.device->capture_into(buffer, 150ms));
    auto too_soon = continuous.device->capture_into(buffer, 1ms);
    ASSERT_FALSE(too_soon);
    EXPECT_EQ(too_soon.error().business_code, "CAMERA_FRAME_TIMEOUT");
    EXPECT_TRUE(continuous.device->capture_into(buffer, 150ms));

    auto software = open_camera(camera_config("MOCK-SOFT", TriggerMode::software));
    auto no_software_edge = software.device->capture_into(buffer, 2ms);
    ASSERT_FALSE(no_software_edge);
    EXPECT_EQ(no_software_edge.error().business_code, "CAMERA_FRAME_TIMEOUT");
    EXPECT_TRUE(software.device->software_trigger());
    EXPECT_TRUE(software.device->software_trigger());
    EXPECT_FALSE(software.device->software_trigger());
    EXPECT_TRUE(software.device->capture_into(buffer, 50ms));
    EXPECT_TRUE(software.device->capture_into(buffer, 50ms));

    auto hardware = open_camera(camera_config("MOCK-HARD", TriggerMode::hardware));
    EXPECT_FALSE(hardware.device->software_trigger());
    EXPECT_TRUE(hardware.control.hardware_trigger(2U));
    EXPECT_FALSE(hardware.control.hardware_trigger());
    EXPECT_TRUE(hardware.device->capture_into(buffer, 50ms));
    EXPECT_TRUE(hardware.device->capture_into(buffer, 50ms));
    EXPECT_EQ(hardware.control.snapshot().value().triggers_rejected, 1U);
}

TEST(CameraMockTriggers, StopWakesPendingCaptureAndClearsTokens)
{
    auto opened = open_camera(camera_config("MOCK-SOFT", TriggerMode::software));
    FrameBuffer buffer{64U};
    std::optional<Result<CapturedFrameMetadata>> result;
    std::jthread waiter(
        [&](std::stop_token) { result.emplace(opened.device->capture_into(buffer, 1s)); });
    std::this_thread::sleep_for(20ms);
    EXPECT_TRUE(opened.device->stop_acquisition());
    waiter.join();
    ASSERT_TRUE(result);
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().business_code, "CAMERA_INVALID_STATE_TRANSITION");
    EXPECT_EQ(opened.control.snapshot().value().pending_software_triggers, 0U);
}

TEST(CameraMockFaults, ExecutesRuntimeFaultsAndReconnectsAfterDisconnect)
{
    auto opened = open_camera(camera_config());
    FrameBuffer buffer{64U};

    EXPECT_TRUE(opened.control.inject_fault({.kind = MockFaultKind::timeout}));
    auto timed_out = opened.device->capture_into(buffer, 50ms);
    ASSERT_FALSE(timed_out);
    EXPECT_EQ(timed_out.error().business_code, "CAMERA_FRAME_TIMEOUT");

    EXPECT_TRUE(opened.control.inject_fault({.kind = MockFaultKind::incomplete_frame}));
    auto incomplete = capture(*opened.device, buffer);
    EXPECT_TRUE(incomplete.flags.incomplete);
    EXPECT_EQ(incomplete.camera_frame_number, 1U);

    EXPECT_TRUE(opened.control.inject_fault({.kind = MockFaultKind::drop_frame, .amount = 2U}));
    EXPECT_EQ(capture(*opened.device, buffer).camera_frame_number, 4U);
    EXPECT_TRUE(
        opened.control.inject_fault({.kind = MockFaultKind::frame_number_jump, .amount = 3U}));
    EXPECT_EQ(capture(*opened.device, buffer).camera_frame_number, 8U);

    EXPECT_TRUE(opened.control.inject_fault(
        {.kind = MockFaultKind::geometry_change, .geometry = FrameGeometry{2U, 2U, 2U}}));
    EXPECT_EQ(capture(*opened.device, buffer).geometry, (FrameGeometry{2U, 2U, 2U}));
    EXPECT_EQ(buffer.size(), 4U);
    EXPECT_TRUE(opened.control.inject_fault(
        {.kind = MockFaultKind::pixel_format_change, .pixel_format = PixelFormat::mono10}));
    EXPECT_EQ(capture(*opened.device, buffer).pixel_format, PixelFormat::mono10);
    EXPECT_EQ(buffer.size(), 24U);

    EXPECT_TRUE(opened.control.inject_fault({.kind = MockFaultKind::disconnect}));
    auto disconnected = opened.device->capture_into(buffer, 50ms);
    ASSERT_FALSE(disconnected);
    EXPECT_EQ(disconnected.error().business_code, "CAMERA_DISCONNECTED");
    EXPECT_FALSE(opened.control.snapshot().value().connected);
    EXPECT_TRUE(opened.device->connect());
    EXPECT_TRUE(opened.device->start_acquisition());
    EXPECT_TRUE(opened.device->capture_into(buffer, 50ms));
}

TEST(CameraMockFaults, RunsScheduledFaultsInOrderAndBoundsRuntimeQueue)
{
    auto config = camera_config();
    config.fault_script = {{1U, {.kind = MockFaultKind::timeout}},
                           {2U, {.kind = MockFaultKind::incomplete_frame}}};
    auto opened = open_camera(config);
    FrameBuffer buffer{64U};
    auto first = opened.device->capture_into(buffer, 50ms);
    ASSERT_FALSE(first);
    EXPECT_EQ(first.error().business_code, "CAMERA_FRAME_TIMEOUT");
    EXPECT_TRUE(capture(*opened.device, buffer).flags.incomplete);

    EXPECT_TRUE(opened.control.inject_fault({.kind = MockFaultKind::timeout}));
    EXPECT_TRUE(opened.control.inject_fault({.kind = MockFaultKind::timeout}));
    auto rejected = opened.control.inject_fault({.kind = MockFaultKind::timeout});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().details.back().value, "fault-queue-full");
    auto snapshot = opened.control.snapshot().value();
    EXPECT_EQ(snapshot.queued_faults, 2U);
    EXPECT_EQ(snapshot.faults_rejected, 1U);
    opened.control.clear_faults();
    EXPECT_EQ(opened.control.snapshot().value().queued_faults, 0U);
}

TEST(CameraMockFaults, RejectsMalformedScriptsAndOversizedFormatChanges)
{
    auto malformed = camera_config();
    malformed.fault_script = {{2U, {.kind = MockFaultKind::timeout}},
                              {1U, {.kind = MockFaultKind::timeout}}};
    auto provider = MockCameraProvider::create({malformed});
    ASSERT_FALSE(provider);
    EXPECT_EQ(provider.error().details.back().value, "fault-script-not-strictly-ordered");

    malformed = camera_config();
    malformed.pattern = static_cast<MockFramePattern>(99);
    provider = MockCameraProvider::create({malformed});
    ASSERT_FALSE(provider);
    EXPECT_EQ(provider.error().details.back().value, "unknown-frame-pattern");

    auto bounded = camera_config();
    bounded.maximum_payload_bytes = 12U;
    auto opened = open_camera(bounded);
    ASSERT_TRUE(opened.control.inject_fault(
        {.kind = MockFaultKind::pixel_format_change, .pixel_format = PixelFormat::mono10}));
    FrameBuffer buffer{64U};
    auto captured = opened.device->capture_into(buffer, 50ms);
    ASSERT_FALSE(captured);
    EXPECT_EQ(captured.error().business_code, "CAMERA_CONFIG_FAILED");
    EXPECT_EQ(buffer.size(), 0U);
}
