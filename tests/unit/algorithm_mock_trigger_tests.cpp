#include "paperbreak/algorithm/mock_trigger_detector.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace std::chrono_literals;
using paperbreak::algorithm::DetectionRegion;
using paperbreak::algorithm::DetectorConfig;
using paperbreak::algorithm::TriggerSource;
using paperbreak::algorithm::mock::ManualTriggerRequestStatus;
using paperbreak::algorithm::mock::MockTriggerDetector;
using paperbreak::algorithm::mock::MockTriggerDetectorConfig;
using paperbreak::algorithm::mock::MockTriggerMode;
using paperbreak::camera::FrameBuffer;
using paperbreak::camera::FramePacket;
using paperbreak::camera::FrameView;
using paperbreak::camera::PixelFormat;

FrameView make_frame(std::string camera_id, const std::uint64_t sequence_number,
                     const std::chrono::milliseconds monotonic_time, const std::uint32_t width,
                     const std::uint32_t height, const std::uint32_t stride,
                     const std::vector<std::uint8_t>& pixels,
                     const PixelFormat pixel_format = PixelFormat::mono8,
                     const bool incomplete = false)
{
    auto buffer = std::make_shared<FrameBuffer>(pixels.size());
    std::transform(pixels.begin(), pixels.end(), buffer->writable_bytes().begin(),
                   [](const std::uint8_t value) { return static_cast<std::byte>(value); });
    if (!buffer->set_size(pixels.size()))
        throw std::runtime_error("test frame buffer size failed");

    const FramePacket packet{
        .camera_id = std::move(camera_id),
        .camera_frame_number = sequence_number + 100U,
        .sequence_number = sequence_number,
        .received_monotonic_time = paperbreak::camera::MonotonicTime{} + monotonic_time,
        .received_wall_clock_time = paperbreak::camera::WallClockTime{} + monotonic_time,
        .geometry = {.width = width, .height = height, .stride = stride},
        .pixel_format = pixel_format,
        .buffer = std::move(buffer),
        .flags = {.incomplete = incomplete},
    };
    auto view = paperbreak::camera::make_frame_view(packet);
    if (!view)
        throw std::runtime_error("test frame view failed");
    return std::move(view).value();
}

std::unique_ptr<MockTriggerDetector> make_detector(MockTriggerDetectorConfig config)
{
    auto detector = MockTriggerDetector::create(std::move(config));
    if (!detector)
        throw std::runtime_error("test detector creation failed");
    return std::move(detector).value();
}

TEST(AlgorithmMockTrigger, ManualRequestIsBoundedOneShotAndUsesExactSource)
{
    auto detector = make_detector({.camera_id = "CAM01", .mode = MockTriggerMode::manual_only});
    EXPECT_EQ(detector->request_manual_trigger(), ManualTriggerRequestStatus::accepted);
    EXPECT_EQ(detector->request_manual_trigger(), ManualTriggerRequestStatus::already_pending);

    auto first = detector->process(
        make_frame("CAM01", 1U, 1ms, 2U, 1U, 2U, {1U, 2U}, PixelFormat::bayer_rg8));
    ASSERT_TRUE(first);
    EXPECT_TRUE(first.value().triggered);
    EXPECT_EQ(first.value().trigger_source, TriggerSource::manual_test);
    EXPECT_EQ(paperbreak::algorithm::to_string(first.value().trigger_source), "ManualTest");
    EXPECT_EQ(first.value().camera_id, "CAM01");
    EXPECT_EQ(first.value().sequence_number, 1U);
    EXPECT_EQ(first.value().camera_frame_number, 101U);

    auto second = detector->process(
        make_frame("CAM01", 2U, 2ms, 2U, 1U, 2U, {3U, 4U}, PixelFormat::bayer_rg8));
    ASSERT_TRUE(second);
    EXPECT_FALSE(second.value().triggered);
    EXPECT_EQ(second.value().trigger_source, TriggerSource::none);

    EXPECT_EQ(detector->request_manual_trigger(), ManualTriggerRequestStatus::accepted);
    auto third = detector->process(
        make_frame("CAM01", 3U, 3ms, 2U, 1U, 2U, {5U, 6U}, PixelFormat::bayer_rg8));
    ASSERT_TRUE(third);
    EXPECT_TRUE(third.value().triggered);
}

TEST(AlgorithmMockTrigger, InvalidFrameDoesNotConsumePendingManualRequest)
{
    auto detector = make_detector({.camera_id = "CAM01", .mode = MockTriggerMode::manual_only});
    ASSERT_EQ(detector->request_manual_trigger(), ManualTriggerRequestStatus::accepted);

    auto wrong_camera = detector->process(make_frame("CAM02", 1U, 1ms, 1U, 1U, 1U, {7U}));
    ASSERT_FALSE(wrong_camera);
    EXPECT_EQ(wrong_camera.error().business_code, "ALGORITHM_PROCESS_FAILED");
    ASSERT_EQ(wrong_camera.error().details.size(), 1U);
    EXPECT_EQ(wrong_camera.error().details.front().value, "camera-id-mismatch");

    auto valid = detector->process(make_frame("CAM01", 1U, 1ms, 1U, 1U, 1U, {7U}));
    ASSERT_TRUE(valid);
    EXPECT_TRUE(valid.value().triggered);
    EXPECT_EQ(valid.value().trigger_source, TriggerSource::manual_test);
}

