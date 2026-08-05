#pragma once

#include "paperbreak/camera/frame.hpp"
#include "paperbreak/common/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace paperbreak::storage
{

inline constexpr std::uint32_t nvme_index_schema_version = 1U;
inline constexpr std::size_t nvme_default_maximum_event_leases = 64U;
inline constexpr std::size_t nvme_default_maximum_query_blocks = 4096U;

using NvmeBlockId = std::array<std::byte, 16U>;

struct NvmeIndexedBlock final
{
    NvmeBlockId block_id{};
    std::string camera_id;
    std::uint64_t generation{};
    std::filesystem::path path;
    std::uint64_t physical_bytes{};
    std::optional<camera::MonotonicTime> start_monotonic_time;
    std::optional<camera::MonotonicTime> end_monotonic_time;
    camera::WallClockTime start_wall_clock_time;
    camera::WallClockTime end_wall_clock_time;
    std::uint64_t start_sequence_number{};
    std::uint64_t end_sequence_number{};
    std::uint32_t frame_count{};
    std::uint64_t sequence_gaps{};
    std::uint32_t header_crc32c{};
    std::uint32_t index_crc32c{};
    std::uint32_t data_crc32c{};
    std::uint32_t footer_crc32c{};
    bool commit_verified{};
    std::size_t lease_count{};
};

struct NvmeEventLeaseRequest final
{
    std::string event_id;
    std::vector<std::string> camera_ids;
    camera::MonotonicTime start_monotonic_time;
    camera::MonotonicTime end_monotonic_time;
    camera::WallClockTime start_wall_clock_time;
    camera::WallClockTime end_wall_clock_time;
};

struct NvmeEventLeaseOutcome final
{
    std::string event_id;
    std::size_t protected_blocks{};
    std::uint64_t protected_bytes{};
    bool updated_existing{};
};

struct NvmeBlockWindowQuery final
{
    std::string camera_id;
    camera::WallClockTime start_wall_clock_time;
    camera::WallClockTime end_wall_clock_time;
    std::size_t maximum_blocks{nvme_default_maximum_query_blocks};
};

struct NvmeFrameSequenceTrace final
{
    std::string camera_id;
    camera::WallClockTime requested_start;
    camera::WallClockTime requested_end;
    std::vector<NvmeIndexedBlock> blocks;
    std::uint64_t sequence_gaps{};
    std::uint64_t sequence_overlaps{};
    bool sequence_contiguous{};
    bool time_range_covered{};
    bool all_commits_verified{};
};

struct NvmeBlockIndexSnapshot final
{
    std::size_t block_count{};
    std::size_t maximum_blocks{};
    std::size_t active_leases{};
    std::size_t maximum_leases{};
    std::size_t protected_blocks{};
    std::uint64_t protected_bytes{};
};

/// A persistent, derived index. Block files remain the recovery source of truth.
class INvmeBlockIndex
{
  public:
    virtual ~INvmeBlockIndex() = default;

    [[nodiscard]] virtual Result<void> prepare(const std::filesystem::path& cache_root,
                                               std::size_t maximum_blocks,
                                               std::size_t maximum_leases) = 0;
    [[nodiscard]] virtual Result<void> register_block(NvmeIndexedBlock block) = 0;
    [[nodiscard]] virtual Result<NvmeEventLeaseOutcome> protect_event_window(
        NvmeEventLeaseRequest request) = 0;
    [[nodiscard]] virtual Result<void> release_event(std::string_view event_id) = 0;
    [[nodiscard]] virtual Result<NvmeFrameSequenceTrace> trace_window(
        const NvmeBlockWindowQuery& query) const = 0;
    [[nodiscard]] virtual Result<std::optional<NvmeIndexedBlock>> oldest_reclaimable() const = 0;
    [[nodiscard]] virtual Result<void> erase_block(const NvmeBlockId& block_id) = 0;
    /// Replaces the derived block table with caller-validated records. Scanning belongs to M7-04.
    [[nodiscard]] virtual Result<void> rebuild(std::span<const NvmeIndexedBlock> blocks) = 0;
    [[nodiscard]] virtual Result<NvmeBlockIndexSnapshot> snapshot() const = 0;
};

[[nodiscard]] std::shared_ptr<INvmeBlockIndex> make_sqlite_nvme_block_index();

} // namespace paperbreak::storage
