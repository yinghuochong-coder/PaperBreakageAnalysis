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

struct UplinkConfigurationValue final
{
    bool enabled{};
    std::string server_url;
    std::uint32_t heartbeat_seconds{5U};
    std::uint32_t chunk_bytes{1024U * 1024U};
    std::uint32_t io_timeout_ms{10000U};
    std::uint32_t upload_limit_mibps{20U};
    std::string credential_reference;
    std::string certificate_reference;
    bool operator==(const UplinkConfigurationValue&) const = default;
};

struct UplinkClientSnapshot final
{
    ipc::ClientConnectionSnapshot connection;
    UplinkConfigurationValue configuration;
    UplinkConfigurationValue effective_configuration;
    std::uint64_t stored_config_revision{};
    std::uint64_t effective_config_revision{};
    std::vector<std::string> pending_restart_paths;
    bool stale{true};
    bool operation_pending{};
    std::optional<Error> error;
};

using UplinkClientObserver = std::function<void(const UplinkClientSnapshot&)>;

class UplinkClient final
{
  public:
    explicit UplinkClient(UplinkClientObserver observer = {}, ipc::IpcClientOptions options = {});
    ~UplinkClient();
    UplinkClient(const UplinkClient&) = delete;
    UplinkClient& operator=(const UplinkClient&) = delete;

    [[nodiscard]] Result<void> start();
    void stop() noexcept;
    void refresh();
    [[nodiscard]] Result<void> update_configuration(UplinkConfigurationValue value);
    [[nodiscard]] const UplinkClientSnapshot& snapshot() const noexcept;

  private:
    void connection_changed(const ipc::ClientConnectionSnapshot& connection);
    void config_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void update_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void notify() const noexcept;

    UplinkClientObserver observer_;
    UplinkClientSnapshot snapshot_;
    std::unique_ptr<ipc::IpcClient> client_;
    std::optional<ipc::ClientRequestHandle> config_request_;
    std::optional<ipc::ClientRequestHandle> update_request_;
};

} // namespace paperbreak::console
