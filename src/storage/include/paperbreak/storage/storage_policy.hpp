#pragma once

#include "paperbreak/common/result.hpp"
#include "paperbreak/storage/metadata_database.hpp"

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

enum class StorageWatermark
{
    normal,
    warning,
    critical,
    stop_save,
};

[[nodiscard]] std::string_view to_string(StorageWatermark watermark) noexcept;

struct StorageSpace final
{
    std::uint64_t capacity_bytes{};
    std::uint64_t available_bytes{};
};

struct StorageWatermarkThresholds final
{
    std::uint64_t warning_available_bytes{};
    std::uint64_t critical_available_bytes{};
    std::uint64_t stop_save_available_bytes{};
};

struct StoragePolicyOptions final
{
    std::filesystem::path event_root;
    std::vector<std::filesystem::path> temporary_roots;
    StorageWatermarkThresholds watermarks;
    std::optional<std::chrono::days> retention_age;
    std::optional<std::uint64_t> maximum_event_bytes;
    std::chrono::hours temporary_maximum_age{24};
    std::size_t maximum_deletions_per_run{32U};
    std::size_t maximum_temporary_entries_per_run{1024U};
};

struct StoragePolicySnapshot final
{
    StorageWatermark watermark{StorageWatermark::normal};
    std::uint64_t capacity_bytes{};
    std::uint64_t available_bytes{};
    std::uint64_t retained_event_bytes{};
    bool ordinary_rolling_writes_allowed{true};
    bool large_writes_allowed{true};
    std::uint64_t maintenance_runs{};
    std::uint64_t events_deleted{};
    std::uint64_t event_delete_failures{};
    std::uint64_t temporary_entries_deleted{};
    std::uint64_t temporary_delete_failures{};
};

struct StorageMaintenanceFailure final
{
    std::string event_id;
    std::filesystem::path path;
    Error error;
};

struct StorageMaintenanceReport final
{
    StoragePolicySnapshot snapshot;
    std::size_t deletion_work_recovered{};
    std::size_t events_deleted{};
    std::size_t temporary_entries_deleted{};
    std::vector<StorageMaintenanceFailure> failures;
};

class IStoragePolicyFileSystem
{
  public:
    virtual ~IStoragePolicyFileSystem() = default;
    [[nodiscard]] virtual Result<StorageSpace> space(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual Result<bool> is_directory(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual Result<void> create_directories(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual Result<void> move_directory_atomically(
        const std::filesystem::path& source, const std::filesystem::path& destination) = 0;
    [[nodiscard]] virtual Result<void> remove_tree(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual Result<std::vector<std::filesystem::path>> expired_temporary_entries(
        const std::filesystem::path& root, std::chrono::system_clock::time_point cutoff,
        std::size_t maximum_entries) = 0;
};

[[nodiscard]] std::shared_ptr<IStoragePolicyFileSystem> make_storage_policy_file_system();

/// Synchronous bounded maintenance coordinator. Invoke only from a storage worker, never a camera
/// acquisition callback.
class StoragePolicyManager final
{
  public:
    class ConstructionKey final
    {
      public:
        ConstructionKey(const ConstructionKey&) = default;

      private:
        friend class StoragePolicyManager;
        ConstructionKey() = default;
    };

    [[nodiscard]] static Result<std::unique_ptr<StoragePolicyManager>> create(
        StoragePolicyOptions options, EventMetadataDatabase& database,
        std::shared_ptr<IStoragePolicyFileSystem> file_system = make_storage_policy_file_system());

    StoragePolicyManager(ConstructionKey, StoragePolicyOptions options,
                         EventMetadataDatabase& database,
                         std::shared_ptr<IStoragePolicyFileSystem> file_system);
    ~StoragePolicyManager();
    StoragePolicyManager(const StoragePolicyManager&) = delete;
    StoragePolicyManager& operator=(const StoragePolicyManager&) = delete;
    StoragePolicyManager(StoragePolicyManager&&) = delete;
    StoragePolicyManager& operator=(StoragePolicyManager&&) = delete;

    [[nodiscard]] Result<StorageMaintenanceReport> run_maintenance(
        std::chrono::system_clock::time_point now);
    [[nodiscard]] Result<void> admit_large_write() const;
    [[nodiscard]] StoragePolicySnapshot snapshot() const noexcept;

    [[nodiscard]] static StorageWatermark classify(
        std::uint64_t available_bytes, const StorageWatermarkThresholds& thresholds) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace paperbreak::storage
