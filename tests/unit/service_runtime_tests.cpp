#include "paperbreak/service/runtime.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using paperbreak::Result;
using paperbreak::service::ILifecycleComponent;
using paperbreak::service::ShutdownPhase;
using paperbreak::service::StopReason;

enum class StartBehavior
{
    succeed,
    fail,
    throw_exception,
    wait_for_cancel,
};

enum class JoinBehavior
{
    succeed,
    wait_until_deadline,
    throw_exception,
};

class RecordingComponent final : public ILifecycleComponent
{
  public:
    RecordingComponent(std::string name, const ShutdownPhase phase, std::vector<std::string>& calls,
                       StartBehavior start_behavior = StartBehavior::succeed,
                       JoinBehavior join_behavior = JoinBehavior::succeed)
        : name_(std::move(name)), phase_(phase), calls_(calls), start_behavior_(start_behavior),
          join_behavior_(join_behavior)
    {
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return name_;
    }

    [[nodiscard]] ShutdownPhase shutdown_phase() const noexcept override
    {
        return phase_;
    }

    [[nodiscard]] Result<void> start(const std::stop_token startup_stop_token) override
    {
        calls_.push_back("start:" + name_);
        entered_.store(true, std::memory_order_release);
        if (start_behavior_ == StartBehavior::fail)
        {
            return Result<void>::failure(
                paperbreak::make_error("CAMERA_OPEN_FAILED", paperbreak::Severity::error,
                                       "injected", "test", "test.start", true));
        }
        if (start_behavior_ == StartBehavior::throw_exception)
        {
            throw std::runtime_error{"injected start exception"};
        }
        if (start_behavior_ == StartBehavior::wait_for_cancel)
        {
            std::mutex mutex;
            std::condition_variable_any condition;
            std::unique_lock lock{mutex};
            static_cast<void>(condition.wait(lock, startup_stop_token, [] { return false; }));
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> request_stop(const StopReason) override
    {
        calls_.push_back("request:" + name_);
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> join(const std::chrono::steady_clock::time_point deadline) override
    {
        calls_.push_back("join:" + name_);
        if (join_behavior_ == JoinBehavior::wait_until_deadline)
        {
            std::this_thread::sleep_until(deadline);
        }
        if (join_behavior_ == JoinBehavior::throw_exception)
        {
            throw std::runtime_error{"injected join exception"};
        }
        return Result<void>::success();
    }

    [[nodiscard]] bool entered() const noexcept
    {
        return entered_.load(std::memory_order_acquire);
    }

  private:
    std::string name_;
    ShutdownPhase phase_;
    std::vector<std::string>& calls_;
    StartBehavior start_behavior_;
    JoinBehavior join_behavior_;
    std::atomic_bool entered_{false};
};

std::unique_ptr<RecordingComponent> component(
    std::string name, const ShutdownPhase phase, std::vector<std::string>& calls,
    const StartBehavior start_behavior = StartBehavior::succeed,
    const JoinBehavior join_behavior = JoinBehavior::succeed)
{
    return std::make_unique<RecordingComponent>(std::move(name), phase, calls, start_behavior,
                                                join_behavior);
}

} // namespace

TEST(ServiceRuntime, StartsInRegistrationOrderAndStopsInPhaseOrder)
{
    std::vector<std::string> calls;
    std::vector<std::unique_ptr<ILifecycleComponent>> components;
    components.push_back(component("ipc", ShutdownPhase::ipc, calls));
    components.push_back(component("config-a", ShutdownPhase::configuration, calls));
    components.push_back(component("logging", ShutdownPhase::logging, calls));
    components.push_back(component("config-b", ShutdownPhase::configuration, calls));
    components.push_back(component("event", ShutdownPhase::event, calls));
    components.push_back(component("processing", ShutdownPhase::processing, calls));
    components.push_back(component("acquisition", ShutdownPhase::acquisition, calls));
    components.push_back(component("uplink", ShutdownPhase::uplink, calls));
    components.push_back(component("monitoring", ShutdownPhase::monitoring, calls));
    paperbreak::service::ServiceRuntime runtime{std::move(components)};

    ASSERT_TRUE(runtime.start());
    runtime.request_stop(StopReason::service_stop);
    ASSERT_TRUE(runtime.shutdown());

    const std::vector<std::string> expected{
        "start:ipc",          "start:config-a",   "start:logging",     "start:config-b",
        "start:event",        "start:processing", "start:acquisition", "start:uplink",
        "start:monitoring",   "request:config-b", "request:config-a",  "request:acquisition",
        "request:processing", "request:event",    "request:uplink",    "request:monitoring",
        "request:ipc",        "request:logging",  "join:config-b",     "join:config-a",
        "join:acquisition",   "join:processing",  "join:event",        "join:uplink",
        "join:monitoring",    "join:ipc",         "join:logging"};
    EXPECT_EQ(calls, expected);
    EXPECT_EQ(runtime.state(), paperbreak::service::ServiceState::stopped);
}

TEST(ServiceRuntime, RepeatedOperationsAreIdempotentAndFirstStopReasonWins)
{
    std::vector<std::string> calls;
    std::vector<std::unique_ptr<ILifecycleComponent>> components;
    components.push_back(component("config", ShutdownPhase::configuration, calls));
    paperbreak::service::ServiceRuntime runtime{std::move(components)};

    ASSERT_TRUE(runtime.start());
    ASSERT_TRUE(runtime.start());
    runtime.request_stop(StopReason::console_interrupt);
    runtime.request_stop(StopReason::system_shutdown);
    ASSERT_TRUE(runtime.shutdown());
    ASSERT_TRUE(runtime.shutdown());

    EXPECT_EQ(runtime.stop_reason(), StopReason::console_interrupt);
    EXPECT_EQ(calls, (std::vector<std::string>{"start:config", "request:config", "join:config"}));
}

TEST(ServiceRuntime, CancelsStartupAndRollsBackEveryStartedComponent)
{
    std::vector<std::string> calls;
    std::vector<std::unique_ptr<ILifecycleComponent>> components;
    components.push_back(component("ready", ShutdownPhase::configuration, calls));
    auto waiting =
        component("waiting", ShutdownPhase::acquisition, calls, StartBehavior::wait_for_cancel);
    RecordingComponent* waiting_pointer = waiting.get();
    components.push_back(std::move(waiting));
    paperbreak::service::ServiceRuntime runtime{std::move(components)};

    auto future = std::async(std::launch::async, [&runtime] { return runtime.start(); });
    const auto wait_limit = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!waiting_pointer->entered() && std::chrono::steady_clock::now() < wait_limit)
    {
        std::this_thread::yield();
    }
    ASSERT_TRUE(waiting_pointer->entered());
    runtime.request_stop(StopReason::system_shutdown);

    const auto result = future.get();
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), paperbreak::service::StartOutcome::cancelled);
    EXPECT_EQ(runtime.state(), paperbreak::service::ServiceState::stopped);
    EXPECT_EQ(calls, (std::vector<std::string>{"start:ready", "start:waiting", "request:waiting",
                                               "request:ready", "join:waiting", "join:ready"}));
}

