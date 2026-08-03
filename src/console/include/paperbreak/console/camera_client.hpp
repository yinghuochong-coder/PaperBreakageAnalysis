#pragma once

#include "paperbreak/common/result.hpp"
#include "paperbreak/ipc/client.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace paperbreak::console
{

struct CameraRoiValue final
{
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t offset_x{};
    std::uint32_t offset_y{};
};

struct CameraParameterValue final
{
    std::optional<double> exposure_us;
    std::optional<double> gain_db;
    std::optional<double> frame_rate;
    std::optional<CameraRoiValue> roi;
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
};

struct CameraDiscoveredDevice final
{
    std::string model;
    std::string serial;
    std::string ip;
    std::string transport_id;
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
    std::string message;
};

struct CameraClientSnapshot final
{
    ipc::ClientConnectionSnapshot connection;
    std::vector<CameraClientItem> cameras;
    std::vector<CameraDiscoveredDevice> discovered_devices;
    bool stale{true};
    std::optional<Error> error;
    std::optional<CameraOperationResult> operation;
};

using CameraClientObserver = std::function<void(const CameraClientSnapshot&)>;

class CameraClient final
{
  public:
    explicit CameraClient(CameraClientObserver observer = {}, ipc::IpcClientOptions options = {});
    ~CameraClient();
    CameraClient(const CameraClient&) = delete;
    CameraClient& operator=(const CameraClient&) = delete;

    [[nodiscard]] Result<void> start();
    void stop() noexcept;
    void refresh();
    [[nodiscard]] Result<void> discover();
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
    std::optional<ipc::ClientRequestHandle> list_request_;
    std::optional<ipc::ClientRequestHandle> operation_request_;
};

} // namespace paperbreak::console
