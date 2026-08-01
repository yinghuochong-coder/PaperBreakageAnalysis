#include "paperbreak/camera/acquisition.hpp"
#include "paperbreak/camera/mock_camera.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using namespace paperbreak;
using namespace paperbreak::camera;
using namespace std::chrono_literals;

FramePacket packet_with_sequence(const std::uint64_t sequence)
{
    auto buffer = std::make_shared<FrameBuffer>(1U);
    buffer->writable_bytes().front() = static_cast<std::byte>(sequence & 0xffU);
    static_cast<void>(buffer->set_size(1U));
    return {.camera_id = "CAM01",
            .camera_frame_number = sequence,
            .sequence_number = sequence,
            .geometry = {1U, 1U, 1U},
            .pixel_format = PixelFormat::mono8,
            .buffer = std::move(buffer)};
}

bool wait_until(const std::function<bool()>& predicate,
                const std::chrono::milliseconds timeout = 1s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::yield();
    }
    return predicate();
}

enum class CaptureAction
{
    success,
    timeout,
    permanent_error,
    slow_success,
    geometry_change,
};

class ScriptedCameraDevice final : public ICameraDevice
{
  public:
    explicit ScriptedCameraDevice(std::vector<CaptureAction> actions,
                                  const bool repeat_last = false)
        : actions_(std::move(actions)), repeat_last_(repeat_last)
    {
    }

    [[nodiscard]] const CameraDeviceDescriptor& descriptor() const noexcept override
    {
        return descriptor_;
    }
    [[nodiscard]] Result<void> connect() override
    {
        return Result<void>::success();
    }
    [[nodiscard]] Result<void> disconnect() override
    {
        return Result<void>::success();
    }
    [[nodiscard]] Result<CameraCapabilities> capabilities() override
    {
        return Result<CameraCapabilities>::success({});
    }
    [[nodiscard]] Result<CameraParameterSnapshot> read_parameters() override
    {
        return Result<CameraParameterSnapshot>::success({});
    }
    [[nodiscard]] Result<CameraParameterSnapshot> apply_parameters(
        const CameraParameterSnapshot& parameters) override
    {
        return Result<CameraParameterSnapshot>::success(parameters);
    }
    [[nodiscard]] Result<void> start_acquisition() override
    {
        return Result<void>::success();
    }
    [[nodiscard]] Result<CapturedFrameMetadata> capture_into(FrameBuffer& destination,
                                                             std::chrono::milliseconds) override
    {
        const auto call = capture_calls_.fetch_add(1U);
        CaptureAction action = CaptureAction::timeout;
        if (call < actions_.size())
        {
            action = actions_[call];
        }
        else if (repeat_last_ && !actions_.empty())
        {
            action = actions_.back();
        }

        if (action == CaptureAction::slow_success)
        {
            std::this_thread::sleep_for(100ms);
            action = CaptureAction::success;
        }
        if (action == CaptureAction::timeout)
        {
            return Result<CapturedFrameMetadata>::failure(make_camera_error(
                CameraErrorKind::frame_timeout, "测试取流超时", "camera.testCapture", "CAM01"));
        }
        if (action == CaptureAction::permanent_error)
        {
            return Result<CapturedFrameMetadata>::failure(make_camera_error(
                CameraErrorKind::config_failed, "测试永久错误", "camera.testCapture", "CAM01"));
        }

        if (action == CaptureAction::geometry_change)
        {
            std::fill(destination.writable_bytes().begin(), destination.writable_bytes().end(),
                      std::byte{0x33});
            static_cast<void>(destination.set_size(6U));
            return Result<CapturedFrameMetadata>::success({.camera_frame_number = 43U,
                                                           .geometry = {3U, 2U, 3U},
                                                           .pixel_format = PixelFormat::mono8});
        }

        std::fill(destination.writable_bytes().begin(), destination.writable_bytes().end(),
                  std::byte{0x5a});
        static_cast<void>(destination.set_size(4U));
        return Result<CapturedFrameMetadata>::success(
            {.camera_frame_number = 42U,
             .camera_timestamp = CameraTimestamp{100U, 1000U, CameraTimestampQuality::synchronized},
             .geometry = {2U, 2U, 2U},
             .pixel_format = PixelFormat::mono8});
    }
    [[nodiscard]] Result<void> software_trigger() override
    {
        trigger_calls_.fetch_add(1U);
        return Result<void>::success();
    }
    [[nodiscard]] Result<void> stop_acquisition() override
    {
        return Result<void>::success();
    }
    [[nodiscard]] Result<void> save_user_set(std::string_view) override
    {
        return Result<void>::success();
    }
    [[nodiscard]] Result<CameraParameterSnapshot> restore_defaults() override
    {
        return Result<CameraParameterSnapshot>::success({});
    }

