#include "paperbreak/camera/frame_pool.hpp"
#include "paperbreak/event/event_window.hpp"
#include "paperbreak/event/memory_ring.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using namespace paperbreak;
using namespace paperbreak::algorithm;
using namespace paperbreak::camera;
using namespace paperbreak::event;

FrameView pooled_frame(FrameBufferPool& pool, const std::string& camera_id,
                       const std::uint64_t sequence, const std::chrono::milliseconds time)
{
    auto acquired = pool.acquire({}, 0ms);
    if (acquired.status != FramePoolAcquireStatus::acquired || !acquired.buffer)
        throw std::runtime_error{"test frame pool exhausted"};
    auto bytes = acquired.buffer->writable_bytes();
    for (std::size_t index = 0U; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::byte>((sequence + index) & 0xffU);
    if (!acquired.buffer->set_size(bytes.size()))
        throw std::runtime_error{"test frame size rejected"};
    auto view = make_frame_view({.camera_id = camera_id,
                                 .camera_frame_number = sequence,
                                 .sequence_number = sequence,
                                 .received_monotonic_time = MonotonicTime{time},
                                 .received_wall_clock_time = WallClockTime{time},
                                 .geometry = {.width = 2U, .height = 2U, .stride = 2U},
                                 .pixel_format = PixelFormat::mono8,
                                 .buffer = acquired.buffer});
    if (!view)
        throw std::runtime_error{"test frame invalid"};
    return std::move(view).value();
}

TriggerResult trigger(const std::string& camera_id, const std::uint64_t sequence,
                      const std::chrono::milliseconds monotonic,
                      const std::chrono::hours wall_offset = 0h)
{
    return {.triggered = true,
            .trigger_source = TriggerSource::manual_test,
            .camera_id = camera_id,
            .sequence_number = sequence,
            .camera_frame_number = sequence,
            .monotonic_time = MonotonicTime{monotonic},
            .wall_clock_time = WallClockTime{monotonic + wall_offset},
            .mean_grayscale = 10.0,
            .mean_grayscale_change = 20.0,
            .paper_ratio = 0.5,
            .reason = "test"};
}

class WindowHarness final
{
  public:
    explicit WindowHarness(std::vector<std::string> camera_ids,
                           const std::chrono::milliseconds pre = 100ms,
                           const std::chrono::milliseconds post = 100ms,
                           const std::chrono::milliseconds maximum = 1000ms,
                           const std::chrono::milliseconds gap = 0ms,
                           const std::size_t ring_capacity = 32U,
                           const std::size_t maximum_active_events = paperbreak::camera_slot_count,
                           const std::size_t maximum_active_leases = 32U,
                           const std::size_t maximum_leased_references = 128U)
    {
        bindings_.reserve(camera_ids.size());
        pools_.reserve(camera_ids.size());
        rings_.reserve(camera_ids.size());
        for (auto& camera_id : camera_ids)
        {
            pools_.push_back(std::make_unique<FrameBufferPool>(128U, 4U));
            rings_.push_back(std::make_unique<MemoryRing>(
                MemoryRingOptions{.camera_id = camera_id,
                                  .capacity_frames = ring_capacity,
                                  .required_history_seconds = 0.0,
                                  .maximum_active_leases = maximum_active_leases,
                                  .maximum_leased_frame_references = maximum_leased_references}));
            bindings_.push_back(
                {.camera_id = std::move(camera_id), .memory_ring = rings_.back().get()});
        }
        auto created = EventWindowManager::create({.cameras = bindings_,
                                                   .pre_event_duration = pre,
                                                   .post_event_duration = post,
                                                   .maximum_event_duration = maximum,
                                                   .merge_gap = gap,
                                                   .maximum_active_events = maximum_active_events});
        if (!created)
            throw std::runtime_error{"event window manager creation failed"};
        manager_ = std::move(created).value();
    }

    void push(const std::string& camera_id, const std::uint64_t sequence,
              const std::chrono::milliseconds time)
    {
        const auto index = camera_index(camera_id);
        auto pushed = rings_[index]->push(pooled_frame(*pools_[index], camera_id, sequence, time));
        if (!pushed)
            throw std::runtime_error{"test ring push failed"};
    }

    EventWindowManager& manager()
    {
        return *manager_;
    }

  private:
    std::size_t camera_index(const std::string& camera_id) const
    {
        for (std::size_t index = 0U; index < bindings_.size(); ++index)
        {
            if (bindings_[index].camera_id == camera_id)
                return index;
        }
        throw std::runtime_error{"unknown test camera"};
    }

    std::vector<std::unique_ptr<FrameBufferPool>> pools_;
    std::vector<std::unique_ptr<MemoryRing>> rings_;
    std::vector<EventWindowCameraBinding> bindings_;
    std::unique_ptr<EventWindowManager> manager_;
};

} // namespace

