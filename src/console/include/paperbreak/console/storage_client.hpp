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

struct StorageConfigurationValue final
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
    bool operator==(const StorageConfigurationValue&) const = default;
};

struct StorageClientSnapshot final
{
    ipc::ClientConnectionSnapshot connection;
    StorageConfigurationValue configuration;
    StorageConfigurationValue effective_configuration;
    std::uint64_t stored_config_revision{};
    std::uint64_t effective_config_revision{};
    std::vector<std::string> pending_restart_paths;
    bool stale{true};
    bool operation_pending{};
    std::optional<Error> error;
};

using StorageClientObserver = std::function<void(const StorageClientSnapshot&)>;

class StorageClient final
{
  public:
    explicit StorageClient(StorageClientObserver observer = {}, ipc::IpcClientOptions options = {});
    ~StorageClient();
    StorageClient(const StorageClient&) = delete;
    StorageClient& operator=(const StorageClient&) = delete;

    [[nodiscard]] Result<void> start();
    void stop() noexcept;
    void refresh();
    [[nodiscard]] Result<void> update_configuration(StorageConfigurationValue value);
    [[nodiscard]] const StorageClientSnapshot& snapshot() const noexcept;

  private:
    void connection_changed(const ipc::ClientConnectionSnapshot& connection);
    void config_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void update_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void notify() const noexcept;

    StorageClientObserver observer_;
    StorageClientSnapshot snapshot_;
    std::unique_ptr<ipc::IpcClient> client_;
    std::optional<ipc::ClientRequestHandle> config_request_;
    std::optional<ipc::ClientRequestHandle> update_request_;
};

} // namespace paperbreak::console
