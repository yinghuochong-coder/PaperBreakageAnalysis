#include "paperbreak/common/camera_slots.hpp"
#include "paperbreak/service/algorithm_metrics.hpp"
#include "paperbreak/service/event_runtime.hpp"

#include "paperbreak/storage/event_inspector.hpp"

#include <gtest/gtest.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

struct ControlledDetectorBehavior final
{
    std::mutex mutex;
    std::condition_variable condition;
    bool hold_cam01{};
    bool release_cam01{};
    std::map<std::string, std::size_t> failures_remaining;
    std::set<std::string> triggered_cameras;
    std::map<std::pair<std::string, std::uint64_t>, double> confidence_by_frame;
    double default_trigger_confidence{1.0};
    std::map<std::string, std::vector<std::uint64_t>> completed_sequences;
    std::map<std::string, std::vector<std::chrono::steady_clock::time_point>> started_at;
    std::map<std::string, std::chrono::milliseconds> processing_delay;
    std::function<void(std::string_view)> advance_clock;
    std::size_t active_calls{};
    std::size_t maximum_active_calls{};
};

class ControlledRuntimeDetector final : public algorithm::IBreakDetector
{
  public:
    explicit ControlledRuntimeDetector(std::shared_ptr<ControlledDetectorBehavior> behavior)
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
        std::chrono::milliseconds delay{};
        {
            std::scoped_lock lock{behavior_->mutex};
            behavior_->started_at[input.camera_id()].push_back(std::chrono::steady_clock::now());
            const auto configured_delay = behavior_->processing_delay.find(input.camera_id());
            if (configured_delay != behavior_->processing_delay.end())
                delay = configured_delay->second;
        }
        if (delay > 0ms)
            std::this_thread::sleep_for(delay);
        if (behavior_->advance_clock)
            behavior_->advance_clock(input.camera_id());
        bool fail = false;
        bool triggered = false;
        double confidence = 0.0;
        {
            std::unique_lock lock{behavior_->mutex};
            ++behavior_->active_calls;
            behavior_->maximum_active_calls =
                std::max(behavior_->maximum_active_calls, behavior_->active_calls);
            if (behavior_->hold_cam01 && input.camera_id() == "CAM01")
                behavior_->condition.wait(lock, [&] { return behavior_->release_cam01; });
            auto& remaining = behavior_->failures_remaining[input.camera_id()];
            if (remaining > 0U)
            {
                --remaining;
                fail = true;
            }
            triggered = behavior_->triggered_cameras.contains(input.camera_id());
            if (triggered)
            {
                const auto configured = behavior_->confidence_by_frame.find(
                    {input.camera_id(), input.sequence_number()});
                confidence = configured == behavior_->confidence_by_frame.end()
                                 ? behavior_->default_trigger_confidence
                                 : configured->second;
            }
            behavior_->completed_sequences[input.camera_id()].push_back(input.sequence_number());
            --behavior_->active_calls;
        }
        if (fail)
            return Result<algorithm::DetectionResult>::failure(
                make_error("ALGORITHM_PROCESS_FAILED", Severity::error, "注入的 Lane 检测失败",
                           "algorithm", "algorithm.test.controlled"));
        return Result<algorithm::DetectionResult>::success(
            {.triggered = triggered,
             .trigger_source = triggered ? algorithm::TriggerSource::fixed_period
                                         : algorithm::TriggerSource::none,
             .camera_id = input.camera_id(),
             .sequence_number = input.sequence_number(),
             .camera_frame_number = input.camera_frame_number(),
             .monotonic_time = input.received_monotonic_time(),
             .wall_clock_time = input.received_wall_clock_time(),
             .evaluated_region = {.width = input.geometry().width,
                                  .height = input.geometry().height},
             .paper_ratio = 1.0,
             .anomalous = triggered,
             .confidence = confidence,
             .detector_version = "controlled-runtime-test/1.0",
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
        return {.plugin_id = "controlled-runtime-test",
                .display_name = "Controlled Runtime Test Detector",
                .implementation_version = "1.0-test",
                .model_version = "none",
                .supports_hot_update = true,
                .prototype_only = true};
    }

  private:
    std::shared_ptr<ControlledDetectorBehavior> behavior_;
    algorithm::DetectorConfig config_;
};

std::function<Result<void>(algorithm::DetectorPluginRegistry&)> controlled_detector_registration(
    std::shared_ptr<ControlledDetectorBehavior> behavior)
{
    return [behavior = std::move(behavior)](algorithm::DetectorPluginRegistry& registry) {
        return registry.register_plugin("controlled-runtime-test", [behavior] {
            return Result<std::unique_ptr<algorithm::IBreakDetector>>::success(
                std::make_unique<ControlledRuntimeDetector>(behavior));
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

config::EdgeConfig four_camera_runtime_config()
{
    auto value = runtime_config();
    value.cameras = {{.id = "CAM01", .enabled = true, .frame_rate = 10.0},
                     {.id = "CAM02", .enabled = true, .frame_rate = 10.0},
                     {.id = "CAM03", .enabled = true, .frame_rate = 10.0},
                     {.id = "CAM04", .enabled = true, .frame_rate = 10.0}};
    value.acquisition.frame_pool_capacity = 512U;
    value.algorithm.enabled = true;
    value.algorithm.type = "controlled-runtime-test";
    value.algorithm.confirmation_duration_ms = 10U;
    return value;
}

config::EdgeConfig six_camera_runtime_config()
{
    auto value = four_camera_runtime_config();
    value.cameras.push_back({.id = "CAM05", .enabled = true, .frame_rate = 10.0});
    value.cameras.push_back({.id = "CAM06", .enabled = true, .frame_rate = 10.0});
    value.acquisition.frame_pool_capacity = 768U;
    return value;
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

struct ThreadRegistrationProbe final
{
    explicit ThreadRegistrationProbe(std::atomic_int& alive) : alive_(alive)
    {
        ++alive_;
    }
    ~ThreadRegistrationProbe()
    {
        --alive_;
    }
    std::atomic_int& alive_;
};

template <typename Rep, typename Period>
FrameView frame(const std::uint64_t sequence, const std::chrono::duration<Rep, Period> offset,
                std::string camera_id = "CAM01")
{
    auto buffer = std::make_shared<FrameBuffer>(16U);
    for (std::size_t index = 0U; index < 16U; ++index)
        buffer->writable_bytes()[index] = static_cast<std::byte>((sequence + index) & 0xffU);
    if (!buffer->set_size(16U))
        throw std::runtime_error{"frame size"};
    const auto wall = WallClockTime{std::chrono::sys_days{std::chrono::year{2026} / 8 / 4}};
    auto view = make_frame_view(
        {.camera_id = std::move(camera_id),
         .camera_frame_number = 1000U + sequence,
         .sequence_number = sequence,
         .received_monotonic_time =
             MonotonicTime{std::chrono::duration_cast<MonotonicTime::duration>(offset)},
         .received_wall_clock_time =
             wall + std::chrono::duration_cast<WallClockTime::duration>(offset),
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
    std::atomic_bool collecting_visible{};
    auto runtime = EventRuntime::create({.configuration = runtime_config(),
                                         .event_root = event_root,
                                         .database = shared_database,
                                         .frame_queue_capacity = 64U,
                                         .error_observer = [&](const Error&) { ++errors; },
                                         .lifecycle_observer =
                                             [&](const auto& event) {
                                                 if (event.persistence_state == "Collecting")
                                                     collecting_visible.store(true);
                                             }});
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
    const auto visibility_started = std::chrono::steady_clock::now();
    ASSERT_TRUE(runtime.value()->submit_frame(frame(11U, 1100ms)));
    ASSERT_TRUE(wait_until([&] { return collecting_visible.load(); }));
    EXPECT_LT(std::chrono::steady_clock::now() - visibility_started, 1s);
    auto collecting_page = shared_database->query_events({.limit = 10U});
    ASSERT_TRUE(collecting_page);
    ASSERT_EQ(collecting_page.value().events.size(), 1U);
    EXPECT_EQ(collecting_page.value().events.front().persistence_state, "Collecting");
    EXPECT_FALSE(collecting_page.value().events.front().artifacts_available);
    EXPECT_TRUE(collecting_page.value().events.front().relative_directory.empty());
    for (std::uint64_t sequence = 12U; sequence <= 23U; ++sequence)
        ASSERT_TRUE(runtime.value()->submit_frame(
            frame(sequence, std::chrono::milliseconds{static_cast<int>(sequence * 100U)})));

    EventQueryPage page;
    for (std::size_t attempt = 0U; attempt < 200U; ++attempt)
    {
        auto queried = shared_database->query_events({.limit = 10U});
        ASSERT_TRUE(queried);
        page = std::move(queried).value();
        if (!page.events.empty() && page.events.front().persistence_state == "Committed")
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
    EXPECT_EQ(snapshot.processed_frames + snapshot.sampled_skipped_frames, 23U);
    EXPECT_GT(snapshot.sampled_skipped_frames, 0U);
    EXPECT_EQ(snapshot.skipped_frames, 0U);
    EXPECT_EQ(snapshot.missed_processing_slots, 0U);
    EXPECT_EQ(snapshot.rejected_frames, 0U);
    EXPECT_EQ(snapshot.events_committed, 1U);
    EXPECT_EQ(snapshot.frame_queue_capacity, 2U);
    EXPECT_EQ(snapshot.persistence_queue_capacity, 8U);
    EXPECT_EQ(snapshot.persistence_queue_depth, 0U);
    EXPECT_EQ(snapshot.persistence_active_events, 0U);
    EXPECT_GT(snapshot.persistence_last_write_bytes, 0U);
    EXPECT_GT(snapshot.persistence_last_write_mib_per_second, 0.0);
    EXPECT_EQ(errors.load(), 0U);
}

TEST(EventRuntimeIntegration, FrozenCandidateMappingSurvivesUntilConfirmationAndNextIdIsUnique)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto behavior = std::make_shared<ControlledDetectorBehavior>();
    behavior->triggered_cameras = {"CAM01"};
    behavior->default_trigger_confidence = 0.7;
    behavior->confidence_by_frame[{"CAM01", 3U}] = 0.9;
    behavior->confidence_by_frame[{"CAM01", 4U}] = 0.9;
    auto configuration = runtime_config();
    configuration.algorithm.enabled = true;
    configuration.algorithm.type = "controlled-runtime-test";
    configuration.algorithm.candidate_threshold = 0.6;
    configuration.algorithm.confirmation_threshold = 0.8;
    configuration.algorithm.confirmation_duration_ms = 10U;
    configuration.algorithm.cooldown_ms = 0U;
    auto runtime = EventRuntime::create(
        {.configuration = configuration,
         .event_root = event_root,
         .database = shared_database,
         .frame_queue_capacity = 64U,
         .detector_registry_configurer = controlled_detector_registration(behavior)});
    ASSERT_TRUE(runtime) << runtime.error().message;
    ASSERT_TRUE(runtime.value()->start());

    ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms)));
    ASSERT_TRUE(wait_until([&] { return runtime.value()->snapshot().events_started == 1U; }));
    auto first_page = shared_database->query_events({.limit = 10U});
    ASSERT_TRUE(first_page);
    ASSERT_EQ(first_page.value().events.size(), 1U);
    const auto first_id = first_page.value().events.front().event_id;

    ASSERT_TRUE(runtime.value()->submit_frame(frame(2U, 1200ms)));
    ASSERT_TRUE(wait_until([&] { return runtime.value()->snapshot().events_frozen == 1U; }));
    ASSERT_TRUE(runtime.value()->submit_frame(frame(3U, 1300ms)));
    ASSERT_TRUE(wait_until([&] {
        std::scoped_lock lock{behavior->mutex};
        return std::ranges::find(behavior->completed_sequences["CAM01"], 3U) !=
               behavior->completed_sequences["CAM01"].end();
    }));
    ASSERT_TRUE(runtime.value()->submit_frame(frame(4U, 1320ms)));
    ASSERT_TRUE(wait_until([&] {
        auto record = shared_database->get_event(first_id);
        return record && record.value().decision_state == "Confirmed";
    }));
    EXPECT_EQ(runtime.value()->snapshot().events_started, 1U);
    for (std::uint64_t sequence = 5U; sequence <= 20U; ++sequence)
    {
        ASSERT_TRUE(runtime.value()->submit_frame(
            frame(sequence, std::chrono::milliseconds{1400U + (sequence - 5U) * 100U})));
        ASSERT_TRUE(wait_until([&] {
            std::scoped_lock lock{behavior->mutex};
            return std::ranges::find(behavior->completed_sequences["CAM01"], sequence) !=
                   behavior->completed_sequences["CAM01"].end();
        }));
    }
    ASSERT_TRUE(wait_until([&] {
        auto lane = runtime.value()->algorithm_snapshot("CAM01");
        return lane && lane.value().metrics.rearm_pending &&
               lane.value().metrics.rearm_suppressed_results > 0U;
    }));
    EXPECT_EQ(runtime.value()->snapshot().events_started, 1U);

    {
        std::scoped_lock lock{behavior->mutex};
        behavior->triggered_cameras.clear();
    }
    for (std::uint64_t sequence = 21U; sequence <= 26U; ++sequence)
    {
        ASSERT_TRUE(runtime.value()->submit_frame(
            frame(sequence, std::chrono::milliseconds{3000U + (sequence - 21U) * 100U})));
        ASSERT_TRUE(wait_until([&] {
            std::scoped_lock lock{behavior->mutex};
            return std::ranges::find(behavior->completed_sequences["CAM01"], sequence) !=
                   behavior->completed_sequences["CAM01"].end();
        }));
    }
    ASSERT_TRUE(wait_until([&] {
        auto lane = runtime.value()->algorithm_snapshot("CAM01");
        return lane && !lane.value().metrics.rearm_pending;
    }));
    {
        std::scoped_lock lock{behavior->mutex};
        behavior->triggered_cameras.insert("CAM01");
    }
    ASSERT_TRUE(runtime.value()->submit_frame(frame(27U, 3600ms)));
    ASSERT_TRUE(wait_until([&] { return runtime.value()->snapshot().events_started == 2U; }));
    auto second_page = shared_database->query_events({.limit = 10U});
    ASSERT_TRUE(second_page);
    ASSERT_EQ(second_page.value().events.size(), 2U);
    std::set<std::string> event_ids;
    for (const auto& event : second_page.value().events)
        event_ids.insert(event.event_id);
    EXPECT_EQ(event_ids.size(), 2U);
    EXPECT_TRUE(event_ids.contains(first_id));

    ASSERT_TRUE(runtime.value()->submit_frame(frame(28U, 4700ms)));
    ASSERT_TRUE(wait_until([&] { return runtime.value()->snapshot().events_frozen == 2U; }));
    EXPECT_EQ(runtime.value()->snapshot().events_started, 2U);
    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(EventRuntimeIntegration, FrozenCandidateTimeoutDoesNotReuseIdAndLaterCandidateIsUnique)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto behavior = std::make_shared<ControlledDetectorBehavior>();
    behavior->triggered_cameras = {"CAM01"};
    behavior->default_trigger_confidence = 0.7;
    auto configuration = runtime_config();
    configuration.algorithm.enabled = true;
    configuration.algorithm.type = "controlled-runtime-test";
    configuration.algorithm.candidate_threshold = 0.6;
    configuration.algorithm.confirmation_threshold = 0.8;
    configuration.algorithm.confirmation_duration_ms = 10U;
    configuration.algorithm.cooldown_ms = 1000U;
    auto runtime = EventRuntime::create(
        {.configuration = configuration,
         .event_root = event_root,
         .database = shared_database,
         .frame_queue_capacity = 64U,
         .detector_registry_configurer = controlled_detector_registration(behavior)});
    ASSERT_TRUE(runtime) << runtime.error().message;
    ASSERT_TRUE(runtime.value()->start());

    ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms)));
    ASSERT_TRUE(wait_until([&] { return runtime.value()->snapshot().events_started == 1U; }));
    auto first_page = shared_database->query_events({.limit = 10U});
    ASSERT_TRUE(first_page);
    ASSERT_EQ(first_page.value().events.size(), 1U);
    const auto first_id = first_page.value().events.front().event_id;
    ASSERT_TRUE(runtime.value()->submit_frame(frame(2U, 1200ms)));
    ASSERT_TRUE(wait_until([&] { return runtime.value()->snapshot().events_frozen == 1U; }));

    ASSERT_TRUE(runtime.value()->submit_frame(frame(3U, 3100ms)));
    ASSERT_TRUE(wait_until([&] {
        auto record = shared_database->get_event(first_id);
        return record && record.value().decision_state == "Timeout";
    }));
    EXPECT_EQ(runtime.value()->snapshot().events_started, 1U);

    {
        std::scoped_lock lock{behavior->mutex};
        behavior->triggered_cameras.clear();
    }
    for (std::uint64_t sequence = 4U; sequence <= 13U; ++sequence)
    {
        ASSERT_TRUE(runtime.value()->submit_frame(
            frame(sequence, std::chrono::milliseconds{3200U + (sequence - 4U) * 100U})));
        ASSERT_TRUE(wait_until([&] {
            std::scoped_lock lock{behavior->mutex};
            return std::ranges::find(behavior->completed_sequences["CAM01"], sequence) !=
                   behavior->completed_sequences["CAM01"].end();
        }));
    }
    ASSERT_TRUE(wait_until([&] {
        auto lane = runtime.value()->algorithm_snapshot("CAM01");
        return lane && !lane.value().metrics.rearm_pending;
    }));
    {
        std::scoped_lock lock{behavior->mutex};
        behavior->triggered_cameras.insert("CAM01");
    }
    ASSERT_TRUE(runtime.value()->submit_frame(frame(14U, 4200ms)));
    ASSERT_TRUE(wait_until([&] { return runtime.value()->snapshot().events_started == 2U; }));
    auto second_page = shared_database->query_events({.limit = 10U});
    ASSERT_TRUE(second_page);
    ASSERT_EQ(second_page.value().events.size(), 2U);
    EXPECT_NE(second_page.value().events[0].event_id, second_page.value().events[1].event_id);
    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(EventRuntimeIntegration, ExternalConfirmationAfterFreezeUpdatesOriginalAndReleasesMapping)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto behavior = std::make_shared<ControlledDetectorBehavior>();
    behavior->triggered_cameras = {"CAM01"};
    behavior->default_trigger_confidence = 0.9;
    auto configuration = runtime_config();
    configuration.algorithm.enabled = true;
    configuration.algorithm.type = "controlled-runtime-test";
    configuration.algorithm.candidate_threshold = 0.6;
    configuration.algorithm.confirmation_threshold = 0.8;
    configuration.algorithm.confirmation_duration_ms = 10U;
    configuration.algorithm.cooldown_ms = 0U;
    configuration.plant_io.enabled = true;
    auto runtime = EventRuntime::create(
        {.configuration = configuration,
         .event_root = event_root,
         .database = shared_database,
         .frame_queue_capacity = 64U,
         .detector_registry_configurer = controlled_detector_registration(behavior)});
    ASSERT_TRUE(runtime) << runtime.error().message;
    ASSERT_TRUE(runtime.value()->start());

    ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms)));
    ASSERT_TRUE(wait_until([&] { return runtime.value()->snapshot().events_started == 1U; }));
    auto first_page = shared_database->query_events({.limit = 10U});
    ASSERT_TRUE(first_page);
    ASSERT_EQ(first_page.value().events.size(), 1U);
    const auto first_id = first_page.value().events.front().event_id;
    ASSERT_TRUE(runtime.value()->submit_frame(frame(2U, 1200ms)));
    ASSERT_TRUE(wait_until([&] { return runtime.value()->snapshot().events_frozen == 1U; }));

    const auto wall = WallClockTime{std::chrono::sys_days{std::chrono::year{2026} / 8 / 4}};
    ASSERT_TRUE(runtime.value()->update_external_confirmation("CAM01", true, MonotonicTime{1300ms},
                                                              wall + 1300ms));
    ASSERT_TRUE(wait_until([&] {
        auto record = shared_database->get_event(first_id);
        return record && record.value().decision_state == "Confirmed";
    }));
    {
        std::scoped_lock lock{behavior->mutex};
        behavior->triggered_cameras.clear();
    }
    for (std::uint64_t sequence = 3U; sequence <= 8U; ++sequence)
    {
        ASSERT_TRUE(runtime.value()->submit_frame(
            frame(sequence, std::chrono::milliseconds{1400U + (sequence - 3U) * 100U})));
        ASSERT_TRUE(wait_until([&] {
            std::scoped_lock lock{behavior->mutex};
            return std::ranges::find(behavior->completed_sequences["CAM01"], sequence) !=
                   behavior->completed_sequences["CAM01"].end();
        }));
    }
    ASSERT_TRUE(wait_until([&] {
        auto lane = runtime.value()->algorithm_snapshot("CAM01");
        return lane && !lane.value().metrics.rearm_pending;
    }));
    {
        std::scoped_lock lock{behavior->mutex};
        behavior->triggered_cameras.insert("CAM01");
    }
    ASSERT_TRUE(runtime.value()->submit_frame(frame(9U, 2000ms)));
    ASSERT_TRUE(wait_until([&] { return runtime.value()->snapshot().events_started == 2U; }));
    auto second_page = shared_database->query_events({.limit = 10U});
    ASSERT_TRUE(second_page);
    ASSERT_EQ(second_page.value().events.size(), 2U);
    EXPECT_NE(second_page.value().events[0].event_id, second_page.value().events[1].event_id);

    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(EventRuntimeIntegration, DeployableDefaultConfigurationSatisfiesPoolBudget)
{
    const auto configuration_path =
        std::filesystem::path{PAPERBREAK_TEST_SOURCE_DIR}.parent_path() / "config" /
        "default-config.json";
    std::ifstream stream{configuration_path, std::ios::binary};
    ASSERT_TRUE(stream);
    const std::string contents{std::istreambuf_iterator<char>{stream},
                               std::istreambuf_iterator<char>{}};
    auto configuration = config::parse_config(contents, configuration_path.parent_path());
    ASSERT_TRUE(configuration) << configuration.error().message;

    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto runtime = EventRuntime::create({.configuration = std::move(configuration).value(),
                                         .event_root = event_root,
                                         .database = std::move(shared_database)});
    ASSERT_TRUE(runtime) << runtime.error().message;
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
            nvme_snapshot.protected_blocks > 0U && errors.load() > 0U)
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

