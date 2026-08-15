#pragma once

#include "paperbreak/common/result.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace paperbreak::time
{

enum class ClockSource : std::uint8_t
{
    unknown = 0U,
    ptp_hardware = 1U,
    ptp_software = 2U,
    ntp = 3U,
    offset_model = 4U,
    receive_clock = 5U,
};

enum class SyncState : std::uint8_t
{
    unknown = 0U,
    synced = 1U,
    syncing = 2U,
    degraded = 3U,
    unsynced = 4U,
};

[[nodiscard]] std::string_view clock_source_name(ClockSource source) noexcept;
[[nodiscard]] std::string_view sync_state_name(SyncState state) noexcept;

struct FrameTimeMetadata final
{
    std::optional<std::uint64_t> camera_timestamp_ticks;
    std::optional<std::uint64_t> camera_timestamp_frequency_hz;
    std::int64_t received_monotonic_ns{};
    std::int64_t received_utc_ns{};
    std::optional<std::int64_t> corrected_capture_utc_ns;
    ClockSource clock_source{ClockSource::unknown};
    std::optional<std::int64_t> clock_offset_ns;
    std::optional<std::int64_t> uncertainty_ns;
    SyncState sync_state{SyncState::unsynced};
    std::uint64_t clock_model_revision{};
    bool operator==(const FrameTimeMetadata&) const = default;
};

struct ClockModelSnapshot final
{
    std::uint64_t model_revision{};
    std::optional<std::string> camera_id;
    ClockSource clock_source{ClockSource::unknown};
    SyncState sync_state{SyncState::unknown};
    std::int64_t anchor_monotonic_ns{};
    std::int64_t anchor_utc_ns{};
    std::optional<std::uint64_t> anchor_camera_ticks;
    std::optional<std::uint64_t> camera_timestamp_frequency_hz;
    std::optional<std::int64_t> offset_ns;
    std::optional<std::int64_t> uncertainty_ns;
    std::optional<std::int64_t> maximum_observed_offset_ns;
    std::int64_t valid_from_monotonic_ns{};
    std::optional<std::int64_t> last_synchronized_utc_ns;
    std::optional<std::string> grandmaster_identity;
    std::optional<std::string> last_error_code;
    bool operator==(const ClockModelSnapshot&) const = default;
};

struct ClockSyncSnapshot final
{
    bool available{};
    std::optional<std::int64_t> current_utc_ns;
    ClockSource clock_source{ClockSource::unknown};
    SyncState sync_state{SyncState::unknown};
    std::optional<std::int64_t> offset_ns;
    bool offset_available{};
    std::optional<std::int64_t> uncertainty_ns;
    bool uncertainty_available{};
    std::optional<std::int64_t> maximum_observed_offset_ns;
    bool maximum_observed_offset_available{};
    std::optional<std::int64_t> last_synchronized_utc_ns;
    bool last_synchronized_utc_available{};
    std::optional<std::string> grandmaster_identity;
    bool grandmaster_available{};
    std::uint64_t model_revision{};
    std::optional<std::string> last_error_code;
    bool operator==(const ClockSyncSnapshot&) const = default;
};

struct ClockTimeMapping final
{
    std::int64_t mapped_time_ns{};
    std::shared_ptr<const ClockModelSnapshot> model;
};

[[nodiscard]] Result<void> validate_frame_time_metadata(const FrameTimeMetadata& metadata);
[[nodiscard]] Result<void> validate_clock_model_snapshot(const ClockModelSnapshot& snapshot);
[[nodiscard]] ClockSyncSnapshot build_clock_sync_snapshot(
    const std::shared_ptr<const ClockModelSnapshot>& model,
    std::int64_t current_monotonic_ns) noexcept;
[[nodiscard]] Result<ClockTimeMapping> map_monotonic_to_utc(
    std::int64_t monotonic_ns, const std::shared_ptr<const ClockModelSnapshot>& model) noexcept;
[[nodiscard]] Result<ClockTimeMapping> map_utc_to_monotonic(
    std::int64_t utc_ns, const std::shared_ptr<const ClockModelSnapshot>& model) noexcept;

/// Capacity-one immutable publication slot. Readers never wait and retain the loaded revision.
class ImmutableClockModelStore final
{
  public:
    ImmutableClockModelStore() = default;
    ImmutableClockModelStore(const ImmutableClockModelStore&) = delete;
    ImmutableClockModelStore& operator=(const ImmutableClockModelStore&) = delete;

    void publish(std::shared_ptr<const ClockModelSnapshot> snapshot) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::shared_ptr<const ClockModelSnapshot> load() const noexcept;

  private:
    std::atomic<std::shared_ptr<const ClockModelSnapshot>> snapshot_;
};

enum class FrameTimeBuildStatus : std::uint8_t
{
    corrected,
    no_model,
    invalid_raw_timestamp,
    missing_camera_timestamp,
    invalid_model,
    frequency_mismatch,
    model_not_yet_valid,
    arithmetic_overflow,
};

struct FrameTimeBuildResult final
{
    FrameTimeMetadata metadata;
    FrameTimeBuildStatus status{FrameTimeBuildStatus::no_model};
};

/// Builds one immutable frame value from exactly one already-published model snapshot.
[[nodiscard]] FrameTimeBuildResult build_frame_time_metadata(
    std::optional<std::uint64_t> camera_timestamp_ticks,
    std::optional<std::uint64_t> camera_timestamp_frequency_hz, std::int64_t received_monotonic_ns,
    std::int64_t received_utc_ns, const std::shared_ptr<const ClockModelSnapshot>& model) noexcept;

[[nodiscard]] std::optional<std::int64_t> monotonic_time_to_nanoseconds(
    std::chrono::steady_clock::time_point value) noexcept;
[[nodiscard]] std::optional<std::int64_t> utc_time_to_nanoseconds(
    std::chrono::system_clock::time_point value) noexcept;

} // namespace paperbreak::time