TEST(ServiceRuntime, WrapsStartupFailureAndRollsBackInReverseOrder)
{
    std::vector<std::string> calls;
    std::vector<std::unique_ptr<ILifecycleComponent>> components;
    components.push_back(component("first", ShutdownPhase::configuration, calls));
    components.push_back(component("second", ShutdownPhase::acquisition, calls));
    components.push_back(component("failure", ShutdownPhase::event, calls, StartBehavior::fail));
    paperbreak::service::ServiceRuntime runtime{std::move(components)};

    const auto result = runtime.start();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "SYS_SERVICE_START_FAILED");
    EXPECT_EQ(runtime.state(), paperbreak::service::ServiceState::failed);
    EXPECT_EQ(calls, (std::vector<std::string>{"start:first", "start:second", "start:failure",
                                               "request:second", "request:first", "join:second",
                                               "join:first"}));
}

TEST(ServiceRuntime, ConvertsComponentExceptionsToStableStartupErrors)
{
    std::vector<std::string> calls;
    std::vector<std::unique_ptr<ILifecycleComponent>> components;
    components.push_back(
        component("throws", ShutdownPhase::configuration, calls, StartBehavior::throw_exception));
    paperbreak::service::ServiceRuntime runtime{std::move(components)};

    const auto result = runtime.start();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "SYS_SERVICE_START_FAILED");
}

TEST(ServiceRuntime, ReportsTheComponentThatExhaustsTheSharedDeadline)
{
    std::vector<std::string> calls;
    std::vector<std::unique_ptr<ILifecycleComponent>> components;
    components.push_back(component("slow", ShutdownPhase::configuration, calls,
                                   StartBehavior::succeed, JoinBehavior::wait_until_deadline));
    paperbreak::service::RuntimeOptions options;
    options.shutdown_timeout = std::chrono::milliseconds{10};
    paperbreak::service::ServiceRuntime runtime{std::move(components), options};
    ASSERT_TRUE(runtime.start());

    runtime.request_stop(StopReason::service_stop);
    const auto result = runtime.shutdown();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "SYS_SHUTDOWN_TIMEOUT");
    EXPECT_EQ(runtime.state(), paperbreak::service::ServiceState::failed);
}

TEST(ServiceRuntime, CatchesExceptionsDuringJoin)
{
    std::vector<std::string> calls;
    std::vector<std::unique_ptr<ILifecycleComponent>> components;
    components.push_back(component("throws", ShutdownPhase::configuration, calls,
                                   StartBehavior::succeed, JoinBehavior::throw_exception));
    paperbreak::service::ServiceRuntime runtime{std::move(components)};
    ASSERT_TRUE(runtime.start());

    runtime.request_stop(StopReason::service_stop);
    const auto result = runtime.shutdown();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "SYS_INTERNAL_ERROR");
    EXPECT_EQ(runtime.state(), paperbreak::service::ServiceState::failed);
}
