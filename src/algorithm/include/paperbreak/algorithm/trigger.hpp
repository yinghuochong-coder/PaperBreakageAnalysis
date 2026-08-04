#pragma once

#include "paperbreak/camera/frame.hpp"
#include "paperbreak/common/result.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace paperbreak::algorithm
{

/// Stable source recorded when a detector asks the event layer to create a candidate.
enum class TriggerSource
{
    none,
    manual_test,
    fixed_period,
    mean_grayscale_change,
    roi_paper_ratio,
};

/// Returns the event-manifest spelling for a trigger source.
[[nodiscard]] std::string_view to_string(TriggerSource source) noexcept;

struct DetectionRegion final
{
    std::uint32_t offset_x{};
    std::uint32_t offset_y{};
    std::uint32_t width{};
    std::uint32_t height{};
    bool operator==(const DetectionRegion&) const = default;
};

enum class DetectionCandidateType
{
    none,
    paper_break,
    paper_missing,
    obstruction,
    flicker,
    indeterminate,
};

struct DetectionDebugMetric final
{
    std::string name;
    double value{};
    bool operator==(const DetectionDebugMetric&) const = default;
};

/// Detector output. The M5 trigger fields remain stable for the event state machine.
struct TriggerResult final
{
    bool triggered{};
    TriggerSource trigger_source{TriggerSource::none};
    std::string camera_id;
    std::uint64_t sequence_number{};
    std::uint64_t camera_frame_number{};
    camera::MonotonicTime monotonic_time;
    camera::WallClockTime wall_clock_time;
    DetectionRegion evaluated_region;
    double mean_grayscale{};
    double mean_grayscale_change{};
    double paper_ratio{};
    std::string reason;
    bool anomalous{};
    DetectionCandidateType candidate_type{DetectionCandidateType::none};
    double confidence{};
    double area_ratio{};
    double change_score{};
    std::chrono::microseconds processing_time{};
    std::string detector_version;
    std::string model_version;
    std::vector<DetectionDebugMetric> debug_metrics;
};

} // namespace paperbreak::algorithm
