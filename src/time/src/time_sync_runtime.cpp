#include "paperbreak/time/time_sync_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

namespace paperbreak::time
{
namespace
{
Error runtime_error(std::string code, Severity severity, std::string message, std::string operation,
                    const bool retryable = false)
{
    return make_error(std::move(code), severity, std::move(message), "time", std::move(operation),
                      retryable);
}

Error invalid_options(const std::string& reason)
{
    auto error = runtime_error("TIME_MODEL_INVALID", Severity::error, "时间同步运行时参数无效",
                               "time.runtime.create");
    error.details.push_back({"reason", reason});
    return error;
}

Error probe_unavailable(const std::string& reason)
{
    auto error = runtime_error("TIME_PROBE_UNAVAILABLE", Severity::warning, "时间探针不可用",
                               "time.probe.sample", true);
    error.details.push_back({"reason", reason});
    return error;
}

Error shutdown_timeout()
{
    return runtime_error("SYS_SHUTDOWN_TIMEOUT", Severity::critical,
                         "时间同步线程未在截止时间内停止", "time.runtime.join");
}

bool system_source(const ClockSource source) noexcept
{
    return source == ClockSource::ptp_hardware || source == ClockSource::ptp_software ||
           source == ClockSource::ntp;
}

bool active_state(const SyncState state) noexcept
{
    return state == SyncState::synced || state == SyncState::syncing ||
           state == SyncState::degraded;
}

bool nonnegative(const std::optional<std::int64_t> value) noexcept
{
    return !value || *value >= 0;
}

bool valid_system_sample(const SystemClockProbeSample& sample) noexcept
{
    return system_source(sample.clock_source) && active_state(sample.sync_state) &&
           sample.uncertainty_ns >= 0 && nonnegative(sample.maximum_observed_offset_ns);
}

bool valid_camera_sample(const CameraClockProbeSample& sample) noexcept
{
    return sample.camera_timestamp_frequency_hz > 0U && sample.uncertainty_ns >= 0 &&
           nonnegative(sample.maximum_observed_offset_ns);
}

std::int64_t saturating_add_nonnegative(const std::int64_t left, const std::int64_t right) noexcept
{
    if (left < 0 || right < 0 || left > std::numeric_limits<std::int64_t>::max() - right)
        return std::numeric_limits<std::int64_t>::max();
    return left + right;
}

std::int64_t absolute_delta_difference(const RuntimeClockReading previous,
                                       const RuntimeClockReading current) noexcept
{
    const auto monotonic_delta = static_cast<std::uint64_t>(current.monotonic_ns) -
                                 static_cast<std::uint64_t>(previous.monotonic_ns);
    const auto utc_delta =
        static_cast<std::uint64_t>(current.utc_ns) - static_cast<std::uint64_t>(previous.utc_ns);
    const auto magnitude =
        monotonic_delta >= utc_delta ? monotonic_delta - utc_delta : utc_delta - monotonic_delta;
    if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(magnitude);
}

std::string stable_probe_error(const Error& error)
{
    if (error.business_code == "TIME_PROBE_UNAVAILABLE" ||
        error.business_code == "TIME_PROBE_NOT_SUPPORTED" ||
        error.business_code == "TIME_MODEL_INVALID" ||
        error.business_code == "SYS_TIME_JUMP_DETECTED")
        return error.business_code;
    return "TIME_PROBE_UNAVAILABLE";
}
} // namespace

RuntimeClockReading StandardRuntimeClock::read() noexcept
{
    return {.monotonic_ns =
                monotonic_time_to_nanoseconds(std::chrono::steady_clock::now()).value_or(0),
            .utc_ns = utc_time_to_nanoseconds(std::chrono::system_clock::now()).value_or(0)};
}

struct TimeSyncRuntime::Impl final
{
    struct CameraSlot final
    {
        std::string camera_id;
        std::unique_ptr<ICameraClockProbe> probe;
        ImmutableClockModelStore model_store;
    };

    explicit Impl(std::unique_ptr<ISystemClockProbe> system,
                  std::vector<std::unique_ptr<ICameraClockProbe>> cameras,
                  std::unique_ptr<IRuntimeClock> clock, TimeSyncRuntimeOptions runtime_options)
        : system_probe(std::move(system)), runtime_clock(std::move(clock)), options(runtime_options)
    {
        camera_slots.reserve(cameras.size());
        for (auto& camera : cameras)
        {
            auto slot = std::make_unique<CameraSlot>();
            slot->camera_id = camera->camera_id();
            slot->probe = std::move(camera);
            camera_slots.push_back(std::move(slot));
        }
    }

