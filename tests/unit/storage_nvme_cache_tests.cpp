#include "paperbreak/camera/frame.hpp"
#include "paperbreak/storage/nvme_cache.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

namespace
{

class TemporaryDirectory final
{
  public:
    explicit TemporaryDirectory(const std::string_view label)
    {
        static std::atomic_uint64_t sequence{};
        path_ =
            std::filesystem::temp_directory_path() / ("paperbreak-nvme-" + std::string{label} +
                                                      "-" + std::to_string(sequence.fetch_add(1U)));
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
    for (auto& byte : buffer->writable_bytes())
        byte = static_cast<std::byte>(value);
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

paperbreak::storage::NvmeRollingCacheOptions options(const std::filesystem::path& root)
{
    return {.root = root,
            .maximum_cache_bytes = 32768U,
            .write_limit_bytes_per_second = 1024U * 1024U,
            .io_timeout = 1s,
            .cameras = {{.camera_id = "CAM01",
                         .maximum_frame_bytes = 4U,
                         .index_capacity = 3U,
                         .required_input_bytes_per_second = 12U}}};
}

class ControlledBlockStore final : public paperbreak::storage::INvmeBlockStore
{
  public:
    paperbreak::Result<void> prepare(const std::filesystem::path&) override
    {
        if (prepare_error)
            return paperbreak::Result<void>::failure(*prepare_error);
        return paperbreak::Result<void>::success();
    }

    paperbreak::Result<paperbreak::storage::NvmeCommittedBlock> write_block(
        const paperbreak::storage::NvmeWriteRequest& request,
        const std::stop_token stop_token) override
    {
        {
            std::unique_lock lock{mutex};
            ++write_calls;
            entered.notify_all();
            condition.wait(lock, stop_token, [&] { return !block_writes; });
        }
        if (write_error)
            return paperbreak::Result<paperbreak::storage::NvmeCommittedBlock>::failure(
                *write_error);
        auto bytes = paperbreak::storage::maximum_nvme_block_bytes(
            request.block->index_capacity, request.block->maximum_frame_bytes);
        if (!bytes)
            return paperbreak::Result<paperbreak::storage::NvmeCommittedBlock>::failure(
                bytes.error());
        return paperbreak::Result<paperbreak::storage::NvmeCommittedBlock>::success(
            {.path = request.root / (std::to_string(write_calls.load()) + ".pbnvme"),
             .physical_bytes = bytes.value(),
             .header_crc32c = 1U,
             .index_crc32c = 2U,
             .data_crc32c = 3U,
             .footer_crc32c = 4U,
             .commit_verified = true});
    }

    paperbreak::Result<void> remove_committed(const std::filesystem::path&) override
    {
        ++remove_calls;
        return paperbreak::Result<void>::success();
    }

    void cancel_pending() noexcept override
    {
        ++cancel_calls;
    }

    void release()
    {
        {
            std::scoped_lock lock{mutex};
            block_writes = false;
        }
        condition.notify_all();
    }

    void wait_until_entered(const std::uint64_t count)
    {
        std::unique_lock lock{mutex};
        ASSERT_TRUE(entered.wait_for(lock, 1s, [&] { return write_calls.load() >= count; }));
    }

    std::mutex mutex;
    std::condition_variable_any condition;
    std::condition_variable entered;
    bool block_writes{};
    std::optional<paperbreak::Error> prepare_error;
    std::optional<paperbreak::Error> write_error;
    std::atomic_uint64_t write_calls{};
    std::atomic_uint64_t remove_calls{};
    std::atomic_uint64_t cancel_calls{};
};

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

std::uint32_t little_u32(const std::vector<std::byte>& bytes, const std::size_t offset)
{
    std::uint32_t result{};
    for (std::size_t index = 0U; index < 4U; ++index)
        result |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
                  << (index * 8U);
    return result;
}

} // namespace

TEST(StorageNvmeCache, Crc32cAndMaximumBlockLayoutMatchV2Contract)
{
    constexpr std::string_view check = "123456789";
    EXPECT_EQ(paperbreak::storage::crc32c(std::as_bytes(std::span{check})), 0xE3069283U);
    auto maximum = paperbreak::storage::maximum_nvme_block_bytes(3U, 4U);
    ASSERT_TRUE(maximum);
    EXPECT_EQ(maximum.value(), 16384U);
}

TEST(StorageNvmeCache, WindowsStorePublishesV2FooterMarkerAndStructuralCrc)
{
    TemporaryDirectory temporary{"format"};
    auto store = paperbreak::storage::make_windows_nvme_block_store();
    ASSERT_TRUE(store->prepare(temporary.path()));
    const auto start = paperbreak::camera::MonotonicTime{1s};
    paperbreak::storage::NvmeBlock block{
        .block_id = {std::byte{1U}},
        .generation = 7U,
        .camera_id = "CAM01",
        .start_monotonic_time = start,
        .start_wall_clock_time = paperbreak::camera::WallClockTime{1s},
        .maximum_frame_bytes = 4U,
        .index_capacity = 3U,
        .frames = {frame(1U, start), frame(2U, start + 10ms, 0x2BU)}};
    auto written = store->write_block({.root = temporary.path(),
                                       .block = &block,
                                       .write_limit_bytes_per_second = 1024U * 1024U * 100U,
                                       .deadline = std::chrono::steady_clock::now() + 2s},
                                      {});
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(written.value().physical_bytes, 16384U);
    std::ifstream input{written.value().path, std::ios::binary};
    std::vector<std::byte> bytes(static_cast<std::size_t>(written.value().physical_bytes));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(input);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes.data()), 7U), "PBNVME2");
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes.data() + bytes.size() - 8U), 7U),
              "COMMIT2");
    EXPECT_EQ(little_u32(bytes, bytes.size() - 4096U + 12U), 2U);
    EXPECT_EQ(little_u32(bytes, 4096U + 76U), 0U);
    EXPECT_EQ(little_u32(bytes, 4096U + 96U + 76U), 0U);
    EXPECT_EQ(little_u32(bytes, bytes.size() - 4096U + 60U), 0U);
    EXPECT_EQ(written.value().data_crc32c, 0U);
    const auto stored_footer_crc = little_u32(bytes, bytes.size() - 12U);
    std::fill(bytes.end() - 12U, bytes.end() - 8U, std::byte{0U});
    EXPECT_EQ(
        paperbreak::storage::crc32c(std::span{bytes}.last(paperbreak::storage::nvme_page_bytes)),
        stored_footer_crc);
}

