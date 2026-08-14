#include "paperbreak/camera/camera.hpp"
#include "paperbreak/camera/state.hpp"
#include "paperbreak/common/camera_slots.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace paperbreak;
using namespace paperbreak::camera;
using namespace std::chrono_literals;

constexpr std::array<CameraState, 8U> all_states = {
    CameraState::disabled,   CameraState::disconnected, CameraState::connecting,
    CameraState::connected,  CameraState::starting,     CameraState::streaming,
    CameraState::recovering, CameraState::faulted};

bool expected_transition(const CameraState from, const CameraState to)
{
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

Error retryable_failure(std::string source)
{
    return make_camera_error(CameraErrorKind::disconnected, "测试相机掉线", "camera.testFailure",
                             std::move(source));
}

ReconnectWaiter immediate_waiter()
{
    return [](const std::stop_token token, std::chrono::milliseconds) {
        return !token.stop_requested();
    };
}
} // namespace

TEST(CameraStateMachine, ImplementsCompleteEightByEightTransitionTable)
{
    for (const CameraState from : all_states)
    {
        for (const CameraState to : all_states)
        {
            EXPECT_EQ(is_camera_transition_allowed(from, to), expected_transition(from, to))
                << camera_state_name(from) << " -> " << camera_state_name(to);
        }
    }
}

TEST(CameraStateMachine, RejectsAndRecordsInvalidTransitionWithStableError)
{
    std::vector<CameraTransitionRecord> records;
    CameraSessionController controller{
        "CAM01", false, {}, [&](const CameraTransitionRecord& record) {
            records.push_back(record);
        }};

    const auto result = controller.transition_to(CameraState::streaming, "skip-startup");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "CAMERA_INVALID_STATE_TRANSITION");
    EXPECT_FALSE(result.error().retryable);
    ASSERT_EQ(records.size(), 1U);
    EXPECT_FALSE(records.front().accepted);
    EXPECT_EQ(records.front().camera_id, "CAM01");
    EXPECT_EQ(records.front().from, CameraState::disabled);
    EXPECT_EQ(records.front().to, CameraState::streaming);
    ASSERT_TRUE(records.front().cause);
    EXPECT_EQ(records.front().cause->business_code, "CAMERA_INVALID_STATE_TRANSITION");
    ASSERT_EQ(result.error().details.size(), 3U);
    EXPECT_EQ(result.error().details[0].value, "Disabled");
    EXPECT_EQ(result.error().details[1].value, "Streaming");
    EXPECT_EQ(result.error().details[2].value, "skip-startup");
}

TEST(CameraReconnect, UsesSixStepBackoffAndCapsLongerPoliciesAtSixtySeconds)
{
    ReconnectController controller{ReconnectPolicy{.maximum_attempts = 8U}, immediate_waiter()};
    const std::array expected = {1s, 2s, 5s, 10s, 30s, 60s, 60s, 60s};

    for (const auto delay : expected)
    {
        ASSERT_EQ(controller.schedule_next(), delay);
        EXPECT_EQ(controller.wait_for_scheduled(), ReconnectWaitResult::ready);
    }
    EXPECT_FALSE(controller.schedule_next());
    EXPECT_TRUE(controller.exhausted());
    EXPECT_EQ(controller.attempt_count(), expected.size());
}

TEST(CameraReconnect, KeepsSchedulingIdempotentAndAllowsOnlyOneReadyWaiter)
{
    ReconnectController controller{{}, immediate_waiter()};
    ASSERT_EQ(controller.schedule_next(), 1s);
    EXPECT_EQ(controller.schedule_next(), 1s);
    EXPECT_EQ(controller.attempt_count(), 1U);
    EXPECT_EQ(controller.wait_for_scheduled(), ReconnectWaitResult::ready);
    EXPECT_EQ(controller.wait_for_scheduled(), ReconnectWaitResult::not_scheduled);
}

TEST(CameraReconnect, ValidatesAttemptBounds)
{
    auto result = validate_reconnect_policy({.maximum_attempts = 0U});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "CAMERA_CONFIG_FAILED");

    result = validate_reconnect_policy({.maximum_attempts = maximum_recovery_attempts_limit + 1U});
    EXPECT_FALSE(result);
    EXPECT_THROW((ReconnectController{ReconnectPolicy{.maximum_attempts = 0U}}),
                 std::invalid_argument);
}