TEST(EventRuntimeIntegration, LatestWinsSamplingIsNonBlockingAndDoesNotRaiseBacklog)
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
    EXPECT_GT(snapshot.sampled_skipped_frames, 0U);
    EXPECT_EQ(snapshot.processed_frames + snapshot.sampled_skipped_frames,
              snapshot.submitted_frames);
    EXPECT_EQ(snapshot.skipped_frames, 0U);
    EXPECT_EQ(snapshot.missed_processing_slots, 0U);
    EXPECT_EQ(snapshot.backlog_active_lanes, 0U);
    EXPECT_EQ(snapshot.frame_queue_capacity, 4U);
    EXPECT_GE(snapshot.frame_queue_high_watermark, 1U);
    EXPECT_LE(snapshot.frame_queue_high_watermark, snapshot.frame_queue_capacity);
    EXPECT_GE(snapshot.processed_frames, 2U);
    EXPECT_EQ(snapshot.algorithm_state, AlgorithmRuntimeState::active);
}

TEST(EventRuntimeIntegration, ConsecutiveDetectorFailuresKeepDetectingAndAlarmUntilRecovery)
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
    configuration.algorithm.confirmation_duration_ms = 10U;
    auto behavior = std::make_shared<DetectorBehavior>(configuration.algorithm.type, 0ms, 3U);
    std::atomic_uint64_t degraded_errors{};
    std::atomic_uint64_t active_alarm_updates{};
    std::atomic_uint64_t recovered_alarms{};
    std::atomic_uint64_t last_consecutive_failures{};
    std::atomic_uint64_t active_updates_with_error{};
    std::atomic_uint64_t observed_failure_limit{};
    auto runtime = EventRuntime::create(
        {.configuration = configuration,
         .event_root = event_root,
         .database = shared_database,
         .consecutive_failure_limit = 2U,
         .detector_registry_configurer = test_detector_registration(behavior),
         .error_observer =
             [&](const Error& error) {
                 if (error.business_code == "ALGORITHM_DEGRADED")
                     ++degraded_errors;
             },
         .detector_failure_state_observer =
             [&](const AlgorithmDetectorFailureStateChange& change) {
                 if (change.active)
                 {
                     ++active_alarm_updates;
                     last_consecutive_failures.store(change.consecutive_failures);
                     if (change.last_error)
                         ++active_updates_with_error;
                     observed_failure_limit.store(change.failure_limit);
                 }
                 else
                 {
                     ++recovered_alarms;
                 }
             }});
    ASSERT_TRUE(runtime) << runtime.error().message;
    ASSERT_TRUE(runtime.value()->start());

    for (std::uint64_t sequence = 1U; sequence <= 4U; ++sequence)
    {
        ASSERT_TRUE(runtime.value()->submit_frame(
            frame(sequence, std::chrono::milliseconds{sequence * 100U})));
        ASSERT_TRUE(wait_until(
            [&] { return runtime.value()->snapshot().detector_process_calls >= sequence; }));
    }

    const auto recovered = runtime.value()->snapshot();
    EXPECT_EQ(recovered.algorithm_state, AlgorithmRuntimeState::active);
    EXPECT_EQ(recovered.detector_process_calls, 4U);
    EXPECT_EQ(recovered.detector_failures, 3U);
    EXPECT_EQ(recovered.consecutive_detector_failures, 0U);
    EXPECT_EQ(degraded_errors.load(), 0U);
    EXPECT_EQ(active_alarm_updates.load(), 2U);
    EXPECT_EQ(last_consecutive_failures.load(), 3U);
    EXPECT_EQ(active_updates_with_error.load(), 2U);
    EXPECT_EQ(observed_failure_limit.load(), 2U);
    EXPECT_EQ(recovered_alarms.load(), 1U);

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

