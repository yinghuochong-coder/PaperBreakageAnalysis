#include "paperbreak/monitoring/monitoring.hpp"

#include "paperbreak/logging/logging.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace paperbreak::monitoring
{
namespace
{

constexpr std::size_t maximum_code_length = 128U;
constexpr std::size_t maximum_source_length = 128U;
constexpr std::size_t maximum_message_length = 1024U;
constexpr std::size_t maximum_detail_count = 32U;
constexpr std::size_t maximum_detail_key_length = 64U;
constexpr std::size_t maximum_detail_value_length = 512U;
constexpr std::size_t maximum_metric_string_length = 512U;

Error monitoring_error(std::string code, std::string message, std::string operation,
                       const Severity severity = Severity::error)
{
    return make_error(std::move(code), severity, std::move(message), "monitoring",
                      std::move(operation));
}

bool valid_code(const std::string_view code)
{
    if (code.empty() || code.size() > maximum_code_length || code.front() < 'A' ||
        code.front() > 'Z')
    {
        return false;
    }
    bool has_separator = false;
    for (const char character : code)
    {
        if (character == '_')
        {
            has_separator = true;
            continue;
        }
        if ((character < 'A' || character > 'Z') && (character < '0' || character > '9'))
        {
            return false;
        }
    }
    return has_separator;
}

bool valid_metric_name(const std::string_view name)
{
    if (name.empty() || name.size() > 192U)
    {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](const char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '.' || character == '_' ||
               character == '-';
    });
}

bool valid_source(const std::string_view source)
{
    return !source.empty() && source.size() <= maximum_source_length &&
           std::all_of(source.begin(), source.end(), [](const char character) {
               return (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') || character == '.' ||
                      character == '_' || character == '-';
           });
}

int severity_rank(const Severity severity) noexcept
{
    return static_cast<int>(severity);
}

std::optional<double> metric_number(const MetricsSnapshot& snapshot, const std::string_view name)
{
    const auto iterator = std::find_if(
        snapshot.metrics.begin(), snapshot.metrics.end(),
        [name](const MetricPoint& point) { return point.name == name && point.available; });
    if (iterator == snapshot.metrics.end())
    {
        return std::nullopt;
    }
    return std::visit(
        [](const auto& value) -> std::optional<double> {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_arithmetic_v<Value>)
            {
                return static_cast<double>(value);
            }
            else
            {
                return std::nullopt;
            }
        },
        iterator->value);
}

AlarmInput threshold_alarm(std::string code, const Severity severity, std::string source,
                           std::string message, const double value, const double threshold)
{
    return {
        .code = std::move(code),
        .severity = severity,
        .source = std::move(source),
        .message = std::move(message),
        .details = {{"value", std::to_string(value)}, {"threshold", std::to_string(threshold)}}};
}

} // namespace

MetricRegistry::MetricRegistry(const std::size_t capacity, const std::size_t source_capacity)
    : capacity_(capacity), source_capacity_(source_capacity)
{
}

