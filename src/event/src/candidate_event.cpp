#include "paperbreak/event/candidate_event.hpp"

#include "paperbreak/event/memory_ring.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <utility>

namespace paperbreak::event
{
namespace
{

constexpr std::size_t maximum_camera_count = 4U;
constexpr std::uint16_t random_high_mask = 0x0fffU;
constexpr std::uint64_t random_low_mask = 0x3fff'ffff'ffff'ffffULL;
constexpr std::uint64_t uuid_timestamp_mask = 0x0000'ffff'ffff'ffffULL;

Error candidate_error(std::string business_code, const Severity severity, std::string message,
                      std::string operation, std::string source_id, std::string reason)
{
    auto error = make_error(std::move(business_code), severity, std::move(message), "event",
                            std::move(operation));
    if (!source_id.empty())
        error.source_id = std::move(source_id);
    error.details.push_back({"reason", std::move(reason)});
    return error;
}

Error config_error(std::string source_id, std::string reason)
{
    return candidate_error("SYS_CONFIG_INVALID", Severity::error, "候选事件状态机配置无效",
                           "event.candidate.create", std::move(source_id), std::move(reason));
}

Error id_generation_error(std::string message, std::string reason)
{
    auto error = candidate_error("SYS_ID_GENERATION_FAILED", Severity::critical, std::move(message),
                                 "event.candidate.generateId", {}, std::move(reason));
    error.retryable = true;
    return error;
}

bool valid_camera_id(const std::string_view camera_id) noexcept
{
    return camera_id.size() == 5U && camera_id.starts_with("CAM0") && camera_id[4] >= '1' &&
           camera_id[4] <= '4';
}

bool same_result(const algorithm::TriggerResult& left,
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

class UuidV7Generator final
{
  public:
    [[nodiscard]] Result<std::string> next()
    {
        try
        {
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            if (now < 0)
            {
                return Result<std::string>::failure(id_generation_error(
                    "系统墙上时间早于 UUIDv7 可表示范围", "wall-clock-before-unix-epoch"));
            }

            auto timestamp = static_cast<std::uint64_t>(now);
            if (timestamp > uuid_timestamp_mask)
            {
                return Result<std::string>::failure(id_generation_error(
                    "系统墙上时间超出 UUIDv7 可表示范围", "wall-clock-out-of-range"));
            }

            if (!initialized_ || timestamp > last_timestamp_ms_)
            {
                last_timestamp_ms_ = timestamp;
                random_high_ = static_cast<std::uint16_t>(random_64() & random_high_mask);
                random_low_ = random_64() & random_low_mask;
                initialized_ = true;
            }
            else
            {
                timestamp = last_timestamp_ms_;
                increment_random(timestamp);
                last_timestamp_ms_ = timestamp;
            }

            std::array<std::uint8_t, 16U> bytes{};
            bytes[0] = static_cast<std::uint8_t>(timestamp >> 40U);
            bytes[1] = static_cast<std::uint8_t>(timestamp >> 32U);
            bytes[2] = static_cast<std::uint8_t>(timestamp >> 24U);
            bytes[3] = static_cast<std::uint8_t>(timestamp >> 16U);
            bytes[4] = static_cast<std::uint8_t>(timestamp >> 8U);
            bytes[5] = static_cast<std::uint8_t>(timestamp);
            bytes[6] = static_cast<std::uint8_t>(0x70U | (random_high_ >> 8U));
            bytes[7] = static_cast<std::uint8_t>(random_high_);
            bytes[8] = static_cast<std::uint8_t>(0x80U | (random_low_ >> 56U));
            for (std::size_t index = 9U; index < bytes.size(); ++index)
            {
                const auto shift = static_cast<unsigned>((15U - index) * 8U);
                bytes[index] = static_cast<std::uint8_t>(random_low_ >> shift);
            }
            return Result<std::string>::success(format(bytes));
        }
        catch (const std::exception&)
        {
            return Result<std::string>::failure(
                id_generation_error("无法从系统熵源生成 UUIDv7", "entropy-source-failed"));
        }
    }

  private:
    std::uint64_t random_64()
    {
        std::uniform_int_distribution<std::uint64_t> distribution;
        return distribution(entropy_);
    }

    void increment_random(std::uint64_t& timestamp) noexcept
    {
        if (random_low_ < random_low_mask)
        {
            ++random_low_;
            return;
        }
        random_low_ = 0U;
        if (random_high_ < random_high_mask)
        {
            ++random_high_;
            return;
        }
        random_high_ = 0U;
        if (timestamp < uuid_timestamp_mask)
            ++timestamp;
    }

    static std::string format(const std::array<std::uint8_t, 16U>& bytes)
    {
        constexpr std::string_view digits = "0123456789abcdef";
        std::string result{"EVT-"};
        result.reserve(40U);
        for (std::size_t index = 0U; index < bytes.size(); ++index)
        {
            if (index == 4U || index == 6U || index == 8U || index == 10U)
                result.push_back('-');
            result.push_back(digits[bytes[index] >> 4U]);
            result.push_back(digits[bytes[index] & 0x0fU]);
        }
        return result;
    }

    std::random_device entropy_;
    std::uint64_t last_timestamp_ms_{};
    std::uint16_t random_high_{};
    std::uint64_t random_low_{};
    bool initialized_{};
};

} // namespace

std::string_view to_string(const CandidateEventState state) noexcept
{
    switch (state)
    {
    case CandidateEventState::idle:
        return "Idle";
    case CandidateEventState::suspicious:
        return "Suspicious";
    case CandidateEventState::candidate:
        return "Candidate";
    case CandidateEventState::confirmed:
        return "Confirmed";
    case CandidateEventState::rejected:
        return "Rejected";
    case CandidateEventState::timeout:
        return "Timeout";
    }
    return "Idle";
}

struct CandidateEventManager::Impl final
{
    struct ActiveEvent final
    {
        CandidateEventSnapshot snapshot;
        std::optional<MemoryRingLease> pre_buffer_lease;
    };

