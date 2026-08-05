#include "paperbreak/storage/nvme_index.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>

using namespace std::chrono_literals;

namespace
{

class TemporaryIndexDirectory final
{
  public:
    explicit TemporaryIndexDirectory(const std::string_view label)
    {
        static std::atomic_uint64_t sequence{};
        path_ = std::filesystem::temp_directory_path() /
                ("paperbreak-nvme-index-" + std::string{label} + "-" +
                 std::to_string(sequence.fetch_add(1U)));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryIndexDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

paperbreak::storage::NvmeBlockId block_id(const std::uint8_t value)
{
    paperbreak::storage::NvmeBlockId result{};
    result.front() = static_cast<std::byte>(value);
    return result;
}

paperbreak::storage::NvmeIndexedBlock indexed_block(
    const std::filesystem::path& root, const std::uint8_t id, const std::uint64_t generation,
    const std::chrono::milliseconds start, const std::chrono::milliseconds end,
    const std::uint64_t start_sequence, const std::uint64_t end_sequence,
    const std::uint64_t sequence_gaps = 0U, std::string camera_id = "CAM01")
{
    return {.block_id = block_id(id),
            .camera_id = std::move(camera_id),
            .generation = generation,
            .path = root / (std::to_string(id) + ".pbnvme"),
            .physical_bytes = 16384U + generation,
            .start_monotonic_time = paperbreak::camera::MonotonicTime{start},
            .end_monotonic_time = paperbreak::camera::MonotonicTime{end},
            .start_wall_clock_time = paperbreak::camera::WallClockTime{start},
            .end_wall_clock_time = paperbreak::camera::WallClockTime{end},
            .start_sequence_number = start_sequence,
            .end_sequence_number = end_sequence,
            .frame_count =
                static_cast<std::uint32_t>(end_sequence - start_sequence + 1U - sequence_gaps),
            .sequence_gaps = sequence_gaps,
            .header_crc32c = static_cast<std::uint32_t>(10U + id),
            .index_crc32c = static_cast<std::uint32_t>(20U + id),
            .data_crc32c = static_cast<std::uint32_t>(30U + id),
            .footer_crc32c = static_cast<std::uint32_t>(40U + id),
            .commit_verified = true};
}

paperbreak::storage::NvmeEventLeaseRequest lease_request(const std::string& event_id)
{
    return {.event_id = event_id,
            .camera_ids = {"CAM01"},
            .start_monotonic_time = paperbreak::camera::MonotonicTime{1s},
            .end_monotonic_time = paperbreak::camera::MonotonicTime{3s},
            .start_wall_clock_time = paperbreak::camera::WallClockTime{1s},
            .end_wall_clock_time = paperbreak::camera::WallClockTime{3s}};
}

} // namespace

TEST(StorageNvmeIndex, PersistsLeaseAndTracesCrossBlockSequenceGap)
{
    TemporaryIndexDirectory temporary{"persist"};
    auto index = paperbreak::storage::make_sqlite_nvme_block_index();
    ASSERT_TRUE(index->prepare(temporary.path(), 8U, 4U));
    ASSERT_TRUE(index->register_block(indexed_block(temporary.path(), 1U, 1U, 1s, 1900ms, 1U, 3U)));
    ASSERT_TRUE(index->register_block(indexed_block(temporary.path(), 2U, 2U, 2s, 2900ms, 5U, 6U)));
    ASSERT_TRUE(index->register_block(
        indexed_block(temporary.path(), 7U, 1U, 1200ms, 2800ms, 100U, 102U, 0U, "CAM02")));

    auto protected_window = index->protect_event_window(lease_request("EVT-1"));
    ASSERT_TRUE(protected_window) << protected_window.error().message;
    EXPECT_EQ(protected_window.value().protected_blocks, 2U);
    auto trace =
        index->trace_window({.camera_id = "CAM01",
                             .start_wall_clock_time = paperbreak::camera::WallClockTime{1s},
                             .end_wall_clock_time = paperbreak::camera::WallClockTime{2500ms},
                             .maximum_blocks = 8U});
    ASSERT_TRUE(trace) << trace.error().message;
    ASSERT_EQ(trace.value().blocks.size(), 2U);
    EXPECT_EQ(trace.value().sequence_gaps, 1U);
    EXPECT_EQ(trace.value().sequence_overlaps, 0U);
    EXPECT_FALSE(trace.value().sequence_contiguous);
    EXPECT_TRUE(trace.value().time_range_covered);
    EXPECT_TRUE(trace.value().all_commits_verified);
    EXPECT_EQ(trace.value().blocks.front().lease_count, 1U);

    index.reset();
    auto reopened = paperbreak::storage::make_sqlite_nvme_block_index();
    ASSERT_TRUE(reopened->prepare(temporary.path(), 8U, 4U));
    auto snapshot = reopened->snapshot();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().block_count, 3U);
    EXPECT_EQ(snapshot.value().active_leases, 1U);
    EXPECT_EQ(snapshot.value().protected_blocks, 2U);
    EXPECT_TRUE(reopened->release_event("EVT-1"));
    auto oldest = reopened->oldest_reclaimable();
    ASSERT_TRUE(oldest);
    ASSERT_TRUE(oldest.value());
    EXPECT_EQ(oldest.value()->generation, 1U);
}

TEST(StorageNvmeIndex, ProtectsFutureOverlappingBlockAndEnforcesLeaseCapacity)
{
    TemporaryIndexDirectory temporary{"future"};
    auto index = paperbreak::storage::make_sqlite_nvme_block_index();
    ASSERT_TRUE(index->prepare(temporary.path(), 4U, 1U));
    auto protected_window = index->protect_event_window(lease_request("EVT-FUTURE"));
    ASSERT_TRUE(protected_window);
    EXPECT_EQ(protected_window.value().protected_blocks, 0U);
    ASSERT_TRUE(
        index->register_block(indexed_block(temporary.path(), 3U, 1U, 1500ms, 2500ms, 10U, 12U)));
    auto snapshot = index->snapshot();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().protected_blocks, 1U);
    auto second = index->protect_event_window(lease_request("EVT-OVERFLOW"));
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().business_code, "NVME_LEASE_CAPACITY");
    auto oldest = index->oldest_reclaimable();
    ASSERT_TRUE(oldest);
    EXPECT_FALSE(oldest.value());
}

