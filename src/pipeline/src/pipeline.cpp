#include "paperbreak/pipeline/pipeline.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace paperbreak::pipeline
{
namespace
{
Error pipeline_error(std::string code, const Severity severity, std::string message,
                     std::string operation, const std::string& camera_id, std::string reason = {})
{
    auto error = make_error(std::move(code), severity, std::move(message), "pipeline",
                            std::move(operation), false);
    if (!camera_id.empty())
    {
        error.source_id = camera_id;
    }
    if (!reason.empty())
    {
        error.details.push_back({"reason", std::move(reason)});
    }
    return error;
}

std::size_t bytes_per_pixel(const camera::PixelFormat format) noexcept
{
    switch (format)
    {
    case camera::PixelFormat::mono8:
    case camera::PixelFormat::bayer_rg8:
        return 1U;
    case camera::PixelFormat::mono10:
    case camera::PixelFormat::mono12:
        return 2U;
    }
    return 0U;
}

bool is_invalid_frame_error(const Error& error) noexcept
{
    return error.business_code == "CAMERA_FRAME_INCOMPLETE" ||
           error.business_code == "CAMERA_FRAME_FORMAT_CHANGED" ||
           error.business_code == "PIPELINE_FRAME_ORDER_VIOLATION" ||
           error.business_code == "CAMERA_CONFIG_FAILED";
}
} // namespace

std::string_view PassThroughNode::name() const noexcept
{
    return "pass-through";
}

Result<void> PassThroughNode::process(ProcessedFrame& frame)
{
    static_cast<void>(frame);
    return Result<void>::success();
}

ValidityCheckNode::ValidityCheckNode(ValidityCheckOptions options) : options_(std::move(options)) {}

std::string_view ValidityCheckNode::name() const noexcept
{
    return "validity-check";
}

Result<void> ValidityCheckNode::process(ProcessedFrame& frame)
{
    if (options_.reject_incomplete && frame.frame.flags().incomplete)
    {
        return Result<void>::failure(pipeline_error("CAMERA_FRAME_INCOMPLETE", Severity::warning,
                                                    "图像帧不完整", "pipeline.validity-check",
                                                    frame.frame.camera_id(), "incomplete-frame"));
    }
    if (options_.expected_geometry && frame.frame.geometry() != *options_.expected_geometry)
    {
        return Result<void>::failure(
            pipeline_error("CAMERA_FRAME_FORMAT_CHANGED", Severity::error, "图像尺寸发生非预期变化",
                           "pipeline.validity-check", frame.frame.camera_id(), "geometry-changed"));
    }
    if (options_.expected_pixel_format &&
        frame.frame.pixel_format() != *options_.expected_pixel_format)
    {
        return Result<void>::failure(pipeline_error(
            "CAMERA_FRAME_FORMAT_CHANGED", Severity::error, "图像像素格式发生非预期变化",
            "pipeline.validity-check", frame.frame.camera_id(), "pixel-format-changed"));
    }

    const auto geometry = frame.frame.geometry();
    const auto pixel_bytes = bytes_per_pixel(frame.frame.pixel_format());
    if (pixel_bytes == 0U ||
        geometry.width > std::numeric_limits<std::uint32_t>::max() / pixel_bytes ||
        geometry.stride < geometry.width * pixel_bytes)
    {
        return Result<void>::failure(pipeline_error(
            "CAMERA_FRAME_FORMAT_CHANGED", Severity::error, "图像行跨度与像素格式不匹配",
            "pipeline.validity-check", frame.frame.camera_id(), "invalid-stride"));
    }
    frame.validity_checked = true;
    return Result<void>::success();
}

std::string_view GrayStatisticsNode::name() const noexcept
{
    return "gray-statistics";
}

Result<void> GrayStatisticsNode::process(ProcessedFrame& frame)
{
    const auto geometry = frame.frame.geometry();
    const auto bytes = frame.frame.bytes();
    const auto pixel_bytes = bytes_per_pixel(frame.frame.pixel_format());
    if (pixel_bytes == 0U || geometry.width == 0U || geometry.height == 0U ||
        geometry.width > std::numeric_limits<std::uint32_t>::max() / pixel_bytes ||
        geometry.stride < geometry.width * pixel_bytes)
    {
        return Result<void>::failure(
            pipeline_error("CAMERA_FRAME_FORMAT_CHANGED", Severity::error, "无法统计非法图像布局",
                           "pipeline.gray-statistics", frame.frame.camera_id(), "invalid-layout"));
    }

    double minimum = 1.0;
    double maximum = 0.0;
    double sum = 0.0;
    const auto sample_count = static_cast<std::uint64_t>(geometry.width) * geometry.height;
    const auto maximum_value = frame.frame.pixel_format() == camera::PixelFormat::mono10   ? 1023U
                               : frame.frame.pixel_format() == camera::PixelFormat::mono12 ? 4095U
                                                                                           : 255U;

    for (std::uint32_t row = 0U; row < geometry.height; ++row)
    {
        const auto row_offset = static_cast<std::size_t>(row) * geometry.stride;
        for (std::uint32_t column = 0U; column < geometry.width; ++column)
        {
            const auto offset = row_offset + static_cast<std::size_t>(column) * pixel_bytes;
            std::uint32_t value = std::to_integer<std::uint8_t>(bytes[offset]);
            if (pixel_bytes == 2U)
            {
                value |=
                    static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U]))
                    << 8U;
                value &= maximum_value;
            }
            const auto normalized = static_cast<double>(value) / maximum_value;
            minimum = std::min(minimum, normalized);
            maximum = std::max(maximum, normalized);
            sum += normalized;
        }
    }
    frame.gray_statistics = GrayStatistics{minimum, maximum, sum / sample_count, sample_count};
    return Result<void>::success();
}

