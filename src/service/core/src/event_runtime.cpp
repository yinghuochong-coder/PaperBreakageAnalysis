#include "paperbreak/service/event_runtime.hpp"

#include "paperbreak/algorithm/classical_vision_detector.hpp"
#include "paperbreak/algorithm/mock_trigger_detector.hpp"
#include "paperbreak/event/candidate_event.hpp"
#include "paperbreak/event/event_window.hpp"
#include "paperbreak/event/key_frame.hpp"
#include "paperbreak/event/key_frame_opencv.hpp"
#include "paperbreak/event/memory_ring.hpp"
#include "paperbreak/storage/event_store.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
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
#include <tuple>
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

Error algorithm_runtime_error(std::string code, Severity severity, std::string message,
                              std::string operation, const std::string_view camera_id,
                              const bool retryable = false)
{
    auto error = runtime_error(std::move(code), severity, std::move(message), std::move(operation),
                               retryable);
    error.module = "algorithm";
    error.source_id = std::string{camera_id};
    return error;
}

std::filesystem::path path_from_utf8(const std::string_view value)
{
    std::u8string converted;
    converted.reserve(value.size());
    for (const unsigned char byte : value)
        converted.push_back(static_cast<char8_t>(byte));
    return std::filesystem::path{converted};
}

std::string algorithm_worker_thread_name(const std::string_view camera_id)
{
    std::string camera_suffix{camera_id};
    std::ranges::transform(camera_suffix, camera_suffix.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return "algorithm-worker-" + camera_suffix;
}

std::filesystem::path resolve_config_path(const std::string_view value)
{
    auto path = path_from_utf8(value);
    if (path.is_relative())
        path = std::filesystem::current_path() / path;
    return path.lexically_normal();
}

struct AlgorithmResultEnvelope final
{
    camera::FrameView frame;
    std::optional<algorithm::DetectionResult> detection;
};

struct QueuedAlgorithmFrame final
{
    camera::FrameView frame;
    std::chrono::steady_clock::time_point enqueued_at;
};

struct Lane final
{
    std::string camera_id;
    std::unique_ptr<event::MemoryRing> ring;
    std::unique_ptr<algorithm::DetectorHost> detector;
    std::optional<algorithm::DetectorInfo> detector_info;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<QueuedAlgorithmFrame> frames;
    std::optional<camera::MonotonicTime> in_flight_time;
    std::jthread worker;
    bool stop_requested{};
    std::optional<camera::FrameView> latest_frame;
    std::uint64_t latest_submitted_sequence{};
    std::uint64_t manual_after_sequence{};
    std::optional<std::uint64_t> manual_target_sequence;
    bool manual_pending{};
    std::size_t frame_high_watermark{};
    std::atomic_bool degraded{};
    std::atomic_uint64_t submitted_frames{};
    std::atomic_uint64_t processed_frames{};
    std::atomic_uint64_t skipped_frames{};
    std::atomic_uint64_t detector_failures{};
    std::atomic_uint64_t consecutive_detector_failures{};
    std::atomic_uint64_t consecutive_backlog_events{};
    bool backlog_active{};
    std::uint64_t consecutive_bad_backlog_windows{};
    std::uint64_t consecutive_healthy_backlog_windows{};
    std::optional<std::chrono::steady_clock::time_point> backlog_window_start;
    std::uint64_t window_submitted{};
    std::uint64_t window_processed{};
    std::uint64_t window_skipped{};
    std::atomic_uint64_t detector_process_calls{};
    std::atomic_int64_t last_algorithm_processing_us{};
    std::atomic_int64_t total_algorithm_processing_us{};
    std::atomic_int64_t maximum_algorithm_processing_us{};
    std::atomic_int64_t last_queue_wait_us{};
    std::atomic_int64_t total_queue_wait_us{};
    std::atomic_int64_t maximum_queue_wait_us{};
    std::atomic_int64_t last_end_to_end_us{};
    std::atomic_int64_t total_end_to_end_us{};
    std::atomic_int64_t maximum_end_to_end_us{};
    std::atomic<double> input_fps{};
    std::atomic<double> processed_fps{};
    std::atomic<double> skipped_ratio{};
    std::atomic_uint64_t result_queue_rejected{};
    std::atomic_uint64_t candidates_created{};
    std::atomic_uint64_t confirmed_events{};
    std::atomic_uint64_t rejected_candidates{};
};

std::string detector_plugin_id(const config::AlgorithmConfig& configuration)
{
    return configuration.type == "mock" ? std::string{algorithm::mock::mock_trigger_plugin_id}
                                        : configuration.type;
}

algorithm::DetectorConfig detector_config(const config::EdgeConfig& configuration,
                                          const std::string_view camera_id)
{
    algorithm::DetectorConfig result{.plugin_id = detector_plugin_id(configuration.algorithm),
                                     .camera_id = std::string{camera_id},
                                     .revision = configuration.config_revision,
                                     .processing_timeout = 100ms};
    if (result.plugin_id == algorithm::classical::classical_vision_plugin_id)
    {
        result.parameters = {
            {.name = "roi_offset_x",
             .value = static_cast<std::int64_t>(configuration.algorithm.roi.offset_x)},
            {.name = "roi_offset_y",
             .value = static_cast<std::int64_t>(configuration.algorithm.roi.offset_y)},
            {.name = "roi_width",
             .value = static_cast<std::int64_t>(configuration.algorithm.roi.width)},
            {.name = "roi_height",
             .value = static_cast<std::int64_t>(configuration.algorithm.roi.height)},
        };
    }
    return result;
}

struct EventPipelineState final
{
    config::EdgeConfig configuration;
    std::unique_ptr<algorithm::DetectorPluginRegistry> registry;
    std::vector<std::unique_ptr<Lane>> lanes;
    std::unique_ptr<event::CandidateEventManager> candidates;
    std::unique_ptr<event::EventWindowManager> windows;
    std::map<std::string, std::string> source_to_canonical;
    std::map<std::string, std::string> source_decisions;
    std::set<std::string> counted_confirmed_events;
    std::mutex lifecycle_mutex;
    camera::MonotonicTime last_monotonic_time{};
    camera::WallClockTime last_wall_clock_time{};
    std::mutex result_mutex;
    std::condition_variable result_condition;
    std::deque<AlgorithmResultEnvelope> results;
    std::size_t result_capacity{algorithm_result_queue_default_capacity};
    std::size_t result_high_watermark{};
    std::uint64_t result_generation{};
    std::jthread event_worker;
    bool event_stop_requested{};

    std::mutex start_mutex;
    std::condition_variable start_condition;
    bool start_gate_open{};
    bool start_cancelled{};
};

Result<std::unique_ptr<EventPipelineState>> build_pipeline(
    const config::EdgeConfig& configuration, const std::size_t frame_queue_capacity,
    const std::size_t result_queue_capacity,
    const std::function<Result<void>(algorithm::DetectorPluginRegistry&)>& registry_configurer)
{
    auto state = std::make_unique<EventPipelineState>();
    state->configuration = configuration;
    state->result_capacity = result_queue_capacity;
    state->registry = std::make_unique<algorithm::DetectorPluginRegistry>();
    if (auto registered = algorithm::mock::register_mock_trigger_detector(*state->registry);
        !registered)
        return Result<std::unique_ptr<EventPipelineState>>::failure(std::move(registered).error());
    if (auto registered =
            algorithm::classical::register_classical_vision_detector(*state->registry);
        !registered)
        return Result<std::unique_ptr<EventPipelineState>>::failure(std::move(registered).error());
    if (registry_configurer)
    {
        auto configured = registry_configurer(*state->registry);
        if (!configured)
            return Result<std::unique_ptr<EventPipelineState>>::failure(
                std::move(configured).error());
    }
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
             .nvme_queue_frames =
                 configuration.storage.rolling_cache_enabled
                     ? (static_cast<std::size_t>(std::ceil(camera.frame_rate)) + 2U) * 4U
                     : 0U,
             .post_event_seconds = static_cast<double>(configuration.event.post_event_seconds),
             .maximum_concurrent_events = 1U,
             .configured_frame_pool_capacity = configuration.acquisition.frame_pool_capacity,
             .memory_budget_bytes = configuration.acquisition.frame_pool_capacity});
        if (!plan)
            return Result<std::unique_ptr<EventPipelineState>>::failure(std::move(plan).error());
        std::unique_ptr<algorithm::DetectorHost> detector;
        std::optional<algorithm::DetectorInfo> detector_information;
        if (configuration.algorithm.enabled)
        {
            detector = std::make_unique<algorithm::DetectorHost>(*state->registry);
            auto loaded = detector->load(detector_config(configuration, camera.id));
            if (!loaded)
                return Result<std::unique_ptr<EventPipelineState>>::failure(
                    std::move(loaded).error());
            auto information = detector->info();
            if (!information)
                return Result<std::unique_ptr<EventPipelineState>>::failure(
                    std::move(information).error());
            detector_information = std::move(information).value();
        }
        // Keep the configured history available even when detection is delayed by a full
        // algorithm queue. The pool plan already budgets these queue-held frame buffers.
        const auto ring_capacity = plan.value().ring_capacity_frames + frame_queue_capacity;
        const auto maximum_references = plan.value().ring_capacity_frames * 8U;
        auto lane = std::make_unique<Lane>();
        lane->camera_id = camera.id;
        lane->ring = std::make_unique<event::MemoryRing>(event::MemoryRingOptions{
            .camera_id = camera.id,
            .capacity_frames = ring_capacity,
            .required_history_seconds = static_cast<double>(configuration.event.pre_event_seconds),
            .maximum_active_leases = 8U,
            .maximum_leased_frame_references = maximum_references});
        lane->detector = std::move(detector);
        lane->detector_info = std::move(detector_information);
        state->lanes.push_back(std::move(lane));
    }
    if (state->lanes.empty())
        return Result<std::unique_ptr<EventPipelineState>>::success(std::move(state));

    std::vector<event::CandidateCameraBinding> candidate_bindings;
    std::vector<event::EventWindowCameraBinding> window_bindings;
    for (auto& lane : state->lanes)
    {
        candidate_bindings.push_back(
            {.camera_id = lane->camera_id, .memory_ring = lane->ring.get()});
        window_bindings.push_back({.camera_id = lane->camera_id, .memory_ring = lane->ring.get()});
    }
    auto candidates = event::CandidateEventManager::create(
        {.cameras = std::move(candidate_bindings),
         .candidate_consecutive_frames = 1U,
         .confirmation_consecutive_frames = configuration.algorithm.consecutive_frames,
         .candidate_confidence_threshold = configuration.algorithm.candidate_threshold,
         .confirmation_confidence_threshold = configuration.algorithm.confirmation_threshold,
         .external_confirmation = configuration.plant_io.enabled
                                      ? event::ExternalConfirmationPolicy::required_active
                                      : event::ExternalConfirmationPolicy::not_used,
         .candidate_timeout = std::chrono::seconds{configuration.event.max_event_seconds},
         .pre_event_duration = std::chrono::seconds{configuration.event.pre_event_seconds},
         .cooldown_duration = std::chrono::milliseconds{configuration.algorithm.cooldown_ms}});
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
        state.lanes, [camera_id](const auto& lane) { return lane->camera_id == camera_id; });
    return found == state.lanes.end() ? nullptr : found->get();
}

