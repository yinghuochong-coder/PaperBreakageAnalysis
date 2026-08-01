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
        if (closed_)
        {
            ++rejected_closed_;
            return FrameEnqueueStatus::closed;
        }
        if (size_ == slots_.size())
        {
            slots_[head_].reset();
            head_ = (head_ + 1U) % slots_.size();
            --size_;
            ++dropped_oldest_;
            status = FrameEnqueueStatus::enqueued_after_dropping_oldest;
        }
        const auto tail = (head_ + size_) % slots_.size();
        slots_[tail] = std::move(packet);
        ++size_;
        ++enqueued_;
        high_watermark_ = std::max(high_watermark_, size_);
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
        ++dequeued_;
        return {FrameDequeueStatus::frame, std::move(packet)};
    };

    if (stop_token.stop_requested())
    {
        ++wait_cancelled_;
        return {FrameDequeueStatus::stopped, std::nullopt};
    }
    if (size_ > 0U)
    {
        return pop_frame();
    }
    if (closed_)
    {
        return {FrameDequeueStatus::closed, std::nullopt};
    }
    if (timeout <= std::chrono::milliseconds::zero())
    {
        ++wait_timeouts_;
        return {FrameDequeueStatus::timeout, std::nullopt};
    }

    const bool ready =
        condition_.wait_for(lock, stop_token, timeout, [&] { return closed_ || size_ > 0U; });
    if (!ready)
    {
        if (stop_token.stop_requested())
        {
            ++wait_cancelled_;
            return {FrameDequeueStatus::stopped, std::nullopt};
        }
        ++wait_timeouts_;
        return {FrameDequeueStatus::timeout, std::nullopt};
    }
    if (stop_token.stop_requested())
    {
        ++wait_cancelled_;
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
        closed_ = true;
    }
    condition_.notify_all();
}

AcquisitionQueueSnapshot AcquisitionQueue::snapshot() const noexcept
{
    std::lock_guard lock{mutex_};
    return {.capacity = slots_.size(),
            .depth = size_,
            .high_watermark = high_watermark_,
            .enqueued = enqueued_,
            .dequeued = dequeued_,
            .dropped_oldest = dropped_oldest_,
            .rejected_closed = rejected_closed_,
            .wait_timeouts = wait_timeouts_,
            .wait_cancelled = wait_cancelled_,
            .closed = closed_};
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
    if (options_.camera_id.empty() || options_.receive_timeout <= std::chrono::milliseconds::zero())
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
    return {.started = started_,
            .running = running_,
            .completed = completed_,
            .last_sequence_number = last_sequence_number_,
            .last_error = last_error_};
}

void AcquisitionWorker::run(const std::stop_token stop_token) noexcept
{
    std::uint64_t sequence_number = 0U;
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
                continue;
            }

            auto captured = device_.capture_into(*acquired.buffer, options_.receive_timeout);
            if (!captured)
            {
                if (captured.error().business_code ==
                    camera_business_code(CameraErrorKind::frame_timeout))
                {
                    continue;
                }
                finish(captured.error(), sequence_number);
                return;
            }

            ++sequence_number;
            const auto metadata = captured.value();
            FramePacket packet{.camera_id = options_.camera_id,
                               .camera_frame_number = metadata.camera_frame_number,
                               .sequence_number = sequence_number,
                               .received_monotonic_time = std::chrono::steady_clock::now(),
                               .received_wall_clock_time = std::chrono::system_clock::now(),
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
