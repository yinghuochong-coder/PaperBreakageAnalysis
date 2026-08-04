#pragma once

#include "paperbreak/algorithm/trigger.hpp"
#include "paperbreak/common/result.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace paperbreak::algorithm
{

using DetectorParameterValue = std::variant<bool, std::int64_t, double, std::string>;

struct DetectorParameter final
{
    std::string name;
    DetectorParameterValue value;
    bool operator==(const DetectorParameter&) const = default;
};

struct DetectorConfig final
{
    std::string plugin_id;
    std::string camera_id;
    std::uint64_t revision{1U};
    std::chrono::milliseconds processing_timeout{100};
    std::vector<DetectorParameter> parameters;
    bool operator==(const DetectorConfig&) const = default;
};

using DetectionResult = TriggerResult;

struct DetectorInfo final
{
    std::string plugin_id;
    std::string display_name;
    std::string implementation_version;
    std::string model_version;
    bool supports_hot_update{};
    bool prototype_only{true};
    bool operator==(const DetectorInfo&) const = default;
};

/// Stable C++ boundary for detectors compiled into the service process.
class IBreakDetector
{
  public:
    virtual ~IBreakDetector() = default;

    IBreakDetector() = default;
    IBreakDetector(const IBreakDetector&) = delete;
    IBreakDetector& operator=(const IBreakDetector&) = delete;
    IBreakDetector(IBreakDetector&&) = delete;
    IBreakDetector& operator=(IBreakDetector&&) = delete;

    [[nodiscard]] virtual Result<void> initialize(const DetectorConfig& config) = 0;
    [[nodiscard]] virtual Result<DetectionResult> process(const camera::FrameView& frame) = 0;
    [[nodiscard]] virtual Result<void> update_config(const DetectorConfig& config) = 0;
    [[nodiscard]] virtual Result<void> reset() = 0;
    [[nodiscard]] virtual DetectorInfo info() const = 0;
};

/// Source-compatible name retained for the M5 event runtime.
using ITriggerDetector = IBreakDetector;

} // namespace paperbreak::algorithm