const Lane* find_lane(const EventPipelineState& state, const std::string_view camera_id)
{
    const auto found = std::ranges::find_if(
        state.lanes, [camera_id](const auto& lane) { return lane->camera_id == camera_id; });
    return found == state.lanes.end() ? nullptr : found->get();
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

algorithm::DetectionResult manual_detection(const camera::FrameView& frame)
{
    return {
        .triggered = true,
        .trigger_source = algorithm::TriggerSource::manual_test,
        .camera_id = frame.camera_id(),
        .sequence_number = frame.sequence_number(),
        .camera_frame_number = frame.camera_frame_number(),
        .monotonic_time = frame.received_monotonic_time(),
        .wall_clock_time = frame.received_wall_clock_time(),
        .evaluated_region = {.width = frame.geometry().width, .height = frame.geometry().height},
        .reason = "manual-test-requested",
        .anomalous = true,
        .candidate_type = algorithm::DetectionCandidateType::indeterminate,
        .confidence = 1.0,
        .detector_version = "manual-trigger/1.0",
        .model_version = "none"};
}

} // namespace

std::string_view to_string(const AlgorithmRuntimeState state) noexcept
{
    switch (state)
    {
    case AlgorithmRuntimeState::disabled:
        return "disabled";
    case AlgorithmRuntimeState::active:
        return "active";
    case AlgorithmRuntimeState::partially_degraded:
        return "partially-degraded";
    case AlgorithmRuntimeState::manual_trigger_only:
        return "manual-trigger-only";
    }
    return "disabled";
}

struct EventRuntimeImpl final
{
    struct BacklogWindowOutcome final
    {
        bool recovered{};
        std::optional<Error> degraded_error;
    };

    struct PendingEvent final
    {
        storage::EventManifestMetadata metadata;
        event::FrozenEventWindow window;
        std::size_t remaining{};
        std::vector<storage::PersistedKeyFrame> key_frames;
        std::vector<std::string> nvme_lease_ids;
        bool save_raw{true};
        bool failed{};
    };

    EventRuntimeOptions options;
    mutable std::mutex mutex;
    std::mutex reconfigure_mutex;
    std::unique_ptr<EventPipelineState> pipeline;
    std::map<std::string, PendingEvent> pending;
    std::set<std::string> nvme_lease_sources;
    std::map<std::string, std::vector<std::string>> nvme_leases_awaiting_commit;
    std::unique_ptr<event::KeyFrameJpegRuntime> jpeg;
    std::unique_ptr<storage::EventPersistenceRuntime> persistence;
    bool started{};
    bool accepting{};
    std::atomic_uint64_t rejected_frames{};
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

    [[nodiscard]] std::chrono::steady_clock::time_point now() const
    {
        return options.monotonic_now ? options.monotonic_now() : std::chrono::steady_clock::now();
    }

    void notify_backlog(const AlgorithmBacklogStateChange& change) noexcept
    {
        try
        {
            if (options.backlog_state_observer)
                options.backlog_state_observer(change);
        }
        catch (...)
        {
        }
    }

    void notify_detector_failure(const AlgorithmDetectorFailureStateChange& change) noexcept
    {
        try
        {
            if (options.detector_failure_state_observer)
                options.detector_failure_state_observer(change);
        }
        catch (...)
        {
        }
    }

    void publish_lifecycle(const storage::EventMetadataRecord& event) noexcept
    {
        try
        {
            if (options.lifecycle_observer)
                options.lifecycle_observer(event);
        }
        catch (...)
        {
        }
    }

    void transition_lifecycle(const std::string_view event_id,
                              const std::string_view decision_state,
                              const std::string_view persistence_state,
                              const std::uint64_t trigger_count,
                              const std::optional<std::int64_t> confirmed_time = std::nullopt)
    {
        auto updated = options.database->update_event_lifecycle(
            event_id, decision_state, persistence_state, trigger_count, confirmed_time);
        if (!updated)
        {
            ++event_failures;
            report(updated.error());
            return;
        }
        publish_lifecycle(updated.value());
    }

