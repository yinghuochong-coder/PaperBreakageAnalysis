#include "paperbreak/camera/mock_camera.hpp"
#include "paperbreak/service/camera_startup.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
using namespace std::chrono_literals;

class ScriptedProvider final : public paperbreak::camera::ICameraProvider
{
  public:
    ScriptedProvider(std::unique_ptr<paperbreak::camera::mock::MockCameraProvider> inner,
                     std::unordered_map<std::string, std::size_t> failures)
        : inner_(std::move(inner)), failures_(std::move(failures))
    {
    }

    [[nodiscard]] paperbreak::Result<std::vector<paperbreak::camera::CameraDeviceDescriptor>>
    enumerate_devices() override
    {
        return inner_->enumerate_devices();
    }

    [[nodiscard]] paperbreak::Result<std::unique_ptr<paperbreak::camera::ICameraDevice>>
    create_device(const std::string_view serial_number) override
    {
        create_calls_.fetch_add(1U, std::memory_order_relaxed);
        {
            std::scoped_lock lock{mutex_};
            auto found = failures_.find(std::string{serial_number});
            if (found != failures_.end() && found->second > 0U)
            {
                --found->second;
                return paperbreak::Result<std::unique_ptr<paperbreak::camera::ICameraDevice>>::
                    failure(paperbreak::camera::make_camera_error(
                        paperbreak::camera::CameraErrorKind::open_failed, "注入的启动连接失败",
                        "camera.test.createDevice", std::string{serial_number}));
            }
        }
        return inner_->create_device(serial_number);
    }

    [[nodiscard]] std::size_t create_calls() const noexcept
    {
        return create_calls_.load(std::memory_order_relaxed);
    }

  private:
    std::unique_ptr<paperbreak::camera::mock::MockCameraProvider> inner_;
    std::unordered_map<std::string, std::size_t> failures_;
    std::atomic_size_t create_calls_{};
    std::mutex mutex_;
};

paperbreak::camera::mock::MockCameraConfig mock_camera(const std::string& serial)
{
    return {.descriptor = {.model_name = "Mock",
                           .serial_number = serial,
                           .ip_address = "127.0.0.1",
                           .network_interface = "loopback"},
            .width = 64U,
            .height = 48U,
            .frame_rate = 30.0};
}

paperbreak::config::CameraConfig camera_configuration(const std::string& id,
                                                      const std::string& serial,
                                                      const bool enabled = true)
{
    return {.id = id,
            .enabled = enabled,
            .serial_number = serial,
            .location = "测试",
            .exposure_us = 1000.0,
            .exposure_auto_mode = paperbreak::config::ExposureAutoMode::off,
            .gain_db = 0.0,
            .frame_rate = 30.0,
            .roi = {64U, 48U, 0U, 0U},
            .reverse_x = false,
            .reverse_y = false,
            .pixel_format = paperbreak::config::PixelFormat::mono8,
            .trigger_mode = paperbreak::config::TriggerMode::continuous,
            .trigger_source = "",
            .trigger_delay_us = 0U,
            .packet_size_bytes = 1500U,
            .inter_packet_delay_ns = 0U};
}

std::pair<std::shared_ptr<paperbreak::camera::CameraControlRuntime>, ScriptedProvider*>
make_runtime(std::vector<paperbreak::camera::mock::MockCameraConfig> cameras,
             std::unordered_map<std::string, std::size_t> failures)
{
    auto mock = paperbreak::camera::mock::MockCameraProvider::create(std::move(cameras));
    EXPECT_TRUE(mock);
    if (!mock)
        return {};
    auto scripted =
        std::make_unique<ScriptedProvider>(std::move(mock).value(), std::move(failures));
    auto* observer = scripted.get();
    std::shared_ptr<paperbreak::camera::ICameraProvider> provider{std::move(scripted)};
    return {std::make_shared<paperbreak::camera::CameraControlRuntime>(std::move(provider)),
            observer};
}

