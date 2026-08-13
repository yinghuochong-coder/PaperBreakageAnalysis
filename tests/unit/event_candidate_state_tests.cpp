#include "paperbreak/camera/frame_pool.hpp"
#include "paperbreak/event/candidate_event.hpp"
#include "paperbreak/event/memory_ring.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
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
        throw std::runtime_error{"candidate test frame pool exhausted"};
    auto bytes = acquired.buffer->writable_bytes();
    for (std::size_t index = 0U; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::byte>((sequence + index) & 0xffU);
    if (!acquired.buffer->set_size(bytes.size()))
        throw std::runtime_error{"candidate test frame size rejected"};
    const FramePacket packet{
        .camera_id = camera_id,
        .camera_frame_number = sequence + 100U,
        .sequence_number = sequence,
        .received_monotonic_time = MonotonicTime{time},
        .received_wall_clock_time = WallClockTime{time},
        .geometry = {.width = 2U, .height = 2U, .stride = 2U},
        .pixel_format = PixelFormat::mono8,
        .buffer = std::move(acquired.buffer),
    };
    auto view = make_frame_view(packet);
    if (!view)
        throw std::runtime_error{"candidate test frame invalid"};
    return std::move(view).value();
}

TriggerResult trigger_result(const std::string& camera_id, const std::uint64_t sequence,
                             const std::chrono::milliseconds time, const bool triggered,
                             const double confidence = 0.0,
                             const TriggerSource source = TriggerSource::fixed_period)
{
    return {.triggered = triggered,
            .trigger_source = triggered ? source : TriggerSource::none,
            .camera_id = camera_id,
            .sequence_number = sequence,
            .camera_frame_number = sequence + 100U,
            .monotonic_time = MonotonicTime{time},
            .wall_clock_time = WallClockTime{time},
            .mean_grayscale = 0.5,
            .mean_grayscale_change = triggered ? 0.5 : 0.0,
            .paper_ratio = triggered ? 0.0 : 1.0,
            .reason = triggered ? "test-trigger" : "",
            .anomalous = triggered,
            .candidate_type =
                triggered ? DetectionCandidateType::indeterminate : DetectionCandidateType::none,
            .confidence = confidence};
}

class CandidateHarness final
{
  public:
    CandidateHarness(
        std::vector<std::string> camera_ids, const std::size_t candidate_frames,
        const std::size_t confirmation_frames, CandidateEventNotificationCallback callback = {},
        const std::chrono::milliseconds timeout = 1000ms,
        const std::chrono::milliseconds pre_event = 200ms, const double candidate_threshold = 0.0,
        const double confirmation_threshold = 0.0,
        const ExternalConfirmationPolicy external = ExternalConfirmationPolicy::not_used,
        const std::chrono::milliseconds cooldown = 0ms,
        const std::chrono::milliseconds confirmation_duration = -1ms,
        const std::chrono::nanoseconds processing_period = 100ms,
        const std::chrono::milliseconds rearm_duration = 500ms)
        : pool_(96U, 4U)
    {
        rings_.reserve(camera_ids.size());
        CandidateEventManagerConfig config{
            .candidate_consecutive_frames = candidate_frames,
            .candidate_confidence_threshold = candidate_threshold,
            .confirmation_confidence_threshold = confirmation_threshold,
            .confirmation_duration =
                confirmation_duration >= 0ms
                    ? confirmation_duration
                    : std::chrono::milliseconds{confirmation_frames > 1U
                                                    ? (confirmation_frames - 1U) * 100U
                                                    : 10U},
            .processing_period = processing_period,
            .external_confirmation = external,
            .candidate_timeout = timeout,
            .pre_event_duration = pre_event,
            .cooldown_duration = cooldown,
            .rearm_duration = rearm_duration,
            .notification_callback = std::move(callback),
        };
        config.cameras.reserve(camera_ids.size());
        for (auto& camera_id : camera_ids)
        {
            rings_.push_back(std::make_unique<MemoryRing>(MemoryRingOptions{
                .camera_id = camera_id,
                .capacity_frames = 12U,
                .required_history_seconds = 0.0,
                .maximum_active_leases = 2U,
                .maximum_leased_frame_references = 24U,
            }));
            config.cameras.push_back(
                {.camera_id = std::move(camera_id), .memory_ring = rings_.back().get()});
        }
        auto created = CandidateEventManager::create(std::move(config));
        if (!created)
            throw std::runtime_error{"candidate test manager creation failed"};
        manager_ = std::move(created).value();
    }

    [[nodiscard]] Result<CandidateProcessOutcome> submit(
        const std::string& camera_id, const std::uint64_t sequence,
        const std::chrono::milliseconds time, const bool triggered, const double confidence = 0.0,
        const TriggerSource source = TriggerSource::fixed_period)
    {
        auto frame = pooled_frame(pool_, camera_id, sequence, time);
        auto* ring = find_ring(camera_id);
        auto pushed = ring->push(std::move(frame));
        if (!pushed)
            throw std::runtime_error{"candidate test ring push failed"};
        return manager_->process(
            trigger_result(camera_id, sequence, time, triggered, confidence, source));
    }

    [[nodiscard]] MemoryRing& ring(const std::string& camera_id)
    {
        return *find_ring(camera_id);
    }

    [[nodiscard]] CandidateEventManager& manager() noexcept
    {
        return *manager_;
    }

  private:
    MemoryRing* find_ring(const std::string& camera_id)
    {
        for (const auto& ring : rings_)
        {
            if (ring->snapshot().camera_id == camera_id)
                return ring.get();
        }
        throw std::runtime_error{"candidate test ring not found"};
    }

    FrameBufferPool pool_;
    std::vector<std::unique_ptr<MemoryRing>> rings_;
    std::unique_ptr<CandidateEventManager> manager_;
};

} // namespace

