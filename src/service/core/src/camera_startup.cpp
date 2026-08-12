#include "paperbreak/service/camera_startup.hpp"

#include <condition_variable>
#include <string>
#include <utility>

namespace paperbreak::service
{

camera::CameraParameterSnapshot configured_camera_parameters(const config::CameraConfig& value)
{
    camera::ExposureAutoMode exposure_auto = camera::ExposureAutoMode::off;
    switch (value.exposure_auto_mode)
    {
    case config::ExposureAutoMode::off:
        exposure_auto = camera::ExposureAutoMode::off;
        break;
    case config::ExposureAutoMode::once:
        exposure_auto = camera::ExposureAutoMode::once;
        break;
    case config::ExposureAutoMode::continuous:
        exposure_auto = camera::ExposureAutoMode::continuous;
        break;
    }
    camera::PixelFormat pixel = camera::PixelFormat::mono8;
    switch (value.pixel_format)
    {
    case config::PixelFormat::mono8:
        pixel = camera::PixelFormat::mono8;
        break;
    case config::PixelFormat::mono10:
        pixel = camera::PixelFormat::mono10;
        break;
    case config::PixelFormat::mono12:
        pixel = camera::PixelFormat::mono12;
        break;
    case config::PixelFormat::bayer_rg8:
        pixel = camera::PixelFormat::bayer_rg8;
        break;
    }
    camera::TriggerMode trigger = camera::TriggerMode::continuous;
    switch (value.trigger_mode)
    {
    case config::TriggerMode::continuous:
        trigger = camera::TriggerMode::continuous;
        break;
    case config::TriggerMode::hardware:
        trigger = camera::TriggerMode::hardware;
        break;
    case config::TriggerMode::software:
        trigger = camera::TriggerMode::software;
        break;
    }
    camera::CameraParameterSnapshot result{
        .exposure_us = value.exposure_us,
        .exposure_auto_mode = exposure_auto,
        .gain_db = value.gain_db,
        .frame_rate = value.frame_rate,
        .roi =
            camera::Roi{value.roi.width, value.roi.height, value.roi.offset_x, value.roi.offset_y},
        .reverse_x = value.reverse_x,
        .reverse_y = value.reverse_y,
        .pixel_format = pixel,
        .trigger_mode = trigger,
        .trigger_delay_us = value.trigger_delay_us,
        .packet_size_bytes = value.packet_size_bytes,
        .inter_packet_delay_ns = value.inter_packet_delay_ns,
        .line_io =
            camera::LineIoParameters{.alarm_input_enabled = value.line_io.alarm_input_enabled,
                                     .strobe_output_enabled = value.line_io.strobe_output_enabled,
                                     .strobe_duration_us = value.line_io.strobe_duration_us,
                                     .strobe_pre_delay_us = value.line_io.strobe_pre_delay_us,
                                     .strobe_post_delay_us = value.line_io.strobe_post_delay_us}};
    if (trigger == camera::TriggerMode::hardware)
        result.trigger_source = value.trigger_source;
    return result;
}

CameraStartupLifecycleComponent::CameraStartupLifecycleComponent(
    std::shared_ptr<camera::CameraControlRuntime> cameras,
    std::vector<config::CameraConfig> configurations, config::AcquisitionConfig acquisition,
    std::shared_ptr<logging::LoggingRuntime> logging)
    : cameras_(std::move(cameras)), configurations_(std::move(configurations)),
      acquisition_(std::move(acquisition)), logging_(std::move(logging))
{
}

std::string_view CameraStartupLifecycleComponent::name() const noexcept
{
    return "camera-startup";
}

ShutdownPhase CameraStartupLifecycleComponent::shutdown_phase() const noexcept
{
    return ShutdownPhase::acquisition;
}

Result<void> CameraStartupLifecycleComponent::start(const std::stop_token startup_stop_token)
{
    if (!cameras_)
    {
        return Result<void>::failure(make_error("SYS_INTERNAL_ERROR", Severity::error,
                                                "相机启动组件缺少相机控制运行时", "service",
                                                "service.cameraStartup.start"));
    }
    if (!acquisition_.auto_start)
    {
        if (logging_)
            static_cast<void>(logging_->log(logging::Category::camera, logging::Level::info,
                                            "operation=camera.autoStart result=disabled"));
        return Result<void>::success();
    }

    {
        std::scoped_lock lock{managed_mutex_};
        managed_camera_ids_.clear();
        for (const auto& configuration : configurations_)
            if (configuration.enabled)
                managed_camera_ids_.push_back(configuration.id);
    }

    const std::uint32_t maximum_attempts = acquisition_.startup_retry_count + 1U;
    for (const auto& configuration : configurations_)
    {
        if (!configuration.enabled || startup_stop_token.stop_requested())
            continue;

        for (std::uint32_t attempt = 1U; attempt <= maximum_attempts; ++attempt)
        {
            if (startup_stop_token.stop_requested())
                break;
            std::string failed_stage;
            auto started = start_slot(configuration, failed_stage);
            if (started)
            {
                log_attempt(configuration, "start", attempt, true);
                break;
            }

            static_cast<void>(cameras_->disconnect(configuration.id));
            const bool final = attempt == maximum_attempts;
            log_attempt(configuration, failed_stage, attempt, final, &started.error());
            if (final || startup_stop_token.stop_requested())
                break;

            std::mutex wait_mutex;
            std::condition_variable_any wait_condition;
            std::unique_lock wait_lock{wait_mutex};
            static_cast<void>(wait_condition.wait_for(
                wait_lock, startup_stop_token,
                std::chrono::milliseconds{acquisition_.startup_retry_interval_ms},
                [] { return false; }));
        }
    }
    return Result<void>::success();
}

Result<void> CameraStartupLifecycleComponent::request_stop(StopReason)
{
    return Result<void>::success();
}

Result<void> CameraStartupLifecycleComponent::join(
    const std::chrono::steady_clock::time_point deadline)
{
    const auto timeout_error = [] {
        return make_error("SYS_SHUTDOWN_TIMEOUT", Severity::critical,
                          "相机启动组件没有剩余关闭预算", "service", "service.cameraStartup.join");
    };
    if (std::chrono::steady_clock::now() >= deadline)
        return Result<void>::failure(timeout_error());
    if (!cameras_)
        return Result<void>::success();

    std::vector<std::string> managed;
    {
        std::scoped_lock lock{managed_mutex_};
        managed = managed_camera_ids_;
    }
    std::optional<Error> first_error;
    for (const auto& camera_id : managed)
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return Result<void>::failure(timeout_error());
        auto disconnected = cameras_->disconnect(camera_id);
        if (std::chrono::steady_clock::now() > deadline)
            return Result<void>::failure(timeout_error());
        if (!disconnected && !first_error)
            first_error = disconnected.error();
        if (!disconnected && logging_)
        {
            const config::CameraConfig configuration{.id = camera_id};
            log_attempt(configuration, "disconnect", 1U, true, &disconnected.error());
        }
    }
    return first_error ? Result<void>::failure(std::move(*first_error)) : Result<void>::success();
}

