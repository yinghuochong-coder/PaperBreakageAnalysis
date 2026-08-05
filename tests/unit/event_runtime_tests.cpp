#include "paperbreak/service/event_runtime.hpp"

#include "paperbreak/storage/event_inspector.hpp"

#include <gtest/gtest.h>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{
using namespace std::chrono_literals;
using namespace paperbreak;
using namespace paperbreak::camera;
using namespace paperbreak::service;
using namespace paperbreak::storage;

class TemporaryDirectory final
{
  public:
    TemporaryDirectory()
    {
        static std::atomic_uint64_t sequence{};
        path_ = std::filesystem::temp_directory_path() /
                (L"PaperBreak-M5-09-runtime-中文 空格-" + std::to_wstring(GetCurrentProcessId()) +
                 L"-" + std::to_wstring(sequence.fetch_add(1U)));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory()
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

struct DetectorBehavior final
{
    DetectorBehavior(std::string id, const std::chrono::milliseconds processing_delay,
                     const std::uint64_t failures)
        : plugin_id(std::move(id)), delay(processing_delay), remaining_failures(failures)
    {
    }

    std::string plugin_id;
    std::chrono::milliseconds delay{};
    std::atomic_uint64_t remaining_failures{};
};

class RuntimeTestDetector final : public algorithm::IBreakDetector
{
  public:
    explicit RuntimeTestDetector(std::shared_ptr<DetectorBehavior> behavior)
        : behavior_(std::move(behavior))
    {
    }

    Result<void> initialize(const algorithm::DetectorConfig& config) override
    {
        config_ = config;
        return Result<void>::success();
    }

    Result<algorithm::DetectionResult> process(const FrameView& input) override
    {
        if (behavior_->delay > 0ms)
            std::this_thread::sleep_for(behavior_->delay);
        auto remaining = behavior_->remaining_failures.load(std::memory_order_relaxed);
        while (remaining > 0U && !behavior_->remaining_failures.compare_exchange_weak(
                                     remaining, remaining - 1U, std::memory_order_relaxed))
        {
        }
        if (remaining > 0U)
            return Result<algorithm::DetectionResult>::failure(
                make_error("ALGORITHM_PROCESS_FAILED", Severity::error, "注入的检测失败",
                           "algorithm", "algorithm.test.process"));
        return Result<algorithm::DetectionResult>::success(
            {.camera_id = input.camera_id(),
             .sequence_number = input.sequence_number(),
             .camera_frame_number = input.camera_frame_number(),
             .monotonic_time = input.received_monotonic_time(),
             .wall_clock_time = input.received_wall_clock_time(),
             .evaluated_region = {.width = input.geometry().width,
                                  .height = input.geometry().height},
             .paper_ratio = 1.0,
             .detector_version = "runtime-test/1.0",
             .model_version = "none"});
    }

    Result<void> update_config(const algorithm::DetectorConfig& config) override
    {
        config_ = config;
        return Result<void>::success();
    }

    Result<void> reset() override
    {
        return Result<void>::success();
    }

    algorithm::DetectorInfo info() const override
    {
        return {.plugin_id = behavior_->plugin_id,
                .display_name = "Runtime Test Detector",
                .implementation_version = "1.0-test",
                .model_version = "none",
                .supports_hot_update = true,
                .prototype_only = true};
    }

  private:
    std::shared_ptr<DetectorBehavior> behavior_;
    algorithm::DetectorConfig config_;
};

std::function<Result<void>(algorithm::DetectorPluginRegistry&)> test_detector_registration(
    std::shared_ptr<DetectorBehavior> behavior)
{
    return [behavior = std::move(behavior)](algorithm::DetectorPluginRegistry& registry) {
        return registry.register_plugin(behavior->plugin_id, [behavior] {
            return Result<std::unique_ptr<algorithm::IBreakDetector>>::success(
                std::make_unique<RuntimeTestDetector>(behavior));
        });
    };
}

config::EdgeConfig runtime_config()
{
    config::EdgeConfig value;
    value.config_revision = 7U;
    value.system.machine_id = "EDGE-TEST";
    value.system.production_line_id = "PM-TEST";
    value.cameras = {{.id = "CAM01", .enabled = true, .frame_rate = 10.0}};
    value.acquisition.frame_pool_capacity = 128U;
    value.acquisition.queue_capacity = 4U;
    value.preview.enabled = false;
    value.event.pre_event_seconds = 1U;
    value.event.post_event_seconds = 1U;
    value.event.max_event_seconds = 3U;
    value.event.merge_gap_seconds = 0U;
    value.event.key_frame_count = 7U;
    value.event.save_raw = true;
    value.storage.event_root = "unused-relative-path";
    return value;
}

FrameView frame(const std::uint64_t sequence, const std::chrono::milliseconds offset,
                std::string camera_id = "CAM01")
{
    auto buffer = std::make_shared<FrameBuffer>(16U);
    for (std::size_t index = 0U; index < 16U; ++index)
        buffer->writable_bytes()[index] = static_cast<std::byte>((sequence + index) & 0xffU);
    if (!buffer->set_size(16U))
        throw std::runtime_error{"frame size"};
    const auto wall = WallClockTime{std::chrono::sys_days{std::chrono::year{2026} / 8 / 4}};
    auto view = make_frame_view({.camera_id = std::move(camera_id),
                                 .camera_frame_number = 1000U + sequence,
                                 .sequence_number = sequence,
                                 .received_monotonic_time = MonotonicTime{offset},
                                 .received_wall_clock_time = wall + offset,
                                 .geometry = {.width = 4U, .height = 4U, .stride = 4U},
                                 .pixel_format = PixelFormat::mono8,
                                 .buffer = std::move(buffer)});
    if (!view)
        throw std::runtime_error{"frame view"};
    return std::move(view).value();
}

} // namespace

TEST(EventRuntimeIntegration, ManualTriggerPersistsContinuousWindowWithoutBlockingSubmitter)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / L"事件 根";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / L"数据库" / L"events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / L"备份"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    std::atomic_uint64_t errors{};
    auto runtime = EventRuntime::create({.configuration = runtime_config(),
                                         .event_root = event_root,
                                         .database = shared_database,
                                         .frame_queue_capacity = 64U,
                                         .error_observer = [&](const Error&) { ++errors; }});
    ASSERT_TRUE(runtime) << runtime.error().message;
    ASSERT_TRUE(runtime.value()->start());