TEST(EventCandidateState, UsesExactConsecutiveBoundariesAndConfirms)
{
    std::vector<CandidateEventNotification> notifications;
    CandidateHarness harness({"CAM01"}, 2U, 4U, [&](const auto& notification) {
        notifications.push_back(notification);
    });

    auto idle = harness.submit("CAM01", 1U, 100ms, false);
    ASSERT_TRUE(idle);
    EXPECT_EQ(idle.value().camera.observation_state, CandidateEventState::idle);

    auto suspicious = harness.submit("CAM01", 2U, 200ms, true);
    ASSERT_TRUE(suspicious);
    EXPECT_EQ(suspicious.value().camera.observation_state, CandidateEventState::suspicious);
    EXPECT_EQ(suspicious.value().camera.consecutive_triggered_frames, 1U);

    auto reset = harness.submit("CAM01", 3U, 300ms, false);
    ASSERT_TRUE(reset);
    EXPECT_EQ(reset.value().camera.observation_state, CandidateEventState::idle);

    ASSERT_TRUE(harness.submit("CAM01", 4U, 400ms, true));
    auto candidate = harness.submit("CAM01", 5U, 500ms, true);
    ASSERT_TRUE(candidate);
    ASSERT_TRUE(candidate.value().camera.event);
    EXPECT_EQ(candidate.value().camera.observation_state, CandidateEventState::candidate);
    EXPECT_EQ(candidate.value().camera.event->first_suspicious_trigger.sequence_number, 4U);
    EXPECT_EQ(candidate.value().camera.event->candidate_trigger.sequence_number, 5U);
    EXPECT_EQ(candidate.value().camera.event->version, 1U);

    auto still_candidate = harness.submit("CAM01", 6U, 600ms, true);
    ASSERT_TRUE(still_candidate);
    EXPECT_EQ(still_candidate.value().camera.observation_state, CandidateEventState::candidate);
    auto confirmed = harness.submit("CAM01", 7U, 700ms, true);
    ASSERT_TRUE(confirmed);
    ASSERT_TRUE(confirmed.value().camera.event);
    EXPECT_EQ(confirmed.value().camera.observation_state, CandidateEventState::confirmed);
    EXPECT_EQ(confirmed.value().camera.event->decision_state, CandidateEventState::confirmed);
    EXPECT_EQ(confirmed.value().camera.event->version, 2U);
    ASSERT_TRUE(confirmed.value().camera.event->decision);
    EXPECT_EQ(confirmed.value().camera.event->decision->monotonic_time, MonotonicTime{700ms});
    EXPECT_EQ(to_string(CandidateEventState::suspicious), "Suspicious");

    ASSERT_EQ(notifications.size(), 2U);
    EXPECT_EQ(notifications[0].kind, CandidateNotificationKind::candidate_created);
    EXPECT_EQ(notifications[1].kind, CandidateNotificationKind::decision_changed);
}

TEST(EventCandidateState, CandidateImmediatelyOwnsIdentityAndProtectsPreBuffer)
{
    std::vector<CandidateEventNotification> notifications;
    CandidateHarness harness({"CAM01"}, 1U, 3U, [&](const auto& notification) {
        notifications.push_back(notification);
    });
    ASSERT_TRUE(harness.submit("CAM01", 1U, 0ms, false));
    ASSERT_TRUE(harness.submit("CAM01", 2U, 100ms, false));
    auto candidate = harness.submit("CAM01", 3U, 200ms, true);
    ASSERT_TRUE(candidate);
    ASSERT_TRUE(candidate.value().camera.event);
    const auto& event = *candidate.value().camera.event;

    const std::regex event_id_pattern{
        R"(^EVT-[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$)"};
    EXPECT_TRUE(std::regex_match(event.event_id, event_id_pattern));
    EXPECT_TRUE(event.pre_buffer_protection_acquired);
    EXPECT_TRUE(event.pre_buffer_complete);
    EXPECT_EQ(event.pre_buffer_frame_count, 3U);
    EXPECT_EQ(event.pre_buffer_sequence_gaps, 0U);
    EXPECT_TRUE(event.pre_buffer_error_code.empty());
    EXPECT_TRUE(event.post_collection_started);
    EXPECT_EQ(harness.ring("CAM01").snapshot().active_leases, 1U);
    ASSERT_EQ(notifications.size(), 1U);
    EXPECT_EQ(notifications.front().event.event_id, event.event_id);
}

TEST(EventCandidateState, AppliesSeparateConfidenceAndConsecutiveConfirmationBoundaries)
{
    CandidateHarness harness({"CAM01"}, 2U, 4U, {}, 2s, 200ms, 0.6, 0.8);

    auto suspicious = harness.submit("CAM01", 1U, 100ms, true, 0.7);
    ASSERT_TRUE(suspicious);
    EXPECT_EQ(suspicious.value().camera.observation_state, CandidateEventState::suspicious);
    EXPECT_EQ(suspicious.value().camera.consecutive_confirmation_frames, 0U);

    auto candidate = harness.submit("CAM01", 2U, 200ms, true, 0.8);
    ASSERT_TRUE(candidate);
    EXPECT_EQ(candidate.value().camera.observation_state, CandidateEventState::candidate);
    EXPECT_EQ(candidate.value().camera.consecutive_confirmation_frames, 1U);

    auto reset_confirmation = harness.submit("CAM01", 3U, 300ms, true, 0.79);
    ASSERT_TRUE(reset_confirmation);
    EXPECT_EQ(reset_confirmation.value().camera.observation_state, CandidateEventState::candidate);
    EXPECT_EQ(reset_confirmation.value().camera.consecutive_confirmation_frames, 0U);

    for (std::uint64_t sequence = 4U; sequence <= 6U; ++sequence)
    {
        auto pending = harness.submit("CAM01", sequence, std::chrono::milliseconds{sequence * 100U},
                                      true, 0.9);
        ASSERT_TRUE(pending);
        EXPECT_EQ(pending.value().camera.observation_state, CandidateEventState::candidate);
    }
    auto confirmed = harness.submit("CAM01", 7U, 700ms, true, 0.9);
    ASSERT_TRUE(confirmed);
    EXPECT_EQ(confirmed.value().camera.observation_state, CandidateEventState::confirmed);
}