    [[nodiscard]] std::optional<Error> enter_degraded(Lane& lane,
                                                      const std::string_view reason) noexcept
    {
        bool expected = false;
        if (!lane.degraded.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return std::nullopt;
        auto error = runtime_error("ALGORITHM_DEGRADED", Severity::error,
                                   "自动视觉检测已降级为仅人工触发", "algorithm.runtime.degrade");
        error.module = "algorithm";
        error.source_id = lane.camera_id;
        error.details.push_back({"reason", std::string{reason}});
        error.details.push_back(
            {"failureLimit", std::to_string(options.consecutive_failure_limit)});
        error.details.push_back(
            {"backlogLimit", std::to_string(options.consecutive_backlog_limit)});
        error.details.push_back(
            {"backlogWindowMs", std::to_string(options.backlog_window.count())});
        error.details.push_back(
            {"backlogWindowLimit", std::to_string(options.backlog_degrade_window_limit)});
        return error;
    }

    [[nodiscard]] BacklogWindowOutcome advance_backlog_windows(
        Lane& lane, const std::chrono::steady_clock::time_point current)
    {
        BacklogWindowOutcome outcome;
        if (!lane.backlog_window_start)
        {
            lane.backlog_window_start = current;
            return outcome;
        }
        const auto seconds = std::chrono::duration<double>{options.backlog_window}.count();
        while (current - *lane.backlog_window_start >= options.backlog_window)
        {
            lane.input_fps.store(static_cast<double>(lane.window_submitted) / seconds,
                                 std::memory_order_relaxed);
            lane.processed_fps.store(static_cast<double>(lane.window_processed) / seconds,
                                     std::memory_order_relaxed);
            lane.skipped_ratio.store(lane.window_submitted == 0U
                                         ? 0.0
                                         : static_cast<double>(lane.window_skipped) /
                                               static_cast<double>(lane.window_submitted),
                                     std::memory_order_relaxed);

            const bool bad = lane.window_skipped >= options.consecutive_backlog_limit;
            if (bad)
            {
                ++lane.consecutive_bad_backlog_windows;
                lane.consecutive_healthy_backlog_windows = 0U;
            }
            else
            {
                lane.consecutive_bad_backlog_windows = 0U;
                if (lane.window_skipped == 0U &&
                    lane.frames.size() * 4U < options.frame_queue_capacity)
                    ++lane.consecutive_healthy_backlog_windows;
                else
                    lane.consecutive_healthy_backlog_windows = 0U;
            }
            if (lane.consecutive_bad_backlog_windows >= options.backlog_degrade_window_limit)
                outcome.degraded_error = enter_degraded(lane, "sustained-queue-backlog");
            if (lane.backlog_active &&
                lane.consecutive_healthy_backlog_windows >= options.backlog_recovery_window_limit)
            {
                lane.backlog_active = false;
                lane.consecutive_backlog_events.store(0U);
                outcome.recovered = true;
            }

            lane.window_submitted = 0U;
            lane.window_processed = 0U;
            lane.window_skipped = 0U;
            *lane.backlog_window_start += options.backlog_window;
        }
        return outcome;
    }

    static void record_processing_time(Lane& lane, const std::chrono::microseconds elapsed) noexcept
    {
        ++lane.detector_process_calls;
        lane.last_algorithm_processing_us.store(elapsed.count(), std::memory_order_relaxed);
        lane.total_algorithm_processing_us.fetch_add(elapsed.count(), std::memory_order_relaxed);
        auto maximum = lane.maximum_algorithm_processing_us.load(std::memory_order_relaxed);
        while (maximum < elapsed.count() &&
               !lane.maximum_algorithm_processing_us.compare_exchange_weak(
                   maximum, elapsed.count(), std::memory_order_relaxed))
        {
        }
    }

    static void record_latency(std::atomic_int64_t& last, std::atomic_int64_t& total,
                               std::atomic_int64_t& maximum,
                               const std::chrono::microseconds elapsed) noexcept
    {
        last.store(elapsed.count(), std::memory_order_relaxed);
        total.fetch_add(elapsed.count(), std::memory_order_relaxed);
        auto observed = maximum.load(std::memory_order_relaxed);
        while (observed < elapsed.count() &&
               !maximum.compare_exchange_weak(observed, elapsed.count(), std::memory_order_relaxed))
        {
        }
    }

    [[nodiscard]] AlgorithmRuntimeSnapshot lane_snapshot(const EventPipelineState& state,
                                                         const Lane& lane) const
    {
        std::scoped_lock lane_lock{lane.mutex};
        const auto process_calls = lane.detector_process_calls.load();
        const auto total_processing = lane.total_algorithm_processing_us.load();
        const auto runtime_state = !state.configuration.algorithm.enabled
                                       ? AlgorithmRuntimeState::disabled
                                       : (lane.degraded.load(std::memory_order_acquire)
                                              ? AlgorithmRuntimeState::manual_trigger_only
                                              : AlgorithmRuntimeState::active);
        return {
            .camera_id = lane.camera_id,
            .config_revision = state.configuration.config_revision,
            .state = runtime_state,
            .has_current_frame = lane.latest_frame.has_value(),
            .latest_sequence_number = lane.latest_frame ? lane.latest_frame->sequence_number() : 0U,
            .detector_info = lane.detector_info,
            .metrics = {
                .frame_queue_depth = lane.frames.size(),
                .frame_queue_capacity = options.frame_queue_capacity,
                .frame_queue_high_watermark = lane.frame_high_watermark,
                .submitted_frames = lane.submitted_frames.load(),
                .processed_frames = lane.processed_frames.load(),
                .skipped_frames = lane.skipped_frames.load(),
                .detector_failures = lane.detector_failures.load(),
                .consecutive_detector_failures = lane.consecutive_detector_failures.load(),
                .consecutive_backlog_events = lane.consecutive_backlog_events.load(),
                .backlog_active = lane.backlog_active,
                .consecutive_bad_backlog_windows = lane.consecutive_bad_backlog_windows,
                .consecutive_healthy_backlog_windows = lane.consecutive_healthy_backlog_windows,
                .detector_process_calls = process_calls,
                .last_algorithm_processing_time =
                    std::chrono::microseconds{lane.last_algorithm_processing_us.load()},
                .average_algorithm_processing_time =
                    std::chrono::microseconds{process_calls == 0U
                                                  ? 0
                                                  : total_processing /
                                                        static_cast<std::int64_t>(process_calls)},
                .maximum_algorithm_processing_time =
                    std::chrono::microseconds{lane.maximum_algorithm_processing_us.load()},
                .last_queue_wait_time = std::chrono::microseconds{lane.last_queue_wait_us.load()},
                .average_queue_wait_time =
                    std::chrono::microseconds{
                        lane.processed_frames.load() == 0U
                            ? 0
                            : lane.total_queue_wait_us.load() /
                                  static_cast<std::int64_t>(lane.processed_frames.load())},
                .maximum_queue_wait_time =
                    std::chrono::microseconds{lane.maximum_queue_wait_us.load()},
                .last_end_to_end_time = std::chrono::microseconds{lane.last_end_to_end_us.load()},
                .average_end_to_end_time =
                    std::chrono::microseconds{
                        lane.processed_frames.load() == 0U
                            ? 0
                            : lane.total_end_to_end_us.load() /
                                  static_cast<std::int64_t>(lane.processed_frames.load())},
                .maximum_end_to_end_time =
                    std::chrono::microseconds{lane.maximum_end_to_end_us.load()},
                .input_fps = lane.input_fps.load(),
                .processed_fps = lane.processed_fps.load(),
                .skipped_ratio = lane.skipped_ratio.load(),
                .result_queue_rejected = lane.result_queue_rejected.load(),
                .candidates_created = lane.candidates_created.load(),
                .confirmed_events = lane.confirmed_events.load(),
                .rejected_candidates = lane.rejected_candidates.load()}};
    }

    void persistence_completed(storage::EventPersistenceCompletion completion)
    {
        if (!completion.outcome)
        {
            ++event_failures;
            transition_lifecycle(completion.event_id, "Rejected", "Incomplete", 1U);
            if (completion.error)
                report(*completion.error);
            return;
        }
        auto indexed = options.database->index_committed_manifest(
            completion.outcome->committed_directory, completion.outcome->manifest_json);
        if (!indexed)
        {
            ++event_failures;
            transition_lifecycle(completion.event_id, "Rejected", "Incomplete", 1U);
            report(indexed.error());
            return;
        }
        auto record = options.database->get_event(completion.event_id);
        if (!record)
        {
            report(record.error());
            return;
        }
        bool leases_released = true;
        std::vector<std::string> lease_ids;
        {
            std::scoped_lock lock{mutex};
            const auto found = nvme_leases_awaiting_commit.find(completion.event_id);
            if (found != nvme_leases_awaiting_commit.end())
                lease_ids = found->second;
        }
        if (options.nvme_cache)
        {
            for (const auto& lease_id : lease_ids)
            {
                auto released = options.nvme_cache->release_event(lease_id);
                if (!released)
                {
                    leases_released = false;
                    report(released.error());
                }
            }
        }
        if (leases_released)
        {
            std::scoped_lock lock{mutex};
            nvme_leases_awaiting_commit.erase(completion.event_id);
            for (const auto& lease_id : lease_ids)
                nvme_lease_sources.erase(lease_id);
        }
        ++events_committed;
        publish_lifecycle(record.value());
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
                    transition_lifecycle(
                        event.metadata.event_id, event.metadata.decision_state, "Incomplete",
                        event.metadata.trigger_count,
                        event.metadata.confirmed_time
                            ? std::optional<std::int64_t>{std::chrono::duration_cast<
                                                              std::chrono::milliseconds>(
                                                              event.metadata.confirmed_time
                                                                  ->time_since_epoch())
                                                              .count()}
                            : std::nullopt);
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
            const auto event_id = event.metadata.event_id;
            transition_lifecycle(
                event_id, event.metadata.decision_state, "Queued", event.metadata.trigger_count,
                event.metadata.confirmed_time
                    ? std::optional<
                          std::int64_t>{std::chrono::duration_cast<std::chrono::milliseconds>(
                                            event.metadata.confirmed_time->time_since_epoch())
                                            .count()}
                    : std::nullopt);
            if (!event.nvme_lease_ids.empty())
            {
                bool lease_tracking_full = false;
                {
                    std::scoped_lock lock{mutex};
                    if (nvme_leases_awaiting_commit.size() <
                            storage::nvme_default_maximum_event_leases ||
                        nvme_leases_awaiting_commit.contains(event_id))
                        nvme_leases_awaiting_commit[event_id] = event.nvme_lease_ids;
                    else
                        lease_tracking_full = true;
                }
                if (lease_tracking_full)
                {
                    ++event_failures;
                    report(runtime_error("NVME_LEASE_CAPACITY", Severity::error,
                                         "事件租约提交跟踪达到固定上限", "event.runtime.nvmeLease",
                                         true));
                }
            }
            auto submitted = persistence->submit({.metadata = std::move(event.metadata),
                                                  .window = std::move(event.window),
                                                  .key_frames = std::move(event.key_frames)});
            if (!submitted)
            {
                ++event_failures;
                transition_lifecycle(event_id, "Rejected", "Incomplete",
                                     event.metadata.trigger_count);
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
                found->second.failed = true;
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
        if (complete && complete->failed)
            transition_lifecycle(complete->metadata.event_id, complete->metadata.decision_state,
                                 "Incomplete", complete->metadata.trigger_count);
        else if (complete)
            submit_persistence(std::move(*complete));
    }

    static bool terminal_decision(const std::string_view decision) noexcept
    {
        return decision == "Confirmed" || decision == "Rejected" || decision == "Timeout";
    }

    static void release_source_mapping(EventPipelineState& state, const std::string_view source_id)
    {
        state.source_decisions.erase(std::string{source_id});
        state.source_to_canonical.erase(std::string{source_id});
        state.counted_confirmed_events.erase(std::string{source_id});
    }

    void freeze(EventPipelineState& state, event::FrozenEventWindow window)
    {
        std::vector<std::string> source_ids;
        source_ids.reserve(window.triggers.size());
        for (const auto& item : window.triggers)
            source_ids.push_back(item.source_event_id);
        const auto release_terminal_sources = [&] {
            for (const auto& source_id : source_ids)
            {
                const auto decision = state.source_decisions.find(source_id);
                if (decision != state.source_decisions.end() && terminal_decision(decision->second))
                    release_source_mapping(state, source_id);
            }
        };
        if (window.triggers.empty() || window.camera_windows.empty())
        {
            release_terminal_sources();
            return;
        }
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
            transition_lifecycle(window.event_id, "Candidate", "Incomplete", source_ids.size());
            report(selected.error());
            release_terminal_sources();
            return;
        }
        if (selected.value().frames.size() > state.configuration.event.key_frame_count)
            selected.value().frames.resize(state.configuration.event.key_frame_count);
        const auto& trigger = window.triggers.front().trigger;
        std::string aggregate_decision = "Candidate";
        std::optional<camera::WallClockTime> aggregate_confirmed_time;
        if (auto aggregate = options.database->get_event(window.event_id); aggregate)
        {
            aggregate_decision = aggregate.value().decision_state;
            if (aggregate.value().confirmed_time_utc_ms)
                aggregate_confirmed_time = camera::WallClockTime{
                    std::chrono::milliseconds{*aggregate.value().confirmed_time_utc_ms}};
        }
        std::vector<std::string> camera_ids;
        for (const auto& camera : window.camera_windows)
            camera_ids.push_back(camera.camera_id);
        PendingEvent pending_event{
            .metadata = {.event_id = window.event_id,
                         .event_state = aggregate_decision,
                         .decision_state = aggregate_decision,
                         .trigger_count = window.triggers.size(),
                         .candidate_time = trigger.wall_clock_time,
                         .confirmed_time = aggregate_confirmed_time,
                         .start_time = earliest_wall_time(window),
                         .end_time = latest_wall_time(window),
                         .camera_ids = std::move(camera_ids),
                         .trigger_camera_id = trigger.camera_id,
                         .trigger_frame_number = trigger.camera_frame_number,
                         .trigger_reason = trigger_reason(trigger),
                         .confidence = trigger.confidence,
                         .pre_event_duration =
                             std::chrono::seconds{state.configuration.event.pre_event_seconds},
                         .post_event_duration =
                             std::chrono::seconds{state.configuration.event.post_event_seconds},
                         .algorithm_name =
                             trigger.trigger_source == algorithm::TriggerSource::manual_test
                                 ? "manual-trigger"
                                 : state.configuration.algorithm.type,
                         .algorithm_version = trigger.detector_version.empty()
                                                  ? "not-reported"
                                                  : trigger.detector_version,
                         .config_version = std::to_string(state.configuration.config_revision),
                         .machine_id = state.configuration.system.machine_id,
                         .production_line_id = state.configuration.system.production_line_id,
                         .paper_type = "not-configured",
                         .upload_state = "Pending",
                         .time_quality = "Normal"},
            .window = std::move(window),
            .remaining = selected.value().frames.size(),
            .save_raw = state.configuration.event.save_raw};
        transition_lifecycle(pending_event.metadata.event_id, pending_event.metadata.decision_state,
                             "Encoding", pending_event.metadata.trigger_count);
        for (const auto& trigger_item : pending_event.window.triggers)
        {
            std::scoped_lock lock{mutex};
            if (nvme_lease_sources.contains(trigger_item.source_event_id))
                pending_event.nvme_lease_ids.push_back(trigger_item.source_event_id);
        }
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
            transition_lifecycle(event_id, "Candidate", "Incomplete", source_ids.size());
            report(runtime_error("EVENT_QUEUE_FULL", Severity::critical, "待关键帧事件达到固定上限",
                                 "event.runtime.pending"));
            release_terminal_sources();
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
            release_terminal_sources();
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
                transition_lifecycle(complete->metadata.event_id, complete->metadata.decision_state,
                                     "Incomplete", complete->metadata.trigger_count);
        }
        release_terminal_sources();
    }

