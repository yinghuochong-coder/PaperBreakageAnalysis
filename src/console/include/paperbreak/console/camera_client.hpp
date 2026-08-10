#pragma once

#include "paperbreak/common/result.hpp"
#include "paperbreak/ipc/client.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QTimer;

namespace paperbreak::console
{

struct CameraRoiValue final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t offset_x{};
    std::uint32_t offset_y{};
};

struct CameraIntegerRangeValue final
{
    std::uint32_t minimum{};
    std::uint32_t maximum{};
    std::uint32_t increment{1U};
};

struct CameraRoiCapabilitiesValue final
{
    std::uint32_t sensor_width{};
    std::uint32_t sensor_height{};
    CameraIntegerRangeValue width;
    CameraIntegerRangeValue height;
    CameraIntegerRangeValue offset_x;
    CameraIntegerRangeValue offset_y;
};

struct CameraParameterValue final
{
    std::optional<double> exposure_us;
    std::optional<double> gain_db;
    std::optional<double> frame_rate;
    std::optional<CameraRoiValue> roi;
    bool reverse_x{};
    bool reverse_y{};
    std::string pixel_format;
    std::string trigger_mode;
    std::string trigger_source;
    std::optional<std::uint32_t> trigger_delay_us;
    std::optional<std::uint32_t> packet_size_bytes;
    std::optional<std::uint32_t> inter_packet_delay_ns;
};

struct CameraClientItem final
{
    std::string id;
    std::string location;
    std::string state;
    std::string serial;
    std::string model;
    std::string ip;
    bool enabled{};
    std::uint64_t saved_config_revision{};
    CameraParameterValue saved;
    CameraParameterValue actual;
    std::optional<CameraRoiCapabilitiesValue> roi_capabilities;
};

struct CameraDiscoveredDevice final
{
    std::string model;
    std::string serial;
    std::string ip;
    std::string network_interface;
    bool exclusive_access_available{};
};

struct CameraOperationResult final
{
    std::string operation;
    std::string camera_id;
    bool pending{};
    bool succeeded{};
    bool saved{};
    bool dispatched{};
    bool applied{};
    bool restart_required{};
    bool outcome_unknown{};
    bool confirmed_by_snapshot{};
    std::string message;
};

struct CameraClientSnapshot final
{
    ipc::ClientConnectionSnapshot connection;
    std::vector<CameraClientItem> cameras;
    std::vector<CameraDiscoveredDevice> discovered_devices;
    std::uint64_t stored_config_revision{};
    bool topology_restart_required{};
    bool stale{true};
    std::optional<Error> error;
    std::optional<CameraOperationResult> operation;
};

using CameraClientObserver = std::function<void(const CameraClientSnapshot&)>;

class CameraClient final
{
  public:
    explicit CameraClient(
        CameraClientObserver observer = {}, ipc::IpcClientOptions options = {},
        std::chrono::milliseconds control_operation_timeout = std::chrono::seconds{30});
    ~CameraClient();
    CameraClient(const CameraClient&) = delete;
    CameraClient& operator=(const CameraClient&) = delete;

    [[nodiscard]] Result<void> start();
    void stop() noexcept;
    void refresh();
    [[nodiscard]] Result<void> discover();
    [[nodiscard]] Result<void> bind(std::string camera_id, std::string serial_number,
                                    std::string location, std::uint64_t expected_revision);
    [[nodiscard]] Result<void> control(std::string command, std::string camera_id);
    [[nodiscard]] Result<void> update_config(std::string camera_id, std::uint64_t expected_revision,
                                             const CameraParameterValue& parameters);
    [[nodiscard]] const CameraClientSnapshot& snapshot() const noexcept;

  private:
    void connection_changed(const ipc::ClientConnectionSnapshot& connection);
    void list_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    [[nodiscard]] Result<void> send_operation(std::string command, std::string camera_id,
                                              std::string payload_json);
    void operation_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void notify() const noexcept;

    CameraClientObserver observer_;
    CameraClientSnapshot snapshot_;
    std::unique_ptr<ipc::IpcClient> client_;
    std::unique_ptr<QTimer> reconciliation_timer_;
    std::chrono::milliseconds control_operation_timeout_;
    std::optional<ipc::ClientRequestHandle> list_request_;
    std::optional<ipc::ClientRequestHandle> operation_request_;
};

} // namespace paperbreak::console
