#include "paperbreak/camera/state.hpp"

#include "paperbreak/camera/camera.hpp"

#include <algorithm>
#include <condition_variable>
#include <stdexcept>
#include <utility>

namespace paperbreak::camera
{
namespace
{
bool default_waiter(const std::stop_token token, const std::chrono::milliseconds delay)
{
    std::mutex mutex;
    std::condition_variable_any condition;
    std::unique_lock lock{mutex};
    static_cast<void>(condition.wait_for(lock, token, delay, [] { return false; }));
    return !token.stop_requested();
}

Error invalid_transition_error(const std::string_view camera_id, const CameraState from,
                               const CameraState to, const std::string_view reason)
{
    return make_camera_error(CameraErrorKind::invalid_state_transition, "相机状态转换不合法",
                             "camera.transition", std::string{camera_id},
                             {{"from", std::string{camera_state_name(from)}},
                              {"to", std::string{camera_state_name(to)}},
                              {"reason", std::string{reason}.substr(0U, 256U)}});
}
} // namespace

std::string_view camera_state_name(const CameraState state) noexcept
{
    switch (state)
    {
    case CameraState::disabled:
        return "Disabled";
    case CameraState::disconnected:
        return "Disconnected";
    case CameraState::connecting:
        return "Connecting";
    case CameraState::connected:
        return "Connected";
    case CameraState::starting:
        return "Starting";
    case CameraState::streaming:
        return "Streaming";
    case CameraState::recovering:
        return "Recovering";
    case CameraState::faulted:
        return "Faulted";
    }
    return "Unknown";
}

bool is_camera_transition_allowed(const CameraState from, const CameraState to) noexcept
{
    if (from == to)
    {
        return false;
    }
    switch (from)
    {
    case CameraState::disabled:
        return to == CameraState::disconnected;
    case CameraState::disconnected:
        return to == CameraState::disabled || to == CameraState::connecting;
    case CameraState::connecting:
        return to == CameraState::disabled || to == CameraState::disconnected ||
               to == CameraState::connected || to == CameraState::recovering ||
               to == CameraState::faulted;
    case CameraState::connected:
        return to == CameraState::disabled || to == CameraState::disconnected ||
               to == CameraState::starting || to == CameraState::recovering ||
               to == CameraState::faulted;
    case CameraState::starting:
        return to == CameraState::disabled || to == CameraState::disconnected ||
               to == CameraState::streaming || to == CameraState::recovering ||
               to == CameraState::faulted;
    case CameraState::streaming:
        return to == CameraState::disabled || to == CameraState::disconnected ||
               to == CameraState::recovering || to == CameraState::faulted;
    case CameraState::recovering:
        return to == CameraState::disabled || to == CameraState::disconnected ||
               to == CameraState::connecting || to == CameraState::faulted;
    case CameraState::faulted:
        return to == CameraState::disabled || to == CameraState::disconnected;
    }
    return false;
}

Result<void> validate_reconnect_policy(const ReconnectPolicy& policy)
{
    if (policy.maximum_attempts == 0U || policy.maximum_attempts > maximum_recovery_attempts_limit)
    {
        return Result<void>::failure(
            make_camera_error(CameraErrorKind::config_failed, "相机最大恢复次数超出允许范围",
                              "camera.validateReconnectPolicy", std::nullopt,
                              {{"parameter", "maximumAttempts"},
                               {"reason", "out-of-range"},
                               {"minimum", "1"},
                               {"maximum", std::to_string(maximum_recovery_attempts_limit)}}));
    }
    return Result<void>::success();
}

ReconnectController::ReconnectController(ReconnectPolicy policy, ReconnectWaiter waiter)
    : policy_(policy), waiter_(std::move(waiter))
{
    if (!validate_reconnect_policy(policy_))
    {
        throw std::invalid_argument{"invalid reconnect policy"};
    }
    if (!waiter_)
    {
        waiter_ = default_waiter;
    }
}

std::optional<std::chrono::milliseconds> ReconnectController::schedule_next()
{
    std::lock_guard lock{mutex_};
    if (next_delay_)
    {
        return next_delay_;
    }
    if (attempt_count_ >= policy_.maximum_attempts)
    {
        next_delay_.reset();
        return std::nullopt;
    }
    ++attempt_count_;
    const std::size_t delay_index =
        std::min(attempt_count_ - 1U, default_reconnect_delays.size() - 1U);
    next_delay_ = default_reconnect_delays[delay_index];
    wait_stop_source_.request_stop();
    wait_stop_source_ = std::stop_source{};
    ++wait_generation_;
    return next_delay_;
}

ReconnectWaitResult ReconnectController::wait_for_scheduled(
    const std::stop_token external_stop) noexcept
{
    std::optional<std::chrono::milliseconds> delay;
    std::stop_source source;
    std::uint64_t generation{};
    {
        std::lock_guard lock{mutex_};
        if (!next_delay_)
        {
            return ReconnectWaitResult::not_scheduled;
        }
        delay = next_delay_;
        source = wait_stop_source_;
        generation = wait_generation_;
    }

    std::stop_callback external_callback{
        external_stop, [source]() mutable { static_cast<void>(source.request_stop()); }};
    bool elapsed{};
    try
    {
        elapsed = waiter_(source.get_token(), *delay);
    }
    catch (...)
    {
        elapsed = false;
    }

    std::lock_guard lock{mutex_};
    if (generation != wait_generation_ || !next_delay_ || source.stop_requested() || !elapsed)
    {
        return ReconnectWaitResult::cancelled;
    }
    next_delay_.reset();
    return ReconnectWaitResult::ready;
}

void ReconnectController::cancel() noexcept
{
    std::lock_guard lock{mutex_};
    static_cast<void>(wait_stop_source_.request_stop());
    next_delay_.reset();
    ++wait_generation_;
}

void ReconnectController::reset() noexcept
{
    std::lock_guard lock{mutex_};
    static_cast<void>(wait_stop_source_.request_stop());
    wait_stop_source_ = std::stop_source{};
    attempt_count_ = 0U;
    next_delay_.reset();
    ++wait_generation_;
}

std::size_t ReconnectController::attempt_count() const noexcept
{
    std::lock_guard lock{mutex_};
    return attempt_count_;
}

std::size_t ReconnectController::maximum_attempts() const noexcept
{
    return policy_.maximum_attempts;
}

bool ReconnectController::exhausted() const noexcept
{
    std::lock_guard lock{mutex_};
    return attempt_count_ >= policy_.maximum_attempts;
}

std::optional<std::chrono::milliseconds> ReconnectController::next_delay() const noexcept
{
    std::lock_guard lock{mutex_};
    return next_delay_;
}

CameraSessionController::CameraSessionController(std::string camera_id, const bool enabled,
                                                 ReconnectPolicy policy,
                                                 CameraTransitionObserver observer,
                                                 ReconnectWaiter waiter)
    : camera_id_(std::move(camera_id)),
      state_(enabled ? CameraState::disconnected : CameraState::disabled),
      observer_(std::move(observer)), reconnect_(policy, std::move(waiter))
{
    if (camera_id_.empty())
    {
        throw std::invalid_argument{"camera id must not be empty"};
    }
}

Result<void> CameraSessionController::transition_to(CameraState target, std::string reason,
                                                    std::optional<Error> cause)
{
    std::optional<TransitionOutcome> outcome;
    {
        std::lock_guard lock{mutex_};
        outcome.emplace(apply_transition_locked(target, std::move(reason), std::move(cause)));
    }
    notify(outcome->record);
    return std::move(outcome->result);
}

Result<void> CameraSessionController::handle_failure(Error error, std::string reason)
{
    std::optional<TransitionOutcome> outcome;
    {
        std::lock_guard lock{mutex_};
        CameraState target = CameraState::faulted;
        if (error.retryable && !stop_requested_)
        {
            if (!is_camera_transition_allowed(state_, CameraState::recovering))
            {
                outcome.emplace(apply_transition_locked(CameraState::recovering, std::move(reason),
                                                        std::move(error)));
            }
            else if (const auto delay = reconnect_.schedule_next(); delay)
            {
                target = CameraState::recovering;
            }
            else
            {
                error.retryable = false;
                error.details.push_back(
                    {"recoveryAttempts", std::to_string(reconnect_.attempt_count())});
                error.details.push_back({"reason", "recovery-attempts-exhausted"});
            }
        }
        if (!outcome)
        {
            outcome.emplace(apply_transition_locked(target, std::move(reason), std::move(error)));
        }
        if (!outcome->result && target == CameraState::recovering)
        {
            reconnect_.cancel();
        }
    }
    notify(outcome->record);
    return std::move(outcome->result);
}

ReconnectWaitResult CameraSessionController::wait_for_retry(
    const std::stop_token external_stop) noexcept
{
    const ReconnectWaitResult result = reconnect_.wait_for_scheduled(external_stop);
    if (result != ReconnectWaitResult::ready)
    {
        return result;
    }

    std::optional<TransitionOutcome> outcome;
    {
        std::lock_guard lock{mutex_};
        if (stop_requested_ || state_ != CameraState::recovering)
        {
            return ReconnectWaitResult::cancelled;
        }
        outcome.emplace(apply_transition_locked(CameraState::connecting, "reconnect-delay-elapsed",
                                                std::nullopt));
    }
    notify(outcome->record);
    return outcome->result ? ReconnectWaitResult::ready : ReconnectWaitResult::cancelled;
}

Result<void> CameraSessionController::request_stop(const bool remain_enabled)
{
    std::optional<TransitionOutcome> outcome;
    {
        std::lock_guard lock{mutex_};
        stop_requested_ = true;
        reconnect_.cancel();
        const CameraState target =
            remain_enabled ? CameraState::disconnected : CameraState::disabled;
        if (state_ == target)
        {
            return Result<void>::success();
        }
        outcome.emplace(apply_transition_locked(target, "stop-requested", std::nullopt));
    }
    notify(outcome->record);
    return std::move(outcome->result);
}

Result<void> CameraSessionController::reset_fault(std::string reason)
{
    std::optional<TransitionOutcome> outcome;
    {
        std::lock_guard lock{mutex_};
        if (state_ != CameraState::faulted)
        {
            outcome.emplace(apply_transition_locked(state_, std::move(reason), std::nullopt));
        }
        else
        {
            stop_requested_ = false;
            outcome.emplace(apply_transition_locked(CameraState::disconnected, std::move(reason),
                                                    std::nullopt));
        }
    }
    notify(outcome->record);
    return std::move(outcome->result);
}

CameraStateSnapshot CameraSessionController::snapshot() const
{
    std::lock_guard lock{mutex_};
    return {.state = state_,
            .recovery_attempt = reconnect_.attempt_count(),
            .next_retry_delay = reconnect_.next_delay(),
            .last_error = last_error_,
            .stop_requested = stop_requested_,
            .transition_sequence = transition_sequence_};
}

CameraSessionController::TransitionOutcome CameraSessionController::apply_transition_locked(
    const CameraState target, std::string reason, std::optional<Error> cause)
{
    const CameraState previous = state_;
    const bool accepted = is_camera_transition_allowed(previous, target);
    ++transition_sequence_;
    if (!accepted)
    {
        Error error = invalid_transition_error(camera_id_, previous, target, reason);
        return {.result = Result<void>::failure(error),
                .record = {.sequence = transition_sequence_,
                           .camera_id = camera_id_,
                           .from = previous,
                           .to = target,
                           .reason = std::move(reason),
                           .accepted = false,
                           .cause = std::move(error),
                           .timestamp = current_utc_timestamp()}};
    }

    state_ = target;
    if (cause)
    {
        last_error_ = cause;
    }
    if (target == CameraState::streaming)
    {
        reconnect_.reset();
        stop_requested_ = false;
        last_error_.reset();
    }
    else if (target == CameraState::disconnected &&
             (previous == CameraState::faulted || previous == CameraState::disabled))
    {
        reconnect_.reset();
        last_error_.reset();
        if (previous == CameraState::disabled)
        {
            stop_requested_ = false;
        }
    }
    else if (target == CameraState::disabled || target == CameraState::disconnected)
    {
        reconnect_.cancel();
    }
    else if (target == CameraState::connecting)
    {
        stop_requested_ = false;
    }

    return {.result = Result<void>::success(),
            .record = {.sequence = transition_sequence_,
                       .camera_id = camera_id_,
                       .from = previous,
                       .to = target,
                       .reason = std::move(reason),
                       .accepted = true,
                       .cause = std::move(cause),
                       .timestamp = current_utc_timestamp()}};
}

void CameraSessionController::notify(const CameraTransitionRecord& record) const noexcept
{
    if (!observer_)
    {
        return;
    }
    try
    {
        observer_(record);
    }
    catch (...)
    {
    }
}

} // namespace paperbreak::camera
