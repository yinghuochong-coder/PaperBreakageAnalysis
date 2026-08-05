#pragma once

#include "paperbreak/algorithm/detector_host.hpp"
#include "paperbreak/camera/frame.hpp"
#include "paperbreak/common/result.hpp"
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

enum class AlgorithmRuntimeState
{
    disabled,
    active,
    manual_trigger_only,
};

[[nodiscard]] std::string_view to_string(AlgorithmRuntimeState state) noexcept;

struct EventRuntimeOptions final
{
    config::EdgeConfig configuration;
    std::filesystem::path event_root;
    std::shared_ptr<storage::EventMetadataDatabase> database;
    std::shared_ptr<storage::StoragePolicyManager> storage_policy;
    std::shared_ptr<storage::NvmeRollingCache> nvme_cache;
    /// Per-camera algorithm frame capacity; total depth is bounded by this value times lanes.
    std::size_t frame_queue_capacity{event_frame_queue_default_capacity};
    std::size_t persistence_capacity{8U};
    std::size_t consecutive_failure_limit{3U};
    std::size_t consecutive_backlog_limit{8U};
    std::function<Result<void>(algorithm::DetectorPluginRegistry&)> detector_registry_configurer;
    std::function<void(const Error&)> error_observer;
    std::function<void(const storage::EventMetadataRecord&)> committed_observer;
};

struct EventRuntimeSnapshot final
{
    bool started{};
    bool accepting{};
    std::size_t frame_queue_depth{};
    std::size_t frame_queue_capacity{};
    std::size_t frame_queue_high_watermark{};
    std::size_t pending_events{};
    std::uint64_t submitted_frames{};
    std::uint64_t processed_frames{};
    std::uint64_t rejected_frames{};
    std::uint64_t skipped_frames{};
    std::uint64_t detector_failures{};
    std::uint64_t consecutive_detector_failures{};
    std::uint64_t consecutive_backlog_events{};
    std::uint64_t detector_process_calls{};
    std::chrono::microseconds last_algorithm_processing_time{};
    std::chrono::microseconds average_algorithm_processing_time{};
    std::chrono::microseconds maximum_algorithm_processing_time{};
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
    EventRuntimeSnapshot metrics;
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
    [[nodiscard]] Result<AlgorithmFrameTestResult> test_current_frame(
        std::string_view camera_id) const;
    void request_stop() noexcept;
    [[nodiscard]] Result<void> join(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] EventRuntimeSnapshot snapshot() const noexcept;

  private:
    std::unique_ptr<struct EventRuntimeImpl> impl_;
};

} // namespace paperbreak::service