TEST(EventCandidateState, RequiresExternalSignalAndHonorsExactCooldownBoundary)
{
    CandidateHarness harness({"CAM01"}, 1U, 1U, {}, 2s, 200ms, 0.5, 0.8,
                             ExternalConfirmationPolicy::required_active, 100ms);

    auto candidate = harness.submit("CAM01", 1U, 100ms, true, 0.9);
    ASSERT_TRUE(candidate);
    EXPECT_EQ(candidate.value().camera.observation_state, CandidateEventState::candidate);
    EXPECT_FALSE(candidate.value().camera.external_signal_active);

    auto stale_signal = harness.manager().update_external_signal("CAM01", true, MonotonicTime{99ms},
                                                                 WallClockTime{99ms});
    ASSERT_FALSE(stale_signal);
    EXPECT_EQ(stale_signal.error().business_code, "PIPELINE_FRAME_ORDER_VIOLATION");

    auto signaled = harness.manager().update_external_signal("CAM01", true, MonotonicTime{150ms},
                                                             WallClockTime{150ms});
    ASSERT_TRUE(signaled);
    EXPECT_EQ(signaled.value().observation_state, CandidateEventState::confirmed);
    ASSERT_TRUE(signaled.value().cooldown_until);
    EXPECT_EQ(*signaled.value().cooldown_until, MonotonicTime{250ms});

    auto cooling = harness.submit("CAM01", 2U, 249ms, true, 0.9);
    ASSERT_TRUE(cooling);
    EXPECT_EQ(cooling.value().camera.observation_state, CandidateEventState::idle);
    EXPECT_TRUE(cooling.value().camera.cooling_down);
    EXPECT_TRUE(cooling.value().camera.rearm_pending);
    EXPECT_EQ(cooling.value().camera.rearm_suppressed_results, 1U);
    EXPECT_FALSE(cooling.value().camera.event.has_value());

    auto boundary = harness.submit("CAM01", 3U, 250ms, true, 0.9);
    ASSERT_TRUE(boundary);
    EXPECT_EQ(boundary.value().camera.observation_state, CandidateEventState::idle);
    EXPECT_TRUE(boundary.value().camera.rearm_pending);
    EXPECT_EQ(boundary.value().camera.rearm_suppressed_results, 2U);

    ASSERT_TRUE(harness.submit("CAM01", 4U, 251ms, false));
    ASSERT_TRUE(harness.submit("CAM01", 5U, 351ms, false));
    ASSERT_TRUE(harness.submit("CAM01", 6U, 451ms, false));
    ASSERT_TRUE(harness.submit("CAM01", 7U, 551ms, false));
    ASSERT_TRUE(harness.submit("CAM01", 8U, 651ms, false));
    auto before_rearm = harness.submit("CAM01", 9U, 750ms, false);
    ASSERT_TRUE(before_rearm);
    EXPECT_TRUE(before_rearm.value().camera.rearm_pending);
    auto rearmed = harness.submit("CAM01", 10U, 751ms, false);
    ASSERT_TRUE(rearmed);
    EXPECT_FALSE(rearmed.value().camera.rearm_pending);
    auto candidate_again = harness.submit("CAM01", 11U, 760ms, true, 0.9);
    ASSERT_TRUE(candidate_again);
    EXPECT_EQ(candidate_again.value().camera.observation_state, CandidateEventState::candidate);
    auto confirmed_again = harness.submit("CAM01", 12U, 770ms, true, 0.9);
    ASSERT_TRUE(confirmed_again);
    EXPECT_EQ(confirmed_again.value().camera.observation_state, CandidateEventState::confirmed);

    auto unknown = harness.manager().update_external_signal("CAM04", true, MonotonicTime{300ms},
                                                            WallClockTime{300ms});
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().business_code, "SYS_CONFIG_INVALID");
}