    struct CameraTracker final
    {
        std::string camera_id;
        MemoryRing* memory_ring{};
        CandidateEventState state{CandidateEventState::idle};
        std::size_t consecutive_triggered_frames{};
        std::optional<algorithm::TriggerResult> first_suspicious_trigger;
        std::optional<algorithm::TriggerResult> last_result;
        std::optional<ActiveEvent> event;
    };

    explicit Impl(CandidateEventManagerConfig value) : config(std::move(value))
    {
        cameras.reserve(config.cameras.size());
        for (auto& binding : config.cameras)
        {
            cameras.push_back(
                {.camera_id = std::move(binding.camera_id), .memory_ring = binding.memory_ring});
        }
    }

    [[nodiscard]] CameraTracker* find_camera(const std::string_view camera_id) noexcept
    {
        const auto found = std::find_if(cameras.begin(), cameras.end(), [&](const auto& camera) {
            return camera.camera_id == camera_id;
        });
        return found == cameras.end() ? nullptr : &*found;
    }

    [[nodiscard]] CameraTracker* find_event(const std::string_view event_id) noexcept
    {
        const auto found = std::find_if(cameras.begin(), cameras.end(), [&](const auto& camera) {
            return camera.event && camera.event->snapshot.event_id == event_id;
        });
        return found == cameras.end() ? nullptr : &*found;
    }

    [[nodiscard]] static CandidateCameraSnapshot camera_snapshot(const CameraTracker& camera)
    {
        return {.camera_id = camera.camera_id,
                .observation_state = camera.state,
                .consecutive_triggered_frames = camera.consecutive_triggered_frames,
                .event = camera.event ? std::optional{camera.event->snapshot} : std::nullopt};
    }

