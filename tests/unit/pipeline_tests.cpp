#include "paperbreak/pipeline/pipeline.hpp"
#include "paperbreak/pipeline/preview.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
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
using namespace std::chrono_literals;
using namespace paperbreak;
using namespace paperbreak::camera;
using namespace paperbreak::pipeline;

FramePacket make_packet(const std::uint64_t sequence, const PixelFormat format = PixelFormat::mono8,
                        const FrameGeometry geometry = {2U, 2U, 2U}, const bool incomplete = false)
{
    auto buffer =
        std::make_shared<FrameBuffer>(static_cast<std::size_t>(geometry.stride) * geometry.height);
    auto bytes = buffer->writable_bytes();
    for (std::size_t index = 0U; index < bytes.size(); ++index)
    {
        bytes[index] = static_cast<std::byte>((index + sequence) & 0xffU);
    }
    static_cast<void>(buffer->set_size(bytes.size()));
    return {.camera_id = "CAM01",
            .camera_frame_number = sequence,
            .sequence_number = sequence,
            .received_monotonic_time = MonotonicTime{std::chrono::milliseconds{sequence}},
            .received_wall_clock_time = WallClockTime{std::chrono::milliseconds{sequence}},
            .geometry = geometry,
            .pixel_format = format,
            .buffer = std::move(buffer),
            .flags = {.incomplete = incomplete}};
}

ProcessedFrame processed_frame(const std::uint64_t sequence)
{
    auto view = make_frame_view(make_packet(sequence));
    if (!view)
    {
        throw std::runtime_error{"test frame invalid"};
    }
    return {.frame = std::move(view).value()};
}

class ThrowingNode final : public IPreprocessingNode
{
  public:
    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "throwing";
    }
    [[nodiscard]] Result<void> process(ProcessedFrame&) override
    {
        throw std::runtime_error{"injected-node-failure"};
    }
};

class SlowNode final : public IPreprocessingNode
{
  public:
    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "slow";
    }
    [[nodiscard]] Result<void> process(ProcessedFrame&) override
    {
        std::this_thread::sleep_for(100ms);
        return Result<void>::success();
    }
};

class CountingPreviewEncoder final : public IPreviewEncoder
{
  public:
    explicit CountingPreviewEncoder(const bool fail = false,
                                    const std::chrono::milliseconds delay = {})
        : fail_(fail), delay_(delay)
    {
    }

    [[nodiscard]] Result<std::vector<std::byte>> encode(const FrameView& frame,
                                                        const PreviewEncodeOptions&) override
    {
        calls.fetch_add(1U, std::memory_order_relaxed);
        if (delay_ > 0ms)
            std::this_thread::sleep_for(delay_);
        if (fail_)
            return Result<std::vector<std::byte>>::failure(
                make_error("PIPELINE_PREVIEW_ENCODE_FAILED", Severity::warning, "注入编码失败",
                           "test", "test.preview"));
        return Result<std::vector<std::byte>>::success(
            {static_cast<std::byte>(frame.sequence_number() & 0xffU)});
    }

    std::atomic_uint64_t calls{};

  private:
    bool fail_{};
    std::chrono::milliseconds delay_{};
};

FrameView preview_frame(const std::uint64_t sequence, const std::string& camera_id = "CAM01")
{
    auto packet = make_packet(sequence);
    packet.camera_id = camera_id;
    auto view = make_frame_view(packet);
    if (!view)
        throw std::runtime_error{"preview frame invalid"};
    return std::move(view).value();
}

bool wait_preview(const std::function<bool()>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}
} // namespace

