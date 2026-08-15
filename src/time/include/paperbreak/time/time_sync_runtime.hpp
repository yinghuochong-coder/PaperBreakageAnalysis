#pragma once

#include "paperbreak/time/time_model.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace paperbreak::time
{

inline constexpr std::size_t time_sync_camera_capacity = 4U;
inline constexpr std::size_t time_sync_control_capacity = 16U;

struct RuntimeClockReading final
{
    std::int64_t monotonic_ns{};
    std::int64_t utc_ns{};
};

class IRuntimeClock
{
  public:
    virtual ~IRuntimeClock() = default;
    /// Called by the worker and read-only status paths; implementations must be thread-safe.
    [[nodiscard]] virtual RuntimeClockReading read() noexcept = 0;
};

class StandardRuntimeClock final : public IRuntimeClock
{
  public:
    [[nodiscard]] RuntimeClockReading read() noexcept override;
};

struct SystemClockProbeSample final
{
    ClockSource clock_source{ClockSource::unknown};
    SyncState sync_state{SyncState::unknown};
    std::int64_t sample_monotonic_ns{};
    std::int64_t sample_utc_ns{};
    std::optional<std::int64_t> offset_ns;
    std::int64_t uncertainty_ns{};
    std::optional<std::int64_t> maximum_observed_offset_ns;
    std::optional<std::int64_t> last_synchronized_utc_ns;
    std::optional<std::string> grandmaster_identity;
    std::optional<std::string> last_error_code;
};

struct CameraClockProbeSample final
{
    std::uint64_t camera_timestamp_ticks{};
    std::uint64_t camera_timestamp_frequency_hz{};
    std::int64_t sample_monotonic_ns{};
    std::int64_t sample_utc_ns{};
    bool hardware_ptp_synchronized{};
    std::optional<std::int64_t> offset_ns;
    std::int64_t uncertainty_ns{};
    std::optional<std::int64_t> maximum_observed_offset_ns;
    std::optional<std::int64_t> last_synchronized_utc_ns;
    std::optional<std::string> grandmaster_identity;
    std::optional<std::string> last_error_code;
};

class ISystemClockProbe
{
  public:
    virtual ~ISystemClockProbe() = default;
    [[nodiscard]] virtual Result<SystemClockProbeSample> sample(
        std::stop_token stop_token, std::chrono::steady_clock::time_point deadline) = 0;
};

class ICameraClockProbe
{
  public:
    virtual ~ICameraClockProbe() = default;
    [[nodiscard]] virtual std::string_view camera_id() const noexcept = 0;
    [[nodiscard]] virtual Result<CameraClockProbeSample> sample(
        std::stop_token stop_token, std::chrono::steady_clock::time_point deadline) = 0;
};

struct TimeSyncRuntimeOptions final
{
    std::chrono::milliseconds sample_period{std::chrono::seconds{1}};
    std::chrono::milliseconds probe_timeout{std::chrono::milliseconds{250}};
    std::chrono::milliseconds first_sample_timeout{std::chrono::seconds{2}};
    std::int64_t receive_clock_uncertainty_ns{50'000'000};
    std::int64_t system_time_jump_threshold_ns{100'000'000};
    std::function<void(std::int64_t, const std::shared_ptr<const ClockModelSnapshot>&,
                       const std::vector<std::shared_ptr<const ClockModelSnapshot>>&)>
        model_observer;
};

enum class TimeSyncRuntimeState : std::uint8_t
{
    created,
    running,
    stop_requested,
    stopped,
    failed,
};

struct TimeSyncRuntimeMetrics final
{
    std::uint64_t sample_cycles{};
    std::uint64_t published_models{};
    std::uint64_t accepted_refresh_requests{};
    std::uint64_t processed_refresh_requests{};
    std::uint64_t rejected_refresh_requests{};
    std::size_t control_depth{};
    std::size_t control_high_watermark{};
    std::uint64_t last_model_revision{};
};

class TimeSyncRuntime final
{
  private:
    struct Impl;

  public:
    static Result<std::unique_ptr<TimeSyncRuntime>> create(
        std::unique_ptr<ISystemClockProbe> system_probe,
        std::vector<std::unique_ptr<ICameraClockProbe>> camera_probes,
        std::unique_ptr<IRuntimeClock> runtime_clock = std::make_unique<StandardRuntimeClock>(),
        TimeSyncRuntimeOptions options = {});

    ~TimeSyncRuntime();
    TimeSyncRuntime(const TimeSyncRuntime&) = delete;
    TimeSyncRuntime& operator=(const TimeSyncRuntime&) = delete;
    TimeSyncRuntime(TimeSyncRuntime&&) = delete;
    TimeSyncRuntime& operator=(TimeSyncRuntime&&) = delete;

    [[nodiscard]] Result<void> start();
    [[nodiscard]] Result<void> request_refresh();
    void request_stop() noexcept;
    [[nodiscard]] Result<void> join(std::chrono::steady_clock::time_point deadline);

    [[nodiscard]] TimeSyncRuntimeState state() const noexcept;
    [[nodiscard]] TimeSyncRuntimeMetrics metrics() const noexcept;
    [[nodiscard]] std::shared_ptr<const ClockModelSnapshot> system_model() const noexcept;
    [[nodiscard]] std::shared_ptr<const ClockModelSnapshot> camera_model(
        std::string_view camera_id) const noexcept;
    [[nodiscard]] ClockSyncSnapshot system_status() const noexcept;
    [[nodiscard]] ClockSyncSnapshot camera_status(std::string_view camera_id) const noexcept;
    [[nodiscard]] Result<ClockTimeMapping> monotonic_to_utc(
        std::int64_t monotonic_ns) const noexcept;
    [[nodiscard]] Result<ClockTimeMapping> utc_to_monotonic(std::int64_t utc_ns) const noexcept;

    /// Internal construction hook; Impl is private so only TimeSyncRuntime can call it.
    explicit TimeSyncRuntime(std::unique_ptr<Impl> impl) noexcept;

  private:
    std::unique_ptr<Impl> impl_;
};

} // namespace paperbreak::time