PreprocessingChain::PreprocessingChain(std::vector<std::unique_ptr<IPreprocessingNode>> nodes)
    : nodes_(std::move(nodes))
{
    if (nodes_.empty() || nodes_.size() > 32U ||
        std::ranges::any_of(nodes_, [](const auto& node) { return node == nullptr; }))
    {
        throw std::invalid_argument{"PreprocessingChain requires 1..32 non-null nodes"};
    }
}

Result<void> PreprocessingChain::process(ProcessedFrame& frame)
{
    for (const auto& node : nodes_)
    {
        auto result = node->process(frame);
        if (!result)
        {
            return result;
        }
    }
    return Result<void>::success();
}

std::size_t PreprocessingChain::size() const noexcept
{
    return nodes_.size();
}

AlgorithmQueue::AlgorithmQueue(const std::size_t capacity) : slots_(capacity)
{
    if (capacity == 0U)
    {
        throw std::invalid_argument{"AlgorithmQueue capacity must be non-zero"};
    }
}

AlgorithmEnqueueStatus AlgorithmQueue::push(ProcessedFrame frame) noexcept
{
    AlgorithmEnqueueStatus status = AlgorithmEnqueueStatus::enqueued;
    {
        std::lock_guard lock{mutex_};
        if (closed_.load(std::memory_order_relaxed))
        {
            rejected_closed_.fetch_add(1U, std::memory_order_relaxed);
            return AlgorithmEnqueueStatus::closed;
        }
        if (size_ == slots_.size())
        {
            slots_[head_].reset();
            head_ = (head_ + 1U) % slots_.size();
            --size_;
            algorithm_skipped_.fetch_add(1U, std::memory_order_relaxed);
            status = AlgorithmEnqueueStatus::enqueued_after_skipping_oldest;
        }
        const auto tail = (head_ + size_) % slots_.size();
        slots_[tail] = std::move(frame);
        ++size_;
        depth_.store(size_, std::memory_order_relaxed);
        enqueued_.fetch_add(1U, std::memory_order_relaxed);
        high_watermark_.store(std::max(high_watermark_.load(std::memory_order_relaxed), size_),
                              std::memory_order_relaxed);
    }
    condition_.notify_one();
    return status;
}