TEST(PipelineNodes, RunsConfiguredChainWithoutCopyingFrameBuffer)
{
    auto packet = make_packet(1U, PixelFormat::mono8, {2U, 2U, 4U});
    auto* original = packet.buffer->bytes().data();
    auto view = make_frame_view(packet);
    ASSERT_TRUE(view);
    ProcessedFrame frame{.frame = std::move(view).value()};
    std::vector<std::unique_ptr<IPreprocessingNode>> nodes;
    nodes.push_back(std::make_unique<PassThroughNode>());
    nodes.push_back(std::make_unique<ValidityCheckNode>(
        ValidityCheckOptions{.expected_geometry = FrameGeometry{2U, 2U, 4U},
                             .expected_pixel_format = PixelFormat::mono8}));
    nodes.push_back(std::make_unique<GrayStatisticsNode>());
    PreprocessingChain chain{std::move(nodes)};

    ASSERT_TRUE(chain.process(frame));
    EXPECT_TRUE(frame.validity_checked);
    ASSERT_TRUE(frame.gray_statistics);
    EXPECT_EQ(frame.gray_statistics->sample_count, 4U);
    EXPECT_EQ(frame.frame.bytes().data(), original);
    EXPECT_NEAR(frame.gray_statistics->minimum, 1.0 / 255.0, 1e-9);
    EXPECT_NEAR(frame.gray_statistics->maximum, 6.0 / 255.0, 1e-9);
    EXPECT_NEAR(frame.gray_statistics->mean, 3.5 / 255.0, 1e-9);
}

TEST(PipelineNodes, ComputesAllSupportedPixelFormatsAndIgnoresPadding)
{
    for (const auto format :
         {PixelFormat::mono8, PixelFormat::bayer_rg8, PixelFormat::mono10, PixelFormat::mono12})
    {
        const bool wide = format == PixelFormat::mono10 || format == PixelFormat::mono12;
        auto packet =
            make_packet(1U, format, wide ? FrameGeometry{2U, 1U, 6U} : FrameGeometry{2U, 1U, 4U});
        auto bytes = std::const_pointer_cast<FrameBuffer>(packet.buffer)->writable_bytes();
        if (wide)
        {
            bytes[0] = std::byte{0x00};
            bytes[1] = std::byte{0x00};
            bytes[2] = std::byte{0xff};
            bytes[3] = format == PixelFormat::mono10 ? std::byte{0x03} : std::byte{0x0f};
        }
        else
        {
            bytes[0] = std::byte{0x00};
            bytes[1] = std::byte{0xff};
        }
        auto view = make_frame_view(packet);
        ASSERT_TRUE(view);
        ProcessedFrame frame{.frame = std::move(view).value()};
        GrayStatisticsNode node;
        ASSERT_TRUE(node.process(frame));
        ASSERT_TRUE(frame.gray_statistics);
        EXPECT_DOUBLE_EQ(frame.gray_statistics->minimum, 0.0);
        EXPECT_DOUBLE_EQ(frame.gray_statistics->maximum, 1.0);
        EXPECT_DOUBLE_EQ(frame.gray_statistics->mean, 0.5);
        EXPECT_EQ(frame.gray_statistics->sample_count, 2U);
    }
}

TEST(PipelineNodes, RejectsIncompleteUnexpectedAndInvalidLayouts)
{
    auto incomplete_view = make_frame_view(make_packet(1U, PixelFormat::mono8, {2U, 2U, 2U}, true));
    ASSERT_TRUE(incomplete_view);
    ProcessedFrame incomplete{.frame = std::move(incomplete_view).value()};
    ValidityCheckNode node{{.expected_geometry = FrameGeometry{2U, 2U, 2U},
                            .expected_pixel_format = PixelFormat::mono8}};
    auto result = node.process(incomplete);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "CAMERA_FRAME_INCOMPLETE");

    auto changed_view = make_frame_view(make_packet(2U, PixelFormat::bayer_rg8));
    ASSERT_TRUE(changed_view);
    ProcessedFrame changed{.frame = std::move(changed_view).value()};
    result = node.process(changed);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "CAMERA_FRAME_FORMAT_CHANGED");

    auto invalid_stride_view = make_frame_view(make_packet(3U, PixelFormat::mono12, {2U, 1U, 2U}));
    ASSERT_TRUE(invalid_stride_view);
    ProcessedFrame invalid_stride{.frame = std::move(invalid_stride_view).value()};
    result = ValidityCheckNode{}.process(invalid_stride);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "CAMERA_FRAME_FORMAT_CHANGED");
}

