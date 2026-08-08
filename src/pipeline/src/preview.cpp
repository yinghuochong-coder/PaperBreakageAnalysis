#include "paperbreak/pipeline/preview.hpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace paperbreak::pipeline
{
namespace
{
Error preview_error(std::string code, std::string message, std::string operation)
{
    return make_error(std::move(code), Severity::warning, std::move(message), "pipeline",
                      std::move(operation), true);
}

class OpenCvPreviewEncoder final : public IPreviewEncoder
{
  public:
    [[nodiscard]] Result<std::vector<std::byte>> encode(
        const camera::FrameView& frame, const PreviewEncodeOptions& options) override
    {
        try
        {
            const auto geometry = frame.geometry();
            if (geometry.width == 0U || geometry.height == 0U || geometry.stride == 0U ||
                options.maximum_width == 0U || options.maximum_height == 0U ||
                options.jpeg_quality == 0U || options.jpeg_quality > 100U)
            {
                return Result<std::vector<std::byte>>::failure(preview_error(
                    "PIPELINE_PREVIEW_ENCODE_FAILED", "预览图像或编码参数无效", "preview.encode"));
            }
            const int type = (frame.pixel_format() == camera::PixelFormat::mono10 ||
                              frame.pixel_format() == camera::PixelFormat::mono12)
                                 ? CV_16UC1
                                 : CV_8UC1;
            const std::size_t minimum_stride =
                static_cast<std::size_t>(geometry.width) * (type == CV_16UC1 ? 2U : 1U);
            if (geometry.stride < minimum_stride ||
                frame.bytes().size() < static_cast<std::size_t>(geometry.height) * geometry.stride)
            {
                return Result<std::vector<std::byte>>::failure(preview_error(
                    "PIPELINE_PREVIEW_ENCODE_FAILED", "预览图像行跨度无效", "preview.encode"));
            }
            cv::Mat source(static_cast<int>(geometry.height), static_cast<int>(geometry.width),
                           type, const_cast<std::byte*>(frame.bytes().data()), geometry.stride);
            cv::Mat image;
            if (type == CV_16UC1)
            {
                const double scale = frame.pixel_format() == camera::PixelFormat::mono10
                                         ? 255.0 / 1023.0
                                         : 255.0 / 4095.0;
                source.convertTo(image, CV_8UC1, scale);
            }
            else if (frame.pixel_format() == camera::PixelFormat::bayer_rg8)
            {
                cv::cvtColor(source, image, cv::COLOR_BayerRG2BGR);
            }
            else
            {
                image = source;
            }
            const double scale =
                std::min({1.0, static_cast<double>(options.maximum_width) / image.cols,
                          static_cast<double>(options.maximum_height) / image.rows});
            if (scale < 1.0)
            {
                cv::resize(image, image,
                           cv::Size{std::max(1, static_cast<int>(std::lround(image.cols * scale))),
                                    std::max(1, static_cast<int>(std::lround(image.rows * scale)))},
                           0.0, 0.0, cv::INTER_AREA);
            }
            std::vector<unsigned char> encoded;
            if (!cv::imencode(".jpg", image, encoded,
                              {cv::IMWRITE_JPEG_QUALITY, static_cast<int>(options.jpeg_quality)}) ||
                encoded.empty() || encoded.size() > options.maximum_binary_bytes)
            {
                return Result<std::vector<std::byte>>::failure(
                    preview_error("PIPELINE_PREVIEW_ENCODE_FAILED", "JPEG 编码失败或超过二进制上限",
                                  "preview.encode"));
            }
            std::vector<std::byte> result(encoded.size());
            std::transform(encoded.begin(), encoded.end(), result.begin(),
                           [](const unsigned char value) { return static_cast<std::byte>(value); });
            return Result<std::vector<std::byte>>::success(std::move(result));
        }
        catch (const cv::Exception&)
        {
            return Result<std::vector<std::byte>>::failure(preview_error(
                "PIPELINE_PREVIEW_ENCODE_FAILED", "OpenCV JPEG 编码失败", "preview.encode"));
        }
    }
};

} // namespace

struct PreviewRuntime::CameraSlot final
{
    std::mutex mutex;
    std::optional<PendingFrame> pending;
    std::optional<camera::MonotonicTime> last_sample;
};

std::unique_ptr<IPreviewEncoder> make_opencv_preview_encoder()
{
    return std::make_unique<OpenCvPreviewEncoder>();
}

PreviewRuntime::PreviewRuntime(std::vector<std::string> camera_ids,
                               std::unique_ptr<IPreviewEncoder> encoder,
                               PreviewDeliveryCallback delivery, PreviewRuntimeOptions options)
    : encoder_(std::move(encoder)), delivery_(std::move(delivery)), options_(std::move(options))
{
    if (!encoder_ || !delivery_ || camera_ids.empty() ||
        camera_ids.size() > options_.maximum_cameras || options_.maximum_cameras == 0U ||
        options_.maximum_subscriptions == 0U || options_.frames_per_second < 2.0 ||
        options_.frames_per_second > 5.0 || options_.encoding.maximum_binary_bytes == 0U ||
        options_.encoding.maximum_binary_bytes > 16U * 1024U * 1024U)
    {
        throw std::invalid_argument{"PreviewRuntime options are invalid"};
    }
    for (std::string& camera_id : camera_ids)
    {
        if (camera_id.empty() ||
            !cameras_.emplace(camera_id, std::make_unique<CameraSlot>()).second)
            throw std::invalid_argument{"PreviewRuntime camera identifiers are invalid"};
    }
}

PreviewRuntime::~PreviewRuntime()
{
    request_stop();
    static_cast<void>(join(std::chrono::steady_clock::now() + std::chrono::seconds{5}));
}

Result<void> PreviewRuntime::start()
{
    std::scoped_lock lock{lifecycle_mutex_};
    if (started_)
        return Result<void>::failure(preview_error("PIPELINE_PREVIEW_INVALID_STATE",
                                                   "预览运行时不能重复启动", "preview.start"));
    started_ = true;
    completed_ = false;
    try
    {
        worker_ = std::jthread([this](const std::stop_token token) { run(token); });
    }
    catch (const std::exception&)
    {
        started_ = false;
        completed_ = true;
        return Result<void>::failure(preview_error("PIPELINE_PREVIEW_START_FAILED",
                                                   "无法创建预览工作线程", "preview.start"));
    }
    return Result<void>::success();
}

void PreviewRuntime::request_stop() noexcept
{
    if (worker_.joinable())
        worker_.request_stop();
}

Result<void> PreviewRuntime::join(const std::chrono::steady_clock::time_point deadline)
{
    std::unique_lock lock{lifecycle_mutex_};
    if (!started_ || completed_)
    {
        lock.unlock();
        if (worker_.joinable())
            worker_.join();
        return Result<void>::success();
    }
    if (!lifecycle_condition_.wait_until(lock, deadline, [this] { return completed_; }))
        return Result<void>::failure(preview_error(
            "SYS_SHUTDOWN_TIMEOUT", "预览工作线程未在截止时间内停止", "preview.join"));
    lock.unlock();
    if (worker_.joinable())
        worker_.join();
    return Result<void>::success();
}

Result<void> PreviewRuntime::subscribe(const std::uint64_t subscriber_id,
                                       const std::vector<std::string>& camera_ids)
{
    if (subscriber_id == 0U || camera_ids.empty() || camera_ids.size() > cameras_.size())
        return Result<void>::failure(
            preview_error("IPC_REQUEST_INVALID", "预览订阅参数无效", "preview.subscribe"));
    std::unordered_set<std::string> selected;
    for (const auto& camera_id : camera_ids)
    {
        if (!cameras_.contains(camera_id) || !selected.insert(camera_id).second)
            return Result<void>::failure(preview_error(
                "IPC_REQUEST_INVALID", "预览订阅包含未知或重复相机", "preview.subscribe"));
    }
    std::scoped_lock lock{subscriptions_mutex_};
    if (!subscriptions_.contains(subscriber_id) &&
        subscriptions_.size() >= options_.maximum_subscriptions)
        return Result<void>::failure(
            preview_error("IPC_BUSY", "预览订阅数已达上限", "preview.subscribe"));
    subscriptions_[subscriber_id] = std::move(selected);
    return Result<void>::success();
}

void PreviewRuntime::unsubscribe(const std::uint64_t subscriber_id) noexcept
{
    std::scoped_lock lock{subscriptions_mutex_};
    subscriptions_.erase(subscriber_id);
}

bool PreviewRuntime::has_subscriber_for(const std::string& camera_id) const noexcept
{
    std::scoped_lock lock{subscriptions_mutex_};
    return std::ranges::any_of(
        subscriptions_, [&camera_id](const auto& item) { return item.second.contains(camera_id); });
}

void PreviewRuntime::submit(camera::FrameView frame, PreviewFrameMetadata metadata) noexcept
{
    frames_received_.fetch_add(1U, std::memory_order_relaxed);
    const auto iterator = cameras_.find(frame.camera_id());
    if (iterator == cameras_.end())
    {
        rejected_unknown_camera_.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    if (!started_ || worker_.get_stop_token().stop_requested())
    {
        rejected_after_stop_.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    if (!has_subscriber_for(frame.camera_id()))
    {
        frames_skipped_without_subscribers_.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    CameraSlot& slot = *iterator->second;
    std::scoped_lock lock{slot.mutex};
    const auto interval = std::chrono::duration_cast<camera::MonotonicTime::duration>(
        std::chrono::duration<double>{1.0 / options_.frames_per_second});
    if (slot.last_sample && frame.received_monotonic_time() - *slot.last_sample < interval)
    {
        frames_skipped_by_rate_.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    slot.last_sample = frame.received_monotonic_time();
    if (slot.pending.has_value())
        frames_replaced_before_encoding_.fetch_add(1U, std::memory_order_relaxed);
    slot.pending = PendingFrame{std::move(frame), std::move(metadata)};
    frames_sampled_.fetch_add(1U, std::memory_order_relaxed);
}

void PreviewRuntime::run(const std::stop_token token) noexcept
{
    const auto thread_registration =
        options_.register_thread ? options_.register_thread("preview-encoder") : nullptr;
    while (!token.stop_requested())
    {
        bool did_work = false;
        for (auto& [camera_id, slot_ptr] : cameras_)
        {
            std::optional<PendingFrame> pending;
            {
                std::scoped_lock lock{slot_ptr->mutex};
                pending = std::move(slot_ptr->pending);
                slot_ptr->pending.reset();
            }
            if (!pending)
                continue;
            did_work = true;
            auto encoded = encoder_->encode(pending->frame, options_.encoding);
            if (!encoded)
            {
                encoding_failures_.fetch_add(1U, std::memory_order_relaxed);
                if (options_.diagnostics.enabled && options_.diagnostics.enabled() &&
                    options_.diagnostics.record)
                    options_.diagnostics.record(
                        "operation=preview.encode result=failure cameraId=" + camera_id +
                        " sequenceNumber=" + std::to_string(pending->frame.sequence_number()) +
                        " businessCode=" + encoded.error().business_code);
                continue;
            }
            encoded_.fetch_add(1U, std::memory_order_relaxed);
            std::vector<std::uint64_t> subscribers;
            {
                std::scoped_lock lock{subscriptions_mutex_};
                for (const auto& [subscriber_id, selected] : subscriptions_)
                    if (selected.contains(camera_id))
                        subscribers.push_back(subscriber_id);
            }
            if (options_.diagnostics.enabled && options_.diagnostics.enabled() &&
                options_.diagnostics.record)
                options_.diagnostics.record(
                    "operation=preview.encode result=success cameraId=" + camera_id +
                    " sequenceNumber=" + std::to_string(pending->frame.sequence_number()) +
                    " sourceBytes=" + std::to_string(pending->frame.bytes().size()) +
                    " jpegBytes=" + std::to_string(encoded.value().size()) +
                    " subscriberCount=" + std::to_string(subscribers.size()));
            for (const auto subscriber_id : subscribers)
            {
                try
                {
                    delivery_({.subscriber_id = subscriber_id,
                               .camera_id = camera_id,
                               .camera_frame_number = pending->frame.camera_frame_number(),
                               .sequence_number = pending->frame.sequence_number(),
                               .source_geometry = pending->frame.geometry(),
                               .metadata = pending->metadata,
                               .jpeg = encoded.value()});
                    deliveries_.fetch_add(1U, std::memory_order_relaxed);
                }
                catch (...)
                {
                    delivery_failures_.fetch_add(1U, std::memory_order_relaxed);
                }
            }
        }
        if (!did_work)
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    finish();
}

void PreviewRuntime::finish() noexcept
{
    {
        std::scoped_lock lock{lifecycle_mutex_};
        completed_ = true;
    }
    lifecycle_condition_.notify_all();
}

PreviewRuntimeSnapshot PreviewRuntime::snapshot() const noexcept
{
    std::size_t subscriptions = 0U;
    {
        std::scoped_lock lock{subscriptions_mutex_};
        subscriptions = subscriptions_.size();
    }
    std::scoped_lock lock{lifecycle_mutex_};
    return {.started = started_ && !completed_,
            .subscriptions = subscriptions,
            .frames_received = frames_received_.load(std::memory_order_relaxed),
            .frames_sampled = frames_sampled_.load(std::memory_order_relaxed),
            .frames_skipped_without_subscribers =
                frames_skipped_without_subscribers_.load(std::memory_order_relaxed),
            .frames_skipped_by_rate = frames_skipped_by_rate_.load(std::memory_order_relaxed),
            .frames_replaced_before_encoding =
                frames_replaced_before_encoding_.load(std::memory_order_relaxed),
            .encoded = encoded_.load(std::memory_order_relaxed),
            .encoding_failures = encoding_failures_.load(std::memory_order_relaxed),
            .deliveries = deliveries_.load(std::memory_order_relaxed),
            .delivery_failures = delivery_failures_.load(std::memory_order_relaxed),
            .rejected_unknown_camera = rejected_unknown_camera_.load(std::memory_order_relaxed),
            .rejected_after_stop = rejected_after_stop_.load(std::memory_order_relaxed)};
}

} // namespace paperbreak::pipeline
