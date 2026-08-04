#include "paperbreak/storage/event_store.hpp"
#include "paperbreak/storage/storage_policy.hpp"

#include <gtest/gtest.h>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using namespace paperbreak;
using namespace paperbreak::camera;
using namespace paperbreak::event;
using namespace paperbreak::storage;

class TemporaryDirectory final
{
  public:
    explicit TemporaryDirectory(const std::string& suffix)
    {
        static std::atomic_uint64_t sequence{};
        path_ = std::filesystem::temp_directory_path() /
                (L"PaperBreak-M5-08-中文 空格-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(sequence.fetch_add(1U)) + L"-" +
                 std::filesystem::path{suffix}.wstring());
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory()
    {
        if (path_.filename().wstring().starts_with(L"PaperBreak-M5-08-"))
        {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

MetadataDatabaseOptions database_options(const TemporaryDirectory& temporary)
{
    return {.database_path = temporary.path() / L"数据库" / L"事件.db",
            .event_root = temporary.path() / L"事件 根",
            .backup_directory = temporary.path() / L"备份"};
}

FrameView frame(const std::uint64_t sequence, const std::chrono::milliseconds offset)
{
    auto buffer = std::make_shared<FrameBuffer>(16U);
    for (std::size_t index = 0U; index < 16U; ++index)
        buffer->writable_bytes()[index] = static_cast<std::byte>(sequence + index);
    EXPECT_TRUE(buffer->set_size(16U));
    const auto wall_base = WallClockTime{std::chrono::sys_days{std::chrono::year{2026} / 8 / 1}};
    auto result = make_frame_view({.camera_id = "CAM01",
                                   .camera_frame_number = 100U + sequence,
                                   .sequence_number = sequence,
                                   .received_monotonic_time = MonotonicTime{offset},
                                   .received_wall_clock_time = wall_base + offset,
                                   .geometry = {.width = 4U, .height = 4U, .stride = 4U},
                                   .pixel_format = PixelFormat::mono8,
                                   .buffer = std::move(buffer)});
    EXPECT_TRUE(result);
    return std::move(result).value();
}

EventPersistenceRequest event_request(const std::string& event_id,
                                      const std::chrono::milliseconds offset,
                                      const std::string& upload_state)
{
    const auto wall_base = WallClockTime{std::chrono::sys_days{std::chrono::year{2026} / 8 / 1}};
    auto first = frame(1U, offset);
    auto second = frame(2U, offset + 100ms);
    KeyFrameDescriptor descriptor{.camera_id = "CAM01",
                                  .camera_frame_number = second.camera_frame_number(),
                                  .sequence_number = second.sequence_number(),
                                  .monotonic_time = second.received_monotonic_time(),
                                  .wall_clock_time = second.received_wall_clock_time(),
                                  .geometry = second.geometry(),
                                  .pixel_format = second.pixel_format(),
                                  .reasons = {KeyFrameReason::candidate_trigger}};
    EventPersistenceRequest request;
    request.metadata = {.event_id = event_id,
                        .event_state = "Confirmed",
                        .candidate_time = wall_base + offset + 100ms,
                        .confirmed_time = wall_base + offset + 150ms,
                        .start_time = wall_base + offset,
                        .end_time = wall_base + offset + 200ms,
                        .camera_ids = {"CAM01"},
                        .trigger_camera_id = "CAM01",
                        .trigger_frame_number = 102U,
                        .trigger_reason = "ManualTest",
                        .confidence = 0.9,
                        .pre_event_duration = 100ms,
                        .post_event_duration = 100ms,
                        .algorithm_name = "mock",
                        .algorithm_version = "m5",
                        .config_version = "1",
                        .machine_id = "EDGE-01",
                        .production_line_id = "LINE-A",
                        .paper_type = "test",
                        .upload_state = upload_state,
                        .time_quality = "Normal"};
    request.window = {.event_id = event_id,
                      .version = 1U,
                      .requested_start = MonotonicTime{offset},
                      .requested_end = MonotonicTime{offset + 200ms},
                      .closed_monotonic_time = MonotonicTime{offset + 201ms},
                      .display_wall_clock_time = wall_base + offset + 100ms,
                      .camera_windows = {{.camera_id = "CAM01",
                                          .requested_start = MonotonicTime{offset},
                                          .requested_end = MonotonicTime{offset + 200ms},
                                          .available_start = MonotonicTime{offset},
                                          .available_end = MonotonicTime{offset + 100ms},
                                          .first_sequence_number = 1U,
                                          .last_sequence_number = 2U,
                                          .frames = {first, second},
                                          .complete = true}},
                      .complete = true};
    request.key_frames.push_back(
        {.descriptor = std::move(descriptor),
         .jpeg = {std::byte{0xff}, std::byte{0xd8}, std::byte{0xff}, std::byte{0xd9}}});
    return request;
}

EventPersistenceOutcome persist_and_index(EventMetadataDatabase& database,
                                          const MetadataDatabaseOptions& options,
                                          EventPersistenceRequest request)
{
    auto writer = EventTransactionWriter::create({.event_root = options.event_root});
    EXPECT_TRUE(writer);
    auto persisted = writer.value()->persist(request);
    EXPECT_TRUE(persisted) << (persisted ? "" : persisted.error().business_code);
    EXPECT_TRUE(database.index_committed_event(persisted.value().committed_directory));
    return std::move(persisted).value();
}

class ControlledFileSystem final : public IStoragePolicyFileSystem
{
  public:
    StorageSpace sampled_space{.capacity_bytes = 1000U, .available_bytes = 1000U};
    bool fail_next_move{};
    bool fail_next_remove{};

    ControlledFileSystem() : delegate_(make_storage_policy_file_system()) {}

    Result<StorageSpace> space(const std::filesystem::path&) override
    {
        return Result<StorageSpace>::success(sampled_space);
    }
    Result<bool> is_directory(const std::filesystem::path& path) override
    {
        return delegate_->is_directory(path);
    }
    Result<void> create_directories(const std::filesystem::path& path) override
    {
        return delegate_->create_directories(path);
    }
    Result<void> move_directory_atomically(const std::filesystem::path& source,
                                           const std::filesystem::path& destination) override
    {
        if (std::exchange(fail_next_move, false))
            return Result<void>::failure(make_error("EVENT_DELETE_FAILED", Severity::error,
                                                    "injected move failure", "storage-test", "move",
                                                    true));
        return delegate_->move_directory_atomically(source, destination);
    }
    Result<void> remove_tree(const std::filesystem::path& path) override
    {
        if (std::exchange(fail_next_remove, false))
            return Result<void>::failure(make_error("EVENT_DELETE_FAILED", Severity::error,
                                                    "injected remove failure", "storage-test",
                                                    "remove", true));
        return delegate_->remove_tree(path);
    }
    Result<std::vector<std::filesystem::path>> expired_temporary_entries(
        const std::filesystem::path& root, const std::chrono::system_clock::time_point cutoff,
        const std::size_t maximum_entries) override
    {
        return delegate_->expired_temporary_entries(root, cutoff, maximum_entries);
    }

  private:
    std::shared_ptr<IStoragePolicyFileSystem> delegate_;
};

StoragePolicyOptions policy_options(const MetadataDatabaseOptions& database,
                                    const std::filesystem::path& temporary_root = {})
{
    StoragePolicyOptions options{.event_root = database.event_root,
                                 .watermarks = {.warning_available_bytes = 600U,
                                                .critical_available_bytes = 300U,
                                                .stop_save_available_bytes = 100U},
                                 .temporary_maximum_age = 24h,
                                 .maximum_deletions_per_run = 16U,
                                 .maximum_temporary_entries_per_run = 16U};
    if (!temporary_root.empty())
        options.temporary_roots.push_back(temporary_root);
    return options;
}

TEST(StoragePolicy, ClassifiesExactWatermarkBoundariesAndRejectsStopSaveWrites)
{
    EXPECT_EQ(StoragePolicyManager::classify(601U, {.warning_available_bytes = 600U,
                                                    .critical_available_bytes = 300U,
                                                    .stop_save_available_bytes = 100U}),
              StorageWatermark::normal);
    EXPECT_EQ(StoragePolicyManager::classify(600U, {.warning_available_bytes = 600U,
                                                    .critical_available_bytes = 300U,
                                                    .stop_save_available_bytes = 100U}),
              StorageWatermark::warning);
    EXPECT_EQ(StoragePolicyManager::classify(300U, {.warning_available_bytes = 600U,
                                                    .critical_available_bytes = 300U,
                                                    .stop_save_available_bytes = 100U}),
              StorageWatermark::critical);
    EXPECT_EQ(StoragePolicyManager::classify(100U, {.warning_available_bytes = 600U,
                                                    .critical_available_bytes = 300U,
                                                    .stop_save_available_bytes = 100U}),
              StorageWatermark::stop_save);

    TemporaryDirectory temporary{"watermark"};
    const auto database_configuration = database_options(temporary);
    auto database = EventMetadataDatabase::open(database_configuration);
    ASSERT_TRUE(database);
    auto files = std::make_shared<ControlledFileSystem>();
    files->sampled_space.available_bytes = 100U;
    auto manager = StoragePolicyManager::create(policy_options(database_configuration),
                                                *database.value(), files);
    ASSERT_TRUE(manager);
    auto denied = manager.value()->admit_large_write();
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().business_code, "STORAGE_LOW_SPACE");
    EXPECT_FALSE(manager.value()->snapshot().large_writes_allowed);

    files->sampled_space.available_bytes = 300U;
    ASSERT_TRUE(manager.value()->run_maintenance(std::chrono::system_clock::now()));
    EXPECT_EQ(manager.value()->snapshot().watermark, StorageWatermark::critical);
    EXPECT_FALSE(manager.value()->snapshot().ordinary_rolling_writes_allowed);
    EXPECT_TRUE(manager.value()->admit_large_write());
}

TEST(StoragePolicy, WarningDeletesOnlyUploadedAllowedUnlockedOldestEvent)
{
    TemporaryDirectory temporary{"eligible"};
    const auto options = database_options(temporary);
    auto database = EventMetadataDatabase::open(options);
    ASSERT_TRUE(database);
    const auto oldest =
        persist_and_index(*database.value(), options,
                          event_request("019fcb80-0001-7000-8000-000000000001", 100ms, "Uploaded"));
    const auto newer = persist_and_index(
        *database.value(), options,
        event_request("019fcb80-0002-7000-8000-000000000002", 2100ms, "Uploaded"));
    const auto pending =
        persist_and_index(*database.value(), options,
                          event_request("019fcb80-0003-7000-8000-000000000003", 3100ms, "Pending"));
    ASSERT_TRUE(database.value()->set_retention_policy(oldest.event_id, false, true, 1));
    ASSERT_TRUE(database.value()->set_retention_policy(newer.event_id, false, true, 1));
    ASSERT_TRUE(database.value()->set_retention_policy(pending.event_id, false, true, 1));

    auto files = std::make_shared<ControlledFileSystem>();
    files->sampled_space.available_bytes = 600U;
    auto policy = policy_options(options);
    policy.maximum_deletions_per_run = 1U;
    auto manager = StoragePolicyManager::create(policy, *database.value(), files);
    ASSERT_TRUE(manager);
    EXPECT_TRUE(manager.value()->set_retention_age(std::chrono::days{2}));
    auto invalid_retention = manager.value()->set_retention_age(std::chrono::days{0});
    ASSERT_FALSE(invalid_retention);
    EXPECT_EQ(invalid_retention.error().business_code, "SYS_CONFIG_INVALID");
    auto report =
        manager.value()->run_maintenance(std::chrono::sys_days{std::chrono::year{2026} / 8 / 4});
    ASSERT_TRUE(report);
    EXPECT_EQ(report.value().events_deleted, 1U);
    EXPECT_FALSE(std::filesystem::exists(oldest.committed_directory));
    EXPECT_TRUE(std::filesystem::is_directory(newer.committed_directory));
    EXPECT_TRUE(std::filesystem::is_directory(pending.committed_directory));

    ASSERT_TRUE(database.value()->set_retention_policy(newer.event_id, true, true, 2));
    policy.maximum_deletions_per_run = 16U;
    auto locked_manager = StoragePolicyManager::create(policy, *database.value(), files);
    ASSERT_TRUE(locked_manager);
    auto locked_report = locked_manager.value()->run_maintenance(
        std::chrono::sys_days{std::chrono::year{2026} / 8 / 4});
    ASSERT_TRUE(locked_report);
    EXPECT_EQ(locked_report.value().events_deleted, 0U);
    EXPECT_TRUE(std::filesystem::is_directory(newer.committed_directory));
    EXPECT_TRUE(std::filesystem::is_directory(pending.committed_directory));
    auto rows = database.value()->query_events({.limit = 10U});
    ASSERT_TRUE(rows);
    const auto locked = std::ranges::find_if(
        rows.value().events, [&](const auto& row) { return row.event_id == newer.event_id; });
    ASSERT_NE(locked, rows.value().events.end());
    EXPECT_TRUE(locked->retention_locked);
    EXPECT_TRUE(locked->deletion_allowed);
    EXPECT_EQ(locked->deletion_state, "Active");
}

TEST(StoragePolicy, RecoversMoveFailureAndCleansOnlyExpiredExplicitTemporaryEntries)
{
    TemporaryDirectory temporary{"recovery"};
    const auto options = database_options(temporary);
    const auto temporary_root = temporary.path() / L"临时 工作";
    std::filesystem::create_directories(temporary_root);
    const auto expired = temporary_root / L"过期 项";
    const auto fresh = temporary_root / L"新 项";
    std::filesystem::create_directories(expired);
    std::filesystem::create_directories(fresh);
    std::filesystem::last_write_time(expired, std::filesystem::file_time_type::clock::now() - 48h);

    auto database = EventMetadataDatabase::open(options);
    ASSERT_TRUE(database);
    const auto event =
        persist_and_index(*database.value(), options,
                          event_request("019fcb80-0004-7000-8000-000000000004", 100ms, "Uploaded"));
    ASSERT_TRUE(database.value()->set_retention_policy(event.event_id, false, true, 1));
    auto files = std::make_shared<ControlledFileSystem>();
    files->sampled_space.available_bytes = 600U;
    files->fail_next_move = true;
    auto manager = StoragePolicyManager::create(policy_options(options, temporary_root),
                                                *database.value(), files);
    ASSERT_TRUE(manager);
    const auto now = std::chrono::system_clock::now();
    auto failed = manager.value()->run_maintenance(now);
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed.value().events_deleted, 0U);
    ASSERT_EQ(failed.value().failures.size(), 1U);
    EXPECT_TRUE(std::filesystem::is_directory(event.committed_directory));
    EXPECT_FALSE(std::filesystem::exists(expired));
    EXPECT_TRUE(std::filesystem::is_directory(fresh));

    files->fail_next_remove = true;
    auto remove_failed = manager.value()->run_maintenance(now + 1s);
    ASSERT_TRUE(remove_failed);
    EXPECT_EQ(remove_failed.value().events_deleted, 0U);
    EXPECT_FALSE(std::filesystem::exists(event.committed_directory));
    EXPECT_TRUE(std::filesystem::is_directory(options.event_root / ".deletions" /
                                              (event.event_id + ".deleting")));
    auto reconciled = database.value()->reconcile();
    ASSERT_TRUE(reconciled);
    EXPECT_EQ(reconciled.value().marked_missing, 1U);

    auto recovered = manager.value()->run_maintenance(now + 2s);
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered.value().deletion_work_recovered, 1U);
    EXPECT_EQ(recovered.value().events_deleted, 1U);
    auto rows = database.value()->query_events({.limit = 10U});
    ASSERT_TRUE(rows);
    ASSERT_EQ(rows.value().events.size(), 1U);
    EXPECT_EQ(rows.value().events.front().storage_state, "Missing");
}

TEST(StoragePolicy, RetentionAgeAndCapacityRemainBoundedAndConfigurationIsValidated)
{
    TemporaryDirectory temporary{"limits"};
    const auto options = database_options(temporary);
    auto database = EventMetadataDatabase::open(options);
    ASSERT_TRUE(database);
    const auto first =
        persist_and_index(*database.value(), options,
                          event_request("019fcb80-0005-7000-8000-000000000005", 100ms, "Uploaded"));
    const auto second = persist_and_index(
        *database.value(), options,
        event_request("019fcb80-0006-7000-8000-000000000006", 2100ms, "Uploaded"));
    ASSERT_TRUE(database.value()->set_retention_policy(first.event_id, false, true, 1));
    ASSERT_TRUE(database.value()->set_retention_policy(second.event_id, false, true, 1));
    auto bytes = database.value()->retained_event_bytes();
    ASSERT_TRUE(bytes);
    ASSERT_GT(bytes.value(), 1U);

    auto files = std::make_shared<ControlledFileSystem>();
    auto policy = policy_options(options);
    policy.maximum_event_bytes = bytes.value() - 1U;
    policy.maximum_deletions_per_run = 1U;
    auto manager = StoragePolicyManager::create(policy, *database.value(), files);
    ASSERT_TRUE(manager);
    auto report =
        manager.value()->run_maintenance(std::chrono::sys_days{std::chrono::year{2026} / 8 / 4});
    ASSERT_TRUE(report);
    EXPECT_EQ(report.value().events_deleted, 1U);
    EXPECT_FALSE(std::filesystem::exists(first.committed_directory));
    EXPECT_TRUE(std::filesystem::is_directory(second.committed_directory));

    auto age_policy = policy_options(options);
    age_policy.retention_age = std::chrono::days{1};
    age_policy.maximum_deletions_per_run = 1U;
    auto age_manager = StoragePolicyManager::create(age_policy, *database.value(), files);
    ASSERT_TRUE(age_manager);
    auto age_report = age_manager.value()->run_maintenance(
        std::chrono::sys_days{std::chrono::year{2026} / 8 / 4});
    ASSERT_TRUE(age_report);
    EXPECT_EQ(age_report.value().events_deleted, 1U);
    EXPECT_FALSE(std::filesystem::exists(second.committed_directory));

    auto invalid = policy_options(options);
    invalid.watermarks.critical_available_bytes = invalid.watermarks.warning_available_bytes;
    auto rejected = StoragePolicyManager::create(invalid, *database.value(), files);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().business_code, "SYS_CONFIG_INVALID");
}