    [[nodiscard]] std::vector<Error> enqueue_result(EventPipelineState& state, Lane& lane,
                                                    AlgorithmResultEnvelope envelope)
    {
        std::vector<Error> errors;
        {
            std::scoped_lock lock{state.result_mutex};
            if (state.results.size() < state.result_capacity)
            {
                state.results.push_back(std::move(envelope));
                state.result_high_watermark =
                    std::max(state.result_high_watermark, state.results.size());
                ++state.result_generation;
                state.result_condition.notify_one();
                return errors;
            }
        }

        ++lane.result_queue_rejected;
        auto full = algorithm_runtime_error("ALGORITHM_RESULT_QUEUE_FULL", Severity::error,
                                            "算法结果入口已满，候选结果无法可靠交付",
                                            "algorithm.runtime.resultQueue", lane.camera_id, true);
        full.details.push_back({"queue", "algorithm.results"});
        full.details.push_back({"capacity", std::to_string(state.result_capacity)});
        errors.push_back(std::move(full));
        if (auto degraded = enter_degraded(lane, "result-queue-rejected"))
            errors.push_back(std::move(*degraded));
        return errors;
    }

    void notify_result_progress(EventPipelineState& state)
    {
        std::scoped_lock lock{state.result_mutex};
        ++state.result_generation;
        state.result_condition.notify_one();
    }

    static std::string aggregate_decision(const EventPipelineState& state,
                                          const std::string_view canonical_id)
    {
        const auto rank = [](const std::string_view value) {
            if (value == "Confirmed")
                return 4;
            if (value == "Candidate")
                return 3;
            if (value == "Timeout")
                return 2;
            if (value == "Rejected")
                return 1;
            return 0;
        };
        std::string aggregate{"Rejected"};
        for (const auto& [source_id, canonical] : state.source_to_canonical)
        {
            if (canonical != canonical_id)
                continue;
            const auto decision = state.source_decisions.find(source_id);
            if (decision != state.source_decisions.end() &&
                rank(decision->second) > rank(aggregate))
                aggregate = decision->second;
        }
        return aggregate;
    }

    void start_candidate_window(EventPipelineState& state, Lane& lane,
                                const event::CandidateEventSnapshot& candidate_event)
    {
        if (state.source_to_canonical.contains(candidate_event.event_id))
            return;

        auto window_started = state.windows->start_or_merge(candidate_event.event_id,
                                                            candidate_event.candidate_trigger);
        if (!window_started)
        {
            ++event_failures;
            report(window_started.error());
            return;
        }

        const auto canonical_id = window_started.value().event.event_id;
        for (const auto& source : window_started.value().event.triggers)
            state.source_to_canonical[source.source_event_id] = canonical_id;
        ++lane.candidates_created;
        state.source_decisions[candidate_event.event_id] =
            std::string{event::to_string(candidate_event.decision_state)};
        const auto aggregate = aggregate_decision(state, canonical_id);
        const auto confirmed_time =
            candidate_event.decision_state == event::CandidateEventState::confirmed &&
                    candidate_event.decision
                ? std::optional<std::int64_t>{std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  candidate_event.decision->wall_clock_time
                                                      .time_since_epoch())
                                                  .count()}
                : std::nullopt;
        auto existing = options.database->get_event(canonical_id);
        if (!existing && existing.error().business_code == "EVENT_NOT_FOUND")
        {
            const auto pre = std::chrono::seconds{state.configuration.event.pre_event_seconds};
            const auto post = std::chrono::seconds{state.configuration.event.post_event_seconds};
            std::vector<std::string> all_cameras;
            all_cameras.reserve(state.lanes.size());
            for (const auto& camera_lane : state.lanes)
                all_cameras.push_back(camera_lane->camera_id);
            const auto wall_ms = [](const camera::WallClockTime time) {
                return std::chrono::duration_cast<std::chrono::milliseconds>(
                           time.time_since_epoch())
                    .count();
            };
            auto created = options.database->create_collecting_event(
                {.event_id = canonical_id,
                 .decision_state = aggregate,
                 .candidate_time_utc_ms =
                     wall_ms(candidate_event.candidate_trigger.wall_clock_time),
                 .confirmed_time_utc_ms = confirmed_time,
                 .start_time_utc_ms =
                     wall_ms(candidate_event.candidate_trigger.wall_clock_time - pre),
                 .end_time_utc_ms =
                     wall_ms(candidate_event.candidate_trigger.wall_clock_time + post),
                 .camera_ids = std::move(all_cameras),
                 .trigger_camera_id = candidate_event.candidate_trigger.camera_id,
                 .trigger_frame_number = candidate_event.candidate_trigger.camera_frame_number,
                 .trigger_reason = trigger_reason(candidate_event.candidate_trigger),
                 .confidence = candidate_event.candidate_trigger.confidence,
                 .trigger_count = window_started.value().event.triggers.size()});
            if (!created)
            {
                ++event_failures;
                report(created.error());
            }
            else
            {
                ++events_started;
                publish_lifecycle(created.value());
            }
        }
        else if (!existing)
        {
            ++event_failures;
            report(existing.error());
        }
        transition_lifecycle(canonical_id, aggregate, "Collecting",
                             window_started.value().event.triggers.size(), confirmed_time);

