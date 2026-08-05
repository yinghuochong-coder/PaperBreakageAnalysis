#include "paperbreak/storage/metadata_database.hpp"
#include "paperbreak/uplink/upload_scheduler.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using paperbreak::storage::EventMetadataDatabase;
using paperbreak::storage::MetadataDatabaseOptions;
using paperbreak::storage::UploadFailureClass;
using paperbreak::storage::UploadJobKind;
using paperbreak::storage::UploadJobRequest;
using paperbreak::storage::UploadJobState;
using paperbreak::uplink::PersistentUploadScheduler;
using paperbreak::uplink::PersistentUploadSchedulerConfig;
using paperbreak::uplink::UploadAttemptDisposition;
using paperbreak::uplink::UploadAttemptOutcome;

class TemporaryUploadDirectory final
{
  public:
    explicit TemporaryUploadDirectory(const std::string_view suffix)
    {
        static std::atomic_uint64_t sequence{};
        path_ = std::filesystem::temp_directory_path() /
                ("paperbreak-m8-03-" + std::string{suffix} + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(++sequence));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryUploadDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

MetadataDatabaseOptions options_for(const TemporaryUploadDirectory& temporary,
                                    const std::size_t jobs = 32U, const std::uint64_t bytes = 1024U)
{
    return {.database_path = temporary.path() / "metadata.db",
            .event_root = temporary.path() / "events",
            .backup_directory = temporary.path() / "backups",
            .upload_job_capacity = jobs,
            .upload_pending_byte_capacity = bytes};
}

UploadJobRequest alarm_job(const std::string& key, const std::int64_t created,
                           const std::uint64_t bytes = 1U)
{
    return {.idempotency_key = key,
            .kind = UploadJobKind::alarm_metadata,
            .logical_id = key,
            .payload_json = "{\"alarm\":\"" + key + "\"}",
            .upload_bytes = bytes,
            .created_at_utc_ms = created};
}

void execute_sql(const std::filesystem::path& path, const std::string& sql)
{
    sqlite3* database = nullptr;
    ASSERT_EQ(sqlite3_open16(path.c_str(), &database), SQLITE_OK);
    char* message = nullptr;
    const auto result = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &message);
    const std::string detail = message == nullptr ? std::string{} : message;
    if (message != nullptr)
        sqlite3_free(message);
    EXPECT_EQ(result, SQLITE_OK) << detail;
    EXPECT_EQ(sqlite3_close_v2(database), SQLITE_OK);
}

void insert_event(const MetadataDatabaseOptions& options, const std::string& event_id)
{
    execute_sql(options.database_path,
                "INSERT INTO events(event_id,event_schema_version,event_state,"
                "candidate_time_utc_ms,confirmed_time_utc_ms,start_time_utc_ms,end_time_utc_ms,"
                "trigger_camera_id,trigger_frame_number,trigger_reason,confidence,pre_event_ms,"
                "post_event_ms,algorithm_name,algorithm_version,config_version,machine_id,"
                "production_line_id,paper_type,paper_speed,upload_state,time_quality,"
                "relative_directory,storage_state,window_complete,truncated_by_maximum_duration,"
                "stopped_early,indexed_at_utc_ms) VALUES('" +
                    event_id +
                    "',1,'Confirmed',1,2,0,3,'CAM01',1,'test',0.9,1,1,'mock','1','1',"
                    "'machine','line','paper',NULL,'NotUploaded','Good','2026/08/05/" +
                    event_id +
                    "','Present',1,0,0,1);"
                    "INSERT INTO event_retention(event_id) VALUES('" +
                    event_id + "')");
}

UploadJobRequest event_job(const std::string& event_id, const UploadJobKind kind,
                           const std::int64_t created)
{
    const auto name = std::string{paperbreak::storage::upload_job_kind_name(kind)};
    return {.idempotency_key = "event:" + event_id + ":" + name,
            .event_id = event_id,
            .kind = kind,
            .logical_id = name,
            .relative_path = event_id + "/" + name,
            .payload_json = "{}",
            .checksum = "sha256",
            .upload_bytes = 1U,
            .created_at_utc_ms = created};
}

template <typename Predicate>
bool wait_until(Predicate predicate, const std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

TEST(UplinkUploadRepository, EnforcesDualCapacityAndIdempotency)
{
    TemporaryUploadDirectory temporary{"capacity"};
    auto options = options_for(temporary, 2U, 10U);
    options.upload_job_history_capacity = 2U;
    auto opened = EventMetadataDatabase::open(options);
    ASSERT_TRUE(opened);
    auto& database = *opened.value();

    auto first = database.enqueue_upload_job(alarm_job("alarm-a", 10, 4U));
    ASSERT_TRUE(first);
    EXPECT_FALSE(first.value().duplicate);
    auto duplicate = database.enqueue_upload_job(alarm_job("alarm-a", 10, 4U));
    ASSERT_TRUE(duplicate);
    EXPECT_TRUE(duplicate.value().duplicate);
    auto conflict = alarm_job("alarm-a", 10, 4U);
    conflict.payload_json = "{\"alarm\":\"changed\"}";
    auto conflicted = database.enqueue_upload_job(conflict);
    ASSERT_FALSE(conflicted);
    EXPECT_EQ(conflicted.error().business_code, "UPLOAD_JOB_CONFLICT");

    ASSERT_TRUE(database.enqueue_upload_job(alarm_job("alarm-b", 11, 6U)));
    auto count_full = database.enqueue_upload_job(alarm_job("alarm-c", 12, 1U));
    ASSERT_FALSE(count_full);
    EXPECT_EQ(count_full.error().business_code, "UPLOAD_ENQUEUE_FAILED");

    auto claimed = database.claim_next_upload_job(20);
    ASSERT_TRUE(claimed);
    ASSERT_TRUE(claimed.value());
    ASSERT_TRUE(database.complete_upload_job(claimed.value()->job_id, "{}", 21));
    ASSERT_TRUE(database.enqueue_upload_job(alarm_job("alarm-c", 22, 4U)));
    auto byte_full = database.enqueue_upload_job(alarm_job("alarm-d", 23, 1U));
    ASSERT_FALSE(byte_full);
    EXPECT_EQ(byte_full.error().business_code, "UPLOAD_ENQUEUE_FAILED");

    auto stats = database.upload_queue_stats();
    ASSERT_TRUE(stats);
    EXPECT_EQ(stats.value().active_jobs, 2U);
    EXPECT_EQ(stats.value().active_bytes, 10U);
    EXPECT_EQ(stats.value().completed_jobs, 0U);
    auto pruned = database.get_upload_job("alarm-a");
    ASSERT_TRUE(pruned);
    EXPECT_FALSE(pruned.value());
}

TEST(UplinkUploadRepository, MigratesLegacyInProgressJobAndPreservesTheEvent)
{
    TemporaryUploadDirectory temporary{"migration"};
    const auto options = options_for(temporary);
    auto opened = EventMetadataDatabase::open(options);
    ASSERT_TRUE(opened);
    insert_event(options, "event-migrate");
    ASSERT_TRUE(opened.value()->enqueue_upload_job(
        event_job("event-migrate", UploadJobKind::manifest, 100)));
    opened.value().reset();

    execute_sql(options.database_path, R"sql(
DROP INDEX idx_upload_jobs_schedule;
DROP INDEX idx_upload_jobs_event;
ALTER TABLE upload_jobs RENAME TO upload_jobs_v4;
CREATE TABLE upload_jobs(
  job_id INTEGER PRIMARY KEY,
  event_id TEXT NOT NULL REFERENCES events(event_id) ON DELETE CASCADE,
  state TEXT NOT NULL,
  attempts INTEGER NOT NULL DEFAULT 0 CHECK(attempts >= 0),
  next_attempt_utc_ms INTEGER,
  checkpoint_json TEXT NOT NULL DEFAULT '{}',
  last_error_code TEXT,
  updated_at_utc_ms INTEGER NOT NULL,
  UNIQUE(event_id)
) STRICT;
INSERT INTO upload_jobs(job_id,event_id,state,attempts,next_attempt_utc_ms,checkpoint_json,
 last_error_code,updated_at_utc_ms)
 SELECT job_id,event_id,'InProgress',attempts,next_attempt_utc_ms,checkpoint_json,
        last_error_code,updated_at_utc_ms FROM upload_jobs_v4;
DROP TABLE upload_jobs_v4;
CREATE INDEX idx_upload_jobs_state_time ON upload_jobs(state,next_attempt_utc_ms);
PRAGMA user_version=3;
)sql");

    auto migrated = EventMetadataDatabase::open(options);
    ASSERT_TRUE(migrated);
    EXPECT_TRUE(migrated.value()->open_report().migrated);
    EXPECT_EQ(migrated.value()->open_report().schema_version,
              paperbreak::storage::database_schema_version);
    ASSERT_TRUE(migrated.value()->open_report().migration_backup);
    auto legacy = migrated.value()->get_upload_job("legacy-event:event-migrate");
    ASSERT_TRUE(legacy);
    ASSERT_TRUE(legacy.value());
    EXPECT_EQ(legacy.value()->state, UploadJobState::retry_wait);
    EXPECT_EQ(legacy.value()->kind, UploadJobKind::manifest);
    auto event = migrated.value()->get_event("event-migrate");
    ASSERT_TRUE(event);
}

TEST(UplinkUploadRepository, ClaimsFiveKindsByFixedPriorityWithoutCreatingEvents)
{
    TemporaryUploadDirectory temporary{"priority"};
    const auto options = options_for(temporary);
    auto opened = EventMetadataDatabase::open(options);
    ASSERT_TRUE(opened);
    insert_event(options, "event-priority");
    auto& database = *opened.value();

    const std::vector kinds{UploadJobKind::raw_file, UploadJobKind::low_rate_replay,
                            UploadJobKind::manifest, UploadJobKind::key_frame};
    for (const auto kind : kinds)
        ASSERT_TRUE(database.enqueue_upload_job(event_job("event-priority", kind, 100)));
    ASSERT_TRUE(database.enqueue_upload_job(alarm_job("alarm-priority", 100)));

    const std::vector expected{UploadJobKind::alarm_metadata, UploadJobKind::key_frame,
                               UploadJobKind::manifest, UploadJobKind::low_rate_replay,
                               UploadJobKind::raw_file};
    for (const auto kind : expected)
    {
        auto claimed = database.claim_next_upload_job(100);
        ASSERT_TRUE(claimed);
        ASSERT_TRUE(claimed.value());
        EXPECT_EQ(claimed.value()->kind, kind);
        ASSERT_TRUE(database.complete_upload_job(claimed.value()->job_id, "{}", 101));
    }
    auto event = database.get_event("event-priority");
    ASSERT_TRUE(event);
    EXPECT_EQ(event.value().upload_state, "Uploaded");
}

TEST(UplinkUploadRepository, RecoversAndKeepsFailureClassesTerminalUntilManualRetry)
{
    TemporaryUploadDirectory temporary{"recovery"};
    const auto options = options_for(temporary);
    auto opened = EventMetadataDatabase::open(options);
    ASSERT_TRUE(opened);
    ASSERT_TRUE(opened.value()->enqueue_upload_job(alarm_job("recover", 10)));

    auto claimed = opened.value()->claim_next_upload_job(10);
    ASSERT_TRUE(claimed);
    ASSERT_TRUE(claimed.value());
    const auto job_id = claimed.value()->job_id;
    opened.value().reset();
    opened = EventMetadataDatabase::open(options);
    ASSERT_TRUE(opened);
    auto& database = *opened.value();
    auto recovery = database.recover_upload_jobs(20);
    ASSERT_TRUE(recovery);
    EXPECT_EQ(recovery.value(), 1U);
    auto recovered = database.get_upload_job("recover");
    ASSERT_TRUE(recovered);
    ASSERT_TRUE(recovered.value());
    EXPECT_EQ(recovered.value()->state, UploadJobState::retry_wait);
    EXPECT_EQ(recovered.value()->last_error_code, "UPLOAD_TRANSFER_INTERRUPTED");

    claimed = database.claim_next_upload_job(20);
    ASSERT_TRUE(claimed.value());
    EXPECT_EQ(claimed.value()->job_id, job_id);
    ASSERT_TRUE(database.fail_upload_job(claimed.value()->job_id, UploadFailureClass::retryable,
                                         "UPLOAD_TRANSFER_FAILED", "{\"offset\":1}", 30, 20));
    EXPECT_FALSE(database.claim_next_upload_job(29).value());
    claimed = database.claim_next_upload_job(30);
    ASSERT_TRUE(claimed.value());
    ASSERT_TRUE(database.fail_upload_job(claimed.value()->job_id, UploadFailureClass::permanent,
                                         "UPLOAD_REJECTED", "{}", std::nullopt, 31));
    EXPECT_FALSE(database.claim_next_upload_job(100).value());
    ASSERT_TRUE(database.retry_upload_job(claimed.value()->job_id, 101));
    claimed = database.claim_next_upload_job(101);
    ASSERT_TRUE(claimed.value());
    ASSERT_TRUE(database.fail_upload_job(claimed.value()->job_id,
                                         UploadFailureClass::manual_intervention,
                                         "UPLOAD_CHECKSUM_MISMATCH", "{}", std::nullopt, 102));
    auto terminal = database.get_upload_job("recover");
    ASSERT_TRUE(terminal.value());
    EXPECT_EQ(terminal.value()->state, UploadJobState::manual_intervention);
}

TEST(UplinkUploadScheduler, AppliesCappedJitterAndBackfillsInPriorityOrder)
{
    PersistentUploadSchedulerConfig delay_config{.initial_retry_delay = 100ms,
                                                 .maximum_retry_delay = 250ms,
                                                 .idle_poll_interval = 1ms,
                                                 .jitter_ratio = 0.2,
                                                 .maximum_attempts = 3U};
    EXPECT_EQ(paperbreak::uplink::upload_retry_delay(delay_config, 1U, 0.0), 80ms);
    EXPECT_EQ(paperbreak::uplink::upload_retry_delay(delay_config, 1U, 1.0), 120ms);
    EXPECT_EQ(paperbreak::uplink::upload_retry_delay(delay_config, 8U, 1.0), 250ms);

    TemporaryUploadDirectory temporary{"scheduler"};
    const auto options = options_for(temporary);
    auto unique = EventMetadataDatabase::open(options);
    ASSERT_TRUE(unique);
    std::shared_ptr<EventMetadataDatabase> database{std::move(unique).value()};
    insert_event(options, "event-scheduler");
    ASSERT_TRUE(
        database->enqueue_upload_job(event_job("event-scheduler", UploadJobKind::raw_file, 100)));
    ASSERT_TRUE(database->enqueue_upload_job(
        event_job("event-scheduler", UploadJobKind::low_rate_replay, 100)));
    ASSERT_TRUE(
        database->enqueue_upload_job(event_job("event-scheduler", UploadJobKind::manifest, 100)));
    ASSERT_TRUE(
        database->enqueue_upload_job(event_job("event-scheduler", UploadJobKind::key_frame, 100)));
    ASSERT_TRUE(database->enqueue_upload_job(alarm_job("alarm-scheduler", 100)));

    std::atomic_uint32_t calls{};
    std::mutex order_mutex;
    std::vector<UploadJobKind> completed_order;
    auto created = PersistentUploadScheduler::create(
        database,
        {.initial_retry_delay = 500ms,
         .maximum_retry_delay = 500ms,
         .idle_poll_interval = 1ms,
         .jitter_ratio = 0.0,
         .maximum_attempts = 3U},
        [&](const paperbreak::storage::UploadJobRecord& job, std::stop_token) {
            if (++calls <= 5U)
                return UploadAttemptOutcome{.disposition =
                                                UploadAttemptDisposition::retryable_failure,
                                            .checkpoint_json = "{\"offline\":true}",
                                            .error_code = "UPLOAD_TRANSFER_FAILED"};
            std::lock_guard lock{order_mutex};
            completed_order.push_back(job.kind);
            return UploadAttemptOutcome{};
        });
    ASSERT_TRUE(created);
    auto scheduler = std::move(created).value();
    ASSERT_TRUE(scheduler->start());
    ASSERT_TRUE(wait_until(
        [&] {
            auto stats = database->upload_queue_stats();
            return stats && stats.value().completed_jobs == 5U;
        },
        4s));
    scheduler->request_stop();
    ASSERT_TRUE(scheduler->join(std::chrono::steady_clock::now() + 1s));

    const std::vector expected{UploadJobKind::alarm_metadata, UploadJobKind::key_frame,
                               UploadJobKind::manifest, UploadJobKind::low_rate_replay,
                               UploadJobKind::raw_file};
    EXPECT_EQ(completed_order, expected);
    const auto snapshot = scheduler->snapshot();
    EXPECT_EQ(snapshot.claimed_jobs, 10U);
    EXPECT_EQ(snapshot.retryable_failures, 5U);
    EXPECT_EQ(snapshot.completed_jobs, 5U);
}

TEST(UplinkUploadScheduler, ExhaustsRetriesAndStopsAnInFlightExecutorDeterministically)
{
    TemporaryUploadDirectory temporary{"stop"};
    auto unique = EventMetadataDatabase::open(options_for(temporary));
    ASSERT_TRUE(unique);
    std::shared_ptr<EventMetadataDatabase> database{std::move(unique).value()};
    ASSERT_TRUE(database->enqueue_upload_job(alarm_job("exhaust", 1)));
    auto exhausted = PersistentUploadScheduler::create(
        database,
        {.initial_retry_delay = 1ms,
         .maximum_retry_delay = 1ms,
         .idle_poll_interval = 1ms,
         .jitter_ratio = 0.0,
         .maximum_attempts = 2U},
        [](const paperbreak::storage::UploadJobRecord&, std::stop_token) {
            return UploadAttemptOutcome{.disposition = UploadAttemptDisposition::retryable_failure,
                                        .error_code = "UPLOAD_TRANSFER_FAILED"};
        });
    ASSERT_TRUE(exhausted);
    auto scheduler = std::move(exhausted).value();
    ASSERT_TRUE(scheduler->start());
    ASSERT_TRUE(wait_until([&] {
        auto job = database->get_upload_job("exhaust");
        return job && job.value() && job.value()->state == UploadJobState::manual_intervention;
    }));
    auto job = database->get_upload_job("exhaust");
    ASSERT_TRUE(job.value());
    EXPECT_EQ(job.value()->attempts, 2U);
    EXPECT_EQ(job.value()->last_error_code, "UPLOAD_RETRY_EXHAUSTED");
    scheduler->request_stop();
    ASSERT_TRUE(scheduler->join(std::chrono::steady_clock::now() + 1s));

    ASSERT_TRUE(database->enqueue_upload_job(alarm_job("stop-token", 2)));
    std::atomic_bool entered{};
    auto stopping = PersistentUploadScheduler::create(
        database,
        {.initial_retry_delay = 1ms,
         .maximum_retry_delay = 1ms,
         .idle_poll_interval = 1ms,
         .jitter_ratio = 0.0,
         .maximum_attempts = 2U},
        [&](const paperbreak::storage::UploadJobRecord&, const std::stop_token token) {
            entered = true;
            while (!token.stop_requested())
                std::this_thread::yield();
            return UploadAttemptOutcome{.disposition = UploadAttemptDisposition::retryable_failure,
                                        .error_code = "UPLOAD_TRANSFER_INTERRUPTED"};
        });
    ASSERT_TRUE(stopping);
    auto stopping_scheduler = std::move(stopping).value();
    ASSERT_TRUE(stopping_scheduler->start());
    ASSERT_TRUE(wait_until([&] { return entered.load(); }));
    stopping_scheduler->request_stop();
    ASSERT_TRUE(stopping_scheduler->join(std::chrono::steady_clock::now() + 1s));
    auto stopped = database->get_upload_job("stop-token");
    ASSERT_TRUE(stopped.value());
    EXPECT_EQ(stopped.value()->state, UploadJobState::retry_wait);
}

TEST(UplinkUploadScheduler, PersistsPermanentAndManualExecutorClassifications)
{
    TemporaryUploadDirectory temporary{"classification"};
    auto unique = EventMetadataDatabase::open(options_for(temporary, 2U, 1024U));
    ASSERT_TRUE(unique);
    std::shared_ptr<EventMetadataDatabase> database{std::move(unique).value()};
    ASSERT_TRUE(database->enqueue_upload_job(alarm_job("manual", 1)));
    ASSERT_TRUE(database->enqueue_upload_job(alarm_job("permanent", 2)));

    auto created = PersistentUploadScheduler::create(
        database,
        {.initial_retry_delay = 1ms,
         .maximum_retry_delay = 1ms,
         .idle_poll_interval = 1ms,
         .jitter_ratio = 0.0,
         .maximum_attempts = 2U},
        [](const paperbreak::storage::UploadJobRecord& job, std::stop_token) {
            return job.idempotency_key == "permanent"
                       ? UploadAttemptOutcome{.disposition =
                                                  UploadAttemptDisposition::permanent_failure,
                                              .error_code = "UPLOAD_REJECTED"}
                       : UploadAttemptOutcome{.disposition =
                                                  UploadAttemptDisposition::manual_intervention,
                                              .error_code = "UPLOAD_CHECKSUM_MISMATCH"};
        });
    ASSERT_TRUE(created);
    auto scheduler = std::move(created).value();
    ASSERT_TRUE(scheduler->start());
    ASSERT_TRUE(wait_until([&] {
        auto stats = database->upload_queue_stats();
        return stats && stats.value().permanent_failed_jobs == 1U &&
               stats.value().manual_intervention_jobs == 1U;
    }));
    scheduler->request_stop();
    ASSERT_TRUE(scheduler->join(std::chrono::steady_clock::now() + 1s));
    auto permanent = database->get_upload_job("permanent");
    auto manual = database->get_upload_job("manual");
    ASSERT_TRUE(permanent);
    ASSERT_TRUE(manual);
    ASSERT_TRUE(permanent.value());
    ASSERT_TRUE(manual.value());
    EXPECT_EQ(permanent.value()->state, UploadJobState::permanent_failed);
    EXPECT_EQ(manual.value()->state, UploadJobState::manual_intervention);
    EXPECT_EQ(scheduler->snapshot().permanent_failures, 1U);
    EXPECT_EQ(scheduler->snapshot().manual_interventions, 1U);
    auto retained_capacity = database->enqueue_upload_job(alarm_job("blocked-by-failures", 3));
    ASSERT_FALSE(retained_capacity);
    EXPECT_EQ(retained_capacity.error().business_code, "UPLOAD_ENQUEUE_FAILED");
}

} // namespace
