#include "paperbreak/service/event_runtime.hpp"

#include "paperbreak/algorithm/mock_trigger_detector.hpp"
#include "paperbreak/event/candidate_event.hpp"
#include "paperbreak/event/event_window.hpp"
#include "paperbreak/event/key_frame.hpp"
#include "paperbreak/event/key_frame_opencv.hpp"
#include "paperbreak/event/memory_ring.hpp"
#include "paperbreak/storage/event_store.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace paperbreak::service
{
namespace
{
using namespace std::chrono_literals;

Error runtime_error(std::string code, Severity severity, std::string message, std::string operation,
                    bool retryable = false)
{
    return make_error(std::move(code), severity, std::move(message), "event", std::move(operation),
                      retryable);
}

std::filesystem::path path_from_utf8(const std::string_view value)
{
    std::u8string converted;
    converted.reserve(value.size());
    for (const unsigned char byte : value)
        converted.push_back(static_cast<char8_t>(byte));
    return std::filesystem::path{converted};
}

std::filesystem::path resolve_config_path(const std::string_view value)
{
    auto path = path_from_utf8(value);
    if (path.is_relative())
        path = std::filesystem::current_path() / path;
    return path.lexically_normal();
}

struct Lane final
{
    std::string camera_id;
    std::unique_ptr<event::MemoryRing> ring;
    std::unique_ptr<algorithm::mock::MockTriggerDetector> detector;
    std::uint64_t latest_submitted_sequence{};
    std::uint64_t manual_after_sequence{};
    bool manual_pending{};
};

struct EventPipelineState final
{
    config::EdgeConfig configuration;
    std::vector<Lane> lanes;
    std::unique_ptr<event::CandidateEventManager> candidates;
    std::unique_ptr<event::EventWindowManager> windows;
    std::map<std::string, std::string> source_to_canonical;
    camera::MonotonicTime last_monotonic_time{};
};

Result<std::unique_ptr<EventPipelineState>> build_pipeline(const config::EdgeConfig& configuration,
                                                           const std::size_t frame_queue_capacity)
{
    auto state = std::make_unique<EventPipelineState>();
    state->configuration = configuration;
    state->lanes.reserve(configuration.cameras.size());
    for (const auto& camera : configuration.cameras)
    {
        if (!camera.enabled)
            continue;
        auto plan = event::plan_memory_ring(
            {.camera_id = camera.id,
             .capacity_mode = event::MemoryRingCapacityMode::duration,
             .configured_duration_seconds =
                 static_cast<double>(configuration.event.pre_event_seconds),
             .configured_frame_rate = camera.frame_rate,
             .safety_margin_frames = 1U,
             .frame_buffer_capacity_bytes = 1U,
             .acquisition_queue_capacity = configuration.acquisition.queue_capacity,
             .algorithm_queue_capacity = frame_queue_capacity,
             .preview_slot_count = configuration.preview.enabled ? 1U : 0U,
             .post_event_seconds = static_cast<double>(configuration.event.post_event_seconds),
             .maximum_concurrent_events = 1U,
             .configured_frame_pool_capacity = configuration.acquisition.frame_pool_capacity,
             .memory_budget_bytes = configuration.acquisition.frame_pool_capacity});
        if (!plan)
            return Result<std::unique_ptr<EventPipelineState>>::failure(std::move(plan).error());
        auto detector = algorithm::mock::MockTriggerDetector::create(
            {.camera_id = camera.id, .mode = algorithm::mock::MockTriggerMode::manual_only});
        if (!detector)
            return Result<std::unique_ptr<EventPipelineState>>::failure(
                std::move(detector).error());
        const auto maximum_references = plan.value().ring_capacity_frames * 8U;
        state->lanes.push_back(
            {.camera_id = camera.id,
             .ring = std::make_unique<event::MemoryRing>(event::MemoryRingOptions{
                 .camera_id = camera.id,
                 .capacity_frames = plan.value().ring_capacity_frames,
                 .required_history_seconds =
                     static_cast<double>(configuration.event.pre_event_seconds),
                 .maximum_active_leases = 8U,
                 .maximum_leased_frame_references = maximum_references}),
             .detector = std::move(detector).value()});
    }
    if (state->lanes.empty())
        return Result<std::unique_ptr<EventPipelineState>>::success(std::move(state));

    std::vector<event::CandidateCameraBinding> candidate_bindings;
    std::vector<event::EventWindowCameraBinding> window_bindings;
    for (auto& lane : state->lanes)
    {
        candidate_bindings.push_back({.camera_id = lane.camera_id, .memory_ring = lane.ring.get()});
        window_bindings.push_back({.camera_id = lane.camera_id, .memory_ring = lane.ring.get()});
    }
    auto candidates = event::CandidateEventManager::create(
        {.cameras = std::move(candidate_bindings),
         .candidate_consecutive_frames = 1U,
         .confirmation_consecutive_frames = 2U,
         .candidate_timeout = std::chrono::seconds{configuration.event.max_event_seconds},
         .pre_event_duration = std::chrono::seconds{configuration.event.pre_event_seconds}});
    if (!candidates)
        return Result<std::unique_ptr<EventPipelineState>>::failure(std::move(candidates).error());
    auto windows = event::EventWindowManager::create(
        {.cameras = std::move(window_bindings),
         .pre_event_duration = std::chrono::seconds{configuration.event.pre_event_seconds},
         .post_event_duration = std::chrono::seconds{configuration.event.post_event_seconds},
         .maximum_event_duration = std::chrono::seconds{configuration.event.max_event_seconds},
         .merge_gap = std::chrono::seconds{configuration.event.merge_gap_seconds},
         .maximum_active_events = 4U});
    if (!windows)
        return Result<std::unique_ptr<EventPipelineState>>::failure(std::move(windows).error());
    state->candidates = std::move(candidates).value();
    state->windows = std::move(windows).value();
    return Result<std::unique_ptr<EventPipelineState>>::success(std::move(state));
}

Lane* find_lane(EventPipelineState& state, const std::string_view camera_id)
{
    const auto found = std::ranges::find_if(
        state.lanes, [camera_id](const Lane& lane) { return lane.camera_id == camera_id; });
    return found == state.lanes.end() ? nullptr : &*found;
}

std::string trigger_reason(const algorithm::TriggerResult& trigger)
{
    const auto source = algorithm::to_string(trigger.trigger_source);
    return source.empty() ? trigger.reason : std::string{source};
}

camera::WallClockTime earliest_wall_time(const event::FrozenEventWindow& window)
{
    auto earliest = window.display_wall_clock_time;
    for (const auto& camera : window.camera_windows)
        for (const auto& frame : camera.frames)
            earliest = std::min(earliest, frame.received_wall_clock_time());
    return earliest;
}

camera::WallClockTime latest_wall_time(const event::FrozenEventWindow& window)
{
    auto latest = window.display_wall_clock_time;
    for (const auto& camera : window.camera_windows)
        for (const auto& frame : camera.frames)
            latest = std::max(latest, frame.received_wall_clock_time());
    return latest;
}

} // namespace

struct EventRuntimeImpl final
{
    struct PendingEvent final
    {
        storage::EventManifestMetadata metadata;
        event::FrozenEventWindow window;
        std::size_t remaining{};
        std::vector<storage::PersistedKeyFrame> key_frames;
        bool save_raw{true};
    };

    EventRuntimeOptions options;
    mutable std::mutex mutex;
    std::condition_variable_any condition;
    std::deque<camera::FrameView> frames;
    std::unique_ptr<EventPipelineState> pipeline;
    std::map<std::string, PendingEvent> pending;
    std::unique_ptr<event::KeyFrameJpegRuntime> jpeg;
    std::unique_ptr<storage::EventPersistenceRuntime> persistence;
    std::jthread worker;
    bool started{};
    bool accepting{};
    bool stop_requested{};
    std::size_t frame_high_watermark{};
    std::atomic_uint64_t submitted_frames{};
    std::atomic_uint64_t processed_frames{};
    std::atomic_uint64_t rejected_frames{};
    std::atomic_uint64_t detector_failures{};
    std::atomic_uint64_t events_started{};
    std::atomic_uint64_t events_frozen{};
    std::atomic_uint64_t events_committed{};
    std::atomic_uint64_t event_failures{};

    void report(const Error& error) noexcept
    {
        try
        {
            if (options.error_observer)
                options.error_observer(error);
        }
        catch (...)
        {
        }
    }

    void persistence_completed(storage::EventPersistenceCompletion completion)
    {
        if (!completion.outcome)
        {
            ++event_failures;
            if (completion.error)
                report(*completion.error);
            return;
        }
        auto indexed =
            options.database->index_committed_event(completion.outcome->committed_directory);
        if (!indexed)
        {
            ++event_failures;
            report(indexed.error());
            return;
        }
        auto record = options.database->get_event(completion.event_id);
        if (!record)
        {
            report(record.error());
            return;
        }
        ++events_committed;
        try
        {
            if (options.committed_observer)
                options.committed_observer(record.value());
        }
        catch (...)
        {
        }
    }

    void submit_persistence(PendingEvent event)
    {
        if (!event.metadata.event_id.empty() && !event.metadata.camera_ids.empty())
        {
            if (options.storage_policy)
            {
                auto admitted = options.storage_policy->admit_large_write();
                if (!admitted)
                {
                    ++event_failures;
                    report(admitted.error());
                    return;
                }
            }
            if (!event.save_raw)
            {
                std::set<std::pair<std::string, std::uint64_t>> selected;
                for (const auto& key : event.key_frames)
                    selected.emplace(key.descriptor.camera_id, key.descriptor.sequence_number);
                for (auto& camera : event.window.camera_windows)
                    std::erase_if(camera.frames, [&](const camera::FrameView& frame) {
                        return !selected.contains({frame.camera_id(), frame.sequence_number()});
                    });
            }
            auto submitted = persistence->submit({.metadata = std::move(event.metadata),
                                                  .window = std::move(event.window),
                                                  .key_frames = std::move(event.key_frames)});
            if (!submitted)
            {
                ++event_failures;
                report(submitted.error());
            }
        }
    }

    void jpeg_completed(event::KeyFrameEncodingResult result)
    {
        std::optional<PendingEvent> complete;
        std::optional<Error> encoding_error;
        {
            std::scoped_lock lock{mutex};
            const auto found = pending.find(result.event_id);
            if (found == pending.end())
                return;
            if (!result.error)
                found->second.key_frames.push_back(
                    {.descriptor = std::move(result.descriptor), .jpeg = std::move(result.jpeg)});
            else
            {
                ++event_failures;
                encoding_error = std::move(result.error);
            }
            if (found->second.remaining > 0U)
                --found->second.remaining;
            if (found->second.remaining == 0U)
            {
                complete.emplace(std::move(found->second));
                pending.erase(found);
            }
        }
        if (encoding_error)
            report(*encoding_error);
        if (complete)
            submit_persistence(std::move(*complete));
    }

    void freeze(event::FrozenEventWindow window)
    {
        if (window.triggers.empty() || window.camera_windows.empty())
            return;
        event::KeyFrameSelectionContext context;
        for (const auto& camera : window.camera_windows)
            for (const auto& frame : camera.frames)
            {
                const bool abnormal = std::ranges::any_of(window.triggers, [&](const auto& item) {
                    return item.trigger.camera_id == frame.camera_id() &&
                           item.trigger.sequence_number == frame.sequence_number();
                });
                context.analyses.push_back({.frame = {.camera_id = frame.camera_id(),
                                                      .sequence_number = frame.sequence_number()},
                                            .abnormal = abnormal,
                                            .change_score = abnormal ? 1.0 : 0.0,
                                            .confidence = abnormal ? 1.0 : 0.0});
            }
        event::KeyFrameSelector selector;
        auto selected = selector.select(window, context);
        if (!selected)
        {
            ++event_failures;
            report(selected.error());
            return;
        }
        if (selected.value().frames.size() > pipeline->configuration.event.key_frame_count)
            selected.value().frames.resize(pipeline->configuration.event.key_frame_count);
        const auto& trigger = window.triggers.front().trigger;
        std::vector<std::string> camera_ids;
        for (const auto& camera : window.camera_windows)
            camera_ids.push_back(camera.camera_id);
        PendingEvent pending_event{
            .metadata = {.event_id = window.event_id,
                         .event_state = "Candidate",
                         .candidate_time = trigger.wall_clock_time,
                         .start_time = earliest_wall_time(window),
                         .end_time = latest_wall_time(window),
                         .camera_ids = std::move(camera_ids),
                         .trigger_camera_id = trigger.camera_id,
                         .trigger_frame_number = trigger.camera_frame_number,
                         .trigger_reason = trigger_reason(trigger),
                         .confidence = trigger.triggered ? 1.0 : 0.0,
                         .pre_event_duration =
                             std::chrono::seconds{pipeline->configuration.event.pre_event_seconds},
                         .post_event_duration =
                             std::chrono::seconds{pipeline->configuration.event.post_event_seconds},
                         .algorithm_name = "mock-detector",
                         .algorithm_version = "m5",
                         .config_version = std::to_string(pipeline->configuration.config_revision),
                         .machine_id = pipeline->configuration.system.machine_id,
                         .production_line_id = pipeline->configuration.system.production_line_id,
                         .paper_type = "not-configured",
                         .upload_state = "Pending",
                         .time_quality = "Normal"},
            .window = std::move(window),
            .remaining = selected.value().frames.size(),
            .save_raw = pipeline->configuration.event.save_raw};
        const auto event_id = pending_event.metadata.event_id;
        bool pending_full = false;
        {
            std::scoped_lock lock{mutex};
            if (pending.size() >= 4U)
            {
                ++event_failures;
                pending_full = true;
            }
            else
            {
                pending.emplace(event_id, std::move(pending_event));
                ++events_frozen;
            }
        }
        if (pending_full)
        {
            report(runtime_error("EVENT_QUEUE_FULL", Severity::critical, "待关键帧事件达到固定上限",
                                 "event.runtime.pending"));
            return;
        }
        if (selected.value().frames.empty())
        {
            std::optional<PendingEvent> complete;
            {
                std::scoped_lock lock{mutex};
                const auto found = pending.find(event_id);
                complete.emplace(std::move(found->second));
                pending.erase(found);
            }
            submit_persistence(std::move(*complete));
            return;
        }
        auto submitted = jpeg->submit(selected.value());
        if (!submitted)
        {
            std::optional<PendingEvent> complete;
            {
                std::scoped_lock lock{mutex};
                const auto found = pending.find(event_id);
                if (found != pending.end())
                {
                    complete.emplace(std::move(found->second));
                    pending.erase(found);
                }
                ++event_failures;
            }
            report(submitted.error());
            if (complete)
                submit_persistence(std::move(*complete));
        }
    }

    void process_frame(camera::FrameView frame)
    {
        auto* lane = find_lane(*pipeline, frame.camera_id());
        if (lane == nullptr || !pipeline->windows || !pipeline->candidates)
            return;
        pipeline->last_monotonic_time = frame.received_monotonic_time();
        {
            std::scoped_lock lock{mutex};
            if (lane->manual_pending && frame.sequence_number() > lane->manual_after_sequence)
            {
                static_cast<void>(lane->detector->request_manual_trigger());
                lane->manual_pending = false;
            }
        }
        auto pushed = lane->ring->push(frame);
        if (!pushed)
        {
            report(pushed.error());
            return;
        }
        auto detection = lane->detector->process(frame);
        if (!detection)
        {
            ++detector_failures;
            report(detection.error());
        }
        else
        {
            auto candidate = pipeline->candidates->process(detection.value());
            if (!candidate)
            {
                ++event_failures;
                report(candidate.error());
            }
            else if (candidate.value().camera.event && detection.value().triggered)
            {
                const auto& source = candidate.value().camera.event->event_id;
                if (!pipeline->source_to_canonical.contains(source))
                {
                    auto window_started =
                        pipeline->windows->start_or_merge(source, detection.value());
                    if (!window_started)
                    {
                        ++event_failures;
                        report(window_started.error());
                    }
                    else
                    {
                        pipeline->source_to_canonical[source] =
                            window_started.value().event.event_id;
                        ++events_started;
                    }
                }
            }
        }
        static_cast<void>(pipeline->candidates->advance_time(frame.received_monotonic_time(),
                                                             frame.received_wall_clock_time()));
        for (auto& frozen : pipeline->windows->advance_time(frame.received_monotonic_time()))
            freeze(std::move(frozen));
    }

    void run(const std::stop_token token)
    {
        while (true)
        {
            std::optional<camera::FrameView> frame;
            {
                std::unique_lock lock{mutex};
                condition.wait(lock, token, [&] { return stop_requested || !frames.empty(); });
                if (frames.empty() && (stop_requested || token.stop_requested()))
                    break;
                if (frames.empty())
                    continue;
                frame.emplace(std::move(frames.front()));
                frames.pop_front();
            }
            process_frame(std::move(*frame));
            {
                std::scoped_lock lock{mutex};
                ++processed_frames;
            }
        }
        if (pipeline->windows)
            for (auto& frozen : pipeline->windows->stop(pipeline->last_monotonic_time))
                freeze(std::move(frozen));
        for (auto& lane : pipeline->lanes)
            lane.ring->close();
    }
};

Result<std::shared_ptr<EventRuntime>> EventRuntime::create(EventRuntimeOptions options)
{
    if (!options.database || options.frame_queue_capacity == 0U ||
        options.frame_queue_capacity > 256U || options.persistence_capacity == 0U ||
        options.persistence_capacity > 64U)
        return Result<std::shared_ptr<EventRuntime>>::failure(runtime_error(
            "SYS_CONFIG_INVALID", Severity::error, "事件运行时配置无效", "event.runtime.create"));
    auto pipeline = build_pipeline(options.configuration, options.frame_queue_capacity);
    if (!pipeline)
        return Result<std::shared_ptr<EventRuntime>>::failure(std::move(pipeline).error());
    const auto event_root = options.event_root.empty()
                                ? resolve_config_path(options.configuration.storage.event_root)
                                : options.event_root;
    auto writer = storage::EventTransactionWriter::create({.event_root = event_root});
    if (!writer)
        return Result<std::shared_ptr<EventRuntime>>::failure(std::move(writer).error());
    auto recovery = writer.value()->recover_pending();
    if (!recovery)
        return Result<std::shared_ptr<EventRuntime>>::failure(std::move(recovery).error());
    auto reconciled = options.database->reconcile();
    if (!reconciled)
        return Result<std::shared_ptr<EventRuntime>>::failure(std::move(reconciled).error());

    auto impl = std::make_unique<EventRuntimeImpl>();
    impl->options = std::move(options);
    impl->pipeline = std::move(pipeline).value();
    auto* raw = impl.get();
    auto persistence_runtime = storage::EventPersistenceRuntime::create(
        std::move(writer).value(),
        [raw](storage::EventPersistenceCompletion completion) {
            raw->persistence_completed(std::move(completion));
        },
        {.event_capacity = impl->options.persistence_capacity});
    if (!persistence_runtime)
        return Result<std::shared_ptr<EventRuntime>>::failure(
            std::move(persistence_runtime).error());
    impl->persistence = std::move(persistence_runtime).value();
    auto jpeg = event::KeyFrameJpegRuntime::create(
        event::make_opencv_key_frame_jpeg_encoder(),
        [raw](event::KeyFrameEncodingResult result) { raw->jpeg_completed(std::move(result)); },
        {.job_capacity = event::key_frame_default_job_capacity});
    if (!jpeg)
        return Result<std::shared_ptr<EventRuntime>>::failure(std::move(jpeg).error());
    impl->jpeg = std::move(jpeg).value();
    return Result<std::shared_ptr<EventRuntime>>::success(
        std::make_shared<EventRuntime>(ConstructionKey{}, std::move(impl)));
}

EventRuntime::EventRuntime(ConstructionKey, std::unique_ptr<EventRuntimeImpl> impl)
    : impl_(std::move(impl))
{
}

EventRuntime::~EventRuntime()
{
    request_stop();
    static_cast<void>(join(std::chrono::steady_clock::now() + 5s));
}

Result<void> EventRuntime::start()
{
    std::scoped_lock lock{impl_->mutex};
    if (impl_->started)
        return Result<void>::success();
    auto persistence = impl_->persistence->start();
    if (!persistence)
        return persistence;
    auto jpeg = impl_->jpeg->start();
    if (!jpeg)
    {
        impl_->persistence->request_stop();
        static_cast<void>(impl_->persistence->join(std::chrono::steady_clock::now() + 2s));
        return jpeg;
    }
    try
    {
        impl_->stop_requested = false;
        impl_->accepting = true;
        impl_->worker = std::jthread{[this](const std::stop_token token) { impl_->run(token); }};
        impl_->started = true;
    }
    catch (const std::exception&)
    {
        impl_->jpeg->request_stop();
        impl_->persistence->request_stop();
        static_cast<void>(impl_->jpeg->join(std::chrono::steady_clock::now() + 2s));
        static_cast<void>(impl_->persistence->join(std::chrono::steady_clock::now() + 2s));
        return Result<void>::failure(runtime_error("SYS_INTERNAL_ERROR", Severity::critical,
                                                   "无法创建事件管理线程", "event.runtime.start"));
    }
    return Result<void>::success();
}

Result<void> EventRuntime::submit_frame(camera::FrameView frame)
{
    std::scoped_lock lock{impl_->mutex};
    if (!impl_->accepting)
    {
        ++impl_->rejected_frames;
        return Result<void>::failure(runtime_error("SYS_SERVICE_STOPPING", Severity::warning,
                                                   "事件运行时未接收新帧", "event.runtime.submit",
                                                   true));
    }
    if (impl_->frames.size() >= impl_->options.frame_queue_capacity)
    {
        ++impl_->rejected_frames;
        return Result<void>::failure(runtime_error("ALGORITHM_QUEUE_FULL", Severity::error,
                                                   "事件帧队列已满", "event.runtime.submit", true));
    }
    if (auto* lane = find_lane(*impl_->pipeline, frame.camera_id()))
        lane->latest_submitted_sequence =
            std::max(lane->latest_submitted_sequence, frame.sequence_number());
    impl_->frames.push_back(std::move(frame));
    ++impl_->submitted_frames;
    impl_->frame_high_watermark = std::max(impl_->frame_high_watermark, impl_->frames.size());
    impl_->condition.notify_one();
    return Result<void>::success();
}

Result<bool> EventRuntime::request_manual_trigger(const std::string_view camera_id)
{
    std::scoped_lock lock{impl_->mutex};
    if (!impl_->accepting)
        return Result<bool>::failure(runtime_error("SYS_SERVICE_STOPPING", Severity::warning,
                                                   "事件运行时未接收人工触发",
                                                   "event.runtime.manualTrigger", true));
    auto* lane = find_lane(*impl_->pipeline, camera_id);
    if (lane == nullptr)
    {
        auto error = runtime_error("CAMERA_NOT_FOUND", Severity::error, "逻辑相机未启用事件检测",
                                   "event.runtime.manualTrigger");
        error.source_id = std::string{camera_id};
        return Result<bool>::failure(std::move(error));
    }
    if (lane->manual_pending)
        return Result<bool>::success(false);
    lane->manual_after_sequence = lane->latest_submitted_sequence;
    lane->manual_pending = true;
    return Result<bool>::success(true);
}

Result<void> EventRuntime::reconfigure(const config::EdgeConfig& configuration)
{
    auto candidate = build_pipeline(configuration, impl_->options.frame_queue_capacity);
    if (!candidate)
        return Result<void>::failure(std::move(candidate).error());
    bool restart_worker = false;
    {
        std::scoped_lock lock{impl_->mutex};
        restart_worker = impl_->started && impl_->accepting;
        if (restart_worker)
        {
            impl_->accepting = false;
            impl_->stop_requested = true;
            impl_->condition.notify_all();
        }
    }
    if (restart_worker && impl_->worker.joinable())
        impl_->worker.join();
    {
        std::scoped_lock lock{impl_->mutex};
        impl_->pipeline = std::move(candidate).value();
        impl_->options.configuration = configuration;
        impl_->stop_requested = false;
        if (restart_worker)
        {
            try
            {
                impl_->worker =
                    std::jthread{[this](const std::stop_token token) { impl_->run(token); }};
                impl_->accepting = true;
            }
            catch (const std::exception&)
            {
                return Result<void>::failure(runtime_error("SYS_INTERNAL_ERROR", Severity::critical,
                                                           "无法重建事件管理线程",
                                                           "event.runtime.reconfigure"));
            }
        }
    }
    return Result<void>::success();
}

void EventRuntime::request_stop() noexcept
{
    {
        std::scoped_lock lock{impl_->mutex};
        impl_->accepting = false;
        impl_->stop_requested = true;
    }
    impl_->condition.notify_all();
    if (impl_->worker.joinable())
        impl_->worker.request_stop();
}

Result<void> EventRuntime::join(const std::chrono::steady_clock::time_point deadline)
{
    request_stop();
    if (impl_->worker.joinable())
        impl_->worker.join();
    impl_->jpeg->request_stop();
    auto jpeg = impl_->jpeg->join(deadline);
    impl_->persistence->request_stop();
    auto persistence = impl_->persistence->join(deadline);
    {
        std::scoped_lock lock{impl_->mutex};
        impl_->started = false;
    }
    if (!jpeg)
        return jpeg;
    return persistence;
}

EventRuntimeSnapshot EventRuntime::snapshot() const noexcept
{
    std::scoped_lock lock{impl_->mutex};
    return {.started = impl_->started,
            .accepting = impl_->accepting,
            .frame_queue_depth = impl_->frames.size(),
            .frame_queue_capacity = impl_->options.frame_queue_capacity,
            .frame_queue_high_watermark = impl_->frame_high_watermark,
            .pending_events = impl_->pending.size(),
            .submitted_frames = impl_->submitted_frames.load(),
            .processed_frames = impl_->processed_frames.load(),
            .rejected_frames = impl_->rejected_frames.load(),
            .detector_failures = impl_->detector_failures.load(),
            .events_started = impl_->events_started.load(),
            .events_frozen = impl_->events_frozen.load(),
            .events_committed = impl_->events_committed.load(),
            .event_failures = impl_->event_failures.load()};
}

} // namespace paperbreak::service