TEST(PipelineAlgorithmQueue, DropsOldestAndReportsBoundedSnapshot)
{
    AlgorithmQueue queue{2U};
    EXPECT_EQ(queue.push(processed_frame(1U)), AlgorithmEnqueueStatus::enqueued);
    EXPECT_EQ(queue.push(processed_frame(2U)), AlgorithmEnqueueStatus::enqueued);
    EXPECT_EQ(queue.push(processed_frame(3U)),
              AlgorithmEnqueueStatus::enqueued_after_skipping_oldest);
    const auto snapshot = queue.snapshot();
    EXPECT_EQ(snapshot.capacity, 2U);
    EXPECT_EQ(snapshot.depth, 2U);
    EXPECT_EQ(snapshot.high_watermark, 2U);
    EXPECT_EQ(snapshot.algorithm_skipped, 1U);

    auto first = queue.wait_pop({}, 0ms);
    auto second = queue.wait_pop({}, 0ms);
    ASSERT_TRUE(first.frame);
    ASSERT_TRUE(second.frame);
    EXPECT_EQ(first.frame->frame.sequence_number(), 2U);
    EXPECT_EQ(second.frame->frame.sequence_number(), 3U);
    queue.close();
    EXPECT_EQ(queue.wait_pop({}, 0ms).status, AlgorithmDequeueStatus::closed);
    EXPECT_EQ(queue.push(processed_frame(4U)), AlgorithmEnqueueStatus::closed);
}

TEST(PipelineAlgorithmQueue, CancelsWaitAndDrainsFramesBeforeClosed)
{
    AlgorithmQueue cancelled{1U};
    AlgorithmDequeueResult cancelled_result;
    std::jthread waiter(
        [&](const std::stop_token token) { cancelled_result = cancelled.wait_pop(token, 10s); });
    waiter.request_stop();
    waiter.join();
    EXPECT_EQ(cancelled_result.status, AlgorithmDequeueStatus::stopped);
    EXPECT_EQ(cancelled.snapshot().wait_cancelled, 1U);

    AlgorithmQueue draining{1U};
    ASSERT_EQ(draining.push(processed_frame(1U)), AlgorithmEnqueueStatus::enqueued);
    draining.close();
    EXPECT_EQ(draining.wait_pop({}, 0ms).status, AlgorithmDequeueStatus::frame);
    EXPECT_EQ(draining.wait_pop({}, 0ms).status, AlgorithmDequeueStatus::closed);
}

TEST(PipelineProcessor, PreservesOrderRejectsInvalidAndDrainsClosedInput)
{
    AcquisitionQueue input{8U};
    AlgorithmQueue output{8U};
    std::vector<std::unique_ptr<IPreprocessingNode>> nodes;
    nodes.push_back(std::make_unique<ValidityCheckNode>());
    nodes.push_back(std::make_unique<GrayStatisticsNode>());
    PerCameraProcessor processor{input,
                                 output,
                                 PreprocessingChain{std::move(nodes)},
                                 {.camera_id = "CAM01", .input_wait_timeout = 10ms}};
    EXPECT_EQ(input.push(make_packet(1U)), FrameEnqueueStatus::enqueued);
    EXPECT_EQ(input.push(make_packet(3U)), FrameEnqueueStatus::enqueued);
    EXPECT_EQ(input.push(make_packet(4U, PixelFormat::mono8, {2U, 2U, 2U}, true)),
              FrameEnqueueStatus::enqueued);
    input.close();

    ASSERT_TRUE(processor.start());
    ASSERT_TRUE(processor.join(std::chrono::steady_clock::now() + 1s));
    const auto snapshot = processor.snapshot();
    EXPECT_EQ(snapshot.frames_processed, 2U);
    EXPECT_EQ(snapshot.frames_rejected, 1U);
    EXPECT_EQ(snapshot.invalid_frames, 1U);
    EXPECT_EQ(snapshot.sequence_gaps, 1U);
    EXPECT_GT(snapshot.maximum_processing_microseconds, 0.0);
    EXPECT_TRUE(snapshot.algorithm_queue.closed);

    auto first = output.wait_pop({}, 0ms);
    auto second = output.wait_pop({}, 0ms);
    ASSERT_TRUE(first.frame);
    ASSERT_TRUE(second.frame);
    EXPECT_EQ(first.frame->frame.sequence_number(), 1U);
    EXPECT_EQ(second.frame->frame.sequence_number(), 3U);
    EXPECT_EQ(output.wait_pop({}, 0ms).status, AlgorithmDequeueStatus::closed);
}