    std::uint64_t next_revision() noexcept
    {
        if (model_revision != std::numeric_limits<std::uint64_t>::max())
            ++model_revision;
        last_model_revision.store(model_revision, std::memory_order_release);
        return model_revision;
    }

    std::shared_ptr<const ClockModelSnapshot> receive_model(const RuntimeClockReading reading,
                                                            std::optional<std::string> camera_id,
                                                            std::string error_code)
    {
        return std::make_shared<const ClockModelSnapshot>(
            ClockModelSnapshot{.model_revision = next_revision(),
                               .camera_id = std::move(camera_id),
                               .clock_source = ClockSource::receive_clock,
                               .sync_state = SyncState::degraded,
                               .anchor_monotonic_ns = reading.monotonic_ns,
                               .anchor_utc_ns = reading.utc_ns,
                               .anchor_camera_ticks = std::nullopt,
                               .camera_timestamp_frequency_hz = std::nullopt,
                               .offset_ns = std::nullopt,
                               .uncertainty_ns = options.receive_clock_uncertainty_ns,
                               .maximum_observed_offset_ns = std::nullopt,
                               .valid_from_monotonic_ns = reading.monotonic_ns,
                               .last_synchronized_utc_ns = std::nullopt,
                               .grandmaster_identity = std::nullopt,
                               .last_error_code = std::move(error_code)});
    }

    std::shared_ptr<const ClockModelSnapshot> system_model_from(
        const RuntimeClockReading reading, const std::optional<SystemClockProbeSample>& sample,
        const std::string& probe_error_code)
    {
        if (!sample || !valid_system_sample(*sample))
            return receive_model(reading, std::nullopt,
                                 probe_error_code.empty() ? "TIME_PROBE_UNAVAILABLE"
                                                          : probe_error_code);

        auto quality_state = sample->sync_state;
        auto uncertainty = sample->uncertainty_ns;
        auto last_error = sample->last_error_code;
        if (jump_detected)
        {
            quality_state = SyncState::degraded;
            uncertainty = std::max(uncertainty, last_jump_magnitude_ns);
            last_error = "SYS_TIME_JUMP_DETECTED";
        }
        auto model = std::make_shared<const ClockModelSnapshot>(
            ClockModelSnapshot{.model_revision = next_revision(),
                               .camera_id = std::nullopt,
                               .clock_source = sample->clock_source,
                               .sync_state = quality_state,
                               .anchor_monotonic_ns = sample->sample_monotonic_ns,
                               .anchor_utc_ns = sample->sample_utc_ns,
                               .anchor_camera_ticks = std::nullopt,
                               .camera_timestamp_frequency_hz = std::nullopt,
                               .offset_ns = sample->offset_ns,
                               .uncertainty_ns = uncertainty,
                               .maximum_observed_offset_ns = sample->maximum_observed_offset_ns,
                               .valid_from_monotonic_ns = sample->sample_monotonic_ns,
                               .last_synchronized_utc_ns = sample->last_synchronized_utc_ns,
                               .grandmaster_identity = sample->grandmaster_identity,
                               .last_error_code = std::move(last_error)});
        if (validate_clock_model_snapshot(*model))
            return model;
        return receive_model(reading, std::nullopt, "TIME_MODEL_INVALID");
    }