TEST(CameraReconnect, ResetsOnlyAfterReturningToStreaming)
{
    CameraSessionController controller{"CAM01", true, {}, {}, immediate_waiter()};
    ASSERT_TRUE(controller.transition_to(CameraState::connecting, "start"));
    ASSERT_TRUE(controller.handle_failure(retryable_failure("CAM01"), "connect-failed"));
    EXPECT_EQ(controller.snapshot().recovery_attempt, 1U);
    ASSERT_EQ(controller.wait_for_retry(), ReconnectWaitResult::ready);
    ASSERT_TRUE(controller.handle_failure(retryable_failure("CAM01"), "connect-failed"));
    EXPECT_EQ(controller.snapshot().recovery_attempt, 2U);
    ASSERT_EQ(controller.wait_for_retry(), ReconnectWaitResult::ready);
    ASSERT_TRUE(controller.transition_to(CameraState::connected, "connected"));
    ASSERT_TRUE(controller.transition_to(CameraState::starting, "starting"));
    EXPECT_EQ(controller.snapshot().recovery_attempt, 2U);
    ASSERT_TRUE(controller.transition_to(CameraState::streaming, "streaming"));
    EXPECT_EQ(controller.snapshot().recovery_attempt, 0U);
    EXPECT_FALSE(controller.snapshot().next_retry_delay);

    ASSERT_TRUE(controller.handle_failure(retryable_failure("CAM01"), "stream-lost"));
    EXPECT_EQ(controller.snapshot().next_retry_delay, 1s);
}

TEST(CameraReconnect, CancelsLongWaitPromptlyDuringStop)
{
    CameraSessionController controller{"CAM01", true};
    ASSERT_TRUE(controller.transition_to(CameraState::connecting, "start"));
    ASSERT_TRUE(controller.handle_failure(retryable_failure("CAM01"), "connect-failed"));

    std::atomic<ReconnectWaitResult> wait_result{ReconnectWaitResult::not_scheduled};
    const auto started = std::chrono::steady_clock::now();
    std::jthread waiter([&](std::stop_token) { wait_result.store(controller.wait_for_retry()); });
    std::this_thread::sleep_for(20ms);
    ASSERT_TRUE(controller.request_stop(true));
    waiter.join();

    EXPECT_EQ(wait_result.load(), ReconnectWaitResult::cancelled);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
    const auto snapshot = controller.snapshot();
    EXPECT_EQ(snapshot.state, CameraState::disconnected);
    EXPECT_TRUE(snapshot.stop_requested);
    EXPECT_FALSE(snapshot.next_retry_delay);
}

TEST(CameraReconnect, EntersFaultedAfterSixFailedRecoveryAttemptsAndCanReset)
{
    CameraSessionController controller{"CAM01", true, {}, {}, immediate_waiter()};
    ASSERT_TRUE(controller.transition_to(CameraState::connecting, "start"));

    for (std::size_t attempt = 0U; attempt < default_maximum_recovery_attempts; ++attempt)
    {
        ASSERT_TRUE(controller.handle_failure(retryable_failure("CAM01"), "retryable"));
        ASSERT_EQ(controller.snapshot().state, CameraState::recovering);
        ASSERT_EQ(controller.wait_for_retry(), ReconnectWaitResult::ready);
    }

    ASSERT_TRUE(controller.handle_failure(retryable_failure("CAM01"), "retry-exhausted"));
    auto snapshot = controller.snapshot();
    ASSERT_EQ(snapshot.state, CameraState::faulted);
    EXPECT_EQ(snapshot.recovery_attempt, default_maximum_recovery_attempts);
    EXPECT_FALSE(snapshot.next_retry_delay);
    ASSERT_TRUE(snapshot.last_error);
    EXPECT_EQ(snapshot.last_error->business_code, "CAMERA_DISCONNECTED");
    EXPECT_FALSE(snapshot.last_error->retryable);

    ASSERT_TRUE(controller.reset_fault("operator-retry"));
    snapshot = controller.snapshot();
    EXPECT_EQ(snapshot.state, CameraState::disconnected);
    EXPECT_EQ(snapshot.recovery_attempt, 0U);
    EXPECT_FALSE(snapshot.last_error);
    EXPECT_FALSE(snapshot.stop_requested);
}