        bool lease_exists = false;
        {
            std::scoped_lock lock{mutex};
            lease_exists = nvme_lease_sources.contains(candidate_event.event_id);
        }
        if (options.nvme_cache && !lease_exists)
        {
            std::vector<std::string> camera_ids;
            camera_ids.reserve(state.lanes.size());
            for (const auto& camera_lane : state.lanes)
                camera_ids.push_back(camera_lane->camera_id);
            const auto pre = std::chrono::seconds{state.configuration.event.pre_event_seconds};
            const auto post = std::chrono::seconds{state.configuration.event.post_event_seconds};
            auto protected_window = options.nvme_cache->protect_event_window(
                {.event_id = candidate_event.event_id,
                 .camera_ids = std::move(camera_ids),
                 .start_monotonic_time = candidate_event.candidate_trigger.monotonic_time - pre,
                 .end_monotonic_time = candidate_event.candidate_trigger.monotonic_time + post,
                 .start_wall_clock_time = candidate_event.candidate_trigger.wall_clock_time - pre,
                 .end_wall_clock_time = candidate_event.candidate_trigger.wall_clock_time + post});
            if (!protected_window)
                report(protected_window.error());
            else
            {
                std::scoped_lock lock{mutex};
                nvme_lease_sources.insert(candidate_event.event_id);
            }
        }
    }

    void apply_candidate_decision(EventPipelineState& state, Lane& lane,
                                  const event::CandidateEventSnapshot& candidate_event)
    {
        const auto decision = std::string{event::to_string(candidate_event.decision_state)};
        if (!terminal_decision(decision))
            return;
        if (candidate_event.decision_state == event::CandidateEventState::confirmed &&
            state.counted_confirmed_events.insert(candidate_event.event_id).second)
            ++lane.confirmed_events;

        const auto canonical = state.source_to_canonical.find(candidate_event.event_id);
        if (canonical == state.source_to_canonical.end())
            return;
        const auto canonical_id = canonical->second;
        state.source_decisions[candidate_event.event_id] = decision;
        auto current = options.database->get_event(canonical_id);
        if (current)
        {
            const auto confirmed_time =
                candidate_event.decision_state == event::CandidateEventState::confirmed &&
                        candidate_event.decision
                    ? std::optional<
                          std::int64_t>{std::chrono::duration_cast<std::chrono::milliseconds>(
                                            candidate_event.decision->wall_clock_time
                                                .time_since_epoch())
                                            .count()}
                    : std::nullopt;
            transition_lifecycle(canonical_id, aggregate_decision(state, canonical_id),
                                 current.value().persistence_state, current.value().trigger_count,
                                 confirmed_time);
        }
        else if (current.error().business_code != "EVENT_NOT_FOUND")
        {
            ++event_failures;
            report(current.error());
        }

        auto active = state.windows->active(candidate_event.event_id);
        if (!active && active.error().business_code == "EVENT_NOT_FOUND")
            release_source_mapping(state, candidate_event.event_id);
        else if (!active)
        {
            ++event_failures;
            report(active.error());
        }
    }

    void process_result(EventPipelineState& state, AlgorithmResultEnvelope envelope)
    {
        auto* lane = find_lane(state, envelope.frame.camera_id());
        if (lane == nullptr || !state.windows || !state.candidates)
            return;
        const std::scoped_lock lifecycle_lock{state.lifecycle_mutex};

        std::vector<event::FrozenEventWindow> frozen_windows;
        {
            state.last_monotonic_time = envelope.frame.received_monotonic_time();
            state.last_wall_clock_time = envelope.frame.received_wall_clock_time();
            if (envelope.detection)
            {
                auto candidate = state.candidates->process(*envelope.detection);
                if (!candidate)
                {
                    ++event_failures;
                    report(candidate.error());
                }
                else
                {
                    for (const auto& notification : candidate.value().notifications)
                    {
                        if (notification.kind ==
                            event::CandidateNotificationKind::candidate_created)
                            start_candidate_window(state, *lane, notification.event);
                        else
                            apply_candidate_decision(state, *lane, notification.event);
                    }
                }
            }
            const auto timed_out =
                state.candidates->advance_time(envelope.frame.received_monotonic_time(),
                                               envelope.frame.received_wall_clock_time());
            for (const auto& candidate_event : timed_out)
            {
                auto* source_lane = find_lane(state, candidate_event.camera_id);
                if (source_lane != nullptr)
                    apply_candidate_decision(state, *source_lane, candidate_event);
            }
            frozen_windows = state.windows->advance_time(envelope.frame.received_monotonic_time());
        }
        for (auto& frozen : frozen_windows)
            freeze(state, std::move(frozen));
    }

    static bool envelope_less(const AlgorithmResultEnvelope& left,
                              const AlgorithmResultEnvelope& right)
    {
        return std::tuple{left.frame.received_monotonic_time(), left.frame.camera_id(),
                          left.frame.sequence_number()} <
               std::tuple{right.frame.received_monotonic_time(), right.frame.camera_id(),
                          right.frame.sequence_number()};
    }

    static std::optional<camera::MonotonicTime> safe_watermark(EventPipelineState& state)
    {
        std::optional<camera::MonotonicTime> watermark;
        for (const auto& lane : state.lanes)
        {
            std::scoped_lock lock{lane->mutex};
            std::optional<camera::MonotonicTime> earliest = lane->in_flight_time;
            if (!lane->frames.empty())
            {
                const auto queued = lane->frames.front().frame.received_monotonic_time();
                earliest = earliest ? std::min(*earliest, queued) : queued;
            }
            if (earliest)
                watermark = watermark ? std::min(*watermark, *earliest) : earliest;
        }
        return watermark;
    }

    bool wait_for_start_gate(EventPipelineState& state)
    {
        std::unique_lock lock{state.start_mutex};
        state.start_condition.wait(lock,
                                   [&] { return state.start_gate_open || state.start_cancelled; });
        return !state.start_cancelled;
    }

    void run_lane(EventPipelineState& state, Lane& lane)
    {
        if (!wait_for_start_gate(state))
            return;
        const auto registration =
            options.register_thread
                ? options.register_thread(algorithm_worker_thread_name(lane.camera_id))
                : nullptr;
        auto diagnostic_window_started = std::chrono::steady_clock::now();
        std::uint64_t diagnostic_calls = 0U;
        std::uint64_t diagnostic_failures = 0U;
        std::uint64_t diagnostic_candidates = 0U;
        std::int64_t diagnostic_total_us = 0;
        std::int64_t diagnostic_maximum_us = 0;
        while (true)
        {
            std::optional<QueuedAlgorithmFrame> queued_frame;
            bool manual = false;
            {
                std::unique_lock lock{lane.mutex};
                lane.condition.wait(lock,
                                    [&] { return lane.stop_requested || !lane.frames.empty(); });
                if (lane.frames.empty() && lane.stop_requested)
                    break;
                queued_frame.emplace(std::move(lane.frames.front()));
                lane.frames.pop_front();
                lane.in_flight_time = queued_frame->frame.received_monotonic_time();
                if (lane.manual_pending && lane.manual_target_sequence &&
                    queued_frame->frame.sequence_number() == *lane.manual_target_sequence)
                {
                    lane.manual_pending = false;
                    lane.manual_target_sequence.reset();
                    manual = true;
                }
            }
            notify_result_progress(state);
            const auto processing_started = now();
            record_latency(lane.last_queue_wait_us, lane.total_queue_wait_us,
                           lane.maximum_queue_wait_us,
                           std::chrono::duration_cast<std::chrono::microseconds>(
                               processing_started - queued_frame->enqueued_at));
            auto& frame = queued_frame->frame;

            std::optional<algorithm::DetectionResult> completed_detection;
            std::optional<Error> detector_error;
            if (manual)
                completed_detection = manual_detection(frame);
            else if (state.configuration.algorithm.enabled && lane.detector &&
                     !lane.degraded.load(std::memory_order_acquire))
            {
                const auto detector_started = now();
                auto detection = lane.detector->process(frame);
                const auto detector_elapsed =
                    std::chrono::duration_cast<std::chrono::microseconds>(now() - detector_started);
                record_processing_time(lane, detector_elapsed);
                ++diagnostic_calls;
                diagnostic_total_us += detector_elapsed.count();
                diagnostic_maximum_us = std::max(diagnostic_maximum_us, detector_elapsed.count());
                if (!detection)
                {
                    ++diagnostic_failures;
                    const auto total_failures = lane.detector_failures.fetch_add(1U) + 1U;
                    const auto failures = lane.consecutive_detector_failures.fetch_add(1U) + 1U;
                    detector_error = detection.error();
                    detector_error->source_id = lane.camera_id;
                    if (failures >= options.consecutive_failure_limit)
                        notify_detector_failure({.camera_id = lane.camera_id,
                                                 .active = true,
                                                 .consecutive_failures = failures,
                                                 .detector_failures = total_failures,
                                                 .failure_limit = options.consecutive_failure_limit,
                                                 .last_error = detector_error});
                }
                else
                {
                    const auto recovered_failures = lane.consecutive_detector_failures.exchange(0U);
                    if (recovered_failures >= options.consecutive_failure_limit)
                        notify_detector_failure(
                            {.camera_id = lane.camera_id,
                             .active = false,
                             .consecutive_failures = 0U,
                             .detector_failures = lane.detector_failures.load(),
                             .failure_limit = options.consecutive_failure_limit});
                    completed_detection = std::move(detection).value();
                    if (completed_detection->triggered)
                        ++diagnostic_candidates;
                }
            }

            const auto diagnostic_now = std::chrono::steady_clock::now();
            if (diagnostic_now - diagnostic_window_started >= 5s)
            {
                if (options.diagnostics.enabled && options.diagnostics.enabled() &&
                    options.diagnostics.record)
                    options.diagnostics.record(
                        "operation=algorithm.detect-summary cameraId=" + lane.camera_id +
                        " windowMs=" +
                        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           diagnostic_now - diagnostic_window_started)
                                           .count()) +
                        " calls=" + std::to_string(diagnostic_calls) +
                        " failures=" + std::to_string(diagnostic_failures) +
                        " candidates=" + std::to_string(diagnostic_candidates) + " averageUs=" +
                        std::to_string(diagnostic_calls == 0U
                                           ? 0
                                           : diagnostic_total_us /
                                                 static_cast<std::int64_t>(diagnostic_calls)) +
                        " maximumUs=" + std::to_string(diagnostic_maximum_us));
                diagnostic_window_started = diagnostic_now;
                diagnostic_calls = 0U;
                diagnostic_failures = 0U;
                diagnostic_candidates = 0U;
                diagnostic_total_us = 0;
                diagnostic_maximum_us = 0;
            }

            auto delivery_errors = enqueue_result(
                state, lane, {.frame = frame, .detection = std::move(completed_detection)});
            const auto completed_at = now();
            if (completed_at >= frame.received_monotonic_time())
                record_latency(lane.last_end_to_end_us, lane.total_end_to_end_us,
                               lane.maximum_end_to_end_us,
                               std::chrono::duration_cast<std::chrono::microseconds>(
                                   completed_at - frame.received_monotonic_time()));
            {
                std::scoped_lock lock{lane.mutex};
                lane.in_flight_time.reset();
                ++lane.processed_frames;
                ++lane.window_processed;
            }
            notify_result_progress(state);
            if (detector_error)
                report(*detector_error);
            for (const auto& error : delivery_errors)
                report(error);
        }
    }

    void run_events(EventPipelineState& state)
    {
        if (!wait_for_start_gate(state))
            return;
        const auto registration =
            options.register_thread ? options.register_thread("event-processing") : nullptr;
        if (options.result_consumer_start_gate)
            options.result_consumer_start_gate();
        while (true)
        {
            std::uint64_t watermark_generation{};
            {
                std::scoped_lock lock{state.result_mutex};
                watermark_generation = state.result_generation;
            }
            const auto watermark = safe_watermark(state);
            std::optional<AlgorithmResultEnvelope> envelope;
            {
                std::unique_lock lock{state.result_mutex};
                if (state.result_generation != watermark_generation)
                    continue;
                std::ranges::sort(state.results, envelope_less);
                if (!state.results.empty() &&
                    (!watermark ||
                     state.results.front().frame.received_monotonic_time() < *watermark))
                {
                    envelope.emplace(std::move(state.results.front()));
                    state.results.pop_front();
                }
                else if (state.event_stop_requested && state.results.empty())
                    break;
                else
                {
                    state.result_condition.wait(
                        lock, [&] { return state.result_generation != watermark_generation; });
                }
            }
            if (envelope)
                process_result(state, std::move(*envelope));
        }

        std::vector<event::FrozenEventWindow> frozen_windows;
        {
            if (state.candidates)
            {
                const auto stopped =
                    state.candidates->stop(state.last_monotonic_time, state.last_wall_clock_time);
                for (const auto& candidate_event : stopped)
                {
                    const auto canonical = state.source_to_canonical.find(candidate_event.event_id);
                    if (canonical == state.source_to_canonical.end())
                        continue;
                    state.source_decisions[candidate_event.event_id] = "Timeout";
                    auto current = options.database->get_event(canonical->second);
                    if (current)
                        transition_lifecycle(
                            canonical->second, aggregate_decision(state, canonical->second),
                            current.value().persistence_state, current.value().trigger_count);
                }
            }
            if (state.windows)
                frozen_windows = state.windows->stop(state.last_monotonic_time);
        }
        for (auto& frozen : frozen_windows)
            freeze(state, std::move(frozen));
    }

    [[nodiscard]] Result<void> allow_thread_start(const std::string_view name) const
    {
        return options.thread_start_gate ? options.thread_start_gate(name)
                                         : Result<void>::success();
    }

    void cancel_prepared_threads(EventPipelineState& state) noexcept
    {
        {
            std::scoped_lock lock{state.start_mutex};
            state.start_cancelled = true;
        }
        state.start_condition.notify_all();
        for (auto& lane : state.lanes)
        {
            {
                std::scoped_lock lock{lane->mutex};
                lane->stop_requested = true;
            }
            lane->condition.notify_all();
        }
        {
            std::scoped_lock lock{state.result_mutex};
            state.event_stop_requested = true;
            ++state.result_generation;
        }
        state.result_condition.notify_all();
        for (auto& lane : state.lanes)
            if (lane->worker.joinable())
                lane->worker.join();
        if (state.event_worker.joinable())
            state.event_worker.join();
    }

    [[nodiscard]] Result<void> prepare_threads(EventPipelineState& state)
    {
        try
        {
            auto allowed = allow_thread_start("event-processing");
            if (!allowed)
                return allowed;
            state.event_worker = std::jthread{[this, &state] { run_events(state); }};
            for (auto& lane : state.lanes)
            {
                const auto name = algorithm_worker_thread_name(lane->camera_id);
                allowed = allow_thread_start(name);
                if (!allowed)
                {
                    cancel_prepared_threads(state);
                    return allowed;
                }
                auto* lane_pointer = lane.get();
                lane->worker =
                    std::jthread{[this, &state, lane_pointer] { run_lane(state, *lane_pointer); }};
            }
        }
        catch (const std::exception&)
        {
            cancel_prepared_threads(state);
            return Result<void>::failure(runtime_error("SYS_INTERNAL_ERROR", Severity::critical,
                                                       "无法创建算法线程组",
                                                       "event.runtime.prepareThreads"));
        }
        return Result<void>::success();
    }

    static void release_start_gate(EventPipelineState& state)
    {
        {
            std::scoped_lock lock{state.start_mutex};
            state.start_gate_open = true;
        }
        state.start_condition.notify_all();
    }

    void stop_and_drain(EventPipelineState& state) noexcept
    {
        for (auto& lane : state.lanes)
        {
            {
                std::scoped_lock lock{lane->mutex};
                lane->stop_requested = true;
            }
            lane->condition.notify_all();
        }
        for (auto& lane : state.lanes)
            if (lane->worker.joinable())
                lane->worker.join();
        {
            std::scoped_lock lock{state.result_mutex};
            state.event_stop_requested = true;
            ++state.result_generation;
        }
        state.result_condition.notify_all();
        if (state.event_worker.joinable())
            state.event_worker.join();
        for (auto& lane : state.lanes)
            lane->ring->close();
    }
};

