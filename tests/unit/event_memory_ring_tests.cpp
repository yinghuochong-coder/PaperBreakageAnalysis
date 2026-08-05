#include "paperbreak/camera/frame_pool.hpp"
#include "paperbreak/event/memory_ring.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
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

FrameView pooled_frame(FrameBufferPool& pool, const std::uint64_t sequence,
                       const std::chrono::milliseconds time, const std::string& camera_id = "CAM01")
{
    auto acquired = pool.acquire({}, 0ms);
    if (acquired.status != FramePoolAcquireStatus::acquired || !acquired.buffer)
        throw std::runtime_error{"test frame pool exhausted"};
    auto bytes = acquired.buffer->writable_bytes();
    for (std::size_t index = 0U; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::byte>((sequence + index) & 0xffU);
    if (!acquired.buffer->set_size(bytes.size()))
        throw std::runtime_error{"test frame size rejected"};
    FramePacket packet{.camera_id = camera_id,
                       .camera_frame_number = sequence,
                       .sequence_number = sequence,
                       .received_monotonic_time = MonotonicTime{time},
                       .received_wall_clock_time = WallClockTime{time},
                       .geometry = {.width = 2U, .height = 2U, .stride = 2U},
                       .pixel_format = PixelFormat::mono8,
                       .buffer = acquired.buffer};
    auto view = make_frame_view(packet);
    if (!view)
        throw std::runtime_error{"test frame invalid"};
    return std::move(view).value();
}

MemoryRingPlanRequest valid_plan_request()
{
    return {.camera_id = "CAM01",
            .capacity_mode = MemoryRingCapacityMode::duration,
            .configured_duration_seconds = 2.0,
            .configured_frame_rate = 2.5,
            .safety_margin_frames = 1U,
            .frame_buffer_capacity_bytes = 4U,
            .acquisition_queue_capacity = 2U,
            .algorithm_queue_capacity = 3U,
            .preview_slot_count = 1U,
            .post_event_seconds = 1.0,
            .maximum_concurrent_events = 2U,
            .configured_frame_pool_capacity = 30U,
            .memory_budget_bytes = 120U};
}

} // namespace

TEST(EventMemoryRingPlan, ConvertsDurationAndFrameCountAndValidatesExactBudget)
{
    auto duration = plan_memory_ring(valid_plan_request());
    ASSERT_TRUE(duration);
    EXPECT_EQ(duration.value().ring_capacity_frames, 6U);
    EXPECT_EQ(duration.value().post_event_frames, 3U);
    EXPECT_EQ(duration.value().pipeline_frames, 6U);
    EXPECT_EQ(duration.value().event_lease_budget_frames, 18U);
    EXPECT_EQ(duration.value().required_frame_pool_capacity, 30U);
    EXPECT_EQ(duration.value().required_memory_bytes, 120U);
    EXPECT_DOUBLE_EQ(duration.value().planned_history_seconds, 2.0);

    auto request = valid_plan_request();
    request.capacity_mode = MemoryRingCapacityMode::frame_count;
    request.configured_frame_count = 5U;
    request.safety_margin_frames = 0U;
    request.configured_frame_pool_capacity = 27U;
    request.memory_budget_bytes = 108U;
    auto frames = plan_memory_ring(request);
    ASSERT_TRUE(frames);
    EXPECT_EQ(frames.value().ring_capacity_frames, 5U);
    EXPECT_DOUBLE_EQ(frames.value().planned_history_seconds, 2.0);
}

TEST(EventMemoryRingPlan, HandlesDifferentFrameRatesAndRejectsUnsafePlans)
{
    auto request = valid_plan_request();
    request.configured_duration_seconds = 10.0;
    request.configured_frame_rate = 29.97;
    request.post_event_seconds = 0.0;
    request.maximum_concurrent_events = 1U;
    request.configured_frame_pool_capacity = 608U;
    request.memory_budget_bytes = 2432U;
    auto plan = plan_memory_ring(request);
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan.value().ring_capacity_frames, 301U);
    EXPECT_EQ(plan.value().required_frame_pool_capacity, 608U);

    request.configured_frame_pool_capacity = 607U;
    plan = plan_memory_ring(request);
    ASSERT_FALSE(plan);
    EXPECT_EQ(plan.error().business_code, "SYS_CONFIG_INVALID");
    EXPECT_EQ(plan.error().details.front().value, "frame-pool-insufficient");

    request.configured_frame_pool_capacity = 608U;
    request.memory_budget_bytes = 2431U;
    plan = plan_memory_ring(request);
    ASSERT_FALSE(plan);
    EXPECT_EQ(plan.error().details.front().value, "memory-budget-exceeded");

    request = valid_plan_request();
    request.configured_frame_rate = 0.0;
    EXPECT_FALSE(plan_memory_ring(request));
    request = valid_plan_request();
    request.safety_margin_frames = (std::numeric_limits<std::size_t>::max)();
    plan = plan_memory_ring(request);
    ASSERT_FALSE(plan);
    EXPECT_EQ(plan.error().details.front().value, "ring-frame-overflow");
}

