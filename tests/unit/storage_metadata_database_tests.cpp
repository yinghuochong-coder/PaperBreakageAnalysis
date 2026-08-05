#include "paperbreak/storage/event_store.hpp"
#include "paperbreak/storage/metadata_database.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
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
                (L"PaperBreak-M5-07-中文 空格-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(sequence.fetch_add(1U)) + L"-" +
                 std::filesystem::path{suffix}.wstring());
        std::error_code error;
        std::filesystem::create_directories(path_, error);
        if (error)
            throw std::runtime_error{"failed to create test directory"};
    }

    ~TemporaryDirectory()
    {
        if (path_.filename().wstring().starts_with(L"PaperBreak-M5-07-"))
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
    return {.database_path = temporary.path() / L"数据库" / L"事件 元数据.db",
            .event_root = temporary.path() / L"事件 根目录",
            .backup_directory = temporary.path() / L"备份 目录"};
}

class RawDatabase final
{
  public:
    explicit RawDatabase(const std::filesystem::path& path)
    {
        if (sqlite3_open16(path.c_str(), &database_) != SQLITE_OK)
            throw std::runtime_error{"failed to open raw sqlite database"};
    }
    ~RawDatabase()
    {
        if (database_ != nullptr)
            sqlite3_close(database_);
    }
    RawDatabase(const RawDatabase&) = delete;
    RawDatabase& operator=(const RawDatabase&) = delete;

    void execute(const std::string& sql)
    {
        char* message = nullptr;
        const auto result = sqlite3_exec(database_, sql.c_str(), nullptr, nullptr, &message);
        const std::string detail = message == nullptr ? std::string{} : message;
        sqlite3_free(message);
        if (result != SQLITE_OK)
            throw std::runtime_error{"sqlite exec failed: " + detail};
    }

    [[nodiscard]] std::int64_t scalar_int64(const std::string& sql) const
    {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
            throw std::runtime_error{"sqlite prepare failed"};
        const auto result = sqlite3_step(statement);
        const auto value = result == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
        sqlite3_finalize(statement);
        return value;
    }

    [[nodiscard]] std::set<std::string> table_names() const
    {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database_,
                               "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name",
                               -1, &statement, nullptr) != SQLITE_OK)
            throw std::runtime_error{"sqlite table query prepare failed"};
        std::set<std::string> names;
        while (sqlite3_step(statement) == SQLITE_ROW)
            names.emplace(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)));
        sqlite3_finalize(statement);
        return names;
    }

  private:
    sqlite3* database_{};
};

