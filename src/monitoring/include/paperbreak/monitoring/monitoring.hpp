#pragma once

#include "paperbreak/common/result.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace paperbreak::monitoring
{

inline constexpr std::size_t default_metric_capacity = 1024U;
inline constexpr std::size_t default_metric_source_capacity = 64U;
inline constexpr std::size_t default_active_alarm_capacity = 1024U;
inline constexpr std::size_t default_alarm_history_capacity = 4096U;

using MetricValue = std::variant<std::int64_t, std::uint64_t, double, bool, std::string>;

struct MetricPoint final
{
    std::string name;
    MetricValue value;
    std::string unit;
    bool available{true};
};

struct MetricsSnapshot final
{
    std::uint64_t version{};
    std::string sampled_at;
    std::vector<MetricPoint> metrics;
};

struct MetricQuery final
{
    std::vector<std::string> prefixes;
    std::size_t limit{256U};
};

struct MetricQueryResult final
{
    MetricsSnapshot snapshot;
    bool truncated{};
};

class MetricRegistry final
{
  public:
    explicit MetricRegistry(std::size_t capacity = default_metric_capacity,
                            std::size_t source_capacity = default_metric_source_capacity);

    [[nodiscard]] Result<void> replace_source(std::string source, std::vector<MetricPoint> metrics);
    [[nodiscard]] MetricQueryResult query(const MetricQuery& query = {}) const;
    [[nodiscard]] std::size_t size() const;

  private:
    struct StoredPoint final
    {
        std::string source;
        MetricPoint point;
    };

    std::size_t capacity_;
    std::size_t source_capacity_;
    mutable std::mutex mutex_;
    std::map<std::string, StoredPoint> points_;
    std::unordered_set<std::string> sources_;
    std::uint64_t version_{};
    std::string sampled_at_;
};

struct AlarmInput final
{
    std::string code;
    Severity severity{Severity::error};
    std::string source;
    std::string message;
    std::vector<ErrorDetail> details;
};

struct AlarmRecord final
{
    std::uint64_t alarm_id{};
    std::uint64_t revision{};
    std::string code;
    Severity severity{Severity::error};
    std::string source;
    std::string first_occurred_at;
    std::string last_occurred_at;
    bool active{true};
    std::uint64_t occurrence_count{1U};
    std::string message;
    std::vector<ErrorDetail> details;
    bool acknowledged{};
};

enum class AlarmChangeKind
{
    raised,
    cleared,
    acknowledged,
};

struct AlarmChange final
{
    AlarmChangeKind kind{AlarmChangeKind::raised};
    std::uint64_t registry_revision{};
    AlarmRecord alarm;
};

struct AlarmQuery final
{
    std::optional<bool> active;
    std::optional<Severity> minimum_severity;
    std::optional<std::string> source;
    std::optional<std::uint64_t> before_alarm_id;
    std::size_t limit{100U};
};

struct AlarmQueryResult final
{
    std::uint64_t registry_revision{};
    std::vector<AlarmRecord> alarms;
    std::optional<std::uint64_t> next_before_alarm_id;
    bool truncated{};
};

using AlarmObserver = std::function<void(const AlarmChange&)>;

class AlarmRegistry final
{
  public:
    explicit AlarmRegistry(std::size_t active_capacity = default_active_alarm_capacity,
                           std::size_t history_capacity = default_alarm_history_capacity);

    [[nodiscard]] Result<AlarmRecord> raise_alarm(AlarmInput input);
    [[nodiscard]] Result<std::optional<AlarmRecord>> clear(std::string_view code,
                                                           std::string_view source);
    [[nodiscard]] Result<AlarmRecord> acknowledge(std::uint64_t alarm_id);
    [[nodiscard]] AlarmQueryResult query(const AlarmQuery& query = {}) const;
    void set_observer(AlarmObserver observer);

  private:
    [[nodiscard]] static std::string key(std::string_view code, std::string_view source);
    void notify(const AlarmChange& change) const noexcept;

    std::size_t active_capacity_;
    std::size_t history_capacity_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, AlarmRecord> active_;
    std::deque<AlarmRecord> history_;
    std::uint64_t next_alarm_id_{1U};
    std::uint64_t registry_revision_{};
    AlarmObserver observer_;
};

class IMetricSource
{
  public:
    virtual ~IMetricSource() = default;
    [[nodiscard]] virtual std::string_view source_name() const noexcept = 0;
    [[nodiscard]] virtual Result<std::vector<MetricPoint>> collect(
        std::stop_token stop_token) noexcept = 0;
};

struct DiskThreshold final
{
    std::string metric_name;
    std::string source;
    double warning_free_gib{};
    double critical_free_gib{};
    double stop_free_gib{};
};

struct HealthMonitorOptions final
{
    std::chrono::milliseconds sample_interval{std::chrono::seconds{1}};
    double cpu_warning_percent{85.0};
    double memory_warning_percent{85.0};
    std::vector<DiskThreshold> disks;
    std::size_t source_capacity{default_metric_source_capacity};
};

class HealthMonitor final
{
  public:
    HealthMonitor(std::shared_ptr<MetricRegistry> metrics, std::shared_ptr<AlarmRegistry> alarms,
                  HealthMonitorOptions options = {});
    ~HealthMonitor();

    HealthMonitor(const HealthMonitor&) = delete;
    HealthMonitor& operator=(const HealthMonitor&) = delete;

    [[nodiscard]] Result<void> register_source(std::shared_ptr<IMetricSource> source);
    [[nodiscard]] Result<void> reconfigure(HealthMonitorOptions options);
    [[nodiscard]] Result<void> start();
    void request_stop() noexcept;
    [[nodiscard]] Result<void> join(std::chrono::steady_clock::time_point deadline);

  private:
    enum class DiskState
    {
        normal,
        warning,
        critical,
        stop,
        unavailable,
    };

    void run(std::stop_token stop_token) noexcept;
    void sample_once(std::stop_token stop_token) noexcept;
    void evaluate_thresholds(const MetricsSnapshot& snapshot) noexcept;

    std::shared_ptr<MetricRegistry> metrics_;
    std::shared_ptr<AlarmRegistry> alarms_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    HealthMonitorOptions options_;
    std::vector<std::shared_ptr<IMetricSource>> sources_;
    std::unordered_map<std::string, DiskState> disk_states_;
    std::unordered_set<std::string> failed_sources_;
    bool cpu_alarm_active_{};
    bool memory_alarm_active_{};
    bool completed_{true};
    bool started_{};
    std::uint64_t configuration_generation_{};
    std::jthread worker_;
};

[[nodiscard]] std::string_view severity_name(Severity severity) noexcept;
[[nodiscard]] std::optional<Severity> parse_severity(std::string_view value) noexcept;
[[nodiscard]] std::string_view alarm_change_name(AlarmChangeKind kind) noexcept;

} // namespace paperbreak::monitoring