TEST(EventWindowFreeze, IncludesTriggerAndExactFirstLastBoundaries)
{
    WindowHarness harness({"CAM01"}, 200ms, 200ms, 1000ms);
    for (std::uint64_t sequence = 1U; sequence <= 3U; ++sequence)
        harness.push("CAM01", sequence, std::chrono::milliseconds{(sequence - 1U) * 100U});

    const auto wall_jump = 50h;
    auto started =
        harness.manager().start_or_merge("EVT-A", trigger("CAM01", 3U, 200ms, wall_jump));
    ASSERT_TRUE(started);
    EXPECT_EQ(started.value().event.requested_start, MonotonicTime{0ms});
    EXPECT_EQ(started.value().event.requested_end, MonotonicTime{400ms});
    EXPECT_EQ(started.value().event.display_wall_clock_time, WallClockTime{200ms + wall_jump});

    harness.push("CAM01", 4U, 300ms);
    harness.push("CAM01", 5U, 400ms);
    EXPECT_TRUE(harness.manager().advance_time(MonotonicTime{400ms}).empty());
    auto frozen = harness.manager().advance_time(MonotonicTime{401ms});
    ASSERT_EQ(frozen.size(), 1U);
    ASSERT_EQ(frozen.front().camera_windows.size(), 1U);
    const auto& camera = frozen.front().camera_windows.front();
    ASSERT_EQ(camera.frames.size(), 5U);
    EXPECT_EQ(camera.frames.front().sequence_number(), 1U);
    EXPECT_EQ(camera.frames[2].sequence_number(), 3U);
    EXPECT_EQ(camera.frames.back().sequence_number(), 5U);
    EXPECT_TRUE(camera.complete);
    EXPECT_TRUE(frozen.front().complete);
}

TEST(EventWindowMerge, MergesExactGapAndBridgeAcrossOutOfOrderCandidates)
{
    WindowHarness harness({"CAM01"}, 0ms, 0ms, 1000ms, 100ms);
    harness.push("CAM01", 1U, 100ms);
    harness.push("CAM01", 2U, 200ms);
    harness.push("CAM01", 3U, 300ms);

    ASSERT_TRUE(harness.manager().start_or_merge("EVT-A", trigger("CAM01", 1U, 100ms)));
    auto separate = harness.manager().start_or_merge("EVT-B", trigger("CAM01", 3U, 300ms, 10h));
    ASSERT_TRUE(separate);
    EXPECT_FALSE(separate.value().merged);
    EXPECT_EQ(harness.manager().snapshot().active_events.size(), 2U);

    auto bridge = harness.manager().start_or_merge("EVT-C", trigger("CAM01", 2U, 200ms, -10h));
    ASSERT_TRUE(bridge);
    EXPECT_TRUE(bridge.value().merged);
    EXPECT_EQ(bridge.value().event.event_id, "EVT-A");
    EXPECT_EQ(bridge.value().event.triggers.size(), 3U);
    EXPECT_EQ(harness.manager().snapshot().active_events.size(), 1U);
    auto alias = harness.manager().active("EVT-B");
    ASSERT_TRUE(alias);
    EXPECT_EQ(alias.value().event_id, "EVT-A");

    auto frozen = harness.manager().advance_time(MonotonicTime{401ms});
    ASSERT_EQ(frozen.size(), 1U);
    ASSERT_EQ(frozen.front().triggers.size(), 3U);
    EXPECT_EQ(frozen.front().triggers[0].source_event_id, "EVT-A");
    EXPECT_EQ(frozen.front().triggers[1].source_event_id, "EVT-C");
    EXPECT_EQ(frozen.front().triggers[2].source_event_id, "EVT-B");
    EXPECT_EQ(frozen.front().camera_windows.front().frames.size(), 3U);
}