TEST(StorageNvmeCache, WindowsStoreEnforcesTotalWriteDeadline)
{
    TemporaryDirectory temporary{"deadline"};
    auto store = paperbreak::storage::make_windows_nvme_block_store();
    ASSERT_TRUE(store->prepare(temporary.path()));
    const auto start = paperbreak::camera::MonotonicTime{1s};
    paperbreak::storage::NvmeBlock block{.block_id = {std::byte{2U}},
                                         .generation = 1U,
                                         .camera_id = "CAM01",
                                         .start_monotonic_time = start,
                                         .start_wall_clock_time =
                                             paperbreak::camera::WallClockTime{1s},
                                         .maximum_frame_bytes = 4U,
                                         .index_capacity = 3U,
                                         .frames = {frame(1U, start)}};
    auto written = store->write_block({.root = temporary.path(),
                                       .block = &block,
                                       .write_limit_bytes_per_second = 12U,
                                       .deadline = std::chrono::steady_clock::now() + 20ms},
                                      {});
    ASSERT_FALSE(written);
    EXPECT_EQ(written.error().business_code, "NVME_WRITE_TIMEOUT");
}

TEST(StorageNvmeCache, QueueIsBoundedPerCameraAndNeverWaitsForBlockedWriter)
{
    TemporaryDirectory temporary{"queue"};
    auto store = std::make_shared<ControlledBlockStore>();
    store->block_writes = true;
    auto cache = paperbreak::storage::NvmeRollingCache::create(options(temporary.path()), store);
    ASSERT_TRUE(cache);
    ASSERT_TRUE(cache.value()->start());
    const auto start = paperbreak::camera::MonotonicTime{1s};
    for (std::uint64_t index = 0U; index < 12U; ++index)
    {
        auto submitted = cache.value()->submit_frame(frame(index + 1U, start + index * 10ms));
        ASSERT_TRUE(submitted);
    }
    store->wait_until_entered(1U);
    const auto snapshot = cache.value()->snapshot();
    EXPECT_EQ(snapshot.queue_capacity, 2U);
    EXPECT_LE(snapshot.queue_depth, 2U);
    EXPECT_EQ(snapshot.queue_high_watermark, 2U);
    EXPECT_GE(snapshot.rejected_blocks, 1U);
    EXPECT_EQ(snapshot.submitted_frames, 12U);
    store->release();
    cache.value()->request_stop();
    EXPECT_TRUE(cache.value()->join(std::chrono::steady_clock::now() + 2s));
}

