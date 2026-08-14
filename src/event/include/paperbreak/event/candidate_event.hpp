#pragma once

#include "paperbreak/algorithm/trigger.hpp"
#include "paperbreak/camera/frame.hpp"
#include "paperbreak/common/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace paperbreak::event
{

class MemoryRing;

enum class CandidateEventState
{
    idle,
    suspicious,
    candidate,
    confirmed,
    rejected,
    timeout,
};

[[nodiscard]] std::string_view to_string(CandidateEventState state) noexcept;

struct CandidateDecision final
{
    CandidateEventState state{CandidateEventState::candidate};
    camera::MonotonicTime monotonic_time;
    camera::WallClockTime wall_clock_time;
};

struct CandidateEventSnapshot final
{
    std::string event_id;
    std::string camera_id;
    CandidateEventState decision_state{CandidateEventState::candidate};
    std::uint64_t version{};
    algorithm::TriggerResult first_suspicious_trigger;
    algorithm::TriggerResult candidate_trigger;
    camera::MonotonicTime candidate_deadline;
    std::optional<CandidateDecision> decision;
    bool pre_buffer_protection_acquired{};
    bool pre_buffer_complete{};
    std::size_t pre_buffer_frame_count{};
    std::uint64_t pre_buffer_sequence_gaps{};
    std::string pre_buffer_error_code;
    bool post_collection_started{};
};

struct CandidateCameraSnapshot final
{
    std::string camera_id;
    CandidateEventState observation_state{CandidateEventState::idle};
    std::size_t consecutive_triggered_frames{};
    std::size_t consecutive_confirmation_frames{};
    std::optional<camera::MonotonicTime> confirmation_started_at;
    std::optional<camera::MonotonicTime> last_confirmation_qualified_at;
    bool external_signal_active{};
    bool cooling_down{};
    std::optional<camera::MonotonicTime> cooldown_until;
    bool rearm_pending{};
    std::optional<camera::MonotonicTime> recovery_started_at;
    std::uint64_t rearm_suppressed_results{};
    std::optional<CandidateEventSnapshot> event;
};

struct CandidateRearmSeed final
{
    std::string camera_id;
    bool rearm_pending{};
    std::optional<camera::MonotonicTime> cooldown_until;
    std::uint64_t rearm_suppressed_results{};
};

enum class ExternalConfirmationPolicy
{
    not_used,
    required_active,
};

enum class CandidateNotificationKind
{
    candidate_created,
    decision_changed,
};

struct CandidateEventNotification final
{
    CandidateNotificationKind kind{CandidateNotificationKind::candidate_created};
    CandidateEventSnapshot event;
};

using CandidateEventNotificationCallback =
    std::function<void(const CandidateEventNotification& notification)>;

struct CandidateCameraBinding final
{
    std::string camera_id;
    /// Non-owning; the ring must outlive the manager and remain bound to this camera.
    MemoryRing* memory_ring{};
};

struct CandidateEventManagerConfig final
{
    std::vector<CandidateCameraBinding> cameras;
    std::size_t candidate_consecutive_frames{2U};
    double candidate_confidence_threshold{};
    double confirmation_confidence_threshold{};
    std::chrono::milliseconds confirmation_duration{120};
    std::chrono::nanoseconds processing_period{66'666'667};
    ExternalConfirmationPolicy external_confirmation{ExternalConfirmationPolicy::not_used};
    std::chrono::milliseconds candidate_timeout{5000};
    std::chrono::milliseconds pre_event_duration{1000};
    std::chrono::milliseconds cooldown_duration{};
    std::chrono::milliseconds rearm_duration{500};
    CandidateEventNotificationCallback notification_callback;
};

struct CandidateProcessOutcome final
{
    CandidateCameraSnapshot camera;
    bool duplicate{};
    /// Ordered, bounded notifications produced by this process call. At most one candidate can be
    /// active per camera, so a single result produces no more than the manager's fixed three-event
    /// notification budget.
    std::vector<CandidateEventNotification> notifications;
};

struct CandidateCommandOutcome final
{
    CandidateEventSnapshot event;
    bool duplicate{};
};

struct CandidateEventManagerSnapshot final
{
    bool stopped{};
    std::uint64_t accepted_results{};
    std::uint64_t duplicate_results{};
    std::uint64_t rejected_results{};
    std::uint64_t events_created{};
    std::uint64_t confirmed_events{};
    std::uint64_t rejected_events{};
    std::uint64_t timed_out_events{};
    std::uint64_t callback_failures{};
    std::vector<CandidateCameraSnapshot> cameras;
};

/// Bounded, thread-safe candidate state manager for at most six logical cameras.
class CandidateEventManager final
{
  public:
    class ConstructionKey final
    {
      public:
        ConstructionKey(const ConstructionKey&) = default;

      private:
        friend class CandidateEventManager;
        ConstructionKey() = default;
    };

    [[nodiscard]] static Result<std::unique_ptr<CandidateEventManager>> create(
        CandidateEventManagerConfig config);

    CandidateEventManager(ConstructionKey, CandidateEventManagerConfig config);
    ~CandidateEventManager();
    CandidateEventManager(const CandidateEventManager&) = delete;
    CandidateEventManager& operator=(const CandidateEventManager&) = delete;
    CandidateEventManager(CandidateEventManager&&) = delete;
    CandidateEventManager& operator=(CandidateEventManager&&) = delete;

    [[nodiscard]] Result<CandidateProcessOutcome> process(const algorithm::TriggerResult& result);
    [[nodiscard]] Result<CandidateCommandOutcome> confirm(std::string_view event_id,
                                                          std::uint64_t expected_version,
                                                          camera::MonotonicTime monotonic_time,
                                                          camera::WallClockTime wall_clock_time);
    [[nodiscard]] Result<CandidateCommandOutcome> reject(std::string_view event_id,
                                                         std::uint64_t expected_version,
                                                         camera::MonotonicTime monotonic_time,
                                                         camera::WallClockTime wall_clock_time);
    [[nodiscard]] Result<CandidateCameraSnapshot> update_external_signal(
        std::string_view camera_id, bool active, camera::MonotonicTime monotonic_time,
        camera::WallClockTime wall_clock_time);

    /// Applies exact-deadline timeout transitions using only the monotonic time argument.
    [[nodiscard]] std::vector<CandidateEventSnapshot> advance_time(
        camera::MonotonicTime monotonic_time, camera::WallClockTime wall_clock_time);

    /// Stops new observations and deterministically times out all active candidates.
    [[nodiscard]] std::vector<CandidateEventSnapshot> stop(camera::MonotonicTime monotonic_time,
                                                           camera::WallClockTime wall_clock_time);

    [[nodiscard]] CandidateEventManagerSnapshot snapshot() const;
    [[nodiscard]] std::vector<CandidateRearmSeed> rearm_seeds(
        camera::MonotonicTime monotonic_time) const;
    [[nodiscard]] Result<void> apply_rearm_seeds(const std::vector<CandidateRearmSeed>& seeds);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace paperbreak::event
