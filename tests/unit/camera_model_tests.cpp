#include "paperbreak/camera/camera.hpp"
#include "paperbreak/camera/control.hpp"
#include "paperbreak/camera/mock_camera.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
using namespace paperbreak;
using namespace paperbreak::camera;

CameraCapabilities rich_capabilities()
{
    return {
        .exposure_us = SteppedRange<double>{10.0, 100.0, 10.0},
        .gain_db = SteppedRange<double>{0.0, 12.0, 0.5},
        .frame_rate = SteppedRange<double>{10.0, 60.0, 5.0},
        .roi = RoiCapabilities{.sensor_width = 100U,
                               .sensor_height = 80U,
                               .width = {20U, 100U, 10U},
                               .height = {10U, 80U, 10U},
                               .offset_x = {0U, 80U, 10U},
                               .offset_y = {0U, 70U, 10U}},
        .supports_reverse_x = true,
        .supports_reverse_y = true,
        .pixel_formats = {PixelFormat::mono8, PixelFormat::bayer_rg8},
        .trigger_modes = {TriggerMode::continuous, TriggerMode::hardware, TriggerMode::software},
        .trigger_sources = {"Line0"},
        .trigger_delay_us = SteppedRange<std::uint32_t>{0U, 1000U, 10U},
        .packet_size_bytes = SteppedRange<std::uint32_t>{1500U, 9000U, 500U},
        .inter_packet_delay_ns = SteppedRange<std::uint32_t>{0U, 10000U, 100U},
        .digital_io = {{"Output0", DigitalIoDirection::output, true},
                       {"Input0", DigitalIoDirection::input, false}},
        .supports_user_sets = true,
        .supports_restore_defaults = true,
        .maximum_payload_bytes = 8000U};
}

CameraParameterSnapshot valid_parameters()
{
    return {.exposure_us = 30.0,
            .gain_db = 1.5,
            .frame_rate = 40.0,
            .roi = Roi{40U, 20U, 20U, 10U},
            .reverse_x = true,
            .reverse_y = false,
            .pixel_format = PixelFormat::mono8,
            .trigger_mode = TriggerMode::hardware,
            .trigger_source = "Line0",
            .trigger_delay_us = 20U,
            .packet_size_bytes = 2000U,
            .inter_packet_delay_ns = 300U,
            .digital_io = {{"Output0", true}}};
}

class FakeCameraDevice final : public ICameraDevice
{
  public:
    explicit FakeCameraDevice(CameraCapabilities capabilities = rich_capabilities())
        : capabilities_(std::move(capabilities))
    {
    }

    [[nodiscard]] const CameraDeviceDescriptor& descriptor() const noexcept override
    {
        return descriptor_;
    }