TEST(StorageNvmeCache, EachStartUsesANewEmptySessionAndLeavesOldCacheUntouched)
{
    TemporaryDirectory temporary{"sessions"};
    const auto old_session = temporary.path() / "sessions" / "old-session";
    const auto legacy_block = temporary.path() / "legacy-block.pbnvme";
    std::filesystem::create_directories(old_session);
    {
        std::ofstream old_file{old_session / "old.pbnvme", std::ios::binary};
        old_file << "old-session-data";
        std::ofstream legacy_file{legacy_block, std::ios::binary};
        legacy_file << "legacy-root-data";
    }

    auto first = paperbreak::storage::NvmeRollingCache::create(options(temporary.path()));
    ASSERT_TRUE(first);
    ASSERT_TRUE(first.value()->start());
    const auto first_snapshot = first.value()->snapshot();
    EXPECT_EQ(first_snapshot.indexed_blocks, 0U);
    EXPECT_EQ(first_snapshot.active_session_root.parent_path(), temporary.path() / "sessions");
    EXPECT_NE(first_snapshot.active_session_root, old_session);
    first.value()->request_stop();
    ASSERT_TRUE(first.value()->join(std::chrono::steady_clock::now() + 2s));

    auto second = paperbreak::storage::NvmeRollingCache::create(options(temporary.path()));
    ASSERT_TRUE(second);
    ASSERT_TRUE(second.value()->start());
    const auto second_snapshot = second.value()->snapshot();
    EXPECT_EQ(second_snapshot.indexed_blocks, 0U);
    EXPECT_NE(second_snapshot.active_session_root, first_snapshot.active_session_root);
    EXPECT_TRUE(std::filesystem::is_regular_file(old_session / "old.pbnvme"));
    EXPECT_TRUE(std::filesystem::is_regular_file(legacy_block));
    second.value()->request_stop();
    EXPECT_TRUE(second.value()->join(std::chrono::steady_clock::now() + 2s));
}

TEST(StorageNvmeCache, JoinHonorsCallerDeadlineAndCanFinishAfterCancellation)
{
    TemporaryDirectory temporary{"join"};
    auto store = std::make_shared<ControlledBlockStore>();
    store->block_writes = true;
    auto cache = paperbreak::storage::NvmeRollingCache::create(options(temporary.path()), store);
    ASSERT_TRUE(cache);
    ASSERT_TRUE(cache.value()->start());
    const auto start = paperbreak::camera::MonotonicTime{1s};
    for (std::uint64_t index = 0U; index < 3U; ++index)
        ASSERT_TRUE(cache.value()->submit_frame(frame(index + 1U, start + index * 10ms)));
    store->wait_until_entered(1U);
    cache.value()->request_stop();
    auto timed_out = cache.value()->join(std::chrono::steady_clock::now() + 10ms);
    ASSERT_FALSE(timed_out);
    EXPECT_EQ(timed_out.error().business_code, "SYS_SHUTDOWN_TIMEOUT");
    EXPECT_EQ(store->cancel_calls.load(), 1U);
    store->release();
    EXPECT_TRUE(cache.value()->join(std::chrono::steady_clock::now() + 2s));
}

