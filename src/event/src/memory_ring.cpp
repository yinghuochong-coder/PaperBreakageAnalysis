#include "paperbreak/event/memory_ring.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace paperbreak::event
{
namespace
{

Error config_error(const MemoryRingPlanRequest& request, std::string message, std::string reason)
{
    auto error = make_error("SYS_CONFIG_INVALID", Severity::error, std::move(message), "event",
                            "event.memoryRing.plan");
    if (!request.camera_id.empty())
        error.source_id = request.camera_id;
    error.details.push_back({"reason", std::move(reason)});
    return error;
}

Error ring_error(std::string code, Severity severity, std::string message, std::string operation,
                 const std::string& camera_id)
{
    auto error =
        make_error(std::move(code), severity, std::move(message), "event", std::move(operation));
    error.source_id = camera_id;
    return error;
}

bool checked_add(const std::size_t left, const std::size_t right, std::size_t& result) noexcept
{
    if (right > (std::numeric_limits<std::size_t>::max)() - left)
        return false;
    result = left + right;
    return true;
}

bool checked_multiply(const std::size_t left, const std::size_t right, std::size_t& result) noexcept
{
    if (left != 0U && right > (std::numeric_limits<std::size_t>::max)() / left)
        return false;
    result = left * right;
    return true;
}

std::optional<std::size_t> frame_count_for_duration(const double seconds,
                                                    const double frames_per_second) noexcept
{
    if (!std::isfinite(seconds) || !std::isfinite(frames_per_second) || seconds < 0.0 ||
        frames_per_second <= 0.0)
        return std::nullopt;
    const long double frames =
        std::ceil(static_cast<long double>(seconds) * static_cast<long double>(frames_per_second));
    if (frames > static_cast<long double>((std::numeric_limits<std::size_t>::max)()))
        return std::nullopt;
    return static_cast<std::size_t>(frames);
}

double duration_seconds(const camera::MonotonicTime start, const camera::MonotonicTime end) noexcept
{
    if (end <= start)
        return 0.0;
    return std::chrono::duration<double>(end - start).count();
}

} // namespace

Result<MemoryRingPlan> plan_memory_ring(const MemoryRingPlanRequest& request)
{
    if (request.camera_id.empty() || !std::isfinite(request.configured_frame_rate) ||
        request.configured_frame_rate <= 0.0 || request.frame_buffer_capacity_bytes == 0U ||
        request.maximum_concurrent_events == 0U || request.configured_frame_pool_capacity == 0U ||
        request.memory_budget_bytes == 0U)
    {
        return Result<MemoryRingPlan>::failure(
            config_error(request, "内存环缓存规划参数无效", "invalid-plan-input"));
    }

    std::size_t base_ring_frames{};
    double planned_history_seconds{};
    if (request.capacity_mode == MemoryRingCapacityMode::duration)
    {
        if (!std::isfinite(request.configured_duration_seconds) ||
            request.configured_duration_seconds <= 0.0)
        {
            return Result<MemoryRingPlan>::failure(
                config_error(request, "按时长配置的环缓存时长必须大于零", "invalid-duration"));
        }
        const auto frames = frame_count_for_duration(request.configured_duration_seconds,
                                                     request.configured_frame_rate);
        if (!frames || *frames == 0U)
        {
            return Result<MemoryRingPlan>::failure(
                config_error(request, "环缓存帧数换算溢出", "ring-frame-overflow"));
        }
        base_ring_frames = *frames;
        planned_history_seconds = request.configured_duration_seconds;
    }
    else
    {
        if (request.configured_frame_count == 0U)
        {
            return Result<MemoryRingPlan>::failure(
                config_error(request, "按帧数配置的环缓存容量必须大于零", "invalid-frame-count"));
        }
        base_ring_frames = request.configured_frame_count;
        planned_history_seconds =
            static_cast<double>(base_ring_frames) / request.configured_frame_rate;
    }

    std::size_t ring_frames{};
    if (!checked_add(base_ring_frames, request.safety_margin_frames, ring_frames))
    {
        return Result<MemoryRingPlan>::failure(
            config_error(request, "环缓存安全余量导致帧数溢出", "ring-frame-overflow"));
    }

    const auto post_frames =
        frame_count_for_duration(request.post_event_seconds, request.configured_frame_rate);
    if (!post_frames)
    {
        return Result<MemoryRingPlan>::failure(
            config_error(request, "事件后置窗口帧数换算溢出", "post-frame-overflow"));
    }

    std::size_t pipeline_frames{};
    if (!checked_add(request.acquisition_queue_capacity, request.algorithm_queue_capacity,
                     pipeline_frames) ||
        !checked_add(pipeline_frames, request.preview_slot_count, pipeline_frames) ||
        !checked_add(pipeline_frames, request.nvme_queue_frames, pipeline_frames))
    {
        return Result<MemoryRingPlan>::failure(
            config_error(request, "管线缓冲帧数溢出", "pipeline-frame-overflow"));
    }

    std::size_t one_event_frames{};
    std::size_t event_lease_frames{};
    if (!checked_add(ring_frames, *post_frames, one_event_frames) ||
        !checked_multiply(one_event_frames, request.maximum_concurrent_events, event_lease_frames))
    {
        return Result<MemoryRingPlan>::failure(
            config_error(request, "事件租约预算帧数溢出", "lease-frame-overflow"));
    }

    std::size_t required_pool_frames{};
    if (!checked_add(ring_frames, pipeline_frames, required_pool_frames) ||
        !checked_add(required_pool_frames, event_lease_frames, required_pool_frames))
    {
        return Result<MemoryRingPlan>::failure(
            config_error(request, "总帧池容量计算溢出", "pool-frame-overflow"));
    }

    std::size_t required_bytes{};
    if (!checked_multiply(required_pool_frames, request.frame_buffer_capacity_bytes,
                          required_bytes))
    {
        return Result<MemoryRingPlan>::failure(
            config_error(request, "内存预算字节数计算溢出", "memory-byte-overflow"));
    }
    if (request.configured_frame_pool_capacity < required_pool_frames)
    {
        auto error = config_error(request, "配置的固定帧池容量不足", "frame-pool-insufficient");
        error.details.push_back({"requiredFrames", std::to_string(required_pool_frames)});
        error.details.push_back(
            {"configuredFrames", std::to_string(request.configured_frame_pool_capacity)});
        return Result<MemoryRingPlan>::failure(std::move(error));
    }
    if (request.memory_budget_bytes < required_bytes)
    {
        auto error = config_error(request, "配置的相机内存预算不足", "memory-budget-exceeded");
        error.details.push_back({"requiredBytes", std::to_string(required_bytes)});
        error.details.push_back({"budgetBytes", std::to_string(request.memory_budget_bytes)});
        return Result<MemoryRingPlan>::failure(std::move(error));
    }

    return Result<MemoryRingPlan>::success({.camera_id = request.camera_id,
                                            .ring_capacity_frames = ring_frames,
                                            .post_event_frames = *post_frames,
                                            .pipeline_frames = pipeline_frames,
                                            .event_lease_budget_frames = event_lease_frames,
                                            .required_frame_pool_capacity = required_pool_frames,
                                            .required_memory_bytes = required_bytes,
                                            .planned_history_seconds = planned_history_seconds});
}

struct MemoryRingLease::CounterState final
{
    CounterState(const std::size_t active_leases, const std::size_t frame_references)
        : maximum_active_leases(active_leases), maximum_frame_references(frame_references)
    {
    }

    std::mutex mutex;
    const std::size_t maximum_active_leases;
    const std::size_t maximum_frame_references;
    std::size_t active_leases{};
    std::size_t frame_references{};
};

MemoryRingLease::MemoryRingLease(std::vector<camera::FrameView> frames, MemoryWindowInfo info,
                                 std::shared_ptr<CounterState> counters) noexcept
    : frames_(std::move(frames)), info_(info), counters_(std::move(counters))
{
}

MemoryRingLease::~MemoryRingLease()
{
    release();
}

MemoryRingLease::MemoryRingLease(MemoryRingLease&& other) noexcept
    : frames_(std::move(other.frames_)), info_(other.info_), counters_(std::move(other.counters_))
{
}

MemoryRingLease& MemoryRingLease::operator=(MemoryRingLease&& other) noexcept
{
    if (this != &other)
    {
        release();
        frames_ = std::move(other.frames_);
        info_ = other.info_;
        counters_ = std::move(other.counters_);
    }
    return *this;
}

std::span<const camera::FrameView> MemoryRingLease::frames() const noexcept
{
    return frames_;
}

const MemoryWindowInfo& MemoryRingLease::info() const noexcept
{
    return info_;
}

void MemoryRingLease::release() noexcept
{
    if (!counters_)
        return;
    {
        std::scoped_lock lock{counters_->mutex};
        if (counters_->active_leases > 0U)
            --counters_->active_leases;
        if (counters_->frame_references >= frames_.size())
            counters_->frame_references -= frames_.size();
        else
            counters_->frame_references = 0U;
    }
    frames_.clear();
    counters_.reset();
}

struct MemoryRing::Impl final
{
    explicit Impl(MemoryRingOptions options)
        : options(std::move(options)), slots(this->options.capacity_frames),
          counters(std::make_shared<MemoryRingLease::CounterState>(
              this->options.maximum_active_leases, this->options.maximum_leased_frame_references))
    {
    }

    std::optional<MemoryRingShortageNotice> update_shortage_locked(
        const bool active, const MemoryRingShortageReason reason, const double requested_seconds,
        const double available_seconds, const std::size_t requested_frames,
        const std::size_t available_frames)
    {
        if (shortage_active == active && (!active || shortage_reason == reason))
            return std::nullopt;
        shortage_active = active;
        shortage_reason = reason;
        return MemoryRingShortageNotice{.active = active,
                                        .reason = reason,
                                        .camera_id = options.camera_id,
                                        .requested_history_seconds = requested_seconds,
                                        .available_history_seconds = available_seconds,
                                        .requested_frames = requested_frames,
                                        .available_frames = available_frames};
    }

    void notify(const std::optional<MemoryRingShortageNotice>& notice) noexcept
    {
        if (!notice || !options.shortage_callback)
            return;
        try
        {
            options.shortage_callback(*notice);
        }
        catch (...)
        {
            std::scoped_lock lock{mutex};
            ++callback_failures;
        }
    }

    [[nodiscard]] double actual_history_seconds_locked() const noexcept
    {
        if (size < 2U)
            return 0.0;
        const auto newest_index = (head + size - 1U) % slots.size();
        return duration_seconds(slots[head]->received_monotonic_time(),
                                slots[newest_index]->received_monotonic_time());
    }

    MemoryRingOptions options;
    mutable std::mutex mutex;
    std::vector<std::optional<camera::FrameView>> slots;
    std::shared_ptr<MemoryRingLease::CounterState> counters;
    std::size_t head{};
    std::size_t size{};
    std::size_t resident_bytes{};
    std::optional<std::uint64_t> last_sequence_number;
    std::optional<camera::MonotonicTime> last_monotonic_time;
    std::uint64_t inserted{};
    std::uint64_t overwritten{};
    std::uint64_t rejected{};
    std::uint64_t observed_sequence_gaps{};
    std::uint64_t incomplete_windows{};
    std::uint64_t lease_capacity_rejections{};
    std::uint64_t callback_failures{};
    MemoryRingShortageReason shortage_reason{MemoryRingShortageReason::history_span};
    bool shortage_active{};
    bool closed{};
};

MemoryRing::MemoryRing(MemoryRingOptions options)
{
    if (options.camera_id.empty() || options.capacity_frames == 0U ||
        !std::isfinite(options.required_history_seconds) ||
        options.required_history_seconds < 0.0 || options.maximum_active_leases == 0U ||
        options.maximum_leased_frame_references == 0U)
    {
        throw std::invalid_argument{"MemoryRing options are invalid"};
    }
    impl_ = std::make_unique<Impl>(std::move(options));
}

MemoryRing::~MemoryRing()
{
    close();
}

Result<MemoryRingPushStatus> MemoryRing::push(camera::FrameView frame)
{
    std::optional<MemoryRingShortageNotice> notice;
    MemoryRingPushStatus status{MemoryRingPushStatus::inserted};
    {
        std::scoped_lock lock{impl_->mutex};
        if (impl_->closed)
            return Result<MemoryRingPushStatus>::success(MemoryRingPushStatus::closed);
        if (frame.camera_id() != impl_->options.camera_id ||
            (impl_->last_monotonic_time &&
             frame.received_monotonic_time() <= *impl_->last_monotonic_time) ||
            (impl_->last_sequence_number &&
             frame.sequence_number() <= *impl_->last_sequence_number))
        {
            ++impl_->rejected;
            return Result<MemoryRingPushStatus>::failure(
                ring_error("PIPELINE_FRAME_ORDER_VIOLATION", Severity::warning,
                           "内存环缓存拒绝了错误相机或非递增帧", "event.memoryRing.push",
                           impl_->options.camera_id));
        }
        if (impl_->last_sequence_number)
        {
            const auto distance = frame.sequence_number() - *impl_->last_sequence_number;
            if (distance > 1U)
            {
                const auto gap = distance - 1U;
                if (gap >
                    (std::numeric_limits<std::uint64_t>::max)() - impl_->observed_sequence_gaps)
                {
                    impl_->observed_sequence_gaps = (std::numeric_limits<std::uint64_t>::max)();
                }
                else
                {
                    impl_->observed_sequence_gaps += gap;
                }
            }
        }

        std::size_t index{};
        if (impl_->size < impl_->slots.size())
        {
            index = (impl_->head + impl_->size) % impl_->slots.size();
            ++impl_->size;
        }
        else
        {
            index = impl_->head;
            impl_->head = (impl_->head + 1U) % impl_->slots.size();
            impl_->resident_bytes -= impl_->slots[index]->buffer_owner()->capacity();
            ++impl_->overwritten;
            status = MemoryRingPushStatus::overwritten;
        }
        impl_->resident_bytes += frame.buffer_owner()->capacity();
        impl_->last_sequence_number = frame.sequence_number();
        impl_->last_monotonic_time = frame.received_monotonic_time();
        impl_->slots[index] = std::move(frame);
        ++impl_->inserted;

        const double actual_seconds = impl_->actual_history_seconds_locked();
        const bool insufficient = impl_->size == impl_->slots.size() &&
                                  impl_->options.required_history_seconds > actual_seconds;
        notice = impl_->update_shortage_locked(insufficient, MemoryRingShortageReason::history_span,
                                               impl_->options.required_history_seconds,
                                               actual_seconds, impl_->slots.size(), impl_->size);
    }
    impl_->notify(notice);
    return Result<MemoryRingPushStatus>::success(status);
}

Result<MemoryRingLease> MemoryRing::lease_window(const camera::MonotonicTime start,
                                                 const camera::MonotonicTime end)
{
    if (end < start)
    {
        return Result<MemoryRingLease>::failure(ring_error(
            "EVENT_BUFFER_INCOMPLETE", Severity::critical, "事件缓存窗口的单调时间范围无效",
            "event.memoryRing.lease", impl_->options.camera_id));
    }

    std::vector<camera::FrameView> frames;
    MemoryWindowInfo info{.requested_start = start, .requested_end = end};
    std::optional<MemoryRingShortageNotice> notice;
    {
        std::scoped_lock lock{impl_->mutex};
        if (impl_->closed || impl_->size == 0U)
        {
            ++impl_->incomplete_windows;
            notice =
                impl_->update_shortage_locked(true, MemoryRingShortageReason::incomplete_window,
                                              duration_seconds(start, end), 0.0, 0U, 0U);
        }
        else
        {
            frames.reserve(impl_->size);
            for (std::size_t offset = 0U; offset < impl_->size; ++offset)
            {
                const auto index = (impl_->head + offset) % impl_->slots.size();
                const auto& frame = *impl_->slots[index];
                const auto time = frame.received_monotonic_time();
                if (time >= start && time <= end)
                    frames.push_back(frame);
            }

            const auto& oldest = *impl_->slots[impl_->head];
            const auto newest_index = (impl_->head + impl_->size - 1U) % impl_->slots.size();
            const auto& newest = *impl_->slots[newest_index];
            info.available_start = oldest.received_monotonic_time();
            info.available_end = newest.received_monotonic_time();
            info.complete =
                !frames.empty() && info.available_start <= start && info.available_end >= end;
            if (!frames.empty())
            {
                info.first_sequence_number = frames.front().sequence_number();
                info.last_sequence_number = frames.back().sequence_number();
                for (std::size_t index = 1U; index < frames.size(); ++index)
                {
                    const auto previous = frames[index - 1U].sequence_number();
                    const auto current = frames[index].sequence_number();
                    const auto distance = current - previous;
                    if (distance > 1U)
                    {
                        const auto gap = distance - 1U;
                        if (gap > (std::numeric_limits<std::uint64_t>::max)() - info.sequence_gaps)
                        {
                            info.sequence_gaps = (std::numeric_limits<std::uint64_t>::max)();
                        }
                        else
                        {
                            info.sequence_gaps += gap;
                        }
                    }
                }
            }
            if (!info.complete)
            {
                ++impl_->incomplete_windows;
                notice = impl_->update_shortage_locked(
                    true, MemoryRingShortageReason::incomplete_window, duration_seconds(start, end),
                    impl_->actual_history_seconds_locked(), impl_->size, frames.size());
            }
            else
            {
                notice = impl_->update_shortage_locked(
                    false, MemoryRingShortageReason::incomplete_window,
                    duration_seconds(start, end), impl_->actual_history_seconds_locked(),
                    frames.size(), frames.size());
            }
        }
    }

    if (frames.empty())
    {
        impl_->notify(notice);
        return Result<MemoryRingLease>::failure(
            ring_error("EVENT_BUFFER_INCOMPLETE", Severity::critical, "事件缓存窗口没有可用帧",
                       "event.memoryRing.lease", impl_->options.camera_id));
    }

    bool capacity_available{};
    {
        std::scoped_lock lock{impl_->counters->mutex};
        const auto remaining_references =
            impl_->counters->maximum_frame_references - impl_->counters->frame_references;
        capacity_available =
            impl_->counters->active_leases < impl_->counters->maximum_active_leases &&
            frames.size() <= remaining_references;
        if (capacity_available)
        {
            ++impl_->counters->active_leases;
            impl_->counters->frame_references += frames.size();
        }
    }
    if (!capacity_available)
    {
        {
            std::scoped_lock lock{impl_->mutex};
            ++impl_->lease_capacity_rejections;
            notice = impl_->update_shortage_locked(
                true, MemoryRingShortageReason::lease_capacity, duration_seconds(start, end),
                impl_->actual_history_seconds_locked(), frames.size(), 0U);
        }
        impl_->notify(notice);
        return Result<MemoryRingLease>::failure(ring_error(
            "EVENT_BUFFER_INCOMPLETE", Severity::critical, "事件缓存租约容量已达固定上限",
            "event.memoryRing.lease", impl_->options.camera_id));
    }

    impl_->notify(notice);
    return Result<MemoryRingLease>::success(
        MemoryRingLease{std::move(frames), info, impl_->counters});
}

void MemoryRing::close() noexcept
{
    std::scoped_lock lock{impl_->mutex};
    if (impl_->closed)
        return;
    for (auto& slot : impl_->slots)
        slot.reset();
    impl_->head = 0U;
    impl_->size = 0U;
    impl_->resident_bytes = 0U;
    impl_->closed = true;
}

MemoryRingSnapshot MemoryRing::snapshot() const noexcept
{
    MemoryRingSnapshot result;
    {
        std::scoped_lock lock{impl_->mutex};
        result = {.camera_id = impl_->options.camera_id,
                  .capacity_frames = impl_->slots.size(),
                  .stored_frames = impl_->size,
                  .resident_bytes = impl_->resident_bytes,
                  .occupancy_ratio =
                      static_cast<double>(impl_->size) / static_cast<double>(impl_->slots.size()),
                  .actual_history_seconds = impl_->actual_history_seconds_locked(),
                  .inserted = impl_->inserted,
                  .overwritten = impl_->overwritten,
                  .rejected = impl_->rejected,
                  .observed_sequence_gaps = impl_->observed_sequence_gaps,
                  .maximum_active_leases = impl_->options.maximum_active_leases,
                  .maximum_leased_frame_references = impl_->options.maximum_leased_frame_references,
                  .incomplete_windows = impl_->incomplete_windows,
                  .lease_capacity_rejections = impl_->lease_capacity_rejections,
                  .callback_failures = impl_->callback_failures,
                  .shortage_active = impl_->shortage_active,
                  .closed = impl_->closed};
    }
    {
        std::scoped_lock lock{impl_->counters->mutex};
        result.active_leases = impl_->counters->active_leases;
        result.leased_frame_references = impl_->counters->frame_references;
    }
    return result;
}

} // namespace paperbreak::event
