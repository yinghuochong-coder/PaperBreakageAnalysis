#include "paperbreak/algorithm/detector_host.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using namespace std::chrono_literals;
using paperbreak::Result;
using paperbreak::Severity;
using paperbreak::algorithm::DetectionResult;
using paperbreak::algorithm::DetectorConfig;
using paperbreak::algorithm::DetectorHost;
using paperbreak::algorithm::DetectorInfo;
using paperbreak::algorithm::DetectorPluginRegistry;
using paperbreak::algorithm::IBreakDetector;
using paperbreak::camera::FrameBuffer;
using paperbreak::camera::FramePacket;
using paperbreak::camera::FrameView;

paperbreak::Error scripted_error(std::string operation)
{
    return paperbreak::make_error("ALGORITHM_SCRIPTED_FAILURE", Severity::error, "脚本化检测器故障",
                                  "algorithm", std::move(operation));
}

FrameView make_frame(const std::uint64_t sequence_number)
{
    const std::vector<std::uint8_t> pixels{1U, 2U, 3U, 4U};
    auto buffer = std::make_shared<FrameBuffer>(pixels.size());
    std::transform(pixels.begin(), pixels.end(), buffer->writable_bytes().begin(),
                   [](const std::uint8_t value) { return static_cast<std::byte>(value); });
    if (!buffer->set_size(pixels.size()))
        throw std::runtime_error("test frame buffer size failed");

    const FramePacket packet{.camera_id = "CAM01",
                             .camera_frame_number = sequence_number + 100U,
                             .sequence_number = sequence_number,
                             .received_monotonic_time = paperbreak::camera::MonotonicTime{} +
                                                        std::chrono::milliseconds{sequence_number},
                             .received_wall_clock_time = paperbreak::camera::WallClockTime{} +
                                                         std::chrono::milliseconds{sequence_number},
                             .geometry = {.width = 2U, .height = 2U, .stride = 2U},
                             .pixel_format = paperbreak::camera::PixelFormat::mono8,
                             .buffer = std::move(buffer)};
    auto view = paperbreak::camera::make_frame_view(packet);
    if (!view)
        throw std::runtime_error("test frame view failed");
    return std::move(view).value();
}

DetectorConfig make_config(const std::uint64_t revision,
                           const std::chrono::milliseconds timeout = 100ms)
{
    return {.plugin_id = "scripted",
            .camera_id = "CAM01",
            .revision = revision,
            .processing_timeout = timeout};
}

struct Script final
{
    bool fail_initialize{};
    bool throw_initialize{};
    bool fail_update{};
    bool throw_update{};
    bool fail_process{};
    bool throw_process{};
    bool throw_unknown_process{};
    bool fail_reset{};
    bool throw_reset{};
    std::chrono::milliseconds process_delay{};
    std::uint64_t created{};
    std::uint64_t initialized{};
    std::uint64_t updated{};
    std::uint64_t reset{};
};

class ScriptedDetector final : public IBreakDetector
{
  public:
    explicit ScriptedDetector(std::shared_ptr<Script> script) : script_(std::move(script))
    {
        ++script_->created;
    }

    Result<void> initialize(const DetectorConfig& config) override
    {
        ++script_->initialized;
        config_ = config;
        if (script_->throw_initialize)
            throw std::runtime_error("initialize exception");
        if (script_->fail_initialize)
            return Result<void>::failure(scripted_error("scripted.initialize"));
        initialized_ = true;
        return Result<void>::success();
    }

    Result<DetectionResult> process(const FrameView& frame) override
    {
        if (!initialized_)
            return Result<DetectionResult>::failure(scripted_error("scripted.notReady"));
        if (script_->process_delay.count() != 0)
            std::this_thread::sleep_for(script_->process_delay);
        if (script_->throw_process)
            throw std::runtime_error("process exception");
        if (script_->throw_unknown_process)
            throw 7;
        if (script_->fail_process)
            return Result<DetectionResult>::failure(scripted_error("scripted.process"));
        return Result<DetectionResult>::success(
            {.camera_id = frame.camera_id(),
             .sequence_number = frame.sequence_number(),
             .camera_frame_number = frame.camera_frame_number(),
             .monotonic_time = frame.received_monotonic_time(),
             .wall_clock_time = frame.received_wall_clock_time(),
             .reason = "revision-" + std::to_string(config_.revision)});
    }