FrameView frame(const std::string& camera_id, const std::uint64_t sequence,
                const std::chrono::milliseconds offset)
{
    auto buffer = std::make_shared<FrameBuffer>(16U);
    for (std::size_t index = 0U; index < 16U; ++index)
        buffer->writable_bytes()[index] = static_cast<std::byte>(sequence + index);
    EXPECT_TRUE(buffer->set_size(16U));
    const auto wall_base = WallClockTime{std::chrono::sys_days{std::chrono::year{2026} / 8 / 4}};
    auto result = make_frame_view({.camera_id = camera_id,
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

EventPersistenceRequest event_request(const std::string& event_id, const std::string& camera_id,
                                      const std::string& state,
                                      const std::chrono::milliseconds offset)
{
    const auto wall_base = WallClockTime{std::chrono::sys_days{std::chrono::year{2026} / 8 / 4}};
    auto first = frame(camera_id, 1U, offset);
    auto second = frame(camera_id, 2U, offset + 100ms);
    KeyFrameDescriptor descriptor{
        .camera_id = camera_id,
        .camera_frame_number = second.camera_frame_number(),
        .sequence_number = second.sequence_number(),
        .monotonic_time = second.received_monotonic_time(),
        .wall_clock_time = second.received_wall_clock_time(),
        .geometry = second.geometry(),
        .pixel_format = second.pixel_format(),
        .reasons = {KeyFrameReason::candidate_trigger, KeyFrameReason::highest_confidence}};
    EventPersistenceRequest request;
    request.metadata = {.event_id = event_id,
                        .event_state = state,
                        .candidate_time = wall_base + offset + 100ms,
                        .confirmed_time = wall_base + offset + 150ms,
                        .start_time = wall_base + offset,
                        .end_time = wall_base + offset + 200ms,
                        .camera_ids = {camera_id},
                        .trigger_camera_id = camera_id,
                        .trigger_frame_number = 102U,
                        .trigger_reason = "ManualTest",
                        .confidence = 0.875,
                        .pre_event_duration = 100ms,
                        .post_event_duration = 100ms,
                        .algorithm_name = "mock-detector",
                        .algorithm_version = "m5",
                        .config_version = "42",
                        .machine_id = "EDGE-01",
                        .production_line_id = "LINE-A",
                        .paper_type = "test-paper",
                        .paper_speed = 900.5,
                        .upload_state = "Pending",
                        .time_quality = "Normal"};
    request.window = {.event_id = event_id,
                      .version = 3U,
                      .requested_start = MonotonicTime{offset},
                      .requested_end = MonotonicTime{offset + 200ms},
                      .closed_monotonic_time = MonotonicTime{offset + 201ms},
                      .display_wall_clock_time = wall_base + offset + 100ms,
                      .camera_windows = {{.camera_id = camera_id,
                                          .requested_start = MonotonicTime{offset},
                                          .requested_end = MonotonicTime{offset + 200ms},
                                          .available_start = MonotonicTime{offset},
                                          .available_end = MonotonicTime{offset + 100ms},
                                          .first_sequence_number = 1U,
                                          .last_sequence_number = 2U,
                                          .frames = {first, second},
                                          .complete = true}},
                      .complete = true};
    request.key_frames.push_back({.descriptor = std::move(descriptor),
                                  .jpeg = {std::byte{0xff}, std::byte{0xd8}, std::byte{0x01},
                                           std::byte{0xff}, std::byte{0xd9}}});
    return request;
}

EventPersistenceOutcome persist_event(const MetadataDatabaseOptions& options,
                                      EventPersistenceRequest request)
{
    auto writer = EventTransactionWriter::create({.event_root = options.event_root});
    EXPECT_TRUE(writer);
    auto result = writer.value()->persist(request);
    EXPECT_TRUE(result) << (result ? "" : result.error().business_code);
    return std::move(result).value();
}

TEST(StorageMetadataDatabase, CreatesAllMetadataTablesAndRepeatedOpenIsIdempotent)
{
    TemporaryDirectory temporary{"schema"};
    const auto options = database_options(temporary);
    auto opened = EventMetadataDatabase::open(options);
    ASSERT_TRUE(opened) << opened.error().business_code;
    EXPECT_TRUE(opened.value()->open_report().created);
    EXPECT_TRUE(opened.value()->open_report().migrated);
    EXPECT_EQ(opened.value()->open_report().schema_version, database_schema_version);
    EXPECT_FALSE(opened.value()->open_report().migration_backup.has_value());
    auto integrity = opened.value()->integrity_check();
    ASSERT_TRUE(integrity);
    EXPECT_TRUE(integrity.value().healthy);
    opened.value().reset();

    const std::set<std::string> expected{
        "alarm_history", "audit_logs",  "config_history",  "device_status_history",
        "event_cameras", "event_files", "event_retention", "events",
        "key_frames",    "upload_jobs"};
    RawDatabase raw{options.database_path};
    EXPECT_EQ(raw.scalar_int64("PRAGMA user_version"), database_schema_version);
    EXPECT_EQ(raw.table_names(), expected);

    auto reopened = EventMetadataDatabase::open(options);
    ASSERT_TRUE(reopened);
    EXPECT_FALSE(reopened.value()->open_report().created);
    EXPECT_FALSE(reopened.value()->open_report().migrated);
    EXPECT_FALSE(reopened.value()->open_report().migration_backup.has_value());
}

TEST(StorageMetadataDatabase, MigratesVersionZeroWithBackupAndRollsBackFailedMigration)
{
    TemporaryDirectory temporary{"migration"};
    const auto options = database_options(temporary);
    std::filesystem::create_directories(options.database_path.parent_path());
    {
        RawDatabase legacy{options.database_path};
        legacy.execute(
            "CREATE TABLE legacy_marker(value TEXT); INSERT INTO legacy_marker VALUES('kept');");
    }
    auto opened = EventMetadataDatabase::open(options);
    ASSERT_TRUE(opened) << opened.error().business_code;
    ASSERT_TRUE(opened.value()->open_report().migration_backup.has_value());
    EXPECT_TRUE(std::filesystem::is_regular_file(*opened.value()->open_report().migration_backup));
    {
        RawDatabase backup{*opened.value()->open_report().migration_backup};
        EXPECT_EQ(backup.scalar_int64("PRAGMA user_version"), 0);
        EXPECT_TRUE(backup.table_names().contains("legacy_marker"));
    }
    opened.value().reset();

    TemporaryDirectory conflict_temporary{"migration-conflict"};
    const auto conflict_options = database_options(conflict_temporary);
    std::filesystem::create_directories(conflict_options.database_path.parent_path());
    {
        RawDatabase conflict{conflict_options.database_path};
        conflict.execute("CREATE TABLE events(wrong_column TEXT);");
    }
    auto failed = EventMetadataDatabase::open(conflict_options);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().business_code, "DATABASE_MIGRATION_FAILED");
    const auto backup =
        conflict_options.backup_directory /
        (conflict_options.database_path.filename().string() + ".pre-migration-v0.bak");
    EXPECT_TRUE(std::filesystem::is_regular_file(backup));
    RawDatabase rolled_back{conflict_options.database_path};
    EXPECT_EQ(rolled_back.scalar_int64("PRAGMA user_version"), 0);
    EXPECT_EQ(rolled_back.scalar_int64(
                  "SELECT COUNT(*) FROM pragma_table_info('events') WHERE name='wrong_column'"),
              1);
}

TEST(StorageMetadataDatabase, MigratesVersionOneRetentionStateWithBackup)
{
    TemporaryDirectory temporary{"migration-v1"};
    const auto options = database_options(temporary);
    std::filesystem::create_directories(options.database_path.parent_path());
    {
        RawDatabase version_one{options.database_path};
        version_one.execute(
            "CREATE TABLE events(event_id TEXT PRIMARY KEY NOT NULL,relative_directory TEXT NOT "
            "NULL);"
            "CREATE TABLE upload_jobs(job_id INTEGER PRIMARY KEY,event_id TEXT NOT NULL "
            "REFERENCES events(event_id) ON DELETE CASCADE,state TEXT NOT NULL,attempts INTEGER "
            "NOT NULL DEFAULT 0,next_attempt_utc_ms INTEGER,checkpoint_json TEXT NOT NULL DEFAULT "
            "'{}',last_error_code TEXT,updated_at_utc_ms INTEGER NOT NULL,UNIQUE(event_id));"
            "INSERT INTO events VALUES('019fcb80-ffff-7000-8000-000000000001',"
            "'2026/08/04/019fcb80-ffff-7000-8000-000000000001');"
            "INSERT INTO upload_jobs(event_id,state,updated_at_utc_ms) VALUES("
            "'019fcb80-ffff-7000-8000-000000000001','Pending',1);"
            "PRAGMA user_version=1;");
    }
    auto opened = EventMetadataDatabase::open(options);
    ASSERT_TRUE(opened) << opened.error().business_code;
    EXPECT_TRUE(opened.value()->open_report().migrated);
    ASSERT_TRUE(opened.value()->open_report().migration_backup.has_value());
    const auto backup_path = *opened.value()->open_report().migration_backup;
    EXPECT_TRUE(std::filesystem::is_regular_file(backup_path));
    opened.value().reset();

    RawDatabase migrated{options.database_path};
    EXPECT_EQ(migrated.scalar_int64("PRAGMA user_version"), database_schema_version);
    EXPECT_EQ(migrated.scalar_int64("SELECT COUNT(*) FROM event_retention"), 1);
    EXPECT_EQ(migrated.scalar_int64("SELECT deletion_allowed FROM event_retention WHERE event_id="
                                    "'019fcb80-ffff-7000-8000-000000000001'"),
              0);
    RawDatabase backup{backup_path};
    EXPECT_EQ(backup.scalar_int64("PRAGMA user_version"), 1);
}

TEST(StorageMetadataDatabase, RejectsUnsupportedAndCorruptDatabasesAndRestoresBackup)
{
    TemporaryDirectory unsupported_temporary{"unsupported"};
    const auto unsupported_options = database_options(unsupported_temporary);
    std::filesystem::create_directories(unsupported_options.database_path.parent_path());
    {
        RawDatabase raw{unsupported_options.database_path};
        raw.execute("PRAGMA user_version=" + std::to_string(database_schema_version + 1U) + ";");
    }
    auto unsupported = EventMetadataDatabase::open(unsupported_options);
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().business_code, "DATABASE_SCHEMA_UNSUPPORTED");

    TemporaryDirectory temporary{"restore"};
    const auto options = database_options(temporary);
    const auto backup_path = temporary.path() / L"手工 备份.db";
    auto opened = EventMetadataDatabase::open(options);
    ASSERT_TRUE(opened);
    ASSERT_TRUE(opened.value()->backup_to(backup_path));
    auto refused_overwrite = opened.value()->backup_to(backup_path);
    ASSERT_FALSE(refused_overwrite);
    EXPECT_EQ(refused_overwrite.error().business_code, "DATABASE_ERROR");
    opened.value().reset();
    {
        std::ofstream corrupt{options.database_path, std::ios::binary | std::ios::trunc};
        corrupt << "not-a-sqlite-database";
    }
    auto corrupt = EventMetadataDatabase::open(options);
    ASSERT_FALSE(corrupt);
    EXPECT_EQ(corrupt.error().business_code, "DATABASE_CORRUPT");
    ASSERT_TRUE(EventMetadataDatabase::restore_backup(options.database_path, backup_path));
    EXPECT_TRUE(std::filesystem::is_regular_file(backup_path));
    auto restored = EventMetadataDatabase::open(options);
    ASSERT_TRUE(restored) << restored.error().business_code;
    EXPECT_EQ(restored.value()->open_report().schema_version, database_schema_version);
}

TEST(StorageMetadataDatabase, IndexesAndQueriesByStablePageTimeStateAndCamera)
{
    TemporaryDirectory temporary{"query"};
    const auto options = database_options(temporary);
    auto database = EventMetadataDatabase::open(options);
    ASSERT_TRUE(database);
    const auto first = persist_event(options, event_request("019fcb3d-1111-7000-8000-000000000001",
                                                            "CAM01", "Confirmed", 100ms));
    const auto second = persist_event(options, event_request("019fcb3d-2222-7000-8000-000000000002",
                                                             "CAM02", "Rejected", 2100ms));
    ASSERT_TRUE(database.value()->index_committed_event(first.committed_directory));
    ASSERT_TRUE(database.value()->index_committed_event(second.committed_directory));

    auto newest = database.value()->query_events({.limit = 1U});
    ASSERT_TRUE(newest);
    ASSERT_EQ(newest.value().total, 2U);
    ASSERT_EQ(newest.value().events.size(), 1U);
    EXPECT_EQ(newest.value().events.front().event_id, second.event_id);
    EXPECT_EQ(newest.value().events.front().camera_ids, std::vector<std::string>{"CAM02"});
    auto older = database.value()->query_events({.offset = 1U, .limit = 1U});
    ASSERT_TRUE(older);
    ASSERT_EQ(older.value().events.size(), 1U);
    EXPECT_EQ(older.value().events.front().event_id, first.event_id);

    auto state = database.value()->query_events({.event_state = "Confirmed", .limit = 10U});
    ASSERT_TRUE(state);
    ASSERT_EQ(state.value().events.size(), 1U);
    EXPECT_EQ(state.value().events.front().event_id, first.event_id);
    auto camera = database.value()->query_events({.camera_id = "CAM02", .limit = 10U});
    ASSERT_TRUE(camera);
    ASSERT_EQ(camera.value().events.size(), 1U);
    EXPECT_EQ(camera.value().events.front().event_id, second.event_id);
    auto time = database.value()->query_events(
        {.start_time_utc_ms = newest.value().events.front().candidate_time_utc_ms,
         .end_time_utc_ms = newest.value().events.front().candidate_time_utc_ms,
         .limit = 10U});
    ASSERT_TRUE(time);
    ASSERT_EQ(time.value().events.size(), 1U);
    EXPECT_EQ(time.value().events.front().event_id, second.event_id);
    auto invalid = database.value()->query_events({.limit = database_maximum_page_size + 1U});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().business_code, "SYS_CONFIG_INVALID");

    RawDatabase raw{options.database_path};
    EXPECT_EQ(raw.scalar_int64("SELECT COUNT(*) FROM event_cameras"), 2);
    EXPECT_EQ(raw.scalar_int64("SELECT COUNT(*) FROM key_frames"), 2);
    EXPECT_EQ(raw.scalar_int64("SELECT COUNT(*) FROM event_files"), 8);
}

