#include "paperbreak/camera/acquisition.hpp"
#include "paperbreak/camera/mock_camera.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using namespace paperbreak::camera;
using namespace paperbreak::camera::mock;

bool wait_until(const auto& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}
} // namespace

TEST(SimulationMockCamera, FourRoutesAcquireAndIsolateInjectedFailuresWithoutHardware)
{
    std::vector<MockCameraConfig> configurations;
    for (std::size_t index = 0U; index < 4U; ++index)
    {
        configurations.push_back(
            {.descriptor = {"PaperBreak Mock", "MOCK-000" + std::to_string(index + 1U),
                            "192.0.2." + std::to_string(index + 1U), "mock0"},
             .width = static_cast<std::uint32_t>(4U + index),
             .height = 4U,
             .frame_rate = 100.0,
             .pixel_format = index == 3U ? PixelFormat::mono10 : PixelFormat::mono8,
             .pattern = index == 2U ? MockFramePattern::noise : MockFramePattern::gradient,
             .random_seed = 100U + index,
             .maximum_payload_bytes = 64U});
    }
    auto provider_result = MockCameraProvider::create(std::move(configurations));
    ASSERT_TRUE(provider_result);
    auto provider = std::move(provider_result).value();

    std::array<std::unique_ptr<ICameraDevice>, 4U> devices;
    std::array<std::unique_ptr<FrameBufferPool>, 4U> pools;
    std::array<std::unique_ptr<AcquisitionQueue>, 4U> queues;
    std::array<std::unique_ptr<AcquisitionWorker>, 4U> workers;
    for (std::size_t index = 0U; index < devices.size(); ++index)
    {
        const auto serial = "MOCK-000" + std::to_string(index + 1U);
        auto created = provider->create_device(serial);
        ASSERT_TRUE(created);
        devices[index] = std::move(created).value();
        ASSERT_TRUE(devices[index]->connect());
        ASSERT_TRUE(devices[index]->start_acquisition());
        pools[index] = std::make_unique<FrameBufferPool>(4U, 64U);
        queues[index] = std::make_unique<AcquisitionQueue>(4U);
        workers[index] = std::make_unique<AcquisitionWorker>(
            *devices[index], *pools[index], *queues[index],
            AcquisitionWorkerOptions{.camera_id = "CAM0" + std::to_string(index + 1U),
                                     .receive_timeout = 20ms});
    }

    ASSERT_TRUE(
        provider->control("MOCK-0001").value().inject_fault({.kind = MockFaultKind::disconnect}));
    ASSERT_TRUE(
        provider->control("MOCK-0002").value().inject_fault({.kind = MockFaultKind::timeout}));
    ASSERT_TRUE(provider->control("MOCK-0003")
                    .value()
                    .inject_fault({.kind = MockFaultKind::drop_frame, .amount = 1U}));
    for (auto& worker : workers)
    {
        ASSERT_TRUE(worker->start());
    }

    ASSERT_TRUE(wait_until([&] { return workers[0]->snapshot().completed; }));
    EXPECT_EQ(workers[0]->snapshot().last_error->business_code, "CAMERA_DISCONNECTED");
    for (std::size_t index = 1U; index < queues.size(); ++index)
    {
        ASSERT_TRUE(wait_until([&] { return queues[index]->snapshot().depth > 0U; }));
    }

    const auto second = queues[1]->wait_pop({}, 0ms);
    const auto third = queues[2]->wait_pop({}, 0ms);
    const auto fourth = queues[3]->wait_pop({}, 0ms);
    ASSERT_TRUE(second.packet);
    ASSERT_TRUE(third.packet);
    ASSERT_TRUE(fourth.packet);
    EXPECT_GE(second.packet->camera_frame_number, 1U);
    EXPECT_GE(third.packet->camera_frame_number, 2U);
    EXPECT_EQ(fourth.packet->pixel_format, PixelFormat::mono10);

    for (auto& worker : workers)
    {
        worker->request_stop();
        EXPECT_TRUE(worker->join(std::chrono::steady_clock::now() + 1s));
    }

    EXPECT_TRUE(devices[0]->connect());
    EXPECT_TRUE(devices[0]->start_acquisition());
    FrameBuffer recovered{64U};
    EXPECT_TRUE(devices[0]->capture_into(recovered, 50ms));
}