Result<void> MetricRegistry::replace_source(std::string source, std::vector<MetricPoint> metrics)
{
    if (!valid_source(source) || metrics.size() > capacity_)
    {
        return Result<void>::failure(monitoring_error(
            "MONITORING_RECORD_INVALID", "指标来源或数量无效", "monitoring.metrics.replace"));
    }
    std::set<std::string> names;
    for (auto& metric : metrics)
    {
        if (!valid_metric_name(metric.name) || metric.unit.size() > 32U ||
            !names.emplace(metric.name).second)
        {
            return Result<void>::failure(monitoring_error("MONITORING_RECORD_INVALID",
                                                          "指标名称、单位或重复项无效",
                                                          "monitoring.metrics.replace"));
        }
        if (auto* string_value = std::get_if<std::string>(&metric.value))
        {
            if (string_value->size() > maximum_metric_string_length)
            {
                return Result<void>::failure(monitoring_error("MONITORING_RECORD_INVALID",
                                                              "指标字符串值超过允许范围",
                                                              "monitoring.metrics.replace"));
            }
            *string_value = logging::redact_sensitive(*string_value);
        }
    }

    std::scoped_lock lock{mutex_};
    if (!sources_.contains(source) && sources_.size() >= source_capacity_)
    {
        return Result<void>::failure(monitoring_error(
            "MONITORING_CAPACITY_EXCEEDED", "指标源数量达到上限", "monitoring.metrics.replace"));
    }
    std::size_t retained = 0U;
    for (const auto& [stored_key, stored] : points_)
    {
        static_cast<void>(stored_key);
        if (stored.source != source)
        {
            ++retained;
        }
    }
    if (metrics.size() > capacity_ - std::min(retained, capacity_))
    {
        return Result<void>::failure(monitoring_error(
            "MONITORING_CAPACITY_EXCEEDED", "指标注册表容量不足", "monitoring.metrics.replace"));
    }
    for (auto iterator = points_.begin(); iterator != points_.end();)
    {
        if (iterator->second.source == source)
        {
            iterator = points_.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
    for (auto& point : metrics)
    {
        const std::string storage_key = source + '\x1f' + point.name;
        points_.emplace(storage_key, StoredPoint{.source = source, .point = std::move(point)});
    }
    sources_.emplace(source);
    ++version_;
    sampled_at_ = current_utc_timestamp();
    return Result<void>::success();
}

MetricQueryResult MetricRegistry::query(const MetricQuery& query) const
{
    MetricQueryResult result;
    const std::size_t limit = std::min<std::size_t>(query.limit, 256U);
    std::scoped_lock lock{mutex_};
    result.snapshot.version = version_;
    result.snapshot.sampled_at = sampled_at_;
    for (const auto& [stored_key, stored] : points_)
    {
        static_cast<void>(stored_key);
        const bool matches =
            query.prefixes.empty() || std::any_of(query.prefixes.begin(), query.prefixes.end(),
                                                  [&stored](const std::string& prefix) {
                                                      return stored.point.name.starts_with(prefix);
                                                  });
        if (!matches)
        {
            continue;
        }
        if (result.snapshot.metrics.size() >= limit)
        {
            result.truncated = true;
            break;
        }
        result.snapshot.metrics.push_back(stored.point);
    }
    return result;
}

std::size_t MetricRegistry::size() const
{
    std::scoped_lock lock{mutex_};
    return points_.size();
}

AlarmRegistry::AlarmRegistry(const std::size_t active_capacity, const std::size_t history_capacity)
    : active_capacity_(active_capacity), history_capacity_(history_capacity)
{
}

std::string AlarmRegistry::key(const std::string_view code, const std::string_view source)
{
    return std::string{code} + '\x1f' + std::string{source};
}

Result<AlarmRecord> AlarmRegistry::raise_alarm(AlarmInput input)
{
    if (!valid_code(input.code) || !valid_source(input.source) || input.message.empty() ||
        input.message.size() > maximum_message_length ||
        input.details.size() > maximum_detail_count ||
        std::any_of(input.details.begin(), input.details.end(), [](const ErrorDetail& detail) {
            return detail.key.empty() || detail.key.size() > maximum_detail_key_length ||
                   detail.value.size() > maximum_detail_value_length;
        }))
    {
        return Result<AlarmRecord>::failure(monitoring_error(
            "MONITORING_RECORD_INVALID", "报警字段或详情超过允许范围", "monitoring.alarm.raise"));
    }
    input.message = logging::redact_sensitive(input.message);
    for (auto& detail : input.details)
    {
        detail.value = logging::redact_sensitive(detail.value);
    }

    AlarmChange change;
    {
        std::scoped_lock lock{mutex_};
        const std::string alarm_key = key(input.code, input.source);
        auto iterator = active_.find(alarm_key);
        if (iterator == active_.end())
        {
            if (active_.size() >= active_capacity_)
            {
                return Result<AlarmRecord>::failure(monitoring_error("MONITORING_CAPACITY_EXCEEDED",
                                                                     "活动报警登记表容量不足",
                                                                     "monitoring.alarm.raise"));
            }
            const std::string timestamp = current_utc_timestamp();
            AlarmRecord record{.alarm_id = next_alarm_id_++,
                               .revision = ++registry_revision_,
                               .code = std::move(input.code),
                               .severity = input.severity,
                               .source = std::move(input.source),
                               .first_occurred_at = timestamp,
                               .last_occurred_at = timestamp,
                               .active = true,
                               .occurrence_count = 1U,
                               .message = std::move(input.message),
                               .details = std::move(input.details),
                               .acknowledged = false};
            iterator = active_.emplace(alarm_key, std::move(record)).first;
        }
        else
        {
            AlarmRecord& record = iterator->second;
            record.revision = ++registry_revision_;
            record.severity = input.severity;
            record.last_occurred_at = current_utc_timestamp();
            if (record.occurrence_count < (std::numeric_limits<std::uint64_t>::max)())
            {
                ++record.occurrence_count;
            }
            record.message = std::move(input.message);
            record.details = std::move(input.details);
            record.acknowledged = false;
        }
        change = {.kind = AlarmChangeKind::raised,
                  .registry_revision = registry_revision_,
                  .alarm = iterator->second};
    }
    notify(change);
    return Result<AlarmRecord>::success(change.alarm);
}

Result<std::optional<AlarmRecord>> AlarmRegistry::clear(const std::string_view code,
                                                        const std::string_view source)
{
    if (!valid_code(code) || !valid_source(source))
    {
        return Result<std::optional<AlarmRecord>>::failure(monitoring_error(
            "MONITORING_RECORD_INVALID", "报警代码或来源无效", "monitoring.alarm.clear"));
    }
    AlarmChange change;
    bool changed = false;
    {
        std::scoped_lock lock{mutex_};
        const auto iterator = active_.find(key(code, source));
        if (iterator == active_.end())
        {
            return Result<std::optional<AlarmRecord>>::success(std::nullopt);
        }
        AlarmRecord record = std::move(iterator->second);
        active_.erase(iterator);
        record.active = false;
        record.revision = ++registry_revision_;
        if (history_capacity_ > 0U)
        {
            while (history_.size() >= history_capacity_)
            {
                history_.pop_front();
            }
            history_.push_back(record);
        }
        change = {.kind = AlarmChangeKind::cleared,
                  .registry_revision = registry_revision_,
                  .alarm = record};
        changed = true;
    }
    if (changed)
    {
        notify(change);
    }
    return Result<std::optional<AlarmRecord>>::success(change.alarm);
}

Result<AlarmRecord> AlarmRegistry::acknowledge(const std::uint64_t alarm_id)
{
    AlarmChange change;
    bool notify_change = false;
    {
        std::scoped_lock lock{mutex_};
        AlarmRecord* record = nullptr;
        for (auto& [alarm_key, active] : active_)
        {
            static_cast<void>(alarm_key);
            if (active.alarm_id == alarm_id)
            {
                record = &active;
                break;
            }
        }
        if (record == nullptr)
        {
            const auto iterator = std::find_if(history_.begin(), history_.end(),
                                               [alarm_id](const AlarmRecord& candidate) {
                                                   return candidate.alarm_id == alarm_id;
                                               });
            if (iterator != history_.end())
            {
                record = &*iterator;
            }
        }
        if (record == nullptr)
        {
            return Result<AlarmRecord>::failure(monitoring_error(
                "ALARM_NOT_FOUND", "报警不存在或已从内存历史淘汰", "monitoring.alarm.acknowledge"));
        }
        if (!record->acknowledged)
        {
            record->acknowledged = true;
            record->revision = ++registry_revision_;
            notify_change = true;
        }
        change = {.kind = AlarmChangeKind::acknowledged,
                  .registry_revision = registry_revision_,
                  .alarm = *record};
    }
    if (notify_change)
    {
        notify(change);
    }
    return Result<AlarmRecord>::success(change.alarm);
}

AlarmQueryResult AlarmRegistry::query(const AlarmQuery& query) const
{
    AlarmQueryResult result;
    const std::size_t limit = std::min<std::size_t>(query.limit, 200U);
    std::vector<AlarmRecord> candidates;
    {
        std::scoped_lock lock{mutex_};
        result.registry_revision = registry_revision_;
        candidates.reserve(active_.size() + history_.size());
        for (const auto& [alarm_key, record] : active_)
        {
            static_cast<void>(alarm_key);
            candidates.push_back(record);
        }
        candidates.insert(candidates.end(), history_.begin(), history_.end());
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const AlarmRecord& left, const AlarmRecord& right) {
                  return left.alarm_id > right.alarm_id;
              });
    for (const auto& record : candidates)
    {
        if ((query.active.has_value() && record.active != query.active.value()) ||
            (query.minimum_severity.has_value() &&
             severity_rank(record.severity) < severity_rank(query.minimum_severity.value())) ||
            (query.source.has_value() && record.source != query.source.value()) ||
            (query.before_alarm_id.has_value() && record.alarm_id >= query.before_alarm_id.value()))
        {
            continue;
        }
        if (result.alarms.size() >= limit)
        {
            result.truncated = true;
            break;
        }
        result.alarms.push_back(record);
    }
    if (result.truncated && !result.alarms.empty())
    {
        result.next_before_alarm_id = result.alarms.back().alarm_id;
    }
    return result;
}

void AlarmRegistry::set_observer(AlarmObserver observer)
{
    std::scoped_lock lock{mutex_};
    observer_ = std::move(observer);
}

void AlarmRegistry::notify(const AlarmChange& change) const noexcept
{
    AlarmObserver observer;
    {
        std::scoped_lock lock{mutex_};
        observer = observer_;
    }
    if (!observer)
    {
        return;
    }
    try
    {
        observer(change);
    }
    catch (...)
    {
    }
}

HealthMonitor::HealthMonitor(std::shared_ptr<MetricRegistry> metrics,
                             std::shared_ptr<AlarmRegistry> alarms, HealthMonitorOptions options)
    : metrics_(std::move(metrics)), alarms_(std::move(alarms)), options_(std::move(options))
{
}

HealthMonitor::~HealthMonitor()
{
    request_stop();
    static_cast<void>(join(std::chrono::steady_clock::now() + std::chrono::seconds{5}));
}

Result<void> HealthMonitor::register_source(std::shared_ptr<IMetricSource> source)
{
    if (!source || !valid_source(source->source_name()))
    {
        return Result<void>::failure(monitoring_error("MONITORING_RECORD_INVALID", "指标源无效",
                                                      "monitoring.health.registerSource"));
    }
    std::scoped_lock lock{mutex_};
    if (sources_.size() >= std::min(options_.source_capacity, default_metric_source_capacity))
    {
        return Result<void>::failure(monitoring_error("MONITORING_CAPACITY_EXCEEDED",
                                                      "指标源数量达到上限",
                                                      "monitoring.health.registerSource"));
    }
    if (std::any_of(sources_.begin(), sources_.end(), [&source](const auto& existing) {
            return existing->source_name() == source->source_name();
        }))
    {
        return Result<void>::failure(monitoring_error("MONITORING_RECORD_INVALID", "指标源名称重复",
                                                      "monitoring.health.registerSource"));
    }
    sources_.push_back(std::move(source));
    ++configuration_generation_;
    condition_.notify_all();
    return Result<void>::success();
}

Result<void> HealthMonitor::reconfigure(HealthMonitorOptions options)
{
    if (options.sample_interval < std::chrono::milliseconds{100} ||
        options.sample_interval > std::chrono::seconds{60} || options.cpu_warning_percent < 1.0 ||
        options.cpu_warning_percent > 100.0 || options.memory_warning_percent < 1.0 ||
        options.memory_warning_percent > 100.0 || options.source_capacity == 0U ||
        options.source_capacity > default_metric_source_capacity ||
        options.disks.size() > default_metric_source_capacity)
    {
        return Result<void>::failure(monitoring_error("MONITORING_RECORD_INVALID",
                                                      "健康监测配置超出允许范围",
                                                      "monitoring.health.reconfigure"));
    }
    std::unordered_set<std::string> disk_sources;
    if (std::any_of(options.disks.begin(), options.disks.end(), [&disk_sources](const auto& disk) {
            return !valid_metric_name(disk.metric_name) || !valid_source(disk.source) ||
                   disk.warning_free_gib < 0.0 || disk.critical_free_gib < 0.0 ||
                   disk.stop_free_gib < 0.0 || disk.warning_free_gib < disk.critical_free_gib ||
                   disk.critical_free_gib < disk.stop_free_gib ||
                   !disk_sources.emplace(disk.source).second;
        }))
    {
        return Result<void>::failure(monitoring_error(
            "MONITORING_RECORD_INVALID", "磁盘监测配置无效", "monitoring.health.reconfigure"));
    }
    std::scoped_lock lock{mutex_};
    if (sources_.size() > options.source_capacity)
    {
        return Result<void>::failure(monitoring_error("MONITORING_CAPACITY_EXCEEDED",
                                                      "新的指标源容量小于当前注册数量",
                                                      "monitoring.health.reconfigure"));
    }
    if (!options.register_thread)
        options.register_thread = options_.register_thread;
    options_ = std::move(options);
    ++configuration_generation_;
    condition_.notify_all();
    return Result<void>::success();
}

Result<void> HealthMonitor::start()
{
    if (!metrics_ || !alarms_)
    {
        return Result<void>::failure(
            monitoring_error("SYS_INTERNAL_ERROR", "健康监测依赖为空", "monitoring.health.start"));
    }
    HealthMonitorOptions initial_options;
    {
        std::scoped_lock lock{mutex_};
        initial_options = options_;
    }
    auto validated = reconfigure(std::move(initial_options));
    if (!validated)
    {
        return validated;
    }
    std::scoped_lock lock{mutex_};
    if (started_)
    {
        return Result<void>::success();
    }
    completed_ = false;
    started_ = true;
    try
    {
        worker_ = std::jthread([this](const std::stop_token token) { run(token); });
    }
    catch (...)
    {
        completed_ = true;
        started_ = false;
        return Result<void>::failure(
            monitoring_error("SYS_SERVICE_START_FAILED", "无法创建健康监测线程",
                             "monitoring.health.start", Severity::critical));
    }
    return Result<void>::success();
}

void HealthMonitor::request_stop() noexcept
{
    std::scoped_lock lock{mutex_};
    if (worker_.joinable())
    {
        worker_.request_stop();
    }
    condition_.notify_all();
}

Result<void> HealthMonitor::join(const std::chrono::steady_clock::time_point deadline)
{
    request_stop();
    {
        std::unique_lock lock{mutex_};
        if (!condition_.wait_until(lock, deadline, [this] { return completed_; }))
        {
            return Result<void>::failure(
                monitoring_error("SYS_SHUTDOWN_TIMEOUT", "健康监测线程未在截止时间内退出",
                                 "monitoring.health.join", Severity::critical));
        }
    }
    if (worker_.joinable())
    {
        worker_.join();
    }
    std::scoped_lock lock{mutex_};
    started_ = false;
    return Result<void>::success();
}

void HealthMonitor::run(const std::stop_token stop_token) noexcept
{
    ThreadRegistrationFactory registrar;
    {
        std::scoped_lock lock{mutex_};
        registrar = options_.register_thread;
    }
    const auto thread_registration = registrar ? registrar("health-monitor") : nullptr;
    while (!stop_token.stop_requested())
    {
        sample_once(stop_token);
        std::unique_lock lock{mutex_};
        const auto interval = options_.sample_interval;
        const auto generation = configuration_generation_;
        condition_.wait_for(lock, interval, [this, stop_token, generation] {
            return stop_token.stop_requested() || configuration_generation_ != generation;
        });
    }
    {
        std::scoped_lock lock{mutex_};
        completed_ = true;
    }
    condition_.notify_all();
}

void HealthMonitor::sample_once(const std::stop_token stop_token) noexcept
{
    std::vector<std::shared_ptr<IMetricSource>> sources;
    {
        std::scoped_lock lock{mutex_};
        sources = sources_;
    }
    for (const auto& source : sources)
    {
        if (stop_token.stop_requested())
        {
            return;
        }
        auto collected = source->collect(stop_token);
        if (!collected)
        {
            if (failed_sources_.emplace(source->source_name()).second)
            {
                static_cast<void>(alarms_->raise_alarm(
                    {.code = "SYS_MONITORING_SAMPLE_FAILED",
                     .severity = Severity::warning,
                     .source = std::string{source->source_name()},
                     .message = "健康指标源采样失败",
                     .details = {{"businessCode", collected.error().business_code}}}));
            }
            continue;
        }
        auto replaced = metrics_->replace_source(std::string{source->source_name()},
                                                 std::move(collected).value());
        if (!replaced)
        {
            if (failed_sources_.emplace(source->source_name()).second)
            {
                static_cast<void>(alarms_->raise_alarm(
                    {.code = "SYS_MONITORING_SAMPLE_FAILED",
                     .severity = Severity::warning,
                     .source = std::string{source->source_name()},
                     .message = "健康指标快照登记失败",
                     .details = {{"businessCode", replaced.error().business_code}}}));
            }
            continue;
        }
        if (failed_sources_.erase(std::string{source->source_name()}) > 0U)
        {
            static_cast<void>(
                alarms_->clear("SYS_MONITORING_SAMPLE_FAILED", source->source_name()));
        }
    }
    evaluate_thresholds(metrics_->query({.prefixes = {}, .limit = 256U}).snapshot);
}

void HealthMonitor::evaluate_thresholds(const MetricsSnapshot& snapshot) noexcept
{
    HealthMonitorOptions options;
    {
        std::scoped_lock lock{mutex_};
        options = options_;
    }
    const auto cpu = metric_number(snapshot, "process.cpu.percent");
    const bool cpu_high = cpu.has_value() && cpu.value() >= options.cpu_warning_percent;
    if (cpu_high && !cpu_alarm_active_)
    {
        static_cast<void>(alarms_->raise_alarm(
            threshold_alarm("SYS_CPU_USAGE_HIGH", Severity::warning, "process",
                            "进程 CPU 使用率超过阈值", cpu.value(), options.cpu_warning_percent)));
    }
    else if (!cpu_high && cpu_alarm_active_)
    {
        static_cast<void>(alarms_->clear("SYS_CPU_USAGE_HIGH", "process"));
    }
    cpu_alarm_active_ = cpu_high;

    const auto memory = metric_number(snapshot, "system.memory.used_percent");
    const bool memory_high = memory.has_value() && memory.value() >= options.memory_warning_percent;
    if (memory_high && !memory_alarm_active_)
    {
        static_cast<void>(alarms_->raise_alarm(threshold_alarm(
            "SYS_MEMORY_USAGE_HIGH", Severity::warning, "system", "系统内存使用率超过阈值",
            memory.value(), options.memory_warning_percent)));
    }
    else if (!memory_high && memory_alarm_active_)
    {
        static_cast<void>(alarms_->clear("SYS_MEMORY_USAGE_HIGH", "system"));
    }
    memory_alarm_active_ = memory_high;

    for (const auto& disk : options.disks)
    {
        const auto free_gib = metric_number(snapshot, disk.metric_name);
        DiskState next = DiskState::unavailable;
        if (free_gib.has_value())
        {
            next = DiskState::normal;
            if (free_gib.value() <= disk.stop_free_gib)
                next = DiskState::stop;
            else if (free_gib.value() <= disk.critical_free_gib)
                next = DiskState::critical;
            else if (free_gib.value() <= disk.warning_free_gib)
                next = DiskState::warning;
        }
        const DiskState previous = disk_states_[disk.source];
        if (next == previous)
        {
            continue;
        }
        static_cast<void>(alarms_->clear("STORAGE_LOW_SPACE", disk.source));
        static_cast<void>(alarms_->clear("STORAGE_CRITICAL_SPACE", disk.source));
        static_cast<void>(alarms_->clear("STORAGE_STOP_SAVE", disk.source));
        if (free_gib.has_value() && next != DiskState::normal)
        {
            std::string code = "STORAGE_LOW_SPACE";
            Severity severity = Severity::warning;
            double threshold = disk.warning_free_gib;
            if (next == DiskState::critical)
            {
                code = "STORAGE_CRITICAL_SPACE";
                severity = Severity::critical;
                threshold = disk.critical_free_gib;
            }
            else if (next == DiskState::stop)
            {
                code = "STORAGE_STOP_SAVE";
                severity = Severity::critical;
                threshold = disk.stop_free_gib;
            }
            static_cast<void>(alarms_->raise_alarm(
                threshold_alarm(std::move(code), severity, disk.source, "磁盘可用空间低于配置阈值",
                                free_gib.value(), threshold)));
        }
        disk_states_[disk.source] = next;
    }
}

std::string_view severity_name(const Severity severity) noexcept
{
    switch (severity)
    {
    case Severity::info:
        return "Info";
    case Severity::warning:
        return "Warning";
    case Severity::error:
        return "Error";
    case Severity::critical:
        return "Critical";
    }
    return "Error";
}

std::optional<Severity> parse_severity(const std::string_view value) noexcept
{
    if (value == "Info")
        return Severity::info;
    if (value == "Warning")
        return Severity::warning;
    if (value == "Error")
        return Severity::error;
    if (value == "Critical")
        return Severity::critical;
    return std::nullopt;
}

std::string_view alarm_change_name(const AlarmChangeKind kind) noexcept
{
    switch (kind)
    {
    case AlarmChangeKind::raised:
        return "alarm.raised";
    case AlarmChangeKind::cleared:
        return "alarm.cleared";
    case AlarmChangeKind::acknowledged:
        return "alarm.acknowledged";
    }
    return "alarm.raised";
}

} // namespace paperbreak::monitoring
