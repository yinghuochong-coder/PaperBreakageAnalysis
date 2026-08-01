#include "paperbreak/camera/acquisition.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace paperbreak::camera
{
namespace
{
Error acquisition_error(std::string code, const Severity severity, std::string message,
                        std::string operation, const std::string& camera_id)
{
    auto error = make_error(std::move(code), severity, std::move(message), "camera",
                            std::move(operation), false);
    error.source_id = camera_id;
    return error;
}
} // namespace

AcquisitionQueue::AcquisitionQueue(const std::size_t capacity) : slots_(capacity)
{
    if (capacity == 0U)
    {
        throw std::invalid_argument{"AcquisitionQueue capacity must be non-zero"};
    }
}

FrameEnqueueStatus AcquisitionQueue::push(FramePacket packet) noexcept
{
    FrameEnqueueStatus status = FrameEnqueueStatus::enqueued;
    {
        std::lock_guard lock{mutex_};
        if (closed_.load(std::memory_order_relaxed))
        {
            rejected_closed_.fetch_add(1U, std::memory_order_relaxed);
            return FrameEnqueueStatus::closed;
        }
        if (size_ == slots_.size())
        {
            slots_[head_].reset();
            head_ = (head_ + 1U) % slots_.size();
            --size_;
            dropped_oldest_.fetch_add(1U, std::memory_order_relaxed);
            status = FrameEnqueueStatus::enqueued_after_dropping_oldest;
        }
        const auto tail = (head_ + size_) % slots_.size();
        slots_[tail] = std::move(packet);
        ++size_;
        depth_.store(size_, std::memory_order_relaxed);
        enqueued_.fetch_add(1U, std::memory_order_relaxed);
        high_watermark_.store(std::max(high_watermark_.load(std::memory_order_relaxed), size_),
                              std::memory_order_relaxed);
    }
    condition_.notify_one();
    return status;
}

FrameDequeueResult AcquisitionQueue::wait_pop(const std::stop_token stop_token,
                                              const std::chrono::milliseconds timeout) noexcept
{
    std::unique_lock lock{mutex_};
    const auto pop_frame = [&]() -> FrameDequeueResult {
        auto packet = std::move(slots_[head_]);
        slots_[head_].reset();
        head_ = (head_ + 1U) % slots_.size();
        --size_;
        depth_.store(size_, std::memory_order_relaxed);
        dequeued_.fetch_add(1U, std::memory_order_relaxed);
        return {FrameDequeueStatus::frame, std::move(packet)};
    };

    if (stop_token.stop_requested())
    {
        wait_cancelled_.fetch_add(1U, std::memory_order_relaxed);
        return {FrameDequeueStatus::stopped, std::nullopt};
    }
    if (size_ > 0U)
    {
        return pop_frame();
    }
    if (closed_.load(std::memory_order_relaxed))
    {
        return {FrameDequeueStatus::closed, std::nullopt};
    }
    if (timeout <= std::chrono::milliseconds::zero())
    {
        wait_timeouts_.fetch_add(1U, std::memory_order_relaxed);
        return {FrameDequeueStatus::timeout, std::nullopt};
    }

    const bool ready = condition_.wait_for(lock, stop_token, timeout, [&] {
        return closed_.load(std::memory_order_relaxed) || size_ > 0U;
    });
    if (!ready)
    {
        if (stop_token.stop_requested())
        {
            wait_cancelled_.fetch_add(1U, std::memory_order_relaxed);
            return {FrameDequeueStatus::stopped, std::nullopt};
        }
        wait_timeouts_.fetch_add(1U, std::memory_order_relaxed);
        return {FrameDequeueStatus::timeout, std::nullopt};
    }
    if (stop_token.stop_requested())
    {
        wait_cancelled_.fetch_add(1U, std::memory_order_relaxed);
        return {FrameDequeueStatus::stopped, std::nullopt};
    }
    if (size_ > 0U)
    {
        return pop_frame();
    }
    return {FrameDequeueStatus::closed, std::nullopt};
}

void AcquisitionQueue::close() noexcept
{
    {
        std::lock_guard lock{mutex_};
        closed_.store(true, std::memory_order_release);
    }
    condition_.notify_all();
}

AcquisitionQueueSnapshot AcquisitionQueue::snapshot() const noexcept
{
    return {.capacity = slots_.size(),
            .depth = depth_.load(std::memory_order_relaxed),
            .high_watermark = high_watermark_.load(std::memory_order_relaxed),
            .enqueued = enqueued_.load(std::memory_order_relaxed),
            .dequeued = dequeued_.load(std::memory_order_relaxed),
            .dropped_oldest = dropped_oldest_.load(std::memory_order_relaxed),
            .rejected_closed = rejected_closed_.load(std::memory_order_relaxed),
            .wait_timeouts = wait_timeouts_.load(std::memory_order_relaxed),
            .wait_cancelled = wait_cancelled_.load(std::memory_order_relaxed),
            .closed = closed_.load(std::memory_order_acquire)};
}

AcquisitionWorker::AcquisitionWorker(ICameraDevice& device, FrameBufferPool& pool,
                                     AcquisitionQueue& queue, AcquisitionWorkerOptions options)
    : device_(device), pool_(pool), queue_(queue), options_(std::move(options))
{
}

AcquisitionWorker::~AcquisitionWorker()
{
    request_stop();
    std::unique_lock lock{mutex_};
    condition_.wait(lock, [this] { return completed_; });
    if (worker_.joinable())
    {
        worker_.join();
    }
}

Result<void> AcquisitionWorker::start()
{
    std::lock_guard lock{mutex_};
    if (options_.camera_id.empty() ||
        options_.receive_timeout <= std::chrono::milliseconds::zero() ||
        options_.statistics_window <= std::chrono::milliseconds::zero() ||
        options_.consecutive_timeout_limit == 0U ||
        (options_.software_trigger_interval &&
         *options_.software_trigger_interval <= std::chrono::milliseconds::zero()))
    {
        return Result<void>::failure(
            acquisition_error("CAMERA_CONFIG_FAILED", Severity::error, "采集工作线程配置无效",
                              "camera.acquisition.start", options_.camera_id));
    }
    if (started_)
    {
        return Result<void>::failure(acquisition_error(
            "CAMERA_INVALID_STATE_TRANSITION", Severity::error, "采集工作线程不能重复启动",
            "camera.acquisition.start", options_.camera_id));
    }

    started_ = true;
    running_ = true;
    completed_ = false;
    last_sequence_number_ = 0U;
    last_error_.reset();
    frames_received_.store(0U, std::memory_order_relaxed);
    camera_frame_gaps_.store(0U, std::memory_order_relaxed);
    capture_timeouts_.store(0U, std::memory_order_relaxed);
    incomplete_frames_.store(0U, std::memory_order_relaxed);
    bytes_received_.store(0U, std::memory_order_relaxed);
    actual_fps_.store(0.0, std::memory_order_relaxed);
    bandwidth_bytes_per_second_.store(0.0, std::memory_order_relaxed);
    has_last_frame_.store(false, std::memory_order_relaxed);
    try
    {
        worker_ = std::jthread([this](const std::stop_token stop_token) { run(stop_token); });
    }
    catch (const std::exception& exception)
    {
        started_ = false;
        running_ = false;
        completed_ = true;
        auto error =
            acquisition_error("CAMERA_OPEN_FAILED", Severity::error, "无法创建采集工作线程",
                              "camera.acquisition.start", options_.camera_id);
        error.details.push_back({"exception", exception.what()});
        return Result<void>::failure(std::move(error));
    }
    return Result<void>::success();
}

void AcquisitionWorker::request_stop() noexcept
{
    std::lock_guard lock{mutex_};
    if (worker_.joinable())
    {
        worker_.request_stop();
    }
}

Result<void> AcquisitionWorker::join(const std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lock{mutex_};
    if (!started_)
    {
        return Result<void>::success();
    }
    if (!condition_.wait_until(lock, deadline, [this] { return completed_; }))
    {
        return Result<void>::failure(acquisition_error(
            "SYS_SHUTDOWN_TIMEOUT", Severity::critical, "采集工作线程未在截止时间内退出",
            "camera.acquisition.join", options_.camera_id));
    }
    if (worker_.joinable())
    {
        worker_.join();
    }
    return Result<void>::success();
}

bool AcquisitionWorker::wait_until_completed(
    const std::chrono::steady_clock::time_point deadline) noexcept
{
    std::unique_lock lock{mutex_};
    return condition_.wait_until(lock, deadline, [this] { return completed_; });
}

AcquisitionWorkerSnapshot AcquisitionWorker::snapshot() const
{
    std::lock_guard lock{mutex_};
    AcquisitionWorkerSnapshot result{
        .started = started_,
        .running = running_,
        .completed = completed_,
        .last_sequence_number = last_sequence_number_,
        .frames_received = frames_received_.load(std::memory_order_relaxed),
        .camera_frame_gaps = camera_frame_gaps_.load(std::memory_order_relaxed),
        .capture_timeouts = capture_timeouts_.load(std::memory_order_relaxed),
        .incomplete_frames = incomplete_frames_.load(std::memory_order_relaxed),
        .bytes_received = bytes_received_.load(std::memory_order_relaxed),
        .actual_fps = actual_fps_.load(std::memory_order_relaxed),
        .bandwidth_bytes_per_second = bandwidth_bytes_per_second_.load(std::memory_order_relaxed),
        .last_error = last_error_};
    if (has_last_frame_.load(std::memory_order_acquire))
    {
        result.last_frame_monotonic_time = MonotonicTime{
            MonotonicTime::duration{last_frame_monotonic_ticks_.load(std::memory_order_relaxed)}};
        result.last_frame_wall_clock_time = WallClockTime{
            WallClockTime::duration{last_frame_wall_clock_ticks_.load(std::memory_order_relaxed)}};
    }
    return result;
}

void AcquisitionWorker::run(const std::stop_token stop_token) noexcept
{
    std::uint64_t sequence_number = 0U;
    std::uint64_t previous_camera_frame_number = 0U;
    std::uint64_t window_frames = 0U;
    std::uint64_t window_bytes = 0U;
    std::size_t consecutive_timeouts = 0U;
    std::optional<FrameGeometry> expected_geometry;
    std::optional<PixelFormat> expected_pixel_format;
    auto window_started = std::chrono::steady_clock::now();
    auto next_software_trigger = window_started;
    std::mutex trigger_wait_mutex;
    const auto publish_rates = [&](const std::chrono::steady_clock::time_point now) {
        const auto elapsed = now - window_started;
        if (elapsed < options_.statistics_window)
        {
            return;
        }
        const auto seconds = std::chrono::duration<double>{elapsed}.count();
        actual_fps_.store(static_cast<double>(window_frames) / seconds, std::memory_order_relaxed);
        bandwidth_bytes_per_second_.store(static_cast<double>(window_bytes) / seconds,
                                          std::memory_order_relaxed);
        window_frames = 0U;
        window_bytes = 0U;
        window_started = now;
    };
    try
    {
        while (!stop_token.stop_requested())
        {
            if (options_.software_trigger_interval)
            {
                std::unique_lock wait_lock{trigger_wait_mutex};
                if (trigger_wait_condition_.wait_until(
                        wait_lock, stop_token, next_software_trigger,
                        [&stop_token] { return stop_token.stop_requested(); }))
                {
                    break;
                }
                if (auto triggered = device_.software_trigger(); !triggered)
                {
                    finish(triggered.error(), sequence_number);
                    return;
                }
                next_software_trigger =
                    std::chrono::steady_clock::now() + *options_.software_trigger_interval;
            }

            auto acquired = pool_.acquire(stop_token, options_.receive_timeout);
            if (acquired.status == FramePoolAcquireStatus::stopped ||
                acquired.status == FramePoolAcquireStatus::closed)
            {
                break;
            }
            if (acquired.status != FramePoolAcquireStatus::acquired)
            {
                publish_rates(std::chrono::steady_clock::now());
                continue;
            }

            auto captured = device_.capture_into(*acquired.buffer, options_.receive_timeout);
            if (!captured)
            {
                if (captured.error().business_code ==
                    camera_business_code(CameraErrorKind::frame_timeout))
                {
                    capture_timeouts_.fetch_add(1U, std::memory_order_relaxed);
                    ++consecutive_timeouts;
                    publish_rates(std::chrono::steady_clock::now());
                    if (consecutive_timeouts >= options_.consecutive_timeout_limit)
                    {
                        auto error = captured.error();
                        error.details.push_back(
                            {"consecutiveTimeouts", std::to_string(consecutive_timeouts)});
                        error.details.push_back(
                            {"timeoutLimit", std::to_string(options_.consecutive_timeout_limit)});
                        finish(std::move(error), sequence_number);
                        return;
                    }
                    continue;
                }
                finish(captured.error(), sequence_number);
                return;
            }

            ++sequence_number;
            const auto metadata = captured.value();
            consecutive_timeouts = 0U;
            if (!expected_geometry)
            {
                expected_geometry = metadata.geometry;
                expected_pixel_format = metadata.pixel_format;
            }
            else if (metadata.geometry != *expected_geometry ||
                     metadata.pixel_format != *expected_pixel_format)
            {
                auto error = make_camera_error(
                    CameraErrorKind::frame_format_changed, "采集期间帧尺寸或像素格式发生变化",
                    "camera.acquisition.capture", options_.camera_id,
                    {{"expectedWidth", std::to_string(expected_geometry->width)},
                     {"expectedHeight", std::to_string(expected_geometry->height)},
                     {"expectedStride", std::to_string(expected_geometry->stride)},
                     {"actualWidth", std::to_string(metadata.geometry.width)},
                     {"actualHeight", std::to_string(metadata.geometry.height)},
                     {"actualStride", std::to_string(metadata.geometry.stride)},
                     {"expectedPixelFormat",
                      std::to_string(static_cast<unsigned int>(*expected_pixel_format))},
                     {"actualPixelFormat",
                      std::to_string(static_cast<unsigned int>(metadata.pixel_format))}});
                finish(std::move(error), sequence_number - 1U);
                return;
            }
            const auto received_monotonic_time = std::chrono::steady_clock::now();
            const auto received_wall_clock_time = std::chrono::system_clock::now();
            if (previous_camera_frame_number > 0U &&
                metadata.camera_frame_number > previous_camera_frame_number &&
                metadata.camera_frame_number - previous_camera_frame_number > 1U)
            {
                camera_frame_gaps_.fetch_add(metadata.camera_frame_number -
                                                 previous_camera_frame_number - 1U,
                                             std::memory_order_relaxed);
            }
            previous_camera_frame_number = metadata.camera_frame_number;
            frames_received_.fetch_add(1U, std::memory_order_relaxed);
            bytes_received_.fetch_add(acquired.buffer->size(), std::memory_order_relaxed);
            if (metadata.flags.incomplete)
            {
                incomplete_frames_.fetch_add(1U, std::memory_order_relaxed);
            }
            ++window_frames;
            window_bytes += acquired.buffer->size();
            last_frame_monotonic_ticks_.store(received_monotonic_time.time_since_epoch().count(),
                                              std::memory_order_relaxed);
            last_frame_wall_clock_ticks_.store(received_wall_clock_time.time_since_epoch().count(),
                                               std::memory_order_relaxed);
            has_last_frame_.store(true, std::memory_order_release);
            publish_rates(received_monotonic_time);
            FramePacket packet{.camera_id = options_.camera_id,
                               .camera_frame_number = metadata.camera_frame_number,
                               .sequence_number = sequence_number,
                               .received_monotonic_time = received_monotonic_time,
                               .received_wall_clock_time = received_wall_clock_time,
                               .camera_timestamp = metadata.camera_timestamp,
                               .geometry = metadata.geometry,
                               .pixel_format = metadata.pixel_format,
                               .buffer = std::move(acquired.buffer),
                               .flags = metadata.flags};
            if (queue_.push(std::move(packet)) == FrameEnqueueStatus::closed)
            {
                break;
            }
        }
    }
    catch (const std::exception& exception)
    {
        auto error =
            acquisition_error("CAMERA_CONFIG_FAILED", Severity::error, "采集设备调用引发异常",
                              "camera.acquisition.capture", options_.camera_id);
        error.details.push_back({"exception", exception.what()});
        finish(std::move(error), sequence_number);
        return;
    }
    catch (...)
    {
        finish(acquisition_error("CAMERA_CONFIG_FAILED", Severity::error,
                                 "采集设备调用引发未知异常", "camera.acquisition.capture",
                                 options_.camera_id),
               sequence_number);
        return;
    }
    finish(std::nullopt, sequence_number);
}

void AcquisitionWorker::finish(std::optional<Error> error,
                               const std::uint64_t last_sequence_number) noexcept
{
    {
        std::lock_guard lock{mutex_};
        running_ = false;
        completed_ = true;
        last_sequence_number_ = last_sequence_number;
        last_error_ = std::move(error);
    }
    condition_.notify_all();
}

RecoveringCameraSession::RecoveringCameraSession(ICameraProvider& provider, FrameBufferPool& pool,
                                                 AcquisitionQueue& queue,
                                                 RecoveringCameraSessionOptions options,
                                                 CameraTransitionObserver observer,
                                                 ReconnectWaiter waiter)
    : provider_(provider), pool_(pool), queue_(queue), options_(std::move(options)),
      controller_(options_.camera_id, true, options_.reconnect_policy, std::move(observer),
                  std::move(waiter))
{
}

RecoveringCameraSession::~RecoveringCameraSession()
{
    request_stop();
    std::unique_lock lock{mutex_};
    condition_.wait(lock, [this] { return completed_; });
    if (worker_.joinable())
    {
        worker_.join();
    }
}

Result<void> RecoveringCameraSession::start()
{
    std::lock_guard lock{mutex_};
    if (options_.camera_id.empty() || options_.serial_number.empty() ||
        options_.receive_timeout <= std::chrono::milliseconds::zero() ||
        options_.statistics_window <= std::chrono::milliseconds::zero() ||
        options_.consecutive_timeout_limit == 0U ||
        !validate_reconnect_policy(options_.reconnect_policy))
    {
        return Result<void>::failure(make_camera_error(
            CameraErrorKind::config_failed, "相机恢复会话配置无效", "camera.session.start",
            options_.camera_id, {{"reason", "invalid-session-options"}}));
    }
    if (started_)
    {
        return Result<void>::failure(make_camera_error(
            CameraErrorKind::invalid_state_transition, "相机恢复会话不能重复启动",
            "camera.session.start", options_.camera_id, {{"reason", "already-started"}}));
    }
    started_ = true;
    running_ = true;
    completed_ = false;
    connection_attempts_.store(0U, std::memory_order_relaxed);
    try
    {
        worker_ = std::jthread([this](const std::stop_token token) { run(token); });
    }
    catch (const std::exception& exception)
    {
        started_ = false;
        running_ = false;
        completed_ = true;
        auto error = make_camera_error(CameraErrorKind::open_failed, "无法创建相机恢复线程",
                                       "camera.session.start", options_.camera_id);
        error.details.push_back({"exception", exception.what()});
        return Result<void>::failure(std::move(error));
    }
    return Result<void>::success();
}

void RecoveringCameraSession::request_stop() noexcept
{
    bool request_controller_stop{};
    {
        std::lock_guard lock{mutex_};
        if (!started_ || completed_)
        {
            return;
        }
        request_controller_stop = true;
        if (worker_.joinable())
        {
            worker_.request_stop();
        }
        if (active_worker_ != nullptr)
        {
            active_worker_->request_stop();
        }
    }
    pool_.close();
    queue_.close();
    if (request_controller_stop)
    {
        static_cast<void>(controller_.request_stop(true));
    }
}

Result<void> RecoveringCameraSession::join(const std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lock{mutex_};
    if (!started_)
    {
        return Result<void>::success();
    }
    if (!condition_.wait_until(lock, deadline, [this] { return completed_; }))
    {
        return Result<void>::failure(acquisition_error("SYS_SHUTDOWN_TIMEOUT", Severity::critical,
                                                       "相机恢复会话未在截止时间内退出",
                                                       "camera.session.join", options_.camera_id));
    }
    if (worker_.joinable())
    {
        worker_.join();
    }
    return Result<void>::success();
}

RecoveringCameraSessionSnapshot RecoveringCameraSession::snapshot() const
{
    std::lock_guard lock{mutex_};
    return {.started = started_,
            .running = running_,
            .completed = completed_,
            .state = controller_.snapshot(),
            .acquisition =
                active_worker_ != nullptr ? active_worker_->snapshot() : last_acquisition_,
            .connection_attempts = connection_attempts_.load(std::memory_order_relaxed)};
}

bool RecoveringCameraSession::recover_from(Error error, std::string reason,
                                           const std::stop_token stop_token) noexcept
{
    error.source_id = options_.camera_id;
    error.details.push_back(
        {"attempt", std::to_string(connection_attempts_.load(std::memory_order_relaxed))});
    if (!controller_.handle_failure(std::move(error), std::move(reason)))
    {
        return false;
    }
    if (controller_.snapshot().state != CameraState::recovering)
    {
        return false;
    }
    return controller_.wait_for_retry(stop_token) == ReconnectWaitResult::ready;
}

void RecoveringCameraSession::run(const std::stop_token stop_token) noexcept
{
    try
    {
        if (!controller_.transition_to(CameraState::connecting, "session-start"))
        {
            finish();
            return;
        }
        while (!stop_token.stop_requested())
        {
            connection_attempts_.fetch_add(1U, std::memory_order_relaxed);
            auto created = provider_.create_device(options_.serial_number);
            if (!created)
            {
                if (!recover_from(created.error(), "create-device-failed", stop_token))
                    break;
                continue;
            }
            auto device = std::move(created).value();
            auto connected = device->connect();
            if (!connected)
            {
                static_cast<void>(device->disconnect());
                if (!recover_from(connected.error(), "connect-failed", stop_token))
                    break;
                continue;
            }
            if (!controller_.transition_to(CameraState::connected, "device-connected") ||
                !controller_.transition_to(CameraState::starting, "stream-starting"))
            {
                static_cast<void>(device->disconnect());
                break;
            }
            auto stream_started = device->start_acquisition();
            if (!stream_started)
            {
                static_cast<void>(device->stop_acquisition());
                static_cast<void>(device->disconnect());
                if (!recover_from(stream_started.error(), "stream-start-failed", stop_token))
                    break;
                continue;
            }
            if (!controller_.transition_to(CameraState::streaming, "stream-started"))
            {
                static_cast<void>(device->stop_acquisition());
                static_cast<void>(device->disconnect());
                break;
            }

            AcquisitionWorker acquisition{
                *device,
                pool_,
                queue_,
                {.camera_id = options_.camera_id,
                 .receive_timeout = options_.receive_timeout,
                 .statistics_window = options_.statistics_window,
                 .consecutive_timeout_limit = options_.consecutive_timeout_limit}};
            {
                std::lock_guard lock{mutex_};
                active_worker_ = &acquisition;
            }
            auto acquisition_started = acquisition.start();
            if (acquisition_started)
            {
                while (true)
                {
                    if (stop_token.stop_requested())
                    {
                        acquisition.request_stop();
                    }
                    if (acquisition.wait_until_completed(std::chrono::steady_clock::now() +
                                                         std::chrono::milliseconds{20}))
                    {
                        break;
                    }
                }
                static_cast<void>(acquisition.join(std::chrono::steady_clock::now()));
            }
            auto acquisition_snapshot = acquisition.snapshot();
            {
                std::lock_guard lock{mutex_};
                last_acquisition_ = acquisition_snapshot;
                active_worker_ = nullptr;
            }
            auto stopped = device->stop_acquisition();
            auto disconnected = device->disconnect();

            if (stop_token.stop_requested() || queue_.snapshot().closed)
            {
                break;
            }
            Error failure = acquisition_started
                                ? acquisition_snapshot.last_error.value_or(make_camera_error(
                                      CameraErrorKind::disconnected, "采集工作线程意外停止",
                                      "camera.session.capture", options_.camera_id,
                                      {{"reason", "worker-completed-without-error"}}))
                                : acquisition_started.error();
            if (!stopped && !acquisition_snapshot.last_error)
                failure = stopped.error();
            if (!disconnected && !acquisition_snapshot.last_error && stopped)
                failure = disconnected.error();
            if (!recover_from(std::move(failure), "capture-failed", stop_token))
                break;
        }
    }
    catch (const std::exception& exception)
    {
        auto error = make_camera_error(CameraErrorKind::disconnected, "相机恢复会话发生异常",
                                       "camera.session.run", options_.camera_id);
        error.details.push_back({"exception", exception.what()});
        static_cast<void>(controller_.handle_failure(std::move(error), "session-exception"));
    }
    catch (...)
    {
        static_cast<void>(controller_.handle_failure(
            make_camera_error(CameraErrorKind::disconnected, "相机恢复会话发生未知异常",
                              "camera.session.run", options_.camera_id),
            "session-unknown-exception"));
    }
    finish();
}

void RecoveringCameraSession::finish() noexcept
{
    queue_.close();
    pool_.close();
    {
        std::lock_guard lock{mutex_};
        active_worker_ = nullptr;
        running_ = false;
        completed_ = true;
    }
    condition_.notify_all();
}

} // namespace paperbreak::camera
