#pragma once

#include "paperbreak/algorithm/detector_host.hpp"

#include <memory>
#include <string_view>

namespace paperbreak::algorithm::classical
{

inline constexpr std::string_view classical_vision_plugin_id = "classical-vision";

/// Creates an uninitialized M6-02 classical detector instance.
[[nodiscard]] Result<std::unique_ptr<IBreakDetector>> make_classical_vision_detector();

/// Registers the built-in detector factory with the M6-01 compile-time registry.
[[nodiscard]] Result<void> register_classical_vision_detector(DetectorPluginRegistry& registry);

} // namespace paperbreak::algorithm::classical