Result<std::shared_ptr<EventRuntime>> EventRuntime::create(EventRuntimeOptions options)
{
    if (!options.database || options.frame_queue_capacity == 0U ||
        options.frame_queue_capacity > 256U || options.result_queue_capacity == 0U ||
        options.result_queue_capacity > algorithm_result_queue_default_capacity ||
        options.persistence_capacity == 0U || options.persistence_capacity > 64U ||
        options.consecutive_failure_limit == 0U || options.consecutive_failure_limit > 1000U ||
        options.consecutive_backlog_limit == 0U || options.consecutive_backlog_limit > 1000U ||
        options.backlog_window.count() <= 0 || options.backlog_window > 60s ||
        options.backlog_degrade_window_limit == 0U || options.backlog_degrade_window_limit > 60U ||
        options.backlog_recovery_window_limit == 0U || options.backlog_recovery_window_limit > 60U)
        return Result<std::shared_ptr<EventRuntime>>::failure(runtime_error(
            "SYS_CONFIG_INVALID", Severity::error, "事件运行时配置无效", "event.runtime.create"));
    auto pipeline =
        build_pipeline(options.configuration, options.frame_queue_capacity,
                       options.result_queue_capacity, options.detector_registry_configurer);
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
        {.event_capacity = impl->options.persistence_capacity,
         .register_thread = impl->options.register_thread,
         .diagnostics = impl->options.diagnostics,
         .writing_observer = [raw](const std::string_view event_id) {
             auto current = raw->options.database->get_event(event_id);
             if (current)
                 raw->transition_lifecycle(event_id, current.value().decision_state, "Writing",
                                           current.value().trigger_count,
                                           current.value().confirmed_time_utc_ms);
         }});
    if (!persistence_runtime)
        return Result<std::shared_ptr<EventRuntime>>::failure(
            std::move(persistence_runtime).error());
    impl->persistence = std::move(persistence_runtime).value();
    auto jpeg = event::KeyFrameJpegRuntime::create(
        event::make_opencv_key_frame_jpeg_encoder(),
        [raw](event::KeyFrameEncodingResult result) { raw->jpeg_completed(std::move(result)); },
        {.job_capacity = event::key_frame_default_job_capacity,
         .register_thread = impl->options.register_thread,
         .diagnostics = impl->options.diagnostics});
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
    std::scoped_lock transaction_lock{impl_->reconfigure_mutex};
    std::unique_lock lock{impl_->mutex};
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
    auto prepared = impl_->prepare_threads(*impl_->pipeline);
    if (!prepared)
    {
        impl_->jpeg->request_stop();
        impl_->persistence->request_stop();
        lock.unlock();
        static_cast<void>(impl_->jpeg->join(std::chrono::steady_clock::now() + 2s));
        static_cast<void>(impl_->persistence->join(std::chrono::steady_clock::now() + 2s));
        return prepared;
    }
    impl_->accepting = true;
    impl_->started = true;
    impl_->release_start_gate(*impl_->pipeline);
    return Result<void>::success();
}

Result<void> EventRuntime::submit_frame(camera::FrameView frame)
{
    bool backlog_started = false;
    bool backlog_recovered = false;
    std::string source_id = frame.camera_id();
    std::optional<Error> degraded_error;
    std::optional<Error> backlog_error;
    std::optional<AlgorithmBacklogStateChange> backlog_change;
    std::vector<Error> delivery_errors;
    const auto submitted_at = impl_->now();
    std::unique_lock lock{impl_->mutex};
    if (!impl_->accepting)
    {
        ++impl_->rejected_frames;
        return Result<void>::failure(runtime_error("SYS_SERVICE_STOPPING", Severity::warning,
                                                   "事件运行时未接收新帧", "event.runtime.submit",
                                                   true));
    }
    auto* lane = find_lane(*impl_->pipeline, source_id);
    if (lane == nullptr)
    {
        ++impl_->rejected_frames;
        auto error = runtime_error("CAMERA_NOT_FOUND", Severity::error,
                                   "算法队列拒绝了未配置相机帧", "algorithm.runtime.submit");
        error.module = "algorithm";
        error.source_id = source_id;
        return Result<void>::failure(std::move(error));
    }
    std::unique_lock lane_lock{lane->mutex};
    auto window_outcome = impl_->advance_backlog_windows(*lane, submitted_at);
    backlog_recovered = window_outcome.recovered;
    degraded_error = std::move(window_outcome.degraded_error);
    auto cached = lane->ring->push(frame);
    if (!cached)
    {
        ++impl_->rejected_frames;
        return Result<void>::failure(std::move(cached).error());
    }
    lane->latest_frame = frame;
    if (lane->manual_pending && !lane->manual_target_sequence &&
        frame.sequence_number() > lane->manual_after_sequence)
        lane->manual_target_sequence = frame.sequence_number();
    bool enqueue = true;
    std::optional<camera::FrameView> skipped_frame;
    ++lane->window_submitted;
    if (lane->frames.size() >= impl_->options.frame_queue_capacity)
    {
        const auto oldest =
            std::ranges::find_if(lane->frames, [&](const QueuedAlgorithmFrame& pending) {
                return !lane->manual_target_sequence ||
                       pending.frame.sequence_number() != *lane->manual_target_sequence;
            });
        if (oldest != lane->frames.end())
        {
            skipped_frame.emplace(std::move(oldest->frame));
            lane->frames.erase(oldest);
        }
        else
        {
            enqueue = false;
            skipped_frame = frame;
        }
        ++lane->skipped_frames;
        ++lane->window_skipped;
        lane->consecutive_backlog_events.fetch_add(1U);
        backlog_started = !lane->backlog_active;
        lane->backlog_active = true;
        if (lane->window_skipped >= impl_->options.consecutive_backlog_limit &&
            impl_->options.backlog_degrade_window_limit == 1U)
        {
            lane->consecutive_bad_backlog_windows = 1U;
            degraded_error = impl_->enter_degraded(*lane, "sustained-queue-backlog");
        }
    }
    lane->latest_submitted_sequence =
        std::max(lane->latest_submitted_sequence, frame.sequence_number());
    ++lane->submitted_frames;
    if (skipped_frame)
        delivery_errors = impl_->enqueue_result(
            *impl_->pipeline, *lane, {.frame = *skipped_frame, .detection = std::nullopt});
    if (enqueue)
    {
        lane->frames.push_back({.frame = std::move(frame), .enqueued_at = submitted_at});
        lane->frame_high_watermark = std::max(lane->frame_high_watermark, lane->frames.size());
        impl_->notify_result_progress(*impl_->pipeline);
        lane->condition.notify_one();
    }
    if (backlog_started)
    {
        auto error =
            runtime_error("ALGORITHM_QUEUE_BACKLOG", Severity::warning,
                          "算法队列积压，已跳过最旧待检测帧", "algorithm.runtime.submit", true);
        error.module = "algorithm";
        error.source_id = source_id;
        error.details.push_back({"queue", "algorithm.frames[" + source_id + "]"});
        error.details.push_back({"capacity", std::to_string(impl_->options.frame_queue_capacity)});
        error.details.push_back({"overflowAction", "drop-oldest"});
        backlog_error = std::move(error);
    }
    if (backlog_started || backlog_recovered)
        backlog_change =
            AlgorithmBacklogStateChange{.camera_id = source_id,
                                        .active = backlog_started,
                                        .queue_depth = lane->frames.size(),
                                        .queue_capacity = impl_->options.frame_queue_capacity,
                                        .skipped_frames = lane->skipped_frames.load()};
    lane_lock.unlock();
    lock.unlock();
    if (backlog_change)
        impl_->notify_backlog(*backlog_change);
    if (backlog_error)
        impl_->report(*backlog_error);
    if (degraded_error)
        impl_->report(*degraded_error);
    for (const auto& error : delivery_errors)
        impl_->report(error);
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
    std::scoped_lock lane_lock{lane->mutex};
    if (lane->manual_pending)
        return Result<bool>::success(false);
    lane->manual_after_sequence = lane->latest_submitted_sequence;
    lane->manual_target_sequence.reset();
    lane->manual_pending = true;
    return Result<bool>::success(true);
}

