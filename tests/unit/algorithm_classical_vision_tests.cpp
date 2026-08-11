#include "paperbreak/algorithm/classical_vision_detector.hpp"

#include "paperbreak/camera/frame.hpp"

#include <gtest/gtest.h>

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
using paperbreak::algorithm::DetectionCandidateType;
using paperbreak::algorithm::DetectorConfig;
using paperbreak::algorithm::DetectorHost;
using paperbreak::algorithm::DetectorParameter;
using paperbreak::algorithm::DetectorPluginRegistry;
using paperbreak::algorithm::TriggerSource;
using paperbreak::algorithm::classical::classical_vision_plugin_id;
using paperbreak::camera::FrameBuffer;
using paperbreak::camera::FramePacket;
using paperbreak::camera::FrameView;
using paperbreak::camera::PixelFormat;

DetectorParameter integer(std::string name, const std::int64_t value)
{
    return {.name = std::move(name), .value = value};
}

DetectorParameter real(std::string name, const double value)
{
    return {.name = std::move(name), .value = value};
}

DetectorParameter boolean(std::string name, const bool value)
{
    return {.name = std::move(name), .value = value};
}

DetectorConfig config(const std::uint64_t revision = 1U,
                      std::vector<DetectorParameter> parameters = {})
{
    return {.plugin_id = std::string{classical_vision_plugin_id},
            .camera_id = "CAM01",
            .revision = revision,
            .processing_timeout = 500ms,
            .parameters = std::move(parameters)};
}

FrameView make_frame(const std::uint64_t sequence, const std::chrono::milliseconds monotonic,
                     const std::uint32_t width, const std::uint32_t height,
                     const std::uint32_t stride, const std::vector<std::uint8_t>& pixels,
                     const PixelFormat format = PixelFormat::mono8, const bool incomplete = false,
                     std::string camera_id = "CAM01")
{
    if (pixels.size() != static_cast<std::size_t>(stride) * height)
        throw std::runtime_error("invalid test payload");
    auto buffer = std::make_shared<FrameBuffer>(pixels.size());
    for (std::size_t index = 0U; index < pixels.size(); ++index)
        buffer->writable_bytes()[index] = static_cast<std::byte>(pixels[index]);
    if (!buffer->set_size(pixels.size()))
        throw std::runtime_error("test buffer size failed");

    FramePacket packet{
        .camera_id = std::move(camera_id),
        .camera_frame_number = sequence + 100U,
        .sequence_number = sequence,
        .received_monotonic_time = paperbreak::camera::MonotonicTime{} + monotonic,
        .received_wall_clock_time = paperbreak::camera::WallClockTime{} + monotonic,
        .geometry = {.width = width, .height = height, .stride = stride},
        .pixel_format = format,
        .buffer = std::move(buffer),
        .flags = {.incomplete = incomplete},
    };
    auto view = paperbreak::camera::make_frame_view(packet);
    if (!view)
        throw std::runtime_error("test frame view failed");
    return std::move(view).value();
}

FrameView uniform_frame(const std::uint64_t sequence, const std::chrono::milliseconds monotonic,
                        const std::uint8_t value, const std::uint32_t width = 4U,
                        const std::uint32_t height = 4U,
                        const PixelFormat format = PixelFormat::mono8,
                        const bool incomplete = false)
{
    return make_frame(sequence, monotonic, width, height, width,
                      std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height, value),
                      format, incomplete);
}

std::unique_ptr<DetectorHost> make_host(const DetectorConfig& detector_config,
                                        DetectorPluginRegistry& registry)
{
    auto registered =
        paperbreak::algorithm::classical::register_classical_vision_detector(registry);
    if (!registered)
        throw std::runtime_error("classical detector registration failed");
    auto host = std::make_unique<DetectorHost>(registry);
    auto loaded = host->load(detector_config);
    if (!loaded)
        throw std::runtime_error("classical detector load failed");
    return host;
}

