#pragma once

#include "paperbreak/common/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace paperbreak::storage
{

inline constexpr std::uint32_t database_schema_version = 5U;
inline constexpr std::size_t database_default_page_size = 50U;
inline constexpr std::size_t database_maximum_page_size = 200U;
inline constexpr std::size_t maximum_upload_job_capacity = 1000000U;
inline constexpr std::size_t maximum_upload_job_history_capacity = 2000000U;
inline constexpr std::uint64_t maximum_upload_pending_bytes =
    16ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t maximum_upload_json_bytes = 1024U * 1024U;

struct MetadataDatabaseOptions final
{
    std::filesystem::path database_path;
    std::filesystem::path event_root;
    std::filesystem::path backup_directory;
    std::chrono::milliseconds busy_timeout{250};
    std::size_t maximum_reconcile_events{10000U};
    std::size_t maximum_manifest_bytes{8U * 1024U * 1024U};
    std::size_t upload_job_capacity{10000U};
    std::size_t upload_job_history_capacity{50000U};
    std::uint64_t upload_pending_byte_capacity{1024ULL * 1024ULL * 1024ULL * 1024ULL};
};

struct MetadataDatabaseOpenReport final
{
    std::uint32_t schema_version{};
    bool created{};
    bool migrated{};
    std::optional<std::filesystem::path> migration_backup;
};

struct DatabaseIntegrityReport final
{
    bool healthy{};
    std::string detail;
};

struct EventMetadataRecord final
{
    std::string event_id;
    std::uint32_t event_schema_version{};
    std::string event_state;
    std::string decision_state;
    std::string persistence_state;
    std::string review_state;
    std::optional<std::string> review_decision;
    bool artifacts_available{};
    std::uint64_t trigger_count{1U};
    std::uint64_t review_revision{1U};
    std::optional<std::int64_t> reviewed_at_utc_ms;
    std::string reviewed_by;
    std::int64_t candidate_time_utc_ms{};
    std::optional<std::int64_t> confirmed_time_utc_ms;
    std::int64_t start_time_utc_ms{};
    std::int64_t end_time_utc_ms{};
    std::vector<std::string> camera_ids;
    std::string trigger_camera_id;
    std::uint64_t trigger_frame_number{};
    std::string trigger_reason;
    double confidence{};
    std::string upload_state;
    std::string storage_state;
    bool retention_locked{};
    bool deletion_allowed{};
    std::string deletion_state;
    std::filesystem::path relative_directory;
};

struct EventQuery final
{
    std::optional<std::string> event_id;
    std::optional<std::int64_t> start_time_utc_ms;
    std::optional<std::int64_t> end_time_utc_ms;
    std::optional<std::string> event_state;
    std::optional<std::string> decision_state;
    std::optional<std::string> persistence_state;
    std::optional<std::string> review_state;
    std::optional<std::string> review_decision;
    std::optional<std::string> camera_id;
    std::size_t offset{};
    std::size_t limit{database_default_page_size};
};

struct EventLifecycleSummary final
{
    std::uint64_t decision_candidates{};
    std::uint64_t decision_confirmed{};
    std::uint64_t decision_rejected{};
    std::uint64_t decision_timeout{};
    std::uint64_t persistence_collecting{};
    std::uint64_t persistence_encoding{};
    std::uint64_t persistence_queued{};
    std::uint64_t persistence_writing{};
    std::uint64_t persistence_committed{};
    std::uint64_t persistence_incomplete{};
    std::uint64_t review_unreviewed{};
    std::uint64_t review_confirmed{};
    std::uint64_t review_rejected{};
};

enum class EventReviewDecision
{
    confirmed,
    rejected,
};

struct EventReviewOutcome final
{
    EventMetadataRecord event;
    bool duplicate{};
};

struct EventQueryPage final
{
    std::vector<EventMetadataRecord> events;
    std::size_t total{};
    std::size_t offset{};
    std::size_t limit{};
    EventLifecycleSummary summary;
};

struct CollectingEventRecord final
{
    std::string event_id;
    std::string decision_state{"Candidate"};
    std::int64_t candidate_time_utc_ms{};
    std::optional<std::int64_t> confirmed_time_utc_ms;
    std::int64_t start_time_utc_ms{};
    std::int64_t end_time_utc_ms{};
    std::vector<std::string> camera_ids;
    std::string trigger_camera_id;
    std::uint64_t trigger_frame_number{};
    std::string trigger_reason;
    double confidence{};
    std::uint64_t trigger_count{1U};
};

struct EventReconcileReport final
{
    std::size_t directories_scanned{};
    std::size_t indexed{};
    std::size_t refreshed{};
    std::size_t marked_missing{};
};

struct EventRetentionRecord final
{
    std::string event_id;
    std::int64_t candidate_time_utc_ms{};
    std::string upload_state;
    std::string storage_state;
    std::filesystem::path relative_directory;
    std::uint64_t indexed_file_bytes{};
    bool locked{};
    bool deletion_allowed{};
    std::string deletion_state;
    std::filesystem::path deletion_relative_path;
    std::string last_error;
};

enum class UploadJobKind
{
    alarm_metadata,
    key_frame,
    manifest,
    low_rate_replay,
    raw_file,
};

enum class UploadJobState
{
    pending,
    in_progress,
    retry_wait,
    completed,
    permanent_failed,
    manual_intervention,
};

enum class UploadFailureClass
{
    retryable,
    permanent,
    manual_intervention,
};

struct UploadJobRequest final
{
    std::string idempotency_key;
    std::optional<std::string> event_id;
    UploadJobKind kind{UploadJobKind::manifest};
    std::string logical_id;
    std::string relative_path;
    std::string payload_json{"{}"};
    std::string checksum;
    std::uint64_t upload_bytes{};
    std::int64_t created_at_utc_ms{};
};

struct UploadJobRecord final
{
    std::int64_t job_id{};
    std::string idempotency_key;
    std::optional<std::string> event_id;
    UploadJobKind kind{UploadJobKind::manifest};
    std::int32_t priority{};
    std::string logical_id;
    std::string relative_path;
    std::string payload_json;
    std::string checksum;
    std::uint64_t upload_bytes{};
    UploadJobState state{UploadJobState::pending};
    std::uint32_t attempts{};
    std::int64_t next_attempt_utc_ms{};
    std::string checkpoint_json{"{}"};
    std::string last_error_code;
    std::int64_t created_at_utc_ms{};
    std::int64_t updated_at_utc_ms{};
};

struct UploadJobEnqueueOutcome final
{
    UploadJobRecord job;
    bool duplicate{};
};

struct UploadQueueStats final
{
    std::size_t active_jobs{};
    std::uint64_t active_bytes{};
    std::size_t pending_jobs{};
    std::size_t in_progress_jobs{};
    std::size_t retry_wait_jobs{};
    std::size_t completed_jobs{};
    std::size_t permanent_failed_jobs{};
    std::size_t manual_intervention_jobs{};
};

[[nodiscard]] std::string_view upload_job_kind_name(UploadJobKind kind) noexcept;
[[nodiscard]] std::int32_t upload_job_priority(UploadJobKind kind) noexcept;
[[nodiscard]] std::string_view upload_job_state_name(UploadJobState state) noexcept;

/// SQLite metadata index. The event directory remains the immutable file source of truth.
class EventMetadataDatabase final
{
  public:
    class ConstructionKey final
    {
      public:
        ConstructionKey(const ConstructionKey&) = default;

      private:
        friend class EventMetadataDatabase;
        ConstructionKey() = default;
    };

    [[nodiscard]] static Result<std::unique_ptr<EventMetadataDatabase>> open(
        MetadataDatabaseOptions options);

    EventMetadataDatabase(ConstructionKey, std::unique_ptr<struct MetadataDatabaseImpl> impl);
    ~EventMetadataDatabase();
    EventMetadataDatabase(const EventMetadataDatabase&) = delete;
    EventMetadataDatabase& operator=(const EventMetadataDatabase&) = delete;
    EventMetadataDatabase(EventMetadataDatabase&&) = delete;
    EventMetadataDatabase& operator=(EventMetadataDatabase&&) = delete;

    [[nodiscard]] const MetadataDatabaseOpenReport& open_report() const noexcept;
    [[nodiscard]] Result<DatabaseIntegrityReport> integrity_check() const;

    /// Creates a consistent SQLite backup without replacing an existing destination.
    [[nodiscard]] Result<void> backup_to(const std::filesystem::path& destination) const;

    /// Restores a closed database from a verified backup. The backup is retained.
    [[nodiscard]] static Result<void> restore_backup(
        const std::filesystem::path& database_path, const std::filesystem::path& backup_path,
        std::chrono::milliseconds busy_timeout = std::chrono::milliseconds{250});

    /// Verifies and atomically indexes one already committed M5-06 event directory.
    [[nodiscard]] Result<void> index_committed_event(
        const std::filesystem::path& committed_directory);

    /// Creates the query-visible row as soon as the canonical aggregate id is known.
    [[nodiscard]] Result<EventMetadataRecord> create_collecting_event(
        const CollectingEventRecord& event);
    /// Updates one aggregate without changing its review fields.
    [[nodiscard]] Result<EventMetadataRecord> update_event_lifecycle(
        std::string_view event_id, std::string_view decision_state,
        std::string_view persistence_state, std::uint64_t trigger_count,
        std::optional<std::int64_t> confirmed_time_utc_ms = std::nullopt);

    [[nodiscard]] Result<EventQueryPage> query_events(const EventQuery& query) const;

    /// Returns one indexed event or EVENT_NOT_FOUND. The immutable manifest is not modified.
    [[nodiscard]] Result<EventMetadataRecord> get_event(std::string_view event_id) const;

    /// Applies an operator review with optimistic concurrency. Repeating the same terminal
    /// decision is idempotent; a stale conflicting decision returns EVENT_VERSION_CONFLICT.
    [[nodiscard]] Result<EventReviewOutcome> review_event(std::string_view event_id,
                                                          std::uint64_t expected_review_revision,
                                                          EventReviewDecision decision,
                                                          std::int64_t reviewed_at_utc_ms,
                                                          std::string_view reviewed_by);

    /// Updates operator-controlled retention flags. Upload state is managed separately by M8.
    [[nodiscard]] Result<void> set_retention_policy(std::string_view event_id, bool locked,
                                                    bool deletion_allowed,
                                                    std::int64_t updated_at_utc_ms);

    /// Returns oldest eligible events. Only Uploaded, Present, explicitly deletable and unlocked
    /// rows are returned. A cutoff, when present, is inclusive.
    [[nodiscard]] Result<std::vector<EventRetentionRecord>> retention_candidates(
        std::optional<std::int64_t> candidate_time_cutoff_utc_ms, std::size_t limit) const;

    /// Returns interrupted or failed deletion work in oldest-first order.
    [[nodiscard]] Result<std::vector<EventRetentionRecord>> deletion_work(std::size_t limit) const;

    /// Atomically claims an eligible event before any file operation.
    [[nodiscard]] Result<bool> begin_deletion(std::string_view event_id,
                                              const std::filesystem::path& deletion_relative_path,
                                              std::int64_t updated_at_utc_ms);
    [[nodiscard]] Result<void> complete_deletion(std::string_view event_id,
                                                 std::int64_t deleted_at_utc_ms);
    [[nodiscard]] Result<void> fail_deletion(std::string_view event_id, std::string_view reason,
                                             std::int64_t updated_at_utc_ms);

    /// Sum of indexed immutable event files for rows that have not completed deletion.
    [[nodiscard]] Result<std::uint64_t> retained_event_bytes() const;

    /// Atomically inserts one bounded persistent upload task. The idempotency key never creates or
    /// modifies an event. Repeating identical content returns the existing task as a duplicate.
    [[nodiscard]] Result<UploadJobEnqueueOutcome> enqueue_upload_job(
        const UploadJobRequest& request);

    /// Claims the highest-priority due task and transitions it to InProgress in one transaction.
    [[nodiscard]] Result<std::optional<UploadJobRecord>> claim_next_upload_job(
        std::int64_t now_utc_ms);

    [[nodiscard]] Result<void> complete_upload_job(std::int64_t job_id,
                                                   std::string_view checkpoint_json,
                                                   std::int64_t completed_at_utc_ms);
    [[nodiscard]] Result<void> fail_upload_job(std::int64_t job_id,
                                               UploadFailureClass failure_class,
                                               std::string_view error_code,
                                               std::string_view checkpoint_json,
                                               std::optional<std::int64_t> next_attempt_utc_ms,
                                               std::int64_t updated_at_utc_ms);

    /// Moves a terminal failed task back to Pending after an explicit operator action.
    [[nodiscard]] Result<void> retry_upload_job(std::int64_t job_id,
                                                std::int64_t updated_at_utc_ms);
    [[nodiscard]] Result<std::size_t> retry_event_upload_jobs(std::string_view event_id,
                                                              std::int64_t updated_at_utc_ms);

    /// Reclaims tasks left InProgress by a previous process without duplicating rows or events.
    [[nodiscard]] Result<std::size_t> recover_upload_jobs(std::int64_t now_utc_ms);
    [[nodiscard]] Result<std::optional<UploadJobRecord>> get_upload_job(
        std::string_view idempotency_key) const;
    [[nodiscard]] Result<UploadQueueStats> upload_queue_stats() const;

    /// Bounded reconciliation. Directory-only events are indexed; database-only rows are marked
    /// Missing. No event directory or database row is deleted.
    [[nodiscard]] Result<EventReconcileReport> reconcile();

  private:
    std::unique_ptr<struct MetadataDatabaseImpl> impl_;
};

} // namespace paperbreak::storage