TEST(StorageNvmeCache, ReclaimsOldestKnownOrdinaryBlockAtFixedCapacity)
{
    TemporaryDirectory temporary{"capacity"};
    auto store = std::make_shared<ControlledBlockStore>();
    auto configured = options(temporary.path());
    configured.maximum_cache_bytes = 16384U;
    auto cache = paperbreak::storage::NvmeRollingCache::create(configured, store);
    ASSERT_TRUE(cache);
    ASSERT_TRUE(cache.value()->start());
    const auto start = paperbreak::camera::MonotonicTime{1s};
    for (std::uint64_t index = 0U; index < 6U; ++index)
        ASSERT_TRUE(cache.value()->submit_frame(frame(index + 1U, start + index * 10ms)));
    ASSERT_TRUE(wait_for([&] { return cache.value()->snapshot().committed_blocks == 2U; }));
    const auto snapshot = cache.value()->snapshot();
    EXPECT_EQ(snapshot.current_cache_bytes, 16384U);
    EXPECT_EQ(snapshot.blocks_reclaimed, 1U);
    EXPECT_EQ(store->remove_calls.load(), 1U);
    cache.value()->request_stop();
    EXPECT_TRUE(cache.value()->join(std::chrono::steady_clock::now() + 2s));
}

TEST(StorageNvmeCache, IoFailureDegradesToMemoryAndReportsStableError)
{
    TemporaryDirectory temporary{"failure"};
    auto store = std::make_shared<ControlledBlockStore>();
    store->write_error = paperbreak::make_error("NVME_WRITE_TIMEOUT", paperbreak::Severity::error,
                                                "injected", "storage", "storage.nvme.write", true);
    std::mutex errors_mutex;
    std::vector<std::string> errors;
    auto configured = options(temporary.path());
    configured.error_observer = [&errors, &errors_mutex](const paperbreak::Error& error) {
        std::scoped_lock lock{errors_mutex};
        errors.push_back(error.business_code);
    };
    auto cache = paperbreak::storage::NvmeRollingCache::create(configured, store);
    ASSERT_TRUE(cache);
    ASSERT_TRUE(cache.value()->start());
    const auto start = paperbreak::camera::MonotonicTime{1s};
    for (std::uint64_t index = 0U; index < 3U; ++index)
        ASSERT_TRUE(cache.value()->submit_frame(frame(index + 1U, start + index * 10ms)));
    ASSERT_TRUE(wait_for([&] {
        return cache.value()->snapshot().state ==
               paperbreak::storage::NvmeCacheState::memory_degraded;
    }));
    const auto snapshot = cache.value()->snapshot();
    EXPECT_EQ(snapshot.write_timeouts, 1U);
    EXPECT_TRUE(snapshot.event_writes_allowed);
    ASSERT_TRUE(wait_for([&] {
        std::scoped_lock lock{errors_mutex};
        return !errors.empty();
    }));
    {
        std::scoped_lock lock{errors_mutex};
        EXPECT_EQ(errors.back(), "NVME_WRITE_TIMEOUT");
    }
    auto submitted = cache.value()->submit_frame(frame(4U, start + 40ms));
    ASSERT_TRUE(submitted);
    EXPECT_EQ(submitted.value(), paperbreak::storage::NvmeSubmitStatus::memory_degraded);
}

