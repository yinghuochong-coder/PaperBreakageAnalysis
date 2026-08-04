#pragma once

#include "paperbreak/camera/frame.hpp"
#include "paperbreak/common/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>

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

/// Minimal M5 detector output consumed by the later candidate state machine.
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
};

/// M5-only candidate trigger boundary. The full plugin lifecycle is introduced in M6-01.
class ITriggerDetector
{
  public:
    virtual ~ITriggerDetector() = default;

    ITriggerDetector() = default;
    ITriggerDetector(const ITriggerDetector&) = delete;
    ITriggerDetector& operator=(const ITriggerDetector&) = delete;
    ITriggerDetector(ITriggerDetector&&) = delete;
    ITriggerDetector& operator=(ITriggerDetector&&) = delete;

    [[nodiscard]] virtual Result<TriggerResult> process(const camera::FrameView& frame) = 0;
};

} // namespace paperbreak::algorithm