TEST(AlgorithmClassicalVision, RegistersThroughHostAndReportsPrototypeResult)
{
    DetectorPluginRegistry registry;
    auto registered =
        paperbreak::algorithm::classical::register_classical_vision_detector(registry);
    ASSERT_TRUE(registered);
    EXPECT_EQ(registry.size(), 1U);

    DetectorHost host{registry};
    ASSERT_TRUE(host.load(config()));
    auto info = host.info();
    ASSERT_TRUE(info);
    EXPECT_EQ(info.value().plugin_id, classical_vision_plugin_id);
    EXPECT_EQ(info.value().implementation_version, "1.0.0-prototype");
    EXPECT_EQ(info.value().model_version, "none");
    EXPECT_TRUE(info.value().supports_hot_update);
    EXPECT_TRUE(info.value().prototype_only);

    auto result = host.process(uniform_frame(1U, 1ms, 200U));
    ASSERT_TRUE(result);
    EXPECT_FALSE(result.value().anomalous);
    EXPECT_EQ(result.value().trigger_source, TriggerSource::none);
    EXPECT_EQ(result.value().evaluated_region.width, 4U);
    EXPECT_DOUBLE_EQ(result.value().paper_ratio, 1.0);
    EXPECT_NEAR(result.value().mean_grayscale, 200.0 / 255.0, 1e-9);
    EXPECT_EQ(result.value().detector_version, "1.0.0-prototype");
    EXPECT_EQ(result.value().model_version, "none");
    EXPECT_EQ(result.value().debug_metrics.size(), 11U);
    EXPECT_EQ(host.metrics().process_successes, 1U);
}

TEST(AlgorithmClassicalVision, HonorsSubRoiAndPaddedStride)
{
    DetectorPluginRegistry registry;
    auto detector_config = config(1U, {integer("roi_offset_x", 1), integer("roi_offset_y", 1),
                                       integer("roi_width", 2), integer("roi_height", 1)});
    auto host = make_host(detector_config, registry);
    const std::vector<std::uint8_t> pixels{0U, 0U,  0U,  0U, 77U, 77U, 0U, 255U, 255U,
                                           0U, 77U, 77U, 0U, 0U,  0U,  0U, 77U,  77U};

    auto result = host->process(make_frame(1U, 1ms, 4U, 3U, 6U, pixels));
    ASSERT_TRUE(result);
    EXPECT_FALSE(result.value().anomalous);
    EXPECT_EQ(result.value().evaluated_region.offset_x, 1U);
    EXPECT_EQ(result.value().evaluated_region.offset_y, 1U);
    EXPECT_EQ(result.value().evaluated_region.width, 2U);
    EXPECT_EQ(result.value().evaluated_region.height, 1U);
    EXPECT_DOUBLE_EQ(result.value().mean_grayscale, 1.0);
    EXPECT_DOUBLE_EQ(result.value().paper_ratio, 1.0);
}

TEST(AlgorithmClassicalVision, FullFrameSentinelHonorsPaddedStride)
{
    DetectorPluginRegistry registry;
    auto host = make_host(config(1U, {integer("roi_offset_x", 0), integer("roi_offset_y", 0),
                                      integer("roi_width", 0), integer("roi_height", 0)}),
                          registry);
    const std::vector<std::uint8_t> pixels{0U, 255U, 77U, 77U, 255U, 255U, 77U, 77U};

    auto result = host->process(make_frame(1U, 1ms, 2U, 2U, 4U, pixels));
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().evaluated_region.width, 2U);
    EXPECT_EQ(result.value().evaluated_region.height, 2U);
    EXPECT_NEAR(result.value().mean_grayscale, 191.25 / 255.0, 1e-12);
    EXPECT_DOUBLE_EQ(result.value().paper_ratio, 0.75);
}

TEST(AlgorithmClassicalVision, DetectsMissingPaperWithAreaAndConfidence)
{
    DetectorPluginRegistry registry;
    auto host = make_host(config(1U, {boolean("enable_mean_change", false),
                                      boolean("enable_background_compare", false),
                                      real("minimum_paper_ratio", 0.75)}),
                          registry);
    ASSERT_TRUE(host->process(uniform_frame(1U, 1ms, 255U)));

    std::vector<std::uint8_t> half_missing(16U, 255U);
    for (std::size_t index = 0U; index < 8U; ++index)
        half_missing[index] = 0U;
    auto result = host->process(make_frame(2U, 2ms, 4U, 4U, 4U, half_missing));
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().triggered);
    EXPECT_TRUE(result.value().anomalous);
    EXPECT_EQ(result.value().trigger_source, TriggerSource::roi_paper_ratio);
    EXPECT_EQ(result.value().candidate_type, DetectionCandidateType::paper_missing);
    EXPECT_EQ(result.value().reason, "paper-ratio-below-minimum");
    EXPECT_DOUBLE_EQ(result.value().paper_ratio, 0.5);
    EXPECT_DOUBLE_EQ(result.value().area_ratio, 0.5);
    EXPECT_DOUBLE_EQ(result.value().confidence, 0.5);
}