TEST(StorageMetadataDatabase, ReviewsWithOptimisticConcurrencyAndReconcilePreservesDecision)
{
    TemporaryDirectory temporary{"review"};
    const auto options = database_options(temporary);
    auto database = EventMetadataDatabase::open(options);
    ASSERT_TRUE(database);
    const auto persisted =
        persist_event(options, event_request("019fcb3d-7777-7000-8000-000000000007", "CAM01",
                                             "Candidate", 7100ms));
    ASSERT_TRUE(database.value()->index_committed_event(persisted.committed_directory));
    auto initial = database.value()->get_event(persisted.event_id);
    ASSERT_TRUE(initial);
    EXPECT_EQ(initial.value().review_revision, 1U);
    EXPECT_EQ(initial.value().event_state, "Candidate");

    auto confirmed = database.value()->review_event(
        persisted.event_id, 1U, EventReviewDecision::confirmed, 1234567, "S-1-5-21-operator");
    ASSERT_TRUE(confirmed);
    EXPECT_FALSE(confirmed.value().duplicate);
    EXPECT_EQ(confirmed.value().event.event_state, "Confirmed");
    EXPECT_EQ(confirmed.value().event.review_revision, 2U);
    EXPECT_EQ(confirmed.value().event.reviewed_by, "S-1-5-21-operator");

    auto duplicate = database.value()->review_event(
        persisted.event_id, 1U, EventReviewDecision::confirmed, 1234568, "S-1-5-21-operator");
    ASSERT_TRUE(duplicate);
    EXPECT_TRUE(duplicate.value().duplicate);
    EXPECT_EQ(duplicate.value().event.review_revision, 2U);
    auto conflict = database.value()->review_event(
        persisted.event_id, 1U, EventReviewDecision::rejected, 1234569, "S-1-5-21-other");
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().business_code, "EVENT_VERSION_CONFLICT");

    ASSERT_TRUE(database.value()->reconcile());
    auto after_reconcile = database.value()->get_event(persisted.event_id);
    ASSERT_TRUE(after_reconcile);
    EXPECT_EQ(after_reconcile.value().event_state, "Confirmed");
    EXPECT_EQ(after_reconcile.value().review_revision, 2U);
}