    [[nodiscard]] std::size_t capture_calls() const noexcept
    {
        return capture_calls_.load();
    }
    [[nodiscard]] std::size_t trigger_calls() const noexcept
    {
        return trigger_calls_.load();
    }

  private:
    CameraDeviceDescriptor descriptor_{"Mock", "MOCK-0001", "192.0.2.1", "mock0"};
    std::vector<CaptureAction> actions_;
    bool repeat_last_{};
    std::atomic<std::size_t> capture_calls_{};
    std::atomic<std::size_t> trigger_calls_{};
};
} // namespace

TEST(CameraFrameBufferPool, PreallocatesFixedBuffersAndDoesNotGrowWhenExhausted)
{
    FrameBufferPool pool{2U, 16U};
    const auto first = pool.acquire({}, 0ms);
    const auto second = pool.acquire({}, 0ms);
    const auto exhausted = pool.acquire({}, 0ms);

    ASSERT_EQ(first.status, FramePoolAcquireStatus::acquired);
    ASSERT_EQ(second.status, FramePoolAcquireStatus::acquired);
    EXPECT_EQ(first.buffer->capacity(), 16U);
    EXPECT_EQ(second.buffer->capacity(), 16U);
    EXPECT_NE(first.buffer.get(), second.buffer.get());
    EXPECT_EQ(exhausted.status, FramePoolAcquireStatus::exhausted);
    EXPECT_FALSE(exhausted.buffer);

    const auto snapshot = pool.snapshot();
    EXPECT_EQ(snapshot.capacity, 2U);
    EXPECT_EQ(snapshot.available, 0U);
    EXPECT_EQ(snapshot.in_use, 2U);
    EXPECT_EQ(snapshot.in_use_high_watermark, 2U);
    EXPECT_EQ(snapshot.acquired, 2U);
    EXPECT_EQ(snapshot.exhausted, 1U);
}

TEST(CameraFrameBufferPool, ReusesOnlyPreallocatedAddressesAcrossManyLeases)
{
    FrameBufferPool pool{3U, 8U};
    std::set<const FrameBuffer*> addresses;
    for (std::size_t index = 0U; index < 1000U; ++index)
    {
        auto acquired = pool.acquire({}, 0ms);
        ASSERT_EQ(acquired.status, FramePoolAcquireStatus::acquired);
        addresses.insert(acquired.buffer.get());
        ASSERT_TRUE(acquired.buffer->set_size(4U));
        acquired.buffer.reset();
    }
    EXPECT_LE(addresses.size(), 3U);
    const auto snapshot = pool.snapshot();
    EXPECT_EQ(snapshot.available, 3U);
    EXPECT_EQ(snapshot.in_use, 0U);
    EXPECT_EQ(snapshot.acquired, 1000U);
}

