#include "paperbreak/algorithm/mock_trigger_detector.hpp"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace paperbreak::algorithm::mock
{
namespace
{

Error config_error(const MockTriggerDetectorConfig& config, std::string reason)
{
    auto error = make_error("SYS_CONFIG_INVALID", Severity::error, "模拟检测器配置无效",
                            "algorithm", "algorithm.mock.create");
    if (!config.camera_id.empty())
        error.source_id = config.camera_id;
    error.details.push_back({"reason", std::move(reason)});
    return error;
}

Error process_error(const std::string& camera_id, std::string reason)
{
    auto error = make_error("ALGORITHM_PROCESS_FAILED", Severity::error, "模拟检测器无法处理帧",
                            "algorithm", "algorithm.mock.process");
    if (!camera_id.empty())
        error.source_id = camera_id;
    error.details.push_back({"reason", std::move(reason)});
    return error;
}

bool normalized(const double value) noexcept
{
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool valid_mode(const MockTriggerMode mode) noexcept
{
    switch (mode)
    {
    case MockTriggerMode::manual_only:
    case MockTriggerMode::fixed_period:
    case MockTriggerMode::mean_grayscale_change:
    case MockTriggerMode::roi_paper_ratio:
        return true;
    }
    return false;
}

DetectionRegion full_frame_region(const camera::FrameGeometry geometry) noexcept
{
    return {.offset_x = 0U, .offset_y = 0U, .width = geometry.width, .height = geometry.height};
}

bool region_fits(const DetectionRegion& region, const camera::FrameGeometry geometry) noexcept
{
    return region.width != 0U && region.height != 0U && region.offset_x <= geometry.width &&
           region.offset_y <= geometry.height && region.width <= geometry.width - region.offset_x &&
           region.height <= geometry.height - region.offset_y;
}

std::optional<double> mean_grayscale(const camera::FrameView& frame,
                                     const DetectionRegion& region) noexcept
{
    const auto geometry = frame.geometry();
    if (frame.pixel_format() != camera::PixelFormat::mono8 || geometry.stride < geometry.width ||
        !region_fits(region, geometry))
    {
        return std::nullopt;
    }

    const auto bytes = frame.bytes();
    long double sum{};
    for (std::uint32_t y = 0U; y < region.height; ++y)
    {
        const auto row = static_cast<std::size_t>(region.offset_y + y) * geometry.stride;
        for (std::uint32_t x = 0U; x < region.width; ++x)
        {
            sum += std::to_integer<unsigned int>(bytes[row + region.offset_x + x]);
        }
    }

    const auto count = static_cast<double>(region.width) * static_cast<double>(region.height);
    return static_cast<double>(sum / count / 255.0L);
}

std::optional<double> paper_ratio(const camera::FrameView& frame, const DetectionRegion& region,
                                  const std::uint8_t threshold) noexcept
{
    const auto geometry = frame.geometry();
    if (frame.pixel_format() != camera::PixelFormat::mono8 || geometry.stride < geometry.width ||
        !region_fits(region, geometry))
    {
        return std::nullopt;
    }

    const auto bytes = frame.bytes();
    std::uint64_t paper_pixels{};
    for (std::uint32_t y = 0U; y < region.height; ++y)
    {
        const auto row = static_cast<std::size_t>(region.offset_y + y) * geometry.stride;
        for (std::uint32_t x = 0U; x < region.width; ++x)
        {
            if (std::to_integer<unsigned int>(bytes[row + region.offset_x + x]) >= threshold)
                ++paper_pixels;
        }
    }

    const auto count = static_cast<double>(region.width) * static_cast<double>(region.height);
    return static_cast<double>(paper_pixels) / count;
}

TriggerResult base_result(const camera::FrameView& frame)
{
    return {.triggered = false,
            .trigger_source = TriggerSource::none,
            .camera_id = frame.camera_id(),
            .sequence_number = frame.sequence_number(),
            .camera_frame_number = frame.camera_frame_number(),
            .monotonic_time = frame.received_monotonic_time(),
            .wall_clock_time = frame.received_wall_clock_time(),
            .evaluated_region = full_frame_region(frame.geometry())};
}

void mark_triggered(TriggerResult& result, const TriggerSource source, std::string reason)
{
    result.triggered = true;
    result.trigger_source = source;
    result.reason = std::move(reason);
}

} // namespace

struct MockTriggerDetector::Impl final
{
    explicit Impl(MockTriggerDetectorConfig value) : config(std::move(value)) {}

    MockTriggerDetectorConfig config;
    std::atomic_bool manual_pending{};
    std::optional<camera::MonotonicTime> last_frame_time;
    std::optional<std::uint64_t> last_sequence_number;
    std::optional<camera::MonotonicTime> period_anchor;
    std::optional<double> previous_mean_grayscale;
};

Result<std::unique_ptr<MockTriggerDetector>> MockTriggerDetector::create(
    MockTriggerDetectorConfig config)
{
    if (config.camera_id.empty())
    {
        return Result<std::unique_ptr<MockTriggerDetector>>::failure(
            config_error(config, "missing-camera-id"));
    }
    if (!valid_mode(config.mode))
    {
        return Result<std::unique_ptr<MockTriggerDetector>>::failure(
            config_error(config, "invalid-trigger-mode"));
    }
    if (config.mode == MockTriggerMode::fixed_period && config.fixed_period.count() <= 0)
    {
        return Result<std::unique_ptr<MockTriggerDetector>>::failure(
            config_error(config, "invalid-fixed-period"));
    }
    if (config.mode == MockTriggerMode::mean_grayscale_change &&
        (!normalized(config.mean_grayscale_change_threshold) ||
         config.mean_grayscale_change_threshold <= 0.0))
    {
        return Result<std::unique_ptr<MockTriggerDetector>>::failure(
            config_error(config, "invalid-grayscale-change-threshold"));
    }
    if (config.mode == MockTriggerMode::roi_paper_ratio &&
        (config.roi.width == 0U || config.roi.height == 0U ||
         !normalized(config.minimum_paper_ratio)))
    {
        return Result<std::unique_ptr<MockTriggerDetector>>::failure(
            config_error(config, "invalid-paper-ratio-configuration"));
    }

    auto detector = std::make_unique<MockTriggerDetector>(ConstructionKey{}, std::move(config));
    return Result<std::unique_ptr<MockTriggerDetector>>::success(std::move(detector));
}

MockTriggerDetector::MockTriggerDetector(ConstructionKey, MockTriggerDetectorConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

MockTriggerDetector::~MockTriggerDetector() = default;

ManualTriggerRequestStatus MockTriggerDetector::request_manual_trigger() noexcept
{
    bool expected = false;
    if (impl_->manual_pending.compare_exchange_strong(expected, true, std::memory_order_release,
                                                      std::memory_order_relaxed))
    {
        return ManualTriggerRequestStatus::accepted;
    }
    return ManualTriggerRequestStatus::already_pending;
}

Result<TriggerResult> MockTriggerDetector::process(const camera::FrameView& frame)
{
    if (frame.camera_id() != impl_->config.camera_id)
    {
        return Result<TriggerResult>::failure(
            process_error(impl_->config.camera_id, "camera-id-mismatch"));
    }
    if (frame.flags().incomplete)
    {
        return Result<TriggerResult>::failure(
            process_error(impl_->config.camera_id, "incomplete-frame"));
    }
    if (impl_->last_sequence_number && frame.sequence_number() <= *impl_->last_sequence_number)
    {
        return Result<TriggerResult>::failure(
            process_error(impl_->config.camera_id, "non-increasing-sequence-number"));
    }
    if (impl_->last_frame_time && frame.received_monotonic_time() < *impl_->last_frame_time)
    {
        return Result<TriggerResult>::failure(
            process_error(impl_->config.camera_id, "monotonic-time-regression"));
    }

    auto result = base_result(frame);
    switch (impl_->config.mode)
    {
    case MockTriggerMode::manual_only:
        break;
    case MockTriggerMode::fixed_period:
        if (!impl_->period_anchor)
        {
            impl_->period_anchor = frame.received_monotonic_time();
        }
        else if (frame.received_monotonic_time() - *impl_->period_anchor >=
                 impl_->config.fixed_period)
        {
            mark_triggered(result, TriggerSource::fixed_period, "fixed-period-elapsed");
            impl_->period_anchor = frame.received_monotonic_time();
        }
        break;
    case MockTriggerMode::mean_grayscale_change: {
        const auto mean = mean_grayscale(frame, result.evaluated_region);
        if (!mean)
        {
            return Result<TriggerResult>::failure(
                process_error(impl_->config.camera_id, "mono8-frame-required"));
        }
        result.mean_grayscale = *mean;
        if (impl_->previous_mean_grayscale)
        {
            result.mean_grayscale_change = std::abs(*mean - *impl_->previous_mean_grayscale);
            if (result.mean_grayscale_change >= impl_->config.mean_grayscale_change_threshold)
            {
                mark_triggered(result, TriggerSource::mean_grayscale_change,
                               "mean-grayscale-change-threshold-reached");
            }
        }
        impl_->previous_mean_grayscale = *mean;
        break;
    }
    case MockTriggerMode::roi_paper_ratio: {
        result.evaluated_region = impl_->config.roi;
        const auto ratio =
            paper_ratio(frame, impl_->config.roi, impl_->config.paper_grayscale_threshold);
        if (!ratio)
        {
            const auto reason = frame.pixel_format() == camera::PixelFormat::mono8
                                    ? "roi-out-of-frame"
                                    : "mono8-frame-required";
            return Result<TriggerResult>::failure(process_error(impl_->config.camera_id, reason));
        }
        result.paper_ratio = *ratio;
        if (*ratio < impl_->config.minimum_paper_ratio)
        {
            mark_triggered(result, TriggerSource::roi_paper_ratio, "paper-ratio-below-minimum");
        }
        break;
    }
    }

    impl_->last_frame_time = frame.received_monotonic_time();
    impl_->last_sequence_number = frame.sequence_number();

    if (impl_->manual_pending.exchange(false, std::memory_order_acq_rel))
        mark_triggered(result, TriggerSource::manual_test, "manual-test-requested");

    return Result<TriggerResult>::success(std::move(result));
}

} // namespace paperbreak::algorithm::mock
