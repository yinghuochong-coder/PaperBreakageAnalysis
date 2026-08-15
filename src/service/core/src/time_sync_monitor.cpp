#include "paperbreak/service/time_sync_monitor.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace paperbreak::service
{
namespace
{
constexpr std::size_t maximum_time_alarm_entities = 5U;
constexpr std::size_t maximum_time_camera_entities = 4U;

Error invalid_options()
{
    return make_error("SYS_CONFIG_INVALID", Severity::error, "时间同步报警阈值或持续时间无效",
                      "service", "service.timeSyncAlarm.reconfigure");
}

std::int64_t duration_ns(const std::chrono::milliseconds value) noexcept
{
    constexpr auto maximum_ms = (std::numeric_limits<std::int64_t>::max)() / 1'000'000LL;
    return value.count() > maximum_ms ? (std::numeric_limits<std::int64_t>::max)()
                                      : value.count() * 1'000'000LL;
}

std::int64_t magnitude(const std::optional<std::int64_t> value) noexcept
{
    if (!value)
        return 0;
    if (*value == (std::numeric_limits<std::int64_t>::min)())
        return (std::numeric_limits<std::int64_t>::max)();
    return *value < 0 ? -*value : *value;
}

std::vector<ErrorDetail> threshold_details(const time::ClockModelSnapshot& model,
                                           const std::int64_t observed,
                                           const std::int64_t threshold)
{
    return {{"clockSource", std::string{time::clock_source_name(model.clock_source)}},
            {"syncState", std::string{time::sync_state_name(model.sync_state)}},
            {"observedQualityNs", std::to_string(observed)},
            {"thresholdNs", std::to_string(threshold)},
            {"modelRevision", std::to_string(model.model_revision)}};
}
} // namespace

TimeSyncAlarmMonitor::TimeSyncAlarmMonitor(std::shared_ptr<monitoring::AlarmRegistry> alarms,
                                           TimeSyncAlarmOptions options)
    : alarms_(std::move(alarms)), options_(options)
{
}

Result<void> TimeSyncAlarmMonitor::reconfigure(TimeSyncAlarmOptions options)
{
    if (options.warning_threshold_ns <= 0 ||
        options.alarm_threshold_ns <= options.warning_threshold_ns ||
        options.warning_duration < std::chrono::milliseconds::zero() ||
        options.alarm_duration < std::chrono::milliseconds::zero())
        return Result<void>::failure(invalid_options());
    std::scoped_lock lock{mutex_};
    for (auto& [source_id, state] : states_)
        clear_thresholds(source_id, state);
    states_.clear();
    options_ = options;
    return Result<void>::success();
}

void TimeSyncAlarmMonitor::observe(
    const std::int64_t monotonic_ns,
    const std::shared_ptr<const time::ClockModelSnapshot>& system_model,
    const std::vector<std::shared_ptr<const time::ClockModelSnapshot>>& camera_models) noexcept
{
    std::scoped_lock lock{mutex_};
    observe_one("time.system", monotonic_ns, system_model);
    for (std::size_t index = 0U;
         index < camera_models.size() && index < maximum_time_camera_entities; ++index)
    {
        const auto& model = camera_models[index];
        const std::string source_id =
            model && model->camera_id ? *model->camera_id : "time.camera." + std::to_string(index);
        observe_one(source_id, monotonic_ns, model);
    }
}

void TimeSyncAlarmMonitor::observe_one(
    std::string source_id, const std::int64_t monotonic_ns,
    const std::shared_ptr<const time::ClockModelSnapshot>& model) noexcept
{
    auto found = states_.find(source_id);
    if (found == states_.end())
    {
        if (states_.size() >= maximum_time_alarm_entities)
            return;
        found = states_.emplace(source_id, EntityState{}).first;
    }
    EntityState& state = found->second;
    if (model && state.source && *state.source != model->clock_source && alarms_)
    {
        static_cast<void>(alarms_->raise_alarm(
            {.code = "TIME_SYNC_SOURCE_CHANGED",
             .severity = Severity::warning,
             .source = source_id,
             .message = "时间同步来源发生变化",
             .details = {
                 {"previousSource", std::string{time::clock_source_name(*state.source)}},
                 {"currentSource", std::string{time::clock_source_name(model->clock_source)}},
                 {"modelRevision", std::to_string(model->model_revision)}}}));
        static_cast<void>(alarms_->clear("TIME_SYNC_SOURCE_CHANGED", source_id));
    }
    if (model)
        state.source = model->clock_source;

    if (!model || !model->uncertainty_ns || *model->uncertainty_ns < 0)
    {
        clear_thresholds(source_id, state);
        state.warning_since_ns.reset();
        state.alarm_since_ns.reset();
        return;
    }
    const auto observed = std::max(magnitude(model->offset_ns), *model->uncertainty_ns);
    const bool warning_exceeded = observed >= options_.warning_threshold_ns;
    const bool alarm_exceeded = observed >= options_.alarm_threshold_ns;

    if (!warning_exceeded)
    {
        clear_thresholds(source_id, state);
        state.warning_since_ns.reset();
        state.alarm_since_ns.reset();
        return;
    }
    if (!state.warning_since_ns)
        state.warning_since_ns = monotonic_ns;
    if (alarm_exceeded)
    {
        if (!state.alarm_since_ns)
            state.alarm_since_ns = monotonic_ns;
    }
    else
    {
        state.alarm_since_ns.reset();
        if (state.alarm_active && alarms_)
            static_cast<void>(alarms_->clear("TIME_SYNC_ALARM_THRESHOLD_EXCEEDED", source_id));
        state.alarm_active = false;
    }

    const bool alarm_due =
        alarm_exceeded && monotonic_ns >= *state.alarm_since_ns &&
        monotonic_ns - *state.alarm_since_ns >= duration_ns(options_.alarm_duration);
    if (alarm_due)
    {
        if (!state.alarm_active && alarms_)
            static_cast<void>(alarms_->raise_alarm(
                {.code = "TIME_SYNC_ALARM_THRESHOLD_EXCEEDED",
                 .severity = Severity::error,
                 .source = source_id,
                 .message = "时间同步质量持续超过 Alarm 阈值",
                 .details = threshold_details(*model, observed, options_.alarm_threshold_ns)}));
        if (state.warning_active && alarms_)
            static_cast<void>(alarms_->clear("TIME_SYNC_WARNING_THRESHOLD_EXCEEDED", source_id));
        state.alarm_active = true;
        state.warning_active = false;
        return;
    }

    const bool warning_due =
        monotonic_ns >= *state.warning_since_ns &&
        monotonic_ns - *state.warning_since_ns >= duration_ns(options_.warning_duration);
    if (warning_due && !state.warning_active && alarms_)
    {
        static_cast<void>(alarms_->raise_alarm(
            {.code = "TIME_SYNC_WARNING_THRESHOLD_EXCEEDED",
             .severity = Severity::warning,
             .source = source_id,
             .message = "时间同步质量持续超过 Warning 阈值",
             .details = threshold_details(*model, observed, options_.warning_threshold_ns)}));
        state.warning_active = true;
    }
}

void TimeSyncAlarmMonitor::clear_thresholds(const std::string& source_id,
                                            EntityState& state) noexcept
{
    if (alarms_)
    {
        if (state.warning_active)
            static_cast<void>(alarms_->clear("TIME_SYNC_WARNING_THRESHOLD_EXCEEDED", source_id));
        if (state.alarm_active)
            static_cast<void>(alarms_->clear("TIME_SYNC_ALARM_THRESHOLD_EXCEEDED", source_id));
    }
    state.warning_active = false;
    state.alarm_active = false;
}

void TimeSyncAlarmMonitor::stop() noexcept
{
    std::scoped_lock lock{mutex_};
    for (auto& [source_id, state] : states_)
        clear_thresholds(source_id, state);
    states_.clear();
}

} // namespace paperbreak::service