    Result<void> update_config(const DetectorConfig& config) override
    {
        ++script_->updated;
        config_ = config;
        if (script_->throw_update)
            throw std::runtime_error("update exception");
        if (script_->fail_update)
            return Result<void>::failure(scripted_error("scripted.update"));
        return Result<void>::success();
    }

    Result<void> reset() override
    {
        ++script_->reset;
        if (script_->throw_reset)
            throw std::runtime_error("reset exception");
        if (script_->fail_reset)
            return Result<void>::failure(scripted_error("scripted.reset"));
        return Result<void>::success();
    }

    DetectorInfo info() const override
    {
        return {.plugin_id = "scripted",
                .display_name = "Scripted detector",
                .implementation_version = "6.1.0-test",
                .model_version = "none",
                .supports_hot_update = true,
                .prototype_only = true};
    }

  private:
    std::shared_ptr<Script> script_;
    DetectorConfig config_;
    bool initialized_{};
};

void register_scripted(DetectorPluginRegistry& registry, const std::shared_ptr<Script>& script)
{
    auto registered = registry.register_plugin("scripted", [script] {
        std::unique_ptr<IBreakDetector> detector = std::make_unique<ScriptedDetector>(script);
        return Result<std::unique_ptr<IBreakDetector>>::success(std::move(detector));
    });
    if (!registered)
        throw std::runtime_error("test plugin registration failed");
}

TEST(AlgorithmDetectorRegistry, RejectsInvalidDuplicateUnknownAndFailingFactories)
{
    DetectorPluginRegistry registry;
    EXPECT_FALSE(registry.register_plugin("", {}));

    auto script = std::make_shared<Script>();
    register_scripted(registry, script);
    EXPECT_EQ(registry.size(), 1U);
    EXPECT_FALSE(registry.register_plugin("scripted", [script] {
        std::unique_ptr<IBreakDetector> detector = std::make_unique<ScriptedDetector>(script);
        return Result<std::unique_ptr<IBreakDetector>>::success(std::move(detector));
    }));

    auto missing = registry.create("missing");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().business_code, "ALGORITHM_PLUGIN_LOAD_FAILED");

    ASSERT_TRUE(registry.register_plugin("factory-failure", [] {
        return Result<std::unique_ptr<IBreakDetector>>::failure(scripted_error("scripted.factory"));
    }));
    auto failed = registry.create("factory-failure");
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().business_code, "ALGORITHM_SCRIPTED_FAILURE");

    ASSERT_TRUE(registry.register_plugin("factory-exception",
                                         []() -> Result<std::unique_ptr<IBreakDetector>> {
                                             throw std::runtime_error("factory exception");
                                         }));
    auto exception = registry.create("factory-exception");
    ASSERT_FALSE(exception);
    EXPECT_EQ(exception.error().business_code, "ALGORITHM_PLUGIN_EXCEPTION");
}

TEST(AlgorithmDetectorHost, LoadFailureLeavesExistingDetectorActive)
{
    DetectorPluginRegistry registry;
    auto script = std::make_shared<Script>();
    register_scripted(registry, script);
    DetectorHost host{registry};

    ASSERT_TRUE(host.load(make_config(1U)));
    auto info = host.info();
    ASSERT_TRUE(info);
    EXPECT_EQ(info.value().implementation_version, "6.1.0-test");

    script->fail_initialize = true;
    auto failed = host.load(make_config(2U));
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().business_code, "ALGORITHM_SCRIPTED_FAILURE");
    ASSERT_NE(host.active_config(), nullptr);
    EXPECT_EQ(host.active_config()->revision, 1U);

    script->fail_initialize = false;
    auto detection = host.process(make_frame(1U));
    ASSERT_TRUE(detection);
    EXPECT_EQ(detection.value().reason, "revision-1");
    EXPECT_EQ(detection.value().detector_version, "6.1.0-test");
}