    for (std::uint64_t sequence = 1U; sequence <= 10U; ++sequence)
        ASSERT_TRUE(runtime.value()->submit_frame(
            frame(sequence, std::chrono::milliseconds{static_cast<int>(sequence * 100U)})));
    auto requested = runtime.value()->request_manual_trigger("CAM01");
    ASSERT_TRUE(requested);
    EXPECT_TRUE(requested.value());
    auto coalesced = runtime.value()->request_manual_trigger("CAM01");
    ASSERT_TRUE(coalesced);
    EXPECT_FALSE(coalesced.value());
    for (std::uint64_t sequence = 11U; sequence <= 23U; ++sequence)
        ASSERT_TRUE(runtime.value()->submit_frame(
            frame(sequence, std::chrono::milliseconds{static_cast<int>(sequence * 100U)})));

    EventQueryPage page;
    for (std::size_t attempt = 0U; attempt < 200U; ++attempt)
    {
        auto queried = shared_database->query_events({.limit = 10U});
        ASSERT_TRUE(queried);
        page = std::move(queried).value();
        if (!page.events.empty())
            break;
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_EQ(page.events.size(), 1U);
    EXPECT_EQ(page.events.front().trigger_camera_id, "CAM01");
    EXPECT_EQ(page.events.front().trigger_reason, "ManualTest");
    EXPECT_EQ(page.events.front().trigger_frame_number, 1011U);
    EXPECT_EQ(page.events.front().storage_state, "Present");

    auto inspector = EventInspector::create({.event_root = event_root});
    ASSERT_TRUE(inspector);
    auto inspected = inspector.value()->inspect(page.events.front().relative_directory);
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected.value().key_frames_traceable);
    EXPECT_EQ(inspected.value().observed_sequence_gaps, 0U);
    EXPECT_LE(inspected.value().raw_frames.front().sequence_number, 1U);
    EXPECT_GE(inspected.value().raw_frames.back().sequence_number, 21U);

