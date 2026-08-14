#include "paperbreak/time/time_model.hpp"

#include <limits>
#include <utility>

namespace paperbreak::time
{
namespace
{
Error time_error(std::string code, std::string reason)
{
    auto error = make_error(std::move(code), Severity::error, "时间模型无效", "time",
                            "time.validate", false);
    error.details.push_back({"reason", std::move(reason)});
    return error;
}

bool corrected_state(const SyncState state) noexcept
{
    return state == SyncState::synced || state == SyncState::syncing ||
           state == SyncState::degraded;
}

bool known_source(const ClockSource source) noexcept
{
    return source == ClockSource::ptp_hardware || source == ClockSource::ptp_software ||
           source == ClockSource::ntp || source == ClockSource::offset_model ||
           source == ClockSource::receive_clock;
}

bool source_allows_state(const ClockSource source, const SyncState state) noexcept
{
    return state != SyncState::synced ||
           (source != ClockSource::offset_model && source != ClockSource::receive_clock);
}

bool checked_add(const std::int64_t left, const std::int64_t right, std::int64_t& output) noexcept
{
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right))
    {
        return false;
    }
    output = left + right;
    return true;
}

struct UnsignedDivision final
{
    std::uint64_t quotient{};
    std::uint64_t remainder{};
};

/// Computes (value * multiplier) / divisor without a wider integer type.
UnsignedDivision multiply_divide(const std::uint64_t value, const std::uint32_t multiplier,
                                 const std::uint64_t divisor) noexcept
{
    UnsignedDivision result;
    for (int bit = 31; bit >= 0; --bit)
    {
        const bool doubled_wraps_divisor = result.remainder >= divisor - result.remainder;
        if (doubled_wraps_divisor)
        {
            result.remainder -= divisor - result.remainder;
        }
        else
        {
            result.remainder += result.remainder;
        }
        result.quotient = result.quotient * 2U + (doubled_wraps_divisor ? 1U : 0U);

        if ((multiplier & (std::uint32_t{1U} << bit)) != 0U)
        {
            const bool addition_wraps_divisor = result.remainder >= divisor - value;
            if (addition_wraps_divisor)
            {
                result.remainder -= divisor - value;
            }
            else
            {
                result.remainder += value;
            }
            result.quotient += addition_wraps_divisor ? 1U : 0U;
        }
    }
    return result;
}

std::optional<std::int64_t> camera_delta_nanoseconds(const std::uint64_t ticks,
                                                     const std::uint64_t anchor_ticks,
                                                     const std::uint64_t frequency_hz) noexcept
{
    constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000ULL;
    const bool negative = ticks < anchor_ticks;
    const auto magnitude_ticks = negative ? anchor_ticks - ticks : ticks - anchor_ticks;
    const auto whole_seconds = magnitude_ticks / frequency_hz;
    const auto remaining_ticks = magnitude_ticks % frequency_hz;
    if (whole_seconds > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
                            nanoseconds_per_second)
    {
        return std::nullopt;
    }
    const auto fractional = multiply_divide(remaining_ticks, 1'000'000'000U, frequency_hz);
    const auto whole_nanoseconds = whole_seconds * nanoseconds_per_second;
    if (fractional.quotient >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - whole_nanoseconds)
    {
        return std::nullopt;
    }
    const auto magnitude = static_cast<std::int64_t>(whole_nanoseconds + fractional.quotient);
    return negative ? -magnitude : magnitude;
}

template <typename Clock>
std::optional<std::int64_t> time_point_nanoseconds(
    const std::chrono::time_point<Clock> value) noexcept
{
    constexpr std::int64_t nanoseconds_per_second = 1'000'000'000LL;
    const auto duration = value.time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    const auto remainder = duration - seconds;
    const auto remainder_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(remainder).count();
    const auto second_count = seconds.count();
    if (second_count > std::numeric_limits<std::int64_t>::max() / nanoseconds_per_second ||
        second_count < std::numeric_limits<std::int64_t>::min() / nanoseconds_per_second)
    {
        return std::nullopt;
    }
    std::int64_t result{};
    if (!checked_add(static_cast<std::int64_t>(second_count) * nanoseconds_per_second,
                     static_cast<std::int64_t>(remainder_ns), result))
    {
        return std::nullopt;
    }
    return result;
}
} // namespace

