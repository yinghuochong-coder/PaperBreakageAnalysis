#pragma once

#include "paperbreak/camera/frame.hpp"
#include "paperbreak/common/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace paperbreak::event
{

enum class MemoryRingCapacityMode
{
    frame_count,
    duration,
};

struct MemoryRingPlanRequest final
{
    std::string camera_id;
    MemoryRingCapacityMode capacity_mode{MemoryRingCapacityMode::duration};
    std::size_t configured_frame_count{};
    double configured_duration_seconds{};
    double configured_frame_rate{};
    std::size_t safety_margin_frames{1U};
    std::size_t frame_buffer_capacity_bytes{};
    std::size_t acquisition_queue_capacity{};
    std::size_t algorithm_queue_capacity{};
    std::size_t preview_slot_count{};
    std::size_t nvme_queue_frames{};
    double post_event_seconds{};
    std::size_t maximum_concurrent_events{1U};
    std::size_t configured_frame_pool_capacity{};
    std::size_t memory_budget_bytes{};
};

struct MemoryRingPlan final
{
    std::string camera_id;
    std::size_t ring_capacity_frames{};
    std::size_t post_event_frames{};
    std::size_t pipeline_frames{};
    std::size_t event_lease_budget_frames{};
    std::size_t required_frame_pool_capacity{};
    std::size_t required_memory_bytes{};
    double planned_history_seconds{};
};

/// Computes fixed ring and frame-pool capacities and rejects unsafe or over-budget plans.
[[nodiscard]] Result<MemoryRingPlan> plan_memory_ring(const MemoryRingPlanRequest& request);

enum class MemoryRingShortageReason
{
    history_span,
    incomplete_window,
    lease_capacity,
};

struct MemoryRingShortageNotice final
{
    bool active{};
    MemoryRingShortageReason reason{MemoryRingShortageReason::history_span};
    std::string camera_id;
    double requested_history_seconds{};
    double available_history_seconds{};
    std::size_t requested_frames{};
    std::size_t available_frames{};
};

using MemoryRingShortageCallback = std::function<void(const MemoryRingShortageNotice&)>;

struct MemoryRingOptions final
{
    std::string camera_id;
    std::size_t capacity_frames{};
    double required_history_seconds{};
    std::size_t maximum_active_leases{1U};
    std::size_t maximum_leased_frame_references{};
    MemoryRingShortageCallback shortage_callback;
};

struct MemoryRingSnapshot final
{
    std::string camera_id;
    std::size_t capacity_frames{};
    std::size_t stored_frames{};
    std::size_t resident_bytes{};
    double occupancy_ratio{};
    double actual_history_seconds{};
    std::uint64_t inserted{};
    std::uint64_t overwritten{};
    std::uint64_t rejected{};
    std::uint64_t observed_sequence_gaps{};
    std::size_t maximum_active_leases{};
    std::size_t active_leases{};
    std::size_t maximum_leased_frame_references{};
    std::size_t leased_frame_references{};
    std::uint64_t incomplete_windows{};
    std::uint64_t lease_capacity_rejections{};
    std::uint64_t callback_failures{};
    bool shortage_active{};
    bool closed{};
};

struct MemoryWindowInfo final
{
    camera::MonotonicTime requested_start;
    camera::MonotonicTime requested_end;
    camera::MonotonicTime available_start;
    camera::MonotonicTime available_end;
    std::uint64_t first_sequence_number{};
    std::uint64_t last_sequence_number{};
    std::uint64_t sequence_gaps{};
    bool complete{};
};

class MemoryRingLease final
{
  public:
    ~MemoryRingLease();
    MemoryRingLease(const MemoryRingLease&) = delete;
    MemoryRingLease& operator=(const MemoryRingLease&) = delete;
    MemoryRingLease(MemoryRingLease&& other) noexcept;
    MemoryRingLease& operator=(MemoryRingLease&& other) noexcept;

    [[nodiscard]] std::span<const camera::FrameView> frames() const noexcept;
    [[nodiscard]] const MemoryWindowInfo& info() const noexcept;

  private:
    friend class MemoryRing;
    struct CounterState;

    MemoryRingLease(std::vector<camera::FrameView> frames, MemoryWindowInfo info,
                    std::shared_ptr<CounterState> counters) noexcept;
    void release() noexcept;

    std::vector<camera::FrameView> frames_;
    MemoryWindowInfo info_;
    std::shared_ptr<CounterState> counters_;
};

enum class MemoryRingPushStatus
{
    inserted,
    overwritten,
    closed,
};

/// Fixed-slot per-camera ring. Pixel buffers remain owned by their fixed camera pool.
class MemoryRing final
{
  public:
    explicit MemoryRing(MemoryRingOptions options);
    ~MemoryRing();

    MemoryRing(const MemoryRing&) = delete;
    MemoryRing& operator=(const MemoryRing&) = delete;
    MemoryRing(MemoryRing&&) = delete;
    MemoryRing& operator=(MemoryRing&&) = delete;

    [[nodiscard]] Result<MemoryRingPushStatus> push(camera::FrameView frame);
    [[nodiscard]] Result<MemoryRingLease> lease_window(camera::MonotonicTime start,
                                                       camera::MonotonicTime end);
    void close() noexcept;
    [[nodiscard]] MemoryRingSnapshot snapshot() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace paperbreak::event