TEST(PipelineProcessor, ContainsNodeExceptionsPerFrameAndStopsDeterministically)
{
    AcquisitionQueue input{4U};
    AlgorithmQueue output{2U};
    std::vector<std::unique_ptr<IPreprocessingNode>> nodes;
    nodes.push_back(std::make_unique<ThrowingNode>());
    PerCameraProcessor processor{input,
                                 output,
                                 PreprocessingChain{std::move(nodes)},
                                 {.camera_id = "CAM01", .input_wait_timeout = 10ms}};
    EXPECT_EQ(input.push(make_packet(1U)), FrameEnqueueStatus::enqueued);
    EXPECT_EQ(input.push(make_packet(2U)), FrameEnqueueStatus::enqueued);
    input.close();
    ASSERT_TRUE(processor.start());
    ASSERT_TRUE(processor.join(std::chrono::steady_clock::now() + 1s));
    const auto snapshot = processor.snapshot();
    EXPECT_EQ(snapshot.frames_rejected, 2U);
    EXPECT_EQ(snapshot.node_failures, 2U);
    ASSERT_TRUE(snapshot.last_error);
    EXPECT_EQ(snapshot.last_error->business_code, "ALGORITHM_PROCESS_FAILED");
    EXPECT_EQ(snapshot.algorithm_queue.enqueued, 0U);
}

TEST(PipelineProcessor, CancelsWaitAndRejectsRepeatedStart)
{
    AcquisitionQueue input{1U};
    AlgorithmQueue output{1U};
    std::vector<std::unique_ptr<IPreprocessingNode>> nodes;
    nodes.push_back(std::make_unique<PassThroughNode>());
    PerCameraProcessor processor{input,
                                 output,
                                 PreprocessingChain{std::move(nodes)},
                                 {.camera_id = "CAM01", .input_wait_timeout = 10s}};
    ASSERT_TRUE(processor.start());
    EXPECT_FALSE(processor.start());
    processor.request_stop();
    EXPECT_TRUE(processor.join(std::chrono::steady_clock::now() + 500ms));
    EXPECT_TRUE(processor.snapshot().completed);
}

TEST(PipelineProcessor, ReportsJoinDeadlineAndThenCompletes)
{
    AcquisitionQueue input{1U};
    AlgorithmQueue output{1U};
    std::vector<std::unique_ptr<IPreprocessingNode>> nodes;
    nodes.push_back(std::make_unique<SlowNode>());
    PerCameraProcessor processor{input,
                                 output,
                                 PreprocessingChain{std::move(nodes)},
                                 {.camera_id = "CAM01", .input_wait_timeout = 10ms}};
    ASSERT_EQ(input.push(make_packet(1U)), FrameEnqueueStatus::enqueued);
    input.close();
    ASSERT_TRUE(processor.start());
    auto early = processor.join(std::chrono::steady_clock::now() + 5ms);
    ASSERT_FALSE(early);
    EXPECT_EQ(early.error().business_code, "SYS_SHUTDOWN_TIMEOUT");
    processor.request_stop();
    EXPECT_TRUE(processor.join(std::chrono::steady_clock::now() + 1s));
}

