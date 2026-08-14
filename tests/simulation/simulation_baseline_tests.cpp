#include "paperbreak/camera/acquisition.hpp"
#include "paperbreak/camera/mock_camera.hpp"
#include "paperbreak/common/camera_slots.hpp"
#include "paperbreak/pipeline/pipeline.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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
using namespace paperbreak::pipeline;

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

TEST(SimulationMockCamera, SixRoutesAcquireAndIsolateInjectedFailuresWithoutHardware)
{
    std::vector<MockCameraConfig> configurations;
    for (std::size_t index = 0U; index < paperbreak::camera_slot_count; ++index)
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

    std::array<std::unique_ptr<ICameraDevice>, paperbreak::camera_slot_count> devices;
    std::array<std::unique_ptr<FrameBufferPool>, paperbreak::camera_slot_count> pools;
    std::array<std::unique_ptr<AcquisitionQueue>, paperbreak::camera_slot_count> queues;
    std::array<std::unique_ptr<AcquisitionWorker>, paperbreak::camera_slot_count> workers;
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
            AcquisitionWorkerOptions{.camera_id =
                                         std::string{paperbreak::canonical_camera_ids[index]},
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

TEST(SimulationProcessingPipeline, SixRoutesRemainBoundedAndIsolateInjectedFailures)
{
    std::vector<MockCameraConfig> configurations;
    for (std::size_t index = 0U; index < paperbreak::camera_slot_count; ++index)
    {
        configurations.push_back(
            {.descriptor = {"PaperBreak Mock", "PIPE-000" + std::to_string(index + 1U),
                            "192.0.2." + std::to_string(index + 11U), "mock0"},
             .width = 4U,
             .height = 4U,
             .frame_rate = 200.0,
             .pixel_format = index == 3U ? PixelFormat::mono12 : PixelFormat::mono8,
             .pattern = MockFramePattern::gradient,
             .random_seed = 200U + index,
             .maximum_payload_bytes = 64U});
    }
    auto provider_result = MockCameraProvider::create(std::move(configurations));
    ASSERT_TRUE(provider_result);
    auto provider = std::move(provider_result).value();

    std::array<std::unique_ptr<ICameraDevice>, paperbreak::camera_slot_count> devices;
    std::array<std::unique_ptr<FrameBufferPool>, paperbreak::camera_slot_count> pools;
    std::array<std::unique_ptr<AcquisitionQueue>, paperbreak::camera_slot_count> inputs;
    std::array<std::unique_ptr<AlgorithmQueue>, paperbreak::camera_slot_count> outputs;
    std::array<std::unique_ptr<AcquisitionWorker>, paperbreak::camera_slot_count> workers;
    std::array<PerCameraProcessor*, paperbreak::camera_slot_count> processor_views{};
    ProcessingRuntime runtime;

    for (std::size_t index = 0U; index < devices.size(); ++index)
    {
        const auto serial = "PIPE-000" + std::to_string(index + 1U);
        const auto camera_id = std::string{paperbreak::canonical_camera_ids[index]};
        auto created = provider->create_device(serial);
        ASSERT_TRUE(created);
        devices[index] = std::move(created).value();
        ASSERT_TRUE(devices[index]->connect());
        ASSERT_TRUE(devices[index]->start_acquisition());
        pools[index] = std::make_unique<FrameBufferPool>(8U, 64U);
        inputs[index] = std::make_unique<AcquisitionQueue>(4U);
        outputs[index] = std::make_unique<AlgorithmQueue>(2U);
        workers[index] = std::make_unique<AcquisitionWorker>(
            *devices[index], *pools[index], *inputs[index],
            AcquisitionWorkerOptions{
                .camera_id = camera_id, .receive_timeout = 20ms, .statistics_window = 20ms});
        std::vector<std::unique_ptr<IPreprocessingNode>> nodes;
        nodes.push_back(std::make_unique<ValidityCheckNode>(ValidityCheckOptions{
            .expected_geometry = FrameGeometry{4U, 4U, index == 3U ? 8U : 4U},
            .expected_pixel_format = index == 3U ? PixelFormat::mono12 : PixelFormat::mono8}));
        nodes.push_back(std::make_unique<GrayStatisticsNode>());
        auto processor = std::make_unique<PerCameraProcessor>(
            *inputs[index], *outputs[index], PreprocessingChain{std::move(nodes)},
            PerCameraProcessorOptions{.camera_id = camera_id, .input_wait_timeout = 10ms});
        processor_views[index] = processor.get();
        ASSERT_TRUE(runtime.add(std::move(processor)));
    }

    ASSERT_TRUE(runtime.start());
    for (auto& worker : workers)
    {
        ASSERT_TRUE(worker->start());
    }
    ASSERT_TRUE(wait_until([&] {
        return std::ranges::all_of(
            workers, [](const auto& worker) { return worker->snapshot().frames_received > 0U; });
    }));
    ASSERT_TRUE(
        provider->control("PIPE-0002").value().inject_fault({.kind = MockFaultKind::timeout}));
    ASSERT_TRUE(provider->control("PIPE-0003")
                    .value()
                    .inject_fault({.kind = MockFaultKind::drop_frame, .amount = 2U}));
    ASSERT_TRUE(provider->control("PIPE-0004")
                    .value()
                    .inject_fault({.kind = MockFaultKind::incomplete_frame}));
    ASSERT_TRUE(wait_until([&] {
        return std::ranges::all_of(
                   outputs,
                   [](const auto& output) { return output->snapshot().algorithm_skipped > 0U; }) &&
               workers[1]->snapshot().capture_timeouts > 0U &&
               workers[2]->snapshot().camera_frame_gaps >= 2U &&
               workers[3]->snapshot().incomplete_frames > 0U &&
               processor_views[3]->snapshot().invalid_frames > 0U;
    }));
    ASSERT_TRUE(
        provider->control("PIPE-0001").value().inject_fault({.kind = MockFaultKind::disconnect}));
    ASSERT_TRUE(wait_until([&] { return workers[0]->snapshot().completed; }));

    EXPECT_GE(workers[1]->snapshot().capture_timeouts, 1U);
    EXPECT_GE(workers[2]->snapshot().camera_frame_gaps, 2U);
    EXPECT_GE(workers[3]->snapshot().incomplete_frames, 1U);
    EXPECT_GE(processor_views[3]->snapshot().invalid_frames, 1U);
    for (std::size_t index = 1U; index < workers.size(); ++index)
    {
        EXPECT_TRUE(workers[index]->snapshot().running);
        EXPECT_GT(processor_views[index]->snapshot().frames_processed, 0U);
        EXPECT_LE(outputs[index]->snapshot().depth, outputs[index]->snapshot().capacity);
    }

    for (auto& worker : workers)
    {
        worker->request_stop();
        EXPECT_TRUE(worker->join(std::chrono::steady_clock::now() + 1s));
    }
    for (auto& input : inputs)
    {
        input->close();
    }
    EXPECT_TRUE(runtime.join(std::chrono::steady_clock::now() + 1s));
    for (const auto& output : outputs)
    {
        EXPECT_TRUE(output->snapshot().closed);
    }
}