TEST(AlgorithmDetectorHost, HotUpdateUsesCandidateAndRollsBackFailureOrException)
{
    DetectorPluginRegistry registry;
    auto script = std::make_shared<Script>();
    register_scripted(registry, script);
    DetectorHost host{registry};
    ASSERT_TRUE(host.load(make_config(1U)));

    script->fail_update = true;
    auto failed = host.update_config(make_config(2U));
    ASSERT_FALSE(failed);
    ASSERT_NE(host.active_config(), nullptr);
    EXPECT_EQ(host.active_config()->revision, 1U);
    auto old_detection = host.process(make_frame(1U));
    ASSERT_TRUE(old_detection);
    EXPECT_EQ(old_detection.value().reason, "revision-1");

    script->fail_update = false;
    script->throw_update = true;
    auto exception = host.update_config(make_config(2U));
    ASSERT_FALSE(exception);
    EXPECT_EQ(exception.error().business_code, "ALGORITHM_PLUGIN_EXCEPTION");
    EXPECT_EQ(host.active_config()->revision, 1U);

    script->throw_update = false;
    ASSERT_TRUE(host.update_config(make_config(2U)));
    EXPECT_EQ(host.active_config()->revision, 2U);
    auto updated_detection = host.process(make_frame(2U));
    ASSERT_TRUE(updated_detection);
    EXPECT_EQ(updated_detection.value().reason, "revision-2");
    EXPECT_EQ(host.metrics().successful_config_updates, 1U);

    EXPECT_FALSE(host.update_config(make_config(2U)));
    EXPECT_EQ(host.active_config()->revision, 2U);
}

TEST(AlgorithmDetectorHost, IsolatesProcessFailuresAndReportsSoftTimeoutMetrics)
{
    DetectorPluginRegistry registry;
    auto script = std::make_shared<Script>();
    register_scripted(registry, script);
    DetectorHost host{registry};
    ASSERT_TRUE(host.load(make_config(1U, 1ms)));

    script->throw_process = true;
    auto exception = host.process(make_frame(1U));
    ASSERT_FALSE(exception);
    EXPECT_EQ(exception.error().business_code, "ALGORITHM_PLUGIN_EXCEPTION");

    script->throw_process = false;
    script->throw_unknown_process = true;
    auto unknown = host.process(make_frame(2U));
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().business_code, "ALGORITHM_PLUGIN_EXCEPTION");

    script->throw_unknown_process = false;
    script->process_delay = 5ms;
    auto timeout = host.process(make_frame(3U));
    ASSERT_FALSE(timeout);
    EXPECT_EQ(timeout.error().business_code, "ALGORITHM_PROCESS_TIMEOUT");

    const auto metrics = host.metrics();
    EXPECT_EQ(metrics.process_calls, 3U);
    EXPECT_EQ(metrics.process_successes, 0U);
    EXPECT_EQ(metrics.process_failures, 3U);
    EXPECT_EQ(metrics.process_timeouts, 1U);
    EXPECT_GE(metrics.last_processing_time, 1ms);
    EXPECT_GE(metrics.maximum_processing_time, metrics.last_processing_time);
}

TEST(AlgorithmDetectorHost, ResetFailuresAreContainedAndHostRemainsUsable)
{
    DetectorPluginRegistry registry;
    auto script = std::make_shared<Script>();
    register_scripted(registry, script);
    DetectorHost host{registry};

    auto not_ready = host.reset();
    ASSERT_FALSE(not_ready);
    EXPECT_EQ(not_ready.error().business_code, "ALGORITHM_NOT_READY");
    ASSERT_TRUE(host.load(make_config(1U)));
    ASSERT_TRUE(host.reset());

    script->fail_reset = true;
    auto failed = host.reset();
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().business_code, "ALGORITHM_SCRIPTED_FAILURE");

    script->fail_reset = false;
    script->throw_reset = true;
    auto exception = host.reset();
    ASSERT_FALSE(exception);
    EXPECT_EQ(exception.error().business_code, "ALGORITHM_PLUGIN_EXCEPTION");

    script->throw_reset = false;
    auto detection = host.process(make_frame(1U));
    EXPECT_TRUE(detection);
    EXPECT_EQ(host.metrics().reset_calls, 3U);
}

TEST(AlgorithmDetectorHost, RejectsInvalidBoundedConfigurationBeforeFactoryCall)
{
    DetectorPluginRegistry registry;
    auto script = std::make_shared<Script>();
    register_scripted(registry, script);
    DetectorHost host{registry};

    auto invalid = make_config(1U);
    invalid.parameters = {{.name = "threshold", .value = 0.5},
                          {.name = "threshold", .value = 0.75}};
    auto result = host.load(invalid);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "SYS_CONFIG_INVALID");
    EXPECT_EQ(script->created, 0U);
}

} // namespace