    [[nodiscard]] Result<void> validate_result(const CameraTracker& camera,
                                               const algorithm::TriggerResult& result) const
    {
        if (result.triggered == (result.trigger_source == algorithm::TriggerSource::none))
        {
            return Result<void>::failure(
                candidate_error("PIPELINE_FRAME_ORDER_VIOLATION", Severity::warning,
                                "检测结果的触发标志和来源不一致", "event.candidate.process",
                                camera.camera_id, "trigger-source-inconsistent"));
        }
        if (!std::isfinite(result.mean_grayscale) || !std::isfinite(result.mean_grayscale_change) ||
            !std::isfinite(result.paper_ratio))
        {
            return Result<void>::failure(candidate_error(
                "PIPELINE_FRAME_ORDER_VIOLATION", Severity::warning, "检测结果包含非有限数值",
                "event.candidate.process", camera.camera_id, "non-finite-metric"));
        }
        if (camera.last_result)
        {
            if (result.sequence_number < camera.last_result->sequence_number ||
                (result.sequence_number > camera.last_result->sequence_number &&
                 result.monotonic_time <= camera.last_result->monotonic_time))
            {
                return Result<void>::failure(
                    candidate_error("PIPELINE_FRAME_ORDER_VIOLATION", Severity::warning,
                                    "候选事件状态机拒绝了回退的检测结果", "event.candidate.process",
                                    camera.camera_id, "result-order-regression"));
            }
            if (result.sequence_number == camera.last_result->sequence_number &&
                !same_result(result, *camera.last_result))
            {
                return Result<void>::failure(
                    candidate_error("PIPELINE_FRAME_ORDER_VIOLATION", Severity::warning,
                                    "同一序号的检测结果内容发生冲突", "event.candidate.process",
                                    camera.camera_id, "duplicate-result-conflict"));
            }
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> create_candidate(
        CameraTracker& camera, const algorithm::TriggerResult& result,
        std::vector<CandidateEventNotification>& notifications)
    {
        auto event_id = id_generator.next();
        if (!event_id)
            return Result<void>::failure(std::move(event_id.error()));

        CandidateEventSnapshot snapshot{
            .event_id = std::move(event_id).value(),
            .camera_id = camera.camera_id,
            .decision_state = CandidateEventState::candidate,
            .version = 1U,
            .first_suspicious_trigger = *camera.first_suspicious_trigger,
            .candidate_trigger = result,
            .candidate_deadline = add_saturating(result.monotonic_time, config.candidate_timeout),
            .post_collection_started = true,
        };

        std::optional<MemoryRingLease> lease;
        auto protected_window = camera.memory_ring->lease_window(
            subtract_saturating(result.monotonic_time, config.pre_event_duration),
            result.monotonic_time);
        if (protected_window)
        {
            lease.emplace(std::move(protected_window).value());
            snapshot.pre_buffer_protection_acquired = true;
            snapshot.pre_buffer_complete = lease->info().complete;
            snapshot.pre_buffer_frame_count = lease->frames().size();
            snapshot.pre_buffer_sequence_gaps = lease->info().sequence_gaps;
            if (!snapshot.pre_buffer_complete)
                snapshot.pre_buffer_error_code = "EVENT_BUFFER_INCOMPLETE";
        }
        else
        {
            snapshot.pre_buffer_error_code = protected_window.error().business_code;
        }

        camera.event.emplace(
            ActiveEvent{.snapshot = std::move(snapshot), .pre_buffer_lease = std::move(lease)});
        camera.state = CandidateEventState::candidate;
        ++events_created;
        notifications.push_back({.kind = CandidateNotificationKind::candidate_created,
                                 .event = camera.event->snapshot});
        return Result<void>::success();
    }

    void transition_decision(CameraTracker& camera, const CandidateEventState state,
                             const camera::MonotonicTime monotonic_time,
                             const camera::WallClockTime wall_clock_time,
                             std::vector<CandidateEventNotification>& notifications)
    {
        camera.state = state;
        camera.event->snapshot.decision_state = state;
        ++camera.event->snapshot.version;
        camera.event->snapshot.decision = CandidateDecision{
            .state = state, .monotonic_time = monotonic_time, .wall_clock_time = wall_clock_time};
        if (state == CandidateEventState::confirmed)
            ++confirmed_events;
        else if (state == CandidateEventState::rejected)
            ++rejected_events;
        else if (state == CandidateEventState::timeout)
            ++timed_out_events;
        notifications.push_back(
            {.kind = CandidateNotificationKind::decision_changed, .event = camera.event->snapshot});
    }

    void deliver(const std::vector<CandidateEventNotification>& notifications) noexcept
    {
        if (!config.notification_callback)
            return;
        for (const auto& notification : notifications)
        {
            try
            {
                config.notification_callback(notification);
            }
            catch (...)
            {
                std::scoped_lock lock{mutex};
                ++callback_failures;
            }
        }
    }

    [[nodiscard]] Result<CandidateCommandOutcome> decision_command(
        const std::string_view event_id, const std::uint64_t expected_version,
        const CandidateEventState target, const camera::MonotonicTime monotonic_time,
        const camera::WallClockTime wall_clock_time)
    {
        std::vector<CandidateEventNotification> notifications;
        notifications.reserve(1U);
        std::optional<Result<CandidateCommandOutcome>> response;
        {
            std::scoped_lock lock{mutex};
            auto* camera = find_event(event_id);
            if (camera == nullptr)
            {
                response.emplace(Result<CandidateCommandOutcome>::failure(candidate_error(
                    "EVENT_NOT_FOUND", Severity::error, "候选事件不存在",
                    target == CandidateEventState::confirmed ? "event.candidate.confirm"
                                                             : "event.candidate.reject",
                    std::string{event_id}, "event-not-found")));
            }
            else if (camera->state == target)
            {
                response.emplace(Result<CandidateCommandOutcome>::success(
                    {.event = camera->event->snapshot, .duplicate = true}));
            }
            else if (expected_version != camera->event->snapshot.version)
            {
                auto error = candidate_error(
                    "EVENT_VERSION_CONFLICT", Severity::warning, "候选事件版本冲突",
                    target == CandidateEventState::confirmed ? "event.candidate.confirm"
                                                             : "event.candidate.reject",
                    camera->event->snapshot.event_id, "expected-version-mismatch");
                error.details.push_back({"expectedVersion", std::to_string(expected_version)});
                error.details.push_back(
                    {"currentVersion", std::to_string(camera->event->snapshot.version)});
                response.emplace(Result<CandidateCommandOutcome>::failure(std::move(error)));
            }
            else if (camera->state != CandidateEventState::candidate || stopped)
            {
                response.emplace(Result<CandidateCommandOutcome>::failure(candidate_error(
                    "EVENT_INVALID_TRANSITION", Severity::error, "候选事件当前状态不允许该转换",
                    target == CandidateEventState::confirmed ? "event.candidate.confirm"
                                                             : "event.candidate.reject",
                    camera->event->snapshot.event_id, "terminal-or-stopped")));
            }
            else if (monotonic_time < camera->event->snapshot.candidate_trigger.monotonic_time)
            {
                response.emplace(Result<CandidateCommandOutcome>::failure(candidate_error(
                    "EVENT_INVALID_TRANSITION", Severity::error, "候选事件决策时间早于候选时间",
                    target == CandidateEventState::confirmed ? "event.candidate.confirm"
                                                             : "event.candidate.reject",
                    camera->event->snapshot.event_id, "decision-time-before-candidate")));
            }
            else if (monotonic_time >= camera->event->snapshot.candidate_deadline)
            {
                transition_decision(*camera, CandidateEventState::timeout, monotonic_time,
                                    wall_clock_time, notifications);
                response.emplace(Result<CandidateCommandOutcome>::failure(candidate_error(
                    "EVENT_INVALID_TRANSITION", Severity::warning, "候选事件已达到确认截止时间",
                    target == CandidateEventState::confirmed ? "event.candidate.confirm"
                                                             : "event.candidate.reject",
                    camera->event->snapshot.event_id, "candidate-deadline-reached")));
            }
            else
            {
                transition_decision(*camera, target, monotonic_time, wall_clock_time,
                                    notifications);
                response.emplace(Result<CandidateCommandOutcome>::success(
                    {.event = camera->event->snapshot, .duplicate = false}));
            }
        }
        deliver(notifications);
        return std::move(*response);
    }

    CandidateEventManagerConfig config;
    mutable std::mutex mutex;
    std::vector<CameraTracker> cameras;
    UuidV7Generator id_generator;
    std::uint64_t accepted_results{};
    std::uint64_t duplicate_results{};
    std::uint64_t rejected_results{};
    std::uint64_t events_created{};
    std::uint64_t confirmed_events{};
    std::uint64_t rejected_events{};
    std::uint64_t timed_out_events{};
    std::uint64_t callback_failures{};
    bool stopped{};
};

Result<std::unique_ptr<CandidateEventManager>> CandidateEventManager::create(
    CandidateEventManagerConfig config)
{
    if (config.cameras.empty() || config.cameras.size() > maximum_camera_count)
    {
        return Result<std::unique_ptr<CandidateEventManager>>::failure(
            config_error({}, "camera-count-out-of-range"));
    }
    if (config.candidate_consecutive_frames == 0U ||
        config.confirmation_consecutive_frames < config.candidate_consecutive_frames)
    {
        return Result<std::unique_ptr<CandidateEventManager>>::failure(
            config_error({}, "invalid-consecutive-frame-threshold"));
    }
    if (config.candidate_timeout <= std::chrono::milliseconds::zero() ||
        config.pre_event_duration < std::chrono::milliseconds::zero())
    {
        return Result<std::unique_ptr<CandidateEventManager>>::failure(
            config_error({}, "invalid-event-duration"));
    }
    const auto maximum_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        camera::MonotonicTime::duration::max());
    if (config.candidate_timeout > maximum_duration || config.pre_event_duration > maximum_duration)
    {
        return Result<std::unique_ptr<CandidateEventManager>>::failure(
            config_error({}, "event-duration-out-of-range"));
    }
    for (std::size_t index = 0U; index < config.cameras.size(); ++index)
    {
        const auto& binding = config.cameras[index];
        if (!valid_camera_id(binding.camera_id) || binding.memory_ring == nullptr)
        {
            return Result<std::unique_ptr<CandidateEventManager>>::failure(
                config_error(binding.camera_id, "invalid-camera-binding"));
        }
        const auto ring = binding.memory_ring->snapshot();
        if (ring.camera_id != binding.camera_id || ring.closed)
        {
            return Result<std::unique_ptr<CandidateEventManager>>::failure(
                config_error(binding.camera_id, "memory-ring-binding-mismatch"));
        }
        for (std::size_t previous = 0U; previous < index; ++previous)
        {
            if (config.cameras[previous].camera_id == binding.camera_id)
            {
                return Result<std::unique_ptr<CandidateEventManager>>::failure(
                    config_error(binding.camera_id, "duplicate-camera-binding"));
            }
        }
    }

    try
    {
        return Result<std::unique_ptr<CandidateEventManager>>::success(
            std::make_unique<CandidateEventManager>(ConstructionKey{}, std::move(config)));
    }
    catch (const std::exception&)
    {
        auto error = id_generation_error("无法初始化候选事件 ID 生成器",
                                         "entropy-source-initialization-failed");
        error.operation = "event.candidate.create";
        return Result<std::unique_ptr<CandidateEventManager>>::failure(std::move(error));
    }
}

CandidateEventManager::CandidateEventManager(ConstructionKey, CandidateEventManagerConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

CandidateEventManager::~CandidateEventManager() = default;

Result<CandidateProcessOutcome> CandidateEventManager::process(
    const algorithm::TriggerResult& result)
{
    std::vector<CandidateEventNotification> notifications;
    notifications.reserve(3U);
    std::optional<Result<CandidateProcessOutcome>> response;
    {
        std::scoped_lock lock{impl_->mutex};
        if (impl_->stopped)
        {
            ++impl_->rejected_results;
            response.emplace(Result<CandidateProcessOutcome>::failure(candidate_error(
                "EVENT_INVALID_TRANSITION", Severity::error, "服务停止后不再接受候选检测结果",
                "event.candidate.process", result.camera_id, "manager-stopped")));
        }
        else if (auto* camera = impl_->find_camera(result.camera_id); camera == nullptr)
        {
            ++impl_->rejected_results;
            response.emplace(Result<CandidateProcessOutcome>::failure(
                candidate_error("PIPELINE_FRAME_ORDER_VIOLATION", Severity::warning,
                                "检测结果引用了未配置的逻辑相机", "event.candidate.process",
                                result.camera_id, "unknown-camera")));
        }
        else if (auto valid = impl_->validate_result(*camera, result); !valid)
        {
            ++impl_->rejected_results;
            response.emplace(Result<CandidateProcessOutcome>::failure(std::move(valid.error())));
        }
        else if (camera->last_result &&
                 result.sequence_number == camera->last_result->sequence_number)
        {
            ++impl_->duplicate_results;
            response.emplace(Result<CandidateProcessOutcome>::success(
                {.camera = Impl::camera_snapshot(*camera), .duplicate = true}));
        }
        else
        {
            ++impl_->accepted_results;
            camera->last_result = result;

            if (camera->state == CandidateEventState::candidate && camera->event &&
                result.monotonic_time >= camera->event->snapshot.candidate_deadline)
            {
                impl_->transition_decision(*camera, CandidateEventState::timeout,
                                           result.monotonic_time, result.wall_clock_time,
                                           notifications);
            }

            if (camera->state == CandidateEventState::confirmed ||
                camera->state == CandidateEventState::rejected ||
                camera->state == CandidateEventState::timeout)
            {
                camera->event.reset();
                camera->state = CandidateEventState::idle;
                camera->consecutive_triggered_frames = 0U;
                camera->first_suspicious_trigger.reset();
            }

            if (camera->state == CandidateEventState::candidate)
            {
                if (result.triggered)
                {
                    if (camera->consecutive_triggered_frames <
                        (std::numeric_limits<std::size_t>::max)())
                    {
                        ++camera->consecutive_triggered_frames;
                    }
                    if (camera->consecutive_triggered_frames >=
                        impl_->config.confirmation_consecutive_frames)
                    {
                        impl_->transition_decision(*camera, CandidateEventState::confirmed,
                                                   result.monotonic_time, result.wall_clock_time,
                                                   notifications);
                    }
                }
                else
                {
                    camera->consecutive_triggered_frames = 0U;
                }
            }
            else if (!result.triggered)
            {
                camera->state = CandidateEventState::idle;
                camera->consecutive_triggered_frames = 0U;
                camera->first_suspicious_trigger.reset();
            }
            else
            {
                if (camera->state == CandidateEventState::idle)
                {
                    camera->state = CandidateEventState::suspicious;
                    camera->consecutive_triggered_frames = 1U;
                    camera->first_suspicious_trigger = result;
                }
                else if (camera->consecutive_triggered_frames <
                         (std::numeric_limits<std::size_t>::max)())
                {
                    ++camera->consecutive_triggered_frames;
                }

                if (camera->consecutive_triggered_frames >=
                    impl_->config.candidate_consecutive_frames)
                {
                    auto created = impl_->create_candidate(*camera, result, notifications);
                    if (!created)
                    {
                        ++impl_->rejected_results;
                        response.emplace(
                            Result<CandidateProcessOutcome>::failure(std::move(created.error())));
                    }
                    else if (camera->consecutive_triggered_frames >=
                             impl_->config.confirmation_consecutive_frames)
                    {
                        impl_->transition_decision(*camera, CandidateEventState::confirmed,
                                                   result.monotonic_time, result.wall_clock_time,
                                                   notifications);
                    }
                }
            }

            if (!response)
            {
                response.emplace(Result<CandidateProcessOutcome>::success(
                    {.camera = Impl::camera_snapshot(*camera), .duplicate = false}));
            }
        }
    }
    impl_->deliver(notifications);
    return std::move(*response);
}

Result<CandidateCommandOutcome> CandidateEventManager::confirm(
    const std::string_view event_id, const std::uint64_t expected_version,
    const camera::MonotonicTime monotonic_time, const camera::WallClockTime wall_clock_time)
{
    return impl_->decision_command(event_id, expected_version, CandidateEventState::confirmed,
                                   monotonic_time, wall_clock_time);
}

Result<CandidateCommandOutcome> CandidateEventManager::reject(
    const std::string_view event_id, const std::uint64_t expected_version,
    const camera::MonotonicTime monotonic_time, const camera::WallClockTime wall_clock_time)
{
    return impl_->decision_command(event_id, expected_version, CandidateEventState::rejected,
                                   monotonic_time, wall_clock_time);
}

std::vector<CandidateEventSnapshot> CandidateEventManager::advance_time(
    const camera::MonotonicTime monotonic_time, const camera::WallClockTime wall_clock_time)
{
    std::vector<CandidateEventSnapshot> transitioned;
    std::vector<CandidateEventNotification> notifications;
    {
        std::scoped_lock lock{impl_->mutex};
        transitioned.reserve(impl_->cameras.size());
        notifications.reserve(impl_->cameras.size());
        if (!impl_->stopped)
        {
            for (auto& camera : impl_->cameras)
            {
                if (camera.state != CandidateEventState::candidate || !camera.event ||
                    monotonic_time < camera.event->snapshot.candidate_deadline)
                {
                    continue;
                }
                impl_->transition_decision(camera, CandidateEventState::timeout, monotonic_time,
                                           wall_clock_time, notifications);
                transitioned.push_back(camera.event->snapshot);
            }
        }
    }
    impl_->deliver(notifications);
    return transitioned;
}

std::vector<CandidateEventSnapshot> CandidateEventManager::stop(
    const camera::MonotonicTime monotonic_time, const camera::WallClockTime wall_clock_time)
{
    std::vector<CandidateEventSnapshot> transitioned;
    std::vector<CandidateEventNotification> notifications;
    {
        std::scoped_lock lock{impl_->mutex};
        transitioned.reserve(impl_->cameras.size());
        notifications.reserve(impl_->cameras.size());
        if (!impl_->stopped)
        {
            impl_->stopped = true;
            for (auto& camera : impl_->cameras)
            {
                if (camera.state == CandidateEventState::candidate && camera.event)
                {
                    const auto decision_time =
                        (std::max)(monotonic_time,
                                   camera.event->snapshot.candidate_trigger.monotonic_time);
                    impl_->transition_decision(camera, CandidateEventState::timeout, decision_time,
                                               wall_clock_time, notifications);
                    transitioned.push_back(camera.event->snapshot);
                    camera.event->pre_buffer_lease.reset();
                }
                else if (camera.state == CandidateEventState::suspicious)
                {
                    camera.state = CandidateEventState::idle;
                    camera.consecutive_triggered_frames = 0U;
                    camera.first_suspicious_trigger.reset();
                }
            }
        }
    }
    impl_->deliver(notifications);
    return transitioned;
}

CandidateEventManagerSnapshot CandidateEventManager::snapshot() const
{
    std::scoped_lock lock{impl_->mutex};
    CandidateEventManagerSnapshot result{
        .stopped = impl_->stopped,
        .accepted_results = impl_->accepted_results,
        .duplicate_results = impl_->duplicate_results,
        .rejected_results = impl_->rejected_results,
        .events_created = impl_->events_created,
        .confirmed_events = impl_->confirmed_events,
        .rejected_events = impl_->rejected_events,
        .timed_out_events = impl_->timed_out_events,
        .callback_failures = impl_->callback_failures,
    };
    result.cameras.reserve(impl_->cameras.size());
    for (const auto& camera : impl_->cameras)
        result.cameras.push_back(Impl::camera_snapshot(camera));
    return result;
}

} // namespace paperbreak::event
