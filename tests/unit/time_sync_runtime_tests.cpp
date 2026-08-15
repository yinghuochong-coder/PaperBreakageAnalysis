#include "paperbreak/time/time_sync_runtime.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using namespace paperbreak;
using namespace paperbreak::time;

Error unavailable_error()
{
    return make_error("TIME_PROBE_UNAVAILABLE", Severity::warning, "unavailable", "test",
                      "test.probe", true);
}

SystemClockProbeSample system_sample(const ClockSource source = ClockSource::ntp,
                                     const std::int64_t monotonic_ns = 1'000'000'000,
                                     const std::int64_t utc_ns = 10'000'000'000)
{
    return {.clock_source = source,
            .sync_state = SyncState::synced,
            .sample_monotonic_ns = monotonic_ns,
            .sample_utc_ns = utc_ns,
            .offset_ns = 2'000,
            .uncertainty_ns = 100'000,
            .maximum_observed_offset_ns = 3'000,
            .last_synchronized_utc_ns = utc_ns - 1'000,
            .grandmaster_identity = "GM-01",
            .last_error_code = std::nullopt};
}

CameraClockProbeSample camera_sample(const std::uint64_t ticks = 1'000U,
                                     const std::int64_t monotonic_ns = 1'000'000'000,
                                     const std::int64_t utc_ns = 10'000'000'000,
                                     const bool hardware_ptp = false)
{
    return {.camera_timestamp_ticks = ticks,
            .camera_timestamp_frequency_hz = 1'000U,
            .sample_monotonic_ns = monotonic_ns,
            .sample_utc_ns = utc_ns,
            .hardware_ptp_synchronized = hardware_ptp,
            .offset_ns = 5'000,
            .uncertainty_ns = 200'000,
            .maximum_observed_offset_ns = 7'000,
            .last_synchronized_utc_ns = utc_ns - 2'000,
            .grandmaster_identity =
                hardware_ptp ? std::optional<std::string>{"CAM-GM"} : std::nullopt,
            .last_error_code = std::nullopt};
}

class ManualClock final : public IRuntimeClock
{
  public:
    explicit ManualClock(const RuntimeClockReading initial) : reading_(initial) {}

    RuntimeClockReading read() noexcept override
    {
        std::lock_guard lock{mutex_};
        return reading_;
    }

    void set(const RuntimeClockReading reading)
    {
        std::lock_guard lock{mutex_};
        reading_ = reading;
    }

  private:
    std::mutex mutex_;
    RuntimeClockReading reading_;
};

template <typename Sample> using ProbeStep = std::variant<Sample, Error>;

class ScriptedSystemProbe final : public ISystemClockProbe
{
  public:
    explicit ScriptedSystemProbe(std::vector<ProbeStep<SystemClockProbeSample>> steps)
        : steps_(std::move(steps))
    {
    }

    Result<SystemClockProbeSample> sample(std::stop_token,
                                          std::chrono::steady_clock::time_point) override
    {
        std::lock_guard lock{mutex_};
        const auto& step = steps_[std::min(index_, steps_.size() - 1U)];
        ++index_;
        if (std::holds_alternative<SystemClockProbeSample>(step))
            return Result<SystemClockProbeSample>::success(std::get<SystemClockProbeSample>(step));
        return Result<SystemClockProbeSample>::failure(std::get<Error>(step));
    }

  private:
    std::mutex mutex_;
    std::vector<ProbeStep<SystemClockProbeSample>> steps_;
    std::size_t index_{};
};

class ScriptedCameraProbe : public ICameraClockProbe
{
  public:
    ScriptedCameraProbe(std::string camera_id, std::vector<ProbeStep<CameraClockProbeSample>> steps)
        : camera_id_(std::move(camera_id)), steps_(std::move(steps))
    {
    }

    std::string_view camera_id() const noexcept override
    {
        return camera_id_;
    }

    Result<CameraClockProbeSample> sample(std::stop_token,
                                          std::chrono::steady_clock::time_point) override
    {
        std::lock_guard lock{mutex_};
        const auto& step = steps_[std::min(index_, steps_.size() - 1U)];
        ++index_;
        if (std::holds_alternative<CameraClockProbeSample>(step))
            return Result<CameraClockProbeSample>::success(std::get<CameraClockProbeSample>(step));
        return Result<CameraClockProbeSample>::failure(std::get<Error>(step));
    }

  private:
    std::string camera_id_;
    std::mutex mutex_;
    std::vector<ProbeStep<CameraClockProbeSample>> steps_;
    std::size_t index_{};
};

class GateCameraProbe final : public ICameraClockProbe
{
  public:
    explicit GateCameraProbe(const bool ignore_stop = false) : ignore_stop_(ignore_stop) {}

    std::string_view camera_id() const noexcept override
    {
        return "CAM01";
    }

    Result<CameraClockProbeSample> sample(const std::stop_token stop_token,
                                          std::chrono::steady_clock::time_point) override
    {
        const auto call = calls_.fetch_add(1U, std::memory_order_relaxed);
        if (call == 0U)
            return Result<CameraClockProbeSample>::success(camera_sample());

        {
            std::lock_guard lock{mutex_};
            entered_ = true;
        }
        condition_.notify_all();
        if (ignore_stop_)
        {
            std::this_thread::sleep_for(150ms);
        }
        else
        {
            std::stop_callback callback{stop_token, [this] { condition_.notify_all(); }};
            std::unique_lock lock{mutex_};
            condition_.wait(
                lock, [this, &stop_token] { return released_ || stop_token.stop_requested(); });
        }
        if (stop_token.stop_requested())
            return Result<CameraClockProbeSample>::failure(unavailable_error());
        return Result<CameraClockProbeSample>::success(camera_sample(2'000U));
    }

    bool wait_until_entered(const std::chrono::steady_clock::time_point deadline)
    {
        std::unique_lock lock{mutex_};
        return condition_.wait_until(lock, deadline, [this] { return entered_; });
    }

  private:
    bool ignore_stop_{};
    std::atomic<std::uint64_t> calls_{};
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_{};
    bool released_{};
};

template <typename Predicate>
bool wait_until(Predicate predicate, const std::chrono::milliseconds timeout = 1s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

std::unique_ptr<TimeSyncRuntime> create_runtime(
    std::unique_ptr<ISystemClockProbe> system_probe,
    std::vector<std::unique_ptr<ICameraClockProbe>> camera_probes,
    std::unique_ptr<IRuntimeClock> clock, TimeSyncRuntimeOptions options = {})
{
    options.sample_period = 1h;
    options.probe_timeout = 1s;
    options.first_sample_timeout = 1s;
    auto created = TimeSyncRuntime::create(std::move(system_probe), std::move(camera_probes),
                                           std::move(clock), options);
    EXPECT_TRUE(created);
    if (!created)
        return {};
    return std::move(created).value();
}

void stop_runtime(TimeSyncRuntime& runtime)
{
    runtime.request_stop();
    EXPECT_TRUE(runtime.join(std::chrono::steady_clock::now() + 1s));
}
} // namespace

TEST(TimeSyncRuntimeSelection, HardwarePtpOutranksTheOsSource)
{
    auto clock = std::make_unique<ManualClock>(RuntimeClockReading{1'000'000'000, 10'000'000'000});
    std::vector<std::unique_ptr<ICameraClockProbe>> cameras;
    cameras.push_back(std::make_unique<ScriptedCameraProbe>(
        "CAM01", std::vector<ProbeStep<CameraClockProbeSample>>{
                     camera_sample(1'000U, 1'000'000'000, 10'000'000'000, true)}));
    auto runtime = create_runtime(
        std::make_unique<ScriptedSystemProbe>(
            std::vector<ProbeStep<SystemClockProbeSample>>{system_sample(ClockSource::ntp)}),
        std::move(cameras), std::move(clock));
    ASSERT_NE(runtime, nullptr);
    ASSERT_TRUE(runtime->start());

    const auto system = runtime->system_model();
    const auto camera = runtime->camera_model("CAM01");
    ASSERT_NE(system, nullptr);
    ASSERT_NE(camera, nullptr);
    EXPECT_EQ(system->clock_source, ClockSource::ntp);
    EXPECT_EQ(camera->clock_source, ClockSource::ptp_hardware);
    EXPECT_EQ(camera->sync_state, SyncState::synced);
    EXPECT_LT(system->model_revision, camera->model_revision);
    EXPECT_EQ(runtime->metrics().published_models, 2U);

    const auto mapped = runtime->utc_to_monotonic(10'500'000'000);
    ASSERT_TRUE(mapped);
    EXPECT_EQ(mapped.value().mapped_time_ns, 1'500'000'000);
    EXPECT_EQ(mapped.value().model.get(), system.get());
    stop_runtime(*runtime);
}

TEST(TimeSyncRuntimeSelection, SwitchesFromOsToOffsetThenReceiveWithoutRewritingHistory)
{
    auto clock = std::make_unique<ManualClock>(RuntimeClockReading{1'000'000'000, 10'000'000'000});
    auto* clock_control = clock.get();
    auto second_camera = camera_sample(2'000U, 2'000'000'000, 11'000'100'000);
    second_camera.offset_ns = 100'000;
    std::vector<std::unique_ptr<ICameraClockProbe>> cameras;
    cameras.push_back(std::make_unique<ScriptedCameraProbe>(
        "CAM01", std::vector<ProbeStep<CameraClockProbeSample>>{camera_sample(), second_camera,
                                                                unavailable_error()}));
    auto runtime = create_runtime(
        std::make_unique<ScriptedSystemProbe>(std::vector<ProbeStep<SystemClockProbeSample>>{
            system_sample(ClockSource::ptp_software), unavailable_error(), unavailable_error()}),
        std::move(cameras), std::move(clock));
    ASSERT_TRUE(runtime->start());

    const auto historical = runtime->camera_model("CAM01");
    ASSERT_NE(historical, nullptr);
    EXPECT_EQ(historical->clock_source, ClockSource::ptp_software);

    clock_control->set({2'000'000'000, 11'000'000'000});
    ASSERT_TRUE(runtime->request_refresh());
    ASSERT_TRUE(wait_until([&] { return runtime->metrics().sample_cycles >= 2U; }));
    const auto offset = runtime->camera_model("CAM01");
    ASSERT_NE(offset, nullptr);
    EXPECT_EQ(offset->clock_source, ClockSource::offset_model);
    EXPECT_EQ(offset->sync_state, SyncState::degraded);
    EXPECT_EQ(offset->offset_ns, 100'000);
    EXPECT_EQ(offset->last_error_code, "TIME_SYNC_DEGRADED");
    EXPECT_EQ(historical->clock_source, ClockSource::ptp_software);

    clock_control->set({3'000'000'000, 12'000'000'000});
    ASSERT_TRUE(runtime->request_refresh());
    ASSERT_TRUE(wait_until([&] { return runtime->metrics().sample_cycles >= 3U; }));
    const auto receive = runtime->camera_model("CAM01");
    ASSERT_NE(receive, nullptr);
    EXPECT_EQ(receive->clock_source, ClockSource::receive_clock);
    EXPECT_EQ(receive->last_error_code, "TIME_PROBE_UNAVAILABLE");
    EXPECT_GT(receive->model_revision, offset->model_revision);

    const auto frame = build_frame_time_metadata({}, {}, 3'100'000'000, 12'100'000'000, receive);
    EXPECT_EQ(frame.status, FrameTimeBuildStatus::corrected);
    EXPECT_EQ(frame.metadata.corrected_capture_utc_ns, 12'100'000'000);
    stop_runtime(*runtime);
}

TEST(TimeSyncRuntimeQuality, LatchesSystemTimeJumpAsP0Degradation)
{
    auto clock = std::make_unique<ManualClock>(RuntimeClockReading{1'000'000'000, 10'000'000'000});
    auto* clock_control = clock.get();
    std::vector<std::unique_ptr<ICameraClockProbe>> cameras;
    cameras.push_back(std::make_unique<ScriptedCameraProbe>(
        "CAM01", std::vector<ProbeStep<CameraClockProbeSample>>{
                     camera_sample(), camera_sample(2'000U, 2'000'000'000, 12'000'000'000),
                     camera_sample(3'000U, 3'000'000'000, 13'000'000'000)}));
    TimeSyncRuntimeOptions options;
    options.system_time_jump_threshold_ns = 100'000'000;
    auto runtime = create_runtime(
        std::make_unique<ScriptedSystemProbe>(std::vector<ProbeStep<SystemClockProbeSample>>{
            system_sample(ClockSource::ntp),
            system_sample(ClockSource::ntp, 2'000'000'000, 12'000'000'000),
            system_sample(ClockSource::ntp, 3'000'000'000, 13'000'000'000)}),
        std::move(cameras), std::move(clock), options);
    ASSERT_TRUE(runtime->start());

    clock_control->set({2'000'000'000, 12'000'000'000});
    ASSERT_TRUE(runtime->request_refresh());
    ASSERT_TRUE(wait_until([&] { return runtime->metrics().sample_cycles >= 2U; }));
    auto system = runtime->system_model();
    ASSERT_NE(system, nullptr);
    EXPECT_EQ(system->sync_state, SyncState::degraded);
    EXPECT_EQ(system->last_error_code, "SYS_TIME_JUMP_DETECTED");
    EXPECT_GE(system->uncertainty_ns.value_or(0), 1'000'000'000);

    clock_control->set({3'000'000'000, 13'000'000'000});
    ASSERT_TRUE(runtime->request_refresh());
    ASSERT_TRUE(wait_until([&] { return runtime->metrics().sample_cycles >= 3U; }));
    system = runtime->system_model();
    EXPECT_EQ(system->sync_state, SyncState::degraded);
    EXPECT_EQ(system->last_error_code, "SYS_TIME_JUMP_DETECTED");
    stop_runtime(*runtime);
}

TEST(TimeSyncRuntimeControl, BoundsRefreshQueueAndCancelsACooperativeProbe)
{
    auto gate = std::make_unique<GateCameraProbe>();
    auto* gate_control = gate.get();
    std::vector<std::unique_ptr<ICameraClockProbe>> cameras;
    cameras.push_back(std::move(gate));
    auto runtime = create_runtime(
        std::make_unique<ScriptedSystemProbe>(
            std::vector<ProbeStep<SystemClockProbeSample>>{system_sample()}),
        std::move(cameras),
        std::make_unique<ManualClock>(RuntimeClockReading{1'000'000'000, 10'000'000'000}));
    ASSERT_TRUE(runtime->start());
    ASSERT_TRUE(runtime->request_refresh());
    ASSERT_TRUE(gate_control->wait_until_entered(std::chrono::steady_clock::now() + 1s));

    for (std::size_t index = 0; index < time_sync_control_capacity; ++index)
        EXPECT_TRUE(runtime->request_refresh());
    const auto rejected = runtime->request_refresh();
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().business_code, "SYS_BUSY");
    EXPECT_EQ(runtime->metrics().control_depth, time_sync_control_capacity);
    EXPECT_EQ(runtime->metrics().control_high_watermark, time_sync_control_capacity);

    runtime->request_stop();
    EXPECT_TRUE(runtime->join(std::chrono::steady_clock::now() + 1s));
    EXPECT_EQ(runtime->state(), TimeSyncRuntimeState::stopped);
    EXPECT_EQ(runtime->metrics().rejected_refresh_requests, 1U);
}

TEST(TimeSyncRuntimeShutdown, ReportsDeadlineWhenAProbeViolatesCancellationContract)
{
    auto gate = std::make_unique<GateCameraProbe>(true);
    auto* gate_control = gate.get();
    std::vector<std::unique_ptr<ICameraClockProbe>> cameras;
    cameras.push_back(std::move(gate));
    auto runtime = create_runtime(
        std::make_unique<ScriptedSystemProbe>(
            std::vector<ProbeStep<SystemClockProbeSample>>{system_sample()}),
        std::move(cameras),
        std::make_unique<ManualClock>(RuntimeClockReading{1'000'000'000, 10'000'000'000}));
    ASSERT_TRUE(runtime->start());
    ASSERT_TRUE(runtime->request_refresh());
    ASSERT_TRUE(gate_control->wait_until_entered(std::chrono::steady_clock::now() + 1s));

    runtime->request_stop();
    const auto timed_out = runtime->join(std::chrono::steady_clock::now() + 10ms);
    ASSERT_FALSE(timed_out);
    EXPECT_EQ(timed_out.error().business_code, "SYS_SHUTDOWN_TIMEOUT");
    EXPECT_TRUE(runtime->join(std::chrono::steady_clock::now() + 1s));
}

TEST(TimeSyncRuntimeValidation, RejectsDuplicateAndExcessCameraProbeSets)
{
    std::vector<std::unique_ptr<ICameraClockProbe>> duplicates;
    duplicates.push_back(std::make_unique<ScriptedCameraProbe>(
        "CAM01", std::vector<ProbeStep<CameraClockProbeSample>>{camera_sample()}));
    duplicates.push_back(std::make_unique<ScriptedCameraProbe>(
        "CAM01", std::vector<ProbeStep<CameraClockProbeSample>>{camera_sample()}));
    auto duplicate = TimeSyncRuntime::create(
        std::make_unique<ScriptedSystemProbe>(
            std::vector<ProbeStep<SystemClockProbeSample>>{system_sample()}),
        std::move(duplicates));
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().business_code, "TIME_MODEL_INVALID");

    std::vector<std::unique_ptr<ICameraClockProbe>> excessive;
    for (std::size_t index = 0; index <= time_sync_camera_capacity; ++index)
        excessive.push_back(std::make_unique<ScriptedCameraProbe>(
            "CAM0" + std::to_string(index + 1U),
            std::vector<ProbeStep<CameraClockProbeSample>>{camera_sample()}));
    auto too_many = TimeSyncRuntime::create(
        std::make_unique<ScriptedSystemProbe>(
            std::vector<ProbeStep<SystemClockProbeSample>>{system_sample()}),
        std::move(excessive));
    EXPECT_FALSE(too_many);
}
