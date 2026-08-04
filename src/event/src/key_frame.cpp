#include "paperbreak/event/key_frame.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>

namespace paperbreak::event
{
namespace
{

constexpr std::size_t maximum_camera_count = 4U;
constexpr std::size_t maximum_job_capacity = 256U;
constexpr std::size_t absolute_maximum_window_frames = 262144U;
constexpr std::uint32_t absolute_maximum_dimension = 32768U;
constexpr std::size_t absolute_maximum_input_bytes = 512U * 1024U * 1024U;
constexpr std::size_t absolute_maximum_jpeg_bytes = 128U * 1024U * 1024U;

Error key_frame_error(std::string code, const Severity severity, std::string message,
                      std::string operation, const bool retryable = false)
{
    return make_error(std::move(code), severity, std::move(message), "event", std::move(operation),
                      retryable);
}

using FrameKey = std::pair<std::string, std::uint64_t>;

FrameKey frame_key(const camera::FrameView& frame)
{
    return {frame.camera_id(), frame.sequence_number()};
}

FrameKey frame_key(const KeyFrameReference& reference)
{
    return {reference.camera_id, reference.sequence_number};
}

auto frame_order(const camera::FrameView& frame)
{
    return std::tuple{frame.received_monotonic_time(), frame.camera_id(), frame.sequence_number()};
}

KeyFrameDescriptor describe(const camera::FrameView& frame, const KeyFrameReason reason)
{
    return {.camera_id = frame.camera_id(),
            .camera_frame_number = frame.camera_frame_number(),
            .sequence_number = frame.sequence_number(),
            .monotonic_time = frame.received_monotonic_time(),
            .wall_clock_time = frame.received_wall_clock_time(),
            .geometry = frame.geometry(),
            .pixel_format = frame.pixel_format(),
            .reasons = {reason}};
}

bool valid_reason(const KeyFrameReason reason) noexcept
{
    switch (reason)
    {
    case KeyFrameReason::normal_reference:
    case KeyFrameReason::earliest_abnormal:
    case KeyFrameReason::candidate_trigger:
    case KeyFrameReason::maximum_change:
    case KeyFrameReason::highest_confidence:
    case KeyFrameReason::formal_confirmation:
    case KeyFrameReason::post_event_state:
        return true;
    }
    return false;
}

bool descriptor_matches(const SelectedKeyFrame& selected)
{
    const auto& descriptor = selected.descriptor;
    const auto& frame = selected.frame;
    return !descriptor.camera_id.empty() && descriptor.camera_id == frame.camera_id() &&
           descriptor.camera_frame_number == frame.camera_frame_number() &&
           descriptor.sequence_number == frame.sequence_number() &&
           descriptor.monotonic_time == frame.received_monotonic_time() &&
           descriptor.wall_clock_time == frame.received_wall_clock_time() &&
           descriptor.geometry == frame.geometry() &&
           descriptor.pixel_format == frame.pixel_format() && !descriptor.reasons.empty() &&
           descriptor.reasons.size() <= key_frame_reason_count &&
           std::ranges::all_of(descriptor.reasons, valid_reason) &&
           std::ranges::all_of(descriptor.reasons, [&descriptor](const auto reason) {
               return std::ranges::count(descriptor.reasons, reason) == 1;
           });
}

} // namespace

std::string_view to_string(const KeyFrameReason reason) noexcept
{
    switch (reason)
    {
    case KeyFrameReason::normal_reference:
        return "NormalReference";
    case KeyFrameReason::earliest_abnormal:
        return "EarliestAbnormal";
    case KeyFrameReason::candidate_trigger:
        return "CandidateTrigger";
    case KeyFrameReason::maximum_change:
        return "MaximumChange";
    case KeyFrameReason::highest_confidence:
        return "HighestConfidence";
    case KeyFrameReason::formal_confirmation:
        return "FormalConfirmation";
    case KeyFrameReason::post_event_state:
        return "PostEventState";
    }
    return "Unknown";
}

KeyFrameSelector::KeyFrameSelector(KeyFrameSelectorConfig config) : config_(config)
{
    if (config_.maximum_window_frames == 0U ||
        config_.maximum_window_frames > absolute_maximum_window_frames ||
        config_.maximum_analysis_records == 0U ||
        config_.maximum_analysis_records > absolute_maximum_window_frames)
    {
        throw std::invalid_argument{"KeyFrameSelector configuration is invalid"};
    }
}

Result<KeyFrameSelectionResult> KeyFrameSelector::select(
    const FrozenEventWindow& event, const KeyFrameSelectionContext& context) const
{
    if (event.event_id.empty() || event.camera_windows.empty() ||
        event.camera_windows.size() > maximum_camera_count || event.triggers.empty() ||
        context.analyses.size() > config_.maximum_analysis_records)
    {
        return Result<KeyFrameSelectionResult>::failure(
            key_frame_error("EVENT_KEYFRAME_SELECTION_FAILED", Severity::error,
                            "关键帧事件或分析证据无效", "keyframe.select"));
    }

    std::map<FrameKey, const camera::FrameView*> frames;
    std::size_t frame_count = 0U;
    for (const auto& camera_window : event.camera_windows)
    {
        if (camera_window.camera_id.empty())
            return Result<KeyFrameSelectionResult>::failure(
                key_frame_error("EVENT_KEYFRAME_SELECTION_FAILED", Severity::error,
                                "关键帧窗口包含无效相机", "keyframe.select"));
        for (const auto& frame : camera_window.frames)
        {
            ++frame_count;
            if (frame_count > config_.maximum_window_frames ||
                frame.camera_id() != camera_window.camera_id || frame.flags().incomplete ||
                !frames.emplace(frame_key(frame), &frame).second)
            {
                return Result<KeyFrameSelectionResult>::failure(
                    key_frame_error("EVENT_KEYFRAME_SELECTION_FAILED", Severity::error,
                                    "关键帧窗口帧无效、重复或超过上限", "keyframe.select"));
            }
        }
    }

    std::map<FrameKey, const KeyFrameAnalysis*> analyses;
    for (const auto& analysis : context.analyses)
    {
        const auto key = frame_key(analysis.frame);
        if (analysis.frame.camera_id.empty() || !frames.contains(key) ||
            !std::isfinite(analysis.change_score) || analysis.change_score < 0.0 ||
            !std::isfinite(analysis.confidence) || analysis.confidence < 0.0 ||
            analysis.confidence > 1.0 || !analyses.emplace(key, &analysis).second)
        {
            return Result<KeyFrameSelectionResult>::failure(
                key_frame_error("EVENT_KEYFRAME_SELECTION_FAILED", Severity::error,
                                "关键帧逐帧证据无效、重复或不属于事件窗口", "keyframe.select"));
        }
    }
    if (context.confirmation_frame && !frames.contains(frame_key(*context.confirmation_frame)))
    {
        return Result<KeyFrameSelectionResult>::failure(
            key_frame_error("EVENT_KEYFRAME_SELECTION_FAILED", Severity::error,
                            "正式确认帧不属于事件窗口", "keyframe.select"));
    }

    std::map<FrameKey, SelectedKeyFrame> selected;
    std::vector<KeyFrameReason> missing;
    const auto add_reason = [&](const camera::FrameView* frame, const KeyFrameReason reason) {
        if (frame == nullptr)
        {
            missing.push_back(reason);
            return;
        }
        const auto key = frame_key(*frame);
        const auto [iterator, inserted] = selected.try_emplace(
            key, SelectedKeyFrame{.descriptor = describe(*frame, reason), .frame = *frame});
        if (!inserted && std::ranges::find(iterator->second.descriptor.reasons, reason) ==
                             iterator->second.descriptor.reasons.end())
        {
            iterator->second.descriptor.reasons.push_back(reason);
        }
    };

    const KeyFrameAnalysis* earliest_abnormal = nullptr;
    for (const auto& [key, analysis] : analyses)
    {
        static_cast<void>(key);
        if (!analysis->abnormal)
            continue;
        const auto* frame = frames.at(frame_key(analysis->frame));
        if (earliest_abnormal == nullptr ||
            frame_order(*frame) < frame_order(*frames.at(frame_key(earliest_abnormal->frame))))
        {
            earliest_abnormal = analysis;
        }
    }

    const camera::FrameView* normal_reference = nullptr;
    if (earliest_abnormal != nullptr)
    {
        const auto* abnormal_frame = frames.at(frame_key(earliest_abnormal->frame));
        for (const auto& [key, analysis] : analyses)
        {
            if (analysis->abnormal || analysis->frame.camera_id != abnormal_frame->camera_id())
                continue;
            const auto* frame = frames.at(key);
            if (frame_order(*frame) >= frame_order(*abnormal_frame))
                continue;
            if (normal_reference == nullptr || frame_order(*normal_reference) < frame_order(*frame))
                normal_reference = frame;
        }
    }
    add_reason(normal_reference, KeyFrameReason::normal_reference);
    add_reason(earliest_abnormal == nullptr ? nullptr
                                            : frames.at(frame_key(earliest_abnormal->frame)),
               KeyFrameReason::earliest_abnormal);

    const EventWindowTrigger* first_trigger = nullptr;
    const EventWindowTrigger* last_trigger = nullptr;
    for (const auto& trigger : event.triggers)
    {
        if (trigger.trigger.camera_id.empty() || trigger.trigger.sequence_number == 0U)
            return Result<KeyFrameSelectionResult>::failure(
                key_frame_error("EVENT_KEYFRAME_SELECTION_FAILED", Severity::error,
                                "关键帧触发记录无效", "keyframe.select"));
        const auto order = std::tuple{trigger.trigger.monotonic_time, trigger.trigger.camera_id,
                                      trigger.trigger.sequence_number};
        if (first_trigger == nullptr || order < std::tuple{first_trigger->trigger.monotonic_time,
                                                           first_trigger->trigger.camera_id,
                                                           first_trigger->trigger.sequence_number})
            first_trigger = &trigger;
        if (last_trigger == nullptr || order > std::tuple{last_trigger->trigger.monotonic_time,
                                                          last_trigger->trigger.camera_id,
                                                          last_trigger->trigger.sequence_number})
            last_trigger = &trigger;
    }
    const auto first_trigger_frame =
        frames.find({first_trigger->trigger.camera_id, first_trigger->trigger.sequence_number});
    add_reason(first_trigger_frame == frames.end() ? nullptr : first_trigger_frame->second,
               KeyFrameReason::candidate_trigger);

    const KeyFrameAnalysis* maximum_change = nullptr;
    const KeyFrameAnalysis* highest_confidence = nullptr;
    for (const auto& [key, analysis] : analyses)
    {
        const auto* frame = frames.at(key);
        if (maximum_change == nullptr || analysis->change_score > maximum_change->change_score ||
            (analysis->change_score == maximum_change->change_score &&
             frame_order(*frame) < frame_order(*frames.at(frame_key(maximum_change->frame)))))
            maximum_change = analysis;
        if (highest_confidence == nullptr ||
            analysis->confidence > highest_confidence->confidence ||
            (analysis->confidence == highest_confidence->confidence &&
             frame_order(*frame) < frame_order(*frames.at(frame_key(highest_confidence->frame)))))
            highest_confidence = analysis;
    }
    add_reason(maximum_change == nullptr ? nullptr : frames.at(frame_key(maximum_change->frame)),
               KeyFrameReason::maximum_change);
    add_reason(highest_confidence == nullptr ? nullptr
                                             : frames.at(frame_key(highest_confidence->frame)),
               KeyFrameReason::highest_confidence);
    add_reason(context.confirmation_frame ? frames.at(frame_key(*context.confirmation_frame))
                                          : nullptr,
               KeyFrameReason::formal_confirmation);

    const camera::FrameView* post_event_frame = nullptr;
    for (const auto& [key, frame] : frames)
    {
        static_cast<void>(key);
        if (frame->camera_id() != last_trigger->trigger.camera_id ||
            frame->received_monotonic_time() <= last_trigger->trigger.monotonic_time)
            continue;
        if (post_event_frame == nullptr || frame_order(*frame) < frame_order(*post_event_frame))
            post_event_frame = frame;
    }
    add_reason(post_event_frame, KeyFrameReason::post_event_state);

    KeyFrameSelectionResult result{.event_id = event.event_id,
                                   .missing_reasons = std::move(missing)};
    result.frames.reserve(selected.size());
    for (auto& [key, frame] : selected)
    {
        static_cast<void>(key);
        std::ranges::sort(frame.descriptor.reasons, [](const auto left, const auto right) {
            return static_cast<int>(left) < static_cast<int>(right);
        });
        result.frames.push_back(std::move(frame));
    }
    std::ranges::sort(result.frames, [](const auto& left, const auto& right) {
        return std::tuple{left.descriptor.monotonic_time, left.descriptor.camera_id,
                          left.descriptor.sequence_number} <
               std::tuple{right.descriptor.monotonic_time, right.descriptor.camera_id,
                          right.descriptor.sequence_number};
    });
    result.complete = result.missing_reasons.empty();
    return Result<KeyFrameSelectionResult>::success(std::move(result));
}

struct KeyFrameJpegRuntime::Impl final
{
    struct Job final
    {
        std::string event_id;
        SelectedKeyFrame selected;
    };