TEST(EventCandidateState, ConfirmsByOneHundredTwentyMillisecondsAtAllConfiguredRates)
{
    for (const auto fps : {15U, 30U, 60U})
    {
        const auto period = std::chrono::nanoseconds{1'000'000'000LL / fps};
        CandidateHarness harness({"CAM01"}, 1U, 1U, {}, 2s, 0ms, 0.5, 0.8,
                                 ExternalConfirmationPolicy::not_used, 0ms, 120ms, period);
        ASSERT_TRUE(harness.submit("CAM01", 1U, 0ms, true, 0.9));
        const auto step = std::chrono::milliseconds{(1000U + fps - 1U) / fps};
        std::uint64_t sequence = 2U;
        auto time = step;
        Result<CandidateProcessOutcome> confirmed =
            harness.submit("CAM01", sequence++, time, true, 0.9);
        while (time < 120ms)
        {
            ASSERT_TRUE(confirmed);
            EXPECT_EQ(confirmed.value().camera.observation_state, CandidateEventState::candidate);
            time += step;
            confirmed = harness.submit("CAM01", sequence++, time, true, 0.9);
        }
        ASSERT_TRUE(confirmed);
        EXPECT_EQ(confirmed.value().camera.observation_state, CandidateEventState::confirmed);
        ASSERT_TRUE(confirmed.value().camera.event->decision);
        EXPECT_GE(confirmed.value().camera.event->decision->monotonic_time, MonotonicTime{120ms});
        EXPECT_LE(confirmed.value().camera.event->decision->monotonic_time,
                  MonotonicTime{120ms} + period);
    }
}

TEST(EventCandidateState, ContinuousAutomaticAnomalyCreatesOnlyOneCandidateAcrossCooldowns)
{
    CandidateHarness harness({"CAM01"}, 1U, 1U, {}, 10s, 0ms, 0.5, 0.8,
                             ExternalConfirmationPolicy::not_used, 100ms, 10ms, 100ms, 500ms);
    ASSERT_TRUE(harness.submit("CAM01", 1U, 0ms, true, 0.9));
    auto confirmed = harness.submit("CAM01", 2U, 10ms, true, 0.9);
    ASSERT_TRUE(confirmed);
    EXPECT_EQ(confirmed.value().camera.observation_state, CandidateEventState::confirmed);

    for (std::uint64_t sequence = 3U; sequence <= 40U; ++sequence)
    {
        auto suppressed = harness.submit("CAM01", sequence,
                                         std::chrono::milliseconds{sequence * 100U}, true, 0.1);
        ASSERT_TRUE(suppressed);
        EXPECT_TRUE(suppressed.value().camera.rearm_pending);
        EXPECT_FALSE(suppressed.value().camera.event.has_value());
    }
    const auto snapshot = harness.manager().snapshot();
    EXPECT_EQ(snapshot.events_created, 1U);
    ASSERT_EQ(snapshot.cameras.size(), 1U);
    EXPECT_EQ(snapshot.cameras.front().rearm_suppressed_results, 38U);
}

TEST(EventCandidateState, RecoveryRequiresStrictNormalDurationAndResetsOnAnomalyOrGap)
{
    CandidateHarness harness({"CAM01"}, 1U, 1U, {}, 10s, 0ms, 0.5, 0.8,
                             ExternalConfirmationPolicy::not_used, 0ms, 10ms, 100ms, 500ms);
    ASSERT_TRUE(harness.submit("CAM01", 1U, 0ms, true, 0.9));
    ASSERT_TRUE(harness.submit("CAM01", 2U, 10ms, true, 0.9));
    ASSERT_TRUE(harness.submit("CAM01", 3U, 20ms, false));
    auto low_confidence_anomaly = harness.submit("CAM01", 4U, 519ms, true, 0.1);
    ASSERT_TRUE(low_confidence_anomaly);
    EXPECT_TRUE(low_confidence_anomaly.value().camera.rearm_pending);
    EXPECT_FALSE(low_confidence_anomaly.value().camera.recovery_started_at);

    ASSERT_TRUE(harness.submit("CAM01", 5U, 520ms, false));
    ASSERT_TRUE(harness.submit("CAM01", 6U, 620ms, false));
    ASSERT_TRUE(harness.submit("CAM01", 7U, 720ms, false));
    ASSERT_TRUE(harness.submit("CAM01", 8U, 820ms, false));
    ASSERT_TRUE(harness.submit("CAM01", 9U, 920ms, false));
    auto at_499 = harness.submit("CAM01", 10U, 1019ms, false);
    ASSERT_TRUE(at_499);
    EXPECT_TRUE(at_499.value().camera.rearm_pending);
    auto exact = harness.submit("CAM01", 11U, 1020ms, false);
    ASSERT_TRUE(exact);
    EXPECT_FALSE(exact.value().camera.rearm_pending);

    auto second = harness.submit("CAM01", 12U, 1030ms, true, 0.9);
    ASSERT_TRUE(second);
    ASSERT_TRUE(second.value().camera.event);
    auto rejected = harness.manager().reject(second.value().camera.event->event_id, 1U,
                                             MonotonicTime{1040ms}, WallClockTime{1040ms});
    ASSERT_TRUE(rejected);
    ASSERT_TRUE(harness.submit("CAM01", 13U, 1050ms, false));
    ASSERT_TRUE(harness.submit("CAM01", 14U, 1250ms, false));
    auto gap_restart = harness.submit("CAM01", 15U, 1451ms, false);
    ASSERT_TRUE(gap_restart);
    ASSERT_TRUE(gap_restart.value().camera.recovery_started_at);
    EXPECT_EQ(*gap_restart.value().camera.recovery_started_at, MonotonicTime{1451ms});
    ASSERT_TRUE(harness.submit("CAM01", 16U, 1551ms, false));
    ASSERT_TRUE(harness.submit("CAM01", 17U, 1651ms, false));
    ASSERT_TRUE(harness.submit("CAM01", 18U, 1751ms, false));
    ASSERT_TRUE(harness.submit("CAM01", 19U, 1851ms, false));
    auto gap_499 = harness.submit("CAM01", 20U, 1950ms, false);
    ASSERT_TRUE(gap_499);
    EXPECT_TRUE(gap_499.value().camera.rearm_pending);
    auto gap_exact = harness.submit("CAM01", 21U, 1951ms, false);
    ASSERT_TRUE(gap_exact);
    EXPECT_FALSE(gap_exact.value().camera.rearm_pending);
}

TEST(EventCandidateState, RecoveryAccumulatesDuringCooldownAndZeroDurationNeedsOneNormalResult)
{
    CandidateHarness cooling({"CAM01"}, 1U, 1U, {}, 10s, 0ms, 0.5, 0.8,
                             ExternalConfirmationPolicy::not_used, 1s, 10ms, 100ms, 500ms);
    ASSERT_TRUE(cooling.submit("CAM01", 1U, 0ms, true, 0.9));
    ASSERT_TRUE(cooling.submit("CAM01", 2U, 10ms, true, 0.9));
    for (std::uint64_t sequence = 3U; sequence <= 12U; ++sequence)
    {
        auto normal = cooling.submit("CAM01", sequence,
                                     std::chrono::milliseconds{(sequence - 2U) * 100U}, false);
        ASSERT_TRUE(normal);
        EXPECT_TRUE(normal.value().camera.rearm_pending);
    }
    auto cooldown_boundary = cooling.submit("CAM01", 13U, 1010ms, false);
    ASSERT_TRUE(cooldown_boundary);
    EXPECT_FALSE(cooldown_boundary.value().camera.rearm_pending);

    CandidateHarness zero({"CAM01"}, 1U, 1U, {}, 10s, 0ms, 0.5, 0.8,
                          ExternalConfirmationPolicy::not_used, 0ms, 10ms, 100ms, 0ms);
    ASSERT_TRUE(zero.submit("CAM01", 1U, 0ms, true, 0.9));
    ASSERT_TRUE(zero.submit("CAM01", 2U, 10ms, true, 0.9));
    EXPECT_TRUE(zero.manager().snapshot().cameras.front().rearm_pending);
    auto first_normal = zero.submit("CAM01", 3U, 20ms, false);
    ASSERT_TRUE(first_normal);
    EXPECT_FALSE(first_normal.value().camera.rearm_pending);
}

TEST(EventCandidateState, AllTerminalStatesLatchAndExternalSignalDoesNotRearm)
{
    for (const auto terminal : {CandidateEventState::confirmed, CandidateEventState::rejected,
                                CandidateEventState::timeout})
    {
        CandidateHarness harness({"CAM01"}, 1U, 1U, {}, 100ms, 0ms, 0.5, 0.8,
                                 ExternalConfirmationPolicy::required_active, 0ms, 10ms, 100ms,
                                 500ms);
        auto candidate = harness.submit("CAM01", 1U, 0ms, true, 0.9);
        ASSERT_TRUE(candidate);
        ASSERT_TRUE(candidate.value().camera.event);
        if (terminal == CandidateEventState::confirmed)
        {
            ASSERT_TRUE(harness.submit("CAM01", 2U, 10ms, true, 0.9));
            auto signal = harness.manager().update_external_signal(
                "CAM01", true, MonotonicTime{10ms}, WallClockTime{10ms});
            ASSERT_TRUE(signal);
        }
        else if (terminal == CandidateEventState::rejected)
        {
            ASSERT_TRUE(harness.manager().reject(candidate.value().camera.event->event_id, 1U,
                                                 MonotonicTime{10ms}, WallClockTime{10ms}));
        }
        else
        {
            ASSERT_EQ(
                harness.manager().advance_time(MonotonicTime{100ms}, WallClockTime{100ms}).size(),
                1U);
        }
        auto snapshot = harness.manager().snapshot();
        ASSERT_EQ(snapshot.cameras.size(), 1U);
        EXPECT_TRUE(snapshot.cameras.front().rearm_pending);
        auto signal = harness.manager().update_external_signal("CAM01", false, MonotonicTime{110ms},
                                                               WallClockTime{110ms});
        ASSERT_TRUE(signal);
        EXPECT_TRUE(signal.value().rearm_pending);
    }
}

TEST(EventCandidateState, ManualTriggerBypassesLatchWithoutParallelCandidateAndSeedsHotReconfigure)
{
    CandidateHarness old({"CAM01"}, 1U, 1U, {}, 10s, 0ms, 0.5, 0.8,
                         ExternalConfirmationPolicy::not_used, 1s, 10ms, 100ms, 500ms);
    ASSERT_TRUE(old.submit("CAM01", 1U, 0ms, true, 0.9));
    auto manual_joins = old.submit("CAM01", 2U, 5ms, true, 0.1, TriggerSource::manual_test);
    ASSERT_TRUE(manual_joins);
    EXPECT_EQ(old.manager().snapshot().events_created, 1U);
    EXPECT_EQ(manual_joins.value().camera.observation_state, CandidateEventState::candidate);
    ASSERT_TRUE(old.submit("CAM01", 3U, 15ms, true, 0.9));
    ASSERT_TRUE(old.submit("CAM01", 4U, 25ms, true, 0.9));

    auto manual = old.submit("CAM01", 5U, 30ms, true, 0.1, TriggerSource::manual_test);
    ASSERT_TRUE(manual);
    EXPECT_EQ(manual.value().camera.observation_state, CandidateEventState::candidate);
    EXPECT_FALSE(manual.value().camera.rearm_pending);
    EXPECT_EQ(old.manager().snapshot().events_created, 2U);

    auto automatic_joins = old.submit("CAM01", 6U, 40ms, true, 0.9);
    ASSERT_TRUE(automatic_joins);
    EXPECT_EQ(old.manager().snapshot().events_created, 2U);
    EXPECT_EQ(automatic_joins.value().camera.observation_state, CandidateEventState::candidate);
    auto manual_confirmed = old.submit("CAM01", 7U, 50ms, true, 0.9);
    ASSERT_TRUE(manual_confirmed);
    EXPECT_EQ(manual_confirmed.value().camera.observation_state, CandidateEventState::confirmed);

    const auto seeds = old.manager().rearm_seeds(MonotonicTime{50ms});
    CandidateHarness replacement({"CAM01"}, 1U, 1U, {}, 10s, 0ms, 0.5, 0.8,
                                 ExternalConfirmationPolicy::not_used, 1s, 10ms, 100ms, 500ms);
    ASSERT_TRUE(replacement.manager().apply_rearm_seeds(seeds));
    auto suppressed = replacement.submit("CAM01", 1U, 60ms, true, 0.9);
    ASSERT_TRUE(suppressed);
    EXPECT_TRUE(suppressed.value().camera.rearm_pending);
    EXPECT_EQ(replacement.manager().snapshot().events_created, 0U);
}

TEST(EventCandidateState, HotReconfigureSeedDropsAccumulatedRecoveryDuration)
{
    CandidateHarness old({"CAM01"}, 1U, 1U, {}, 10s, 0ms, 0.5, 0.8,
                         ExternalConfirmationPolicy::not_used, 0ms, 10ms, 100ms, 500ms);
    ASSERT_TRUE(old.submit("CAM01", 1U, 0ms, true, 0.9));
    ASSERT_TRUE(old.submit("CAM01", 2U, 10ms, true, 0.9));
    for (std::uint64_t sequence = 3U; sequence <= 7U; ++sequence)
    {
        ASSERT_TRUE(old.submit("CAM01", sequence,
                               std::chrono::milliseconds{20U + (sequence - 3U) * 100U}, false));
    }
    EXPECT_TRUE(old.manager().snapshot().cameras.front().rearm_pending);

    CandidateHarness replacement({"CAM01"}, 1U, 1U, {}, 10s, 0ms, 0.5, 0.8,
                                 ExternalConfirmationPolicy::not_used, 0ms, 10ms, 100ms, 500ms);
    ASSERT_TRUE(
        replacement.manager().apply_rearm_seeds(old.manager().rearm_seeds(MonotonicTime{420ms})));
    for (std::uint64_t sequence = 1U; sequence <= 5U; ++sequence)
    {
        ASSERT_TRUE(replacement.submit(
            "CAM01", sequence, std::chrono::milliseconds{430U + (sequence - 1U) * 100U}, false));
    }
    auto before = replacement.submit("CAM01", 6U, 929ms, false);
    ASSERT_TRUE(before);
    EXPECT_TRUE(before.value().camera.rearm_pending);
    auto exact = replacement.submit("CAM01", 7U, 930ms, false);
    ASSERT_TRUE(exact);
    EXPECT_FALSE(exact.value().camera.rearm_pending);
}

TEST(EventCandidateState, FourCameraRecoveryAndSuppressionRemainIndependent)
{
    CandidateHarness harness({"CAM01", "CAM02", "CAM03", "CAM04"}, 1U, 1U, {}, 10s, 0ms, 0.5, 0.8,
                             ExternalConfirmationPolicy::not_used, 0ms, 10ms, 100ms, 500ms);
    for (const auto camera_id : {"CAM01", "CAM02", "CAM03", "CAM04"})
    {
        ASSERT_TRUE(harness.submit(camera_id, 1U, 0ms, true, 0.9));
        ASSERT_TRUE(harness.submit(camera_id, 2U, 10ms, true, 0.9));
    }
    for (std::uint64_t sequence = 3U; sequence <= 8U; ++sequence)
    {
        ASSERT_TRUE(harness.submit("CAM01", sequence,
                                   std::chrono::milliseconds{20U + (sequence - 3U) * 100U}, false));
    }
    ASSERT_TRUE(harness.submit("CAM02", 3U, 20ms, true, 0.1));

    const auto snapshot = harness.manager().snapshot();
    const auto find = [&](const std::string_view camera_id) -> const CandidateCameraSnapshot& {
        return *std::ranges::find(snapshot.cameras, camera_id, &CandidateCameraSnapshot::camera_id);
    };
    EXPECT_FALSE(find("CAM01").rearm_pending);
    EXPECT_TRUE(find("CAM02").rearm_pending);
    EXPECT_EQ(find("CAM02").rearm_suppressed_results, 1U);
    EXPECT_TRUE(find("CAM03").rearm_pending);
    EXPECT_EQ(find("CAM03").rearm_suppressed_results, 0U);
    EXPECT_TRUE(find("CAM04").rearm_pending);
    EXPECT_EQ(find("CAM04").rearm_suppressed_results, 0U);
}

TEST(EventCandidateState, ResetsOnlyBeyondTwoPeriodsAndRejectsStaleExternalConfirmation)
{
    CandidateHarness exact({"CAM01"}, 1U, 1U, {}, 2s, 0ms, 0.5, 0.8,
                           ExternalConfirmationPolicy::required_active, 0ms, 120ms, 50ms);
    ASSERT_TRUE(exact.submit("CAM01", 1U, 0ms, true, 0.9));
    ASSERT_TRUE(exact.submit("CAM01", 2U, 100ms, true, 0.9));
    auto exact_signal = exact.manager().update_external_signal("CAM01", true, MonotonicTime{100ms},
                                                               WallClockTime{100ms});
    ASSERT_TRUE(exact_signal);
    EXPECT_EQ(exact_signal.value().observation_state, CandidateEventState::candidate);
    ASSERT_TRUE(exact.submit("CAM01", 3U, 120ms, true, 0.9));
    auto fresh_signal = exact.manager().update_external_signal("CAM01", true, MonotonicTime{120ms},
                                                               WallClockTime{120ms});
    ASSERT_TRUE(fresh_signal);
    EXPECT_EQ(fresh_signal.value().observation_state, CandidateEventState::confirmed);

    CandidateHarness stale({"CAM01"}, 1U, 1U, {}, 2s, 0ms, 0.5, 0.8,
                           ExternalConfirmationPolicy::required_active, 0ms, 120ms, 50ms);
    ASSERT_TRUE(stale.submit("CAM01", 1U, 0ms, true, 0.9));
    ASSERT_TRUE(stale.submit("CAM01", 2U, 101ms, true, 0.9));
    auto stale_signal = stale.manager().update_external_signal("CAM01", true, MonotonicTime{250ms},
                                                               WallClockTime{250ms});
    ASSERT_TRUE(stale_signal);
    EXPECT_EQ(stale_signal.value().observation_state, CandidateEventState::candidate);
}

TEST(EventCandidateState, ExternalSignalAtCandidateDeadlineTimesOutInsteadOfConfirming)
{
    CandidateHarness harness({"CAM01"}, 1U, 1U, {}, 100ms, 0ms, 0.5, 0.8,
                             ExternalConfirmationPolicy::required_active);

    auto candidate = harness.submit("CAM01", 1U, 100ms, true, 0.9);
    ASSERT_TRUE(candidate);
    EXPECT_EQ(candidate.value().camera.observation_state, CandidateEventState::candidate);

    auto at_deadline = harness.manager().update_external_signal("CAM01", true, MonotonicTime{200ms},
                                                                WallClockTime{200ms});
    ASSERT_TRUE(at_deadline);
    EXPECT_EQ(at_deadline.value().observation_state, CandidateEventState::timeout);
    ASSERT_TRUE(at_deadline.value().event);
    EXPECT_EQ(at_deadline.value().event->decision_state, CandidateEventState::timeout);
}

TEST(EventCandidateState, RejectsWithVersioningAndMakesRepeatedCommandIdempotent)
{
    CandidateHarness harness({"CAM01"}, 1U, 5U);
    auto candidate = harness.submit("CAM01", 1U, 100ms, true);
    ASSERT_TRUE(candidate);
    const auto event_id = candidate.value().camera.event->event_id;

    auto conflict =
        harness.manager().reject(event_id, 2U, MonotonicTime{200ms}, WallClockTime{200ms});
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().business_code, "EVENT_VERSION_CONFLICT");

    auto rejected =
        harness.manager().reject(event_id, 1U, MonotonicTime{200ms}, WallClockTime{200ms});
    ASSERT_TRUE(rejected);
    EXPECT_FALSE(rejected.value().duplicate);
    EXPECT_EQ(rejected.value().event.decision_state, CandidateEventState::rejected);
    EXPECT_EQ(rejected.value().event.version, 2U);

    auto duplicate =
        harness.manager().reject(event_id, 1U, MonotonicTime{300ms}, WallClockTime{300ms});
    ASSERT_TRUE(duplicate);
    EXPECT_TRUE(duplicate.value().duplicate);
    EXPECT_EQ(duplicate.value().event.version, 2U);

    auto invalid =
        harness.manager().confirm(event_id, 2U, MonotonicTime{300ms}, WallClockTime{300ms});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().business_code, "EVENT_INVALID_TRANSITION");
    auto missing =
        harness.manager().confirm("EVT-missing", 1U, MonotonicTime{300ms}, WallClockTime{300ms});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().business_code, "EVENT_NOT_FOUND");
}

