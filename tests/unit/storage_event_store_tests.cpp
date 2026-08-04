#include "paperbreak/storage/event_store.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
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
                (L"PaperBreak-M5-06-中文-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(sequence.fetch_add(1U)) + L"-" +
                 std::filesystem::path{suffix}.wstring());
        std::error_code error;
        std::filesystem::create_directories(path_, error);
        if (error)
            throw std::runtime_error{"failed to create test directory"};
    }

    ~TemporaryDirectory()
    {
        if (path_.filename().wstring().starts_with(L"PaperBreak-M5-06-"))
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

EventPersistenceRequest request(const std::string& event_id)
{
    const auto wall_base = WallClockTime{std::chrono::sys_days{std::chrono::year{2026} / 8 / 4}};
    auto first = frame("CAM01", 1U, 100ms);
    auto second = frame("CAM01", 2U, 200ms);
    KeyFrameDescriptor descriptor{
        .camera_id = "CAM01",
        .camera_frame_number = second.camera_frame_number(),
        .sequence_number = second.sequence_number(),
        .monotonic_time = second.received_monotonic_time(),
        .wall_clock_time = second.received_wall_clock_time(),
        .geometry = second.geometry(),
        .pixel_format = second.pixel_format(),
        .reasons = {KeyFrameReason::candidate_trigger, KeyFrameReason::highest_confidence}};
    EventPersistenceRequest value;
    value.metadata = {.event_id = event_id,
                      .event_state = "Confirmed",
                      .candidate_time = wall_base + 200ms,
                      .confirmed_time = wall_base + 250ms,
                      .start_time = wall_base + 100ms,
                      .end_time = wall_base + 300ms,
                      .camera_ids = {"CAM01"},
                      .trigger_camera_id = "CAM01",
                      .trigger_frame_number = 102U,
                      .trigger_reason = "ManualTest",
                      .confidence = 0.875,
                      .pre_event_duration = 100ms,
                      .post_event_duration = 100ms,
                      .algorithm_name = "mock-detector",
                      .algorithm_version = "m5",
                      .config_version = "42",
                      .machine_id = "PM-01",
                      .production_line_id = "LINE-A",
                      .paper_type = "test-paper",
                      .paper_speed = 900.5,
                      .upload_state = "Pending",
                      .time_quality = "Normal"};
    value.window = {.event_id = event_id,
                    .version = 3U,
                    .requested_start = MonotonicTime{100ms},
                    .requested_end = MonotonicTime{300ms},
                    .closed_monotonic_time = MonotonicTime{301ms},
                    .display_wall_clock_time = wall_base + 200ms,
                    .camera_windows = {{.camera_id = "CAM01",
                                        .requested_start = MonotonicTime{100ms},
                                        .requested_end = MonotonicTime{300ms},
                                        .available_start = MonotonicTime{100ms},
                                        .available_end = MonotonicTime{200ms},
                                        .first_sequence_number = 1U,
                                        .last_sequence_number = 2U,
                                        .frames = {first, second},
                                        .complete = true}},
                    .complete = true};
    value.key_frames.push_back({.descriptor = std::move(descriptor),
                                .jpeg = {std::byte{0xff}, std::byte{0xd8}, std::byte{0x01},
                                         std::byte{0xff}, std::byte{0xd9}}});
    return value;
}

Error injected_io_error(const std::string& code)
{
    Error error = make_error("STORAGE_IO_FAILED", Severity::error, "injected I/O failure",
                             "storage-test", "inject", true);
    error.native_domain = "win32";
    error.native_code = code;
    return error;
}

class FaultingFileSystem final : public IEventFileSystem
{
  public:
    explicit FaultingFileSystem(std::shared_ptr<IEventFileSystem> delegate)
        : delegate_(std::move(delegate))
    {
    }

    Result<void> create_directories(const std::filesystem::path& path) override
    {
        return delegate_->create_directories(path);
    }

    Result<void> create_directory_exclusive(const std::filesystem::path& path) override
    {
        return delegate_->create_directory_exclusive(path);
    }

    Result<void> write_new_file_durable(const std::filesystem::path& path,
                                        const std::span<const std::byte> contents) override
    {
        if (!fail_write_filename.empty() && path.filename() == fail_write_filename)
            return Result<void>::failure(injected_io_error(native_code));
        return delegate_->write_new_file_durable(path, contents);
    }

    Result<std::vector<std::byte>> read_file_bounded(const std::filesystem::path& path,
                                                     const std::size_t maximum_bytes) override
    {
        auto result = delegate_->read_file_bounded(path, maximum_bytes);
        if (result && !corrupt_read_filename.empty() && path.filename() == corrupt_read_filename &&
            !result.value().empty())
            result.value().front() ^= std::byte{0xff};
        return result;
    }

    Result<EventPathKind> path_kind(const std::filesystem::path& path) override
    {
        return delegate_->path_kind(path);
    }

    Result<std::vector<std::filesystem::path>> list_directories_bounded(
        const std::filesystem::path& path, const std::size_t maximum_entries) override
    {
        return delegate_->list_directories_bounded(path, maximum_entries);
    }

    Result<void> move_directory_atomically(const std::filesystem::path& source,
                                           const std::filesystem::path& destination) override
    {
        if (fail_move)
            return Result<void>::failure(injected_io_error(native_code));
        return delegate_->move_directory_atomically(source, destination);
    }

    std::filesystem::path fail_write_filename;
    std::filesystem::path corrupt_read_filename;
    std::string native_code{"112"};
    bool fail_move{};

  private:
    std::shared_ptr<IEventFileSystem> delegate_;
};

std::unique_ptr<EventTransactionWriter> writer_for(
    const std::filesystem::path& root, const std::shared_ptr<IEventFileSystem>& file_system)
{
    auto result = EventTransactionWriter::create({.event_root = root}, file_system);
    EXPECT_TRUE(result);
    return std::move(result).value();
}

TEST(StorageEventStore, PersistsReplayableManifestWithChecksumsUnderChinesePath)
{
    TemporaryDirectory temporary{"persist"};
    auto writer = writer_for(temporary.path() / L"事件 数据", make_windows_event_file_system());

    auto persisted = writer->persist(request("019f-m506-persist-0001"));

    ASSERT_TRUE(persisted) << persisted.error().message;
    EXPECT_EQ(persisted.value().raw_file_count, 2U);
    EXPECT_EQ(persisted.value().key_frame_count, 1U);
    EXPECT_TRUE(std::filesystem::is_directory(persisted.value().committed_directory));
    auto verified = writer->verify_committed_manifest(persisted.value().committed_directory);
    ASSERT_TRUE(verified) << verified.error().message;
    const auto manifest = nlohmann::json::parse(verified.value());
    EXPECT_EQ(manifest["schemaVersion"], 1U);
    EXPECT_EQ(manifest["eventId"], "019f-m506-persist-0001");
    EXPECT_EQ(manifest["eventState"], "Confirmed");
    EXPECT_EQ(manifest["candidateTime"], "2026-08-04T00:00:00.200Z");
    EXPECT_EQ(manifest["cameraIds"].size(), 1U);
    EXPECT_EQ(manifest["rawFiles"].size(), 2U);
    EXPECT_EQ(manifest["keyFrames"].size(), 1U);
    EXPECT_EQ(manifest["keyFrames"][0]["reasons"].size(), 2U);
    EXPECT_EQ(manifest["fileChecksums"].size(), 4U);
    EXPECT_EQ(manifest["fileSizes"].size(), 4U);
    for (const auto& [name, checksum] : manifest["fileChecksums"].items())
    {
        EXPECT_TRUE(checksum.get<std::string>().starts_with("sha256:"));
        EXPECT_TRUE(std::filesystem::is_regular_file(persisted.value().committed_directory / name));
    }
    const auto raw_path =
        persisted.value().committed_directory / manifest["rawFiles"][0]["path"].get<std::string>();
    EXPECT_EQ(manifest["fileChecksums"][manifest["rawFiles"][0]["path"].get<std::string>()],
              "sha256:5dfbabeedf318bf33c0927c43d7630f51b82f351740301354fa3d7fc51f0132e");
    EXPECT_EQ(std::filesystem::file_size(raw_path), 16U);
    EXPECT_FALSE(std::filesystem::exists(persisted.value().transaction_directory));
}

TEST(StorageEventStore, RejectsUnsafeIdentityBudgetOverflowAndUntraceableKeyFrameBeforeIo)
{
    TemporaryDirectory temporary{"validation"};
    auto writer = writer_for(temporary.path(), make_windows_event_file_system());

    auto unsafe = request("../unsafe-event");
    auto unsafe_result = writer->persist(unsafe);
    ASSERT_FALSE(unsafe_result);
    EXPECT_EQ(unsafe_result.error().business_code, "EVENT_WRITE_FAILED");

    auto untraceable = request("019f-m506-untraceable");
    untraceable.key_frames[0].descriptor.sequence_number = 999U;
    auto untraceable_result = writer->persist(untraceable);
    ASSERT_FALSE(untraceable_result);
    EXPECT_EQ(untraceable_result.error().business_code, "EVENT_WRITE_FAILED");

    auto small_writer_result = EventTransactionWriter::create(
        {.event_root = temporary.path() / "small", .maximum_raw_frames = 1U},
        make_windows_event_file_system());
    ASSERT_TRUE(small_writer_result);
    auto overflow = std::move(small_writer_result).value()->persist(request("019f-m506-overflow"));
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().business_code, "EVENT_WRITE_FAILED");
    EXPECT_FALSE(std::filesystem::exists(temporary.path() / ".transactions"));
}

