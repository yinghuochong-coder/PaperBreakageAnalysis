#pragma once

#include "paperbreak/algorithm/detector_host.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace paperbreak::algorithm::mock
{

inline constexpr std::string_view mock_trigger_plugin_id = "mock-trigger";

/// Creates an uninitialized detector for the compile-time plugin registry.
[[nodiscard]] Result<std::unique_ptr<IBreakDetector>> make_mock_trigger_detector();
[[nodiscard]] Result<void> register_mock_trigger_detector(DetectorPluginRegistry& registry);

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
        friend Result<std::unique_ptr<IBreakDetector>> make_mock_trigger_detector();
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
    [[nodiscard]] Result<void> initialize(const DetectorConfig& config) override;
    [[nodiscard]] Result<TriggerResult> process(const camera::FrameView& frame) override;
    [[nodiscard]] Result<void> update_config(const DetectorConfig& config) override;
    [[nodiscard]] Result<void> reset() override;
    [[nodiscard]] DetectorInfo info() const override;

    /// ConstructionKey keeps direct construction unavailable while allowing std::make_unique.
    MockTriggerDetector(ConstructionKey, MockTriggerDetectorConfig config);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace paperbreak::algorithm::mock