    [[nodiscard]] Result<void> connect() override
    {
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> disconnect() override
    {
        return Result<void>::success();
    }

    [[nodiscard]] Result<CameraCapabilities> capabilities() override
    {
        return Result<CameraCapabilities>::success(capabilities_);
    }

    [[nodiscard]] Result<CameraParameterSnapshot> read_parameters() override
    {
        return Result<CameraParameterSnapshot>::success(parameters_);
    }

    [[nodiscard]] Result<CameraParameterSnapshot> apply_parameters(
        const CameraParameterSnapshot& parameters) override
    {
        ++apply_calls;
        parameters_ = parameters;
        return Result<CameraParameterSnapshot>::success(parameters_);
    }

    [[nodiscard]] Result<void> start_acquisition() override
    {
        return Result<void>::success();
    }

    [[nodiscard]] Result<CapturedFrameMetadata> capture_into(
        FrameBuffer& destination, const std::chrono::milliseconds timeout) override
    {
        if (timeout <= std::chrono::milliseconds::zero() || destination.capacity() < 4U)
        {
            return Result<CapturedFrameMetadata>::failure(make_camera_error(
                CameraErrorKind::frame_timeout, "测试取帧失败", "camera.capture"));
        }
        std::fill_n(destination.writable_bytes().begin(), 4U, std::byte{0x2A});
        static_cast<void>(destination.set_size(4U));
        return Result<CapturedFrameMetadata>::success(
            {.camera_frame_number = 7U,
             .camera_timestamp =
                 CameraTimestamp{100U, 1000U, CameraTimestampQuality::unsynchronized},
             .geometry = {2U, 2U, 2U},
             .pixel_format = PixelFormat::mono8,
             .flags = {}});
    }

    [[nodiscard]] Result<void> software_trigger() override
    {
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> stop_acquisition() override
    {
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> save_user_set(std::string_view) override
    {
        return Result<void>::success();
    }

    [[nodiscard]] Result<CameraParameterSnapshot> restore_defaults() override
    {
        parameters_ = {};
        return Result<CameraParameterSnapshot>::success(parameters_);
    }

    int apply_calls{};

  private:
    CameraDeviceDescriptor descriptor_{"MockCamera", "MOCK-0001", "192.0.2.10", "mock0"};
    CameraCapabilities capabilities_;
    CameraParameterSnapshot parameters_;
};

class FakeCameraProvider final : public ICameraProvider
{
  public:
    [[nodiscard]] Result<std::vector<CameraDeviceDescriptor>> enumerate_devices() override
    {
        return Result<std::vector<CameraDeviceDescriptor>>::success(
            {{"MockCamera", "MOCK-0001", "192.0.2.10", "mock0"}});
    }

    [[nodiscard]] Result<std::unique_ptr<ICameraDevice>> create_device(
        const std::string_view serial_number) override
    {
        if (serial_number != "MOCK-0001")
        {
            return Result<std::unique_ptr<ICameraDevice>>::failure(make_camera_error(
                CameraErrorKind::not_found, "测试设备不存在", "camera.createDevice"));
        }
        std::unique_ptr<ICameraDevice> device = std::make_unique<FakeCameraDevice>();
        return Result<std::unique_ptr<ICameraDevice>>::success(std::move(device));
    }
};
} // namespace

TEST(CameraCapabilities, AcceptsSupportedParameterSnapshot)
{
    const auto result = validate_parameters(rich_capabilities(), valid_parameters());
    EXPECT_TRUE(result);
}

TEST(CameraCapabilities, AcceptsContinuousFloatingRangeAndRejectsNonFiniteValue)
{
    CameraCapabilities capabilities;
    capabilities.exposure_us = SteppedRange<double>{10.0, 100.0, 0.0};

    EXPECT_TRUE(validate_parameters(capabilities, {.exposure_us = 25.125}));
    EXPECT_FALSE(validate_parameters(capabilities,
                                     {.exposure_us = std::numeric_limits<double>::infinity()}));
}

TEST(CameraCapabilities, RejectsUnsupportedOutOfStepAndInvalidCombination)
{
    CameraCapabilities limited;
    limited.exposure_us = SteppedRange<double>{10.0, 100.0, 10.0};
    limited.pixel_formats = {PixelFormat::mono8};
    limited.trigger_modes = {TriggerMode::continuous};

    CameraParameterSnapshot unsupported_gain;
    unsupported_gain.gain_db = 1.0;
    auto result = validate_parameters(limited, unsupported_gain);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "CAMERA_CONFIG_FAILED");
    ASSERT_EQ(result.error().details.size(), 2U);
    EXPECT_EQ(result.error().details.front().value, "gainDb");

    CameraParameterSnapshot out_of_step;
    out_of_step.exposure_us = 25.0;
    result = validate_parameters(limited, out_of_step);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().details.back().value, "out-of-range-or-step");

    CameraParameterSnapshot unsupported_format;
    unsupported_format.pixel_format = PixelFormat::bayer_rg8;
    result = validate_parameters(limited, unsupported_format);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().details.front().value, "pixelFormat");

    CameraParameterSnapshot invalid_trigger;
    invalid_trigger.trigger_mode = TriggerMode::continuous;
    invalid_trigger.trigger_source = "Line0";
    result = validate_parameters(rich_capabilities(), invalid_trigger);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().details.back().value, "requires-hardware-trigger");
}

TEST(CameraCapabilities, RejectsRoiAndDigitalIoOutsideCapabilities)
{
    auto parameters = valid_parameters();
    parameters.roi = Roi{80U, 20U, 30U, 10U};
    auto result = validate_parameters(rich_capabilities(), parameters);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().details.back().value, "outside-sensor");

    parameters = valid_parameters();
    parameters.digital_io = {{"Input0", true}};
    result = validate_parameters(rich_capabilities(), parameters);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().details.back().value, "unsupported-line");
}

TEST(CameraCapabilities, IdentifiesTheSpecificInvalidRoiField)
{
    auto parameters = valid_parameters();
    parameters.roi->offset_y = 11U;

    const auto result = validate_parameters(rich_capabilities(), parameters);

    ASSERT_FALSE(result);
    ASSERT_EQ(result.error().details.size(), 2U);
    EXPECT_EQ(result.error().details.front().value, "roi.offsetY");
    EXPECT_EQ(result.error().details.back().value, "out-of-range-or-step");
    EXPECT_EQ(result.error().message,
              "相机参数不符合设备能力（参数：roi.offsetY，原因：out-of-range-or-step）");
}