TEST(EventWindowMerge, CorrelatesCrossCameraTriggersAndFreezesEveryCamera)
{
    WindowHarness harness({"CAM01", "CAM02"}, 100ms, 100ms, 1000ms, 100ms);
    for (std::uint64_t sequence = 1U; sequence <= 3U; ++sequence)
    {
        const auto time = std::chrono::milliseconds{sequence * 50U};
        harness.push("CAM01", sequence, time);
        harness.push("CAM02", sequence, time);
    }
    ASSERT_TRUE(harness.manager().start_or_merge("EVT-A", trigger("CAM01", 3U, 150ms)));
    harness.push("CAM01", 4U, 200ms);
    harness.push("CAM02", 4U, 200ms);
    auto merged = harness.manager().start_or_merge("EVT-B", trigger("CAM02", 4U, 200ms));
    ASSERT_TRUE(merged);
    EXPECT_TRUE(merged.value().merged);
    EXPECT_EQ(merged.value().event.event_id, "EVT-A");
    EXPECT_EQ(merged.value().event.requested_start, MonotonicTime{50ms});
    EXPECT_EQ(merged.value().event.requested_end, MonotonicTime{300ms});

    harness.push("CAM01", 5U, 250ms);
    harness.push("CAM02", 5U, 250ms);
    harness.push("CAM01", 6U, 300ms);
    harness.push("CAM02", 6U, 300ms);
    auto frozen = harness.manager().advance_time(MonotonicTime{401ms});
    ASSERT_EQ(frozen.size(), 1U);
    ASSERT_EQ(frozen.front().camera_windows.size(), 2U);
    for (const auto& camera : frozen.front().camera_windows)
    {
        EXPECT_TRUE(camera.complete);
        ASSERT_EQ(camera.frames.size(), 6U);
        EXPECT_EQ(camera.frames.front().sequence_number(), 1U);
        EXPECT_EQ(camera.frames.back().sequence_number(), 6U);
    }
}

TEST(EventWindowLimit, CapsMergedWindowAtMaximumDuration)
{
    WindowHarness harness({"CAM01"}, 100ms, 100ms, 300ms, 100ms);
    for (std::uint64_t sequence = 1U; sequence <= 7U; ++sequence)
        harness.push("CAM01", sequence, std::chrono::milliseconds{(sequence - 1U) * 50U});

    ASSERT_TRUE(harness.manager().start_or_merge("EVT-A", trigger("CAM01", 3U, 100ms)));
    auto merged = harness.manager().start_or_merge("EVT-B", trigger("CAM01", 6U, 250ms));
    ASSERT_TRUE(merged);
    EXPECT_TRUE(merged.value().event.truncated_by_maximum_duration);
    EXPECT_EQ(merged.value().event.requested_start, MonotonicTime{0ms});
    EXPECT_EQ(merged.value().event.requested_end, MonotonicTime{300ms});
    EXPECT_EQ(merged.value().event.merge_deadline, MonotonicTime{300ms});

    auto frozen = harness.manager().advance_time(MonotonicTime{301ms});
    ASSERT_EQ(frozen.size(), 1U);
    EXPECT_TRUE(frozen.front().truncated_by_maximum_duration);
    EXPECT_EQ(frozen.front().requested_end, MonotonicTime{300ms});
    EXPECT_TRUE(frozen.front().complete);
}

TEST(EventWindowShortage, KeepsAvailableFramesAndMarksMissingHistoryAndSequenceGap)
{
    WindowHarness harness({"CAM01"}, 200ms, 200ms, 1000ms, 0ms, 3U);
    harness.push("CAM01", 2U, 100ms);
    harness.push("CAM01", 4U, 200ms);
    auto started = harness.manager().start_or_merge("EVT-A", trigger("CAM01", 4U, 200ms));
    ASSERT_TRUE(started);
    EXPECT_TRUE(started.value().event.buffer_shortage_observed);

    harness.push("CAM01", 5U, 300ms);
    harness.push("CAM01", 6U, 400ms);
    auto frozen = harness.manager().advance_time(MonotonicTime{401ms});
    ASSERT_EQ(frozen.size(), 1U);
    ASSERT_EQ(frozen.front().camera_windows.size(), 1U);
    const auto& camera = frozen.front().camera_windows.front();
    EXPECT_FALSE(camera.complete);
    EXPECT_EQ(camera.error_code, "EVENT_BUFFER_INCOMPLETE");
    EXPECT_EQ(camera.sequence_gaps, 1U);
    ASSERT_EQ(camera.frames.size(), 4U);
    EXPECT_EQ(camera.frames.front().sequence_number(), 2U);
    EXPECT_EQ(camera.frames.back().sequence_number(), 6U);
    EXPECT_FALSE(frozen.front().complete);
    EXPECT_EQ(harness.manager().snapshot().incomplete_events, 1U);
}