TEST(CameraReconnect, SendsNonRetryableFailureDirectlyToFaulted)
{
    CameraSessionController controller{"CAM01", true};
    ASSERT_TRUE(controller.transition_to(CameraState::connecting, "start"));
    Error error = make_camera_error(CameraErrorKind::config_failed, "测试配置错误",
                                    "camera.testFailure", "CAM01");
    ASSERT_TRUE(controller.handle_failure(error, "permanent"));

    const auto snapshot = controller.snapshot();
    ASSERT_EQ(snapshot.state, CameraState::faulted);
    EXPECT_EQ(snapshot.recovery_attempt, 0U);
    ASSERT_TRUE(snapshot.last_error);
    EXPECT_EQ(snapshot.last_error->business_code, "CAMERA_CONFIG_FAILED");
}

TEST(CameraReconnect, ReenableResetsCancelledRecoveryPolicy)
{
    CameraSessionController controller{"CAM01", true, {}, {}, immediate_waiter()};
    ASSERT_TRUE(controller.transition_to(CameraState::connecting, "start"));
    ASSERT_TRUE(controller.handle_failure(retryable_failure("CAM01"), "connect-failed"));
    ASSERT_EQ(controller.snapshot().recovery_attempt, 1U);
    ASSERT_TRUE(controller.request_stop(false));
    ASSERT_EQ(controller.snapshot().state, CameraState::disabled);
    ASSERT_TRUE(controller.snapshot().stop_requested);

    ASSERT_TRUE(controller.transition_to(CameraState::disconnected, "reenabled"));
    const auto snapshot = controller.snapshot();
    EXPECT_EQ(snapshot.recovery_attempt, 0U);
    EXPECT_FALSE(snapshot.next_retry_delay);
    EXPECT_FALSE(snapshot.stop_requested);
}

TEST(CameraReconnect, DoesNotConsumeRetryWhenFailureIsIllegalInCurrentState)
{
    CameraSessionController controller{"CAM01", false, {}, {}, immediate_waiter()};
    const auto result = controller.handle_failure(retryable_failure("CAM01"), "disabled-failure");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "CAMERA_INVALID_STATE_TRANSITION");
    EXPECT_EQ(controller.snapshot().state, CameraState::disabled);
    EXPECT_EQ(controller.snapshot().recovery_attempt, 0U);
    EXPECT_FALSE(controller.snapshot().next_retry_delay);
}

TEST(CameraStateMachine, IsolatesSixConcurrentCameraControllers)
{
    std::array<std::unique_ptr<CameraSessionController>, paperbreak::camera_slot_count> controllers;
    std::array<std::vector<CameraTransitionRecord>, paperbreak::camera_slot_count> records;
    std::array<std::mutex, paperbreak::camera_slot_count> record_mutexes;
    for (std::size_t index = 0U; index < controllers.size(); ++index)
    {
        const std::string id{paperbreak::canonical_camera_ids[index]};
        controllers[index] = std::make_unique<CameraSessionController>(
            id, true, ReconnectPolicy{},
            [&, index](const CameraTransitionRecord& record) {
                std::lock_guard lock{record_mutexes[index]};
                records[index].push_back(record);
            },
            immediate_waiter());
    }

    std::array<std::jthread, paperbreak::camera_slot_count> workers;
    for (std::size_t index = 0U; index < workers.size(); ++index)
    {
        workers[index] = std::jthread([&, index](std::stop_token) {
            static_cast<void>(controllers[index]->transition_to(CameraState::connecting, "start"));
            for (std::size_t attempt = 0U; attempt <= index; ++attempt)
            {
                static_cast<void>(controllers[index]->handle_failure(
                    retryable_failure(std::string{paperbreak::canonical_camera_ids[index]}),
                    "failure"));
                static_cast<void>(controllers[index]->wait_for_retry());
            }
        });
    }
    for (auto& worker : workers)
    {
        worker.join();
    }

    for (std::size_t index = 0U; index < controllers.size(); ++index)
    {
        const auto snapshot = controllers[index]->snapshot();
        EXPECT_EQ(snapshot.state, CameraState::connecting);
        EXPECT_EQ(snapshot.recovery_attempt, index + 1U);
        ASSERT_FALSE(records[index].empty());
        for (const auto& record : records[index])
        {
            EXPECT_EQ(record.camera_id, paperbreak::canonical_camera_ids[index]);
        }
    }
}

TEST(CameraStateMachine, ContainsObserverExceptionsAfterStateCommit)
{
    CameraSessionController controller{
        "CAM01", true, {}, [](const CameraTransitionRecord&) { throw std::runtime_error{"test"}; }};
    EXPECT_TRUE(controller.transition_to(CameraState::connecting, "start"));
    EXPECT_EQ(controller.snapshot().state, CameraState::connecting);
}