TEST(StorageMetadataDatabase, SerializesCompetingReviewUpdates)
{
    TemporaryDirectory temporary{"review-race"};
    const auto options = database_options(temporary);
    auto database = EventMetadataDatabase::open(options);
    ASSERT_TRUE(database);
    const auto persisted =
        persist_event(options, event_request("019fcb3d-8888-7000-8000-000000000008", "CAM01",
                                             "Candidate", 8100ms));
    ASSERT_TRUE(database.value()->index_committed_event(persisted.committed_directory));
    std::array<bool, 2U> succeeded{};
    std::array<std::string, 2U> errors;
    std::thread confirm_thread{[&] {
        auto result = database.value()->review_event(
            persisted.event_id, 1U, EventReviewDecision::confirmed, 2234567, "operator-a");
        succeeded[0] = result.has_value();
        if (!result)
            errors[0] = result.error().business_code;
    }};
    std::thread reject_thread{[&] {
        auto result = database.value()->review_event(
            persisted.event_id, 1U, EventReviewDecision::rejected, 2234567, "operator-b");
        succeeded[1] = result.has_value();
        if (!result)
            errors[1] = result.error().business_code;
    }};
    confirm_thread.join();
    reject_thread.join();
    EXPECT_NE(succeeded[0], succeeded[1]);
    EXPECT_TRUE(errors[0] == "EVENT_VERSION_CONFLICT" || errors[1] == "EVENT_VERSION_CONFLICT");
    auto final = database.value()->get_event(persisted.event_id);
    ASSERT_TRUE(final);
    EXPECT_EQ(final.value().review_revision, 2U);
    EXPECT_TRUE(final.value().event_state == "Confirmed" ||
                final.value().event_state == "Rejected");
}