Result<void> CameraStartupLifecycleComponent::start_slot(const config::CameraConfig& configuration,
                                                         std::string& failed_stage)
{
    failed_stage = "connect";
    auto connected = cameras_->connect(configuration.id, configuration.serial_number);
    if (!connected)
        return Result<void>::failure(connected.error());

    failed_stage = "applyConfig";
    auto updated = cameras_->update(configuration.id, configured_camera_parameters(configuration));
    if (!updated)
        return Result<void>::failure(updated.error());

    failed_stage = "start";
    auto started = cameras_->start(configuration.id);
    if (!started)
        return Result<void>::failure(started.error());
    return Result<void>::success();
}

void CameraStartupLifecycleComponent::log_attempt(const config::CameraConfig& configuration,
                                                  const std::string_view stage,
                                                  const std::uint32_t attempt, const bool final,
                                                  const Error* error) const noexcept
{
    if (!logging_)
        return;
    std::string message = "operation=camera.autoStart cameraId=" + configuration.id +
                          " serialNumber=" + configuration.serial_number +
                          " stage=" + std::string{stage} + " attempt=" + std::to_string(attempt);
    if (error)
    {
        message += " result=" + std::string{final ? "failure" : "retry"} +
                   " businessCode=" + error->business_code;
        if (error->native_domain)
            message += " nativeDomain=" + *error->native_domain;
        if (error->native_code)
            message += " nativeCode=" + *error->native_code;
    }
    else
    {
        message += " result=success";
    }
    static_cast<void>(logging_->log(
        logging::Category::camera,
        error ? (final ? logging::Level::error : logging::Level::warning) : logging::Level::info,
        message));
}

} // namespace paperbreak::service