TEST(EventRuntimeScheduling, LimitsSixtyFpsInputAndAlwaysProcessesTheLatestAvailableFrame)
{
    struct Case final
    {
        config::AlgorithmProcessingFps fps;
    };
    const std::vector<Case> cases{{config::AlgorithmProcessingFps::fps15},
                                  {config::AlgorithmProcessingFps::fps30},
                                  {config::AlgorithmProcessingFps::fps60}};
    TemporaryDirectory temporary;
    for (const auto& test_case : cases)
    {
        const auto fps = static_cast<std::uint32_t>(test_case.fps);
        const auto root = temporary.path() / std::to_wstring(fps);
        auto database =
            EventMetadataDatabase::open({.database_path = root / "database" / "events.db",
                                         .event_root = root / "events",
                                         .backup_directory = root / "backups"});
        ASSERT_TRUE(database);
        std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
        auto behavior = std::make_shared<ControlledDetectorBehavior>();
        auto configuration = runtime_config();
        configuration.algorithm.enabled = true;
        configuration.algorithm.type = "controlled-runtime-test";
        configuration.algorithm.processing_fps = test_case.fps;
        configuration.algorithm.confirmation_duration_ms = 10U;
        std::atomic_int64_t elapsed_ns{};
        auto runtime = EventRuntime::create(
            {.configuration = configuration,
             .event_root = root / "events",
             .database = shared_database,
             .detector_registry_configurer = controlled_detector_registration(behavior),
             .monotonic_now = [&] {
                 return std::chrono::steady_clock::time_point{
                     std::chrono::nanoseconds{elapsed_ns.load()}};
             }});
        ASSERT_TRUE(runtime) << "fps=" << fps;
        ASSERT_TRUE(runtime.value()->start());

        constexpr auto input_period = std::chrono::nanoseconds{16'666'667};
        const auto processing_period = std::chrono::nanoseconds{1'000'000'000LL / fps};
        std::size_t expected_calls = 0U;
        std::chrono::nanoseconds last_expected_start{};
        for (std::uint64_t sequence = 1U; sequence <= 25U; ++sequence)
        {
            const auto input_time = input_period * static_cast<std::int64_t>(sequence - 1U);
            elapsed_ns.store(input_time.count());
            ASSERT_TRUE(runtime.value()->submit_frame(frame(sequence, input_time)));
            if (sequence == 1U || input_time - last_expected_start >= processing_period)
            {
                ++expected_calls;
                last_expected_start = input_time;
                ASSERT_TRUE(wait_until([&] {
                    return runtime.value()->snapshot().detector_process_calls == expected_calls;
                }));
            }
        }

        std::vector<std::uint64_t> sequences;
        {
            std::scoped_lock lock{behavior->mutex};
            sequences = behavior->completed_sequences["CAM01"];
        }
        EXPECT_EQ(sequences.size(), (24U * fps / 60U) + 1U) << "fps=" << fps;
        EXPECT_EQ(sequences.front(), 1U);
        EXPECT_GE(sequences.back(), 24U);
        EXPECT_TRUE(std::ranges::is_sorted(sequences));
        EXPECT_EQ(std::ranges::adjacent_find(sequences), sequences.end());

        const auto calls_before_idle = runtime.value()->snapshot().detector_process_calls;
        elapsed_ns.fetch_add((processing_period * 2).count());
        std::this_thread::sleep_for(10ms);
        const auto lane_before_stop = runtime.value()->algorithm_snapshot("CAM01");
        ASSERT_TRUE(lane_before_stop);
        EXPECT_EQ(runtime.value()->snapshot().detector_process_calls, calls_before_idle);
        EXPECT_EQ(lane_before_stop.value().metrics.configured_processing_fps, fps);
        EXPECT_EQ(lane_before_stop.value().metrics.missed_processing_slots, 0U);
        EXPECT_EQ(lane_before_stop.value().metrics.skipped_frames, 0U);
        EXPECT_FALSE(lane_before_stop.value().metrics.backlog_active);
        runtime.value()->request_stop();
        ASSERT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
        const auto lane_after_stop = runtime.value()->algorithm_snapshot("CAM01");
        ASSERT_TRUE(lane_after_stop);
        EXPECT_EQ(lane_after_stop.value().metrics.processed_frames +
                      lane_after_stop.value().metrics.sampled_skipped_frames,
                  25U);
        std::scoped_lock behavior_lock{behavior->mutex};
        EXPECT_EQ(behavior->completed_sequences["CAM01"].back(), 25U);
    }
}

TEST(EventRuntimeLanes, BlockedCameraDoesNotStopOtherLatestWinsLanes)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto behavior = std::make_shared<ControlledDetectorBehavior>();
    behavior->hold_cam01 = true;
    auto runtime = EventRuntime::create(
        {.configuration = four_camera_runtime_config(),
         .event_root = event_root,
         .database = shared_database,
         .frame_queue_capacity = 8U,
         .detector_registry_configurer = controlled_detector_registration(behavior)});
    ASSERT_TRUE(runtime) << runtime.error().message;
    ASSERT_TRUE(runtime.value()->start());

    ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms, "CAM01")));
    ASSERT_TRUE(wait_until([&] {
        std::scoped_lock lock{behavior->mutex};
        return behavior->active_calls == 1U;
    }));
    for (const auto camera_id : {"CAM02", "CAM03", "CAM04"})
        ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms, camera_id)));
    const bool first_round_completed = wait_until([&] {
        for (const auto camera_id : {"CAM02", "CAM03", "CAM04"})
        {
            const auto lane = runtime.value()->algorithm_snapshot(camera_id);
            if (!lane || lane.value().metrics.processed_frames != 1U)
                return false;
        }
        return true;
    });
    for (const auto camera_id : {"CAM02", "CAM03", "CAM04"})
    {
        ASSERT_TRUE(runtime.value()->submit_frame(frame(2U, 200ms, camera_id)));
        ASSERT_TRUE(runtime.value()->submit_frame(frame(3U, 300ms, camera_id)));
    }
    const bool latest_round_completed = wait_until([&] {
        for (const auto camera_id : {"CAM02", "CAM03", "CAM04"})
        {
            const auto lane = runtime.value()->algorithm_snapshot(camera_id);
            if (!lane || lane.value().metrics.processed_frames != 2U)
                return false;
        }
        return true;
    });
    const auto blocked = runtime.value()->algorithm_snapshot("CAM01");
    {
        std::scoped_lock lock{behavior->mutex};
        behavior->release_cam01 = true;
    }
    behavior->condition.notify_all();
    runtime.value()->request_stop();
    ASSERT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));

    EXPECT_TRUE(first_round_completed);
    EXPECT_TRUE(latest_round_completed);
    ASSERT_TRUE(blocked);
    EXPECT_EQ(blocked.value().metrics.processed_frames, 0U);
    std::scoped_lock behavior_lock{behavior->mutex};
    EXPECT_GE(behavior->maximum_active_calls, 2U);
    for (const auto camera_id : {"CAM02", "CAM03", "CAM04"})
        EXPECT_EQ(behavior->completed_sequences[camera_id], (std::vector<std::uint64_t>{1U, 3U}));
}