TEST(CameraCapabilities, RejectsEnablingUnsupportedImageMirroring)
{
    CameraCapabilities capabilities;
    auto result = validate_parameters(capabilities, {.reverse_x = true});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().details.front().value, "reverseX");
    EXPECT_EQ(result.error().details.back().value, "unsupported");

    EXPECT_TRUE(validate_parameters(capabilities, {.reverse_x = false, .reverse_y = false}));
}

TEST(CameraCapabilities, ValidationPreventsDeviceWrite)
{
    FakeCameraDevice device;
    auto invalid = valid_parameters();
    invalid.exposure_us = 25.0;

    auto result = apply_validated_parameters(device, invalid);
    ASSERT_FALSE(result);
    EXPECT_EQ(device.apply_calls, 0);

    result = apply_validated_parameters(device, valid_parameters());
    ASSERT_TRUE(result);
    EXPECT_EQ(device.apply_calls, 1);
    EXPECT_EQ(result.value(), valid_parameters());
}

TEST(CameraInventory, FindsUniqueDeviceAndRejectsMissingOrDuplicateSerial)
{
    const std::vector<CameraDeviceDescriptor> devices = {
        {"ModelA", "SERIAL-0001", "192.0.2.1", "nic0"},
        {"ModelB", "SERIAL-0002", "192.0.2.2", "nic1"}};

    auto result = find_device_by_serial(devices, "SERIAL-0002");
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().model_name, "ModelB");

    result = find_device_by_serial(devices, "SERIAL-9999");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "CAMERA_NOT_FOUND");
    EXPECT_TRUE(result.error().retryable);
    ASSERT_EQ(result.error().details.size(), 1U);
    EXPECT_EQ(result.error().details.front().value, "9999");

    result = find_device_by_serial(devices, "");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "CAMERA_CONFIG_FAILED");
    EXPECT_EQ(result.error().details.front().value, "invalid-serial-number");

    const std::vector<CameraDeviceDescriptor> duplicates = {
        {"ModelA", "SERIAL-0001", "192.0.2.1", "nic0"},
        {"ModelB", "SERIAL-0001", "192.0.2.2", "nic1"}};
    result = find_device_by_serial(duplicates, "SERIAL-0001");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "CAMERA_CONFIG_FAILED");
    ASSERT_EQ(result.error().details.size(), 2U);
    EXPECT_EQ(result.error().details.front().value, "duplicate-serial-number");
}

TEST(CameraInventory, ReconcilesFourLogicalSlotsBySerialAndReportsProblems)
{
    const std::vector<CameraSlotBinding> bindings = {{"CAM01", "SERIAL-0001"},
                                                     {"CAM02", "SERIAL-0002"},
                                                     {"CAM03", "SERIAL-0003"},
                                                     {"CAM04", "SERIAL-0004"}};
    const std::vector<CameraDeviceDescriptor> devices = {
        {"ModelA", "SERIAL-0001", "192.0.2.99", "192.0.2.10", true},
        {"ModelB", "SERIAL-0002", "192.0.2.2", "192.0.2.10", false},
        {"Wrong", "SERIAL-9999", "192.0.2.3", "192.0.2.10", true}};

    const auto result = reconcile_camera_slots(bindings, devices);

    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().slots.size(), 4U);
    EXPECT_EQ(result.value().slots[0].camera_id, "CAM01");
    EXPECT_EQ(result.value().slots[0].status, CameraSlotStatus::ready);
    ASSERT_TRUE(result.value().slots[0].device);
    EXPECT_EQ(result.value().slots[0].device->ip_address, "192.0.2.99");
    EXPECT_EQ(result.value().slots[1].status, CameraSlotStatus::occupied);
    EXPECT_EQ(result.value().slots[2].status, CameraSlotStatus::missing);
    EXPECT_EQ(result.value().slots[3].status, CameraSlotStatus::missing);
    ASSERT_EQ(result.value().unexpected_devices.size(), 1U);
    EXPECT_EQ(result.value().unexpected_devices.front().serial_number, "SERIAL-9999");
}

TEST(CameraInventory, RejectsInvalidSlotsAndDuplicateConfiguredSerials)
{
    std::vector<CameraSlotBinding> bindings = {{"CAM05", "SERIAL-0001"}};
    auto result = reconcile_camera_slots(bindings, {});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "CAMERA_CONFIG_FAILED");
    EXPECT_EQ(result.error().details.front().value, "invalid-or-duplicate-camera-id");

    bindings = {{"CAM01", "SERIAL-0001"}, {"CAM02", "SERIAL-0001"}};
    result = reconcile_camera_slots(bindings, {});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().details.front().value, "invalid-or-duplicate-configured-serial");

    bindings = {{"CAM01", "SERIAL-0001"},
                {"CAM02", "SERIAL-0002"},
                {"CAM03", "SERIAL-0003"},
                {"CAM04", "SERIAL-0004"},
                {"CAM01", "SERIAL-0005"}};
    result = reconcile_camera_slots(bindings, {});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().details.front().value, "too-many-camera-slots");
}

