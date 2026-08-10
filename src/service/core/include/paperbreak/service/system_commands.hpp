#pragma once

#include "paperbreak/camera/control.hpp"
#include "paperbreak/config/config_repository.hpp"
#include "paperbreak/ipc/server.hpp"
#include "paperbreak/logging/logging.hpp"
#include "paperbreak/monitoring/monitoring.hpp"
#include "paperbreak/pipeline/preview.hpp"
#include "paperbreak/service/event_runtime.hpp"
#include "paperbreak/service/runtime.hpp"
#include "paperbreak/storage/event_inspector.hpp"
#include "paperbreak/storage/metadata_database.hpp"
#include "paperbreak/uplink/runtime.hpp"

#include <atomic>
#include <filesystem>
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
    SystemCommandService(
        config::ConfigRepository& repository, std::shared_ptr<ServiceStatusStore> status,
        std::shared_ptr<monitoring::MetricRegistry> metrics = {},
        std::shared_ptr<monitoring::AlarmRegistry> alarms = {},
        std::shared_ptr<logging::LoggingRuntime> logging = {},
        std::filesystem::path config_directory = {},
        std::shared_ptr<pipeline::PreviewRuntime> preview = {},
        std::shared_ptr<camera::CameraControlRuntime> cameras = {},
        std::shared_ptr<EventRuntime> event_runtime = {},
        std::shared_ptr<storage::EventMetadataDatabase> event_database = {},
        std::shared_ptr<storage::EventInspector> event_inspector = {},
        std::function<void(const storage::EventMetadataRecord&)> event_review_observer = {});

    [[nodiscard]] Result<ipc::CommandResponse> handle(const ipc::RequestMessage& request,
                                                      const ipc::PeerIdentity& peer,
                                                      std::stop_token stop_token) override;
    [[nodiscard]] ipc::IRequestHandler::ExecutionClass execution_class(
        const ipc::RequestMessage& request) const noexcept override;

    /// Executes one validated Uplink v1 command through the same service-side dispatcher used by
    /// local IPC. Mutating commands require operator confirmation and an available audit logger.
    [[nodiscard]] Result<std::string> handle_uplink_command(const uplink::RemoteCommand& command,
                                                            std::stop_token stop_token);

  private:
    [[nodiscard]] Result<ipc::CommandResponse> handle_with_source(
        const ipc::RequestMessage& request, const ipc::PeerIdentity& peer,
        std::stop_token stop_token, config::ConfigChangeSource config_source);

    config::ConfigRepository& repository_;
    std::shared_ptr<ServiceStatusStore> status_;
    std::shared_ptr<monitoring::MetricRegistry> metrics_;
    std::shared_ptr<monitoring::AlarmRegistry> alarms_;
    std::shared_ptr<logging::LoggingRuntime> logging_;
    std::filesystem::path config_directory_;
    std::shared_ptr<pipeline::PreviewRuntime> preview_;
    std::shared_ptr<camera::CameraControlRuntime> cameras_;
    std::shared_ptr<EventRuntime> event_runtime_;
    std::shared_ptr<storage::EventMetadataDatabase> event_database_;
    std::shared_ptr<storage::EventInspector> event_inspector_;
    std::function<void(const storage::EventMetadataRecord&)> event_review_observer_;
};

} // namespace paperbreak::service