TEST(StorageMetadataDatabase, ReconcilesDirectoryOnlyAndDatabaseOnlyEventsWithoutDeletingRows)
{
    TemporaryDirectory temporary{"reconcile"};
    const auto options = database_options(temporary);
    auto database = EventMetadataDatabase::open(options);
    ASSERT_TRUE(database);
    const auto directory_only =
        persist_event(options, event_request("019fcb3d-3333-7000-8000-000000000003", "CAM01",
                                             "Confirmed", 3100ms));
    const auto database_only =
        persist_event(options, event_request("019fcb3d-4444-7000-8000-000000000004", "CAM02",
                                             "Confirmed", 4100ms));
    ASSERT_TRUE(database.value()->index_committed_event(database_only.committed_directory));
    const auto holding = temporary.path() / L"暂存" / database_only.event_id;
    std::filesystem::create_directories(holding.parent_path());
    std::filesystem::rename(database_only.committed_directory, holding);

    auto reconciled = database.value()->reconcile();
    ASSERT_TRUE(reconciled) << reconciled.error().business_code;
    EXPECT_EQ(reconciled.value().directories_scanned, 1U);
    EXPECT_EQ(reconciled.value().indexed, 1U);
    EXPECT_EQ(reconciled.value().marked_missing, 1U);
    auto rows = database.value()->query_events({.limit = 10U});
    ASSERT_TRUE(rows);
    ASSERT_EQ(rows.value().events.size(), 2U);
    const auto missing = std::ranges::find_if(rows.value().events, [&](const auto& event) {
        return event.event_id == database_only.event_id;
    });
    ASSERT_NE(missing, rows.value().events.end());
    EXPECT_EQ(missing->storage_state, "Missing");

    std::filesystem::create_directories(database_only.committed_directory.parent_path());
    std::filesystem::rename(holding, database_only.committed_directory);
    auto repeated = database.value()->reconcile();
    ASSERT_TRUE(repeated);
    EXPECT_EQ(repeated.value().directories_scanned, 2U);
    EXPECT_EQ(repeated.value().indexed, 0U);
    EXPECT_EQ(repeated.value().refreshed, 2U);
    EXPECT_EQ(repeated.value().marked_missing, 0U);
    auto present = database.value()->query_events({.camera_id = "CAM02", .limit = 10U});
    ASSERT_TRUE(present);
    ASSERT_EQ(present.value().events.size(), 1U);
    EXPECT_EQ(present.value().events.front().storage_state, "Present");
}