TEST(CameraFrameBuffer, KeepsFixedCapacityAndRejectsOversizedPayload)
{
    FrameBuffer buffer{8U};
    EXPECT_EQ(buffer.capacity(), 8U);
    EXPECT_EQ(buffer.writable_bytes().size(), 8U);
    EXPECT_TRUE(buffer.set_size(4U));
    EXPECT_EQ(buffer.size(), 4U);
    EXPECT_FALSE(buffer.set_size(9U));
    EXPECT_EQ(buffer.size(), 4U);
    EXPECT_EQ(buffer.capacity(), 8U);
    buffer.clear();
    EXPECT_EQ(buffer.size(), 0U);
}

TEST(CameraFrameView, PreservesMetadataAndSharesReadOnlyBufferWithoutCopy)
{
    static_assert(std::is_same_v<decltype(std::declval<const FrameView&>().bytes()),
                                 std::span<const std::byte>>);

    auto buffer = std::make_shared<FrameBuffer>(4U);
    std::fill(buffer->writable_bytes().begin(), buffer->writable_bytes().end(), std::byte{0x33});
    ASSERT_TRUE(buffer->set_size(4U));
    const auto* original_data = buffer->bytes().data();
    const auto monotonic = MonotonicTime{std::chrono::milliseconds{123}};
    const auto wall = WallClockTime{std::chrono::milliseconds{456}};

    FramePacket packet{.camera_id = "CAM01",
                       .camera_frame_number = 42U,
                       .sequence_number = 9U,
                       .received_monotonic_time = monotonic,
                       .received_wall_clock_time = wall,
                       .camera_timestamp =
                           CameraTimestamp{77U, 1000U, CameraTimestampQuality::synchronized},
                       .geometry = {2U, 2U, 2U},
                       .pixel_format = PixelFormat::mono8,
                       .buffer = buffer,
                       .flags = {.incomplete = true}};

    auto result = make_frame_view(packet);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().camera_id(), "CAM01");
    EXPECT_EQ(result.value().camera_frame_number(), 42U);
    EXPECT_EQ(result.value().sequence_number(), 9U);
    EXPECT_EQ(result.value().received_monotonic_time(), monotonic);
    EXPECT_EQ(result.value().received_wall_clock_time(), wall);
    ASSERT_TRUE(result.value().camera_timestamp());
    EXPECT_EQ(result.value().camera_timestamp()->ticks, 77U);
    EXPECT_TRUE(result.value().flags().incomplete);
    EXPECT_EQ(result.value().bytes().data(), original_data);
    EXPECT_EQ(result.value().buffer_owner().get(), buffer.get());

    packet.buffer.reset();
    buffer.reset();
    EXPECT_EQ(result.value().bytes().size(), 4U);
    EXPECT_EQ(result.value().bytes().data(), original_data);
}

TEST(CameraFrameView, RejectsMissingBufferAndPayloadLayoutMismatch)
{
    FramePacket packet{.camera_id = "CAM01", .geometry = {2U, 2U, 2U}};
    auto result = make_frame_view(packet);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().details.front().value, "missing-buffer");

    auto buffer = std::make_shared<FrameBuffer>(3U);
    ASSERT_TRUE(buffer->set_size(3U));
    packet.buffer = std::move(buffer);
    result = make_frame_view(packet);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().details.front().value, "payload-size-mismatch");
}

TEST(CameraInterfaces, WorkThroughVendorIndependentProviderAndDeviceContracts)
{
    FakeCameraProvider provider;
    auto devices = provider.enumerate_devices();
    ASSERT_TRUE(devices);
    ASSERT_EQ(devices.value().size(), 1U);
    EXPECT_TRUE(validate_device_inventory(devices.value()));

    auto created = provider.create_device("MOCK-0001");
    ASSERT_TRUE(created);
    EXPECT_EQ(created.value()->descriptor().serial_number, "MOCK-0001");
    EXPECT_TRUE(created.value()->connect());
    EXPECT_TRUE(created.value()->start_acquisition());

    FrameBuffer destination{4U};
    auto captured = created.value()->capture_into(destination, std::chrono::milliseconds{10});
    ASSERT_TRUE(captured);
    EXPECT_EQ(captured.value().camera_frame_number, 7U);
    EXPECT_EQ(destination.size(), 4U);
    EXPECT_TRUE(created.value()->stop_acquisition());
    EXPECT_TRUE(created.value()->disconnect());
}