AlgorithmDequeueResult AlgorithmQueue::wait_pop(const std::stop_token stop_token,
                                                const std::chrono::milliseconds timeout) noexcept
{
    std::unique_lock lock{mutex_};
    const auto pop_frame = [&]() -> AlgorithmDequeueResult {
        auto frame = std::move(slots_[head_]);
        slots_[head_].reset();
        head_ = (head_ + 1U) % slots_.size();
        --size_;
        depth_.store(size_, std::memory_order_relaxed);
        dequeued_.fetch_add(1U, std::memory_order_relaxed);
        return {AlgorithmDequeueStatus::frame, std::move(frame)};
    };

    if (stop_token.stop_requested())
    {
        wait_cancelled_.fetch_add(1U, std::memory_order_relaxed);
        return {AlgorithmDequeueStatus::stopped, std::nullopt};
    }
    if (size_ > 0U)
    {
        return pop_frame();
    }
    if (closed_.load(std::memory_order_relaxed))
    {
        return {AlgorithmDequeueStatus::closed, std::nullopt};
    }
    if (timeout <= std::chrono::milliseconds::zero())
    {
        wait_timeouts_.fetch_add(1U, std::memory_order_relaxed);
        return {AlgorithmDequeueStatus::timeout, std::nullopt};
    }
    const bool ready = condition_.wait_for(lock, stop_token, timeout, [&] {
        return closed_.load(std::memory_order_relaxed) || size_ > 0U;
    });
    if (!ready)
    {
        if (stop_token.stop_requested())
        {
            wait_cancelled_.fetch_add(1U, std::memory_order_relaxed);
            return {AlgorithmDequeueStatus::stopped, std::nullopt};
        }
        wait_timeouts_.fetch_add(1U, std::memory_order_relaxed);
        return {AlgorithmDequeueStatus::timeout, std::nullopt};
    }
    if (stop_token.stop_requested())
    {
        wait_cancelled_.fetch_add(1U, std::memory_order_relaxed);
        return {AlgorithmDequeueStatus::stopped, std::nullopt};
    }
    if (size_ > 0U)
    {
        return pop_frame();
    }
    return {AlgorithmDequeueStatus::closed, std::nullopt};
}

void AlgorithmQueue::close() noexcept
{
    {
        std::lock_guard lock{mutex_};
        closed_.store(true, std::memory_order_release);
    }
    condition_.notify_all();
}

AlgorithmQueueSnapshot AlgorithmQueue::snapshot() const noexcept
{
    return {.capacity = slots_.size(),
            .depth = depth_.load(std::memory_order_relaxed),
            .high_watermark = high_watermark_.load(std::memory_order_relaxed),
            .enqueued = enqueued_.load(std::memory_order_relaxed),
            .dequeued = dequeued_.load(std::memory_order_relaxed),
            .algorithm_skipped = algorithm_skipped_.load(std::memory_order_relaxed),
            .rejected_closed = rejected_closed_.load(std::memory_order_relaxed),
            .wait_timeouts = wait_timeouts_.load(std::memory_order_relaxed),
            .wait_cancelled = wait_cancelled_.load(std::memory_order_relaxed),
            .closed = closed_.load(std::memory_order_acquire)};
}

PerCameraProcessor::PerCameraProcessor(camera::AcquisitionQueue& input, AlgorithmQueue& output,
                                       PreprocessingChain chain, PerCameraProcessorOptions options)
    : input_(input), output_(output), chain_(std::move(chain)), options_(std::move(options))
{
}

PerCameraProcessor::~PerCameraProcessor()
{
    request_stop();
    std::unique_lock lock{mutex_};
    condition_.wait(lock, [this] { return completed_; });
    if (worker_.joinable())
    {
        worker_.join();
    }
}

Result<void> PerCameraProcessor::start()
{
    std::lock_guard lock{mutex_};
    if (options_.camera_id.empty() ||
        options_.input_wait_timeout <= std::chrono::milliseconds::zero())
    {
        return Result<void>::failure(
            pipeline_error("CAMERA_CONFIG_FAILED", Severity::error, "处理执行器配置无效",
                           "pipeline.processor.start", options_.camera_id, "invalid-options"));
    }
    if (started_)
    {
        return Result<void>::failure(pipeline_error(
            "CAMERA_INVALID_STATE_TRANSITION", Severity::error, "处理执行器不能重复启动",
            "pipeline.processor.start", options_.camera_id, "already-started"));
    }
    started_ = true;
    running_ = true;
    completed_ = false;
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
            pipeline_error("CAMERA_OPEN_FAILED", Severity::error, "无法创建处理工作线程",
                           "pipeline.processor.start", options_.camera_id, "thread-create-failed");
        error.details.push_back({"exception", exception.what()});
        return Result<void>::failure(std::move(error));
    }
    return Result<void>::success();
}

void PerCameraProcessor::request_stop() noexcept
{
    std::lock_guard lock{mutex_};
    if (worker_.joinable())
    {
        worker_.request_stop();
    }
}

Result<void> PerCameraProcessor::join(const std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lock{mutex_};
    if (!started_)
    {
        return Result<void>::success();
    }
    if (!condition_.wait_until(lock, deadline, [this] { return completed_; }))
    {
        return Result<void>::failure(pipeline_error(
            "SYS_SHUTDOWN_TIMEOUT", Severity::critical, "处理工作线程未在截止时间内退出",
            "pipeline.processor.join", options_.camera_id, "deadline-exceeded"));
    }
    if (worker_.joinable())
    {
        worker_.join();
    }
    return Result<void>::success();
}