TEST(EventCandidateState, TimesOutAtExactMonotonicDeadlineDespiteWallClockJump)
{
    CandidateHarness harness({"CAM01"}, 1U, 5U, {}, 1000ms);
    auto candidate = harness.submit("CAM01", 1U, 100ms, true);
    ASSERT_TRUE(candidate);
    EXPECT_TRUE(harness.manager().advance_time(MonotonicTime{1099ms}, WallClockTime{50h}).empty());

    auto timed_out = harness.manager().advance_time(MonotonicTime{1100ms}, WallClockTime{-50h});
    ASSERT_EQ(timed_out.size(), 1U);
    EXPECT_EQ(timed_out.front().decision_state, CandidateEventState::timeout);
    ASSERT_TRUE(timed_out.front().decision);
    EXPECT_EQ(timed_out.front().decision->monotonic_time, MonotonicTime{1100ms});
    EXPECT_EQ(timed_out.front().decision->wall_clock_time, WallClockTime{-50h});
    EXPECT_TRUE(harness.manager().advance_time(MonotonicTime{1200ms}, WallClockTime{}).empty());
}

TEST(EventCandidateState, DuplicateResultIsIdempotentAndOrderingErrorsRecover)
{
    CandidateHarness harness({"CAM01"}, 2U, 5U);
    const auto first_result = trigger_result("CAM01", 1U, 100ms, true);
    auto first = harness.submit("CAM01", 1U, 100ms, true);
    ASSERT_TRUE(first);

    auto duplicate = harness.manager().process(first_result);
    ASSERT_TRUE(duplicate);
    EXPECT_TRUE(duplicate.value().duplicate);
    EXPECT_EQ(duplicate.value().camera.consecutive_triggered_frames, 1U);

    auto conflicting = first_result;
    conflicting.reason = "conflict";
    auto conflict = harness.manager().process(conflicting);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().business_code, "PIPELINE_FRAME_ORDER_VIOLATION");

    auto same_time = harness.manager().process(trigger_result("CAM01", 2U, 100ms, true));
    ASSERT_FALSE(same_time);
    EXPECT_EQ(same_time.error().business_code, "PIPELINE_FRAME_ORDER_VIOLATION");
    auto unknown = harness.manager().process(trigger_result("CAM02", 1U, 200ms, true));
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().business_code, "PIPELINE_FRAME_ORDER_VIOLATION");

    auto candidate = harness.submit("CAM01", 2U, 200ms, true);
    ASSERT_TRUE(candidate);
    EXPECT_EQ(candidate.value().camera.observation_state, CandidateEventState::candidate);
    const auto snapshot = harness.manager().snapshot();
    EXPECT_EQ(snapshot.events_created, 1U);
    EXPECT_EQ(snapshot.duplicate_results, 1U);
    EXPECT_EQ(snapshot.rejected_results, 3U);
}