Result<void> EventRuntime::update_external_confirmation(const std::string_view camera_id,
                                                        const bool active,
                                                        const camera::MonotonicTime monotonic_time,
                                                        const camera::WallClockTime wall_clock_time)
{
    std::scoped_lock transaction_lock{impl_->reconfigure_mutex};
    EventPipelineState* state{};
    {
        std::scoped_lock lock{impl_->mutex};
        if (!impl_->accepting || !impl_->pipeline->candidates)
            return Result<void>::failure(runtime_error("SYS_SERVICE_STOPPING", Severity::warning,
                                                       "事件运行时未接收外部确认信号",
                                                       "event.runtime.externalConfirmation", true));
        state = impl_->pipeline.get();
    }
    auto updated = state->candidates->update_external_signal(camera_id, active, monotonic_time,
                                                             wall_clock_time);
    if (!updated)
        return Result<void>::failure(std::move(updated).error());
    if (updated.value().event)
    {
        auto* lane = find_lane(*state, camera_id);
        if (lane != nullptr)
        {
            const std::scoped_lock lifecycle_lock{state->lifecycle_mutex};
            impl_->apply_candidate_decision(*state, *lane, *updated.value().event);
        }
    }
    return Result<void>::success();
}

Result<void> EventRuntime::reconfigure(const config::EdgeConfig& configuration)
{
    std::scoped_lock transaction_lock{impl_->reconfigure_mutex};
    bool restart_workers = false;
    {
        std::scoped_lock lock{impl_->mutex};
        if (impl_->started && !impl_->accepting)
            return Result<void>::failure(runtime_error("SYS_SERVICE_STOPPING", Severity::warning,
                                                       "事件运行时正在停止，拒绝重配置",
                                                       "event.runtime.reconfigure", true));
        restart_workers = impl_->started;
    }
    auto candidate = build_pipeline(configuration, impl_->options.frame_queue_capacity,
                                    impl_->options.result_queue_capacity,
                                    impl_->options.detector_registry_configurer);
    if (!candidate)
        return Result<void>::failure(std::move(candidate).error());
    auto next = std::move(candidate).value();
    if (restart_workers)
    {
        auto prepared = impl_->prepare_threads(*next);
        if (!prepared)
            return prepared;
        {
            std::scoped_lock lock{impl_->mutex};
            if (!impl_->started || !impl_->accepting)
            {
                impl_->cancel_prepared_threads(*next);
                return Result<void>::failure(runtime_error(
                    "SYS_SERVICE_STOPPING", Severity::warning,
                    "事件运行时在候选线程组准备期间开始停止", "event.runtime.reconfigure", true));
            }
            impl_->accepting = false;
        }
        impl_->stop_and_drain(*impl_->pipeline);
    }

    std::unique_ptr<EventPipelineState> previous;
    std::vector<AlgorithmBacklogStateChange> removed_backlogs;
    std::vector<AlgorithmDetectorFailureStateChange> recovered_detector_failures;
    const auto backlog_window_start = impl_->now();
    {
        std::scoped_lock lock{impl_->mutex};
        for (const auto& old_lane : impl_->pipeline->lanes)
        {
            std::scoped_lock old_lane_lock{old_lane->mutex};
            if (old_lane->consecutive_detector_failures.load() >=
                impl_->options.consecutive_failure_limit)
                recovered_detector_failures.push_back(
                    {.camera_id = old_lane->camera_id,
                     .active = false,
                     .consecutive_failures = 0U,
                     .detector_failures = old_lane->detector_failures.load(),
                     .failure_limit = impl_->options.consecutive_failure_limit});
            if (old_lane->backlog_active && find_lane(*next, old_lane->camera_id) == nullptr)
                removed_backlogs.push_back({.camera_id = old_lane->camera_id,
                                            .active = false,
                                            .queue_depth = 0U,
                                            .queue_capacity = impl_->options.frame_queue_capacity,
                                            .skipped_frames = old_lane->skipped_frames.load()});
        }
        for (auto& lane : next->lanes)
        {
            const auto* old_lane = find_lane(*impl_->pipeline, lane->camera_id);
            if (old_lane != nullptr)
            {
                std::scoped_lock old_lane_lock{old_lane->mutex};
                lane->latest_frame = old_lane->latest_frame;
                lane->latest_submitted_sequence = old_lane->latest_submitted_sequence;
                lane->frame_high_watermark = old_lane->frame_high_watermark;
                lane->submitted_frames.store(old_lane->submitted_frames.load());
                lane->processed_frames.store(old_lane->processed_frames.load());
                lane->skipped_frames.store(old_lane->skipped_frames.load());
                lane->detector_failures.store(old_lane->detector_failures.load());
                lane->consecutive_backlog_events.store(old_lane->consecutive_backlog_events.load());
                lane->backlog_active = old_lane->backlog_active;
                lane->backlog_window_start = backlog_window_start;
                lane->detector_process_calls.store(old_lane->detector_process_calls.load());
                lane->last_algorithm_processing_us.store(
                    old_lane->last_algorithm_processing_us.load());
                lane->total_algorithm_processing_us.store(
                    old_lane->total_algorithm_processing_us.load());
                lane->maximum_algorithm_processing_us.store(
                    old_lane->maximum_algorithm_processing_us.load());
                lane->last_queue_wait_us.store(old_lane->last_queue_wait_us.load());
                lane->total_queue_wait_us.store(old_lane->total_queue_wait_us.load());
                lane->maximum_queue_wait_us.store(old_lane->maximum_queue_wait_us.load());
                lane->last_end_to_end_us.store(old_lane->last_end_to_end_us.load());
                lane->total_end_to_end_us.store(old_lane->total_end_to_end_us.load());
                lane->maximum_end_to_end_us.store(old_lane->maximum_end_to_end_us.load());
                lane->input_fps.store(old_lane->input_fps.load());
                lane->processed_fps.store(old_lane->processed_fps.load());
                lane->skipped_ratio.store(old_lane->skipped_ratio.load());
                lane->result_queue_rejected.store(old_lane->result_queue_rejected.load());
                lane->candidates_created.store(old_lane->candidates_created.load());
                lane->confirmed_events.store(old_lane->confirmed_events.load());
                lane->rejected_candidates.store(old_lane->rejected_candidates.load());
            }
        }
        previous = std::move(impl_->pipeline);
        impl_->pipeline = std::move(next);
        impl_->options.configuration = configuration;
        impl_->accepting = restart_workers;
        if (restart_workers)
            impl_->release_start_gate(*impl_->pipeline);
    }
    for (const auto& change : removed_backlogs)
        impl_->notify_backlog(change);
    for (const auto& change : recovered_detector_failures)
        impl_->notify_detector_failure(change);
    return Result<void>::success();
}

Result<AlgorithmRuntimeSnapshot> EventRuntime::algorithm_snapshot(
    const std::string_view camera_id) const
{
    std::scoped_lock lock{impl_->mutex};
    const auto* lane = find_lane(*impl_->pipeline, camera_id);
    if (lane == nullptr)
    {
        return Result<AlgorithmRuntimeSnapshot>::failure(
            algorithm_runtime_error("CAMERA_NOT_FOUND", Severity::error, "逻辑相机未启用算法运行时",
                                    "algorithm.runtime.snapshot", camera_id));
    }
    return Result<AlgorithmRuntimeSnapshot>::success(impl_->lane_snapshot(*impl_->pipeline, *lane));
}

std::vector<AlgorithmRuntimeSnapshot> EventRuntime::algorithm_snapshots() const
{
    std::vector<AlgorithmRuntimeSnapshot> result;
    std::scoped_lock lock{impl_->mutex};
    result.reserve(impl_->pipeline->lanes.size());
    for (const auto& lane : impl_->pipeline->lanes)
        result.push_back(impl_->lane_snapshot(*impl_->pipeline, *lane));
    return result;
}

