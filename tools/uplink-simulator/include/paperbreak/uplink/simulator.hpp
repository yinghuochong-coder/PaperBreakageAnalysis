#pragma once

#include "paperbreak/common/result.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace paperbreak::uplink::simulator
{

inline constexpr std::size_t maximum_devices = 16U;
inline constexpr std::size_t storage_queue_capacity = 128U;
inline constexpr std::size_t pending_response_capacity = 32U;
inline constexpr std::size_t command_queue_capacity_per_device = 64U;

struct Options final
{
    std::string listen_address{"0.0.0.0"};
    std::uint16_t port{18080U};
    std::filesystem::path workspace;
    std::size_t maximum_device_count{maximum_devices};
    std::uint64_t workspace_limit_bytes{20ULL * 1024ULL * 1024ULL * 1024ULL};
    std::optional<std::filesystem::path> scenario_path;
};

struct FaultProfile final
{
    bool reject_connections{};
    bool disconnect_websockets{};
    bool duplicate_acknowledgements{};
    bool replay_commands{};
    bool force_checksum_mismatch{};
    std::uint32_t response_delay_ms{};
    std::uint32_t fail_next_requests{};
    std::optional<std::uint32_t> disconnect_after_chunk;
};

struct DeviceSnapshot final
{
    std::string machine_id;
    std::string production_line_id;
    std::string software_version;
    bool connected{};
    std::string last_seen;
    std::size_t received_messages{};
    std::size_t alarm_count{};
    std::string last_message_type;
    std::size_t received_previews{};
    std::size_t overwritten_previews{};
    std::size_t event_count{};
    std::size_t upload_count{};
    std::size_t pending_commands{};
    std::vector<std::string> capabilities;
    std::string preview_camera_id;
    std::vector<std::byte> latest_preview_jpeg;
};

struct UploadSnapshot final
{
    std::string upload_id;
    std::string machine_id;
    std::string event_id;
    std::string logical_file_id;
    std::string state;
    std::uint64_t received_bytes{};
    std::uint64_t total_bytes{};
};

struct Snapshot final
{
    bool running{};
    std::string listen_address;
    std::uint16_t port{};
    std::filesystem::path workspace;
    std::uint64_t workspace_used_bytes{};
    std::uint64_t workspace_limit_bytes{};
    std::size_t storage_queue_depth{};
    std::size_t storage_queue_high_watermark{};
    std::size_t rejected_storage_tasks{};
    std::vector<DeviceSnapshot> devices;
    std::vector<UploadSnapshot> uploads;
    std::vector<std::string> recent_logs;
};

struct CommandRequest final
{
    std::string command_id;
    std::string machine_id;
    std::string command_type;
    std::string deadline;
    std::string payload_json{"{}"};
    bool operator_confirmed{};
};

class Runtime final
{
  public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    [[nodiscard]] Result<void> start(Options options);
    void stop(std::chrono::milliseconds timeout = std::chrono::seconds{10}) noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] Snapshot snapshot() const;
    [[nodiscard]] Result<void> enqueue_command(CommandRequest command);
    [[nodiscard]] Result<void> set_fault_profile(std::string machine_id, FaultProfile profile);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/// Parses the versioned fault scenario JSON shared by GUI and headless mode.
[[nodiscard]] Result<std::vector<std::pair<std::string, FaultProfile>>> parse_scenario(
    std::string_view json) noexcept;

} // namespace paperbreak::uplink::simulator