    runtime.value()->request_stop();
    ASSERT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
    const auto snapshot = runtime.value()->snapshot();
    EXPECT_EQ(snapshot.submitted_frames, 23U);
    EXPECT_EQ(snapshot.processed_frames, 23U);
    EXPECT_EQ(snapshot.rejected_frames, 0U);
    EXPECT_EQ(snapshot.events_committed, 1U);
    EXPECT_EQ(errors.load(), 0U);
}

TEST(EventRuntimeNvme, SuccessfulEventCommitReleasesProtectedBlocks)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto nvme = NvmeRollingCache::create({.root = temporary.path() / "cache",
                                          .maximum_cache_bytes = 65536U,
                                          .write_limit_bytes_per_second = 16U * 1024U * 1024U,
                                          .io_timeout = 2s,
                                          .cameras = {{.camera_id = "CAM01",
                                                       .maximum_frame_bytes = 16U,
                                                       .index_capacity = 12U,
                                                       .required_input_bytes_per_second = 1024U}}});
    ASSERT_TRUE(nvme);
    ASSERT_TRUE(nvme.value()->start());
    auto configuration = runtime_config();
    configuration.storage.rolling_cache_enabled = true;
    configuration.acquisition.frame_pool_capacity = 512U;
    std::atomic_uint64_t errors{};
    auto runtime = EventRuntime::create({.configuration = configuration,
                                         .event_root = event_root,
                                         .database = shared_database,
                                         .nvme_cache = nvme.value(),
                                         .frame_queue_capacity = 64U,
                                         .error_observer = [&](const Error&) { ++errors; }});
    ASSERT_TRUE(runtime) << runtime.error().message;
    ASSERT_TRUE(runtime.value()->start());
    const auto submit = [&](const std::uint64_t sequence) {
        auto input = frame(sequence, std::chrono::milliseconds{static_cast<int>(sequence * 100U)});
        EXPECT_TRUE(runtime.value()->submit_frame(input));
        EXPECT_TRUE(nvme.value()->submit_frame(std::move(input)));
    };
    for (std::uint64_t sequence = 1U; sequence <= 10U; ++sequence)
        submit(sequence);
    auto requested = runtime.value()->request_manual_trigger("CAM01");
    ASSERT_TRUE(requested);
    ASSERT_TRUE(requested.value());
    submit(11U);
    submit(12U);
    bool lease_observed = false;
    for (std::size_t attempt = 0U; attempt < 200U; ++attempt)
    {
        if (nvme.value()->snapshot().active_event_leases == 1U)
        {
            lease_observed = true;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(lease_observed);
    for (std::uint64_t sequence = 13U; sequence <= 24U; ++sequence)
        submit(sequence);
    bool committed_and_released = false;
    for (std::size_t attempt = 0U; attempt < 400U; ++attempt)
    {
        const auto event_snapshot = runtime.value()->snapshot();
        const auto nvme_snapshot = nvme.value()->snapshot();
        if (event_snapshot.events_committed == 1U && nvme_snapshot.active_event_leases == 0U)
        {
            committed_and_released = true;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(committed_and_released);
    const auto nvme_snapshot = nvme.value()->snapshot();
    EXPECT_GE(nvme_snapshot.indexed_blocks, 2U);
    EXPECT_EQ(nvme_snapshot.protected_blocks, 0U);
    EXPECT_EQ(nvme_snapshot.lease_failures, 0U);
    EXPECT_EQ(errors.load(), 0U);
    runtime.value()->request_stop();
    ASSERT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
    nvme.value()->request_stop();
    ASSERT_TRUE(nvme.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(EventRuntimeNvme, FailedEventPersistenceKeepsLeaseProtected)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "blocked-events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto nvme = NvmeRollingCache::create({.root = temporary.path() / "cache",
                                          .maximum_cache_bytes = 65536U,
                                          .write_limit_bytes_per_second = 16U * 1024U * 1024U,
                                          .io_timeout = 2s,
                                          .cameras = {{.camera_id = "CAM01",
                                                       .maximum_frame_bytes = 16U,
                                                       .index_capacity = 12U,
                                                       .required_input_bytes_per_second = 1024U}}});
    ASSERT_TRUE(nvme);
    ASSERT_TRUE(nvme.value()->start());
    auto configuration = runtime_config();
    configuration.storage.rolling_cache_enabled = true;
    configuration.acquisition.frame_pool_capacity = 512U;
    std::atomic_uint64_t errors{};
    auto runtime = EventRuntime::create({.configuration = configuration,
                                         .event_root = event_root,
                                         .database = shared_database,
                                         .nvme_cache = nvme.value(),
                                         .frame_queue_capacity = 64U,
                                         .error_observer = [&](const Error&) { ++errors; }});
    ASSERT_TRUE(runtime) << runtime.error().message;
    std::error_code file_error;
    std::filesystem::remove_all(event_root, file_error);
    ASSERT_FALSE(file_error);
    std::ofstream blocker{event_root, std::ios::binary};
    ASSERT_TRUE(blocker);
    blocker << "not-a-directory";
    blocker.close();
    ASSERT_TRUE(runtime.value()->start());
    const auto submit = [&](const std::uint64_t sequence) {
        auto input = frame(sequence, std::chrono::milliseconds{static_cast<int>(sequence * 100U)});
        EXPECT_TRUE(runtime.value()->submit_frame(input));
        EXPECT_TRUE(nvme.value()->submit_frame(std::move(input)));
    };
    for (std::uint64_t sequence = 1U; sequence <= 10U; ++sequence)
        submit(sequence);
    auto requested = runtime.value()->request_manual_trigger("CAM01");
    ASSERT_TRUE(requested);
    ASSERT_TRUE(requested.value());
    for (std::uint64_t sequence = 11U; sequence <= 24U; ++sequence)
        submit(sequence);
    bool failed_and_protected = false;
    for (std::size_t attempt = 0U; attempt < 400U; ++attempt)
    {
        const auto event_snapshot = runtime.value()->snapshot();
        const auto nvme_snapshot = nvme.value()->snapshot();
        if (event_snapshot.event_failures > 0U && nvme_snapshot.active_event_leases == 1U &&
            nvme_snapshot.protected_blocks > 0U)
        {
            failed_and_protected = true;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(failed_and_protected);
    EXPECT_EQ(runtime.value()->snapshot().events_committed, 0U);
    EXPECT_GT(errors.load(), 0U);
    runtime.value()->request_stop();
    ASSERT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
    nvme.value()->request_stop();
    ASSERT_TRUE(nvme.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(EventRuntimeIntegration, RejectsUnsafePoolBudgetAndUnknownManualCamera)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto unsafe = runtime_config();
    unsafe.acquisition.frame_pool_capacity = 16U;
    auto rejected = EventRuntime::create(
        {.configuration = unsafe, .event_root = event_root, .database = shared_database});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().business_code, "SYS_CONFIG_INVALID");

    auto runtime = EventRuntime::create(
        {.configuration = runtime_config(), .event_root = event_root, .database = shared_database});
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value()->start());
    auto missing = runtime.value()->request_manual_trigger("CAM04");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().business_code, "CAMERA_NOT_FOUND");
    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(EventRuntimeIntegration, DropsOldestOnBacklogAndKeepsSubmissionNonBlocking)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto configuration = runtime_config();
    configuration.cameras.push_back({.id = "CAM02", .enabled = true, .frame_rate = 10.0});
    configuration.algorithm.enabled = true;
    configuration.algorithm.type = "slow-runtime-test";
    auto behavior = std::make_shared<DetectorBehavior>(configuration.algorithm.type, 40ms, 0U);
    auto runtime = EventRuntime::create(
        {.configuration = configuration,
         .event_root = event_root,
         .database = shared_database,
         .frame_queue_capacity = 1U,
         .consecutive_backlog_limit = 100U,
         .detector_registry_configurer = test_detector_registration(behavior)});
    ASSERT_TRUE(runtime) << runtime.error().message;
    ASSERT_TRUE(runtime.value()->start());

    ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms, "CAM02")));
    for (std::uint64_t sequence = 1U; sequence <= 12U; ++sequence)
        ASSERT_TRUE(runtime.value()->submit_frame(
            frame(sequence, std::chrono::milliseconds{sequence * 100U})));

    runtime.value()->request_stop();
    ASSERT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
    const auto snapshot = runtime.value()->snapshot();
    EXPECT_EQ(snapshot.submitted_frames, 13U);
    EXPECT_EQ(snapshot.rejected_frames, 0U);
    EXPECT_GT(snapshot.skipped_frames, 0U);
    EXPECT_EQ(snapshot.processed_frames + snapshot.skipped_frames, snapshot.submitted_frames);
    EXPECT_EQ(snapshot.frame_queue_capacity, 2U);
    EXPECT_GE(snapshot.frame_queue_high_watermark, 1U);
    EXPECT_LE(snapshot.frame_queue_high_watermark, snapshot.frame_queue_capacity);
    EXPECT_GE(snapshot.processed_frames, 2U);
    EXPECT_EQ(snapshot.algorithm_state, AlgorithmRuntimeState::active);
}