TEST(EventMemoryRingPlan, IncludesCurrentAndQueuedNvmeFrameReferences)
{
    auto request = valid_plan_request();
    request.nvme_queue_frames = 12U;
    request.configured_frame_pool_capacity = 42U;
    request.memory_budget_bytes = 168U;
    auto plan = plan_memory_ring(request);
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan.value().pipeline_frames, 18U);
    EXPECT_EQ(plan.value().required_frame_pool_capacity, 42U);

    request.configured_frame_pool_capacity = 41U;
    plan = plan_memory_ring(request);
    ASSERT_FALSE(plan);
    EXPECT_EQ(plan.error().details.front().value, "frame-pool-insufficient");
}

TEST(EventMemoryRingBuffer, WrapsAtFixedCapacityAndIncludesExactWindowBoundaries)
{
    FrameBufferPool pool{4U, 4U};
    MemoryRing ring{{.camera_id = "CAM01",
                     .capacity_frames = 3U,
                     .required_history_seconds = 0.2,
                     .maximum_active_leases = 2U,
                     .maximum_leased_frame_references = 6U}};
    EXPECT_EQ(ring.push(pooled_frame(pool, 1U, 0ms)).value(), MemoryRingPushStatus::inserted);
    EXPECT_EQ(ring.push(pooled_frame(pool, 2U, 100ms)).value(), MemoryRingPushStatus::inserted);
    EXPECT_EQ(ring.push(pooled_frame(pool, 3U, 200ms)).value(), MemoryRingPushStatus::inserted);
    EXPECT_EQ(ring.push(pooled_frame(pool, 4U, 300ms)).value(), MemoryRingPushStatus::overwritten);

    const auto snapshot = ring.snapshot();
    EXPECT_EQ(snapshot.capacity_frames, 3U);
    EXPECT_EQ(snapshot.stored_frames, 3U);
    EXPECT_EQ(snapshot.resident_bytes, 12U);
    EXPECT_DOUBLE_EQ(snapshot.occupancy_ratio, 1.0);
    EXPECT_DOUBLE_EQ(snapshot.actual_history_seconds, 0.2);
    EXPECT_EQ(snapshot.inserted, 4U);
    EXPECT_EQ(snapshot.overwritten, 1U);

    auto lease = ring.lease_window(MonotonicTime{100ms}, MonotonicTime{300ms});
    ASSERT_TRUE(lease);
    ASSERT_EQ(lease.value().frames().size(), 3U);
    EXPECT_EQ(lease.value().frames().front().sequence_number(), 2U);
    EXPECT_EQ(lease.value().frames().back().sequence_number(), 4U);
    EXPECT_TRUE(lease.value().info().complete);
}

TEST(EventMemoryRingBuffer, FixedCameraPoolExhaustsWhileRingAndLeaseProtectFrames)
{
    FrameBufferPool pool{3U, 4U};
    MemoryRing ring{{.camera_id = "CAM01",
                     .capacity_frames = 2U,
                     .required_history_seconds = 0.0,
                     .maximum_active_leases = 1U,
                     .maximum_leased_frame_references = 2U}};
    ASSERT_TRUE(ring.push(pooled_frame(pool, 1U, 0ms)));
    ASSERT_TRUE(ring.push(pooled_frame(pool, 2U, 1ms)));
    auto lease = ring.lease_window(MonotonicTime{0ms}, MonotonicTime{1ms});
    ASSERT_TRUE(lease);
    ASSERT_TRUE(ring.push(pooled_frame(pool, 3U, 2ms)));

    const auto exhausted = pool.acquire({}, 0ms);
    EXPECT_EQ(exhausted.status, FramePoolAcquireStatus::exhausted);
    EXPECT_EQ(pool.snapshot().in_use, 3U);

    ring.close();
    EXPECT_EQ(pool.snapshot().in_use, 2U);
    lease = Result<MemoryRingLease>::failure(
        make_error("EVENT_BUFFER_INCOMPLETE", Severity::critical, "release", "event", "test"));
    EXPECT_EQ(pool.snapshot().in_use, 0U);
}