TEST(EventCandidateState, ConcurrentFourCameraTriggersRemainIndependentAndUnique)
{
    CandidateHarness harness({"CAM01", "CAM02", "CAM03", "CAM04"}, 1U, 5U);
    const std::array<std::string, 4U> camera_ids{"CAM01", "CAM02", "CAM03", "CAM04"};
    std::array<std::optional<Result<CandidateProcessOutcome>>, 4U> outcomes;
    std::vector<std::jthread> threads;
    threads.reserve(camera_ids.size());
    for (std::size_t index = 0U; index < camera_ids.size(); ++index)
    {
        threads.emplace_back([&, index] {
            outcomes[index].emplace(harness.submit(camera_ids[index], 1U, 100ms, true));
        });
    }
    threads.clear();

    std::set<std::string> event_ids;
    for (const auto& outcome : outcomes)
    {
        ASSERT_TRUE(outcome);
        ASSERT_TRUE(*outcome);
        ASSERT_TRUE(outcome->value().camera.event);
        EXPECT_EQ(outcome->value().camera.observation_state, CandidateEventState::candidate);
        event_ids.insert(outcome->value().camera.event->event_id);
    }
    EXPECT_EQ(event_ids.size(), 4U);
    const auto snapshot = harness.manager().snapshot();
    EXPECT_EQ(snapshot.events_created, 4U);
    EXPECT_EQ(snapshot.cameras.size(), 4U);
}