TEST(EventWindowShortage, PreservesPreWindowWhenPostLeaseCapacityIsExhausted)
{
    WindowHarness harness({"CAM01"}, 100ms, 100ms, 1000ms, 0ms, 8U, 4U, 1U, 8U);
    harness.push("CAM01", 1U, 0ms);
    harness.push("CAM01", 2U, 100ms);
    ASSERT_TRUE(harness.manager().start_or_merge("EVT-A", trigger("CAM01", 2U, 100ms)));
    harness.push("CAM01", 3U, 200ms);

    auto frozen = harness.manager().advance_time(MonotonicTime{201ms});
    ASSERT_EQ(frozen.size(), 1U);
    const auto& camera = frozen.front().camera_windows.front();
    EXPECT_FALSE(camera.complete);
    EXPECT_EQ(camera.error_code, "EVENT_BUFFER_INCOMPLETE");
    ASSERT_EQ(camera.frames.size(), 2U);
    EXPECT_EQ(camera.frames.front().sequence_number(), 1U);
    EXPECT_EQ(camera.frames.back().sequence_number(), 2U);
}

TEST(EventWindowBounds, DuplicateIsIdempotentCapacityIsFixedAndStopIsDeterministic)
{
    WindowHarness harness({"CAM01"}, 0ms, 0ms, 1000ms, 0ms, 8U, 1U);
    harness.push("CAM01", 1U, 100ms);
    harness.push("CAM01", 2U, 500ms);
    const auto first_trigger = trigger("CAM01", 1U, 100ms);
    auto first = harness.manager().start_or_merge("EVT-A", first_trigger);
    ASSERT_TRUE(first);
    auto duplicate = harness.manager().start_or_merge("EVT-A", first_trigger);
    ASSERT_TRUE(duplicate);
    EXPECT_TRUE(duplicate.value().duplicate);
    EXPECT_EQ(duplicate.value().event.version, 1U);

    auto conflict_trigger = first_trigger;
    conflict_trigger.reason = "different";
    auto conflict = harness.manager().start_or_merge("EVT-A", conflict_trigger);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().business_code, "PIPELINE_FRAME_ORDER_VIOLATION");
    auto full = harness.manager().start_or_merge("EVT-B", trigger("CAM01", 2U, 500ms));
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().business_code, "EVENT_INVALID_TRANSITION");

    auto stopped = harness.manager().stop(MonotonicTime{100ms});
    ASSERT_EQ(stopped.size(), 1U);
    EXPECT_FALSE(stopped.front().stopped_early);
    EXPECT_TRUE(harness.manager().stop(MonotonicTime{200ms}).empty());
    auto after_stop = harness.manager().start_or_merge("EVT-C", trigger("CAM01", 2U, 500ms));
    ASSERT_FALSE(after_stop);
    EXPECT_EQ(after_stop.error().business_code, "EVENT_INVALID_TRANSITION");
}