TEST(StoragePolicy, CompletesInterruptedMovedAndAlreadyRemovedDeletionWork)
{
    TemporaryDirectory temporary{"interrupted"};
    const auto options = database_options(temporary);
    auto database = EventMetadataDatabase::open(options);
    ASSERT_TRUE(database);
    const auto moved =
        persist_and_index(*database.value(), options,
                          event_request("019fcb80-0007-7000-8000-000000000007", 100ms, "Uploaded"));
    const auto removed = persist_and_index(
        *database.value(), options,
        event_request("019fcb80-0008-7000-8000-000000000008", 2100ms, "Uploaded"));
    ASSERT_TRUE(database.value()->set_retention_policy(moved.event_id, false, true, 1));
    ASSERT_TRUE(database.value()->set_retention_policy(removed.event_id, false, true, 1));
    const auto moved_relative =
        std::filesystem::path{".deletions"} / (moved.event_id + ".deleting");
    const auto removed_relative =
        std::filesystem::path{".deletions"} / (removed.event_id + ".deleting");
    auto moved_claimed = database.value()->begin_deletion(moved.event_id, moved_relative, 2);
    auto removed_claimed = database.value()->begin_deletion(removed.event_id, removed_relative, 2);
    ASSERT_TRUE(moved_claimed);
    ASSERT_TRUE(removed_claimed);
    EXPECT_TRUE(moved_claimed.value());
    EXPECT_TRUE(removed_claimed.value());
    std::filesystem::create_directories((options.event_root / moved_relative).parent_path());
    std::filesystem::rename(moved.committed_directory, options.event_root / moved_relative);
    std::filesystem::remove_all(removed.committed_directory);

    auto files = std::make_shared<ControlledFileSystem>();
    auto manager = StoragePolicyManager::create(policy_options(options), *database.value(), files);
    ASSERT_TRUE(manager);
    auto report = manager.value()->run_maintenance(std::chrono::system_clock::now());
    ASSERT_TRUE(report);
    EXPECT_EQ(report.value().deletion_work_recovered, 2U);
    EXPECT_EQ(report.value().events_deleted, 2U);
    EXPECT_FALSE(std::filesystem::exists(options.event_root / moved_relative));

    auto rows = database.value()->query_events({.limit = 10U});
    ASSERT_TRUE(rows);
    ASSERT_EQ(rows.value().events.size(), 2U);
    for (const auto& row : rows.value().events)
    {
        EXPECT_EQ(row.storage_state, "Missing");
        EXPECT_EQ(row.deletion_state, "Deleted");
    }
}

} // namespace