TEST(StorageNvmeCache, CriticalStopsOnlyOrdinaryCacheWhileStopSaveBlocksEvents)
{
    TemporaryDirectory temporary{"watermark"};
    auto store = std::make_shared<ControlledBlockStore>();
    auto cache = paperbreak::storage::NvmeRollingCache::create(options(temporary.path()), store);
    ASSERT_TRUE(cache);
    ASSERT_TRUE(cache.value()->start());
    cache.value()->set_storage_watermark(paperbreak::storage::StorageWatermark::critical);
    auto critical = cache.value()->snapshot();
    EXPECT_EQ(critical.state, paperbreak::storage::NvmeCacheState::watermark_blocked);
    EXPECT_TRUE(critical.event_writes_allowed);
    auto submitted = cache.value()->submit_frame(frame(1U, paperbreak::camera::MonotonicTime{1s}));
    ASSERT_TRUE(submitted);
    EXPECT_EQ(submitted.value(), paperbreak::storage::NvmeSubmitStatus::watermark_blocked);
    cache.value()->set_storage_watermark(paperbreak::storage::StorageWatermark::stop_save);
    EXPECT_FALSE(cache.value()->snapshot().event_writes_allowed);
    cache.value()->request_stop();
    EXPECT_TRUE(cache.value()->join(std::chrono::steady_clock::now() + 2s));
}

TEST(StorageNvmeCache, UnavailableRootStartsInExplicitMemoryDegradation)
{
    TemporaryDirectory temporary{"unavailable"};
    auto store = std::make_shared<ControlledBlockStore>();
    store->prepare_error =
        paperbreak::make_error("NVME_CACHE_UNAVAILABLE", paperbreak::Severity::error, "injected",
                               "storage", "storage.nvme.prepare", true);
    auto cache = paperbreak::storage::NvmeRollingCache::create(options(temporary.path()), store);
    ASSERT_TRUE(cache);
    EXPECT_TRUE(cache.value()->start());
    const auto snapshot = cache.value()->snapshot();
    EXPECT_EQ(snapshot.state, paperbreak::storage::NvmeCacheState::memory_degraded);
    EXPECT_TRUE(snapshot.event_writes_allowed);
}

TEST(StorageNvmeCache, ProtectedOnlyCapacityNeverDeletesAndDegradesExplicitly)
{
    TemporaryDirectory temporary{"protected"};
    auto store = std::make_shared<ControlledBlockStore>();
    auto configured = options(temporary.path());
    configured.maximum_cache_bytes = 16384U;
    auto cache = paperbreak::storage::NvmeRollingCache::create(configured, store);
    ASSERT_TRUE(cache);
    ASSERT_TRUE(cache.value()->start());
    const auto start = paperbreak::camera::MonotonicTime{1s};
    for (std::uint64_t index = 0U; index < 3U; ++index)
        ASSERT_TRUE(cache.value()->submit_frame(frame(index + 1U, start + index * 10ms)));
    ASSERT_TRUE(wait_for([&] { return cache.value()->snapshot().committed_blocks == 1U; }));
    auto lease = cache.value()->protect_event_window(
        {.event_id = "EVT-PROTECTED",
         .camera_ids = {"CAM01"},
         .start_monotonic_time = paperbreak::camera::MonotonicTime{0s},
         .end_monotonic_time = paperbreak::camera::MonotonicTime{3s},
         .start_wall_clock_time = paperbreak::camera::WallClockTime{0s},
         .end_wall_clock_time = paperbreak::camera::WallClockTime{3s}});
    ASSERT_TRUE(lease) << lease.error().message;
    EXPECT_EQ(lease.value().protected_blocks, 1U);
    for (std::uint64_t index = 3U; index < 6U; ++index)
        ASSERT_TRUE(cache.value()->submit_frame(frame(index + 1U, start + index * 10ms)));
    ASSERT_TRUE(wait_for([&] {
        return cache.value()->snapshot().state ==
               paperbreak::storage::NvmeCacheState::memory_degraded;
    }));
    const auto snapshot = cache.value()->snapshot();
    ASSERT_TRUE(snapshot.last_error);
    EXPECT_EQ(snapshot.last_error->business_code, "NVME_CACHE_PROTECTED");
    EXPECT_EQ(snapshot.protected_blocks, 1U);
    EXPECT_EQ(store->remove_calls.load(), 0U);
}