TEST(EventRuntimeLanes, NormalLatestWinsSamplingNeverDegradesAnyLane)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto behavior = std::make_shared<ControlledDetectorBehavior>();
    behavior->hold_cam01 = true;
    auto runtime = EventRuntime::create(
        {.configuration = four_camera_runtime_config(),
         .event_root = event_root,
         .database = shared_database,
         .frame_queue_capacity = 1U,
         .consecutive_backlog_limit = 2U,
         .backlog_degrade_window_limit = 1U,
         .detector_registry_configurer = controlled_detector_registration(behavior)});
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value()->start());
    ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms, "CAM01")));
    ASSERT_TRUE(wait_until([&] {
        std::scoped_lock lock{behavior->mutex};
        return behavior->active_calls == 1U;
    }));
    for (std::uint64_t sequence = 2U; sequence <= 4U; ++sequence)
        ASSERT_TRUE(runtime.value()->submit_frame(
            frame(sequence, std::chrono::milliseconds{sequence * 100U}, "CAM01")));
    ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms, "CAM02")));
    ASSERT_TRUE(wait_until([&] {
        auto lane = runtime.value()->algorithm_snapshot("CAM02");
        return lane && lane.value().metrics.processed_frames == 1U;
    }));

    const auto cam01 = runtime.value()->algorithm_snapshot("CAM01");
    const auto cam02 = runtime.value()->algorithm_snapshot("CAM02");
    ASSERT_TRUE(cam01);
    ASSERT_TRUE(cam02);
    EXPECT_EQ(cam01.value().state, AlgorithmRuntimeState::active);
    EXPECT_EQ(cam01.value().metrics.sampled_skipped_frames, 2U);
    EXPECT_EQ(cam01.value().metrics.skipped_frames, 0U);
    EXPECT_EQ(cam01.value().metrics.missed_processing_slots, 0U);
    EXPECT_EQ(cam01.value().metrics.consecutive_backlog_events, 0U);
    EXPECT_EQ(cam02.value().state, AlgorithmRuntimeState::active);
    EXPECT_EQ(cam02.value().metrics.skipped_frames, 0U);
    EXPECT_EQ(cam02.value().metrics.consecutive_backlog_events, 0U);
    EXPECT_EQ(runtime.value()->snapshot().algorithm_state, AlgorithmRuntimeState::active);

    {
        std::scoped_lock lock{behavior->mutex};
        behavior->release_cam01 = true;
    }
    behavior->condition.notify_all();
    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(EventRuntimeLanes, BacklogActivatesOnceAndRecoversAfterHealthyWindows)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto behavior = std::make_shared<ControlledDetectorBehavior>();
    behavior->hold_cam01 = true;
    std::atomic_int64_t elapsed_ms{};
    std::mutex observer_mutex;
    std::vector<AlgorithmBacklogStateChange> changes;
    std::vector<Error> errors;
    auto runtime = EventRuntime::create(
        {.configuration = four_camera_runtime_config(),
         .event_root = event_root,
         .database = shared_database,
         .frame_queue_capacity = 1U,
         .consecutive_backlog_limit = 8U,
         .backlog_window = 1s,
         .detector_registry_configurer = controlled_detector_registration(behavior),
         .error_observer =
             [&](const Error& error) {
                 std::scoped_lock lock{observer_mutex};
                 errors.push_back(error);
             },
         .backlog_state_observer =
             [&](const AlgorithmBacklogStateChange& change) {
                 std::scoped_lock lock{observer_mutex};
                 changes.push_back(change);
             },
         .monotonic_now =
             [&] {
                 return std::chrono::steady_clock::time_point{
                     std::chrono::milliseconds{elapsed_ms.load()}};
             }});
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value()->start());
    ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 0ms, "CAM01")));
    ASSERT_TRUE(wait_until([&] {
        std::scoped_lock lock{behavior->mutex};
        return behavior->active_calls == 1U;
    }));
    for (std::uint64_t sequence = 2U; sequence <= 4U; ++sequence)
        ASSERT_TRUE(runtime.value()->submit_frame(
            frame(sequence, std::chrono::milliseconds{sequence - 1U}, "CAM01")));
    {
        std::scoped_lock lock{observer_mutex};
        EXPECT_TRUE(changes.empty());
        EXPECT_EQ(std::ranges::count_if(errors,
                                        [](const Error& error) {
                                            return error.business_code == "ALGORITHM_QUEUE_BACKLOG";
                                        }),
                  0);
    }

    elapsed_ms.store(250);
    {
        std::scoped_lock lock{behavior->mutex};
        behavior->release_cam01 = true;
    }
    behavior->condition.notify_all();
    ASSERT_TRUE(wait_until([&] {
        const auto lane = runtime.value()->algorithm_snapshot("CAM01");
        return lane && lane.value().metrics.processed_frames == 2U;
    }));
    const auto latency = runtime.value()->algorithm_snapshot("CAM01");
    ASSERT_TRUE(latency);
    EXPECT_EQ(latency.value().metrics.missed_processing_slots, 3U);
    EXPECT_EQ(latency.value().metrics.sampled_skipped_frames, 2U);
    EXPECT_EQ(latency.value().metrics.last_queue_wait_time, 250ms);
    EXPECT_EQ(latency.value().metrics.average_queue_wait_time, 125ms);
    EXPECT_EQ(latency.value().metrics.maximum_queue_wait_time, 250ms);
    EXPECT_EQ(latency.value().metrics.last_end_to_end_time, 247ms);
    EXPECT_EQ(latency.value().metrics.average_end_to_end_time, std::chrono::microseconds{248500});
    EXPECT_EQ(latency.value().metrics.maximum_end_to_end_time, 250ms);
    {
        std::scoped_lock lock{observer_mutex};
        ASSERT_EQ(changes.size(), 1U);
        EXPECT_TRUE(changes.front().active);
        EXPECT_EQ(std::ranges::count_if(errors,
                                        [](const Error& error) {
                                            return error.business_code == "ALGORITHM_QUEUE_BACKLOG";
                                        }),
                  1);
    }
    for (std::uint64_t window = 1U; window <= 6U; ++window)
    {
        elapsed_ms.store(static_cast<std::int64_t>(window * 1000U));
        ASSERT_TRUE(runtime.value()->submit_frame(
            frame(4U + window, std::chrono::milliseconds{(4U + window) * 100U}, "CAM01")));
        ASSERT_TRUE(wait_until([&] {
            const auto lane = runtime.value()->algorithm_snapshot("CAM01");
            return lane && lane.value().metrics.processed_frames == 2U + window;
        }));
    }
    const auto recovered = runtime.value()->algorithm_snapshot("CAM01");
    ASSERT_TRUE(recovered);
    EXPECT_FALSE(recovered.value().metrics.backlog_active);
    EXPECT_EQ(recovered.value().metrics.consecutive_backlog_events, 0U);
    EXPECT_DOUBLE_EQ(recovered.value().metrics.input_fps, 1.0);
    EXPECT_DOUBLE_EQ(recovered.value().metrics.processed_fps, 1.0);
    EXPECT_DOUBLE_EQ(recovered.value().metrics.skipped_ratio, 0.0);
    {
        std::scoped_lock lock{observer_mutex};
        ASSERT_EQ(changes.size(), 2U);
        EXPECT_FALSE(changes.back().active);
        EXPECT_EQ(changes.back().camera_id, "CAM01");
    }
    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(EventRuntimeLanes, FiveBadBacklogWindowsDegradeOnlyTheOverloadedCamera)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto behavior = std::make_shared<ControlledDetectorBehavior>();
    std::atomic_int64_t elapsed_ms{};
    behavior->advance_clock = [&](const std::string_view camera_id) {
        if (camera_id == "CAM01")
            elapsed_ms.fetch_add(80);
    };
    std::atomic_uint64_t backlog_warnings{};
    std::atomic_uint64_t degraded_errors{};
    std::atomic_uint64_t backlog_activations{};
    auto configuration = four_camera_runtime_config();
    configuration.algorithm.processing_fps = config::AlgorithmProcessingFps::fps60;
    auto runtime = EventRuntime::create(
        {.configuration = configuration,
         .event_root = event_root,
         .database = shared_database,
         .frame_queue_capacity = 1U,
         .consecutive_backlog_limit = 1U,
         .backlog_window = 100ms,
         .backlog_degrade_window_limit = 5U,
         .detector_registry_configurer = controlled_detector_registration(behavior),
         .error_observer =
             [&](const Error& error) {
                 if (error.business_code == "ALGORITHM_QUEUE_BACKLOG")
                     ++backlog_warnings;
                 if (error.business_code == "ALGORITHM_DEGRADED")
                     ++degraded_errors;
             },
         .backlog_state_observer =
             [&](const AlgorithmBacklogStateChange& change) {
                 if (change.active)
                     ++backlog_activations;
             },
         .monotonic_now =
             [&] {
                 return std::chrono::steady_clock::time_point{
                     std::chrono::milliseconds{elapsed_ms.load()}};
             }});
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value()->start());
    std::uint64_t sequence = 1U;
    ASSERT_TRUE(runtime.value()->submit_frame(frame(sequence++, 0ms, "CAM01")));
    ASSERT_TRUE(
        wait_until([&] { return runtime.value()->snapshot().detector_process_calls == 1U; }));
    for (std::uint64_t window = 1U; window <= 5U; ++window)
    {
        elapsed_ms.store(static_cast<std::int64_t>(window * 100U));
        ASSERT_TRUE(runtime.value()->submit_frame(
            frame(sequence++, std::chrono::milliseconds{window * 100U}, "CAM01")));
        if (window < 5U)
            ASSERT_TRUE(wait_until(
                [&] { return runtime.value()->snapshot().detector_process_calls == window + 1U; }));
    }
    const auto cam01 = runtime.value()->algorithm_snapshot("CAM01");
    ASSERT_TRUE(cam01);
    EXPECT_EQ(cam01.value().state, AlgorithmRuntimeState::manual_trigger_only);
    EXPECT_EQ(cam01.value().metrics.consecutive_bad_backlog_windows, 5U);
    EXPECT_GE(cam01.value().metrics.missed_processing_slots, 20U);
    EXPECT_EQ(backlog_warnings.load(), 1U);
    EXPECT_EQ(backlog_activations.load(), 1U);
    EXPECT_EQ(degraded_errors.load(), 1U);

    ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms, "CAM02")));
    ASSERT_TRUE(wait_until([&] {
        const auto lane = runtime.value()->algorithm_snapshot("CAM02");
        return lane && lane.value().metrics.processed_frames == 1U;
    }));
    const auto cam02 = runtime.value()->algorithm_snapshot("CAM02");
    ASSERT_TRUE(cam02);
    EXPECT_EQ(cam02.value().state, AlgorithmRuntimeState::active);
    EXPECT_EQ(cam02.value().metrics.skipped_frames, 0U);

    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(EventRuntimeLanes, DetectorFailureAlarmsRemainLaneLocalAndReconfigureClearsThem)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto configuration = four_camera_runtime_config();
    auto behavior = std::make_shared<ControlledDetectorBehavior>();
    behavior->failures_remaining = {{"CAM01", 1U}, {"CAM02", 1U}, {"CAM03", 1U}, {"CAM04", 1U}};
    std::atomic_uint64_t active_alarm_updates{};
    std::atomic_uint64_t recovered_alarms{};
    auto runtime = EventRuntime::create(
        {.configuration = configuration,
         .event_root = event_root,
         .database = shared_database,
         .consecutive_failure_limit = 1U,
         .detector_registry_configurer = controlled_detector_registration(behavior),
         .detector_failure_state_observer = [&](const AlgorithmDetectorFailureStateChange& change) {
             if (change.active)
                 ++active_alarm_updates;
             else
                 ++recovered_alarms;
         }});
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value()->start());

    for (const auto* camera_id : {"CAM01", "CAM02", "CAM03", "CAM04"})
        ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms, camera_id)));
    ASSERT_TRUE(
        wait_until([&] { return runtime.value()->snapshot().detector_process_calls == 4U; }));
    EXPECT_EQ(runtime.value()->snapshot().algorithm_state, AlgorithmRuntimeState::active);
    EXPECT_EQ(active_alarm_updates.load(), 4U);
    for (const auto& lane : runtime.value()->algorithm_snapshots())
    {
        EXPECT_EQ(lane.state, AlgorithmRuntimeState::active);
        EXPECT_EQ(lane.metrics.detector_failures, 1U);
        EXPECT_EQ(lane.metrics.consecutive_detector_failures, 1U);
    }

    ASSERT_TRUE(runtime.value()->submit_frame(frame(2U, 200ms, "CAM01")));
    ASSERT_TRUE(wait_until([&] { return recovered_alarms.load() == 1U; }));
    const auto recovered_cam01 = runtime.value()->algorithm_snapshot("CAM01");
    ASSERT_TRUE(recovered_cam01);
    EXPECT_EQ(recovered_cam01.value().metrics.consecutive_detector_failures, 0U);

    configuration.config_revision += 1U;
    ASSERT_TRUE(runtime.value()->reconfigure(configuration));
    EXPECT_EQ(runtime.value()->snapshot().algorithm_state, AlgorithmRuntimeState::active);
    EXPECT_EQ(recovered_alarms.load(), 4U);
    for (const auto& lane : runtime.value()->algorithm_snapshots())
        EXPECT_EQ(lane.state, AlgorithmRuntimeState::active);
    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(EventRuntimeLanes, SafeWatermarkOrdersSlowOlderResultBeforeFastNewerResult)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto behavior = std::make_shared<ControlledDetectorBehavior>();
    behavior->hold_cam01 = true;
    behavior->triggered_cameras = {"CAM01", "CAM02"};
    auto runtime = EventRuntime::create(
        {.configuration = four_camera_runtime_config(),
         .event_root = event_root,
         .database = shared_database,
         .detector_registry_configurer = controlled_detector_registration(behavior)});
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value()->start());
    ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms, "CAM01")));
    ASSERT_TRUE(wait_until([&] {
        std::scoped_lock lock{behavior->mutex};
        return behavior->active_calls == 1U;
    }));
    ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 200ms, "CAM02")));
    ASSERT_TRUE(wait_until([&] {
        auto lane = runtime.value()->algorithm_snapshot("CAM02");
        return lane && lane.value().metrics.processed_frames == 1U;
    }));
    EXPECT_EQ(runtime.value()->snapshot().events_started, 0U);
    {
        std::scoped_lock lock{behavior->mutex};
        behavior->release_cam01 = true;
    }
    behavior->condition.notify_all();
    ASSERT_TRUE(wait_until([&] {
        const auto snapshot = runtime.value()->snapshot();
        return snapshot.events_started == 1U && snapshot.candidates_created == 2U;
    }));
    runtime.value()->request_stop();
    ASSERT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));

    auto page = shared_database->query_events({.limit = 10U});
    ASSERT_TRUE(page);
    ASSERT_EQ(page.value().events.size(), 1U);
    EXPECT_EQ(page.value().events.front().trigger_camera_id, "CAM01");
}