TEST(CameraFrameBufferPool, CancelsWaitAndWakesOnClose)
{
    FrameBufferPool pool{1U, 4U};
    auto held = pool.acquire({}, 0ms).buffer;
    ASSERT_TRUE(held);
    EXPECT_EQ(pool.acquire({}, 1ms).status, FramePoolAcquireStatus::timeout);

    std::stop_source stop_source;
    std::atomic<FramePoolAcquireStatus> stopped{FramePoolAcquireStatus::acquired};
    std::jthread waiter(
        [&](std::stop_token) { stopped.store(pool.acquire(stop_source.get_token(), 1s).status); });
    stop_source.request_stop();
    waiter.join();
    EXPECT_EQ(stopped.load(), FramePoolAcquireStatus::stopped);

    std::atomic<FramePoolAcquireStatus> closed{FramePoolAcquireStatus::acquired};
    std::jthread close_waiter([&](std::stop_token) { closed.store(pool.acquire({}, 1s).status); });
    pool.close();
    pool.close();
    close_waiter.join();
    EXPECT_EQ(closed.load(), FramePoolAcquireStatus::closed);
    held.reset();
    EXPECT_EQ(pool.acquire({}, 0ms).status, FramePoolAcquireStatus::closed);
    const auto snapshot = pool.snapshot();
    EXPECT_EQ(snapshot.timed_out, 1U);
    EXPECT_EQ(snapshot.cancelled, 1U);
    EXPECT_TRUE(snapshot.closed);
}

TEST(CameraFrameBufferPool, LeaseCanOutliveOuterPoolObject)
{
    std::shared_ptr<FrameBuffer> lease;
    {
        auto pool = std::make_unique<FrameBufferPool>(1U, 4U);
        lease = pool->acquire({}, 0ms).buffer;
        ASSERT_TRUE(lease);
        ASSERT_TRUE(lease->set_size(4U));
    }
    EXPECT_EQ(lease->size(), 4U);
    lease.reset();
}

TEST(CameraAcquisitionQueue, DropsOldestAndReportsBoundedMetrics)
{
    AcquisitionQueue queue{2U};
    EXPECT_EQ(queue.push(packet_with_sequence(1U)), FrameEnqueueStatus::enqueued);
    EXPECT_EQ(queue.push(packet_with_sequence(2U)), FrameEnqueueStatus::enqueued);
    EXPECT_EQ(queue.push(packet_with_sequence(3U)),
              FrameEnqueueStatus::enqueued_after_dropping_oldest);

    auto first = queue.wait_pop({}, 0ms);
    auto second = queue.wait_pop({}, 0ms);
    ASSERT_EQ(first.status, FrameDequeueStatus::frame);
    ASSERT_EQ(second.status, FrameDequeueStatus::frame);
    EXPECT_EQ(first.packet->sequence_number, 2U);
    EXPECT_EQ(second.packet->sequence_number, 3U);

    const auto snapshot = queue.snapshot();
    EXPECT_EQ(snapshot.capacity, 2U);
    EXPECT_EQ(snapshot.depth, 0U);
    EXPECT_EQ(snapshot.high_watermark, 2U);
    EXPECT_EQ(snapshot.enqueued, 3U);
    EXPECT_EQ(snapshot.dequeued, 2U);
    EXPECT_EQ(snapshot.dropped_oldest, 1U);
}

TEST(CameraAcquisitionQueue, SupportsTimeoutCancellationAndCloseDrain)
{
    AcquisitionQueue queue{2U};
    EXPECT_EQ(queue.wait_pop({}, 1ms).status, FrameDequeueStatus::timeout);

    std::stop_source stop_source;
    stop_source.request_stop();
    EXPECT_EQ(queue.wait_pop(stop_source.get_token(), 1s).status, FrameDequeueStatus::stopped);

    ASSERT_EQ(queue.push(packet_with_sequence(7U)), FrameEnqueueStatus::enqueued);
    EXPECT_EQ(queue.wait_pop(stop_source.get_token(), 1s).status, FrameDequeueStatus::stopped);
    EXPECT_EQ(queue.snapshot().depth, 1U);
    queue.close();
    queue.close();
    EXPECT_EQ(queue.push(packet_with_sequence(8U)), FrameEnqueueStatus::closed);
    auto drained = queue.wait_pop({}, 0ms);
    ASSERT_EQ(drained.status, FrameDequeueStatus::frame);
    EXPECT_EQ(drained.packet->sequence_number, 7U);
    EXPECT_EQ(queue.wait_pop({}, 0ms).status, FrameDequeueStatus::closed);

    const auto snapshot = queue.snapshot();
    EXPECT_EQ(snapshot.wait_timeouts, 1U);
    EXPECT_EQ(snapshot.wait_cancelled, 2U);
    EXPECT_EQ(snapshot.rejected_closed, 1U);
    EXPECT_TRUE(snapshot.closed);
}

