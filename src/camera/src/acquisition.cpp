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
        options_.statistics_window <= std::chrono::milliseconds::zero())
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
    auto window_started = std::chrono::steady_clock::now();
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
                    publish_rates(std::chrono::steady_clock::now());
                    continue;
                }
                finish(captured.error(), sequence_number);
                return;
            }

            ++sequence_number;
            const auto metadata = captured.value();
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

} // namespace paperbreak::camera