TEST(StorageEventStore, RetainsTransactionAndNeverPublishesOnEveryCommitWritePointFailure)
{
    const std::vector<std::filesystem::path> write_points{"event.json", "frame-1.raw",
                                                          "keyframe-0.jpg", "manifest.json"};
    for (std::size_t index = 0U; index < write_points.size() + 1U; ++index)
    {
        TemporaryDirectory temporary{"write-point-" + std::to_string(index)};
        auto file_system = std::make_shared<FaultingFileSystem>(make_windows_event_file_system());
        if (index < write_points.size())
            file_system->fail_write_filename = write_points[index];
        else
            file_system->fail_move = true;
        auto writer = writer_for(temporary.path(), file_system);
        const auto event_id = "019f-m506-failure-000" + std::to_string(index);

        auto persisted = writer->persist(request(event_id));

        ASSERT_FALSE(persisted) << index;
        EXPECT_EQ(persisted.error().business_code, "EVENT_WRITE_FAILED") << index;
        EXPECT_EQ(persisted.error().severity, Severity::critical) << index;
        EXPECT_FALSE(std::filesystem::exists(temporary.path() / "2026" / "08" / "04" / event_id));
        std::size_t pending_count = 0U;
        for (const auto& entry :
             std::filesystem::directory_iterator{temporary.path() / ".transactions"})
            pending_count += entry.is_directory() ? 1U : 0U;
        EXPECT_EQ(pending_count, 1U) << index;
    }
}