    Impl(std::unique_ptr<IKeyFrameJpegEncoder> encoder_value,
         KeyFrameEncodingCallback callback_value, KeyFrameJpegRuntimeOptions options_value)
        : encoder(std::move(encoder_value)), callback(std::move(callback_value)),
          options(options_value), jobs(options.job_capacity)
    {
    }

    void run() noexcept
    {
        for (;;)
        {
            std::unique_ptr<Job> job;
            {
                std::unique_lock lock{mutex};
                condition.wait(lock, [this] { return stopping || depth != 0U; });
                if (depth == 0U)
                {
                    if (stopping)
                        break;
                    continue;
                }
                job = std::move(jobs[read_index]);
                read_index = (read_index + 1U) % jobs.size();
                --depth;
            }

            KeyFrameEncodingResult output{.event_id = job->event_id,
                                          .descriptor = job->selected.descriptor};
            try
            {
                auto encoded = encoder->encode(job->selected.frame, options.encoding);
                if (encoded)
                    output.jpeg = std::move(encoded).value();
                else
                    output.error = encoded.error();
            }
            catch (...)
            {
                output.error = key_frame_error("EVENT_KEYFRAME_ENCODE_FAILED", Severity::error,
                                               "关键帧 JPEG 编码器异常", "keyframe.encode", true);
            }

            const bool failed = output.error.has_value();
            try
            {
                callback(std::move(output));
            }
            catch (...)
            {
                std::scoped_lock lock{mutex};
                ++callback_failures;
            }
            {
                std::scoped_lock lock{mutex};
                ++completed;
                if (failed)
                    ++encoding_failures;
            }
        }
        {
            std::scoped_lock lock{mutex};
            completed_run = true;
        }
        completed_condition.notify_all();
    }

