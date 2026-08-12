#pragma once

#include "paperbreak/camera/control.hpp"
#include "paperbreak/config/basic_config.hpp"
#include "paperbreak/logging/logging.hpp"
#include "paperbreak/service/runtime.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace paperbreak::service
{

[[nodiscard]] camera::CameraParameterSnapshot configured_camera_parameters(
    const config::CameraConfig& configuration);

/// Connects, configures and starts enabled camera slots during service startup.
/// Slot failures are logged and isolated so operators retain IPC access for recovery.
class CameraStartupLifecycleComponent final : public ILifecycleComponent
{
  public:
    CameraStartupLifecycleComponent(std::shared_ptr<camera::CameraControlRuntime> cameras,
                                    std::vector<config::CameraConfig> configurations,
                                    config::AcquisitionConfig acquisition,
                                    std::shared_ptr<logging::LoggingRuntime> logging);

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] ShutdownPhase shutdown_phase() const noexcept override;
    [[nodiscard]] Result<void> start(std::stop_token startup_stop_token) override;
    [[nodiscard]] Result<void> request_stop(StopReason reason) override;
    [[nodiscard]] Result<void> join(std::chrono::steady_clock::time_point deadline) override;

  private:
    [[nodiscard]] Result<void> start_slot(const config::CameraConfig& configuration,
                                          std::string& failed_stage);
    void log_attempt(const config::CameraConfig& configuration, std::string_view stage,
                     std::uint32_t attempt, bool final,
                     const Error* error = nullptr) const noexcept;

    std::shared_ptr<camera::CameraControlRuntime> cameras_;
    std::vector<config::CameraConfig> configurations_;
    config::AcquisitionConfig acquisition_;
    std::shared_ptr<logging::LoggingRuntime> logging_;
    std::mutex managed_mutex_;
    std::vector<std::string> managed_camera_ids_;
};

} // namespace paperbreak::service