TEST(StorageEventStore, MapsDiskFullAndAccessDeniedWithoutLosingNativeDiagnostics)
{
    for (const auto& native_code : {std::string{"112"}, std::string{"5"}})
    {
        TemporaryDirectory temporary{"native-" + native_code};
        auto file_system = std::make_shared<FaultingFileSystem>(make_windows_event_file_system());
        file_system->fail_write_filename = "event.json";
        file_system->native_code = native_code;
        auto writer = writer_for(temporary.path(), file_system);

        auto persisted = writer->persist(request("019f-m506-native-" + native_code));

        ASSERT_FALSE(persisted);
        EXPECT_EQ(persisted.error().business_code, "EVENT_WRITE_FAILED");
        ASSERT_TRUE(persisted.error().native_code.has_value());
        EXPECT_EQ(*persisted.error().native_code, native_code);
    }
}

TEST(StorageEventStore, DetectsWriteBackChecksumMismatchAndKeepsPendingEvidenceHidden)
{
    TemporaryDirectory temporary{"checksum"};
    auto file_system = std::make_shared<FaultingFileSystem>(make_windows_event_file_system());
    file_system->corrupt_read_filename = "frame-1.raw";
    auto writer = writer_for(temporary.path(), file_system);

    auto persisted = writer->persist(request("019f-m506-checksum-0001"));

    ASSERT_FALSE(persisted);
    EXPECT_EQ(persisted.error().business_code, "EVENT_CHECKSUM_FAILED");
    auto pending = std::filesystem::directory_iterator{temporary.path() / ".transactions"};
    ASSERT_NE(pending, std::filesystem::directory_iterator{});
    auto hidden = writer->verify_committed_manifest(pending->path());
    ASSERT_FALSE(hidden);
    EXPECT_EQ(hidden.error().business_code, "EVENT_NOT_FOUND");
}

