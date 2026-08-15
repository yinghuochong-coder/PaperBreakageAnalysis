#pragma once

#include "paperbreak/camera/control.hpp"
#include "paperbreak/service/runtime.hpp"
#include "paperbreak/service/time_sync_monitor.hpp"
#include "paperbreak/time/time_sync_runtime.hpp"

#include <memory>
#include <string>

namespace paperbreak::service
{

class CameraControlClockProbe final : public time::ICameraClockProbe
{
  public:
    CameraControlClockProbe(std::string camera_id,
                            std::shared_ptr<camera::CameraControlRuntime> cameras,
                            std::int64_t degraded_uncertainty_ns);

    [[nodiscard]] std::string_view camera_id() const noexcept override;
    [[nodiscard]] Result<time::CameraClockProbeSample> sample(
        std::stop_token stop_token, std::chrono::steady_clock::time_point deadline) override;

  private:
    std::string camera_id_;
    std::shared_ptr<camera::CameraControlRuntime> cameras_;
    std::int64_t degraded_uncertainty_ns_{};
};

class TimeSyncLifecycleComponent final : public ILifecycleComponent
{
  public:
    TimeSyncLifecycleComponent(std::shared_ptr<time::TimeSyncRuntime> runtime,
                               std::shared_ptr<TimeSyncAlarmMonitor> alarm_monitor);

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] ShutdownPhase shutdown_phase() const noexcept override;
    [[nodiscard]] Result<void> start(std::stop_token startup_stop_token) override;
    [[nodiscard]] Result<void> request_stop(StopReason reason) override;
    [[nodiscard]] Result<void> join(std::chrono::steady_clock::time_point deadline) override;

  private:
    std::shared_ptr<time::TimeSyncRuntime> runtime_;
    std::shared_ptr<TimeSyncAlarmMonitor> alarm_monitor_;
};

} // namespace paperbreak::service