paperbreak::config::AcquisitionConfig startup_policy(const bool enabled = true)
{
    paperbreak::config::AcquisitionConfig result;
    result.auto_start = enabled;
    result.startup_retry_interval_ms = 1U;
    result.startup_retry_count = 3U;
    return result;
}

} // namespace

TEST(CameraStartup, RetriesThenStartsAndDisconnectsManagedCamera)
{
    auto [runtime, provider] = make_runtime({mock_camera("MOCK-01")}, {{"MOCK-01", 2U}});
    ASSERT_TRUE(runtime);
    ASSERT_NE(provider, nullptr);
    paperbreak::service::CameraStartupLifecycleComponent component{
        runtime, {camera_configuration("CAM01", "MOCK-01")}, startup_policy(), {}};

    ASSERT_TRUE(component.start({}));
    EXPECT_EQ(provider->create_calls(), 3U);
    const auto started = runtime->get("CAM01", "MOCK-01");
    ASSERT_TRUE(started);
    EXPECT_EQ(started.value().state, paperbreak::camera::CameraControlState::acquiring);

    ASSERT_TRUE(component.request_stop(paperbreak::service::StopReason::service_stop));
    ASSERT_TRUE(component.join(std::chrono::steady_clock::now() + 1s));
    const auto stopped = runtime->get("CAM01", "MOCK-01");
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped.value().state, paperbreak::camera::CameraControlState::disconnected);
}

TEST(CameraStartup, IsolatesFailedAndDisabledSlots)
{
    auto [runtime, provider] =
        make_runtime({mock_camera("MOCK-01"), mock_camera("MOCK-02"), mock_camera("MOCK-03")},
                     {{"MOCK-01", 20U}});
    ASSERT_TRUE(runtime);
    paperbreak::service::CameraStartupLifecycleComponent component{
        runtime,
        {camera_configuration("CAM01", "MOCK-01"), camera_configuration("CAM02", "MOCK-02"),
         camera_configuration("CAM03", "MOCK-03", false)},
        startup_policy(),
        {}};

    ASSERT_TRUE(component.start({}));
    EXPECT_EQ(provider->create_calls(), 5U);
    EXPECT_EQ(runtime->get("CAM01", "MOCK-01").value().state,
              paperbreak::camera::CameraControlState::disconnected);
    EXPECT_EQ(runtime->get("CAM02", "MOCK-02").value().state,
              paperbreak::camera::CameraControlState::acquiring);
    EXPECT_EQ(runtime->get("CAM03", "MOCK-03").value().state,
              paperbreak::camera::CameraControlState::disconnected);
}

TEST(CameraStartup, DisabledPolicyDoesNotTouchProvider)
{
    auto [runtime, provider] = make_runtime({mock_camera("MOCK-01")}, {});
    ASSERT_TRUE(runtime);
    paperbreak::service::CameraStartupLifecycleComponent component{
        runtime, {camera_configuration("CAM01", "MOCK-01")}, startup_policy(false), {}};

    ASSERT_TRUE(component.start({}));
    EXPECT_EQ(provider->create_calls(), 0U);
}

TEST(CameraStartup, StopTokenCancelsRetryWait)
{
    auto [runtime, provider] = make_runtime({mock_camera("MOCK-01")}, {{"MOCK-01", 20U}});
    ASSERT_TRUE(runtime);
    auto policy = startup_policy();
    policy.startup_retry_interval_ms = 60000U;
    paperbreak::service::CameraStartupLifecycleComponent component{
        runtime, {camera_configuration("CAM01", "MOCK-01")}, policy, {}};
    std::atomic_bool completed{};
    std::jthread starter{[&](const std::stop_token token) {
        static_cast<void>(component.start(token));
        completed.store(true, std::memory_order_release);
    }};

    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (provider->create_calls() == 0U && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_GT(provider->create_calls(), 0U);
    starter.request_stop();
    starter.join();

    EXPECT_TRUE(completed.load(std::memory_order_acquire));
    EXPECT_EQ(provider->create_calls(), 1U);
}