TEST(StorageEventStore, StartupRecoveryCommitsOnlyCompleteValidatedResidualTransaction)
{
    TemporaryDirectory temporary{"recover-complete"};
    auto file_system = std::make_shared<FaultingFileSystem>(make_windows_event_file_system());
    file_system->fail_move = true;
    auto first_writer = writer_for(temporary.path(), file_system);
    auto interrupted = first_writer->persist(request("019f-m506-recover-0001"));
    ASSERT_FALSE(interrupted);
    file_system->fail_move = false;
    auto recovery_writer = writer_for(temporary.path(), file_system);

    auto recovery = recovery_writer->recover_pending();

    ASSERT_TRUE(recovery) << recovery.error().message;
    EXPECT_EQ(recovery.value().scanned, 1U);
    EXPECT_EQ(recovery.value().committed, 1U);
    EXPECT_EQ(recovery.value().quarantined, 0U);
    ASSERT_EQ(recovery.value().items.size(), 1U);
    EXPECT_EQ(recovery.value().items[0].disposition, EventRecoveryDisposition::committed);
    EXPECT_TRUE(
        recovery_writer->verify_committed_manifest(recovery.value().items[0].resulting_directory));
}

TEST(StorageEventStore, StartupRecoveryQuarantinesMissingAndUnsupportedManifests)
{
    TemporaryDirectory temporary{"recover-damaged"};
    auto file_system = std::make_shared<FaultingFileSystem>(make_windows_event_file_system());
    file_system->fail_write_filename = "manifest.json";
    auto first_writer = writer_for(temporary.path(), file_system);
    ASSERT_FALSE(first_writer->persist(request("019f-m506-missing-0001")));
    file_system->fail_write_filename.clear();

    auto recovery_writer = writer_for(temporary.path(), file_system);
    auto missing = recovery_writer->recover_pending();
    ASSERT_TRUE(missing);
    ASSERT_EQ(missing.value().quarantined, 1U);
    EXPECT_EQ(missing.value().items[0].reason, "manifest-missing");
    EXPECT_TRUE(std::filesystem::is_regular_file(missing.value().items[0].resulting_directory /
                                                 "recovery.json"));

    file_system->fail_move = true;
    ASSERT_FALSE(first_writer->persist(request("019f-m506-schema-0001")));
    file_system->fail_move = false;
    auto transaction_iterator =
        std::filesystem::directory_iterator{temporary.path() / ".transactions"};
    ASSERT_NE(transaction_iterator, std::filesystem::directory_iterator{});
    const auto manifest_path = transaction_iterator->path() / "manifest.json";
    nlohmann::json manifest;
    {
        std::ifstream input{manifest_path};
        input >> manifest;
    }
    manifest["schemaVersion"] = 99U;
    {
        std::ofstream output{manifest_path, std::ios::trunc};
        output << manifest.dump(2) << '\n';
    }

    auto unsupported = recovery_writer->recover_pending();

    ASSERT_TRUE(unsupported);
    ASSERT_EQ(unsupported.value().quarantined, 1U);
    EXPECT_EQ(unsupported.value().items[0].reason, "EVENT_SCHEMA_UNSUPPORTED");
}

TEST(StorageEventStore, StartupRecoveryQuarantinesChecksumMismatchInsteadOfPublishing)
{
    TemporaryDirectory temporary{"recover-checksum"};
    auto file_system = std::make_shared<FaultingFileSystem>(make_windows_event_file_system());
    file_system->fail_move = true;
    auto first_writer = writer_for(temporary.path(), file_system);
    const auto event_id = "019f-m506-recovery-checksum";
    ASSERT_FALSE(first_writer->persist(request(event_id)));
    file_system->fail_move = false;
    auto transaction_iterator =
        std::filesystem::directory_iterator{temporary.path() / ".transactions"};
    ASSERT_NE(transaction_iterator, std::filesystem::directory_iterator{});
    const auto raw_path = transaction_iterator->path() / "raw" / "camera-0" / "frame-1.raw";
    {
        std::fstream raw{raw_path, std::ios::in | std::ios::out | std::ios::binary};
        ASSERT_TRUE(raw);
        raw.put(static_cast<char>(0xff));
    }
    auto recovery_writer = writer_for(temporary.path(), file_system);

    auto recovery = recovery_writer->recover_pending();

    ASSERT_TRUE(recovery);
    ASSERT_EQ(recovery.value().quarantined, 1U);
    EXPECT_EQ(recovery.value().items[0].reason, "EVENT_CHECKSUM_FAILED");
    EXPECT_FALSE(std::filesystem::exists(temporary.path() / "2026" / "08" / "04" / event_id));
}

