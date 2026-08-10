#pragma once

#include "paperbreak/common/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace paperbreak::config
{

inline constexpr std::size_t config_max_bytes = 1024U * 1024U;
inline constexpr std::uint32_t config_schema_version = 2U;
inline constexpr std::size_t maximum_camera_count = 4U;

enum class PixelFormat
{
    mono8,
    mono10,
    mono12,
    bayer_rg8,
};

enum class TriggerMode
{
    continuous,
    hardware,
    software,
};

enum class LogLevel
{
    trace,
    debug,
    info,
    warning,
    error,
    critical,
};

struct RoiConfig final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t offset_x{};
    std::uint32_t offset_y{};
    bool operator==(const RoiConfig&) const = default;
};

struct SystemConfig final
{
    std::string machine_id;
    std::string production_line_id;
    std::uint32_t shutdown_timeout_ms{30000U};
    bool operator==(const SystemConfig&) const = default;
};

struct CameraConfig final
{
    std::string id;
    bool enabled{};
    std::string serial_number;
    std::string location;
    double exposure_us{};
    double gain_db{};
    double frame_rate{};
    RoiConfig roi;
    bool reverse_x{};
    bool reverse_y{};
    PixelFormat pixel_format{PixelFormat::mono8};
    TriggerMode trigger_mode{TriggerMode::continuous};
    std::string trigger_source;
    std::uint32_t trigger_delay_us{};
    std::uint32_t packet_size_bytes{1500U};
    std::uint32_t inter_packet_delay_ns{};
    bool operator==(const CameraConfig&) const = default;
};

struct AcquisitionConfig final
{
    std::uint32_t frame_pool_capacity{2048U};
    std::uint32_t queue_capacity{128U};
    std::uint32_t receive_timeout_ms{1000U};
    std::string thread_priority{"normal"};
    bool operator==(const AcquisitionConfig&) const = default;
};

struct PreviewConfig final
{
    bool enabled{true};
    double fps{3.0};
    std::uint32_t max_width{1280U};
    std::uint32_t max_height{720U};
    std::uint32_t jpeg_quality{80U};
    bool operator==(const PreviewConfig&) const = default;
};

struct AlgorithmConfig final
{
    bool enabled{};
    std::string type{"mock"};
    RoiConfig roi;
    double candidate_threshold{0.6};
    double confirmation_threshold{0.8};
    std::uint32_t consecutive_frames{3U};
    std::uint32_t cooldown_ms{1000U};
    std::string model_reference;
    std::string model_version;
    std::string device{"cpu"};
    bool debug_overlay{};
    bool operator==(const AlgorithmConfig&) const = default;
};

struct EventConfig final
{
    std::uint32_t pre_event_seconds{10U};
    std::uint32_t post_event_seconds{10U};
    std::uint32_t max_event_seconds{60U};
    std::uint32_t merge_gap_seconds{3U};
    std::uint32_t key_frame_count{7U};
    bool save_raw{true};
    bool generate_preview_video{};
    std::string upload_policy{"confirmed"};
    std::uint32_t retention_days{30U};
    bool operator==(const EventConfig&) const = default;
};

struct StorageConfig final
{
    std::string event_root;
    std::string cache_root;
    bool rolling_cache_enabled{};
    std::uint32_t maximum_cache_storage_gib{1000U};
    std::uint32_t rolling_cache_write_limit_mibps{600U};
    std::uint32_t rolling_cache_io_timeout_ms{10000U};
    std::uint32_t warning_free_space_gib{200U};
    std::uint32_t critical_free_space_gib{100U};
    std::uint32_t stop_free_space_gib{20U};
    std::uint32_t maximum_event_storage_gib{1000U};
    bool operator==(const StorageConfig&) const = default;
};

struct UplinkConfig final
{
    bool enabled{};
    std::string server_url;
    std::uint32_t heartbeat_seconds{5U};
    std::uint32_t chunk_bytes{1024U * 1024U};
    std::uint32_t io_timeout_ms{10000U};
    std::uint32_t upload_limit_mibps{20U};
    std::string credential_reference;
    std::string certificate_reference;
    bool operator==(const UplinkConfig&) const = default;
};

struct PlantIoConfig final
{
    bool enabled{};
    std::string adapter_type{"disabled"};
    std::string endpoint;
    std::string credential_reference;
    std::uint32_t poll_interval_ms{1000U};
    bool operator==(const PlantIoConfig&) const = default;
};

struct LoggingConfig final
{
    LogLevel level{LogLevel::info};
    std::string directory;
    std::uint32_t retention_days{30U};
    std::uint32_t maximum_file_size_mib{10U};
    std::uint32_t maximum_files_per_day{5U};
    std::uint32_t queue_capacity{8192U};
    bool operator==(const LoggingConfig&) const = default;
};

struct HealthConfig final
{
    std::uint32_t sample_interval_ms{1000U};
    double cpu_warning_percent{85.0};
    double memory_warning_percent{85.0};
    double dropped_frame_warning_ratio{0.01};
    std::uint32_t heartbeat_stale_seconds{30U};
    bool operator==(const HealthConfig&) const = default;
};

struct EdgeConfig final
{
    std::uint32_t config_schema_version{config_schema_version};
    std::uint64_t config_revision{1U};
    std::string modified_at;
    SystemConfig system;
    std::vector<CameraConfig> cameras;
    AcquisitionConfig acquisition;
    PreviewConfig preview;
    AlgorithmConfig algorithm;
    EventConfig event;
    StorageConfig storage;
    UplinkConfig uplink;
    PlantIoConfig plant_io;
    LoggingConfig logging;
    HealthConfig health;
    bool operator==(const EdgeConfig&) const = default;
};

struct BasicConfigInfo final
{
    std::uint32_t schema_version{};
    std::uint64_t config_revision{};
    std::size_t file_size_bytes{};
};

/// Parses strict schema v2 and resolves/validates paths against config_directory.
[[nodiscard]] Result<EdgeConfig> parse_config(
    std::string_view contents, const std::filesystem::path& config_directory) noexcept;
[[nodiscard]] std::string serialize_config(const EdgeConfig& config);

/// Performs the complete M1-03 validation while retaining the M1-01 CLI surface.
[[nodiscard]] Result<BasicConfigInfo> validate_basic_config(
    const std::filesystem::path& path, std::size_t maximum_bytes = config_max_bytes) noexcept;

[[nodiscard]] std::vector<std::string> changed_config_paths(const EdgeConfig& current,
                                                            const EdgeConfig& candidate);
[[nodiscard]] bool is_restart_required_path(std::string_view json_pointer) noexcept;

} // namespace paperbreak::config