PerCameraProcessorSnapshot PerCameraProcessor::snapshot() const
{
    std::lock_guard lock{mutex_};
    const auto processed = frames_processed_.load(std::memory_order_relaxed);
    const auto rejected = frames_rejected_.load(std::memory_order_relaxed);
    const auto attempts = processed + rejected;
    return {
        .started = started_,
        .running = running_,
        .completed = completed_,
        .frames_processed = processed,
        .frames_rejected = rejected,
        .invalid_frames = invalid_frames_.load(std::memory_order_relaxed),
        .sequence_gaps = sequence_gaps_.load(std::memory_order_relaxed),
        .node_failures = node_failures_.load(std::memory_order_relaxed),
        .average_processing_microseconds =
            attempts == 0U ? 0.0
                           : static_cast<double>(
                                 total_processing_nanoseconds_.load(std::memory_order_relaxed)) /
                                 static_cast<double>(attempts) / 1000.0,
        .maximum_processing_microseconds =
            static_cast<double>(maximum_processing_nanoseconds_.load(std::memory_order_relaxed)) /
            1000.0,
        .algorithm_queue = output_.snapshot(),
        .last_error = last_error_};
}

const std::string& PerCameraProcessor::camera_id() const noexcept
{
    return options_.camera_id;
}

void PerCameraProcessor::run(const std::stop_token stop_token) noexcept
{
    std::string thread_camera_id = options_.camera_id;
    std::ranges::transform(
        thread_camera_id, thread_camera_id.begin(),
        [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
    const auto thread_registration =
        options_.register_thread
            ? options_.register_thread("pipeline-processing-" + thread_camera_id)
            : std::shared_ptr<void>{};
    std::uint64_t previous_sequence = 0U;
    try
    {
        while (!stop_token.stop_requested())
        {
            auto dequeued = input_.wait_pop(stop_token, options_.input_wait_timeout);
            if (dequeued.status == camera::FrameDequeueStatus::stopped ||
                dequeued.status == camera::FrameDequeueStatus::closed)
            {
                break;
            }
            if (dequeued.status != camera::FrameDequeueStatus::frame || !dequeued.packet)
            {
                continue;
            }

            const auto started = std::chrono::steady_clock::now();
            auto view = camera::make_frame_view(*dequeued.packet);
            if (!view)
            {
                frames_rejected_.fetch_add(1U, std::memory_order_relaxed);
                invalid_frames_.fetch_add(1U, std::memory_order_relaxed);
                record_failure(view.error());
            }
            else
            {
                const auto sequence = view.value().sequence_number();
                if (previous_sequence > 0U && sequence <= previous_sequence)
                {
                    auto error = pipeline_error("PIPELINE_FRAME_ORDER_VIOLATION", Severity::warning,
                                                "处理管线收到重复或回退的服务帧序号",
                                                "pipeline.processor.order", options_.camera_id,
                                                "sequence-not-increasing");
                    frames_rejected_.fetch_add(1U, std::memory_order_relaxed);
                    invalid_frames_.fetch_add(1U, std::memory_order_relaxed);
                    record_failure(error);
                }
                else
                {
                    if (previous_sequence > 0U && sequence > previous_sequence + 1U)
                    {
                        sequence_gaps_.fetch_add(sequence - previous_sequence - 1U,
                                                 std::memory_order_relaxed);
                    }
                    previous_sequence = sequence;
                    ProcessedFrame frame{.frame = std::move(view).value()};
                    auto result = [&]() -> Result<void> {
                        try
                        {
                            return chain_.process(frame);
                        }
                        catch (const std::exception& exception)
                        {
                            auto error = pipeline_error(
                                "ALGORITHM_PROCESS_FAILED", Severity::error, "预处理节点引发异常",
                                "pipeline.processor.process", options_.camera_id, "node-exception");
                            error.details.push_back({"exception", exception.what()});
                            return Result<void>::failure(std::move(error));
                        }
                        catch (...)
                        {
                            return Result<void>::failure(pipeline_error(
                                "ALGORITHM_PROCESS_FAILED", Severity::error,
                                "预处理节点引发未知异常", "pipeline.processor.process",
                                options_.camera_id, "unknown-node-exception"));
                        }
                    }();
                    if (!result)
                    {
                        frames_rejected_.fetch_add(1U, std::memory_order_relaxed);
                        node_failures_.fetch_add(1U, std::memory_order_relaxed);
                        if (is_invalid_frame_error(result.error()))
                        {
                            invalid_frames_.fetch_add(1U, std::memory_order_relaxed);
                        }
                        record_failure(result.error());
                    }
                    else
                    {
                        frames_processed_.fetch_add(1U, std::memory_order_relaxed);
                        if (output_.push(std::move(frame)) == AlgorithmEnqueueStatus::closed)
                        {
                            break;
                        }
                    }
                }
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
            const auto duration = static_cast<std::uint64_t>(std::max<std::int64_t>(elapsed, 0));
            total_processing_nanoseconds_.fetch_add(duration, std::memory_order_relaxed);
            auto maximum = maximum_processing_nanoseconds_.load(std::memory_order_relaxed);
            while (maximum < duration && !maximum_processing_nanoseconds_.compare_exchange_weak(
                                             maximum, duration, std::memory_order_relaxed))
            {
            }
        }
    }
    catch (const std::exception& exception)
    {
        auto error =
            pipeline_error("ALGORITHM_PROCESS_FAILED", Severity::error, "预处理节点引发异常",
                           "pipeline.processor.process", options_.camera_id, "node-exception");
        error.details.push_back({"exception", exception.what()});
        output_.close();
        finish(std::move(error));
        return;
    }
    catch (...)
    {
        output_.close();
        finish(pipeline_error("ALGORITHM_PROCESS_FAILED", Severity::error, "预处理节点引发未知异常",
                              "pipeline.processor.process", options_.camera_id,
                              "unknown-node-exception"));
        return;
    }
    output_.close();
    finish(std::nullopt);
}

void PerCameraProcessor::finish(std::optional<Error> error) noexcept
{
    {
        std::lock_guard lock{mutex_};
        running_ = false;
        completed_ = true;
        if (error)
        {
            last_error_ = std::move(error);
        }
    }
    condition_.notify_all();
}

void PerCameraProcessor::record_failure(const Error& error) noexcept
{
    try
    {
        std::lock_guard lock{mutex_};
        last_error_ = error;
    }
    catch (...)
    {
    }
}

Result<void> ProcessingRuntime::add(std::unique_ptr<PerCameraProcessor> processor)
{
    if (!processor || started_ || processors_.size() >= 4U)
    {
        return Result<void>::failure(pipeline_error(
            "CAMERA_CONFIG_FAILED", Severity::error, "处理运行时路由配置无效",
            "pipeline.runtime.add", processor ? processor->camera_id() : std::string{},
            !processor ? "null-processor"
            : started_ ? "runtime-already-started"
                       : "camera-count-exceeded"));
    }
    if (std::ranges::any_of(processors_, [&](const auto& current) {
            return current->camera_id() == processor->camera_id();
        }))
    {
        return Result<void>::failure(
            pipeline_error("CAMERA_CONFIG_FAILED", Severity::error, "处理运行时包含重复相机编号",
                           "pipeline.runtime.add", processor->camera_id(), "duplicate-camera-id"));
    }
    processors_.push_back(std::move(processor));
    return Result<void>::success();
}

Result<void> ProcessingRuntime::start()
{
    if (started_ || processors_.empty())
    {
        return Result<void>::failure(pipeline_error(
            "CAMERA_INVALID_STATE_TRANSITION", Severity::error, "处理运行时不能启动",
            "pipeline.runtime.start", {}, started_ ? "already-started" : "no-processors"));
    }
    std::size_t started_count = 0U;
    for (const auto& processor : processors_)
    {
        auto result = processor->start();
        if (!result)
        {
            for (std::size_t index = 0U; index < started_count; ++index)
            {
                processors_[index]->request_stop();
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            for (std::size_t index = 0U; index < started_count; ++index)
            {
                static_cast<void>(processors_[index]->join(deadline));
            }
            return result;
        }
        ++started_count;
    }
    started_ = true;
    return Result<void>::success();
}

void ProcessingRuntime::request_stop() noexcept
{
    for (const auto& processor : processors_)
    {
        processor->request_stop();
    }
}

Result<void> ProcessingRuntime::join(const std::chrono::steady_clock::time_point deadline)
{
    std::optional<Error> first_error;
    for (const auto& processor : processors_)
    {
        auto result = processor->join(deadline);
        if (!result && !first_error)
        {
            first_error = result.error();
        }
    }
    if (first_error)
    {
        return Result<void>::failure(std::move(*first_error));
    }
    return Result<void>::success();
}

std::size_t ProcessingRuntime::size() const noexcept
{
    return processors_.size();
}

} // namespace paperbreak::pipeline