TEST(EventCandidateState, NotificationExceptionsAreContainedAndCounted)
{
    std::atomic_uint64_t calls{};
    CandidateHarness harness({"CAM01"}, 1U, 2U, [&](const CandidateEventNotification&) {
        ++calls;
        throw std::runtime_error{"injected notification failure"};
    });
    auto candidate = harness.submit("CAM01", 1U, 100ms, true);
    ASSERT_TRUE(candidate);
    auto confirmed = harness.submit("CAM01", 2U, 200ms, true);
    ASSERT_TRUE(confirmed);
    EXPECT_EQ(confirmed.value().camera.observation_state, CandidateEventState::confirmed);
    EXPECT_EQ(calls.load(), 2U);
    EXPECT_EQ(harness.manager().snapshot().callback_failures, 2U);
}

TEST(EventCandidateState, StopTimesOutCandidatesClearsSuspiciousAndRejectsNewResults)
{
    std::vector<CandidateEventNotification> notifications;
    CandidateHarness harness({"CAM01", "CAM02"}, 2U, 5U, [&](const auto& notification) {
        notifications.push_back(notification);
    });
    ASSERT_TRUE(harness.submit("CAM01", 1U, 100ms, true));
    ASSERT_TRUE(harness.submit("CAM01", 2U, 200ms, true));
    ASSERT_TRUE(harness.submit("CAM02", 1U, 100ms, true));
    EXPECT_EQ(harness.ring("CAM01").snapshot().active_leases, 1U);

    auto stopped = harness.manager().stop(MonotonicTime{300ms}, WallClockTime{300ms});
    ASSERT_EQ(stopped.size(), 1U);
    EXPECT_EQ(stopped.front().decision_state, CandidateEventState::timeout);
    EXPECT_EQ(harness.ring("CAM01").snapshot().active_leases, 0U);
    const auto snapshot = harness.manager().snapshot();
    EXPECT_TRUE(snapshot.stopped);
    ASSERT_EQ(snapshot.cameras.size(), 2U);
    EXPECT_EQ(snapshot.cameras[0].observation_state, CandidateEventState::timeout);
    EXPECT_EQ(snapshot.cameras[1].observation_state, CandidateEventState::idle);
    EXPECT_TRUE(harness.manager().stop(MonotonicTime{400ms}, WallClockTime{400ms}).empty());

    auto after_stop = harness.manager().process(trigger_result("CAM01", 3U, 300ms, true));
    ASSERT_FALSE(after_stop);
    EXPECT_EQ(after_stop.error().business_code, "EVENT_INVALID_TRANSITION");
    ASSERT_EQ(notifications.size(), 2U);
    EXPECT_EQ(notifications.back().event.decision_state, CandidateEventState::timeout);
}