TEST(StorageMetadataDatabase, RejectsDamagedManifestWithoutPartialIndex)
{
    TemporaryDirectory temporary{"damaged"};
    const auto options = database_options(temporary);
    auto database = EventMetadataDatabase::open(options);
    ASSERT_TRUE(database);
    const auto persisted =
        persist_event(options, event_request("019fcb3d-5555-7000-8000-000000000005", "CAM01",
                                             "Confirmed", 5100ms));
    std::filesystem::path raw_path;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator{persisted.committed_directory / "raw"})
    {
        if (entry.is_regular_file() && entry.path().extension() == ".raw")
        {
            raw_path = entry.path();
            break;
        }
    }
    ASSERT_FALSE(raw_path.empty());
    {
        std::ofstream raw_file{raw_path, std::ios::binary | std::ios::app};
        raw_file << "damaged";
    }
    auto indexed = database.value()->index_committed_event(persisted.committed_directory);
    ASSERT_FALSE(indexed);
    EXPECT_EQ(indexed.error().business_code, "DATABASE_RECONCILE_FAILED");
    auto rows = database.value()->query_events({.limit = 10U});
    ASSERT_TRUE(rows);
    EXPECT_EQ(rows.value().total, 0U);
}

TEST(StorageMetadataDatabase, RejectsMalformedVerifiedManifestWithoutPartialIndex)
{
    TemporaryDirectory temporary{"malformed"};
    const auto options = database_options(temporary);
    auto database = EventMetadataDatabase::open(options);
    ASSERT_TRUE(database);
    const auto persisted =
        persist_event(options, event_request("019fcb3d-6666-7000-8000-000000000006", "CAM01",
                                             "Confirmed", 6100ms));
    const auto manifest_path = persisted.committed_directory / "manifest.json";
    nlohmann::json manifest;
    {
        std::ifstream input{manifest_path};
        input >> manifest;
    }
    manifest.at("keyFrames").at(0).erase("width");
    {
        std::ofstream output{manifest_path, std::ios::trunc};
        output << manifest.dump(2) << '\n';
    }
    auto indexed = database.value()->index_committed_event(persisted.committed_directory);
    ASSERT_FALSE(indexed);
    EXPECT_EQ(indexed.error().business_code, "DATABASE_RECONCILE_FAILED");
    auto rows = database.value()->query_events({.limit = 10U});
    ASSERT_TRUE(rows);
    EXPECT_EQ(rows.value().total, 0U);
}

} // namespace