class BlockingWriter final : public IEventTransactionWriter
{
  public:
    Result<EventPersistenceOutcome> persist(const EventPersistenceRequest& value) override
    {
        std::unique_lock lock{mutex_};
        ++entered_;
        entered_condition_.notify_all();
        release_condition_.wait(lock, [this] { return released_; });
        return Result<EventPersistenceOutcome>::success(
            {.event_id = value.metadata.event_id,
             .committed_directory = value.metadata.event_id,
             .manifest_json = "{}"});
    }

    void wait_until_entered(const std::size_t count)
    {
        std::unique_lock lock{mutex_};
        ASSERT_TRUE(
            entered_condition_.wait_for(lock, 2s, [this, count] { return entered_ >= count; }));
    }

    void release()
    {
        {
            std::scoped_lock lock{mutex_};
            released_ = true;
        }
        release_condition_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable entered_condition_;
    std::condition_variable release_condition_;
    std::size_t entered_{};
    bool released_{};
};

TEST(StorageEventStore, BoundedRuntimeRejectsExactlyWhenFullAndDrainsAcceptedJobs)
{
    auto writer = std::make_unique<BlockingWriter>();
    auto* writer_pointer = writer.get();
    std::mutex callback_mutex;
    std::condition_variable callback_condition;
    std::size_t callbacks = 0U;
    auto runtime_result =
        EventPersistenceRuntime::create(std::move(writer),
                                        [&](EventPersistenceCompletion completion) {
                                            EXPECT_TRUE(completion.outcome.has_value());
                                            {
                                                std::scoped_lock lock{callback_mutex};
                                                ++callbacks;
                                            }
                                            callback_condition.notify_all();
                                        },
                                        {.event_capacity = 1U});
    ASSERT_TRUE(runtime_result);
    auto runtime = std::move(runtime_result).value();
    ASSERT_TRUE(runtime->start());
    ASSERT_TRUE(runtime->submit(request("019f-m506-runtime-0001")));
    writer_pointer->wait_until_entered(1U);
    ASSERT_TRUE(runtime->submit(request("019f-m506-runtime-0002")));

    auto rejected = runtime->submit(request("019f-m506-runtime-0003"));

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().business_code, "EVENT_WRITE_FAILED");
    EXPECT_EQ(rejected.error().severity, Severity::critical);
    EXPECT_EQ(runtime->snapshot().high_watermark, 1U);
    runtime->request_stop();
    writer_pointer->release();
    ASSERT_TRUE(runtime->join(MonotonicTime::clock::now() + 2s));
    {
        std::unique_lock lock{callback_mutex};
        ASSERT_TRUE(callback_condition.wait_for(lock, 2s, [&] { return callbacks == 2U; }));
    }
    const auto snapshot = runtime->snapshot();
    EXPECT_EQ(snapshot.submitted, 2U);
    EXPECT_EQ(snapshot.completed, 2U);
    EXPECT_EQ(snapshot.rejected, 1U);
    EXPECT_EQ(snapshot.depth, 0U);
}

class FailingWriter final : public IEventTransactionWriter
{
  public:
    Result<EventPersistenceOutcome> persist(const EventPersistenceRequest&) override
    {
        return Result<EventPersistenceOutcome>::failure(make_error(
            "EVENT_WRITE_FAILED", Severity::critical, "test", "storage-test", "persist"));
    }
};

TEST(StorageEventStore, RuntimeIsolatesWriteAndCallbackFailures)
{
    std::condition_variable condition;
    std::mutex mutex;
    bool called = false;
    auto runtime_result = EventPersistenceRuntime::create(
        std::make_unique<FailingWriter>(), [&](EventPersistenceCompletion completion) {
            EXPECT_TRUE(completion.error.has_value());
            {
                std::scoped_lock lock{mutex};
                called = true;
            }
            condition.notify_all();
            throw std::runtime_error{"callback failure"};
        });
    ASSERT_TRUE(runtime_result);
    auto runtime = std::move(runtime_result).value();
    ASSERT_TRUE(runtime->start());
    ASSERT_TRUE(runtime->submit(request("019f-m506-runtime-fail")));
    {
        std::unique_lock lock{mutex};
        ASSERT_TRUE(condition.wait_for(lock, 2s, [&] { return called; }));
    }
    runtime->request_stop();
    ASSERT_TRUE(runtime->join(MonotonicTime::clock::now() + 2s));
    const auto snapshot = runtime->snapshot();
    EXPECT_EQ(snapshot.completed, 1U);
    EXPECT_EQ(snapshot.write_failures, 1U);
    EXPECT_EQ(snapshot.callback_failures, 1U);
}

} // namespace