std::string_view clock_source_name(const ClockSource source) noexcept
{
    switch (source)
    {
    case ClockSource::ptp_hardware:
        return "PTP_HARDWARE";
    case ClockSource::ptp_software:
        return "PTP_SOFTWARE";
    case ClockSource::ntp:
        return "NTP";
    case ClockSource::offset_model:
        return "OFFSET_MODEL";
    case ClockSource::receive_clock:
        return "RECEIVE_CLOCK";
    case ClockSource::unknown:
        return "UNKNOWN";
    }
    return {};
}

std::string_view sync_state_name(const SyncState state) noexcept
{
    switch (state)
    {
    case SyncState::synced:
        return "SYNCED";
    case SyncState::syncing:
        return "SYNCING";
    case SyncState::degraded:
        return "DEGRADED";
    case SyncState::unsynced:
        return "UNSYNCED";
    case SyncState::unknown:
        return "UNKNOWN";
    }
    return {};
}

Result<void> validate_frame_time_metadata(const FrameTimeMetadata& metadata)
{
    if (metadata.camera_timestamp_ticks.has_value() !=
        metadata.camera_timestamp_frequency_hz.has_value())
    {
        return Result<void>::failure(
            time_error("TIME_MODEL_INVALID", "camera-timestamp-pair-mismatch"));
    }
    if (metadata.camera_timestamp_frequency_hz && *metadata.camera_timestamp_frequency_hz == 0U)
    {
        return Result<void>::failure(
            time_error("TIME_MODEL_INVALID", "camera-timestamp-frequency-zero"));
    }
    if (metadata.uncertainty_ns && *metadata.uncertainty_ns < 0)
    {
        return Result<void>::failure(time_error("TIME_MODEL_INVALID", "negative-uncertainty"));
    }
    if (metadata.corrected_capture_utc_ns)
    {
        if (metadata.clock_model_revision == 0U || !metadata.uncertainty_ns ||
            !corrected_state(metadata.sync_state) || !known_source(metadata.clock_source) ||
            !source_allows_state(metadata.clock_source, metadata.sync_state))
        {
            return Result<void>::failure(
                time_error("TIME_MODEL_INVALID", "inconsistent-corrected-time"));
        }
    }
    else if (metadata.sync_state != SyncState::unsynced &&
             metadata.sync_state != SyncState::unknown)
    {
        return Result<void>::failure(
            time_error("TIME_MODEL_INVALID", "missing-corrected-time-for-state"));
    }
    return Result<void>::success();
}

Result<void> validate_clock_model_snapshot(const ClockModelSnapshot& snapshot)
{
    if (snapshot.model_revision == 0U)
        return Result<void>::failure(time_error("TIME_MODEL_INVALID", "zero-model-revision"));
    if (!known_source(snapshot.clock_source) || !corrected_state(snapshot.sync_state) ||
        !source_allows_state(snapshot.clock_source, snapshot.sync_state))
        return Result<void>::failure(time_error("TIME_MODEL_INVALID", "unknown-model-quality"));
    if (snapshot.anchor_camera_ticks.has_value() !=
        snapshot.camera_timestamp_frequency_hz.has_value())
        return Result<void>::failure(
            time_error("TIME_MODEL_INVALID", "camera-anchor-pair-mismatch"));
    if (snapshot.camera_timestamp_frequency_hz && *snapshot.camera_timestamp_frequency_hz == 0U)
        return Result<void>::failure(time_error("TIME_MODEL_INVALID", "model-frequency-zero"));
    if (!snapshot.uncertainty_ns || *snapshot.uncertainty_ns < 0)
        return Result<void>::failure(time_error("TIME_MODEL_INVALID", "invalid-uncertainty"));
    if (snapshot.maximum_observed_offset_ns && *snapshot.maximum_observed_offset_ns < 0)
        return Result<void>::failure(
            time_error("TIME_MODEL_INVALID", "negative-maximum-observed-offset"));
    return Result<void>::success();
}