TEST(EventRuntimeLanes, ResultQueueOverflowIsNonBlockingAndDegradesOnlyRejectedSources)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto behavior = std::make_shared<ControlledDetectorBehavior>();
    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    bool release_consumer = false;
    std::mutex errors_mutex;
    std::vector<Error> errors;
    auto runtime = EventRuntime::create(
        {.configuration = four_camera_runtime_config(),
         .event_root = event_root,
         .database = shared_database,
         .result_queue_capacity = 1U,
         .detector_registry_configurer = controlled_detector_registration(behavior),
         .error_observer =
             [&](const Error& error) {
                 std::scoped_lock lock{errors_mutex};
                 errors.push_back(error);
             },
         .result_consumer_start_gate =
             [&] {
                 std::unique_lock lock{gate_mutex};
                 gate_condition.wait(lock, [&] { return release_consumer; });
             }});
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value()->start());
    const auto started = std::chrono::steady_clock::now();
    for (const auto camera_id : {"CAM01", "CAM02", "CAM03", "CAM04"})
        ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms, camera_id)));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 100ms);
    ASSERT_TRUE(
        wait_until([&] { return runtime.value()->snapshot().result_queue_rejected >= 1U; }));
    const auto aggregate = runtime.value()->snapshot();
    EXPECT_EQ(aggregate.result_queue_capacity, 1U);
    EXPECT_EQ(aggregate.result_queue_high_watermark, 1U);
    EXPECT_GE(aggregate.result_queue_rejected, 1U);
    EXPECT_EQ(aggregate.algorithm_state, AlgorithmRuntimeState::partially_degraded);
    {
        std::scoped_lock lock{errors_mutex};
        EXPECT_TRUE(std::ranges::any_of(errors, [](const Error& error) {
            return error.business_code == "ALGORITHM_RESULT_QUEUE_FULL" &&
                   error.source_id.has_value() && !error.source_id->empty();
        }));
    }
    {
        std::scoped_lock lock{gate_mutex};
        release_consumer = true;
    }
    gate_condition.notify_all();
    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(EventRuntimeLanes, PartialThreadPreparationFailureKeepsOldRuntimeActive)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto behavior = std::make_shared<ControlledDetectorBehavior>();
    std::atomic_bool fail_cam02{};
    auto configuration = four_camera_runtime_config();
    auto runtime = EventRuntime::create(
        {.configuration = configuration,
         .event_root = event_root,
         .database = shared_database,
         .detector_registry_configurer = controlled_detector_registration(behavior),
         .thread_start_gate = [&](const std::string_view name) {
             if (fail_cam02.load() && name == "algorithm-worker-cam02")
                 return Result<void>::failure(make_error("SYS_INTERNAL_ERROR", Severity::critical,
                                                         "注入的线程创建失败", "event",
                                                         "event.test.threadStart"));
             return Result<void>::success();
         }});
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value()->start());
    ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms, "CAM01")));
    ASSERT_TRUE(wait_until([&] {
        auto lane = runtime.value()->algorithm_snapshot("CAM01");
        return lane && lane.value().metrics.processed_frames == 1U;
    }));

    fail_cam02.store(true);
    configuration.config_revision += 1U;
    auto rejected = runtime.value()->reconfigure(configuration);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().business_code, "SYS_INTERNAL_ERROR");
    EXPECT_TRUE(runtime.value()->snapshot().accepting);
    EXPECT_EQ(runtime.value()->algorithm_snapshot("CAM01").value().config_revision, 7U);
    ASSERT_TRUE(runtime.value()->submit_frame(frame(2U, 200ms, "CAM01")));
    ASSERT_TRUE(wait_until([&] {
        auto lane = runtime.value()->algorithm_snapshot("CAM01");
        return lane && lane.value().metrics.processed_frames == 2U;
    }));
    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));

    fail_cam02.store(true);
    auto startup_failure = EventRuntime::create(
        {.configuration = four_camera_runtime_config(),
         .event_root = temporary.path() / "startup-events",
         .database = shared_database,
         .detector_registry_configurer = controlled_detector_registration(behavior),
         .thread_start_gate = [&](const std::string_view name) {
             if (name == "algorithm-worker-cam02")
                 return Result<void>::failure(make_error("SYS_INTERNAL_ERROR", Severity::critical,
                                                         "注入的线程创建失败", "event",
                                                         "event.test.threadStart"));
             return Result<void>::success();
         }});
    ASSERT_TRUE(startup_failure);
    auto not_started = startup_failure.value()->start();
    ASSERT_FALSE(not_started);
    EXPECT_FALSE(startup_failure.value()->snapshot().started);
    EXPECT_FALSE(startup_failure.value()->snapshot().accepting);
}