TEST(CameraAcquisitionQueue, ConcurrentCloseWakesConsumersAndRejectsProducers)
{
    AcquisitionQueue queue{8U};
    std::atomic<bool> consumer_finished{};
    std::jthread consumer([&](const std::stop_token token) {
        while (true)
        {
            const auto status = queue.wait_pop(token, 50ms).status;
            if (status != FrameDequeueStatus::frame && status != FrameDequeueStatus::timeout)
            {
                break;
            }
        }
        consumer_finished.store(true);
    });
    std::jthread producer([&](std::stop_token) {
        std::uint64_t sequence = 1U;
        while (queue.push(packet_with_sequence(sequence++)) != FrameEnqueueStatus::closed)
        {
        }
    });

    ASSERT_TRUE(wait_until([&] { return queue.snapshot().enqueued >= 100U; }));
    queue.close();
    producer.join();
    consumer.join();
    EXPECT_TRUE(consumer_finished.load());
    EXPECT_TRUE(queue.snapshot().closed);
}

TEST(CameraAcquisitionWorker, ContinuesAfterTimeoutAndPublishesCompleteMetadata)
{
    ScriptedCameraDevice device{
        {CaptureAction::timeout, CaptureAction::success, CaptureAction::permanent_error}};
    FrameBufferPool pool{2U, 4U};
    AcquisitionQueue queue{2U};
    AcquisitionWorker worker{device, pool, queue, {.camera_id = "CAM01", .receive_timeout = 10ms}};

    ASSERT_TRUE(worker.start());
    ASSERT_TRUE(worker.join(std::chrono::steady_clock::now() + 1s));
    const auto snapshot = worker.snapshot();
    EXPECT_TRUE(snapshot.started);
    EXPECT_FALSE(snapshot.running);
    EXPECT_TRUE(snapshot.completed);
    EXPECT_EQ(snapshot.last_sequence_number, 1U);
    EXPECT_EQ(snapshot.frames_received, 1U);
    EXPECT_EQ(snapshot.capture_timeouts, 1U);
    EXPECT_EQ(snapshot.bytes_received, 4U);
    ASSERT_TRUE(snapshot.last_frame_monotonic_time);
    ASSERT_TRUE(snapshot.last_frame_wall_clock_time);
    ASSERT_TRUE(snapshot.last_error);
    EXPECT_EQ(snapshot.last_error->business_code, "CAMERA_CONFIG_FAILED");
    EXPECT_EQ(device.capture_calls(), 3U);

    auto dequeued = queue.wait_pop({}, 0ms);
    ASSERT_EQ(dequeued.status, FrameDequeueStatus::frame);
    ASSERT_TRUE(dequeued.packet);
    EXPECT_EQ(dequeued.packet->camera_id, "CAM01");
    EXPECT_EQ(dequeued.packet->camera_frame_number, 42U);
    EXPECT_EQ(dequeued.packet->sequence_number, 1U);
    EXPECT_EQ(dequeued.packet->geometry, (FrameGeometry{2U, 2U, 2U}));
    EXPECT_EQ(dequeued.packet->buffer->size(), 4U);
    EXPECT_NE(dequeued.packet->received_monotonic_time, MonotonicTime{});
    EXPECT_NE(dequeued.packet->received_wall_clock_time, WallClockTime{});
}

