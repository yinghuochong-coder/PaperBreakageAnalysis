#pragma once

#include "paperbreak/config/config_repository.hpp"
#include "paperbreak/ipc/server.hpp"
#include "paperbreak/logging/logging.hpp"
#include "paperbreak/monitoring/monitoring.hpp"
#include "paperbreak/service/runtime.hpp"

#include <atomic>
#include <functional>
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
    void set_observer(std::function<void(const ServiceStatusSnapshot&)> observer);

  private:
    std::atomic<ServiceState> state_{ServiceState::created};
    mutable std::mutex mutex_;
    std::string started_at_;
    std::function<void(const ServiceStatusSnapshot&)> observer_;
};

class SystemCommandService final : public ipc::IRequestHandler
{
  public:
    SystemCommandService(config::ConfigRepository& repository,
                         std::shared_ptr<ServiceStatusStore> status,
                         std::shared_ptr<monitoring::MetricRegistry> metrics = {},
                         std::shared_ptr<monitoring::AlarmRegistry> alarms = {},
                         std::shared_ptr<logging::LoggingRuntime> logging = {});

    [[nodiscard]] Result<ipc::CommandResponse> handle(const ipc::RequestMessage& request,
                                                      const ipc::PeerIdentity& peer,
                                                      std::stop_token stop_token) override;

  private:
    config::ConfigRepository& repository_;
    std::shared_ptr<ServiceStatusStore> status_;
    std::shared_ptr<monitoring::MetricRegistry> metrics_;
    std::shared_ptr<monitoring::AlarmRegistry> alarms_;
    std::shared_ptr<logging::LoggingRuntime> logging_;
};

} // namespace paperbreak::service