    std::shared_ptr<const ClockModelSnapshot> camera_model_from(
        const RuntimeClockReading reading, const CameraSlot& slot,
        const std::optional<CameraClockProbeSample>& camera_sample,
        const std::string& camera_error_code,
        const std::shared_ptr<const ClockModelSnapshot>& selected_system_model)
    {
        if (!camera_sample || !valid_camera_sample(*camera_sample))
            return receive_model(reading, slot.camera_id,
                                 camera_error_code.empty() ? "TIME_PROBE_UNAVAILABLE"
                                                           : camera_error_code);

        ClockSource source = ClockSource::offset_model;
        SyncState quality_state = SyncState::degraded;
        std::int64_t uncertainty = camera_sample->uncertainty_ns;
        std::optional<std::int64_t> last_synchronized = camera_sample->last_synchronized_utc_ns;
        std::optional<std::string> grandmaster = camera_sample->grandmaster_identity;
        std::optional<std::string> last_error = camera_sample->last_error_code;

        if (camera_sample->hardware_ptp_synchronized)
        {
            source = ClockSource::ptp_hardware;
            quality_state = SyncState::synced;
        }
        else if (selected_system_model && system_source(selected_system_model->clock_source))
        {
            source = selected_system_model->clock_source;
            quality_state = selected_system_model->sync_state;
            uncertainty = saturating_add_nonnegative(
                uncertainty, selected_system_model->uncertainty_ns.value_or(0));
            if (!last_synchronized)
                last_synchronized = selected_system_model->last_synchronized_utc_ns;
            if (!grandmaster)
                grandmaster = selected_system_model->grandmaster_identity;
            if (!last_error)
                last_error = selected_system_model->last_error_code;
        }
        else if (!last_error)
        {
            last_error = "TIME_SYNC_DEGRADED";
        }

        if (jump_detected)
        {
            quality_state = SyncState::degraded;
            uncertainty = std::max(uncertainty, last_jump_magnitude_ns);
            last_error = "SYS_TIME_JUMP_DETECTED";
        }

        auto model = std::make_shared<const ClockModelSnapshot>(ClockModelSnapshot{
            .model_revision = next_revision(),
            .camera_id = slot.camera_id,
            .clock_source = source,
            .sync_state = quality_state,
            .anchor_monotonic_ns = camera_sample->sample_monotonic_ns,
            .anchor_utc_ns = camera_sample->sample_utc_ns,
            .anchor_camera_ticks = camera_sample->camera_timestamp_ticks,
            .camera_timestamp_frequency_hz = camera_sample->camera_timestamp_frequency_hz,
            .offset_ns = camera_sample->offset_ns,
            .uncertainty_ns = uncertainty,
            .maximum_observed_offset_ns = camera_sample->maximum_observed_offset_ns,
            .valid_from_monotonic_ns = camera_sample->sample_monotonic_ns,
            .last_synchronized_utc_ns = std::move(last_synchronized),
            .grandmaster_identity = std::move(grandmaster),
            .last_error_code = std::move(last_error)});
        if (validate_clock_model_snapshot(*model))
            return model;
        return receive_model(reading, slot.camera_id, "TIME_MODEL_INVALID");
    }

    std::optional<SystemClockProbeSample> sample_system(const std::stop_token token,
                                                        std::string& error_code)
    {
        const auto deadline = std::chrono::steady_clock::now() + options.probe_timeout;
        try
        {
            auto result = system_probe->sample(token, deadline);
            if (std::chrono::steady_clock::now() > deadline)
            {
                error_code = "TIME_PROBE_UNAVAILABLE";
                return std::nullopt;
            }
            if (!result)
            {
                error_code = stable_probe_error(result.error());
                return std::nullopt;
            }
            if (!valid_system_sample(result.value()))
            {
                error_code = "TIME_MODEL_INVALID";
                return std::nullopt;
            }
            return std::move(result).value();
        }
        catch (...)
        {
            error_code = "TIME_PROBE_UNAVAILABLE";
            return std::nullopt;
        }
    }

    std::optional<CameraClockProbeSample> sample_camera(CameraSlot& slot,
                                                        const std::stop_token token,
                                                        std::string& error_code)
    {
        const auto deadline = std::chrono::steady_clock::now() + options.probe_timeout;
        try
        {
            auto result = slot.probe->sample(token, deadline);
            if (std::chrono::steady_clock::now() > deadline)
            {
                error_code = "TIME_PROBE_UNAVAILABLE";
                return std::nullopt;
            }
            if (!result)
            {
                error_code = stable_probe_error(result.error());
                return std::nullopt;
            }
            if (!valid_camera_sample(result.value()))
            {
                error_code = "TIME_MODEL_INVALID";
                return std::nullopt;
            }
            return std::move(result).value();
        }
        catch (...)
        {
            error_code = "TIME_PROBE_UNAVAILABLE";
            return std::nullopt;
        }
    }