TEST(EventRuntimeLanes, HotReconfigurePreservesRearmLatchAndFailedAttemptLeavesItUntouched)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto behavior = std::make_shared<ControlledDetectorBehavior>();
    behavior->triggered_cameras = {"CAM01"};
    auto configuration = runtime_config();
    configuration.algorithm.enabled = true;
    configuration.algorithm.type = "controlled-runtime-test";
    configuration.algorithm.confirmation_duration_ms = 10U;
    configuration.algorithm.rearm_duration_ms = 500U;
    std::atomic_bool fail_worker{};
    auto runtime = EventRuntime::create(
        {.configuration = configuration,
         .event_root = event_root,
         .database = shared_database,
         .detector_registry_configurer = controlled_detector_registration(behavior),
         .thread_start_gate = [&](const std::string_view name) {
             if (fail_worker.load() && name == "algorithm-worker-cam01")
                 return Result<void>::failure(make_error("SYS_INTERNAL_ERROR", Severity::critical,
                                                         "注入的线程创建失败", "event",
                                                         "event.test.threadStart"));
             return Result<void>::success();
         }});
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value()->start());
    for (const auto [sequence, time] :
         std::array{std::pair{1U, 0ms}, std::pair{2U, 100ms}, std::pair{3U, 200ms}})
    {
        ASSERT_TRUE(runtime.value()->submit_frame(frame(sequence, time)));
        ASSERT_TRUE(wait_until([&] {
            std::scoped_lock lock{behavior->mutex};
            return std::ranges::find(behavior->completed_sequences["CAM01"], sequence) !=
                   behavior->completed_sequences["CAM01"].end();
        }));
    }
    ASSERT_TRUE(wait_until([&] {
        auto lane = runtime.value()->algorithm_snapshot("CAM01");
        return lane && lane.value().metrics.rearm_pending &&
               lane.value().metrics.rearm_suppressed_results == 1U;
    }));
    EXPECT_EQ(runtime.value()->snapshot().candidates_created, 1U);

    configuration.config_revision += 1U;
    ASSERT_TRUE(runtime.value()->reconfigure(configuration));
    auto after_success = runtime.value()->algorithm_snapshot("CAM01");
    ASSERT_TRUE(after_success);
    EXPECT_TRUE(after_success.value().metrics.rearm_pending);
    EXPECT_EQ(after_success.value().metrics.rearm_suppressed_results, 1U);
    ASSERT_TRUE(runtime.value()->submit_frame(frame(4U, 300ms)));
    ASSERT_TRUE(wait_until([&] {
        auto lane = runtime.value()->algorithm_snapshot("CAM01");
        return lane && lane.value().metrics.rearm_suppressed_results == 2U;
    }));
    EXPECT_EQ(runtime.value()->algorithm_snapshot("CAM01").value().metrics.candidates_created, 1U);

    fail_worker.store(true);
    configuration.config_revision += 1U;
    auto failed = runtime.value()->reconfigure(configuration);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().business_code, "SYS_INTERNAL_ERROR");
    auto after_failure = runtime.value()->algorithm_snapshot("CAM01");
    ASSERT_TRUE(after_failure);
    EXPECT_TRUE(after_failure.value().metrics.rearm_pending);
    EXPECT_EQ(after_failure.value().metrics.rearm_suppressed_results, 2U);
    EXPECT_EQ(after_failure.value().config_revision, configuration.config_revision - 1U);
    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(EventRuntimeLanes, SixCameraContinuousAnomalyCreatesOneSourcePerCameraAndOneEvent)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto behavior = std::make_shared<ControlledDetectorBehavior>();
    behavior->triggered_cameras = {"CAM01", "CAM02", "CAM03", "CAM04", "CAM05", "CAM06"};
    auto configuration = six_camera_runtime_config();
    configuration.algorithm.rearm_duration_ms = 500U;
    auto runtime = EventRuntime::create(
        {.configuration = configuration,
         .event_root = event_root,
         .database = shared_database,
         .detector_registry_configurer = controlled_detector_registration(behavior)});
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value()->start());

    for (const auto camera_id : {"CAM01", "CAM02", "CAM03", "CAM04", "CAM05", "CAM06"})
        ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 0ms, camera_id)));
    ASSERT_TRUE(wait_until(
        [&] { return runtime.value()->snapshot().candidates_created == camera_slot_count; }));
    for (const auto camera_id : {"CAM01", "CAM02", "CAM03", "CAM04", "CAM05", "CAM06"})
        ASSERT_TRUE(runtime.value()->submit_frame(frame(2U, 100ms, camera_id)));
    ASSERT_TRUE(wait_until(
        [&] { return runtime.value()->snapshot().confirmed_events == camera_slot_count; }));
    for (const auto camera_id : {"CAM01", "CAM02", "CAM03", "CAM04", "CAM05", "CAM06"})
        ASSERT_TRUE(runtime.value()->submit_frame(frame(3U, 200ms, camera_id)));
    ASSERT_TRUE(wait_until([&] {
        return std::ranges::all_of(runtime.value()->algorithm_snapshots(), [](const auto& lane) {
            return lane.metrics.rearm_pending && lane.metrics.rearm_suppressed_results == 1U;
        });
    }));
    const auto aggregate = runtime.value()->snapshot();
    EXPECT_EQ(aggregate.candidates_created, camera_slot_count);
    EXPECT_EQ(aggregate.events_started, 1U);
    EXPECT_EQ(aggregate.rearm_pending_lanes, camera_slot_count);
    EXPECT_EQ(aggregate.rearm_suppressed_results, camera_slot_count);
    auto page = shared_database->query_events({.limit = 10U});
    ASSERT_TRUE(page);
    ASSERT_EQ(page.value().events.size(), 1U);
    EXPECT_EQ(page.value().events.front().trigger_count, camera_slot_count);
    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(EventRuntimeLanes, SingleCameraContinuousAnomalyPersistsOneTriggerWithoutCapacityFailure)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto behavior = std::make_shared<ControlledDetectorBehavior>();
    behavior->triggered_cameras = {"CAM01"};
    auto configuration = runtime_config();
    configuration.algorithm.enabled = true;
    configuration.algorithm.type = "controlled-runtime-test";
    configuration.algorithm.confirmation_duration_ms = 10U;
    configuration.algorithm.rearm_duration_ms = 500U;
    auto runtime = EventRuntime::create(
        {.configuration = configuration,
         .event_root = event_root,
         .database = shared_database,
         .detector_registry_configurer = controlled_detector_registration(behavior)});
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value()->start());

    for (std::uint64_t sequence = 1U; sequence <= 40U; ++sequence)
    {
        ASSERT_TRUE(runtime.value()->submit_frame(
            frame(sequence, std::chrono::milliseconds{(sequence - 1U) * 100U})));
        ASSERT_TRUE(wait_until([&] {
            std::scoped_lock lock{behavior->mutex};
            return std::ranges::find(behavior->completed_sequences["CAM01"], sequence) !=
                   behavior->completed_sequences["CAM01"].end();
        }));
    }
    ASSERT_TRUE(wait_until([&] {
        auto lane = runtime.value()->algorithm_snapshot("CAM01");
        return lane && lane.value().metrics.rearm_pending &&
               lane.value().metrics.rearm_suppressed_results == 38U;
    }));
    const auto aggregate = runtime.value()->snapshot();
    EXPECT_EQ(aggregate.candidates_created, 1U);
    EXPECT_EQ(aggregate.events_started, 1U);
    auto page = shared_database->query_events({.limit = 10U});
    ASSERT_TRUE(page);
    ASSERT_EQ(page.value().events.size(), 1U);
    EXPECT_EQ(page.value().events.front().trigger_count, 1U);
    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}