TEST(CameraAcquisitionWorker, PublishesRecentFrameRateAndBandwidthWithoutQueueLock)
{
    ScriptedCameraDevice device{{CaptureAction::slow_success}, true};
    FrameBufferPool pool{4U, 4U};
    AcquisitionQueue queue{4U};
    AcquisitionWorker worker{
        device,
        pool,
        queue,
        {.camera_id = "CAM01", .receive_timeout = 200ms, .statistics_window = 20ms}};

    ASSERT_TRUE(worker.start());
    ASSERT_TRUE(wait_until([&] { return worker.snapshot().frames_received >= 2U; }, 1s));
    const auto snapshot = worker.snapshot();
    EXPECT_GT(snapshot.actual_fps, 0.0);
    EXPECT_GT(snapshot.bandwidth_bytes_per_second, 0.0);
    EXPECT_EQ(snapshot.bytes_received, snapshot.frames_received * 4U);
    EXPECT_NO_THROW(static_cast<void>(queue.snapshot()));
    worker.request_stop();
    EXPECT_TRUE(worker.join(std::chrono::steady_clock::now() + 1s));
}

TEST(CameraAcquisitionWorker, StopCancelsPoolWaitAndQueueCloseStopsProducer)
{
    ScriptedCameraDevice blocked_device{{CaptureAction::success}, true};
    FrameBufferPool blocked_pool{1U, 4U};
    auto held = blocked_pool.acquire({}, 0ms).buffer;
    AcquisitionQueue blocked_queue{1U};
    AcquisitionWorker blocked_worker{blocked_device,
                                     blocked_pool,
                                     blocked_queue,
                                     {.camera_id = "CAM01", .receive_timeout = 500ms}};
    ASSERT_TRUE(blocked_worker.start());
    blocked_worker.request_stop();
    EXPECT_TRUE(blocked_worker.join(std::chrono::steady_clock::now() + 500ms));
    EXPECT_EQ(blocked_device.capture_calls(), 0U);

    ScriptedCameraDevice producing_device{{CaptureAction::success}, true};
    FrameBufferPool producing_pool{3U, 4U};
    AcquisitionQueue producing_queue{1U};
    AcquisitionWorker producing_worker{producing_device,
                                       producing_pool,
                                       producing_queue,
                                       {.camera_id = "CAM02", .receive_timeout = 10ms}};
    ASSERT_TRUE(producing_worker.start());
    ASSERT_TRUE(wait_until([&] { return producing_device.capture_calls() >= 2U; }));
    producing_queue.close();
    EXPECT_TRUE(producing_worker.join(std::chrono::steady_clock::now() + 500ms));
    EXPECT_FALSE(producing_worker.snapshot().last_error);
}

TEST(CameraAcquisitionWorker, JoinReportsDeadlineAndStopEventuallyCompletes)
{
    ScriptedCameraDevice device{{CaptureAction::slow_success, CaptureAction::permanent_error}};
    FrameBufferPool pool{2U, 4U};
    AcquisitionQueue queue{1U};
    AcquisitionWorker worker{device, pool, queue, {.camera_id = "CAM01", .receive_timeout = 10ms}};
    ASSERT_TRUE(worker.start());
    ASSERT_TRUE(wait_until([&] { return device.capture_calls() >= 1U; }));

    auto result = worker.join(std::chrono::steady_clock::now());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "SYS_SHUTDOWN_TIMEOUT");
    worker.request_stop();
    EXPECT_TRUE(worker.join(std::chrono::steady_clock::now() + 1s));
}

TEST(CameraAcquisitionWorker, RejectsInvalidOrRepeatedStart)
{
    ScriptedCameraDevice device{{CaptureAction::permanent_error}};
    FrameBufferPool pool{1U, 4U};
    AcquisitionQueue queue{1U};
    AcquisitionWorker invalid{device, pool, queue, {.camera_id = "", .receive_timeout = 10ms}};
    auto result = invalid.start();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "CAMERA_CONFIG_FAILED");

    AcquisitionWorker worker{device, pool, queue, {.camera_id = "CAM01", .receive_timeout = 10ms}};
    ASSERT_TRUE(worker.start());
    result = worker.start();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().business_code, "CAMERA_INVALID_STATE_TRANSITION");
    EXPECT_TRUE(worker.join(std::chrono::steady_clock::now() + 1s));
}

