#pragma once

#include "paperbreak/algorithm/trigger.hpp"
#include "paperbreak/camera/frame.hpp"
#include "paperbreak/common/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace paperbreak::event
{

class MemoryRing;

struct EventWindowCameraBinding final
{
    std::string camera_id;
    /// Non-owning; every ring must outlive the manager.
    MemoryRing* memory_ring{};
};

struct EventWindowManagerConfig final
{
    std::vector<EventWindowCameraBinding> cameras;
    std::chrono::milliseconds pre_event_duration{1000};
    std::chrono::milliseconds post_event_duration{1000};
    std::chrono::milliseconds maximum_event_duration{60000};
    std::chrono::milliseconds merge_gap{3000};
    std::size_t maximum_active_events{4U};
};

struct EventWindowTrigger final
{
    std::string source_event_id;
    algorithm::TriggerResult trigger;
};

struct ActiveEventWindowSnapshot final
{
    std::string event_id;
    std::uint64_t version{};
    camera::MonotonicTime requested_start;
    camera::MonotonicTime requested_end;
    camera::MonotonicTime merge_deadline;
    camera::WallClockTime display_wall_clock_time;
    std::vector<EventWindowTrigger> triggers;
    bool truncated_by_maximum_duration{};
    bool buffer_shortage_observed{};
};

struct FrozenCameraWindow final
{
    std::string camera_id;
    camera::MonotonicTime requested_start;
    camera::MonotonicTime requested_end;
    camera::MonotonicTime available_start;
    camera::MonotonicTime available_end;
    std::uint64_t first_sequence_number{};
    std::uint64_t last_sequence_number{};
    std::uint64_t sequence_gaps{};
    std::vector<camera::FrameView> frames;
    bool complete{};
    std::string error_code;
};

struct FrozenEventWindow final
{
    std::string event_id;
    std::uint64_t version{};
    camera::MonotonicTime requested_start;
    camera::MonotonicTime requested_end;
    camera::MonotonicTime closed_monotonic_time;
    camera::WallClockTime display_wall_clock_time;
    std::vector<EventWindowTrigger> triggers;
    std::vector<FrozenCameraWindow> camera_windows;
    bool complete{};
    bool truncated_by_maximum_duration{};
    bool stopped_early{};
};

struct EventWindowStartOutcome final
{
    ActiveEventWindowSnapshot event;
    bool duplicate{};
    bool merged{};
};

struct EventWindowManagerSnapshot final
{
    bool stopped{};
    std::uint64_t accepted_triggers{};
    std::uint64_t duplicate_triggers{};
    std::uint64_t rejected_triggers{};
    std::uint64_t events_created{};
    std::uint64_t events_merged{};
    std::uint64_t events_frozen{};
    std::uint64_t incomplete_events{};
    std::vector<ActiveEventWindowSnapshot> active_events;
};

/// Bounded M5 window freezer. It owns only read-only frame references and performs no I/O.
class EventWindowManager final
{
  public:
    class ConstructionKey final
    {
      public:
        ConstructionKey(const ConstructionKey&) = default;

      private:
        friend class EventWindowManager;
        ConstructionKey() = default;
    };

    [[nodiscard]] static Result<std::unique_ptr<EventWindowManager>> create(
        EventWindowManagerConfig config);

    EventWindowManager(ConstructionKey, EventWindowManagerConfig config);
    ~EventWindowManager();
    EventWindowManager(const EventWindowManager&) = delete;
    EventWindowManager& operator=(const EventWindowManager&) = delete;
    EventWindowManager(EventWindowManager&&) = delete;
    EventWindowManager& operator=(EventWindowManager&&) = delete;

    /// Creates a window or appends the candidate to an overlapping/adjacent active window.
    [[nodiscard]] Result<EventWindowStartOutcome> start_or_merge(
        std::string source_event_id, const algorithm::TriggerResult& trigger);

    /// Freezes windows only after their inclusive merge boundary has passed.
    [[nodiscard]] std::vector<FrozenEventWindow> advance_time(camera::MonotonicTime monotonic_time);

    /// Freezes all active windows with the evidence currently available and rejects new input.
    [[nodiscard]] std::vector<FrozenEventWindow> stop(camera::MonotonicTime monotonic_time);

    [[nodiscard]] Result<ActiveEventWindowSnapshot> active(std::string_view event_id) const;
    [[nodiscard]] EventWindowManagerSnapshot snapshot() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace paperbreak::event