    void publish(const std::shared_ptr<const ClockModelSnapshot>& system,
                 const std::vector<std::shared_ptr<const ClockModelSnapshot>>& cameras)
    {
        system_model_store.publish(system);
        published_models.fetch_add(1U, std::memory_order_relaxed);
        for (std::size_t index = 0; index < camera_slots.size(); ++index)
        {
            camera_slots[index]->model_store.publish(cameras[index]);
            published_models.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    void perform_sample_cycle(const std::stop_token token)
    {
        const RuntimeClockReading reading = runtime_clock->read();
        if (previous_clock_reading)
        {
            const auto difference = absolute_delta_difference(*previous_clock_reading, reading);
            if (difference > options.system_time_jump_threshold_ns)
            {
                jump_detected = true;
                last_jump_magnitude_ns = difference;
            }
        }
        previous_clock_reading = reading;

        std::string system_error_code;
        const auto system_sample = sample_system(token, system_error_code);
        const auto selected_system = system_model_from(reading, system_sample, system_error_code);

        std::vector<std::shared_ptr<const ClockModelSnapshot>> camera_models;
        camera_models.reserve(camera_slots.size());
        for (auto& slot : camera_slots)
        {
            std::string camera_error_code;
            const auto camera_sample = sample_camera(*slot, token, camera_error_code);
            camera_models.push_back(camera_model_from(reading, *slot, camera_sample,
                                                      camera_error_code, selected_system));
        }
        publish(selected_system, camera_models);
        if (options.model_observer)
        {
            try
            {
                options.model_observer(reading.monotonic_ns, selected_system, camera_models);
            }
            catch (...)
            {
            }
        }
        sample_cycles.fetch_add(1U, std::memory_order_relaxed);
    }

    void publish_stopped_snapshot()
    {
        const auto reading = runtime_clock->read();
        const auto stopped_system = receive_model(reading, std::nullopt, "SYS_SERVICE_STOPPING");
        std::vector<std::shared_ptr<const ClockModelSnapshot>> camera_models;
        camera_models.reserve(camera_slots.size());
        for (const auto& slot : camera_slots)
            camera_models.push_back(
                receive_model(reading, slot->camera_id, "SYS_SERVICE_STOPPING"));
        publish(stopped_system, camera_models);
    }

    void run(const std::stop_token token) noexcept
    {
        try
        {
            auto next_periodic_sample = std::chrono::steady_clock::now();
            while (!token.stop_requested())
            {
                perform_sample_cycle(token);
                {
                    std::lock_guard lock{mutex};
                    if (!first_sample_complete)
                    {
                        first_sample_complete = true;
                        condition.notify_all();
                    }
                }

                next_periodic_sample = std::chrono::steady_clock::now() + options.sample_period;
                std::unique_lock lock{mutex};
                condition.wait_until(lock, next_periodic_sample, [this, &token] {
                    return token.stop_requested() || pending_refresh_requests > 0U;
                });
                if (token.stop_requested())
                    break;
                if (pending_refresh_requests > 0U)
                {
                    --pending_refresh_requests;
                    processed_refresh_requests.fetch_add(1U, std::memory_order_relaxed);
                }
            }
            publish_stopped_snapshot();
        }
        catch (...)
        {
            state.store(TimeSyncRuntimeState::failed, std::memory_order_release);
        }

        {
            std::lock_guard lock{mutex};
            completed = true;
            accepting_controls = false;
        }
        if (state.load(std::memory_order_acquire) != TimeSyncRuntimeState::failed)
            state.store(TimeSyncRuntimeState::stopped, std::memory_order_release);
        condition.notify_all();
    }

    std::unique_ptr<ISystemClockProbe> system_probe;
    std::vector<std::unique_ptr<CameraSlot>> camera_slots;
    std::unique_ptr<IRuntimeClock> runtime_clock;
    TimeSyncRuntimeOptions options;
    ImmutableClockModelStore system_model_store;
    std::jthread worker;
    std::atomic<TimeSyncRuntimeState> state{TimeSyncRuntimeState::created};
    mutable std::mutex mutex;
    std::condition_variable condition;
    bool accepting_controls{};
    bool first_sample_complete{};
    bool completed{};
    std::size_t pending_refresh_requests{};
    std::size_t control_high_watermark{};
    std::uint64_t model_revision{};
    std::optional<RuntimeClockReading> previous_clock_reading;
    bool jump_detected{};
    std::int64_t last_jump_magnitude_ns{};
    std::atomic<std::uint64_t> sample_cycles{};
    std::atomic<std::uint64_t> published_models{};
    std::atomic<std::uint64_t> accepted_refresh_requests{};
    std::atomic<std::uint64_t> processed_refresh_requests{};
    std::atomic<std::uint64_t> rejected_refresh_requests{};
    std::atomic<std::uint64_t> last_model_revision{};
};

Result<std::unique_ptr<TimeSyncRuntime>> TimeSyncRuntime::create(
    std::unique_ptr<ISystemClockProbe> system_probe,
    std::vector<std::unique_ptr<ICameraClockProbe>> camera_probes,
    std::unique_ptr<IRuntimeClock> runtime_clock, const TimeSyncRuntimeOptions options)
{
    if (!system_probe)
        return Result<std::unique_ptr<TimeSyncRuntime>>::failure(
            invalid_options("missing-system-probe"));
    if (!runtime_clock)
        return Result<std::unique_ptr<TimeSyncRuntime>>::failure(
            invalid_options("missing-runtime-clock"));
    if (camera_probes.size() > time_sync_camera_capacity)
        return Result<std::unique_ptr<TimeSyncRuntime>>::failure(
            invalid_options("camera-capacity-exceeded"));
    if (options.sample_period <= std::chrono::milliseconds::zero() ||
        options.probe_timeout <= std::chrono::milliseconds::zero() ||
        options.first_sample_timeout <= std::chrono::milliseconds::zero() ||
        options.receive_clock_uncertainty_ns < 0 || options.system_time_jump_threshold_ns <= 0)
        return Result<std::unique_ptr<TimeSyncRuntime>>::failure(
            invalid_options("invalid-time-or-uncertainty-option"));

    std::unordered_set<std::string> camera_ids;
    for (const auto& camera : camera_probes)
    {
        if (!camera || camera->camera_id().empty())
            return Result<std::unique_ptr<TimeSyncRuntime>>::failure(
                invalid_options("missing-camera-probe-or-id"));
        if (!camera_ids.emplace(camera->camera_id()).second)
            return Result<std::unique_ptr<TimeSyncRuntime>>::failure(
                invalid_options("duplicate-camera-id"));
    }

    auto impl = std::make_unique<Impl>(std::move(system_probe), std::move(camera_probes),
                                       std::move(runtime_clock), options);
    return Result<std::unique_ptr<TimeSyncRuntime>>::success(
        std::make_unique<TimeSyncRuntime>(std::move(impl)));
}

TimeSyncRuntime::TimeSyncRuntime(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

TimeSyncRuntime::~TimeSyncRuntime()
{
    request_stop();
    static_cast<void>(join(std::chrono::steady_clock::time_point::max()));
}

Result<void> TimeSyncRuntime::start()
{
    auto expected = TimeSyncRuntimeState::created;
    if (!impl_->state.compare_exchange_strong(expected, TimeSyncRuntimeState::running,
                                              std::memory_order_acq_rel))
        return Result<void>::failure(runtime_error("TIME_MODEL_INVALID", Severity::error,
                                                   "时间同步运行时不能从当前状态启动",
                                                   "time.runtime.start"));

    {
        std::lock_guard lock{impl_->mutex};
        impl_->accepting_controls = true;
    }
    impl_->worker =
        std::jthread([impl = impl_.get()](const std::stop_token token) { impl->run(token); });

    const auto deadline = std::chrono::steady_clock::now() + impl_->options.first_sample_timeout;
    std::unique_lock lock{impl_->mutex};
    if (!impl_->condition.wait_until(
            lock, deadline, [this] { return impl_->first_sample_complete || impl_->completed; }))
    {
        lock.unlock();
        request_stop();
        return Result<void>::failure(probe_unavailable("first-sample-timeout"));
    }
    if (impl_->state.load(std::memory_order_acquire) == TimeSyncRuntimeState::failed)
        return Result<void>::failure(probe_unavailable("worker-failed"));
    return Result<void>::success();
}

Result<void> TimeSyncRuntime::request_refresh()
{
    std::lock_guard lock{impl_->mutex};
    if (!impl_->accepting_controls ||
        impl_->state.load(std::memory_order_acquire) != TimeSyncRuntimeState::running)
        return Result<void>::failure(runtime_error("SYS_SERVICE_STOPPING", Severity::warning,
                                                   "时间同步运行时正在停止", "time.runtime.refresh",
                                                   true));
    if (impl_->pending_refresh_requests >= time_sync_control_capacity)
    {
        impl_->rejected_refresh_requests.fetch_add(1U, std::memory_order_relaxed);
        return Result<void>::failure(runtime_error(
            "SYS_BUSY", Severity::warning, "时间同步控制通道已满", "time.runtime.refresh", true));
    }
    ++impl_->pending_refresh_requests;
    impl_->control_high_watermark =
        std::max(impl_->control_high_watermark, impl_->pending_refresh_requests);
    impl_->accepted_refresh_requests.fetch_add(1U, std::memory_order_relaxed);
    impl_->condition.notify_one();
    return Result<void>::success();
}

void TimeSyncRuntime::request_stop() noexcept
{
    auto current = impl_->state.load(std::memory_order_acquire);
    while (current == TimeSyncRuntimeState::running)
    {
        if (impl_->state.compare_exchange_weak(current, TimeSyncRuntimeState::stop_requested,
                                               std::memory_order_acq_rel))
            break;
    }
    {
        std::lock_guard lock{impl_->mutex};
        impl_->accepting_controls = false;
        impl_->pending_refresh_requests = 0U;
    }
    if (impl_->worker.joinable())
        impl_->worker.request_stop();
    impl_->condition.notify_all();
}

Result<void> TimeSyncRuntime::join(const std::chrono::steady_clock::time_point deadline)
{
    if (impl_->state.load(std::memory_order_acquire) == TimeSyncRuntimeState::created)
    {
        impl_->state.store(TimeSyncRuntimeState::stopped, std::memory_order_release);
        return Result<void>::success();
    }
    {
        std::unique_lock lock{impl_->mutex};
        const bool finished =
            deadline == std::chrono::steady_clock::time_point::max()
                ? (impl_->condition.wait(lock, [this] { return impl_->completed; }), true)
                : impl_->condition.wait_until(lock, deadline, [this] { return impl_->completed; });
        if (!finished)
            return Result<void>::failure(shutdown_timeout());
    }
    if (impl_->worker.joinable())
        impl_->worker.join();
    if (impl_->state.load(std::memory_order_acquire) == TimeSyncRuntimeState::failed)
        return Result<void>::failure(probe_unavailable("worker-failed"));
    return Result<void>::success();
}

TimeSyncRuntimeState TimeSyncRuntime::state() const noexcept
{
    return impl_->state.load(std::memory_order_acquire);
}

TimeSyncRuntimeMetrics TimeSyncRuntime::metrics() const noexcept
{
    TimeSyncRuntimeMetrics result{
        .sample_cycles = impl_->sample_cycles.load(std::memory_order_relaxed),
        .published_models = impl_->published_models.load(std::memory_order_relaxed),
        .accepted_refresh_requests =
            impl_->accepted_refresh_requests.load(std::memory_order_relaxed),
        .processed_refresh_requests =
            impl_->processed_refresh_requests.load(std::memory_order_relaxed),
        .rejected_refresh_requests =
            impl_->rejected_refresh_requests.load(std::memory_order_relaxed),
        .last_model_revision = impl_->last_model_revision.load(std::memory_order_acquire)};
    std::lock_guard lock{impl_->mutex};
    result.control_depth = impl_->pending_refresh_requests;
    result.control_high_watermark = impl_->control_high_watermark;
    return result;
}

std::shared_ptr<const ClockModelSnapshot> TimeSyncRuntime::system_model() const noexcept
{
    return impl_->system_model_store.load();
}

std::shared_ptr<const ClockModelSnapshot> TimeSyncRuntime::camera_model(
    const std::string_view camera_id) const noexcept
{
    for (const auto& slot : impl_->camera_slots)
        if (slot->camera_id == camera_id)
            return slot->model_store.load();
    return {};
}

ClockSyncSnapshot TimeSyncRuntime::system_status() const noexcept
{
    return build_clock_sync_snapshot(system_model(), impl_->runtime_clock->read().monotonic_ns);
}

ClockSyncSnapshot TimeSyncRuntime::camera_status(const std::string_view camera_id) const noexcept
{
    return build_clock_sync_snapshot(camera_model(camera_id),
                                     impl_->runtime_clock->read().monotonic_ns);
}

Result<ClockTimeMapping> TimeSyncRuntime::monotonic_to_utc(
    const std::int64_t monotonic_ns) const noexcept
{
    return map_monotonic_to_utc(monotonic_ns, system_model());
}

Result<ClockTimeMapping> TimeSyncRuntime::utc_to_monotonic(const std::int64_t utc_ns) const noexcept
{
    return map_utc_to_monotonic(utc_ns, system_model());
}

} // namespace paperbreak::time