TEST(CameraControlRuntime, ControlsMockDeviceAndReadsBackActualValues)
{
    auto provider = paperbreak::camera::mock::MockCameraProvider::create(
        {{.descriptor = {.model_name = "Mock",
                         .serial_number = "MOCK-01",
                         .ip_address = "127.0.0.1",
                         .network_interface = "loopback"},
          .width = 64U,
          .height = 48U,
          .frame_rate = 30.0}});
    ASSERT_TRUE(provider);
    std::shared_ptr<paperbreak::camera::ICameraProvider> shared{std::move(provider).value()};
    paperbreak::camera::CameraControlRuntime runtime{shared};
    ASSERT_TRUE(runtime.discover());
    ASSERT_TRUE(runtime.connect("CAM01", "MOCK-01"));
    auto conflicting = runtime.connect("CAM01", "OTHER-SERIAL");
    ASSERT_FALSE(conflicting);
    EXPECT_EQ(conflicting.error().business_code, "CAMERA_CONFIG_FAILED");
    auto configured = runtime.update("CAM01", {.exposure_us = 100.0, .gain_db = 2.0});
    ASSERT_TRUE(configured);
    ASSERT_TRUE(configured.value().actual.has_value());
    EXPECT_DOUBLE_EQ(configured.value().actual->exposure_us.value(), 100.0);
    ASSERT_TRUE(runtime.start("CAM01"));
    ASSERT_TRUE(runtime.stop("CAM01"));
    ASSERT_TRUE(runtime.disconnect("CAM01"));
}

TEST(CameraControlRuntime, ForwardsBoundedFramesWhileAcquiringAndStopsDeterministically)
{
    auto provider = paperbreak::camera::mock::MockCameraProvider::create(
        {{.descriptor = {.model_name = "Mock",
                         .serial_number = "MOCK-PREVIEW-01",
                         .ip_address = "127.0.0.1",
                         .network_interface = "loopback"},
          .width = 64U,
          .height = 48U,
          .frame_rate = 30.0}});
    ASSERT_TRUE(provider);
    std::shared_ptr<paperbreak::camera::ICameraProvider> shared{std::move(provider).value()};
    std::mutex mutex;
    std::condition_variable condition;
    std::uint64_t delivered{};
    bool frames_valid{true};
    paperbreak::camera::CameraControlRuntime runtime{
        shared, [&](paperbreak::camera::FrameView frame) {
            {
                std::scoped_lock lock{mutex};
                frames_valid =
                    frames_valid && frame.camera_id() == "CAM01" &&
                    frame.geometry() == paperbreak::camera::FrameGeometry{64U, 48U, 64U} &&
                    frame.bytes().size() == 64U * 48U;
                ++delivered;
            }
            condition.notify_all();
        }};

    ASSERT_TRUE(runtime.connect("CAM01", "MOCK-PREVIEW-01"));
    ASSERT_TRUE(runtime.start("CAM01"));
    {
        std::unique_lock lock{mutex};
        ASSERT_TRUE(
            condition.wait_for(lock, std::chrono::seconds{2}, [&] { return delivered >= 2U; }));
        EXPECT_TRUE(frames_valid);
    }
    auto acquisition = runtime.get("CAM01", "MOCK-PREVIEW-01");
    ASSERT_TRUE(acquisition);
    ASSERT_TRUE(acquisition.value().acquisition.has_value());
    EXPECT_GE(acquisition.value().acquisition->frames_received, 2U);
    EXPECT_TRUE(acquisition.value().acquisition->last_frame_wall_clock_time.has_value());
    std::uint64_t before_update{};
    {
        std::scoped_lock lock{mutex};
        before_update = delivered;
    }
    auto updated = runtime.update("CAM01", {.exposure_us = 100.0});
    ASSERT_TRUE(updated);
    EXPECT_EQ(updated.value().state, paperbreak::camera::CameraControlState::acquiring);
    {
        std::unique_lock lock{mutex};
        ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds{2},
                                       [&] { return delivered > before_update; }));
    }
    ASSERT_TRUE(runtime.stop("CAM01"));
    std::uint64_t stopped_count{};
    {
        std::scoped_lock lock{mutex};
        stopped_count = delivered;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    {
        std::scoped_lock lock{mutex};
        EXPECT_EQ(delivered, stopped_count);
    }
    ASSERT_TRUE(runtime.disconnect("CAM01"));
}