TEST(AlgorithmMockTrigger, FixedPeriodUsesMonotonicBoundariesWithoutCatchUpBurst)
{
    auto detector = make_detector(
        {.camera_id = "CAM01", .mode = MockTriggerMode::fixed_period, .fixed_period = 1000ms});

    auto baseline = detector->process(make_frame("CAM01", 1U, 0ms, 1U, 1U, 1U, {1U}));
    auto before = detector->process(make_frame("CAM01", 2U, 999ms, 1U, 1U, 1U, {1U}));
    auto boundary = detector->process(make_frame("CAM01", 3U, 1000ms, 1U, 1U, 1U, {1U}));
    auto jump = detector->process(make_frame("CAM01", 4U, 10000ms, 1U, 1U, 1U, {1U}));
    auto after_jump = detector->process(make_frame("CAM01", 5U, 10001ms, 1U, 1U, 1U, {1U}));

    ASSERT_TRUE(baseline);
    ASSERT_TRUE(before);
    ASSERT_TRUE(boundary);
    ASSERT_TRUE(jump);
    ASSERT_TRUE(after_jump);
    EXPECT_FALSE(baseline.value().triggered);
    EXPECT_FALSE(before.value().triggered);
    EXPECT_EQ(boundary.value().trigger_source, TriggerSource::fixed_period);
    EXPECT_EQ(jump.value().trigger_source, TriggerSource::fixed_period);
    EXPECT_FALSE(after_jump.value().triggered);
}

TEST(AlgorithmMockTrigger, MeanGrayscaleChangeIgnoresStridePaddingAndIncludesExactThreshold)
{
    auto detector = make_detector({.camera_id = "CAM01",
                                   .mode = MockTriggerMode::mean_grayscale_change,
                                   .mean_grayscale_change_threshold = 64.0 / 255.0});

    auto baseline = detector->process(
        make_frame("CAM01", 1U, 1ms, 2U, 2U, 4U, {0U, 0U, 255U, 255U, 0U, 0U, 255U, 255U}));
    auto below = detector->process(
        make_frame("CAM01", 2U, 2ms, 2U, 2U, 4U, {63U, 63U, 0U, 0U, 63U, 63U, 0U, 0U}));
    auto exact = detector->process(
        make_frame("CAM01", 3U, 3ms, 2U, 2U, 4U, {127U, 127U, 255U, 255U, 127U, 127U, 255U, 255U}));

    ASSERT_TRUE(baseline);
    ASSERT_TRUE(below);
    ASSERT_TRUE(exact);
    EXPECT_DOUBLE_EQ(baseline.value().mean_grayscale, 0.0);
    EXPECT_FALSE(baseline.value().triggered);
    EXPECT_FALSE(below.value().triggered);
    EXPECT_TRUE(exact.value().triggered);
    EXPECT_EQ(exact.value().trigger_source, TriggerSource::mean_grayscale_change);
    EXPECT_DOUBLE_EQ(exact.value().mean_grayscale_change, 64.0 / 255.0);
}

TEST(AlgorithmMockTrigger, RoiPaperRatioUsesConfiguredRegionAndStrictMinimum)
{
    auto detector =
        make_detector({.camera_id = "CAM01",
                       .mode = MockTriggerMode::roi_paper_ratio,
                       .roi = {.offset_x = 1U, .offset_y = 0U, .width = 2U, .height = 2U},
                       .paper_grayscale_threshold = 128U,
                       .minimum_paper_ratio = 0.75});

    auto exact = detector->process(
        make_frame("CAM01", 1U, 1ms, 4U, 2U, 4U, {0U, 255U, 255U, 0U, 0U, 255U, 0U, 0U}));
    auto below = detector->process(
        make_frame("CAM01", 2U, 2ms, 4U, 2U, 4U, {0U, 255U, 0U, 0U, 0U, 255U, 0U, 0U}));

    ASSERT_TRUE(exact);
    ASSERT_TRUE(below);
    EXPECT_DOUBLE_EQ(exact.value().paper_ratio, 0.75);
    EXPECT_FALSE(exact.value().triggered);
    EXPECT_EQ(exact.value().evaluated_region,
              (DetectionRegion{.offset_x = 1U, .offset_y = 0U, .width = 2U, .height = 2U}));
    EXPECT_DOUBLE_EQ(below.value().paper_ratio, 0.5);
    EXPECT_TRUE(below.value().triggered);
    EXPECT_EQ(below.value().trigger_source, TriggerSource::roi_paper_ratio);
}

