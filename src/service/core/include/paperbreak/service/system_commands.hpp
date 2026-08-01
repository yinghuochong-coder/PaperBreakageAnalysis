#pragma once

#include "paperbreak/config/config_repository.hpp"
#include "paperbreak/ipc/server.hpp"
#include "paperbreak/service/runtime.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace paperbreak::service
{

struct ServiceStatusSnapshot final
{
    ServiceState state{ServiceState::created};
    bool accepting_writes{};
    std::string started_at;
};

class ServiceStatusStore final
{
  public:
    void set_state(ServiceState state);
    [[nodiscard]] ServiceStatusSnapshot snapshot() const;

  private:
    std::atomic<ServiceState> state_{ServiceState::created};
    mutable std::mutex mutex_;
    std::string started_at_;
};

class SystemCommandService final : public ipc::IRequestHandler
{
  public:
    SystemCommandService(config::ConfigRepository& repository,
                         std::shared_ptr<ServiceStatusStore> status);

    [[nodiscard]] Result<ipc::CommandResponse> handle(const ipc::RequestMessage& request,
                                                      const ipc::PeerIdentity& peer,
                                                      std::stop_token stop_token) override;

  private:
    config::ConfigRepository& repository_;
    std::shared_ptr<ServiceStatusStore> status_;
};

} // namespace paperbreak::service