TEST(EventRuntimeLanes, RepeatedStartStopLeavesNoRegisteredRuntimeThreads)
{
    TemporaryDirectory temporary;
    std::atomic_int alive_threads{};
    std::mutex registration_names_mutex;
    std::vector<std::string> registration_names;
    for (std::size_t iteration = 0U; iteration < 3U; ++iteration)
    {
        const auto root = temporary.path() / std::to_wstring(iteration);
        auto database =
            EventMetadataDatabase::open({.database_path = root / "database" / "events.db",
                                         .event_root = root / "events",
                                         .backup_directory = root / "backups"});
        ASSERT_TRUE(database);
        std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
        auto runtime =
            EventRuntime::create({.configuration = runtime_config(),
                                  .event_root = root / "events",
                                  .database = shared_database,
                                  .register_thread = [&](const std::string_view name) {
                                      {
                                          std::scoped_lock lock{registration_names_mutex};
                                          registration_names.emplace_back(name);
                                      }
                                      return std::static_pointer_cast<void>(
                                          std::make_shared<ThreadRegistrationProbe>(alive_threads));
                                  }});
        ASSERT_TRUE(runtime);
        ASSERT_TRUE(runtime.value()->start());
        ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms)));
        runtime.value()->request_stop();
        ASSERT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
        EXPECT_EQ(alive_threads.load(), 0);
    }
    std::scoped_lock lock{registration_names_mutex};
    EXPECT_EQ(std::ranges::count(registration_names, "event-processing"), 3);
    EXPECT_EQ(std::ranges::count(registration_names, "algorithm-worker-cam01"), 3);
}