TEST(EventRuntimeIntegration, ConsecutiveDetectorFailuresDegradeButManualTriggerStillWorks)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto configuration = runtime_config();
    configuration.algorithm.enabled = true;
    configuration.algorithm.type = "failing-runtime-test";
    configuration.algorithm.consecutive_frames = 1U;
    auto behavior = std::make_shared<DetectorBehavior>(configuration.algorithm.type, 0ms, 2U);
    std::atomic_uint64_t degraded_errors{};
    auto runtime =
        EventRuntime::create({.configuration = configuration,
                              .event_root = event_root,
                              .database = shared_database,
                              .consecutive_failure_limit = 2U,
                              .detector_registry_configurer = test_detector_registration(behavior),
                              .error_observer = [&](const Error& error) {
                                  if (error.business_code == "ALGORITHM_DEGRADED")
                                      ++degraded_errors;
                              }});
    ASSERT_TRUE(runtime) << runtime.error().message;
    ASSERT_TRUE(runtime.value()->start());

    ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms)));
    for (std::size_t attempt = 0U;
         attempt < 100U && runtime.value()->snapshot().detector_process_calls < 1U; ++attempt)
        std::this_thread::sleep_for(2ms);
    ASSERT_TRUE(runtime.value()->submit_frame(frame(2U, 200ms)));
    for (std::size_t attempt = 0U; attempt < 100U && runtime.value()->snapshot().algorithm_state !=
                                                         AlgorithmRuntimeState::manual_trigger_only;
         ++attempt)
        std::this_thread::sleep_for(2ms);

    auto degraded = runtime.value()->snapshot();
    EXPECT_EQ(degraded.algorithm_state, AlgorithmRuntimeState::manual_trigger_only);
    EXPECT_EQ(degraded.detector_failures, 2U);
    EXPECT_EQ(degraded_errors.load(), 1U);

    auto requested = runtime.value()->request_manual_trigger("CAM01");
    ASSERT_TRUE(requested);
    EXPECT_TRUE(requested.value());
    ASSERT_TRUE(runtime.value()->submit_frame(frame(3U, 300ms)));
    for (std::size_t attempt = 0U;
         attempt < 100U && runtime.value()->snapshot().events_started == 0U; ++attempt)
        std::this_thread::sleep_for(2ms);
    EXPECT_EQ(runtime.value()->snapshot().events_started, 1U);

    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(EventRuntimeIntegration, FailedReconfigurationKeepsActiveDetectorAndConfiguration)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto configuration = runtime_config();
    configuration.algorithm.enabled = true;
    configuration.algorithm.type = "mock";
    auto runtime = EventRuntime::create(
        {.configuration = configuration, .event_root = event_root, .database = shared_database});
    ASSERT_TRUE(runtime) << runtime.error().message;
    ASSERT_TRUE(runtime.value()->start());
    ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms)));
    for (std::size_t attempt = 0U;
         attempt < 100U && runtime.value()->snapshot().detector_process_calls < 1U; ++attempt)
        std::this_thread::sleep_for(2ms);

    auto invalid = configuration;
    invalid.config_revision = configuration.config_revision + 1U;
    invalid.algorithm.type = "plugin-not-registered";
    auto rejected = runtime.value()->reconfigure(invalid);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().business_code, "ALGORITHM_PLUGIN_LOAD_FAILED");

    ASSERT_TRUE(runtime.value()->submit_frame(frame(2U, 200ms)));
    for (std::size_t attempt = 0U;
         attempt < 100U && runtime.value()->snapshot().detector_process_calls < 2U; ++attempt)
        std::this_thread::sleep_for(2ms);
    const auto snapshot = runtime.value()->snapshot();
    EXPECT_EQ(snapshot.algorithm_state, AlgorithmRuntimeState::active);
    EXPECT_EQ(snapshot.detector_process_calls, 2U);

    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(EventRuntimeIntegration, SingleDetectorFailureIsIsolatedAndNextFrameRecovers)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto configuration = runtime_config();
    configuration.algorithm.enabled = true;
    configuration.algorithm.type = "recovering-runtime-test";
    auto behavior = std::make_shared<DetectorBehavior>(configuration.algorithm.type, 0ms, 1U);
    auto runtime = EventRuntime::create(
        {.configuration = configuration,
         .event_root = event_root,
         .database = shared_database,
         .consecutive_failure_limit = 2U,
         .detector_registry_configurer = test_detector_registration(behavior)});
    ASSERT_TRUE(runtime) << runtime.error().message;
    ASSERT_TRUE(runtime.value()->start());

    ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms)));
    for (std::size_t attempt = 0U;
         attempt < 100U && runtime.value()->snapshot().detector_process_calls < 1U; ++attempt)
        std::this_thread::sleep_for(2ms);
    ASSERT_TRUE(runtime.value()->submit_frame(frame(2U, 200ms)));
    for (std::size_t attempt = 0U;
         attempt < 100U && runtime.value()->snapshot().detector_process_calls < 2U; ++attempt)
        std::this_thread::sleep_for(2ms);

    const auto snapshot = runtime.value()->snapshot();
    EXPECT_EQ(snapshot.detector_failures, 1U);
    EXPECT_EQ(snapshot.consecutive_detector_failures, 0U);
    EXPECT_EQ(snapshot.algorithm_state, AlgorithmRuntimeState::active);
    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(EventRuntimeIntegration, LoadsClassicalDetectorThroughSameRuntimeBoundary)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto configuration = runtime_config();
    configuration.algorithm.enabled = true;
    configuration.algorithm.type = "classical-vision";
    configuration.algorithm.roi = {.width = 4U, .height = 4U};
    auto runtime = EventRuntime::create(
        {.configuration = configuration, .event_root = event_root, .database = shared_database});
    ASSERT_TRUE(runtime) << runtime.error().message;
    ASSERT_TRUE(runtime.value()->start());
    ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms)));
    for (std::size_t attempt = 0U;
         attempt < 100U && runtime.value()->snapshot().detector_process_calls < 1U; ++attempt)
        std::this_thread::sleep_for(2ms);
    const auto snapshot = runtime.value()->snapshot();
    EXPECT_EQ(snapshot.detector_process_calls, 1U);
    EXPECT_EQ(snapshot.detector_failures, 0U);
    EXPECT_GT(snapshot.last_algorithm_processing_time.count(), 0);
    EXPECT_EQ(snapshot.algorithm_state, AlgorithmRuntimeState::active);
    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(EventRuntimeIntegration, ExposesAppliedAlgorithmStateAndTestsLatestFrameInIsolation)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto configuration = runtime_config();
    configuration.algorithm.enabled = true;
    configuration.algorithm.type = "classical-vision";
    configuration.algorithm.roi = {.width = 4U, .height = 4U, .offset_x = 0U, .offset_y = 0U};
    auto runtime = EventRuntime::create(
        {.configuration = configuration, .event_root = event_root, .database = shared_database});
    ASSERT_TRUE(runtime) << runtime.error().message;
    ASSERT_TRUE(runtime.value()->start());

    auto initial = runtime.value()->algorithm_snapshot("CAM01");
    ASSERT_TRUE(initial) << initial.error().message;
    EXPECT_EQ(initial.value().state, AlgorithmRuntimeState::active);
    EXPECT_EQ(initial.value().config_revision, 7U);
    EXPECT_FALSE(initial.value().has_current_frame);
    ASSERT_TRUE(initial.value().detector_info.has_value());
    EXPECT_EQ(initial.value().detector_info->plugin_id, "classical-vision");
    EXPECT_TRUE(initial.value().detector_info->prototype_only);

    auto missing = runtime.value()->test_current_frame("CAM01");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().business_code, "ALGORITHM_NOT_READY");
    ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms)));
    for (std::size_t attempt = 0U;
         attempt < 100U && runtime.value()->snapshot().processed_frames < 1U; ++attempt)
        std::this_thread::sleep_for(2ms);
    const auto candidates_before = runtime.value()->snapshot().candidates_created;

    auto tested = runtime.value()->test_current_frame("CAM01");
    ASSERT_TRUE(tested) << tested.error().message;
    EXPECT_EQ(tested.value().detector_info.plugin_id, "classical-vision");
    EXPECT_TRUE(tested.value().detector_info.prototype_only);
    EXPECT_EQ(tested.value().detection.sequence_number, 1U);
    EXPECT_EQ(tested.value().detection.camera_id, "CAM01");
    EXPECT_FALSE(tested.value().detection.debug_metrics.empty());
    EXPECT_EQ(tested.value().source_width, 4U);
    EXPECT_EQ(tested.value().source_height, 4U);
    EXPECT_FALSE(tested.value().preview_jpeg.empty());
    EXPECT_EQ(runtime.value()->snapshot().candidates_created, candidates_before);

    configuration.config_revision = 8U;
    configuration.algorithm.enabled = false;
    configuration.algorithm.type = "mock";
    ASSERT_TRUE(runtime.value()->reconfigure(configuration));
    auto disabled = runtime.value()->algorithm_snapshot("CAM01");
    ASSERT_TRUE(disabled);
    EXPECT_EQ(disabled.value().state, AlgorithmRuntimeState::disabled);
    EXPECT_EQ(disabled.value().config_revision, 8U);
    EXPECT_TRUE(disabled.value().has_current_frame);
    EXPECT_FALSE(disabled.value().detector_info.has_value());
    const auto disabled_candidates_before = runtime.value()->snapshot().candidates_created;
    auto disabled_test = runtime.value()->test_current_frame("CAM01");
    ASSERT_TRUE(disabled_test) << disabled_test.error().message;
    EXPECT_EQ(disabled_test.value().detector_info.plugin_id, "mock-trigger");
    EXPECT_FALSE(disabled_test.value().preview_jpeg.empty());
    EXPECT_EQ(runtime.value()->snapshot().candidates_created, disabled_candidates_before);

    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}