TEST(AlgorithmMockTrigger, RejectsInvalidConfiguration)
{
    auto missing_camera = MockTriggerDetector::create({});
    ASSERT_FALSE(missing_camera);
    EXPECT_EQ(missing_camera.error().business_code, "SYS_CONFIG_INVALID");

    auto bad_period = MockTriggerDetector::create(
        {.camera_id = "CAM01", .mode = MockTriggerMode::fixed_period, .fixed_period = 0ms});
    EXPECT_FALSE(bad_period);

    auto bad_change = MockTriggerDetector::create({.camera_id = "CAM01",
                                                   .mode = MockTriggerMode::mean_grayscale_change,
                                                   .mean_grayscale_change_threshold = 0.0});
    EXPECT_FALSE(bad_change);

    auto bad_roi = MockTriggerDetector::create(
        {.camera_id = "CAM01",
         .mode = MockTriggerMode::roi_paper_ratio,
         .roi = {.offset_x = 0U, .offset_y = 0U, .width = 0U, .height = 1U}});
    EXPECT_FALSE(bad_roi);

    auto bad_ratio = MockTriggerDetector::create(
        {.camera_id = "CAM01",
         .mode = MockTriggerMode::roi_paper_ratio,
         .roi = {.offset_x = 0U, .offset_y = 0U, .width = 1U, .height = 1U},
         .minimum_paper_ratio = 1.1});
    EXPECT_FALSE(bad_ratio);
}

TEST(AlgorithmMockTrigger, PixelAndOrderingErrorsAreRecoverable)
{
    auto grayscale = make_detector({.camera_id = "CAM01",
                                    .mode = MockTriggerMode::mean_grayscale_change,
                                    .mean_grayscale_change_threshold = 0.5});

    auto unsupported =
        grayscale->process(make_frame("CAM01", 1U, 1ms, 1U, 1U, 1U, {0U}, PixelFormat::mono10));
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().details.front().value, "mono8-frame-required");

    auto baseline = grayscale->process(make_frame("CAM01", 1U, 2ms, 1U, 1U, 1U, {0U}));
    ASSERT_TRUE(baseline);

    auto duplicate = grayscale->process(make_frame("CAM01", 1U, 3ms, 1U, 1U, 1U, {255U}));
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().details.front().value, "non-increasing-sequence-number");

    auto time_regression = grayscale->process(make_frame("CAM01", 2U, 1ms, 1U, 1U, 1U, {255U}));
    ASSERT_FALSE(time_regression);
    EXPECT_EQ(time_regression.error().details.front().value, "monotonic-time-regression");

    auto recovered = grayscale->process(make_frame("CAM01", 2U, 3ms, 1U, 1U, 1U, {255U}));
    ASSERT_TRUE(recovered);
    EXPECT_TRUE(recovered.value().triggered);
}

TEST(AlgorithmMockTrigger, RejectsIncompleteFrameAndOutOfBoundsRoi)
{
    auto detector =
        make_detector({.camera_id = "CAM01",
                       .mode = MockTriggerMode::roi_paper_ratio,
                       .roi = {.offset_x = 1U, .offset_y = 0U, .width = 2U, .height = 1U},
                       .minimum_paper_ratio = 0.5});

    auto incomplete = detector->process(
        make_frame("CAM01", 1U, 1ms, 3U, 1U, 3U, {255U, 255U, 255U}, PixelFormat::mono8, true));
    ASSERT_FALSE(incomplete);
    EXPECT_EQ(incomplete.error().details.front().value, "incomplete-frame");

    auto too_narrow = detector->process(make_frame("CAM01", 1U, 2ms, 2U, 1U, 2U, {255U, 255U}));
    ASSERT_FALSE(too_narrow);
    EXPECT_EQ(too_narrow.error().details.front().value, "roi-out-of-frame");

    auto recovered =
        detector->process(make_frame("CAM01", 1U, 3ms, 3U, 1U, 3U, {255U, 255U, 255U}));
    ASSERT_TRUE(recovered);
    EXPECT_FALSE(recovered.value().triggered);
}

TEST(AlgorithmMockTrigger, ImplementsLifecycleInformationHotUpdateAndReset)
{
    auto detector = make_detector({.camera_id = "CAM01", .mode = MockTriggerMode::manual_only});

    const auto info = detector->info();
    EXPECT_EQ(info.plugin_id, paperbreak::algorithm::mock::mock_trigger_plugin_id);
    EXPECT_TRUE(info.supports_hot_update);
    EXPECT_TRUE(info.prototype_only);

    EXPECT_EQ(detector->request_manual_trigger(), ManualTriggerRequestStatus::accepted);
    ASSERT_TRUE(detector->reset());
    auto after_reset = detector->process(make_frame("CAM01", 1U, 1ms, 1U, 1U, 1U, {0U}));
    ASSERT_TRUE(after_reset);
    EXPECT_FALSE(after_reset.value().triggered);

    const DetectorConfig revision_two{
        .plugin_id = std::string{paperbreak::algorithm::mock::mock_trigger_plugin_id},
        .camera_id = "CAM01",
        .revision = 2U,
        .processing_timeout = 50ms};
    ASSERT_TRUE(detector->update_config(revision_two));
    EXPECT_FALSE(detector->update_config(revision_two));

    auto wrong_camera = revision_two;
    wrong_camera.revision = 3U;
    wrong_camera.camera_id = "CAM02";
    EXPECT_FALSE(detector->update_config(wrong_camera));
}

} // namespace
