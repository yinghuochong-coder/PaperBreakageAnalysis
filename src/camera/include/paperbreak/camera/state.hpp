#pragma once

#include "paperbreak/common/result.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace paperbreak::camera
{

enum class CameraState
{
    disabled,
    disconnected,
    connecting,
    connected,
    starting,
    streaming,
    recovering,
    faulted,
};

[[nodiscard]] std::string_view camera_state_name(CameraState state) noexcept;
[[nodiscard]] bool is_camera_transition_allowed(CameraState from, CameraState to) noexcept;

struct CameraTransitionRecord final
{
    std::uint64_t sequence{};
    std::string camera_id;
    CameraState from{CameraState::disabled};
    CameraState to{CameraState::disabled};
    std::string reason;
    bool accepted{};
    std::optional<Error> cause;
    std::string timestamp;
};

using CameraTransitionObserver = std::function<void(const CameraTransitionRecord&)>;

inline constexpr std::size_t default_maximum_recovery_attempts = 6U;
inline constexpr std::size_t maximum_recovery_attempts_limit = 1024U;
inline constexpr std::array<std::chrono::milliseconds, 6U> default_reconnect_delays = {
    std::chrono::seconds{1},  std::chrono::seconds{2},  std::chrono::seconds{5},
    std::chrono::seconds{10}, std::chrono::seconds{30}, std::chrono::seconds{60}};

struct ReconnectPolicy final
{
    std::size_t maximum_attempts{default_maximum_recovery_attempts};
    bool operator==(const ReconnectPolicy&) const = default;
};

enum class ReconnectWaitResult
{
    ready,
    cancelled,
    not_scheduled,
};

/// Returns true when the delay elapsed and false when the supplied token cancelled the wait.
using ReconnectWaiter = std::function<bool(std::stop_token, std::chrono::milliseconds)>;

[[nodiscard]] Result<void> validate_reconnect_policy(const ReconnectPolicy& policy);

class ReconnectController final
{
  public:
    explicit ReconnectController(ReconnectPolicy policy = {}, ReconnectWaiter waiter = {});

    ReconnectController(const ReconnectController&) = delete;
    ReconnectController& operator=(const ReconnectController&) = delete;
    ReconnectController(ReconnectController&&) = delete;
    ReconnectController& operator=(ReconnectController&&) = delete;

    /// Arms the next retry and returns its delay, or nullopt after the attempt limit.
    [[nodiscard]] std::optional<std::chrono::milliseconds> schedule_next();
    [[nodiscard]] ReconnectWaitResult wait_for_scheduled(
        std::stop_token external_stop = {}) noexcept;
    void cancel() noexcept;
    void reset() noexcept;

    [[nodiscard]] std::size_t attempt_count() const noexcept;
    [[nodiscard]] std::size_t maximum_attempts() const noexcept;
    [[nodiscard]] bool exhausted() const noexcept;
    [[nodiscard]] std::optional<std::chrono::milliseconds> next_delay() const noexcept;

  private:
    ReconnectPolicy policy_;
    ReconnectWaiter waiter_;
    mutable std::mutex mutex_;
    std::size_t attempt_count_{};
    std::optional<std::chrono::milliseconds> next_delay_;
    std::stop_source wait_stop_source_;
    std::uint64_t wait_generation_{};
};

struct CameraStateSnapshot final
{
    CameraState state{CameraState::disabled};
    std::size_t recovery_attempt{};
    std::optional<std::chrono::milliseconds> next_retry_delay;
    std::optional<Error> last_error;
    bool stop_requested{};
    std::uint64_t transition_sequence{};
};

/// Per-camera state owner. Device operations remain outside this controller.
class CameraSessionController final
{
  public:
    explicit CameraSessionController(std::string camera_id, bool enabled = false,
                                     ReconnectPolicy policy = {},
                                     CameraTransitionObserver observer = {},
                                     ReconnectWaiter waiter = {});

    CameraSessionController(const CameraSessionController&) = delete;
    CameraSessionController& operator=(const CameraSessionController&) = delete;
    CameraSessionController(CameraSessionController&&) = delete;
    CameraSessionController& operator=(CameraSessionController&&) = delete;

    [[nodiscard]] Result<void> transition_to(CameraState target, std::string reason,
                                             std::optional<Error> cause = std::nullopt);
    [[nodiscard]] Result<void> handle_failure(Error error, std::string reason);
    [[nodiscard]] ReconnectWaitResult wait_for_retry(std::stop_token external_stop = {}) noexcept;
    [[nodiscard]] Result<void> request_stop(bool remain_enabled);
    [[nodiscard]] Result<void> reset_fault(std::string reason);
    [[nodiscard]] CameraStateSnapshot snapshot() const;

  private:
    struct TransitionOutcome final
    {
        Result<void> result;
        CameraTransitionRecord record;
    };

    [[nodiscard]] TransitionOutcome apply_transition_locked(CameraState target, std::string reason,
                                                            std::optional<Error> cause);
    void notify(const CameraTransitionRecord& record) const noexcept;

    std::string camera_id_;
    mutable std::mutex mutex_;
    CameraState state_{CameraState::disabled};
    std::optional<Error> last_error_;
    bool stop_requested_{};
    std::uint64_t transition_sequence_{};
    CameraTransitionObserver observer_;
    ReconnectController reconnect_;
};

} // namespace paperbreak::camera