TEST(CameraAcquisitionWorker, EscalatesConsecutiveTimeoutsWithBoundedContext)
{
    ScriptedCameraDevice device{{CaptureAction::timeout}, true};
    FrameBufferPool pool{1U, 4U};
    AcquisitionQueue queue{1U};
    AcquisitionWorker worker{
        device,
        pool,
        queue,
        {.camera_id = "CAM01", .receive_timeout = 1ms, .consecutive_timeout_limit = 3U}};

    ASSERT_TRUE(worker.start());
    ASSERT_TRUE(worker.join(std::chrono::steady_clock::now() + 1s));
    const auto snapshot = worker.snapshot();
    EXPECT_EQ(snapshot.capture_timeouts, 3U);
    ASSERT_TRUE(snapshot.last_error);
    EXPECT_EQ(snapshot.last_error->business_code, "CAMERA_FRAME_TIMEOUT");
    EXPECT_EQ(snapshot.last_error->source_id, "CAM01");
    EXPECT_EQ(snapshot.last_error->details.back().value, "3");
}

TEST(CameraAcquisitionWorker, SoftwareTriggerRunsBeforeCaptureAndWaitIsCancellable)
{
    ScriptedCameraDevice device{{CaptureAction::success}, true};
    FrameBufferPool pool{2U, 4U};
    AcquisitionQueue queue{2U};
    AcquisitionWorker worker{
        device,
        pool,
        queue,
        {.camera_id = "CAM01", .receive_timeout = 10ms, .software_trigger_interval = 2s}};

    ASSERT_TRUE(worker.start());
    ASSERT_TRUE(wait_until([&] { return worker.snapshot().frames_received >= 1U; }));
    EXPECT_EQ(device.trigger_calls(), device.capture_calls());
    worker.request_stop();
    EXPECT_TRUE(worker.join(std::chrono::steady_clock::now() + 200ms));
    EXPECT_FALSE(worker.snapshot().last_error);
}

TEST(CameraAcquisitionWorker, StopsBeforePublishingUnexpectedGeometryChange)
{
    ScriptedCameraDevice device{{CaptureAction::success, CaptureAction::geometry_change}};
    FrameBufferPool pool{2U, 8U};
    AcquisitionQueue queue{2U};
    AcquisitionWorker worker{device, pool, queue, {.camera_id = "CAM01", .receive_timeout = 1ms}};

    ASSERT_TRUE(worker.start());
    ASSERT_TRUE(worker.join(std::chrono::steady_clock::now() + 1s));
    const auto snapshot = worker.snapshot();
    EXPECT_EQ(snapshot.frames_received, 1U);
    ASSERT_TRUE(snapshot.last_error);
    EXPECT_EQ(snapshot.last_error->business_code, "CAMERA_FRAME_FORMAT_CHANGED");
    EXPECT_EQ(queue.snapshot().enqueued, 1U);
}

TEST(CameraRecoverySession, RecreatesDeviceAfterDisconnectAndCancelsDeterministically)
{
    using namespace paperbreak::camera::mock;
    MockCameraConfig config;
    config.descriptor = {"Mock", "RECOVERY-0001", "192.0.2.1", "mock0"};
    config.width = 2U;
    config.height = 2U;
    config.maximum_payload_bytes = 4U;
    config.fault_script = {{1U, {.kind = MockFaultKind::disconnect}}};
    auto provider_result = MockCameraProvider::create({config});
    ASSERT_TRUE(provider_result);
    auto provider = std::move(provider_result).value();
    FrameBufferPool pool{4U, 4U};
    AcquisitionQueue queue{2U};
    std::mutex records_mutex;
    std::vector<CameraTransitionRecord> records;
    RecoveringCameraSession session{*provider,
                                    pool,
                                    queue,
                                    {.camera_id = "CAM01",
                                     .serial_number = "RECOVERY-0001",
                                     .receive_timeout = 5ms,
                                     .statistics_window = 10ms,
                                     .consecutive_timeout_limit = 2U},
                                    [&](const CameraTransitionRecord& record) {
                                        std::lock_guard lock{records_mutex};
                                        records.push_back(record);
                                    },
                                    [](const std::stop_token token, std::chrono::milliseconds) {
                                        return !token.stop_requested();
                                    }};

    ASSERT_TRUE(session.start());
    ASSERT_TRUE(wait_until(
        [&] {
            const auto value = session.snapshot();
            return value.connection_attempts >= 2U && value.state.state == CameraState::streaming &&
                   value.acquisition.frames_received > 0U && queue.snapshot().dropped_oldest > 0U;
        },
        2s));
    EXPECT_GE(queue.snapshot().dropped_oldest, 1U);
    session.request_stop();
    EXPECT_TRUE(session.join(std::chrono::steady_clock::now() + 1s));
    const auto final = session.snapshot();
    EXPECT_FALSE(final.running);
    EXPECT_TRUE(final.completed);
    EXPECT_EQ(final.state.state, CameraState::disconnected);
    EXPECT_TRUE(queue.snapshot().closed);
    EXPECT_TRUE(pool.snapshot().closed);

    std::lock_guard lock{records_mutex};
    EXPECT_NE(std::find_if(records.begin(), records.end(),
                           [](const auto& record) {
                               return record.to == CameraState::recovering && record.cause &&
                                      record.cause->business_code == "CAMERA_DISCONNECTED";
                           }),
              records.end());
}