TEST(AlgorithmClassicalVision, DoesNotSeedBackgroundFromMissingPaper)
{
    DetectorPluginRegistry registry;
    auto host = make_host(config(1U, {boolean("enable_mean_change", false)}), registry);

    auto missing = host->process(uniform_frame(1U, 1ms, 0U));
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing.value().trigger_source, TriggerSource::roi_paper_ratio);

    auto healthy = host->process(uniform_frame(2U, 2ms, 255U));
    ASSERT_TRUE(healthy);
    EXPECT_FALSE(healthy.value().anomalous);
    EXPECT_EQ(healthy.value().trigger_source, TriggerSource::none);
}

TEST(AlgorithmClassicalVision, DetectsMeanGrayscaleChange)
{
    DetectorPluginRegistry registry;
    auto host = make_host(config(1U, {boolean("enable_paper_ratio", false),
                                      boolean("enable_background_compare", false),
                                      real("maximum_mean_grayscale_change", 0.20)}),
                          registry);
    ASSERT_TRUE(host->process(uniform_frame(1U, 1ms, 200U)));

    auto result = host->process(uniform_frame(2U, 2ms, 100U));
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().anomalous);
    EXPECT_EQ(result.value().trigger_source, TriggerSource::mean_grayscale_change);
    EXPECT_EQ(result.value().candidate_type, DetectionCandidateType::paper_break);
    EXPECT_EQ(result.value().reason, "mean-grayscale-change-threshold-reached");
    EXPECT_NEAR(result.value().mean_grayscale_change, 100.0 / 255.0, 1e-9);
    EXPECT_NEAR(result.value().confidence, 100.0 / 255.0, 1e-9);
    EXPECT_DOUBLE_EQ(result.value().area_ratio, 1.0);
}

TEST(AlgorithmClassicalVision, DetectsLocalizedBackgroundChange)
{
    DetectorPluginRegistry registry;
    auto host = make_host(
        config(1U, {boolean("enable_paper_ratio", false), boolean("enable_mean_change", false),
                    real("maximum_background_change", 0.05),
                    real("background_pixel_change_threshold", 0.20)}),
        registry);
    ASSERT_TRUE(host->process(uniform_frame(1U, 1ms, 100U)));

    std::vector<std::uint8_t> localized(16U, 100U);
    for (std::size_t index = 0U; index < 4U; ++index)
        localized[index] = 200U;
    auto result = host->process(make_frame(2U, 2ms, 4U, 4U, 4U, localized));
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().anomalous);
    EXPECT_EQ(result.value().trigger_source, TriggerSource::background_change);
    EXPECT_EQ(result.value().candidate_type, DetectionCandidateType::paper_break);
    EXPECT_EQ(result.value().reason, "background-change-threshold-reached");
    EXPECT_NEAR(result.value().change_score, 25.0 / 255.0, 1e-9);
    EXPECT_DOUBLE_EQ(result.value().area_ratio, 0.25);
    EXPECT_NEAR(result.value().confidence, 25.0 / 255.0, 1e-9);
}

TEST(AlgorithmClassicalVision, FusedStatisticsPreserveBackgroundLearningAndThresholdBoundary)
{
    DetectorPluginRegistry registry;
    auto host = make_host(
        config(1U, {boolean("enable_paper_ratio", false), boolean("enable_mean_change", false),
                    real("maximum_background_change", 0.50),
                    real("background_pixel_change_threshold", 10.0 / 255.0),
                    real("background_learning_rate", 0.50)}),
        registry);
    ASSERT_TRUE(host->process(uniform_frame(1U, 1ms, 100U, 2U, 2U)));

    auto learned = host->process(uniform_frame(2U, 2ms, 110U, 2U, 2U));
    ASSERT_TRUE(learned);
    EXPECT_FALSE(learned.value().anomalous);
    EXPECT_NEAR(learned.value().change_score, 10.0 / 255.0, 1e-12);
    EXPECT_DOUBLE_EQ(learned.value().area_ratio, 1.0);

    auto updated = host->process(uniform_frame(3U, 3ms, 110U, 2U, 2U));
    ASSERT_TRUE(updated);
    EXPECT_FALSE(updated.value().anomalous);
    EXPECT_NEAR(updated.value().change_score, 5.0 / 255.0, 1e-12);
    EXPECT_DOUBLE_EQ(updated.value().area_ratio, 0.0);
}

