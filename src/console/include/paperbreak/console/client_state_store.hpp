#pragma once

#include "paperbreak/common/result.hpp"
#include "paperbreak/ipc/client.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace paperbreak::console
{

struct ServiceStatusSummary final
{
    std::string service_state;
    std::string machine_id;
    std::string service_timestamp;
    bool accepting_writes{};
    std::uint64_t generation{};
};

struct ClientStateSnapshot final
{
    ipc::ClientConnectionSnapshot connection;
    std::optional<ServiceStatusSummary> service_status;
    bool service_status_stale{true};
    std::optional<Error> synchronization_error;
};

using ClientStateObserver = std::function<void(const ClientStateSnapshot&)>;

class ClientStateStore final
{
  public:
    // The store is thread-confined to the Qt event-loop thread that constructs it.
    explicit ClientStateStore(ClientStateObserver observer = {},
                              ipc::IpcClientOptions options = {});
    ~ClientStateStore();

    ClientStateStore(const ClientStateStore&) = delete;
    ClientStateStore& operator=(const ClientStateStore&) = delete;
    ClientStateStore(ClientStateStore&&) = delete;
    ClientStateStore& operator=(ClientStateStore&&) = delete;

    [[nodiscard]] Result<void> start();
    void stop() noexcept;
    [[nodiscard]] const ClientStateSnapshot& snapshot() const noexcept;

  private:
    void connection_changed(const ipc::ClientConnectionSnapshot& connection);
    void push_received(std::uint64_t generation, const ipc::PushMessage& push);
    void synchronize_status(std::uint64_t generation);
    void status_completed(ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result);
    void notify() const noexcept;

    ClientStateObserver observer_;
    ClientStateSnapshot snapshot_;
    std::unique_ptr<ipc::IpcClient> client_;
    std::optional<ipc::ClientRequestHandle> status_request_;
};

} // namespace paperbreak::console