TEST(StorageNvmeIndex, RebuildsDerivedBlocksAndReattachesPersistedLease)
{
    TemporaryIndexDirectory temporary{"rebuild"};
    auto index = paperbreak::storage::make_sqlite_nvme_block_index();
    ASSERT_TRUE(index->prepare(temporary.path(), 8U, 4U));
    ASSERT_TRUE(index->register_block(indexed_block(temporary.path(), 1U, 1U, 1s, 1900ms, 1U, 3U)));
    ASSERT_TRUE(index->protect_event_window(lease_request("EVT-REBUILD")));
    const auto replacement = indexed_block(temporary.path(), 4U, 4U, 2s, 2800ms, 20U, 22U);
    ASSERT_TRUE(index->rebuild(std::span{&replacement, 1U}));
    auto snapshot = index->snapshot();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().block_count, 1U);
    EXPECT_EQ(snapshot.value().protected_blocks, 1U);
    auto trace =
        index->trace_window({.camera_id = "CAM01",
                             .start_wall_clock_time = paperbreak::camera::WallClockTime{2s},
                             .end_wall_clock_time = paperbreak::camera::WallClockTime{2500ms}});
    ASSERT_TRUE(trace);
    ASSERT_EQ(trace.value().blocks.size(), 1U);
    EXPECT_EQ(trace.value().blocks.front().block_id, replacement.block_id);
}

TEST(StorageNvmeIndex, RejectsPartialTraceBeyondCallerBound)
{
    TemporaryIndexDirectory temporary{"query-limit"};
    auto index = paperbreak::storage::make_sqlite_nvme_block_index();
    ASSERT_TRUE(index->prepare(temporary.path(), 4U, 2U));
    ASSERT_TRUE(index->register_block(indexed_block(temporary.path(), 1U, 1U, 1s, 1500ms, 1U, 2U)));
    ASSERT_TRUE(index->register_block(indexed_block(temporary.path(), 2U, 2U, 1600ms, 2s, 3U, 4U)));
    auto trace =
        index->trace_window({.camera_id = "CAM01",
                             .start_wall_clock_time = paperbreak::camera::WallClockTime{1s},
                             .end_wall_clock_time = paperbreak::camera::WallClockTime{2s},
                             .maximum_blocks = 1U});
    ASSERT_FALSE(trace);
    EXPECT_EQ(trace.error().business_code, "NVME_INDEX_QUERY_LIMIT");
}