TEST(PipelineRuntime, EnforcesFourUniqueRoutesAndRollsBackPartialStart)
{
    std::vector<std::unique_ptr<AcquisitionQueue>> inputs;
    std::vector<std::unique_ptr<AlgorithmQueue>> outputs;
    ProcessingRuntime runtime;
    PerCameraProcessor* first{};
    for (std::size_t index = 0U; index < 2U; ++index)
    {
        inputs.push_back(std::make_unique<AcquisitionQueue>(1U));
        outputs.push_back(std::make_unique<AlgorithmQueue>(1U));
        std::vector<std::unique_ptr<IPreprocessingNode>> nodes;
        nodes.push_back(std::make_unique<PassThroughNode>());
        auto processor = std::make_unique<PerCameraProcessor>(
            *inputs.back(), *outputs.back(), PreprocessingChain{std::move(nodes)},
            PerCameraProcessorOptions{.camera_id = index == 0U ? "CAM01" : "",
                                      .input_wait_timeout = 10ms});
        if (index == 0U)
        {
            first = processor.get();
        }
        ASSERT_TRUE(runtime.add(std::move(processor)));
    }
    auto started = runtime.start();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().business_code, "CAMERA_CONFIG_FAILED");
    ASSERT_NE(first, nullptr);
    EXPECT_TRUE(first->snapshot().completed);

    ProcessingRuntime capacity_runtime;
    for (std::size_t index = 0U; index < 5U; ++index)
    {
        inputs.push_back(std::make_unique<AcquisitionQueue>(1U));
        outputs.push_back(std::make_unique<AlgorithmQueue>(1U));
        std::vector<std::unique_ptr<IPreprocessingNode>> nodes;
        nodes.push_back(std::make_unique<PassThroughNode>());
        auto processor = std::make_unique<PerCameraProcessor>(
            *inputs.back(), *outputs.back(), PreprocessingChain{std::move(nodes)},
            PerCameraProcessorOptions{.camera_id = "CAP0" + std::to_string(index + 1U),
                                      .input_wait_timeout = 10ms});
        const auto added = capacity_runtime.add(std::move(processor));
        EXPECT_EQ(static_cast<bool>(added), index < 4U);
    }
    EXPECT_EQ(capacity_runtime.size(), 4U);
}

TEST(PipelinePreviewRuntime, DoesNotEncodeWithoutSubscribersAndSamplesAtConfiguredRate)
{
    auto encoder = std::make_unique<CountingPreviewEncoder>();
    auto* encoder_ptr = encoder.get();
    PreviewRuntime runtime{
        {"CAM01"}, std::move(encoder), [](PreviewDelivery) {}, {.frames_per_second = 3.0}};
    ASSERT_TRUE(runtime.start());
    AcquisitionQueue acquisition{2U};
    auto source_packet = make_packet(1U);
    auto source_view = make_frame_view(source_packet);
    ASSERT_TRUE(source_view);
    ASSERT_EQ(acquisition.push(source_packet), FrameEnqueueStatus::enqueued);
    const auto before_preview = acquisition.snapshot();
    runtime.submit(std::move(source_view).value());
    ASSERT_TRUE(
        wait_preview([&] { return runtime.snapshot().frames_skipped_without_subscribers == 1U; }));
    EXPECT_EQ(encoder_ptr->calls.load(), 0U);
    const auto after_preview = acquisition.snapshot();
    EXPECT_EQ(after_preview.enqueued, before_preview.enqueued);
    EXPECT_EQ(after_preview.dequeued, before_preview.dequeued);
    EXPECT_EQ(after_preview.dropped_oldest, before_preview.dropped_oldest);

    ASSERT_TRUE(runtime.subscribe(11U, {"CAM01"}));
    runtime.submit(preview_frame(10U));
    runtime.submit(preview_frame(11U));
    ASSERT_TRUE(wait_preview([&] { return runtime.snapshot().encoded == 1U; }));
    EXPECT_EQ(encoder_ptr->calls.load(), 1U);
    EXPECT_GE(runtime.snapshot().frames_skipped_by_rate, 1U);
    EXPECT_EQ(runtime.snapshot().encoding_attempts, 1U);
    EXPECT_GE(runtime.snapshot().last_encoding_time.count(), 0);
    EXPECT_GE(runtime.snapshot().average_encoding_time.count(), 0);
    EXPECT_GE(runtime.snapshot().maximum_encoding_time, runtime.snapshot().last_encoding_time);
    ASSERT_EQ(runtime.snapshot().cameras.size(), 1U);
    EXPECT_EQ(runtime.snapshot().cameras.front().encoding_attempts, 1U);
    runtime.request_stop();
    EXPECT_TRUE(runtime.join(std::chrono::steady_clock::now() + 1s));
}

