#include "paperbreak/service/event_runtime.hpp"

#include "paperbreak/storage/event_inspector.hpp"

#include <gtest/gtest.h>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
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

FrameView frame(const std::uint64_t sequence, const std::chrono::milliseconds offset)
{
    auto buffer = std::make_shared<FrameBuffer>(16U);
    for (std::size_t index = 0U; index < 16U; ++index)
        buffer->writable_bytes()[index] = static_cast<std::byte>((sequence + index) & 0xffU);
    if (!buffer->set_size(16U))
        throw std::runtime_error{"frame size"};
    const auto wall = WallClockTime{std::chrono::sys_days{std::chrono::year{2026} / 8 / 4}};
    auto view = make_frame_view({.camera_id = "CAM01",
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
