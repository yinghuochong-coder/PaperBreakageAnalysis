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

inline constexpr std::uint32_t database_schema_version = 2U;
inline constexpr std::size_t database_default_page_size = 50U;
inline constexpr std::size_t database_maximum_page_size = 200U;

struct MetadataDatabaseOptions final
{
    std::filesystem::path database_path;
    std::filesystem::path event_root;
    std::filesystem::path backup_directory;
    std::chrono::milliseconds busy_timeout{250};
    std::size_t maximum_reconcile_events{10000U};
    std::size_t maximum_manifest_bytes{8U * 1024U * 1024U};
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
    std::optional<std::int64_t> start_time_utc_ms;
    std::optional<std::int64_t> end_time_utc_ms;
    std::optional<std::string> event_state;
    std::optional<std::string> camera_id;
    std::size_t offset{};
    std::size_t limit{database_default_page_size};
};

struct EventQueryPage final
{
    std::vector<EventMetadataRecord> events;
    std::size_t total{};
    std::size_t offset{};
    std::size_t limit{};
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

    [[nodiscard]] Result<EventQueryPage> query_events(const EventQuery& query) const;

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

    /// Bounded reconciliation. Directory-only events are indexed; database-only rows are marked
    /// Missing. No event directory or database row is deleted.
    [[nodiscard]] Result<EventReconcileReport> reconcile();

  private:
    std::unique_ptr<struct MetadataDatabaseImpl> impl_;
};

} // namespace paperbreak::storage
