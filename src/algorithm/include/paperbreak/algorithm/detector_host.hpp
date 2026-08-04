#pragma once

#include "paperbreak/algorithm/detector.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace paperbreak::algorithm
{

using DetectorFactory = std::function<Result<std::unique_ptr<IBreakDetector>>()>;

class DetectorPluginRegistry final
{
  public:
    static constexpr std::size_t maximum_plugins = 16U;

    DetectorPluginRegistry();
    ~DetectorPluginRegistry();
    DetectorPluginRegistry(const DetectorPluginRegistry&) = delete;
    DetectorPluginRegistry& operator=(const DetectorPluginRegistry&) = delete;
    DetectorPluginRegistry(DetectorPluginRegistry&&) = delete;
    DetectorPluginRegistry& operator=(DetectorPluginRegistry&&) = delete;

    [[nodiscard]] Result<void> register_plugin(std::string plugin_id, DetectorFactory factory);
    [[nodiscard]] Result<std::unique_ptr<IBreakDetector>> create(std::string_view plugin_id) const;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct DetectorHostMetrics final
{
    std::uint64_t process_calls{};
    std::uint64_t process_successes{};
    std::uint64_t process_failures{};
    std::uint64_t process_timeouts{};
    std::uint64_t successful_config_updates{};
    std::uint64_t reset_calls{};
    std::chrono::microseconds last_processing_time{};
    std::chrono::microseconds maximum_processing_time{};
    bool operator==(const DetectorHostMetrics&) const = default;
};

/// Serial lifecycle host. It does not create worker threads or attempt unsafe preemption.
class DetectorHost final
{
  public:
    explicit DetectorHost(const DetectorPluginRegistry& registry);
    ~DetectorHost();
    DetectorHost(const DetectorHost&) = delete;
    DetectorHost& operator=(const DetectorHost&) = delete;
    DetectorHost(DetectorHost&&) = delete;
    DetectorHost& operator=(DetectorHost&&) = delete;

    [[nodiscard]] Result<void> load(const DetectorConfig& config);
    [[nodiscard]] Result<void> update_config(const DetectorConfig& config);
    [[nodiscard]] Result<DetectionResult> process(const camera::FrameView& frame);
    [[nodiscard]] Result<void> reset();
    [[nodiscard]] Result<DetectorInfo> info() const;
    [[nodiscard]] const DetectorConfig* active_config() const noexcept;
    [[nodiscard]] DetectorHostMetrics metrics() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace paperbreak::algorithm
