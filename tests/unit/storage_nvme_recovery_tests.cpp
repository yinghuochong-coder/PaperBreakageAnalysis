#include "paperbreak/camera/frame.hpp"
#include "paperbreak/storage/nvme_cache.hpp"
#include "paperbreak/storage/nvme_recovery.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <ranges>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace
{

class TemporaryDirectory final
{
  public:
    explicit TemporaryDirectory(const std::string_view label)
    {
        static std::atomic_uint64_t sequence{};
        path_ = std::filesystem::temp_directory_path() /
                ("paperbreak-nvme-recovery-" + std::string{label} + "-" +
                 std::to_string(sequence.fetch_add(1U)));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory()
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

paperbreak::camera::FrameView frame(const std::uint64_t sequence,
                                    const paperbreak::camera::MonotonicTime monotonic,
                                    const std::uint8_t value = 0x2AU)
{
    auto buffer = std::make_shared<paperbreak::camera::FrameBuffer>(4U);
    std::ranges::fill(buffer->writable_bytes(), static_cast<std::byte>(value));
    EXPECT_TRUE(buffer->set_size(4U));
    paperbreak::camera::FramePacket packet{.camera_id = "CAM01",
                                           .camera_frame_number = sequence + 100U,
                                           .sequence_number = sequence,
                                           .received_monotonic_time = monotonic,
                                           .received_wall_clock_time =
                                               paperbreak::camera::WallClockTime{1s} +
                                               std::chrono::milliseconds{sequence},
                                           .geometry = {.width = 2U, .height = 2U, .stride = 2U},
                                           .pixel_format = paperbreak::camera::PixelFormat::mono8,
                                           .buffer = std::move(buffer)};
    auto view = paperbreak::camera::make_frame_view(packet);
    EXPECT_TRUE(view);
    return std::move(view).value();
}

paperbreak::storage::NvmeBlock block(const std::uint64_t generation = 7U)
{
    const auto start = paperbreak::camera::MonotonicTime{1s};
    return {.block_id = {std::byte{0x71U}, std::byte{0x04U}},
            .generation = generation,
            .camera_id = "CAM01",
            .start_monotonic_time = start,
            .start_wall_clock_time = paperbreak::camera::WallClockTime{1001ms},
            .maximum_frame_bytes = 4U,
            .index_capacity = 3U,
            .frames = {frame(1U, start), frame(2U, start + 10ms, 0x2BU)}};
}

std::filesystem::path write_committed(const std::filesystem::path& root,
                                      const std::uint64_t generation = 7U)
{
    auto store = paperbreak::storage::make_windows_nvme_block_store();
    EXPECT_TRUE(store->prepare(root));
    auto value = block(generation);
    auto written = store->write_block({.root = root,
                                       .block = &value,
                                       .write_limit_bytes_per_second = 100U * 1024U * 1024U,
                                       .deadline = std::chrono::steady_clock::now() + 2s},
                                      {});
    EXPECT_TRUE(written) << written.error().message;
    return written.value().path;
}

paperbreak::storage::NvmeRecoveryLimits limits()
{
    return {.maximum_files = 64U,
            .maximum_summary_bytes = 1024U * 1024U,
            .deadline = std::chrono::steady_clock::now() + 2s};
}

void write_zeroes(const std::filesystem::path& path, const std::uint64_t offset,
                  const std::size_t count)
{
    std::fstream output{path, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(output);
    std::vector<char> zeroes(count, '\0');
    output.seekp(static_cast<std::streamoff>(offset));
    output.write(zeroes.data(), static_cast<std::streamsize>(zeroes.size()));
    ASSERT_TRUE(output);
}

void flip_byte(const std::filesystem::path& path, const std::uint64_t offset)
{
    std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(file);
    file.seekg(static_cast<std::streamoff>(offset));
    char value{};
    file.read(&value, 1);
    ASSERT_TRUE(file);
    value = static_cast<char>(static_cast<unsigned char>(value) ^ 0x5AU);
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(&value, 1);
    ASSERT_TRUE(file);
}

bool wait_for(const std::function<bool()>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::yield();
    }
    return predicate();
}

paperbreak::storage::NvmeRollingCacheOptions cache_options(const std::filesystem::path& root)
{
    return {.root = root,
            .maximum_cache_bytes = 16384U,
            .write_limit_bytes_per_second = 1024U * 1024U,
            .io_timeout = 1s,
            .recovery_timeout = 2s,
            .recovery_maximum_files = 16U,
            .recovery_summary_bytes = 1024U * 1024U,
            .cameras = {{.camera_id = "CAM01",
                         .maximum_frame_bytes = 4U,
                         .index_capacity = 3U,
                         .required_input_bytes_per_second = 12U}}};
}

class FailingRecovery final : public paperbreak::storage::INvmeBlockRecovery
{
  public:
    paperbreak::Result<paperbreak::storage::NvmeRecoveryReport> recover(
        const std::filesystem::path&, const paperbreak::storage::NvmeRecoveryLimits&) override
    {
        ++calls;
        return paperbreak::Result<paperbreak::storage::NvmeRecoveryReport>::failure(
            paperbreak::make_error("NVME_RECOVERY_FAILED", paperbreak::Severity::error,
                                   "injected disk removal", "storage", "storage.nvme.recovery.read",
                                   true));
    }

    std::atomic_uint64_t calls{};
};

} // namespace

TEST(StorageNvmeRecovery, AcceptsCommittedBlockAndRepairsPayloadCompletePartial)
{
    TemporaryDirectory temporary{"repair"};
    auto path = write_committed(temporary.path());
    auto partial = path;
    partial.replace_extension(L".partial");
    std::filesystem::rename(path, partial);
    write_zeroes(partial, 12288U, paperbreak::storage::nvme_page_bytes);

    auto recovery = paperbreak::storage::make_windows_nvme_block_recovery();
    auto recovered = recovery->recover(temporary.path(), limits());
    ASSERT_TRUE(recovered) << recovered.error().message;
    ASSERT_EQ(recovered.value().blocks.size(), 1U);
    EXPECT_EQ(recovered.value().accepted_blocks, 1U);
    EXPECT_EQ(recovered.value().repaired_blocks, 1U);
    EXPECT_EQ(recovered.value().quarantined_blocks, 0U);
    EXPECT_EQ(recovered.value().blocks.front().generation, 7U);
    EXPECT_EQ(recovered.value().blocks.front().frame_count, 2U);
    EXPECT_EQ(recovered.value().blocks.front().start_sequence_number, 1U);
    EXPECT_EQ(recovered.value().blocks.front().end_sequence_number, 2U);
    EXPECT_TRUE(std::filesystem::exists(recovered.value().blocks.front().path));
    EXPECT_FALSE(std::filesystem::exists(partial));

    auto rescanned = recovery->recover(temporary.path(), limits());
    ASSERT_TRUE(rescanned);
    EXPECT_EQ(rescanned.value().accepted_blocks, 1U);
    EXPECT_EQ(rescanned.value().repaired_blocks, 0U);
    EXPECT_TRUE(rescanned.value().blocks.front().commit_verified);
}

TEST(StorageNvmeRecovery, HandlesInterruptedWriteStagesWithoutInventingFrames)
{
    TemporaryDirectory source{"stages-source"};
    TemporaryDirectory target{"stages-target"};
    const auto committed = write_committed(source.path());
    const auto preallocated = target.path() / "preallocated.partial";
    {
        std::ofstream output{preallocated, std::ios::binary};
        output.seekp(16383);
        output.put('\0');
    }
    const auto header_only = target.path() / "header-only.partial";
    std::filesystem::copy_file(committed, header_only);
    write_zeroes(header_only, 4096U, 12288U);
    const auto index_only = target.path() / "index-only.partial";
    std::filesystem::copy_file(committed, index_only);
    write_zeroes(index_only, 8192U, 8192U);
    const auto footer_without_marker = target.path() / "footer-without-marker.partial";
    std::filesystem::copy_file(committed, footer_without_marker);
    write_zeroes(footer_without_marker, 16376U, 8U);
    const auto committed_not_published = target.path() / "committed-not-published.partial";
    std::filesystem::copy_file(committed, committed_not_published);

    auto recovery = paperbreak::storage::make_windows_nvme_block_recovery();
    auto recovered = recovery->recover(target.path(), limits());
    ASSERT_TRUE(recovered) << recovered.error().message;
    EXPECT_EQ(recovered.value().scanned_files, 5U);
    EXPECT_EQ(recovered.value().accepted_blocks, 2U);
    EXPECT_EQ(recovered.value().repaired_blocks, 2U);
    EXPECT_EQ(recovered.value().quarantined_blocks, 3U);
    EXPECT_EQ(recovered.value().blocks.front().frame_count, 2U);
    EXPECT_TRUE(std::filesystem::exists(target.path() / ".quarantine"));
}

TEST(StorageNvmeRecovery, QuarantinesEveryCommittedIntegrityFailureAndUnknownVersion)
{
    TemporaryDirectory source{"crc-source"};
    TemporaryDirectory target{"crc-target"};
    const auto committed = write_committed(source.path());
    const std::array<std::pair<std::wstring_view, std::uint64_t>, 6U> corruptions{
        {{L"header", 200U},
         {L"version", 8U},
         {L"index", 4101U},
         {L"payload", 8192U},
         {L"footer", 12308U},
         {L"marker", 16380U}}};
    for (const auto& [name, offset] : corruptions)
    {
        const auto destination = target.path() / (std::wstring{name} + L".pbnvme");
        std::filesystem::copy_file(committed, destination);
        flip_byte(destination, offset);
    }

    auto recovery = paperbreak::storage::make_windows_nvme_block_recovery();
    auto recovered = recovery->recover(target.path(), limits());
    ASSERT_TRUE(recovered) << recovered.error().message;
    EXPECT_TRUE(recovered.value().blocks.empty());
    EXPECT_EQ(recovered.value().scanned_files, corruptions.size());
    EXPECT_EQ(recovered.value().quarantined_blocks, corruptions.size());
}

TEST(StorageNvmeRecovery, EnforcesFileMemoryAndTimeBounds)
{
    TemporaryDirectory source{"limits-source"};
    TemporaryDirectory target{"limits-target"};
    const auto committed = write_committed(source.path());
    std::filesystem::copy_file(committed, target.path() / "one.pbnvme");
    std::filesystem::copy_file(committed, target.path() / "two.pbnvme");
    auto recovery = paperbreak::storage::make_windows_nvme_block_recovery();

    auto file_limited = limits();
    file_limited.maximum_files = 1U;
    auto too_many = recovery->recover(target.path(), file_limited);
    ASSERT_FALSE(too_many);
    EXPECT_EQ(too_many.error().business_code, "NVME_RECOVERY_LIMIT");

    TemporaryDirectory memory_target{"memory-limit"};
    std::filesystem::copy_file(committed, memory_target.path() / "one.pbnvme");
    auto memory_limited = limits();
    memory_limited.maximum_summary_bytes = 1U;
    auto too_large = recovery->recover(memory_target.path(), memory_limited);
    ASSERT_FALSE(too_large);
    EXPECT_EQ(too_large.error().business_code, "NVME_RECOVERY_LIMIT");

    auto time_limited = limits();
    time_limited.deadline = std::chrono::steady_clock::now();
    auto timed_out = recovery->recover(memory_target.path(), time_limited);
    ASSERT_FALSE(timed_out);
    EXPECT_EQ(timed_out.error().business_code, "NVME_RECOVERY_LIMIT");
}

TEST(StorageNvmeRecovery, RebuildsMissingIndexAndContinuesGenerationAcrossCapacityWrap)
{
    TemporaryDirectory temporary{"restart"};
    static_cast<void>(write_committed(temporary.path(), 7U));
    std::error_code ignored;
    std::filesystem::remove_all(temporary.path() / ".index", ignored);
    auto cache = paperbreak::storage::NvmeRollingCache::create(cache_options(temporary.path()));
    ASSERT_TRUE(cache);
    ASSERT_TRUE(cache.value()->start());
    auto startup = cache.value()->snapshot();
    EXPECT_EQ(startup.recovery_accepted_blocks, 1U);
    EXPECT_EQ(startup.indexed_blocks, 1U);
    EXPECT_EQ(startup.current_cache_bytes, 16384U);
    auto protected_window = cache.value()->protect_event_window(
        {.event_id = "EVT-NEW-AFTER-RECOVERY",
         .camera_ids = {"CAM01"},
         .start_monotonic_time = paperbreak::camera::MonotonicTime{0s},
         .end_monotonic_time = paperbreak::camera::MonotonicTime{2s},
         .start_wall_clock_time = paperbreak::camera::WallClockTime{1s},
         .end_wall_clock_time = paperbreak::camera::WallClockTime{2s}});
    ASSERT_TRUE(protected_window);
    EXPECT_EQ(protected_window.value().protected_blocks, 1U);
    ASSERT_TRUE(cache.value()->release_event("EVT-NEW-AFTER-RECOVERY"));

    const auto start = paperbreak::camera::MonotonicTime{3s};
    for (std::uint64_t index = 0U; index < 3U; ++index)
        ASSERT_TRUE(cache.value()->submit_frame(frame(10U + index, start + index * 10ms)));
    ASSERT_TRUE(wait_for([&] { return cache.value()->snapshot().committed_blocks == 1U; }));
    const auto wrapped = cache.value()->snapshot();
    EXPECT_EQ(wrapped.blocks_reclaimed, 1U);
    EXPECT_EQ(wrapped.current_cache_bytes, 16384U);
    bool generation_continued{};
    for (const auto& entry : std::filesystem::directory_iterator{temporary.path()})
    {
        if (entry.path().extension() == L".pbnvme" &&
            entry.path().filename().wstring().find(L"CAM01-8-") != std::wstring::npos)
            generation_continued = true;
    }
    EXPECT_TRUE(generation_continued);
    cache.value()->request_stop();
    EXPECT_TRUE(cache.value()->join(std::chrono::steady_clock::now() + 2s));
}

TEST(StorageNvmeRecovery, DiskRemovalDuringStartupDegradesWithoutBlockingEventMemory)
{
    TemporaryDirectory temporary{"disk-removal"};
    auto failing = std::make_shared<FailingRecovery>();
    auto cache = paperbreak::storage::NvmeRollingCache::create(
        cache_options(temporary.path()), paperbreak::storage::make_windows_nvme_block_store(),
        paperbreak::storage::make_sqlite_nvme_block_index(), failing);
    ASSERT_TRUE(cache);
    EXPECT_TRUE(cache.value()->start());
    const auto snapshot = cache.value()->snapshot();
    EXPECT_EQ(snapshot.state, paperbreak::storage::NvmeCacheState::memory_degraded);
    EXPECT_TRUE(snapshot.event_writes_allowed);
    ASSERT_TRUE(snapshot.last_error);
    EXPECT_EQ(snapshot.last_error->business_code, "NVME_RECOVERY_FAILED");
    EXPECT_EQ(failing->calls.load(), 1U);
}

TEST(StorageNvmeRecovery, ReattachesPersistedLeaseBeforeProtectedCapacityCompetition)
{
    TemporaryDirectory temporary{"lease"};
    static_cast<void>(write_committed(temporary.path(), 7U));
    auto recovery = paperbreak::storage::make_windows_nvme_block_recovery();
    auto scanned = recovery->recover(temporary.path(), limits());
    ASSERT_TRUE(scanned);
    auto index = paperbreak::storage::make_sqlite_nvme_block_index();
    ASSERT_TRUE(index->prepare(temporary.path(), 1U, 4U));
    ASSERT_TRUE(index->rebuild(scanned.value().blocks));
    ASSERT_TRUE(index->protect_event_window(
        {.event_id = "EVT-RECOVERY",
         .camera_ids = {"CAM01"},
         .start_monotonic_time = paperbreak::camera::MonotonicTime{0s},
         .end_monotonic_time = paperbreak::camera::MonotonicTime{3s},
         .start_wall_clock_time = paperbreak::camera::WallClockTime{1s},
         .end_wall_clock_time = paperbreak::camera::WallClockTime{2s}}));
    index.reset();

    auto cache = paperbreak::storage::NvmeRollingCache::create(cache_options(temporary.path()));
    ASSERT_TRUE(cache);
    ASSERT_TRUE(cache.value()->start());
    EXPECT_EQ(cache.value()->snapshot().protected_blocks, 1U);
    const auto start = paperbreak::camera::MonotonicTime{3s};
    for (std::uint64_t frame_index = 0U; frame_index < 3U; ++frame_index)
        ASSERT_TRUE(
            cache.value()->submit_frame(frame(20U + frame_index, start + frame_index * 10ms)));
    ASSERT_TRUE(wait_for([&] {
        return cache.value()->snapshot().state ==
               paperbreak::storage::NvmeCacheState::memory_degraded;
    }));
    const auto snapshot = cache.value()->snapshot();
    ASSERT_TRUE(snapshot.last_error);
    EXPECT_EQ(snapshot.last_error->business_code, "NVME_CACHE_PROTECTED");
    EXPECT_EQ(snapshot.blocks_reclaimed, 0U);
    EXPECT_EQ(snapshot.protected_blocks, 1U);
}
