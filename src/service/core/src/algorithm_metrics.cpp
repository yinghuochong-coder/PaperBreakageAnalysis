#include "paperbreak/service/algorithm_metrics.hpp"

#include "paperbreak/service/event_runtime.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace paperbreak::service
{
namespace
{

class AlgorithmMetricSource final : public monitoring::IMetricSource
{
  public:
    explicit AlgorithmMetricSource(std::weak_ptr<EventRuntime> runtime)
        : runtime_(std::move(runtime))
    {
    }

    [[nodiscard]] std::string_view source_name() const noexcept override
    {
        return "algorithm";
    }

    [[nodiscard]] Result<std::vector<monitoring::MetricPoint>> collect(
        std::stop_token) noexcept override
    {
        using Point = monitoring::MetricPoint;
        const auto runtime = runtime_.lock();
        const auto snapshot = runtime ? runtime->snapshot() : EventRuntimeSnapshot{};
        const bool available = runtime != nullptr;
        const auto milliseconds = [](const std::chrono::microseconds value) {
            return static_cast<double>(value.count()) / 1000.0;
        };
        std::vector<Point> points{
            {.name = "algorithm.state",
             .value = std::string{to_string(snapshot.algorithm_state)},
             .unit = "state",
             .available = available},
            {.name = "algorithm.frame_duration.current_ms",
             .value = milliseconds(snapshot.last_algorithm_processing_time),
             .unit = "milliseconds",
             .available = available && snapshot.detector_process_calls > 0U},
            {.name = "algorithm.frame_duration.average_ms",
             .value = milliseconds(snapshot.average_algorithm_processing_time),
             .unit = "milliseconds",
             .available = available && snapshot.detector_process_calls > 0U},
            {.name = "algorithm.frame_duration.maximum_ms",
             .value = milliseconds(snapshot.maximum_algorithm_processing_time),
             .unit = "milliseconds",
             .available = available && snapshot.detector_process_calls > 0U},
            {.name = "algorithm.queue.depth",
             .value = static_cast<std::uint64_t>(snapshot.frame_queue_depth),
             .unit = "count",
             .available = available},
            {.name = "algorithm.queue.capacity",
             .value = static_cast<std::uint64_t>(snapshot.frame_queue_capacity),
             .unit = "count",
             .available = available},
            {.name = "algorithm.queue.high_watermark",
             .value = static_cast<std::uint64_t>(snapshot.frame_queue_high_watermark),
             .unit = "count",
             .available = available},
            {.name = "algorithm.queue_wait.current_ms",
             .value = milliseconds(snapshot.last_queue_wait_time),
             .unit = "milliseconds",
             .available = available && snapshot.processed_frames > 0U},
            {.name = "algorithm.queue_wait.average_ms",
             .value = milliseconds(snapshot.average_queue_wait_time),
             .unit = "milliseconds",
             .available = available && snapshot.processed_frames > 0U},
            {.name = "algorithm.queue_wait.maximum_ms",
             .value = milliseconds(snapshot.maximum_queue_wait_time),
             .unit = "milliseconds",
             .available = available && snapshot.processed_frames > 0U},
            {.name = "algorithm.end_to_end.current_ms",
             .value = milliseconds(snapshot.last_end_to_end_time),
             .unit = "milliseconds",
             .available = available && snapshot.processed_frames > 0U},
            {.name = "algorithm.end_to_end.average_ms",
             .value = milliseconds(snapshot.average_end_to_end_time),
             .unit = "milliseconds",
             .available = available && snapshot.processed_frames > 0U},
            {.name = "algorithm.end_to_end.maximum_ms",
             .value = milliseconds(snapshot.maximum_end_to_end_time),
             .unit = "milliseconds",
             .available = available && snapshot.processed_frames > 0U},
            {.name = "algorithm.input_fps",
             .value = snapshot.input_fps,
             .unit = "frames_per_second",
             .available = available},
            {.name = "algorithm.processed_fps",
             .value = snapshot.processed_fps,
             .unit = "frames_per_second",
             .available = available},
            {.name = "algorithm.skipped_ratio",
             .value = snapshot.skipped_ratio,
             .unit = "ratio",
             .available = available},
            {.name = "algorithm.result_queue.depth",
             .value = static_cast<std::uint64_t>(snapshot.result_queue_depth),
             .unit = "count",
             .available = available},
            {.name = "algorithm.result_queue.capacity",
             .value = static_cast<std::uint64_t>(snapshot.result_queue_capacity),
             .unit = "count",
             .available = available},
            {.name = "algorithm.result_queue.high_watermark",
             .value = static_cast<std::uint64_t>(snapshot.result_queue_high_watermark),
             .unit = "count",
             .available = available},
            {.name = "algorithm.result_queue.rejected_total",
             .value = snapshot.result_queue_rejected,
             .unit = "count",
             .available = available},
            {.name = "algorithm.skipped_frames_total",
             .value = snapshot.skipped_frames,
             .unit = "count",
             .available = available},
            {.name = "algorithm.failures_total",
             .value = snapshot.detector_failures,
             .unit = "count",
             .available = available},
            {.name = "algorithm.candidates_total",
             .value = snapshot.candidates_created,
             .unit = "count",
             .available = available},
            {.name = "algorithm.confirmed_total",
             .value = snapshot.confirmed_events,
             .unit = "count",
             .available = available},
            {.name = "algorithm.false_positives_total",
             .value = snapshot.rejected_candidates,
             .unit = "count",
             .available = available}};
        if (runtime)
        {
            for (const auto& lane : runtime->algorithm_snapshots())
            {
                const auto prefix = "algorithm." + lane.camera_id;
                const auto& metrics = lane.metrics;
                points.push_back({.name = prefix + ".state",
                                  .value = std::string{to_string(lane.state)},
                                  .unit = "state",
                                  .available = true});
                points.push_back({.name = prefix + ".queue.depth",
                                  .value = static_cast<std::uint64_t>(metrics.frame_queue_depth),
                                  .unit = "count",
                                  .available = true});
                points.push_back({.name = prefix + ".queue.capacity",
                                  .value = static_cast<std::uint64_t>(metrics.frame_queue_capacity),
                                  .unit = "count",
                                  .available = true});
                points.push_back(
                    {.name = prefix + ".queue.high_watermark",
                     .value = static_cast<std::uint64_t>(metrics.frame_queue_high_watermark),
                     .unit = "count",
                     .available = true});
                points.push_back({.name = prefix + ".submitted_frames_total",
                                  .value = metrics.submitted_frames,
                                  .unit = "count",
                                  .available = true});
                points.push_back({.name = prefix + ".processed_frames_total",
                                  .value = metrics.processed_frames,
                                  .unit = "count",
                                  .available = true});
                points.push_back({.name = prefix + ".skipped_frames_total",
                                  .value = metrics.skipped_frames,
                                  .unit = "count",
                                  .available = true});
                points.push_back({.name = prefix + ".failures_total",
                                  .value = metrics.detector_failures,
                                  .unit = "count",
                                  .available = true});
                points.push_back({.name = prefix + ".result_queue_rejected_total",
                                  .value = metrics.result_queue_rejected,
                                  .unit = "count",
                                  .available = true});
                points.push_back({.name = prefix + ".frame_duration.current_ms",
                                  .value = milliseconds(metrics.last_algorithm_processing_time),
                                  .unit = "milliseconds",
                                  .available = metrics.detector_process_calls > 0U});
                points.push_back({.name = prefix + ".frame_duration.average_ms",
                                  .value = milliseconds(metrics.average_algorithm_processing_time),
                                  .unit = "milliseconds",
                                  .available = metrics.detector_process_calls > 0U});
                points.push_back({.name = prefix + ".frame_duration.maximum_ms",
                                  .value = milliseconds(metrics.maximum_algorithm_processing_time),
                                  .unit = "milliseconds",
                                  .available = metrics.detector_process_calls > 0U});
                points.push_back({.name = prefix + ".queue_wait.current_ms",
                                  .value = milliseconds(metrics.last_queue_wait_time),
                                  .unit = "milliseconds",
                                  .available = metrics.processed_frames > 0U});
                points.push_back({.name = prefix + ".queue_wait.average_ms",
                                  .value = milliseconds(metrics.average_queue_wait_time),
                                  .unit = "milliseconds",
                                  .available = metrics.processed_frames > 0U});
                points.push_back({.name = prefix + ".queue_wait.maximum_ms",
                                  .value = milliseconds(metrics.maximum_queue_wait_time),
                                  .unit = "milliseconds",
                                  .available = metrics.processed_frames > 0U});
                points.push_back({.name = prefix + ".end_to_end.current_ms",
                                  .value = milliseconds(metrics.last_end_to_end_time),
                                  .unit = "milliseconds",
                                  .available = metrics.processed_frames > 0U});
                points.push_back({.name = prefix + ".end_to_end.average_ms",
                                  .value = milliseconds(metrics.average_end_to_end_time),
                                  .unit = "milliseconds",
                                  .available = metrics.processed_frames > 0U});
                points.push_back({.name = prefix + ".end_to_end.maximum_ms",
                                  .value = milliseconds(metrics.maximum_end_to_end_time),
                                  .unit = "milliseconds",
                                  .available = metrics.processed_frames > 0U});
                points.push_back({.name = prefix + ".input_fps",
                                  .value = metrics.input_fps,
                                  .unit = "frames_per_second",
                                  .available = true});
                points.push_back({.name = prefix + ".processed_fps",
                                  .value = metrics.processed_fps,
                                  .unit = "frames_per_second",
                                  .available = true});
                points.push_back({.name = prefix + ".skipped_ratio",
                                  .value = metrics.skipped_ratio,
                                  .unit = "ratio",
                                  .available = true});
            }
        }
        return Result<std::vector<Point>>::success(std::move(points));
    }

  private:
    std::weak_ptr<EventRuntime> runtime_;
};

} // namespace

std::shared_ptr<monitoring::IMetricSource> make_algorithm_metric_source(
    std::weak_ptr<EventRuntime> runtime)
{
    return std::make_shared<AlgorithmMetricSource>(std::move(runtime));
}

} // namespace paperbreak::service