TEST(EventMemoryRingBuffer, OverlappingLeasesSharePixelOwnersAndRemainBounded)
{
    FrameBufferPool pool{6U, 4U};
    MemoryRing ring{{.camera_id = "CAM01",
                     .capacity_frames = 4U,
                     .required_history_seconds = 0.0,
                     .maximum_active_leases = 2U,
                     .maximum_leased_frame_references = 6U}};
    for (std::uint64_t sequence = 1U; sequence <= 4U; ++sequence)
        ASSERT_TRUE(ring.push(pooled_frame(pool, sequence, std::chrono::milliseconds{sequence})));

    auto first = ring.lease_window(MonotonicTime{2ms}, MonotonicTime{4ms});
    auto second = ring.lease_window(MonotonicTime{3ms}, MonotonicTime{4ms});
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_EQ(first.value().frames().size(), 3U);
    ASSERT_EQ(second.value().frames().size(), 2U);
    EXPECT_EQ(first.value().frames()[1].buffer_owner().get(),
              second.value().frames()[0].buffer_owner().get());
    EXPECT_EQ(ring.snapshot().active_leases, 2U);
    EXPECT_EQ(ring.snapshot().leased_frame_references, 5U);

    auto rejected = ring.lease_window(MonotonicTime{4ms}, MonotonicTime{4ms});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().business_code, "EVENT_BUFFER_INCOMPLETE");
    EXPECT_EQ(ring.snapshot().lease_capacity_rejections, 1U);
}

TEST(EventMemoryRingBuffer, LeaseSurvivesRingCloseAndDestruction)
{
    FrameBufferPool pool{3U, 4U};
    std::optional<MemoryRingLease> retained;
    {
        MemoryRing ring{{.camera_id = "CAM01",
                         .capacity_frames = 2U,
                         .required_history_seconds = 0.0,
                         .maximum_active_leases = 1U,
                         .maximum_leased_frame_references = 2U}};
        ASSERT_TRUE(ring.push(pooled_frame(pool, 1U, 0ms)));
        ASSERT_TRUE(ring.push(pooled_frame(pool, 2U, 1ms)));
        auto lease = ring.lease_window(MonotonicTime{0ms}, MonotonicTime{1ms});
        ASSERT_TRUE(lease);
        retained.emplace(std::move(lease).value());
        ring.close();
        EXPECT_EQ(ring.push(pooled_frame(pool, 3U, 2ms)).value(), MemoryRingPushStatus::closed);
    }
    ASSERT_EQ(retained->frames().size(), 2U);
    EXPECT_EQ(retained->frames().front().sequence_number(), 1U);
    EXPECT_EQ(pool.snapshot().in_use, 2U);
    retained.reset();
    EXPECT_EQ(pool.snapshot().in_use, 0U);
}

TEST(EventMemoryRingBuffer, ReportsSequenceGapsAndShortageTransitions)
{
    std::vector<MemoryRingShortageNotice> notices;
    FrameBufferPool pool{5U, 4U};
    MemoryRing ring{{.camera_id = "CAM01",
                     .capacity_frames = 3U,
                     .required_history_seconds = 2.0,
                     .maximum_active_leases = 1U,
                     .maximum_leased_frame_references = 3U,
                     .shortage_callback = [&](const MemoryRingShortageNotice& notice) {
                         notices.push_back(notice);
                     }}};
    ASSERT_TRUE(ring.push(pooled_frame(pool, 1U, 0ms)));
    ASSERT_TRUE(ring.push(pooled_frame(pool, 3U, 500ms)));
    ASSERT_TRUE(ring.push(pooled_frame(pool, 4U, 1s)));
    ASSERT_EQ(notices.size(), 1U);
    EXPECT_TRUE(notices.back().active);
    EXPECT_EQ(notices.back().reason, MemoryRingShortageReason::history_span);
    EXPECT_EQ(ring.snapshot().observed_sequence_gaps, 1U);

    ASSERT_TRUE(ring.push(pooled_frame(pool, 5U, 3s)));
    ASSERT_EQ(notices.size(), 2U);
    EXPECT_FALSE(notices.back().active);

    auto lease = ring.lease_window(MonotonicTime{0ms}, MonotonicTime{3s});
    ASSERT_TRUE(lease);
    EXPECT_FALSE(lease.value().info().complete);
    EXPECT_EQ(lease.value().info().sequence_gaps, 0U);
    ASSERT_EQ(notices.size(), 3U);
    EXPECT_TRUE(notices.back().active);
    EXPECT_EQ(notices.back().reason, MemoryRingShortageReason::incomplete_window);
}

TEST(EventMemoryRingBuffer, RejectsWrongCameraAndContainsShortageCallbackExceptions)
{
    FrameBufferPool pool{4U, 4U};
    MemoryRing ring{{.camera_id = "CAM01",
                     .capacity_frames = 2U,
                     .required_history_seconds = 2.0,
                     .maximum_active_leases = 1U,
                     .maximum_leased_frame_references = 2U,
                     .shortage_callback = [](const MemoryRingShortageNotice&) {
                         throw std::runtime_error{"injected callback failure"};
                     }}};
    auto wrong = ring.push(pooled_frame(pool, 1U, 0ms, "CAM02"));
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().business_code, "PIPELINE_FRAME_ORDER_VIOLATION");
    ASSERT_TRUE(ring.push(pooled_frame(pool, 1U, 0ms)));
    ASSERT_TRUE(ring.push(pooled_frame(pool, 2U, 1s)));
    EXPECT_EQ(ring.snapshot().callback_failures, 1U);
    EXPECT_TRUE(ring.snapshot().shortage_active);
}
