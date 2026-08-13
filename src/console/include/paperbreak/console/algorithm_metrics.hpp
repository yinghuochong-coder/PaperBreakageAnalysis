#pragma once

#include "paperbreak/console/algorithm_client.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace paperbreak::console
{

enum class AlgorithmMetricGroup
{
    processing_throughput,
    latency_performance,
    backlog_stability,
    detection_results
};

using AlgorithmMetricScalar =
    std::variant<std::uint64_t, std::int64_t, std::uint32_t, double, bool>;

struct AlgorithmMetricDescriptor final
{
    std::string_view key;
    std::string_view chinese_name;
    std::string_view unit;
    AlgorithmMetricGroup group{};
    std::string_view definition;
    AlgorithmMetricScalar (*read)(const AlgorithmMetricValue&);
};

enum class AlgorithmDebugMetricGroup
{
    image_detection,
    thresholds_background,
    plugin
};

struct AlgorithmDebugMetricDescriptor final
{
    std::string_view key;
    std::string_view chinese_name;
    std::string_view unit;
    AlgorithmDebugMetricGroup group{};
    std::string_view definition;
};

struct AlgorithmMetricPoint final
{
    std::uint64_t sample_sequence{};
    std::chrono::system_clock::time_point sample_time;
    double value{};
};

[[nodiscard]] std::span<const AlgorithmMetricDescriptor> algorithm_metric_descriptors() noexcept;
[[nodiscard]] std::string_view algorithm_metric_group_name(AlgorithmMetricGroup group) noexcept;
[[nodiscard]] std::span<const AlgorithmDebugMetricDescriptor>
algorithm_debug_metric_descriptors() noexcept;
[[nodiscard]] const AlgorithmDebugMetricDescriptor* find_algorithm_debug_metric(
    std::string_view key) noexcept;
[[nodiscard]] std::string_view algorithm_debug_metric_group_name(
    AlgorithmDebugMetricGroup group) noexcept;
[[nodiscard]] AlgorithmMetricScalar algorithm_metric_value(
    const AlgorithmMetricDescriptor& descriptor, const AlgorithmMetricValue& metrics);
[[nodiscard]] std::string algorithm_metric_value_text(const AlgorithmMetricScalar& value);
[[nodiscard]] double algorithm_metric_numeric_value(const AlgorithmMetricScalar& value) noexcept;

class AlgorithmMetricHistory final
{
  public:
    static constexpr std::size_t maximum_points = 100U;

    AlgorithmMetricHistory();
    ~AlgorithmMetricHistory();
    AlgorithmMetricHistory(const AlgorithmMetricHistory&) = delete;
    AlgorithmMetricHistory& operator=(const AlgorithmMetricHistory&) = delete;

    [[nodiscard]] bool ingest(const AlgorithmClientSnapshot& snapshot);
    [[nodiscard]] std::vector<AlgorithmMetricPoint> history(std::string_view camera_id,
                                                            std::string_view metric_key) const;
    [[nodiscard]] std::size_t size(std::string_view camera_id,
                                   std::string_view metric_key) const noexcept;

  private:
    struct CameraHistory;
    std::vector<CameraHistory> cameras_;
};

[[nodiscard]] Result<void> export_algorithm_current_values_csv(
    const AlgorithmClientSnapshot& snapshot, const std::filesystem::path& destination,
    std::chrono::system_clock::time_point exported_at = std::chrono::system_clock::now());

} // namespace paperbreak::console
