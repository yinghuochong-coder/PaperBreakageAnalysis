#pragma once

#include "paperbreak/camera/frame.hpp"
#include "paperbreak/common/result.hpp"
#include "paperbreak/common/threading.hpp"
#include "paperbreak/storage/nvme_index.hpp"
#include "paperbreak/storage/nvme_recovery.hpp"
#include "paperbreak/storage/storage_policy.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace paperbreak::storage
{

inline constexpr std::uint16_t nvme_format_version = 1U;
inline constexpr std::uint32_t nvme_page_bytes = 4096U;
inline constexpr std::uint32_t nvme_index_entry_bytes = 96U;
inline constexpr std::chrono::milliseconds nvme_block_duration{1000};
inline constexpr std::size_t nvme_default_queue_capacity_per_camera = 2U;

struct NvmeCameraLayout final
{
    std::string camera_id;
    std::uint32_t maximum_frame_bytes{};
    std::uint32_t index_capacity{};
    std::uint64_t required_input_bytes_per_second{};
};

struct NvmeRollingCacheOptions final
{
    std::filesystem::path root;
    std::uint64_t maximum_cache_bytes{};
    std::uint64_t write_limit_bytes_per_second{};
    std::chrono::milliseconds io_timeout{std::chrono::seconds{10}};
    std::chrono::milliseconds recovery_timeout{nvme_default_recovery_timeout};
    std::size_t recovery_maximum_files{nvme_default_recovery_maximum_files};
    std::size_t recovery_summary_bytes{nvme_default_recovery_summary_bytes};
    std::size_t queue_capacity_per_camera{nvme_default_queue_capacity_per_camera};
    std::vector<NvmeCameraLayout> cameras;
    std::function<void(const Error&)> error_observer;
    ThreadRegistrationFactory register_thread;
    DebugDiagnosticSink diagnostics;
};

struct NvmeBlock final
{
    NvmeBlockId block_id{};
    std::uint64_t generation{};
    std::string camera_id;
    camera::MonotonicTime start_monotonic_time;
    camera::WallClockTime start_wall_clock_time;
    std::uint32_t maximum_frame_bytes{};
    std::uint32_t index_capacity{};
    std::vector<camera::FrameView> frames;
};

struct NvmeWriteRequest final
{
    std::filesystem::path root;
    const NvmeBlock* block{};
    std::uint64_t write_limit_bytes_per_second{};
    std::chrono::steady_clock::time_point deadline;
};

struct NvmeCommittedBlock final
{
    std::filesystem::path path;
    std::uint64_t physical_bytes{};
    std::uint32_t header_crc32c{};
    std::uint32_t index_crc32c{};
    std::uint32_t data_crc32c{};
    std::uint32_t footer_crc32c{};
    bool commit_verified{};
};

class INvmeBlockStore
{
  public:
    virtual ~INvmeBlockStore() = default;
    [[nodiscard]] virtual Result<void> prepare(const std::filesystem::path& root) = 0;
    [[nodiscard]] virtual Result<NvmeCommittedBlock> write_block(const NvmeWriteRequest& request,
                                                                 std::stop_token stop_token) = 0;
    [[nodiscard]] virtual Result<void> remove_committed(const std::filesystem::path& path) = 0;
    virtual void cancel_pending() noexcept = 0;
};

[[nodiscard]] std::shared_ptr<INvmeBlockStore> make_windows_nvme_block_store();

enum class NvmeSubmitStatus
{
    accepted,
    block_enqueued,
    queue_full,
    watermark_blocked,
    memory_degraded,
    closed,
};

enum class NvmeCacheState
{
    stopped,
    running,
    watermark_blocked,
    memory_degraded,
    stopping,
};

[[nodiscard]] std::string_view to_string(NvmeCacheState state) noexcept;

struct NvmeRollingCacheSnapshot final
{
    NvmeCacheState state{NvmeCacheState::stopped};
    StorageWatermark storage_watermark{StorageWatermark::normal};
    std::size_t camera_count{};
    std::size_t queue_capacity{};
    std::size_t queue_depth{};
    std::size_t queue_high_watermark{};
    std::uint64_t submitted_frames{};
    std::uint64_t completed_blocks{};
    std::uint64_t enqueued_blocks{};
    std::uint64_t committed_blocks{};
    std::uint64_t rejected_blocks{};
    std::uint64_t sequence_gaps{};
    std::uint64_t bytes_committed{};
    double write_bytes_per_second{};
    std::uint64_t current_cache_bytes{};
    std::uint64_t blocks_reclaimed{};
    std::uint64_t bytes_reclaimed{};
    std::size_t recovery_scanned_files{};
    std::size_t recovery_accepted_blocks{};
    std::size_t recovery_repaired_blocks{};
    std::size_t recovery_quarantined_blocks{};
    std::size_t indexed_blocks{};
    std::size_t active_event_leases{};
    std::size_t protected_blocks{};
    std::uint64_t protected_bytes{};
    std::uint64_t lease_failures{};
    std::uint64_t write_failures{};
    std::uint64_t write_timeouts{};
    bool accepting{};
    bool event_writes_allowed{true};
    std::optional<Error> last_error;
};

/// One-thread, bounded NVMe rolling cache. submit_frame() never performs file I/O or waits.
class NvmeRollingCache final
{
  public:
    class ConstructionKey final
    {
      public:
        ConstructionKey(const ConstructionKey&) = default;

      private:
        friend class NvmeRollingCache;
        ConstructionKey() = default;
    };

    [[nodiscard]] static Result<std::shared_ptr<NvmeRollingCache>> create(
        NvmeRollingCacheOptions options,
        std::shared_ptr<INvmeBlockStore> store = make_windows_nvme_block_store(),
        std::shared_ptr<INvmeBlockIndex> index = make_sqlite_nvme_block_index(),
        std::shared_ptr<INvmeBlockRecovery> recovery = make_windows_nvme_block_recovery());

    NvmeRollingCache(ConstructionKey, std::unique_ptr<struct NvmeRollingCacheImpl> impl);
    ~NvmeRollingCache();
    NvmeRollingCache(const NvmeRollingCache&) = delete;
    NvmeRollingCache& operator=(const NvmeRollingCache&) = delete;

    [[nodiscard]] Result<void> start();
    [[nodiscard]] Result<NvmeSubmitStatus> submit_frame(camera::FrameView frame);
    [[nodiscard]] Result<NvmeEventLeaseOutcome> protect_event_window(NvmeEventLeaseRequest request);
    [[nodiscard]] Result<void> release_event(std::string_view event_id);
    [[nodiscard]] Result<NvmeFrameSequenceTrace> trace_window(
        const NvmeBlockWindowQuery& query) const;
    void set_storage_watermark(StorageWatermark watermark) noexcept;
    void request_stop() noexcept;
    [[nodiscard]] Result<void> join(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] NvmeRollingCacheSnapshot snapshot() const noexcept;

  private:
    std::unique_ptr<struct NvmeRollingCacheImpl> impl_;
};

[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes,
                                   std::uint32_t seed = 0U) noexcept;
[[nodiscard]] Result<std::uint64_t> maximum_nvme_block_bytes(
    std::uint32_t index_capacity, std::uint32_t maximum_frame_bytes) noexcept;

} // namespace paperbreak::storage
