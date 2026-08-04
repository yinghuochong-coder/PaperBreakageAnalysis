#pragma once

#include "paperbreak/algorithm/trigger.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace paperbreak::algorithm::mock
{

enum class MockTriggerMode
{
    manual_only,
    fixed_period,
    mean_grayscale_change,
    roi_paper_ratio,
};

struct MockTriggerDetectorConfig final
{
    std::string camera_id;
    MockTriggerMode mode{MockTriggerMode::manual_only};
    std::chrono::milliseconds fixed_period{1000};
    double mean_grayscale_change_threshold{0.25};
    DetectionRegion roi;
    std::uint8_t paper_grayscale_threshold{128U};
    double minimum_paper_ratio{0.75};
};

enum class ManualTriggerRequestStatus
{
    accepted,
    already_pending,
};

/// Deterministic first-stage detector. One instance is bound to one logical camera.
class MockTriggerDetector final : public ITriggerDetector
{
  public:
    class ConstructionKey final
    {
      public:
        ConstructionKey(const ConstructionKey&) = default;

      private:
        friend class MockTriggerDetector;
        ConstructionKey() = default;
    };

    [[nodiscard]] static Result<std::unique_ptr<MockTriggerDetector>> create(
        MockTriggerDetectorConfig config);

    ~MockTriggerDetector() override;
    MockTriggerDetector(const MockTriggerDetector&) = delete;
    MockTriggerDetector& operator=(const MockTriggerDetector&) = delete;
    MockTriggerDetector(MockTriggerDetector&&) = delete;
    MockTriggerDetector& operator=(MockTriggerDetector&&) = delete;

    /// Requests one ManualTest result on the next valid frame. Capacity is exactly one request.
    [[nodiscard]] ManualTriggerRequestStatus request_manual_trigger() noexcept;
    [[nodiscard]] Result<TriggerResult> process(const camera::FrameView& frame) override;

    /// ConstructionKey keeps direct construction unavailable while allowing std::make_unique.
    MockTriggerDetector(ConstructionKey, MockTriggerDetectorConfig config);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace paperbreak::algorithm::mock