TEST(EventRuntimeLanes, PublishesAggregateAndPerCameraMonitoringMetrics)
{
    TemporaryDirectory temporary;
    const auto event_root = temporary.path() / "events";
    auto database =
        EventMetadataDatabase::open({.database_path = temporary.path() / "database" / "events.db",
                                     .event_root = event_root,
                                     .backup_directory = temporary.path() / "backups"});
    ASSERT_TRUE(database);
    std::shared_ptr<EventMetadataDatabase> shared_database{std::move(database).value()};
    auto behavior = std::make_shared<ControlledDetectorBehavior>();
    auto runtime = EventRuntime::create(
        {.configuration = four_camera_runtime_config(),
         .event_root = event_root,
         .database = shared_database,
         .detector_registry_configurer = controlled_detector_registration(behavior)});
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value()->start());
    ASSERT_TRUE(runtime.value()->submit_frame(frame(1U, 100ms, "CAM02")));
    ASSERT_TRUE(wait_until([&] {
        auto lane = runtime.value()->algorithm_snapshot("CAM02");
        return lane && lane.value().metrics.processed_frames == 1U;
    }));

    auto source = make_algorithm_metric_source(runtime.value());
    auto collected = source->collect({});
    ASSERT_TRUE(collected);
    std::set<std::string> names;
    for (const auto& point : collected.value())
        names.insert(point.name);
    EXPECT_TRUE(names.contains("algorithm.state"));
    EXPECT_TRUE(names.contains("algorithm.result_queue.capacity"));
    EXPECT_TRUE(names.contains("algorithm.queue_wait.average_ms"));
    EXPECT_TRUE(names.contains("algorithm.end_to_end.maximum_ms"));
    EXPECT_TRUE(names.contains("algorithm.input_fps"));
    EXPECT_TRUE(names.contains("algorithm.processed_fps"));
    EXPECT_TRUE(names.contains("algorithm.skipped_ratio"));
    EXPECT_TRUE(names.contains("algorithm.sampled_skipped_frames_total"));
    EXPECT_TRUE(names.contains("algorithm.missed_processing_slots_total"));
    EXPECT_TRUE(names.contains("algorithm.configured_processing_fps"));
    EXPECT_TRUE(names.contains("algorithm.rearm_pending_lanes"));
    EXPECT_TRUE(names.contains("algorithm.rearm_suppressed_results_total"));
    for (const auto camera_id : {"CAM01", "CAM02", "CAM03", "CAM04"})
    {
        const std::string prefix = "algorithm." + std::string{camera_id};
        EXPECT_TRUE(names.contains(prefix + ".state"));
        EXPECT_TRUE(names.contains(prefix + ".queue.depth"));
        EXPECT_TRUE(names.contains(prefix + ".skipped_frames_total"));
        EXPECT_TRUE(names.contains(prefix + ".failures_total"));
        EXPECT_TRUE(names.contains(prefix + ".frame_duration.average_ms"));
        EXPECT_TRUE(names.contains(prefix + ".queue_wait.average_ms"));
        EXPECT_TRUE(names.contains(prefix + ".end_to_end.maximum_ms"));
        EXPECT_TRUE(names.contains(prefix + ".input_fps"));
        EXPECT_TRUE(names.contains(prefix + ".processed_fps"));
        EXPECT_TRUE(names.contains(prefix + ".skipped_ratio"));
        EXPECT_TRUE(names.contains(prefix + ".sampled_skipped_frames_total"));
        EXPECT_TRUE(names.contains(prefix + ".missed_processing_slots_total"));
        EXPECT_TRUE(names.contains(prefix + ".configured_processing_fps"));
        EXPECT_TRUE(names.contains(prefix + ".rearm_pending"));
        EXPECT_TRUE(names.contains(prefix + ".rearm_suppressed_results_total"));
    }
    runtime.value()->request_stop();
    EXPECT_TRUE(runtime.value()->join(std::chrono::steady_clock::now() + 5s));
}