Result<AlgorithmFrameTestResult> EventRuntime::test_current_frame(
    const std::string_view camera_id) const
{
    config::EdgeConfig configuration;
    std::optional<camera::FrameView> frame;
    std::function<Result<void>(algorithm::DetectorPluginRegistry&)> registry_configurer;
    {
        std::scoped_lock lock{impl_->mutex};
        const auto* lane = find_lane(*impl_->pipeline, camera_id);
        if (lane == nullptr)
        {
            return Result<AlgorithmFrameTestResult>::failure(algorithm_runtime_error(
                "CAMERA_NOT_FOUND", Severity::error, "逻辑相机未启用算法运行时",
                "algorithm.runtime.testCurrentFrame", camera_id));
        }
        std::scoped_lock lane_lock{lane->mutex};
        if (!lane->latest_frame)
        {
            return Result<AlgorithmFrameTestResult>::failure(algorithm_runtime_error(
                "ALGORITHM_NOT_READY", Severity::warning, "当前相机尚无可测试图像",
                "algorithm.runtime.testCurrentFrame", camera_id, true));
        }
        configuration = impl_->pipeline->configuration;
        frame = lane->latest_frame;
        registry_configurer = impl_->options.detector_registry_configurer;
    }

    algorithm::DetectorPluginRegistry registry;
    if (auto registered = algorithm::mock::register_mock_trigger_detector(registry); !registered)
        return Result<AlgorithmFrameTestResult>::failure(std::move(registered).error());
    if (auto registered = algorithm::classical::register_classical_vision_detector(registry);
        !registered)
        return Result<AlgorithmFrameTestResult>::failure(std::move(registered).error());
    if (registry_configurer)
    {
        auto configured = registry_configurer(registry);
        if (!configured)
            return Result<AlgorithmFrameTestResult>::failure(std::move(configured).error());
    }

    algorithm::DetectorHost detector{registry};
    if (auto loaded = detector.load(detector_config(configuration, camera_id)); !loaded)
        return Result<AlgorithmFrameTestResult>::failure(std::move(loaded).error());
    auto information = detector.info();
    if (!information)
        return Result<AlgorithmFrameTestResult>::failure(std::move(information).error());
    auto detection = detector.process(*frame);
    if (!detection)
        return Result<AlgorithmFrameTestResult>::failure(std::move(detection).error());
    auto encoder = event::make_opencv_key_frame_jpeg_encoder();
    auto preview = encoder->encode(*frame, {.jpeg_quality = 85U,
                                            .maximum_dimension = 4096U,
                                            .maximum_input_bytes = 64U * 1024U * 1024U,
                                            .maximum_jpeg_bytes = 8U * 1024U * 1024U});
    if (!preview)
        return Result<AlgorithmFrameTestResult>::failure(std::move(preview).error());
    return Result<AlgorithmFrameTestResult>::success(
        {.detector_info = std::move(information).value(),
         .detection = std::move(detection).value(),
         .source_width = frame->geometry().width,
         .source_height = frame->geometry().height,
         .preview_jpeg = std::move(preview).value()});
}

void EventRuntime::request_stop() noexcept
{
    std::scoped_lock transaction_lock{impl_->reconfigure_mutex};
    EventPipelineState* state{};
    {
        std::scoped_lock lock{impl_->mutex};
        impl_->accepting = false;
        state = impl_->pipeline.get();
    }
    if (state == nullptr)
        return;
    for (auto& lane : state->lanes)
    {
        {
            std::scoped_lock lock{lane->mutex};
            lane->stop_requested = true;
        }
        lane->condition.notify_all();
    }
}

Result<void> EventRuntime::join(const std::chrono::steady_clock::time_point deadline)
{
    request_stop();
    impl_->stop_and_drain(*impl_->pipeline);
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
    event::CandidateEventManagerSnapshot candidates;
    if (impl_->pipeline->candidates)
        candidates = impl_->pipeline->candidates->snapshot();
    std::size_t frame_depth{};
    std::size_t frame_capacity{};
    std::size_t frame_high_watermark{};
    std::uint64_t submitted{};
    std::uint64_t processed{};
    std::uint64_t skipped{};
    std::uint64_t detector_failures{};
    std::uint64_t consecutive_failures{};
    std::uint64_t consecutive_backlogs{};
    std::uint64_t process_calls{};
    std::int64_t last_processing{};
    std::int64_t total_processing{};
    std::int64_t maximum_processing{};
    std::int64_t last_queue_wait{};
    std::int64_t total_queue_wait{};
    std::int64_t maximum_queue_wait{};
    std::int64_t last_end_to_end{};
    std::int64_t total_end_to_end{};
    std::int64_t maximum_end_to_end{};
    double input_fps{};
    double processed_fps{};
    double skipped_fps{};
    std::uint64_t result_rejected{};
    std::size_t degraded_lanes{};
    std::size_t backlog_active_lanes{};
    for (const auto& lane : impl_->pipeline->lanes)
    {
        std::scoped_lock lane_lock{lane->mutex};
        frame_depth += lane->frames.size();
        frame_capacity += impl_->options.frame_queue_capacity;
        frame_high_watermark += lane->frame_high_watermark;
        submitted += lane->submitted_frames.load();
        processed += lane->processed_frames.load();
        skipped += lane->skipped_frames.load();
        detector_failures += lane->detector_failures.load();
        consecutive_failures =
            std::max(consecutive_failures, lane->consecutive_detector_failures.load());
        consecutive_backlogs =
            std::max(consecutive_backlogs, lane->consecutive_backlog_events.load());
        process_calls += lane->detector_process_calls.load();
        last_processing = std::max(last_processing, lane->last_algorithm_processing_us.load());
        total_processing += lane->total_algorithm_processing_us.load();
        maximum_processing =
            std::max(maximum_processing, lane->maximum_algorithm_processing_us.load());
        last_queue_wait = std::max(last_queue_wait, lane->last_queue_wait_us.load());
        total_queue_wait += lane->total_queue_wait_us.load();
        maximum_queue_wait = std::max(maximum_queue_wait, lane->maximum_queue_wait_us.load());
        last_end_to_end = std::max(last_end_to_end, lane->last_end_to_end_us.load());
        total_end_to_end += lane->total_end_to_end_us.load();
        maximum_end_to_end = std::max(maximum_end_to_end, lane->maximum_end_to_end_us.load());
        const auto lane_input_fps = lane->input_fps.load();
        input_fps += lane_input_fps;
        processed_fps += lane->processed_fps.load();
        skipped_fps += lane_input_fps * lane->skipped_ratio.load();
        result_rejected += lane->result_queue_rejected.load();
        if (lane->backlog_active)
            ++backlog_active_lanes;
        if (lane->degraded.load(std::memory_order_acquire))
            ++degraded_lanes;
    }
    std::size_t result_depth{};
    std::size_t result_high_watermark{};
    {
        std::scoped_lock result_lock{impl_->pipeline->result_mutex};
        result_depth = impl_->pipeline->results.size();
        result_high_watermark = impl_->pipeline->result_high_watermark;
    }
    AlgorithmRuntimeState state = AlgorithmRuntimeState::disabled;
    if (impl_->pipeline->configuration.algorithm.enabled)
    {
        if (degraded_lanes == 0U)
            state = AlgorithmRuntimeState::active;
        else if (degraded_lanes == impl_->pipeline->lanes.size())
            state = AlgorithmRuntimeState::manual_trigger_only;
        else
            state = AlgorithmRuntimeState::partially_degraded;
    }
    const auto persistence = impl_->persistence ? impl_->persistence->snapshot()
                                                : storage::EventPersistenceRuntimeSnapshot{};
    return {.started = impl_->started,
            .accepting = impl_->accepting,
            .frame_queue_depth = frame_depth,
            .frame_queue_capacity = frame_capacity,
            .frame_queue_high_watermark = frame_high_watermark,
            .result_queue_depth = result_depth,
            .result_queue_capacity = impl_->pipeline->result_capacity,
            .result_queue_high_watermark = result_high_watermark,
            .pending_events = impl_->pending.size(),
            .persistence_queue_depth = persistence.depth,
            .persistence_queue_capacity = persistence.capacity,
            .persistence_queue_high_watermark = persistence.high_watermark,
            .persistence_active_events = persistence.active_events,
            .persistence_last_write_bytes = persistence.last_write_bytes,
            .persistence_last_write_duration = persistence.last_write_duration,
            .persistence_last_write_mib_per_second = persistence.last_write_mib_per_second,
            .submitted_frames = submitted,
            .processed_frames = processed,
            .rejected_frames = impl_->rejected_frames.load(),
            .skipped_frames = skipped,
            .detector_failures = detector_failures,
            .consecutive_detector_failures = consecutive_failures,
            .consecutive_backlog_events = consecutive_backlogs,
            .backlog_active_lanes = backlog_active_lanes,
            .detector_process_calls = process_calls,
            .result_queue_rejected = result_rejected,
            .last_algorithm_processing_time = std::chrono::microseconds{last_processing},
            .average_algorithm_processing_time =
                std::chrono::microseconds{process_calls == 0U
                                              ? 0
                                              : total_processing /
                                                    static_cast<std::int64_t>(process_calls)},
            .maximum_algorithm_processing_time = std::chrono::microseconds{maximum_processing},
            .last_queue_wait_time = std::chrono::microseconds{last_queue_wait},
            .average_queue_wait_time =
                std::chrono::microseconds{
                    processed == 0U ? 0 : total_queue_wait / static_cast<std::int64_t>(processed)},
            .maximum_queue_wait_time = std::chrono::microseconds{maximum_queue_wait},
            .last_end_to_end_time = std::chrono::microseconds{last_end_to_end},
            .average_end_to_end_time =
                std::chrono::microseconds{
                    processed == 0U ? 0 : total_end_to_end / static_cast<std::int64_t>(processed)},
            .maximum_end_to_end_time = std::chrono::microseconds{maximum_end_to_end},
            .input_fps = input_fps,
            .processed_fps = processed_fps,
            .skipped_ratio = input_fps <= 0.0 ? 0.0 : skipped_fps / input_fps,
            .algorithm_state = state,
            .events_started = impl_->events_started.load(),
            .candidates_created = candidates.events_created,
            .confirmed_events = candidates.confirmed_events,
            .rejected_candidates = candidates.rejected_events,
            .events_frozen = impl_->events_frozen.load(),
            .events_committed = impl_->events_committed.load(),
            .event_failures = impl_->event_failures.load()};
}

} // namespace paperbreak::service
