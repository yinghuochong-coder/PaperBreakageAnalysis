#include "paperbreak/event/event_window.hpp"

#include "paperbreak/event/memory_ring.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace paperbreak::event
{
namespace
{

constexpr std::size_t maximum_camera_count = 4U;
constexpr std::size_t maximum_active_event_count = 4U;
constexpr std::size_t maximum_triggers_per_event = 16U;

Error window_error(std::string business_code, const Severity severity, std::string message,
                   std::string operation, std::string source_id, std::string reason)
{
    auto error = make_error(std::move(business_code), severity, std::move(message), "event",
                            std::move(operation));
    error.source_id = std::move(source_id);
    error.details.push_back({"reason", std::move(reason)});
    return error;
}

Error config_error(std::string source_id, std::string reason)
{
    return window_error("SYS_CONFIG_INVALID", Severity::error, "事件窗口聚合器配置无效",
                        "event.window.create", std::move(source_id), std::move(reason));
}

bool valid_camera_id(const std::string_view camera_id) noexcept
{
    return camera_id.size() == 5U && camera_id.starts_with("CAM0") && camera_id[4] >= '1' &&
           camera_id[4] <= '4';
}

bool valid_event_id(const std::string_view event_id) noexcept
{
    return !event_id.empty() && event_id.size() <= 128U;
}

bool same_trigger(const algorithm::TriggerResult& left,
                  const algorithm::TriggerResult& right) noexcept
{
    return left.triggered == right.triggered && left.trigger_source == right.trigger_source &&
           left.camera_id == right.camera_id && left.sequence_number == right.sequence_number &&
           left.camera_frame_number == right.camera_frame_number &&
           left.monotonic_time == right.monotonic_time &&
           left.wall_clock_time == right.wall_clock_time &&
           left.evaluated_region == right.evaluated_region &&
           left.mean_grayscale == right.mean_grayscale &&
           left.mean_grayscale_change == right.mean_grayscale_change &&
           left.paper_ratio == right.paper_ratio && left.reason == right.reason;
}

camera::MonotonicTime subtract_saturating(const camera::MonotonicTime time,
                                          const std::chrono::milliseconds duration) noexcept
{
    const auto converted = std::chrono::duration_cast<camera::MonotonicTime::duration>(duration);
    return time < camera::MonotonicTime::min() + converted ? camera::MonotonicTime::min()
                                                           : time - converted;
}

camera::MonotonicTime add_saturating(const camera::MonotonicTime time,
                                     const std::chrono::milliseconds duration) noexcept
{
    const auto converted = std::chrono::duration_cast<camera::MonotonicTime::duration>(duration);
    return time > camera::MonotonicTime::max() - converted ? camera::MonotonicTime::max()
                                                           : time + converted;
}

std::uint64_t add_gap_saturating(const std::uint64_t current, const std::uint64_t gap) noexcept
{
    return gap > (std::numeric_limits<std::uint64_t>::max)() - current
               ? (std::numeric_limits<std::uint64_t>::max)()
               : current + gap;
}

} // namespace

struct EventWindowManager::Impl final
{
    struct Coverage final
    {
        camera::MonotonicTime start;
        camera::MonotonicTime end;
    };

    struct CameraProtection final
    {
        std::string camera_id;
        MemoryRing* memory_ring{};
        std::vector<MemoryRingLease> leases;
        bool shortage_observed{};
    };

    struct ActiveEvent final
    {
        std::string event_id;
        std::uint64_t version{1U};
        std::uint64_t creation_order{};
        camera::MonotonicTime requested_start;
        camera::MonotonicTime requested_end;
        camera::MonotonicTime earliest_trigger_time;
        camera::MonotonicTime latest_trigger_time;
        camera::MonotonicTime hard_end;
        camera::MonotonicTime merge_deadline;
        camera::WallClockTime display_wall_clock_time;
        std::vector<EventWindowTrigger> triggers;
        std::vector<CameraProtection> cameras;
        bool truncated_by_maximum_duration{};
    };

    explicit Impl(EventWindowManagerConfig value) : config(std::move(value))
    {
        active_events.reserve(config.maximum_active_events);
    }

    [[nodiscard]] Result<void> validate_trigger(const std::string_view source_event_id,
                                                const algorithm::TriggerResult& trigger) const
    {
        if (!valid_event_id(source_event_id))
        {
            return Result<void>::failure(window_error(
                "PIPELINE_FRAME_ORDER_VIOLATION", Severity::warning, "候选事件 ID 无效",
                "event.window.startOrMerge", std::string{source_event_id}, "invalid-event-id"));
        }
        const auto camera =
            std::find_if(config.cameras.begin(), config.cameras.end(), [&](const auto& binding) {
                return binding.camera_id == trigger.camera_id;
            });
        if (camera == config.cameras.end())
        {
            return Result<void>::failure(window_error(
                "PIPELINE_FRAME_ORDER_VIOLATION", Severity::warning, "候选触发来自未绑定相机",
                "event.window.startOrMerge", trigger.camera_id, "unknown-camera"));
        }
        if (!trigger.triggered || trigger.trigger_source == algorithm::TriggerSource::none ||
            !std::isfinite(trigger.mean_grayscale) ||
            !std::isfinite(trigger.mean_grayscale_change) || !std::isfinite(trigger.paper_ratio))
        {
            return Result<void>::failure(window_error(
                "PIPELINE_FRAME_ORDER_VIOLATION", Severity::warning, "候选触发字段无效",
                "event.window.startOrMerge", trigger.camera_id, "invalid-trigger"));
        }
        return Result<void>::success();
    }

    [[nodiscard]] static ActiveEventWindowSnapshot active_snapshot(const ActiveEvent& event)
    {
        const auto shortage =
            std::any_of(event.cameras.begin(), event.cameras.end(),
                        [](const auto& camera) { return camera.shortage_observed; });
        return {.event_id = event.event_id,
                .version = event.version,
                .requested_start = event.requested_start,
                .requested_end = event.requested_end,
                .merge_deadline = event.merge_deadline,
                .display_wall_clock_time = event.display_wall_clock_time,
                .triggers = event.triggers,
                .truncated_by_maximum_duration = event.truncated_by_maximum_duration,
                .buffer_shortage_observed = shortage};
    }

    void update_deadline(ActiveEvent& event) const noexcept
    {
        event.hard_end = add_saturating(event.requested_start, config.maximum_event_duration);
        if (event.requested_end > event.hard_end)
        {
            event.requested_end = event.hard_end;
            event.truncated_by_maximum_duration = true;
        }
        event.merge_deadline =
            (std::min)(add_saturating(event.requested_end, config.merge_gap), event.hard_end);
    }

    void protect_range(ActiveEvent& event, const camera::MonotonicTime start,
                       const camera::MonotonicTime end)
    {
        for (auto& camera : event.cameras)
        {
            auto lease = camera.memory_ring->lease_window(start, end);
            if (!lease)
            {
                camera.shortage_observed = true;
                continue;
            }
            if (!lease.value().info().complete || lease.value().info().sequence_gaps > 0U)
                camera.shortage_observed = true;
            camera.leases.push_back(std::move(lease).value());
        }
    }

    [[nodiscard]] bool overlaps_with_gap(const ActiveEvent& event,
                                         const camera::MonotonicTime start,
                                         const camera::MonotonicTime end) const noexcept
    {
        return start <= add_saturating(event.requested_end, config.merge_gap) &&
               event.requested_start <= add_saturating(end, config.merge_gap);
    }

    [[nodiscard]] bool respects_trigger_span(
        const ActiveEvent& event, const camera::MonotonicTime new_start,
        const camera::MonotonicTime trigger_time) const noexcept
    {
        const auto union_start = (std::min)(event.requested_start, new_start);
        const auto hard_end = add_saturating(union_start, config.maximum_event_duration);
        return (std::max)(event.latest_trigger_time, trigger_time) <= hard_end;
    }

    void merge_event_into(ActiveEvent& target, ActiveEvent&& source)
    {
        const auto old_start = target.requested_start;
        if (source.earliest_trigger_time < target.earliest_trigger_time)
            target.display_wall_clock_time = source.display_wall_clock_time;
        target.requested_start = (std::min)(target.requested_start, source.requested_start);
        target.requested_end = (std::max)(target.requested_end, source.requested_end);
        target.earliest_trigger_time =
            (std::min)(target.earliest_trigger_time, source.earliest_trigger_time);
        target.latest_trigger_time =
            (std::max)(target.latest_trigger_time, source.latest_trigger_time);
        target.truncated_by_maximum_duration =
            target.truncated_by_maximum_duration || source.truncated_by_maximum_duration;
        for (auto& trigger : source.triggers)
            target.triggers.push_back(std::move(trigger));
        for (std::size_t index = 0U; index < target.cameras.size(); ++index)
        {
            auto& destination = target.cameras[index];
            auto& origin = source.cameras[index];
            destination.shortage_observed =
                destination.shortage_observed || origin.shortage_observed;
            for (auto& lease : origin.leases)
                destination.leases.push_back(std::move(lease));
        }
        ++target.version;
        update_deadline(target);
        if (target.requested_start < old_start)
            protect_range(target, target.requested_start, old_start);
    }

    [[nodiscard]] FrozenCameraWindow freeze_camera(CameraProtection& protection,
                                                   const ActiveEvent& event)
    {
        std::vector<camera::FrameView> frames;
        std::vector<Coverage> coverage;
        bool shortage = protection.shortage_observed;
        for (const auto& lease : protection.leases)
        {
            frames.insert(frames.end(), lease.frames().begin(), lease.frames().end());
            const auto& info = lease.info();
            coverage.push_back({.start = (std::max)(info.available_start, info.requested_start),
                                .end = (std::min)(info.available_end, info.requested_end)});
            if (info.sequence_gaps > 0U)
                shortage = true;
        }

        auto final_lease =
            protection.memory_ring->lease_window(event.earliest_trigger_time, event.requested_end);
        if (final_lease)
        {
            const auto& info = final_lease.value().info();
            frames.insert(frames.end(), final_lease.value().frames().begin(),
                          final_lease.value().frames().end());
            coverage.push_back({.start = (std::max)(info.available_start, info.requested_start),
                                .end = (std::min)(info.available_end, info.requested_end)});
            if (info.sequence_gaps > 0U)
                shortage = true;
        }

        std::sort(frames.begin(), frames.end(), [](const auto& left, const auto& right) {
            if (left.received_monotonic_time() != right.received_monotonic_time())
                return left.received_monotonic_time() < right.received_monotonic_time();
            return left.sequence_number() < right.sequence_number();
        });
        frames.erase(std::unique(frames.begin(), frames.end(),
                                 [](const auto& left, const auto& right) {
                                     return left.sequence_number() == right.sequence_number();
                                 }),
                     frames.end());

        std::sort(coverage.begin(), coverage.end(),
                  [](const auto& left, const auto& right) { return left.start < right.start; });
        auto covered_until = event.requested_start;
        bool continuously_covered = !coverage.empty() && coverage.front().start <= covered_until;
        for (const auto& segment : coverage)
        {
            if (!continuously_covered)
                break;
            if (segment.start > covered_until)
            {
                continuously_covered = false;
                break;
            }
            covered_until = (std::max)(covered_until, segment.end);
        }

        std::uint64_t sequence_gaps{};
        for (std::size_t index = 1U; index < frames.size(); ++index)
        {
            const auto previous = frames[index - 1U].sequence_number();
            const auto current = frames[index].sequence_number();
            if (current > previous && current - previous > 1U)
                sequence_gaps = add_gap_saturating(sequence_gaps, current - previous - 1U);
        }
        if (sequence_gaps > 0U)
            shortage = true;

        FrozenCameraWindow result{.camera_id = protection.camera_id,
                                  .requested_start = event.requested_start,
                                  .requested_end = event.requested_end,
                                  .sequence_gaps = sequence_gaps,
                                  .frames = std::move(frames)};
        if (!result.frames.empty())
        {
            result.available_start = result.frames.front().received_monotonic_time();
            result.available_end = result.frames.back().received_monotonic_time();
            result.first_sequence_number = result.frames.front().sequence_number();
            result.last_sequence_number = result.frames.back().sequence_number();
        }
        result.complete = continuously_covered && covered_until >= event.requested_end &&
                          !shortage && !result.frames.empty();
        if (!result.complete)
            result.error_code = "EVENT_BUFFER_INCOMPLETE";
        return result;
    }

    [[nodiscard]] FrozenEventWindow freeze(ActiveEvent&& event,
                                           const camera::MonotonicTime close_time)
    {
        std::sort(event.triggers.begin(), event.triggers.end(),
                  [](const auto& left, const auto& right) {
                      if (left.trigger.monotonic_time != right.trigger.monotonic_time)
                          return left.trigger.monotonic_time < right.trigger.monotonic_time;
                      if (left.trigger.camera_id != right.trigger.camera_id)
                          return left.trigger.camera_id < right.trigger.camera_id;
                      return left.trigger.sequence_number < right.trigger.sequence_number;
                  });
        FrozenEventWindow result{.event_id = std::move(event.event_id),
                                 .version = event.version,
                                 .requested_start = event.requested_start,
                                 .requested_end = event.requested_end,
                                 .closed_monotonic_time = close_time,
                                 .display_wall_clock_time = event.display_wall_clock_time,
                                 .triggers = std::move(event.triggers),
                                 .truncated_by_maximum_duration =
                                     event.truncated_by_maximum_duration,
                                 .stopped_early = close_time < event.merge_deadline};
        result.camera_windows.reserve(event.cameras.size());
        result.complete = true;
        for (auto& camera : event.cameras)
        {
            result.camera_windows.push_back(freeze_camera(camera, event));
            result.complete = result.complete && result.camera_windows.back().complete;
        }
        ++events_frozen;
        if (!result.complete)
            ++incomplete_events;
        return result;
    }

    EventWindowManagerConfig config;
    mutable std::mutex mutex;
    std::vector<ActiveEvent> active_events;
    std::uint64_t next_creation_order{};
    std::uint64_t accepted_triggers{};
    std::uint64_t duplicate_triggers{};
    std::uint64_t rejected_triggers{};
    std::uint64_t events_created{};
    std::uint64_t events_merged{};
    std::uint64_t events_frozen{};
    std::uint64_t incomplete_events{};
    bool stopped{};
};

Result<std::unique_ptr<EventWindowManager>> EventWindowManager::create(
    EventWindowManagerConfig config)
{
    if (config.cameras.empty() || config.cameras.size() > maximum_camera_count ||
        config.maximum_active_events == 0U ||
        config.maximum_active_events > maximum_active_event_count ||
        config.pre_event_duration < std::chrono::milliseconds::zero() ||
        config.post_event_duration < std::chrono::milliseconds::zero() ||
        config.merge_gap < std::chrono::milliseconds::zero() ||
        config.maximum_event_duration <= std::chrono::milliseconds::zero() ||
        config.pre_event_duration > config.maximum_event_duration ||
        config.post_event_duration > config.maximum_event_duration - config.pre_event_duration)
    {
        return Result<std::unique_ptr<EventWindowManager>>::failure(
            config_error({}, "invalid-window-configuration"));
    }
    const auto maximum_monotonic_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        camera::MonotonicTime::duration::max());
    if (config.pre_event_duration > maximum_monotonic_duration ||
        config.post_event_duration > maximum_monotonic_duration ||
        config.maximum_event_duration > maximum_monotonic_duration ||
        config.merge_gap > maximum_monotonic_duration)
    {
        return Result<std::unique_ptr<EventWindowManager>>::failure(
            config_error({}, "window-duration-out-of-range"));
    }
    for (std::size_t index = 0U; index < config.cameras.size(); ++index)
    {
        const auto& binding = config.cameras[index];
        if (!valid_camera_id(binding.camera_id) || binding.memory_ring == nullptr)
        {
            return Result<std::unique_ptr<EventWindowManager>>::failure(
                config_error(binding.camera_id, "invalid-camera-binding"));
        }
        const auto ring = binding.memory_ring->snapshot();
        if (ring.closed || ring.camera_id != binding.camera_id)
        {
            return Result<std::unique_ptr<EventWindowManager>>::failure(
                config_error(binding.camera_id, "memory-ring-binding-mismatch"));
        }
        for (std::size_t previous = 0U; previous < index; ++previous)
        {
            if (config.cameras[previous].camera_id == binding.camera_id)
            {
                return Result<std::unique_ptr<EventWindowManager>>::failure(
                    config_error(binding.camera_id, "duplicate-camera-binding"));
            }
        }
    }
    return Result<std::unique_ptr<EventWindowManager>>::success(
        std::make_unique<EventWindowManager>(ConstructionKey{}, std::move(config)));
}

