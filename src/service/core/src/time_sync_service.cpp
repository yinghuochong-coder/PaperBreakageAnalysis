#include "paperbreak/service/time_sync_service.hpp"

#include <algorithm>
#include <utility>

namespace paperbreak::service
{
namespace
{
Error mapped_probe_error(const Error& cause, const std::string& camera_id)
{
    const bool unsupported = cause.business_code == "TIME_PROBE_NOT_SUPPORTED" ||
                             cause.business_code == "SYS_NOT_SUPPORTED";
    auto error = make_error(unsupported ? "TIME_PROBE_NOT_SUPPORTED" : "TIME_PROBE_UNAVAILABLE",
                            Severity::warning,
                            unsupported ? "相机不支持时间同步能力采样" : "相机时间同步能力暂不可用",
                            "time", "time.cameraProbe.sample", !unsupported);
    error.source_id = camera_id;
    error.native_domain = cause.native_domain;
    error.native_code = cause.native_code;
    error.details = cause.details;
    error.details.push_back({"causeBusinessCode", cause.business_code});
    return error;
}
} // namespace

CameraControlClockProbe::CameraControlClockProbe(
    std::string camera_id, std::shared_ptr<camera::CameraControlRuntime> cameras,
    const std::int64_t degraded_uncertainty_ns)
    : camera_id_(std::move(camera_id)), cameras_(std::move(cameras)),
      degraded_uncertainty_ns_(degraded_uncertainty_ns)
{
}

std::string_view CameraControlClockProbe::camera_id() const noexcept
{
    return camera_id_;
}

Result<time::CameraClockProbeSample> CameraControlClockProbe::sample(
    const std::stop_token stop_token, const std::chrono::steady_clock::time_point deadline)
{
    if (!cameras_ || degraded_uncertainty_ns_ < 0)
        return Result<time::CameraClockProbeSample>::failure(mapped_probe_error(
            make_error("TIME_PROBE_UNAVAILABLE", Severity::warning, "相机时间探针配置无效", "time",
                       "time.cameraProbe.sample"),
            camera_id_));
    auto sampled = cameras_->sample_clock(camera_id_, stop_token, deadline);
    if (!sampled)
        return Result<time::CameraClockProbeSample>::failure(
            mapped_probe_error(sampled.error(), camera_id_));
    const auto& value = sampled.value();
    const bool hardware_synchronized = value.hardware_ptp_supported && value.hardware_ptp_enabled &&
                                       value.hardware_ptp_synchronized;
    return Result<time::CameraClockProbeSample>::success(
        {.camera_timestamp_ticks = value.camera_timestamp_ticks,
         .camera_timestamp_frequency_hz = value.camera_timestamp_frequency_hz,
         .sample_monotonic_ns = value.sample_monotonic_ns,
         .sample_utc_ns = value.sample_utc_ns,
         .hardware_ptp_synchronized = hardware_synchronized,
         .offset_ns = value.offset_ns,
         .uncertainty_ns = hardware_synchronized
                               ? value.uncertainty_ns
                               : std::max(value.uncertainty_ns, degraded_uncertainty_ns_),
         .maximum_observed_offset_ns = value.maximum_observed_offset_ns,
         .last_synchronized_utc_ns = value.last_synchronized_utc_ns,
         .grandmaster_identity = value.grandmaster_identity,
         .last_error_code = value.last_error_code});
}

TimeSyncLifecycleComponent::TimeSyncLifecycleComponent(
    std::shared_ptr<time::TimeSyncRuntime> runtime,
    std::shared_ptr<TimeSyncAlarmMonitor> alarm_monitor)
    : runtime_(std::move(runtime)), alarm_monitor_(std::move(alarm_monitor))
{
}

std::string_view TimeSyncLifecycleComponent::name() const noexcept
{
    return "time-sync";
}

ShutdownPhase TimeSyncLifecycleComponent::shutdown_phase() const noexcept
{
    return ShutdownPhase::monitoring;
}

Result<void> TimeSyncLifecycleComponent::start(const std::stop_token startup_stop_token)
{
    if (!runtime_ || !alarm_monitor_)
        return Result<void>::failure(make_error("SYS_INTERNAL_ERROR", Severity::error,
                                                "时间同步组件依赖为空", "service",
                                                "service.timeSync.start"));
    if (startup_stop_token.stop_requested())
        return Result<void>::failure(make_error("SYS_SERVICE_STOPPING", Severity::warning,
                                                "服务启动已取消", "service",
                                                "service.timeSync.start", true));
    return runtime_->start();
}

Result<void> TimeSyncLifecycleComponent::request_stop(StopReason)
{
    if (runtime_)
        runtime_->request_stop();
    return Result<void>::success();
}

Result<void> TimeSyncLifecycleComponent::join(const std::chrono::steady_clock::time_point deadline)
{
    auto joined = runtime_ ? runtime_->join(deadline) : Result<void>::success();
    if (alarm_monitor_)
        alarm_monitor_->stop();
    return joined;
}

} // namespace paperbreak::service