TEST(EventWindowBounds, TriggerCapacityIsFixedAndSixCameraCallsAreThreadSafe)
{
    WindowHarness harness({"CAM01", "CAM02", "CAM03", "CAM04", "CAM05", "CAM06"}, 0ms, 0ms, 5000ms,
                          100ms);
    const std::array<std::string, paperbreak::camera_slot_count> camera_ids{
        "CAM01", "CAM02", "CAM03", "CAM04", "CAM05", "CAM06"};
    for (const auto& camera_id : camera_ids)
        harness.push(camera_id, 1U, 100ms);

    std::array<std::optional<Result<EventWindowStartOutcome>>, paperbreak::camera_slot_count>
        outcomes;
    std::vector<std::jthread> threads;
    for (std::size_t index = 0U; index < camera_ids.size(); ++index)
    {
        threads.emplace_back([&, index] {
            outcomes[index].emplace(harness.manager().start_or_merge(
                "EVT-" + camera_ids[index], trigger(camera_ids[index], 1U, 100ms)));
        });
    }
    threads.clear();
    for (const auto& outcome : outcomes)
    {
        ASSERT_TRUE(outcome);
        ASSERT_TRUE(*outcome);
    }
    auto snapshot = harness.manager().snapshot();
    ASSERT_EQ(snapshot.active_events.size(), 1U);
    EXPECT_EQ(snapshot.active_events.front().triggers.size(), paperbreak::camera_slot_count);
    EXPECT_EQ(snapshot.events_merged, paperbreak::camera_slot_count - 1U);

    for (std::uint64_t sequence = 2U; sequence <= 11U; ++sequence)
    {
        const auto time = std::chrono::milliseconds{100U + sequence};
        harness.push("CAM01", sequence, time);
        auto merged = harness.manager().start_or_merge("EVT-extra-" + std::to_string(sequence),
                                                       trigger("CAM01", sequence, time));
        ASSERT_TRUE(merged);
    }
    harness.push("CAM01", 12U, 115ms);
    auto full = harness.manager().start_or_merge("EVT-extra-12", trigger("CAM01", 12U, 115ms));
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().business_code, "EVENT_INVALID_TRANSITION");
    EXPECT_EQ(harness.manager().snapshot().active_events.front().triggers.size(), 16U);
}

TEST(EventWindowBounds, HoldsSixIndependentWindowsAndRejectsSeventh)
{
    const std::array<std::string, paperbreak::camera_slot_count> camera_ids{
        "CAM01", "CAM02", "CAM03", "CAM04", "CAM05", "CAM06"};
    WindowHarness harness({camera_ids.begin(), camera_ids.end()}, 0ms, 0ms, 5000ms, 0ms, 32U,
                          paperbreak::camera_slot_count);
    for (std::size_t index = 0U; index < camera_ids.size(); ++index)
    {
        const auto time = std::chrono::milliseconds{100U + index * 100U};
        harness.push(camera_ids[index], 1U, time);
        const auto started = harness.manager().start_or_merge("EVT-" + camera_ids[index],
                                                              trigger(camera_ids[index], 1U, time));
        ASSERT_TRUE(started) << started.error().message;
        EXPECT_FALSE(started.value().merged);
    }
    EXPECT_EQ(harness.manager().snapshot().active_events.size(), paperbreak::camera_slot_count);

    harness.push("CAM01", 2U, 700ms);
    const auto seventh =
        harness.manager().start_or_merge("EVT-SEVENTH", trigger("CAM01", 2U, 700ms));
    ASSERT_FALSE(seventh);
    EXPECT_EQ(seventh.error().business_code, "EVENT_INVALID_TRANSITION");
    ASSERT_FALSE(seventh.error().details.empty());
    EXPECT_EQ(seventh.error().details.back().value, "active-event-capacity");
}

TEST(EventWindowConfiguration, RejectsUnsafeDurationsAndBindings)
{
    EventWindowManagerConfig empty;
    EXPECT_FALSE(EventWindowManager::create(std::move(empty)));

    MemoryRing ring{{.camera_id = "CAM01",
                     .capacity_frames = 4U,
                     .required_history_seconds = 0.0,
                     .maximum_active_leases = 4U,
                     .maximum_leased_frame_references = 8U}};
    EventWindowManagerConfig too_short{
        .cameras = {{.camera_id = "CAM01", .memory_ring = &ring}},
        .pre_event_duration = 200ms,
        .post_event_duration = 200ms,
        .maximum_event_duration = 399ms,
    };
    auto created = EventWindowManager::create(std::move(too_short));
    ASSERT_FALSE(created);
    EXPECT_EQ(created.error().business_code, "SYS_CONFIG_INVALID");

    EventWindowManagerConfig duplicate{
        .cameras = {{.camera_id = "CAM01", .memory_ring = &ring},
                    {.camera_id = "CAM01", .memory_ring = &ring}},
    };
    EXPECT_FALSE(EventWindowManager::create(std::move(duplicate)));
}
