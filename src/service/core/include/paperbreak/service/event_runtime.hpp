#pragma once

#include "paperbreak/algorithm/detector_host.hpp"
#include "paperbreak/camera/frame.hpp"
#include "paperbreak/common/result.hpp"
#include "paperbreak/common/threading.hpp"
#include "paperbreak/config/basic_config.hpp"
#include "paperbreak/storage/metadata_database.hpp"
#include "paperbreak/storage/nvme_cache.hpp"
#include "paperbreak/storage/storage_policy.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace paperbreak::service
{

inline constexpr std::size_t event_frame_queue_default_capacity = 8U;
inline constexpr std::size_t algorithm_result_queue_default_capacity = 256U;

enum class AlgorithmRuntimeState
{
    disabled,
    active,
    partially_degraded,
    manual_trigger_only,
};

[[nodiscard]] std::string_view to_string(AlgorithmRuntimeState state) noexcept;

struct AlgorithmBacklogStateChange final
{
    std::string camera_id;
    bool active{};
    std::size_t queue_depth{};
    std::size_t queue_capacity{};
    std::uint64_t skipped_frames{};
};

struct AlgorithmDetectorFailureStateChange final
{
    std::string camera_id;
    bool active{};
    std::uint64_t consecutive_failures{};
    std::uint64_t detector_failures{};
    std::size_t failure_limit{};
    std::optional<Error> last_error;
};

struct EventRuntimeOptions final
{
    config::EdgeConfig configuration;
    std::filesystem::path event_root;
    std::shared_ptr<storage::EventMetadataDatabase> database;
    std::shared_ptr<storage::StoragePolicyManager> storage_policy;
    std::shared_ptr<storage::NvmeRollingCache> nvme_cache;
    /// Per-camera algorithm frame capacity; total depth is bounded by this value times lanes.
    std::size_t frame_queue_capacity{event_frame_queue_default_capacity};
    std::size_t result_queue_capacity{algorithm_result_queue_default_capacity};
    std::size_t persistence_capacity{8U};
    std::size_t consecutive_failure_limit{3U};
    /// Drops required in one fixed window for that window to be unhealthy.
    std::size_t consecutive_backlog_limit{8U};
    std::chrono::milliseconds backlog_window{1000};
    std::size_t backlog_degrade_window_limit{5U};
    std::size_t backlog_recovery_window_limit{5U};
    std::function<Result<void>(algorithm::DetectorPluginRegistry&)> detector_registry_configurer;
    std::function<void(const Error&)> error_observer;
    std::function<void(const AlgorithmDetectorFailureStateChange&)> detector_failure_state_observer;
    std::function<void(const AlgorithmBacklogStateChange&)> backlog_state_observer;
    std::function<void(const storage::EventMetadataRecord&)> lifecycle_observer;
    std::function<void(const storage::EventMetadataRecord&)> committed_observer;
    ThreadRegistrationFactory register_thread;
    /// Test seam used to inject deterministic thread-creation failures before a thread is made.
    std::function<Result<void>(std::string_view)> thread_start_gate;
    /// Test seam used to hold the result consumer while exercising bounded overflow behavior.
    std::function<void()> result_consumer_start_gate;
    /// Test seam for deterministic rate/backlog windows; production uses steady_clock::now().
    std::function<std::chrono::steady_clock::time_point()> monotonic_now;
    DebugDiagnosticSink diagnostics;
};

struct AlgorithmLaneMetrics final
{
    std::size_t frame_queue_depth{};
    std::size_t frame_queue_capacity{};
    std::size_t frame_queue_high_watermark{};
    std::uint64_t submitted_frames{};
    std::uint64_t processed_frames{};
    std::uint64_t skipped_frames{};
    std::uint64_t detector_failures{};
    std::uint64_t consecutive_detector_failures{};
    std::uint64_t consecutive_backlog_events{};
    bool backlog_active{};
    std::uint64_t consecutive_bad_backlog_windows{};
    std::uint64_t consecutive_healthy_backlog_windows{};
    std::uint64_t detector_process_calls{};
    std::chrono::microseconds last_algorithm_processing_time{};
    std::chrono::microseconds average_algorithm_processing_time{};
    std::chrono::microseconds maximum_algorithm_processing_time{};
    std::chrono::microseconds last_queue_wait_time{};
    std::chrono::microseconds average_queue_wait_time{};
    std::chrono::microseconds maximum_queue_wait_time{};
    std::chrono::microseconds last_end_to_end_time{};
    std::chrono::microseconds average_end_to_end_time{};
    std::chrono::microseconds maximum_end_to_end_time{};
    double input_fps{};
    double processed_fps{};
    double skipped_ratio{};
    std::uint64_t result_queue_rejected{};
    std::uint64_t candidates_created{};
    std::uint64_t confirmed_events{};
    std::uint64_t rejected_candidates{};
};

struct EventRuntimeSnapshot final
{
    bool started{};
    bool accepting{};
    std::size_t frame_queue_depth{};
    std::size_t frame_queue_capacity{};
    std::size_t frame_queue_high_watermark{};
    std::size_t result_queue_depth{};
    std::size_t result_queue_capacity{};
    std::size_t result_queue_high_watermark{};
    std::size_t pending_events{};
    std::size_t persistence_queue_depth{};
    std::size_t persistence_queue_capacity{};
    std::size_t persistence_queue_high_watermark{};
    std::size_t persistence_active_events{};
    std::uint64_t persistence_last_write_bytes{};
    std::chrono::milliseconds persistence_last_write_duration{};
    double persistence_last_write_mib_per_second{};
    std::uint64_t submitted_frames{};
    std::uint64_t processed_frames{};
    std::uint64_t rejected_frames{};
    std::uint64_t skipped_frames{};
    std::uint64_t detector_failures{};
    std::uint64_t consecutive_detector_failures{};
    std::uint64_t consecutive_backlog_events{};
    std::size_t backlog_active_lanes{};
    std::uint64_t detector_process_calls{};
    std::uint64_t result_queue_rejected{};
    std::chrono::microseconds last_algorithm_processing_time{};
    std::chrono::microseconds average_algorithm_processing_time{};
    std::chrono::microseconds maximum_algorithm_processing_time{};
    std::chrono::microseconds last_queue_wait_time{};
    std::chrono::microseconds average_queue_wait_time{};
    std::chrono::microseconds maximum_queue_wait_time{};
    std::chrono::microseconds last_end_to_end_time{};
    std::chrono::microseconds average_end_to_end_time{};
    std::chrono::microseconds maximum_end_to_end_time{};
    double input_fps{};
    double processed_fps{};
    double skipped_ratio{};
    AlgorithmRuntimeState algorithm_state{AlgorithmRuntimeState::disabled};
    std::uint64_t events_started{};
    std::uint64_t candidates_created{};
    std::uint64_t confirmed_events{};
    std::uint64_t rejected_candidates{};
    std::uint64_t events_frozen{};
    std::uint64_t events_committed{};
    std::uint64_t event_failures{};
};

struct AlgorithmRuntimeSnapshot final
{
    std::string camera_id;
    std::uint64_t config_revision{};
    AlgorithmRuntimeState state{AlgorithmRuntimeState::disabled};
    bool has_current_frame{};
    std::uint64_t latest_sequence_number{};
    std::optional<algorithm::DetectorInfo> detector_info;
    AlgorithmLaneMetrics metrics;
};

struct AlgorithmFrameTestResult final
{
    algorithm::DetectorInfo detector_info;
    algorithm::DetectionResult detection;
    std::uint32_t source_width{};
    std::uint32_t source_height{};
    std::vector<std::byte> preview_jpeg;
};

/// M5 service composition for the bounded in-memory event chain. Camera observers only call
/// submit_frame(), which never waits for encoding, SQLite, or disk I/O.
class EventRuntime final
{
  public:
    class ConstructionKey final
    {
      public:
        ConstructionKey(const ConstructionKey&) = default;

      private:
        friend class EventRuntime;
        ConstructionKey() = default;
    };

    [[nodiscard]] static Result<std::shared_ptr<EventRuntime>> create(EventRuntimeOptions options);

    EventRuntime(ConstructionKey, std::unique_ptr<struct EventRuntimeImpl> impl);
    ~EventRuntime();
    EventRuntime(const EventRuntime&) = delete;
    EventRuntime& operator=(const EventRuntime&) = delete;
    EventRuntime(EventRuntime&&) = delete;
    EventRuntime& operator=(EventRuntime&&) = delete;

    [[nodiscard]] Result<void> start();
    [[nodiscard]] Result<void> submit_frame(camera::FrameView frame);
    [[nodiscard]] Result<bool> request_manual_trigger(std::string_view camera_id);
    [[nodiscard]] Result<void> update_external_confirmation(std::string_view camera_id, bool active,
                                                            camera::MonotonicTime monotonic_time,
                                                            camera::WallClockTime wall_clock_time);
    [[nodiscard]] Result<void> reconfigure(const config::EdgeConfig& configuration);
    [[nodiscard]] Result<AlgorithmRuntimeSnapshot> algorithm_snapshot(
        std::string_view camera_id) const;
    [[nodiscard]] std::vector<AlgorithmRuntimeSnapshot> algorithm_snapshots() const;
    [[nodiscard]] Result<AlgorithmFrameTestResult> test_current_frame(
        std::string_view camera_id) const;
    void request_stop() noexcept;
    [[nodiscard]] Result<void> join(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] EventRuntimeSnapshot snapshot() const noexcept;

  private:
    std::unique_ptr<struct EventRuntimeImpl> impl_;
};

} // namespace paperbreak::service