TEST(AlgorithmClassicalVision, ResetAndHotUpdateRebuildTemporalState)
{
    DetectorPluginRegistry registry;
    auto host = make_host(
        config(1U, {boolean("enable_paper_ratio", false), boolean("enable_mean_change", false),
                    real("maximum_background_change", 0.10)}),
        registry);
    ASSERT_TRUE(host->process(uniform_frame(1U, 1ms, 100U)));
    auto changed = host->process(uniform_frame(2U, 2ms, 200U));
    ASSERT_TRUE(changed);
    EXPECT_TRUE(changed.value().anomalous);

    ASSERT_TRUE(host->reset());
    auto reset_baseline = host->process(uniform_frame(1U, 1ms, 200U));
    ASSERT_TRUE(reset_baseline);
    EXPECT_FALSE(reset_baseline.value().anomalous);

    ASSERT_TRUE(host->update_config(
        config(2U, {boolean("enable_paper_ratio", false), boolean("enable_mean_change", false),
                    real("maximum_background_change", 0.50)})));
    auto updated_baseline = host->process(uniform_frame(1U, 1ms, 20U));
    ASSERT_TRUE(updated_baseline);
    EXPECT_FALSE(updated_baseline.value().anomalous);
    ASSERT_NE(host->active_config(), nullptr);
    EXPECT_EQ(host->active_config()->revision, 2U);

    auto invalid = config(3U, {real("not_a_parameter", 1.0)});
    auto rejected = host->update_config(invalid);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().business_code, "SYS_CONFIG_INVALID");
    EXPECT_EQ(host->active_config()->revision, 2U);
}

TEST(AlgorithmClassicalVision, RejectsInvalidConfiguration)
{
    DetectorPluginRegistry registry;
    ASSERT_TRUE(paperbreak::algorithm::classical::register_classical_vision_detector(registry));

    const std::vector<DetectorConfig> invalid{
        config(1U, {integer("roi_width", 4)}),
        config(1U, {integer("paper_grayscale_threshold", 256)}),
        config(1U, {integer("minimum_paper_ratio", 1)}),
        config(1U, {real("maximum_background_change", 0.0)}),
        config(1U, {real("background_learning_rate", 1.0)}),
        config(1U, {real("unknown", 0.5)}),
    };
    for (const auto& value : invalid)
    {
        DetectorHost host{registry};
        auto loaded = host.load(value);
        ASSERT_FALSE(loaded);
        EXPECT_EQ(loaded.error().business_code, "SYS_CONFIG_INVALID");
    }
}

TEST(AlgorithmClassicalVision, RejectsInvalidFramesWithoutAdvancingState)
{
    DetectorPluginRegistry registry;
    auto host = make_host(config(1U, {integer("roi_offset_x", 2), integer("roi_offset_y", 0),
                                      integer("roi_width", 2), integer("roi_height", 2)}),
                          registry);

    auto non_mono = host->process(uniform_frame(1U, 1ms, 100U, 4U, 4U, PixelFormat::mono10));
    ASSERT_FALSE(non_mono);
    EXPECT_EQ(non_mono.error().business_code, "ALGORITHM_PROCESS_FAILED");
    auto incomplete = host->process(uniform_frame(1U, 1ms, 100U, 4U, 4U, PixelFormat::mono8, true));
    ASSERT_FALSE(incomplete);
    auto small = host->process(uniform_frame(1U, 1ms, 100U, 3U, 2U));
    ASSERT_FALSE(small);

    ASSERT_TRUE(host->process(uniform_frame(1U, 2ms, 100U)));
    auto duplicate = host->process(uniform_frame(1U, 3ms, 100U));
    ASSERT_FALSE(duplicate);
    auto time_regression = host->process(uniform_frame(2U, 1ms, 100U));
    ASSERT_FALSE(time_regression);
    auto valid = host->process(uniform_frame(2U, 3ms, 100U));
    EXPECT_TRUE(valid);
}

} // namespace