TEST(CameraRecoverySession, EntersFaultedAfterBoundedCreateRetries)
{
    using namespace paperbreak::camera::mock;
    MockCameraConfig config;
    config.descriptor = {"Mock", "PRESENT-0001", "192.0.2.1", "mock0"};
    auto provider_result = MockCameraProvider::create({config});
    ASSERT_TRUE(provider_result);
    auto provider = std::move(provider_result).value();
    FrameBufferPool pool{2U, config.width * config.height};
    AcquisitionQueue queue{1U};
    RecoveringCameraSession session{*provider,
                                    pool,
                                    queue,
                                    {.camera_id = "CAM01",
                                     .serial_number = "MISSING-0001",
                                     .receive_timeout = 1ms,
                                     .statistics_window = 1ms,
                                     .reconnect_policy = {.maximum_attempts = 2U}},
                                    {},
                                    [](const std::stop_token token, std::chrono::milliseconds) {
                                        return !token.stop_requested();
                                    }};

    ASSERT_TRUE(session.start());
    ASSERT_TRUE(wait_until([&] { return session.snapshot().state.state == CameraState::faulted; }));
    ASSERT_TRUE(session.join(std::chrono::steady_clock::now() + 1s));
    const auto snapshot = session.snapshot();
    EXPECT_EQ(snapshot.connection_attempts, 3U);
    EXPECT_EQ(snapshot.state.recovery_attempt, 2U);
    ASSERT_TRUE(snapshot.state.last_error);
    EXPECT_EQ(snapshot.state.last_error->business_code, "CAMERA_NOT_FOUND");
    EXPECT_FALSE(snapshot.state.last_error->retryable);
}

TEST(CameraRecoverySession, StopCancelsScheduledReconnectAndClosesFixedResources)
{
    using namespace paperbreak::camera::mock;
    MockCameraConfig config;
    config.descriptor = {"Mock", "PRESENT-0002", "192.0.2.2", "mock1"};
    auto provider_result = MockCameraProvider::create({config});
    ASSERT_TRUE(provider_result);
    auto provider = std::move(provider_result).value();
    FrameBufferPool pool{2U, config.width * config.height};
    AcquisitionQueue queue{1U};
    RecoveringCameraSession session{*provider,
                                    pool,
                                    queue,
                                    {.camera_id = "CAM02",
                                     .serial_number = "MISSING-0002",
                                     .receive_timeout = 5ms,
                                     .statistics_window = 5ms}};

    ASSERT_TRUE(session.start());
    ASSERT_TRUE(
        wait_until([&] { return session.snapshot().state.state == CameraState::recovering; }));
    const auto started = std::chrono::steady_clock::now();
    session.request_stop();
    EXPECT_TRUE(session.join(std::chrono::steady_clock::now() + 500ms));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
    EXPECT_EQ(session.snapshot().state.state, CameraState::disconnected);
    EXPECT_TRUE(pool.snapshot().closed);
    EXPECT_TRUE(queue.snapshot().closed);
}