EventWindowManager::EventWindowManager(ConstructionKey, EventWindowManagerConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

EventWindowManager::~EventWindowManager() = default;

Result<EventWindowStartOutcome> EventWindowManager::start_or_merge(
    std::string source_event_id, const algorithm::TriggerResult& trigger)
{
    std::scoped_lock lock{impl_->mutex};
    if (impl_->stopped)
    {
        ++impl_->rejected_triggers;
        return Result<EventWindowStartOutcome>::failure(window_error(
            "EVENT_INVALID_TRANSITION", Severity::error, "事件窗口聚合器已停止",
            "event.window.startOrMerge", std::move(source_event_id), "manager-stopped"));
    }
    if (auto valid = impl_->validate_trigger(source_event_id, trigger); !valid)
    {
        ++impl_->rejected_triggers;
        return Result<EventWindowStartOutcome>::failure(std::move(valid.error()));
    }

    for (const auto& event : impl_->active_events)
    {
        const auto found =
            std::find_if(event.triggers.begin(), event.triggers.end(), [&](const auto& existing) {
                return existing.source_event_id == source_event_id;
            });
        if (found == event.triggers.end())
            continue;
        if (!same_trigger(found->trigger, trigger))
        {
            ++impl_->rejected_triggers;
            return Result<EventWindowStartOutcome>::failure(
                window_error("PIPELINE_FRAME_ORDER_VIOLATION", Severity::warning,
                             "同一来源候选 ID 的触发内容发生冲突", "event.window.startOrMerge",
                             std::move(source_event_id), "duplicate-event-conflict"));
        }
        ++impl_->duplicate_triggers;
        return Result<EventWindowStartOutcome>::success(
            {.event = Impl::active_snapshot(event), .duplicate = true, .merged = false});
    }

    const auto new_start =
        subtract_saturating(trigger.monotonic_time, impl_->config.pre_event_duration);
    const auto natural_end =
        add_saturating(trigger.monotonic_time, impl_->config.post_event_duration);
    std::vector<std::size_t> matches;
    matches.reserve(impl_->active_events.size());
    for (std::size_t index = 0U; index < impl_->active_events.size(); ++index)
    {
        const auto& event = impl_->active_events[index];
        if (impl_->overlaps_with_gap(event, new_start, natural_end) &&
            impl_->respects_trigger_span(event, new_start, trigger.monotonic_time))
        {
            matches.push_back(index);
        }
    }

    if (matches.empty())
    {
        if (impl_->active_events.size() >= impl_->config.maximum_active_events)
        {
            ++impl_->rejected_triggers;
            return Result<EventWindowStartOutcome>::failure(window_error(
                "EVENT_INVALID_TRANSITION", Severity::warning, "活动事件窗口已达到固定上限",
                "event.window.startOrMerge", std::move(source_event_id), "active-event-capacity"));
        }
        Impl::ActiveEvent event{.event_id = source_event_id,
                                .creation_order = ++impl_->next_creation_order,
                                .requested_start = new_start,
                                .requested_end = natural_end,
                                .earliest_trigger_time = trigger.monotonic_time,
                                .latest_trigger_time = trigger.monotonic_time,
                                .display_wall_clock_time = trigger.wall_clock_time};
        event.triggers.reserve(maximum_triggers_per_event);
        event.triggers.push_back(
            {.source_event_id = std::move(source_event_id), .trigger = trigger});
        event.cameras.reserve(impl_->config.cameras.size());
        for (const auto& binding : impl_->config.cameras)
        {
            event.cameras.push_back(
                {.camera_id = binding.camera_id, .memory_ring = binding.memory_ring});
        }
        impl_->update_deadline(event);
        impl_->protect_range(event, event.requested_start, trigger.monotonic_time);
        impl_->active_events.push_back(std::move(event));
        ++impl_->accepted_triggers;
        ++impl_->events_created;
        return Result<EventWindowStartOutcome>::success(
            {.event = Impl::active_snapshot(impl_->active_events.back()),
             .duplicate = false,
             .merged = false});
    }

    auto primary_index = matches.front();
    for (const auto index : matches)
    {
        if (impl_->active_events[index].creation_order <
            impl_->active_events[primary_index].creation_order)
        {
            primary_index = index;
        }
    }
    if (impl_->active_events[primary_index].triggers.size() >= maximum_triggers_per_event)
    {
        ++impl_->rejected_triggers;
        return Result<EventWindowStartOutcome>::failure(window_error(
            "EVENT_INVALID_TRANSITION", Severity::warning, "事件触发记录已达到固定上限",
            "event.window.startOrMerge", std::move(source_event_id), "trigger-capacity"));
    }

    std::vector<std::size_t> absorb_indices;
    absorb_indices.reserve(matches.size());
    auto union_start = (std::min)(impl_->active_events[primary_index].requested_start, new_start);
    auto union_latest =
        (std::max)(impl_->active_events[primary_index].latest_trigger_time, trigger.monotonic_time);
    auto trigger_total = impl_->active_events[primary_index].triggers.size() + 1U;
    for (const auto index : matches)
    {
        if (index == primary_index)
            continue;
        const auto& candidate = impl_->active_events[index];
        const auto candidate_start = (std::min)(union_start, candidate.requested_start);
        const auto candidate_latest = (std::max)(union_latest, candidate.latest_trigger_time);
        if (candidate_latest >
                add_saturating(candidate_start, impl_->config.maximum_event_duration) ||
            candidate.triggers.size() > maximum_triggers_per_event - trigger_total)
        {
            continue;
        }
        absorb_indices.push_back(index);
        union_start = candidate_start;
        union_latest = candidate_latest;
        trigger_total += candidate.triggers.size();
    }

    auto& primary = impl_->active_events[primary_index];
    const auto old_start = primary.requested_start;
    primary.requested_start = (std::min)(primary.requested_start, new_start);
    primary.requested_end = (std::max)(primary.requested_end, natural_end);
    if (trigger.monotonic_time < primary.earliest_trigger_time)
    {
        primary.earliest_trigger_time = trigger.monotonic_time;
        primary.display_wall_clock_time = trigger.wall_clock_time;
    }
    primary.latest_trigger_time = (std::max)(primary.latest_trigger_time, trigger.monotonic_time);
    primary.triggers.push_back({.source_event_id = std::move(source_event_id), .trigger = trigger});
    ++primary.version;
    impl_->update_deadline(primary);
    if (primary.requested_start < old_start)
        impl_->protect_range(primary, primary.requested_start, old_start);

    std::sort(absorb_indices.begin(), absorb_indices.end(), std::greater<>{});
    for (const auto index : absorb_indices)
    {
        auto source = std::move(impl_->active_events[index]);
        impl_->active_events.erase(impl_->active_events.begin() +
                                   static_cast<std::ptrdiff_t>(index));
        if (index < primary_index)
            --primary_index;
        impl_->merge_event_into(impl_->active_events[primary_index], std::move(source));
    }
    ++impl_->accepted_triggers;
    ++impl_->events_merged;
    return Result<EventWindowStartOutcome>::success(
        {.event = Impl::active_snapshot(impl_->active_events[primary_index]),
         .duplicate = false,
         .merged = true});
}

std::vector<FrozenEventWindow> EventWindowManager::advance_time(
    const camera::MonotonicTime monotonic_time)
{
    std::scoped_lock lock{impl_->mutex};
    std::vector<FrozenEventWindow> result;
    result.reserve(impl_->active_events.size());
    for (std::size_t index = impl_->active_events.size(); index > 0U; --index)
    {
        auto& event = impl_->active_events[index - 1U];
        if (monotonic_time <= event.merge_deadline)
            continue;
        auto completed = std::move(event);
        impl_->active_events.erase(impl_->active_events.begin() +
                                   static_cast<std::ptrdiff_t>(index - 1U));
        result.push_back(impl_->freeze(std::move(completed), monotonic_time));
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.requested_start < right.requested_start;
    });
    return result;
}