    std::unique_ptr<IKeyFrameJpegEncoder> encoder;
    KeyFrameEncodingCallback callback;
    KeyFrameJpegRuntimeOptions options;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::condition_variable completed_condition;
    std::vector<std::unique_ptr<Job>> jobs;
    std::size_t read_index{};
    std::size_t write_index{};
    std::size_t depth{};
    std::jthread worker;
    bool started{};
    bool stopping{};
    bool completed_run{true};
    std::size_t high_watermark{};
    std::uint64_t submitted{};
    std::uint64_t completed{};
    std::uint64_t rejected{};
    std::uint64_t encoding_failures{};
    std::uint64_t callback_failures{};
};

Result<std::unique_ptr<KeyFrameJpegRuntime>> KeyFrameJpegRuntime::create(
    std::unique_ptr<IKeyFrameJpegEncoder> encoder, KeyFrameEncodingCallback callback,
    const KeyFrameJpegRuntimeOptions options)
{
    if (!encoder || !callback || options.job_capacity == 0U ||
        options.job_capacity > maximum_job_capacity || options.encoding.jpeg_quality == 0U ||
        options.encoding.jpeg_quality > 100U || options.encoding.maximum_dimension == 0U ||
        options.encoding.maximum_dimension > absolute_maximum_dimension ||
        options.encoding.maximum_input_bytes == 0U ||
        options.encoding.maximum_input_bytes > absolute_maximum_input_bytes ||
        options.encoding.maximum_jpeg_bytes == 0U ||
        options.encoding.maximum_jpeg_bytes > absolute_maximum_jpeg_bytes)
    {
        return Result<std::unique_ptr<KeyFrameJpegRuntime>>::failure(
            key_frame_error("SYS_CONFIG_INVALID", Severity::error, "关键帧 JPEG 运行时配置无效",
                            "keyframe.createRuntime"));
    }
    return Result<std::unique_ptr<KeyFrameJpegRuntime>>::success(
        std::make_unique<KeyFrameJpegRuntime>(ConstructionKey{}, std::move(encoder),
                                              std::move(callback), options));
}

KeyFrameJpegRuntime::KeyFrameJpegRuntime(ConstructionKey,
                                         std::unique_ptr<IKeyFrameJpegEncoder> encoder,
                                         KeyFrameEncodingCallback callback,
                                         const KeyFrameJpegRuntimeOptions options)
    : impl_(std::make_unique<Impl>(std::move(encoder), std::move(callback), options))
{
}

KeyFrameJpegRuntime::~KeyFrameJpegRuntime()
{
    request_stop();
    static_cast<void>(join(camera::MonotonicTime::max()));
}

Result<void> KeyFrameJpegRuntime::start()
{
    std::scoped_lock lock{impl_->mutex};
    if (impl_->started)
        return Result<void>::failure(key_frame_error("EVENT_INVALID_TRANSITION", Severity::error,
                                                     "关键帧 JPEG 运行时不能重复启动",
                                                     "keyframe.start"));
    impl_->started = true;
    impl_->stopping = false;
    impl_->completed_run = false;
    try
    {
        impl_->worker = std::jthread([this] { impl_->run(); });
    }
    catch (const std::exception&)
    {
        impl_->started = false;
        impl_->completed_run = true;
        return Result<void>::failure(
            key_frame_error("EVENT_KEYFRAME_ENCODE_FAILED", Severity::error,
                            "无法创建关键帧 JPEG 工作线程", "keyframe.start", true));
    }
    return Result<void>::success();
}

Result<void> KeyFrameJpegRuntime::submit(const KeyFrameSelectionResult& selection)
{
    const auto invalid =
        selection.event_id.empty() || selection.frames.empty() ||
        selection.frames.size() > key_frame_reason_count ||
        std::ranges::any_of(selection.frames,
                            [](const auto& frame) { return !descriptor_matches(frame); }) ||
        std::ranges::any_of(selection.frames, [&selection](const auto& frame) {
            return std::ranges::count_if(selection.frames, [&frame](const auto& other) {
                       return other.descriptor.camera_id == frame.descriptor.camera_id &&
                              other.descriptor.sequence_number == frame.descriptor.sequence_number;
                   }) != 1;
        });
    if (invalid)
    {
        return Result<void>::failure(key_frame_error("EVENT_KEYFRAME_SELECTION_FAILED",
                                                     Severity::error, "提交的关键帧选择结果无效",
                                                     "keyframe.submit"));
    }

    std::vector<std::unique_ptr<Impl::Job>> pending;
    try
    {
        pending.reserve(selection.frames.size());
        for (const auto& frame : selection.frames)
            pending.push_back(std::make_unique<Impl::Job>(
                Impl::Job{.event_id = selection.event_id, .selected = frame}));
    }
    catch (const std::exception&)
    {
        std::scoped_lock lock{impl_->mutex};
        impl_->rejected += selection.frames.size();
        return Result<void>::failure(key_frame_error("EVENT_KEYFRAME_QUEUE_FULL", Severity::error,
                                                     "关键帧任务内存预算不足", "keyframe.submit"));
    }

    std::scoped_lock lock{impl_->mutex};
    if (!impl_->started || impl_->stopping ||
        selection.frames.size() > impl_->options.job_capacity - impl_->depth)
    {
        impl_->rejected += selection.frames.size();
        return Result<void>::failure(key_frame_error("EVENT_KEYFRAME_QUEUE_FULL", Severity::error,
                                                     "关键帧任务队列已满或停止接收",
                                                     "keyframe.submit"));
    }
    for (auto& job : pending)
    {
        impl_->jobs[impl_->write_index] = std::move(job);
        impl_->write_index = (impl_->write_index + 1U) % impl_->jobs.size();
        ++impl_->depth;
    }
    impl_->submitted += selection.frames.size();
    impl_->high_watermark = std::max(impl_->high_watermark, impl_->depth);
    impl_->condition.notify_one();
    return Result<void>::success();
}

void KeyFrameJpegRuntime::request_stop() noexcept
{
    {
        std::scoped_lock lock{impl_->mutex};
        if (!impl_->started)
            return;
        impl_->stopping = true;
    }
    impl_->condition.notify_all();
}

Result<void> KeyFrameJpegRuntime::join(const camera::MonotonicTime deadline)
{
    std::unique_lock lock{impl_->mutex};
    if (!impl_->started || impl_->completed_run)
    {
        lock.unlock();
        if (impl_->worker.joinable())
            impl_->worker.join();
        return Result<void>::success();
    }
    if (!impl_->completed_condition.wait_until(lock, deadline,
                                               [this] { return impl_->completed_run; }))
    {
        return Result<void>::failure(key_frame_error("SYS_SHUTDOWN_TIMEOUT", Severity::error,
                                                     "关键帧 JPEG 工作线程未在截止时间内停止",
                                                     "keyframe.join", true));
    }
    lock.unlock();
    if (impl_->worker.joinable())
        impl_->worker.join();
    return Result<void>::success();
}

KeyFrameJpegRuntimeSnapshot KeyFrameJpegRuntime::snapshot() const noexcept
{
    std::scoped_lock lock{impl_->mutex};
    return {.started = impl_->started && !impl_->completed_run,
            .accepting = impl_->started && !impl_->stopping,
            .depth = impl_->depth,
            .capacity = impl_->options.job_capacity,
            .high_watermark = impl_->high_watermark,
            .submitted = impl_->submitted,
            .completed = impl_->completed,
            .rejected = impl_->rejected,
            .encoding_failures = impl_->encoding_failures,
            .callback_failures = impl_->callback_failures};
}

} // namespace paperbreak::event