TEST(PipelinePreviewRuntime, SupportsThirtyFpsAndThrottlesEachSubscriberIndependently)
{
    std::mutex deliveries_mutex;
    std::vector<PreviewDelivery> deliveries;
    PreviewRuntime runtime{{"CAM01"},
                           std::make_unique<CountingPreviewEncoder>(),
                           [&](PreviewDelivery delivery) {
                               std::scoped_lock lock{deliveries_mutex};
                               deliveries.push_back(std::move(delivery));
                           },
                           {.frames_per_second = 2.0}};
    ASSERT_TRUE(runtime.start());
    const auto ten_fps = runtime.subscribe(10U, {"CAM01"}, 10.0);
    const auto thirty_fps = runtime.subscribe(30U, {"CAM01"}, 30.0);
    ASSERT_TRUE(ten_fps);
    ASSERT_TRUE(thirty_fps);
    EXPECT_EQ(ten_fps.value(), 10.0);
    EXPECT_EQ(thirty_fps.value(), 30.0);
    auto invalid = runtime.subscribe(40U, {"CAM01"}, 30.1);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().business_code, "IPC_REQUEST_INVALID");

    const std::array<std::uint64_t, 7U> frame_times_ms{1U, 35U, 69U, 103U, 137U, 171U, 205U};
    for (std::size_t index = 0; index < frame_times_ms.size(); ++index)
    {
        runtime.submit(preview_frame(frame_times_ms[index]));
        ASSERT_TRUE(wait_preview([&] { return runtime.snapshot().encoded >= index + 1U; }));
    }
    {
        std::scoped_lock lock{deliveries_mutex};
        EXPECT_EQ(std::ranges::count(deliveries, 30U, &PreviewDelivery::subscriber_id), 7);
        EXPECT_EQ(std::ranges::count(deliveries, 10U, &PreviewDelivery::subscriber_id), 3);
    }
    runtime.request_stop();
    EXPECT_TRUE(runtime.join(std::chrono::steady_clock::now() + 1s));
}