void ImmutableClockModelStore::publish(std::shared_ptr<const ClockModelSnapshot> snapshot) noexcept
{
    snapshot_.store(std::move(snapshot), std::memory_order_release);
}

void ImmutableClockModelStore::clear() noexcept
{
    snapshot_.store({}, std::memory_order_release);
}

std::shared_ptr<const ClockModelSnapshot> ImmutableClockModelStore::load() const noexcept
{
    return snapshot_.load(std::memory_order_acquire);
}

FrameTimeBuildResult build_frame_time_metadata(
    const std::optional<std::uint64_t> camera_timestamp_ticks,
    const std::optional<std::uint64_t> camera_timestamp_frequency_hz,
    const std::int64_t received_monotonic_ns, const std::int64_t received_utc_ns,
    const std::shared_ptr<const ClockModelSnapshot>& model) noexcept
{
    FrameTimeBuildResult result;
    result.metadata.received_monotonic_ns = received_monotonic_ns;
    result.metadata.received_utc_ns = received_utc_ns;

    if (camera_timestamp_ticks.has_value() != camera_timestamp_frequency_hz.has_value() ||
        (camera_timestamp_frequency_hz && *camera_timestamp_frequency_hz == 0U))
    {
        result.status = FrameTimeBuildStatus::invalid_raw_timestamp;
        return result;
    }
    result.metadata.camera_timestamp_ticks = camera_timestamp_ticks;
    result.metadata.camera_timestamp_frequency_hz = camera_timestamp_frequency_hz;
    if (!model)
    {
        result.status = FrameTimeBuildStatus::no_model;
        return result;
    }
    if (!validate_clock_model_snapshot(*model))
    {
        result.status = FrameTimeBuildStatus::invalid_model;
        return result;
    }
    if (!camera_timestamp_ticks || !model->anchor_camera_ticks ||
        !model->camera_timestamp_frequency_hz)
    {
        result.status = FrameTimeBuildStatus::missing_camera_timestamp;
        return result;
    }
    if (*camera_timestamp_frequency_hz != *model->camera_timestamp_frequency_hz)
    {
        result.status = FrameTimeBuildStatus::frequency_mismatch;
        return result;
    }
    if (received_monotonic_ns < model->valid_from_monotonic_ns)
    {
        result.status = FrameTimeBuildStatus::model_not_yet_valid;
        return result;
    }
    const auto delta =
        camera_delta_nanoseconds(*camera_timestamp_ticks, *model->anchor_camera_ticks,
                                 *model->camera_timestamp_frequency_hz);
    std::int64_t corrected{};
    if (!delta || !checked_add(model->anchor_utc_ns, *delta, corrected))
    {
        result.status = FrameTimeBuildStatus::arithmetic_overflow;
        return result;
    }

    result.metadata.corrected_capture_utc_ns = corrected;
    result.metadata.clock_source = model->clock_source;
    result.metadata.clock_offset_ns = model->offset_ns;
    result.metadata.uncertainty_ns = model->uncertainty_ns;
    result.metadata.sync_state = model->sync_state;
    result.metadata.clock_model_revision = model->model_revision;
    result.status = FrameTimeBuildStatus::corrected;
    return result;
}

std::optional<std::int64_t> monotonic_time_to_nanoseconds(
    const std::chrono::steady_clock::time_point value) noexcept
{
    return time_point_nanoseconds(value);
}

std::optional<std::int64_t> utc_time_to_nanoseconds(
    const std::chrono::system_clock::time_point value) noexcept
{
    return time_point_nanoseconds(value);
}

} // namespace paperbreak::time
