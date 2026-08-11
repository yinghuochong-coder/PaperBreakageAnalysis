#include "paperbreak/algorithm/classical_vision_detector.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace paperbreak::algorithm::classical
{
namespace
{

constexpr std::size_t maximum_roi_pixels = 4U * 1024U * 1024U;
constexpr std::string_view implementation_version = "1.0.0-prototype";

struct ClassicalConfig final
{
    DetectionRegion roi;
    std::uint8_t paper_grayscale_threshold{16U};
    double minimum_paper_ratio{0.75};
    double maximum_mean_grayscale_change{0.20};
    double maximum_background_change{0.15};
    double background_pixel_change_threshold{0.10};
    double background_learning_rate{0.02};
    bool enable_paper_ratio{true};
    bool enable_mean_change{true};
    bool enable_background_compare{true};
};

Error config_error(const std::string_view camera_id, std::string reason)
{
    auto error = make_error("SYS_CONFIG_INVALID", Severity::error, "传统视觉检测器配置无效",
                            "algorithm", "algorithm.classical.configure");
    if (!camera_id.empty())
        error.source_id = std::string{camera_id};
    error.details.push_back({"reason", std::move(reason)});
    return error;
}

Error process_error(const std::string_view camera_id, std::string reason)
{
    auto error = make_error("ALGORITHM_PROCESS_FAILED", Severity::error, "传统视觉检测器无法处理帧",
                            "algorithm", "algorithm.classical.process");
    if (!camera_id.empty())
        error.source_id = std::string{camera_id};
    error.details.push_back({"reason", std::move(reason)});
    return error;
}

bool normalized(const double value) noexcept
{
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

template <typename T> const T* parameter_value(const DetectorParameter& parameter) noexcept
{
    return std::get_if<T>(&parameter.value);
}

Result<std::int64_t> integer_parameter(const DetectorParameter& parameter,
                                       const std::string_view camera_id)
{
    if (const auto* value = parameter_value<std::int64_t>(parameter))
        return Result<std::int64_t>::success(*value);
    return Result<std::int64_t>::failure(config_error(camera_id, parameter.name + "-type"));
}

Result<double> double_parameter(const DetectorParameter& parameter,
                                const std::string_view camera_id)
{
    if (const auto* value = parameter_value<double>(parameter))
        return Result<double>::success(*value);
    return Result<double>::failure(config_error(camera_id, parameter.name + "-type"));
}

Result<bool> bool_parameter(const DetectorParameter& parameter, const std::string_view camera_id)
{
    if (const auto* value = parameter_value<bool>(parameter))
        return Result<bool>::success(*value);
    return Result<bool>::failure(config_error(camera_id, parameter.name + "-type"));
}

Result<void> assign_u32(const DetectorParameter& parameter, const std::string_view camera_id,
                        std::uint32_t& destination)
{
    auto parsed = integer_parameter(parameter, camera_id);
    if (!parsed)
        return Result<void>::failure(std::move(parsed.error()));
    if (parsed.value() < 0 ||
        static_cast<std::uint64_t>(parsed.value()) > std::numeric_limits<std::uint32_t>::max())
    {
        return Result<void>::failure(config_error(camera_id, parameter.name + "-range"));
    }
    destination = static_cast<std::uint32_t>(parsed.value());
    return Result<void>::success();
}

Result<ClassicalConfig> parse_config(const DetectorConfig& config)
{
    if (config.plugin_id != classical_vision_plugin_id || config.camera_id.empty() ||
        config.revision == 0U || config.processing_timeout.count() <= 0)
    {
        return Result<ClassicalConfig>::failure(config_error(config.camera_id, "identity"));
    }

    ClassicalConfig parsed;
    for (const auto& parameter : config.parameters)
    {
        Result<void> applied = Result<void>::success();
        if (parameter.name == "roi_offset_x")
            applied = assign_u32(parameter, config.camera_id, parsed.roi.offset_x);
        else if (parameter.name == "roi_offset_y")
            applied = assign_u32(parameter, config.camera_id, parsed.roi.offset_y);
        else if (parameter.name == "roi_width")
            applied = assign_u32(parameter, config.camera_id, parsed.roi.width);
        else if (parameter.name == "roi_height")
            applied = assign_u32(parameter, config.camera_id, parsed.roi.height);
        else if (parameter.name == "paper_grayscale_threshold")
        {
            auto value = integer_parameter(parameter, config.camera_id);
            if (!value)
                return Result<ClassicalConfig>::failure(std::move(value.error()));
            if (value.value() < 0 || value.value() > 255)
                return Result<ClassicalConfig>::failure(
                    config_error(config.camera_id, parameter.name + "-range"));
            parsed.paper_grayscale_threshold = static_cast<std::uint8_t>(value.value());
        }
        else if (parameter.name == "minimum_paper_ratio")
        {
            auto value = double_parameter(parameter, config.camera_id);
            if (!value)
                return Result<ClassicalConfig>::failure(std::move(value.error()));
            parsed.minimum_paper_ratio = value.value();
        }
        else if (parameter.name == "maximum_mean_grayscale_change")
        {
            auto value = double_parameter(parameter, config.camera_id);
            if (!value)
                return Result<ClassicalConfig>::failure(std::move(value.error()));
            parsed.maximum_mean_grayscale_change = value.value();
        }
        else if (parameter.name == "maximum_background_change")
        {
            auto value = double_parameter(parameter, config.camera_id);
            if (!value)
                return Result<ClassicalConfig>::failure(std::move(value.error()));
            parsed.maximum_background_change = value.value();
        }
        else if (parameter.name == "background_pixel_change_threshold")
        {
            auto value = double_parameter(parameter, config.camera_id);
            if (!value)
                return Result<ClassicalConfig>::failure(std::move(value.error()));
            parsed.background_pixel_change_threshold = value.value();
        }
        else if (parameter.name == "background_learning_rate")
        {
            auto value = double_parameter(parameter, config.camera_id);
            if (!value)
                return Result<ClassicalConfig>::failure(std::move(value.error()));
            parsed.background_learning_rate = value.value();
        }
        else if (parameter.name == "enable_paper_ratio")
        {
            auto value = bool_parameter(parameter, config.camera_id);
            if (!value)
                return Result<ClassicalConfig>::failure(std::move(value.error()));
            parsed.enable_paper_ratio = value.value();
        }
        else if (parameter.name == "enable_mean_change")
        {
            auto value = bool_parameter(parameter, config.camera_id);
            if (!value)
                return Result<ClassicalConfig>::failure(std::move(value.error()));
            parsed.enable_mean_change = value.value();
        }
        else if (parameter.name == "enable_background_compare")
        {
            auto value = bool_parameter(parameter, config.camera_id);
            if (!value)
                return Result<ClassicalConfig>::failure(std::move(value.error()));
            parsed.enable_background_compare = value.value();
        }
        else
        {
            return Result<ClassicalConfig>::failure(
                config_error(config.camera_id, "unknown-parameter:" + parameter.name));
        }
        if (!applied)
            return Result<ClassicalConfig>::failure(std::move(applied.error()));
    }

    const auto full_frame = parsed.roi.width == 0U && parsed.roi.height == 0U;
    if ((parsed.roi.width == 0U) != (parsed.roi.height == 0U) ||
        (full_frame && (parsed.roi.offset_x != 0U || parsed.roi.offset_y != 0U)))
    {
        return Result<ClassicalConfig>::failure(config_error(config.camera_id, "roi-shape"));
    }
    if (!full_frame &&
        static_cast<std::uint64_t>(parsed.roi.width) * parsed.roi.height > maximum_roi_pixels)
    {
        return Result<ClassicalConfig>::failure(config_error(config.camera_id, "roi-too-large"));
    }
    if (!normalized(parsed.minimum_paper_ratio) ||
        !normalized(parsed.maximum_mean_grayscale_change) ||
        parsed.maximum_mean_grayscale_change <= 0.0 ||
        !normalized(parsed.maximum_background_change) || parsed.maximum_background_change <= 0.0 ||
        !normalized(parsed.background_pixel_change_threshold) ||
        parsed.background_pixel_change_threshold <= 0.0 ||
        !normalized(parsed.background_learning_rate) || parsed.background_learning_rate >= 1.0)
    {
        return Result<ClassicalConfig>::failure(config_error(config.camera_id, "threshold-range"));
    }
    return Result<ClassicalConfig>::success(parsed);
}

DetectionRegion effective_region(const ClassicalConfig& config,
                                 const camera::FrameGeometry geometry) noexcept
{
    if (config.roi.width == 0U)
        return {.offset_x = 0U, .offset_y = 0U, .width = geometry.width, .height = geometry.height};
    return config.roi;
}

bool region_fits(const DetectionRegion& region, const camera::FrameGeometry geometry) noexcept
{
    return region.width != 0U && region.height != 0U && region.offset_x <= geometry.width &&
           region.offset_y <= geometry.height && region.width <= geometry.width - region.offset_x &&
           region.height <= geometry.height - region.offset_y;
}

TriggerResult base_result(const camera::FrameView& frame, const DetectionRegion& region)
{
    return {.triggered = false,
            .trigger_source = TriggerSource::none,
            .camera_id = frame.camera_id(),
            .sequence_number = frame.sequence_number(),
            .camera_frame_number = frame.camera_frame_number(),
            .monotonic_time = frame.received_monotonic_time(),
            .wall_clock_time = frame.received_wall_clock_time(),
            .evaluated_region = region,
            .detector_version = std::string{implementation_version},
            .model_version = "none"};
}

void mark_anomaly(TriggerResult& result, const TriggerSource source,
                  const DetectionCandidateType type, const double confidence, std::string reason)
{
    result.triggered = true;
    result.trigger_source = source;
    result.reason = std::move(reason);
    result.anomalous = true;
    result.candidate_type = type;
    result.confidence = std::clamp(confidence, 0.0, 1.0);
}

class ClassicalVisionDetector final : public IBreakDetector
{
  public:
    [[nodiscard]] Result<void> initialize(const DetectorConfig& config) override
    {
        auto parsed = parse_config(config);
        if (!parsed)
            return Result<void>::failure(std::move(parsed.error()));
        lifecycle_config_ = config;
        config_ = std::move(parsed).value();
        initialized_ = true;
        clear_state();
        return Result<void>::success();
    }

    [[nodiscard]] Result<DetectionResult> process(const camera::FrameView& frame) override
    {
        if (!initialized_)
            return Result<DetectionResult>::failure(process_error({}, "not-initialized"));
        if (frame.camera_id() != lifecycle_config_.camera_id)
            return Result<DetectionResult>::failure(
                process_error(lifecycle_config_.camera_id, "camera-id-mismatch"));
        if (frame.flags().incomplete)
            return Result<DetectionResult>::failure(
                process_error(lifecycle_config_.camera_id, "incomplete-frame"));
        if (frame.pixel_format() != camera::PixelFormat::mono8)
            return Result<DetectionResult>::failure(
                process_error(lifecycle_config_.camera_id, "mono8-frame-required"));
        if (last_sequence_number_ && frame.sequence_number() <= *last_sequence_number_)
            return Result<DetectionResult>::failure(
                process_error(lifecycle_config_.camera_id, "non-increasing-sequence-number"));
        if (last_frame_time_ && frame.received_monotonic_time() < *last_frame_time_)
            return Result<DetectionResult>::failure(
                process_error(lifecycle_config_.camera_id, "monotonic-time-regression"));

        const auto geometry = frame.geometry();
        const auto region = effective_region(config_, geometry);
        if (geometry.stride < geometry.width || !region_fits(region, geometry))
            return Result<DetectionResult>::failure(
                process_error(lifecycle_config_.camera_id, "roi-out-of-frame"));
        const auto pixels = static_cast<std::uint64_t>(region.width) * region.height;
        if (pixels > maximum_roi_pixels)
            return Result<DetectionResult>::failure(
                process_error(lifecycle_config_.camera_id, "roi-too-large"));

        try
        {
            const auto bytes = frame.bytes();
            cv::Mat image(static_cast<int>(geometry.height), static_cast<int>(geometry.width),
                          CV_8UC1, const_cast<std::byte*>(bytes.data()), geometry.stride);
            const auto roi =
                image(cv::Rect{static_cast<int>(region.offset_x), static_cast<int>(region.offset_y),
                               static_cast<int>(region.width), static_cast<int>(region.height)});

            const bool has_background = !background_.empty();
            if (has_background && (background_.rows != roi.rows || background_.cols != roi.cols))
                return Result<DetectionResult>::failure(
                    process_error(lifecycle_config_.camera_id, "frame-geometry-changed"));
            const auto pixel_threshold = static_cast<std::uint8_t>(std::clamp(
                std::lround(config_.background_pixel_change_threshold * 255.0), 1L, 255L));
            std::uint64_t grayscale_sum{};
            std::uint64_t paper_pixels{};
            std::uint64_t background_difference_sum{};
            std::uint64_t changed_pixels{};
            for (int row = 0; row < roi.rows; ++row)
            {
                const auto* source = roi.ptr<std::uint8_t>(row);
                const auto* background =
                    has_background ? background_.ptr<std::uint8_t>(row) : nullptr;
                for (int column = 0; column < roi.cols; ++column)
                {
                    const auto value = source[column];
                    grayscale_sum += value;
                    paper_pixels += value >= config_.paper_grayscale_threshold ? 1U : 0U;
                    if (background != nullptr)
                    {
                        const auto difference = static_cast<std::uint8_t>(std::abs(
                            static_cast<int>(value) - static_cast<int>(background[column])));
                        background_difference_sum += difference;
                        changed_pixels += difference >= pixel_threshold ? 1U : 0U;
                    }
                }
            }

            auto result = base_result(frame, region);
            result.mean_grayscale =
                static_cast<double>(grayscale_sum) / (static_cast<double>(pixels) * 255.0);
            if (previous_mean_)
                result.mean_grayscale_change = std::abs(result.mean_grayscale - *previous_mean_);

            result.paper_ratio = static_cast<double>(paper_pixels) / static_cast<double>(pixels);

            double background_mean_change{};
            double background_changed_ratio{};
            if (config_.enable_background_compare && background_.empty() &&
                (!config_.enable_paper_ratio || result.paper_ratio >= config_.minimum_paper_ratio))
            {
                roi.copyTo(background_);
            }
            else if (config_.enable_background_compare && has_background)
            {
                background_mean_change = static_cast<double>(background_difference_sum) /
                                         (static_cast<double>(pixels) * 255.0);
                background_changed_ratio =
                    static_cast<double>(changed_pixels) / static_cast<double>(pixels);
            }

            result.change_score = std::max(result.mean_grayscale_change, background_mean_change);
            result.area_ratio = background_changed_ratio;
            if (config_.enable_paper_ratio && result.paper_ratio < config_.minimum_paper_ratio)
            {
                const auto missing_ratio = 1.0 - result.paper_ratio;
                result.area_ratio = missing_ratio;
                mark_anomaly(result, TriggerSource::roi_paper_ratio,
                             DetectionCandidateType::paper_missing, missing_ratio,
                             "paper-ratio-below-minimum");
            }
            else if (config_.enable_background_compare &&
                     background_mean_change >= config_.maximum_background_change)
            {
                mark_anomaly(result, TriggerSource::background_change,
                             DetectionCandidateType::paper_break, background_mean_change,
                             "background-change-threshold-reached");
            }
            else if (config_.enable_mean_change && previous_mean_ &&
                     result.mean_grayscale_change >= config_.maximum_mean_grayscale_change)
            {
                result.area_ratio = 1.0;
                mark_anomaly(result, TriggerSource::mean_grayscale_change,
                             DetectionCandidateType::paper_break, result.mean_grayscale_change,
                             "mean-grayscale-change-threshold-reached");
            }

            if (!result.anomalous && !background_.empty() && config_.background_learning_rate > 0.0)
            {
                cv::addWeighted(roi, config_.background_learning_rate, background_,
                                1.0 - config_.background_learning_rate, 0.0, background_);
            }

            result.debug_metrics.reserve(11U);
            result.debug_metrics.push_back({"meanGrayscale", result.mean_grayscale});
            result.debug_metrics.push_back({"meanGrayscaleChange", result.mean_grayscale_change});
            result.debug_metrics.push_back({"paperRatio", result.paper_ratio});
            result.debug_metrics.push_back({"backgroundMeanChange", background_mean_change});
            result.debug_metrics.push_back({"backgroundChangedRatio", background_changed_ratio});
            result.debug_metrics.push_back({"minimumPaperRatio", config_.minimum_paper_ratio});
            result.debug_metrics.push_back(
                {"maximumMeanGrayscaleChange", config_.maximum_mean_grayscale_change});
            result.debug_metrics.push_back(
                {"maximumBackgroundChange", config_.maximum_background_change});
            result.debug_metrics.push_back(
                {"backgroundPixelChangeThreshold", config_.background_pixel_change_threshold});
            result.debug_metrics.push_back(
                {"backgroundLearningRate", config_.background_learning_rate});
            result.debug_metrics.push_back({"roiPixels", static_cast<double>(pixels)});

            previous_mean_ = result.mean_grayscale;
            last_sequence_number_ = frame.sequence_number();
            last_frame_time_ = frame.received_monotonic_time();
            return Result<DetectionResult>::success(std::move(result));
        }
        catch (const cv::Exception&)
        {
            return Result<DetectionResult>::failure(
                process_error(lifecycle_config_.camera_id, "opencv-failure"));
        }
    }

    [[nodiscard]] Result<void> update_config(const DetectorConfig& config) override
    {
        if (!initialized_ || config.plugin_id != lifecycle_config_.plugin_id ||
            config.camera_id != lifecycle_config_.camera_id ||
            config.revision <= lifecycle_config_.revision)
        {
            return Result<void>::failure(config_error(config.camera_id, "invalid-hot-update"));
        }
        auto parsed = parse_config(config);
        if (!parsed)
            return Result<void>::failure(std::move(parsed.error()));
        lifecycle_config_ = config;
        config_ = std::move(parsed).value();
        clear_state();
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> reset() override
    {
        if (!initialized_)
            return Result<void>::failure(config_error({}, "not-initialized"));
        clear_state();
        return Result<void>::success();
    }

    [[nodiscard]] DetectorInfo info() const override
    {
        return {.plugin_id = std::string{classical_vision_plugin_id},
                .display_name = "M6 Classical Vision Prototype",
                .implementation_version = std::string{implementation_version},
                .model_version = "none",
                .supports_hot_update = true,
                .prototype_only = true};
    }

  private:
    void clear_state() noexcept
    {
        previous_mean_.reset();
        last_sequence_number_.reset();
        last_frame_time_.reset();
        background_.release();
    }

    DetectorConfig lifecycle_config_;
    ClassicalConfig config_;
    bool initialized_{};
    std::optional<double> previous_mean_;
    std::optional<std::uint64_t> last_sequence_number_;
    std::optional<camera::MonotonicTime> last_frame_time_;
    cv::Mat background_;
};

} // namespace

Result<std::unique_ptr<IBreakDetector>> make_classical_vision_detector()
{
    return Result<std::unique_ptr<IBreakDetector>>::success(
        std::make_unique<ClassicalVisionDetector>());
}

Result<void> register_classical_vision_detector(DetectorPluginRegistry& registry)
{
    return registry.register_plugin(std::string{classical_vision_plugin_id},
                                    [] { return make_classical_vision_detector(); });
}

} // namespace paperbreak::algorithm::classical