std::vector<FrozenEventWindow> EventWindowManager::stop(const camera::MonotonicTime monotonic_time)
{
    std::scoped_lock lock{impl_->mutex};
    if (impl_->stopped)
        return {};
    impl_->stopped = true;
    std::vector<FrozenEventWindow> result;
    result.reserve(impl_->active_events.size());
    for (auto& event : impl_->active_events)
        result.push_back(impl_->freeze(std::move(event), monotonic_time));
    impl_->active_events.clear();
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.requested_start < right.requested_start;
    });
    return result;
}

Result<ActiveEventWindowSnapshot> EventWindowManager::active(const std::string_view event_id) const
{
    std::scoped_lock lock{impl_->mutex};
    const auto found = std::find_if(
        impl_->active_events.begin(), impl_->active_events.end(), [&](const auto& event) {
            return event.event_id == event_id ||
                   std::any_of(
                       event.triggers.begin(), event.triggers.end(),
                       [&](const auto& trigger) { return trigger.source_event_id == event_id; });
        });
    if (found == impl_->active_events.end())
    {
        return Result<ActiveEventWindowSnapshot>::failure(
            window_error("EVENT_NOT_FOUND", Severity::error, "活动事件窗口不存在",
                         "event.window.active", std::string{event_id}, "event-not-found"));
    }
    return Result<ActiveEventWindowSnapshot>::success(Impl::active_snapshot(*found));
}

EventWindowManagerSnapshot EventWindowManager::snapshot() const
{
    std::scoped_lock lock{impl_->mutex};
    EventWindowManagerSnapshot result{.stopped = impl_->stopped,
                                      .accepted_triggers = impl_->accepted_triggers,
                                      .duplicate_triggers = impl_->duplicate_triggers,
                                      .rejected_triggers = impl_->rejected_triggers,
                                      .events_created = impl_->events_created,
                                      .events_merged = impl_->events_merged,
                                      .events_frozen = impl_->events_frozen,
                                      .incomplete_events = impl_->incomplete_events};
    result.active_events.reserve(impl_->active_events.size());
    for (const auto& event : impl_->active_events)
        result.active_events.push_back(Impl::active_snapshot(event));
    return result;
}

} // namespace paperbreak::event