TEST(EventCandidateState, RejectsUnsafeConfiguration)
{
    CandidateEventManagerConfig empty;
    auto created = CandidateEventManager::create(std::move(empty));
    ASSERT_FALSE(created);
    EXPECT_EQ(created.error().business_code, "SYS_CONFIG_INVALID");

    MemoryRing ring{{.camera_id = "CAM01",
                     .capacity_frames = 2U,
                     .required_history_seconds = 0.0,
                     .maximum_active_leases = 1U,
                     .maximum_leased_frame_references = 2U}};
    CandidateEventManagerConfig invalid_threshold{
        .cameras = {{.camera_id = "CAM01", .memory_ring = &ring}},
        .candidate_consecutive_frames = 2U,
        .confirmation_duration = 0ms,
    };
    created = CandidateEventManager::create(std::move(invalid_threshold));
    ASSERT_FALSE(created);
    EXPECT_EQ(created.error().business_code, "SYS_CONFIG_INVALID");

    CandidateEventManagerConfig invalid_confidence{
        .cameras = {{.camera_id = "CAM01", .memory_ring = &ring}},
        .candidate_confidence_threshold = 0.8,
        .confirmation_confidence_threshold = 0.7,
    };
    created = CandidateEventManager::create(std::move(invalid_confidence));
    ASSERT_FALSE(created);
    EXPECT_EQ(created.error().business_code, "SYS_CONFIG_INVALID");

    CandidateEventManagerConfig duplicate{
        .cameras = {{.camera_id = "CAM01", .memory_ring = &ring},
                    {.camera_id = "CAM01", .memory_ring = &ring}},
    };
    created = CandidateEventManager::create(std::move(duplicate));
    ASSERT_FALSE(created);
    EXPECT_EQ(created.error().business_code, "SYS_CONFIG_INVALID");
}