TEST(PipelinePreviewRuntime, ReplacesPendingFramesDeliversToFourSubscribersAndSurvivesFailures)
{
    std::mutex deliveries_mutex;
    std::vector<PreviewDelivery> deliveries;
    auto slow_encoder = std::make_unique<CountingPreviewEncoder>(false, 40ms);
    PreviewRuntime runtime{{"CAM01", "CAM02", "CAM03", "CAM04"},
                           std::move(slow_encoder),
                           [&](PreviewDelivery delivery) {
                               std::scoped_lock lock{deliveries_mutex};
                               deliveries.push_back(std::move(delivery));
                           },
                           {.frames_per_second = 5.0}};
    ASSERT_TRUE(runtime.start());
    for (std::uint64_t subscriber = 1U; subscriber <= 4U; ++subscriber)
        ASSERT_TRUE(runtime.subscribe(subscriber, {"CAM01", "CAM02", "CAM03", "CAM04"}));
    for (std::uint64_t sequence = 1U; sequence <= 12U; ++sequence)
        runtime.submit(preview_frame(sequence * 1000U));
    ASSERT_TRUE(wait_preview([&] {
        std::scoped_lock lock{deliveries_mutex};
        return deliveries.size() >= 4U;
    }));
    EXPECT_GT(runtime.snapshot().frames_replaced_before_encoding, 0U);
    EXPECT_GT(runtime.snapshot().average_encoding_time, 20ms);
    {
        std::scoped_lock lock{deliveries_mutex};
        for (const auto& delivery : deliveries)
        {
            EXPECT_EQ(delivery.camera_id, "CAM01");
            EXPECT_EQ(delivery.jpeg.size(), 1U);
        }
    }
    runtime.unsubscribe(1U);
    runtime.request_stop();
    EXPECT_TRUE(runtime.join(std::chrono::steady_clock::now() + 1s));

    auto failing_encoder = std::make_unique<CountingPreviewEncoder>(true);
    PreviewRuntime failing{
        {"CAM01"}, std::move(failing_encoder), [](PreviewDelivery) {}, {.frames_per_second = 3.0}};
    ASSERT_TRUE(failing.start());
    ASSERT_TRUE(failing.subscribe(9U, {"CAM01"}));
    failing.submit(preview_frame(1U));
    ASSERT_TRUE(wait_preview([&] { return failing.snapshot().encoding_failures == 1U; }));
    failing.request_stop();
    EXPECT_TRUE(failing.join(std::chrono::steady_clock::now() + 1s));
}

TEST(PipelinePreviewRuntime, RotatesFairlyAcrossTwoContinuouslyBusyCameras)
{
    std::mutex mutex;
    std::vector<std::string> delivery_order;
    auto encoder = std::make_unique<CountingPreviewEncoder>(false, 3ms);
    PreviewRuntime runtime{{"CAM01", "CAM02"},
                           std::move(encoder),
                           [&](PreviewDelivery delivery) {
                               std::scoped_lock lock{mutex};
                               delivery_order.push_back(std::move(delivery.camera_id));
                           },
                           {.frames_per_second = 5.0}};
    ASSERT_TRUE(runtime.start());
    ASSERT_TRUE(runtime.subscribe(1U, {"CAM01", "CAM02"}));

    for (std::uint64_t sequence = 1U; sequence <= 100U; ++sequence)
    {
        runtime.submit(preview_frame(sequence * 1000U, "CAM01"));
        runtime.submit(preview_frame(sequence * 1000U, "CAM02"));
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(wait_preview([&] {
        std::scoped_lock lock{mutex};
        return std::count(delivery_order.begin(), delivery_order.end(), "CAM01") >= 8 &&
               std::count(delivery_order.begin(), delivery_order.end(), "CAM02") >= 8;
    }));

    std::size_t longest_run = 0U;
    {
        std::scoped_lock lock{mutex};
        std::size_t run = 0U;
        std::string previous;
        for (const auto& camera_id : delivery_order)
        {
            run = camera_id == previous ? run + 1U : 1U;
            previous = camera_id;
            longest_run = std::max(longest_run, run);
        }
    }
    EXPECT_LE(longest_run, 2U);
    const auto snapshot = runtime.snapshot();
    ASSERT_EQ(snapshot.cameras.size(), 2U);
    for (const auto& camera : snapshot.cameras)
    {
        EXPECT_GT(camera.sampled, 0U);
        EXPECT_GT(camera.encoded, 0U);
        EXPECT_GT(camera.deliveries, 0U);
        EXPECT_TRUE(camera.last_delivery_time.has_value());
    }
    runtime.request_stop();
    EXPECT_TRUE(runtime.join(std::chrono::steady_clock::now() + 1s));
}
