#include "paperbreak/camera/control.hpp"
#include "paperbreak/camera/hikrobot_camera.hpp"
#include "paperbreak/common/version.hpp"
#include "paperbreak/config/basic_config.hpp"
#include "paperbreak/config/config_repository.hpp"
#include "paperbreak/logging/logging.hpp"
#include "paperbreak/monitoring/monitoring.hpp"
#include "paperbreak/pipeline/preview.hpp"
#include "paperbreak/platform/atomic_file.hpp"
#include "paperbreak/platform/system_metrics.hpp"
#include "paperbreak/service/algorithm_metrics.hpp"
#include "paperbreak/service/event_runtime.hpp"
#include "paperbreak/service/runtime.hpp"
#include "paperbreak/service/system_commands.hpp"
#include "paperbreak/service/windows/console_control.hpp"
#include "paperbreak/service/windows/scm.hpp"
#include "paperbreak/service/windows/scm_host.hpp"
#include "paperbreak/storage/event_inspector.hpp"
#include "paperbreak/storage/metadata_database.hpp"
#include "paperbreak/storage/nvme_cache.hpp"
#include "paperbreak/storage/storage_policy.hpp"
#include "paperbreak/uplink/qt_transport.hpp"
#include "paperbreak/uplink/runtime.hpp"
#include "paperbreak/uplink/upload_scheduler.hpp"

#include <QCoreApplication>

#include <nlohmann/json.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

enum class Mode
{
    none,
    console,
    validate_config,
    install,
    uninstall,
    service,
    version,
};

struct Arguments final
{
    Mode mode{Mode::none};
    std::filesystem::path config_path;
    std::chrono::milliseconds run_for{0};
    bool run_for_present{false};
};

paperbreak::Error argument_error(std::string message)
{
    return paperbreak::make_error("SYS_CONFIG_INVALID", paperbreak::Severity::error,
                                  std::move(message), "service", "service.parseArguments");
}

paperbreak::Result<Arguments> parse_arguments(const int argc, char* argv[])
{
    Arguments arguments;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument{argv[index]};
        if (argument == "--console" || argument == "--validate-config" || argument == "--install" ||
            argument == "--uninstall" || argument == "--service" || argument == "--version")
        {
            Mode requested = Mode::version;
            if (argument == "--console")
            {
                requested = Mode::console;
            }
            else if (argument == "--validate-config")
            {
                requested = Mode::validate_config;
            }
            else if (argument == "--install")
            {
                requested = Mode::install;
            }
            else if (argument == "--uninstall")
            {
                requested = Mode::uninstall;
            }
            else if (argument == "--service")
            {
                requested = Mode::service;
            }
            if (arguments.mode != Mode::none)
            {
                return paperbreak::Result<Arguments>::failure(
                    argument_error("运行模式只能指定一次"));
            }
            arguments.mode = requested;
        }
        else if (argument == "--config")
        {
            if (++index >= argc || arguments.config_path.empty() == false)
            {
                return paperbreak::Result<Arguments>::failure(
                    argument_error("--config 必须且只能指定一个路径"));
            }
            arguments.config_path = std::filesystem::path{argv[index]};
            if (arguments.config_path.empty())
            {
                return paperbreak::Result<Arguments>::failure(
                    argument_error("--config 路径不能为空"));
            }
        }
        else if (argument == "--run-for-ms")
        {
            if (++index >= argc || arguments.run_for_present)
            {
                return paperbreak::Result<Arguments>::failure(
                    argument_error("--run-for-ms 必须且只能指定一个值"));
            }
            arguments.run_for_present = true;
            std::uint64_t duration = 0;
            const std::string_view text{argv[index]};
            const auto parse_result =
                std::from_chars(text.data(), text.data() + text.size(), duration);
            if (parse_result.ec != std::errc{} || parse_result.ptr != text.data() + text.size() ||
                duration > 60'000U)
            {
                return paperbreak::Result<Arguments>::failure(
                    argument_error("--run-for-ms 必须是 0 到 60000 的整数"));
            }
            arguments.run_for = std::chrono::milliseconds{duration};
        }
        else
        {
            return paperbreak::Result<Arguments>::failure(argument_error("未知命令行参数"));
        }
    }

    if (arguments.mode == Mode::none)
    {
        return paperbreak::Result<Arguments>::failure(argument_error(
            "必须指定 --console、--validate-config、--install、--uninstall、--service 或 "
            "--version"));
    }
    if (arguments.mode == Mode::version || arguments.mode == Mode::uninstall)
    {
        if (!arguments.config_path.empty() || arguments.run_for_present)
        {
            return paperbreak::Result<Arguments>::failure(
                argument_error("--version 和 --uninstall 不能与配置或运行时限参数组合"));
        }
        return paperbreak::Result<Arguments>::success(std::move(arguments));
    }
    if (arguments.config_path.empty())
    {
        return paperbreak::Result<Arguments>::failure(argument_error(
            "--console、--validate-config、--install 和 --service 必须提供 --config <path>"));
    }
    if (arguments.mode != Mode::console && arguments.run_for_present)
    {
        return paperbreak::Result<Arguments>::failure(
            argument_error("--run-for-ms 只能用于 --console"));
    }
    return paperbreak::Result<Arguments>::success(std::move(arguments));
}

class StopRequestChannel final
{
  public:
    void request(const paperbreak::service::StopReason reason)
    {
        {
            std::scoped_lock lock{mutex_};
            if (!reason_.has_value())
            {
                reason_ = reason;
            }
        }
        condition_.notify_one();
    }

    [[nodiscard]] paperbreak::service::StopReason wait()
    {
        std::unique_lock lock{mutex_};
        condition_.wait(lock, [this] { return reason_.has_value(); });
        return reason_.value();
    }

    [[nodiscard]] bool wait_for(const std::chrono::milliseconds duration)
    {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(lock, duration, [this] { return reason_.has_value(); });
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<paperbreak::service::StopReason> reason_;
};

class LoggingLifecycleComponent final : public paperbreak::service::ILifecycleComponent
{
  public:
    explicit LoggingLifecycleComponent(std::shared_ptr<paperbreak::logging::LoggingRuntime> runtime)
        : runtime_(std::move(runtime))
    {
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "logging";
    }

    [[nodiscard]] paperbreak::service::ShutdownPhase shutdown_phase() const noexcept override
    {
        return paperbreak::service::ShutdownPhase::logging;
    }

    [[nodiscard]] paperbreak::Result<void> start(std::stop_token) override
    {
        auto registration = runtime_->register_current_thread("service-main");
        if (!registration)
            return paperbreak::Result<void>::failure(registration.error());
        registration_ = std::move(registration).value();
        return runtime_->log(paperbreak::logging::Category::service,
                             paperbreak::logging::Level::info,
                             "PaperBreakEdgeService lifecycle started");
    }

    [[nodiscard]] paperbreak::Result<void> request_stop(paperbreak::service::StopReason) override
    {
        return paperbreak::Result<void>::success();
    }

    [[nodiscard]] paperbreak::Result<void> join(
        const std::chrono::steady_clock::time_point deadline) override
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return paperbreak::Result<void>::failure(paperbreak::make_error(
                "SYS_SHUTDOWN_TIMEOUT", paperbreak::Severity::critical, "日志组件没有剩余关闭预算",
                "service", "service.logging.join"));
        }
        static_cast<void>(runtime_->log(paperbreak::logging::Category::service,
                                        paperbreak::logging::Level::info,
                                        "PaperBreakEdgeService lifecycle stopping"));
        registration_.reset();
        return runtime_->shutdown();
    }

  private:
    std::shared_ptr<paperbreak::logging::LoggingRuntime> runtime_;
    std::optional<paperbreak::logging::LoggingRuntime::ThreadRegistration> registration_;
};

class BufferedConfigAuditSink final : public paperbreak::config::IConfigAuditSink
{
  public:
    [[nodiscard]] paperbreak::Result<void> record(
        const paperbreak::config::ConfigAuditRecord& record) override
    {
        std::string message =
            "config-change source=" +
            std::string{paperbreak::config::config_change_source_name(record.source)} +
            " actor=" + paperbreak::logging::redact_sensitive(record.actor) +
            " previousRevision=" + std::to_string(record.previous_revision) +
            " candidateRevision=" + std::to_string(record.candidate_revision) +
            " correlationId=" + record.correlation_id + " paths=";
        for (const auto& path : record.changed_paths)
        {
            message += path + ',';
        }
        message += " changes=";
        for (const auto& change : record.redacted_changes)
        {
            message +=
                change.path + ":" + change.previous_value + "->" + change.candidate_value + ';';
        }
        std::scoped_lock lock{mutex_};
        if (runtime_)
        {
            return runtime_->log(paperbreak::logging::Category::audit,
                                 paperbreak::logging::Level::info, message);
        }
        if (pending_.size() >= 64U)
        {
            return paperbreak::Result<void>::failure(paperbreak::make_error(
                "LOG_WRITE_FAILED", paperbreak::Severity::error, "配置审计启动缓冲已满", "service",
                "service.configAudit.buffer"));
        }
        pending_.push_back(std::move(message));
        return paperbreak::Result<void>::success();
    }

    [[nodiscard]] paperbreak::Result<void> attach(
        std::shared_ptr<paperbreak::logging::LoggingRuntime> runtime)
    {
        std::scoped_lock lock{mutex_};
        runtime_ = std::move(runtime);
        for (const auto& message : pending_)
        {
            auto result = runtime_->log(paperbreak::logging::Category::audit,
                                        paperbreak::logging::Level::info, message);
            if (!result)
            {
                return result;
            }
        }
        pending_.clear();
        return paperbreak::Result<void>::success();
    }

  private:
    std::mutex mutex_;
    std::vector<std::string> pending_;
    std::shared_ptr<paperbreak::logging::LoggingRuntime> runtime_;
};

struct ConfigurationResources final
{
    explicit ConfigurationResources(std::filesystem::path path)
        : repository(std::move(path), files, audit)
    {
    }

    paperbreak::platform::WindowsAtomicFileSystem files;
    BufferedConfigAuditSink audit;
    paperbreak::config::ConfigRepository repository;
    std::vector<std::shared_ptr<paperbreak::config::IConfigApplier>> dynamic_appliers;
};

class ConfigurationLifecycleComponent final : public paperbreak::service::ILifecycleComponent
{
  public:
    explicit ConfigurationLifecycleComponent(std::shared_ptr<ConfigurationResources> resources)
        : resources_(std::move(resources))
    {
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "configuration";
    }
    [[nodiscard]] paperbreak::service::ShutdownPhase shutdown_phase() const noexcept override
    {
        return paperbreak::service::ShutdownPhase::configuration;
    }
    [[nodiscard]] paperbreak::Result<void> start(std::stop_token) override
    {
        auto result = resources_->repository.snapshot();
        if (!result)
            return paperbreak::Result<void>::failure(result.error());
        return paperbreak::Result<void>::success();
    }
    [[nodiscard]] paperbreak::Result<void> request_stop(paperbreak::service::StopReason) override
    {
        resources_->repository.stop_accepting_changes();
        return paperbreak::Result<void>::success();
    }
    [[nodiscard]] paperbreak::Result<void> join(std::chrono::steady_clock::time_point) override
    {
        return paperbreak::Result<void>::success();
    }

  private:
    std::shared_ptr<ConfigurationResources> resources_;
};

class IpcLifecycleComponent final : public paperbreak::service::ILifecycleComponent
{
  public:
    explicit IpcLifecycleComponent(std::shared_ptr<paperbreak::ipc::IpcServer> server)
        : server_(std::move(server))
    {
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "ipc";
    }
    [[nodiscard]] paperbreak::service::ShutdownPhase shutdown_phase() const noexcept override
    {
        return paperbreak::service::ShutdownPhase::ipc;
    }
    [[nodiscard]] paperbreak::Result<void> start(std::stop_token) override
    {
        return server_->start();
    }
    [[nodiscard]] paperbreak::Result<void> request_stop(paperbreak::service::StopReason) override
    {
        server_->request_stop();
        return paperbreak::Result<void>::success();
    }
    [[nodiscard]] paperbreak::Result<void> join(
        const std::chrono::steady_clock::time_point deadline) override
    {
        return server_->join(deadline);
    }

  private:
    std::shared_ptr<paperbreak::ipc::IpcServer> server_;
};

class PreviewLifecycleComponent final : public paperbreak::service::ILifecycleComponent
{
  public:
    explicit PreviewLifecycleComponent(
        std::shared_ptr<paperbreak::pipeline::PreviewRuntime> runtime)
        : runtime_(std::move(runtime))
    {
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "preview";
    }
    [[nodiscard]] paperbreak::service::ShutdownPhase shutdown_phase() const noexcept override
    {
        return paperbreak::service::ShutdownPhase::processing;
    }
    [[nodiscard]] paperbreak::Result<void> start(std::stop_token) override
    {
        return runtime_->start();
    }
    [[nodiscard]] paperbreak::Result<void> request_stop(paperbreak::service::StopReason) override
    {
        runtime_->request_stop();
        return paperbreak::Result<void>::success();
    }
    [[nodiscard]] paperbreak::Result<void> join(
        const std::chrono::steady_clock::time_point deadline) override
    {
        return runtime_->join(deadline);
    }

  private:
    std::shared_ptr<paperbreak::pipeline::PreviewRuntime> runtime_;
};

class EventLifecycleComponent final : public paperbreak::service::ILifecycleComponent
{
  public:
    explicit EventLifecycleComponent(std::shared_ptr<paperbreak::service::EventRuntime> runtime)
        : runtime_(std::move(runtime))
    {
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "event";
    }
    [[nodiscard]] paperbreak::service::ShutdownPhase shutdown_phase() const noexcept override
    {
        return paperbreak::service::ShutdownPhase::event;
    }
    [[nodiscard]] paperbreak::Result<void> start(std::stop_token) override
    {
        return runtime_->start();
    }
    [[nodiscard]] paperbreak::Result<void> request_stop(paperbreak::service::StopReason) override
    {
        runtime_->request_stop();
        return paperbreak::Result<void>::success();
    }
    [[nodiscard]] paperbreak::Result<void> join(
        const std::chrono::steady_clock::time_point deadline) override
    {
        return runtime_->join(deadline);
    }

  private:
    std::shared_ptr<paperbreak::service::EventRuntime> runtime_;
};

class UplinkLifecycleComponent final : public paperbreak::service::ILifecycleComponent
{
  public:
    UplinkLifecycleComponent(
        std::shared_ptr<paperbreak::uplink::UplinkRuntime> runtime,
        std::shared_ptr<paperbreak::uplink::PersistentUploadScheduler> scheduler)
        : runtime_(std::move(runtime)), scheduler_(std::move(scheduler))
    {
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "uplink";
    }

    [[nodiscard]] paperbreak::service::ShutdownPhase shutdown_phase() const noexcept override
    {
        return paperbreak::service::ShutdownPhase::uplink;
    }

    [[nodiscard]] paperbreak::Result<void> start(std::stop_token) override
    {
        auto runtime = runtime_->start();
        if (!runtime)
            return runtime;
        auto scheduler = scheduler_->start();
        if (!scheduler)
        {
            runtime_->request_stop();
            static_cast<void>(
                runtime_->join(std::chrono::steady_clock::now() + std::chrono::seconds{5}));
        }
        return scheduler;
    }

    [[nodiscard]] paperbreak::Result<void> request_stop(paperbreak::service::StopReason) override
    {
        scheduler_->request_stop();
        runtime_->request_stop();
        return paperbreak::Result<void>::success();
    }

    [[nodiscard]] paperbreak::Result<void> join(
        const std::chrono::steady_clock::time_point deadline) override
    {
        auto scheduler = scheduler_->join(deadline);
        auto runtime = runtime_->join(deadline);
        return scheduler ? runtime : scheduler;
    }

  private:
    std::shared_ptr<paperbreak::uplink::UplinkRuntime> runtime_;
    std::shared_ptr<paperbreak::uplink::PersistentUploadScheduler> scheduler_;
};

class NvmeLifecycleComponent final : public paperbreak::service::ILifecycleComponent
{
  public:
    explicit NvmeLifecycleComponent(std::shared_ptr<paperbreak::storage::NvmeRollingCache> runtime)
        : runtime_(std::move(runtime))
    {
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "nvme-cache";
    }
    [[nodiscard]] paperbreak::service::ShutdownPhase shutdown_phase() const noexcept override
    {
        return paperbreak::service::ShutdownPhase::event;
    }
    [[nodiscard]] paperbreak::Result<void> start(std::stop_token) override
    {
        return runtime_->start();
    }
    [[nodiscard]] paperbreak::Result<void> request_stop(paperbreak::service::StopReason) override
    {
        runtime_->request_stop();
        return paperbreak::Result<void>::success();
    }
    [[nodiscard]] paperbreak::Result<void> join(
        const std::chrono::steady_clock::time_point deadline) override
    {
        return runtime_->join(deadline);
    }

  private:
    std::shared_ptr<paperbreak::storage::NvmeRollingCache> runtime_;
};

class StorageMaintenanceLifecycleComponent final : public paperbreak::service::ILifecycleComponent
{
  public:
    StorageMaintenanceLifecycleComponent(
        std::shared_ptr<paperbreak::storage::StoragePolicyManager> manager,
        std::shared_ptr<paperbreak::monitoring::AlarmRegistry> alarms,
        std::weak_ptr<paperbreak::storage::NvmeRollingCache> nvme,
        paperbreak::ThreadRegistrationFactory register_thread)
        : manager_(std::move(manager)), alarms_(std::move(alarms)), nvme_(std::move(nvme)),
          register_thread_(std::move(register_thread))
    {
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "storage-maintenance";
    }
    [[nodiscard]] paperbreak::service::ShutdownPhase shutdown_phase() const noexcept override
    {
        return paperbreak::service::ShutdownPhase::event;
    }
    [[nodiscard]] paperbreak::Result<void> start(std::stop_token) override
    {
        worker_ = std::jthread{[this](const std::stop_token token) { run(token); }};
        return paperbreak::Result<void>::success();
    }
    [[nodiscard]] paperbreak::Result<void> request_stop(paperbreak::service::StopReason) override
    {
        worker_.request_stop();
        condition_.notify_all();
        return paperbreak::Result<void>::success();
    }
    [[nodiscard]] paperbreak::Result<void> join(
        const std::chrono::steady_clock::time_point deadline) override
    {
        if (!worker_.joinable())
            return paperbreak::Result<void>::success();
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return paperbreak::Result<void>::failure(paperbreak::make_error(
                "SYS_SHUTDOWN_TIMEOUT", paperbreak::Severity::critical,
                "存储维护线程没有剩余关闭预算", "storage", "storage.maintenance.join"));
        }
        worker_.join();
        return paperbreak::Result<void>::success();
    }

  private:
    void run(const std::stop_token token) noexcept
    {
        const auto thread_registration =
            register_thread_ ? register_thread_("storage-maintenance") : nullptr;
        while (!token.stop_requested())
        {
            auto maintenance = manager_->run_maintenance(std::chrono::system_clock::now());
            if (!maintenance)
            {
                static_cast<void>(alarms_->raise_alarm({.code = maintenance.error().business_code,
                                                        .severity = maintenance.error().severity,
                                                        .source = "storage",
                                                        .message = maintenance.error().message,
                                                        .details = maintenance.error().details}));
            }
            else if (maintenance.value().snapshot.watermark !=
                     paperbreak::storage::StorageWatermark::normal)
            {
                const auto watermark = maintenance.value().snapshot.watermark;
                const auto severity = watermark == paperbreak::storage::StorageWatermark::warning
                                          ? paperbreak::Severity::warning
                                          : paperbreak::Severity::critical;
                static_cast<void>(alarms_->raise_alarm(
                    {.code = "STORAGE_LOW_SPACE",
                     .severity = severity,
                     .source = "storage",
                     .message = "事件存储空间已达到水位限制",
                     .details = {{.key = "watermark",
                                  .value = std::string{paperbreak::storage::to_string(watermark)}},
                                 {.key = "availableBytes",
                                  .value = std::to_string(
                                      maintenance.value().snapshot.available_bytes)}}}));
            }
            else
            {
                static_cast<void>(alarms_->clear("STORAGE_LOW_SPACE", "storage"));
            }
            if (maintenance)
            {
                if (auto nvme = nvme_.lock())
                    nvme->set_storage_watermark(maintenance.value().snapshot.watermark);
            }

            std::unique_lock lock{mutex_};
            condition_.wait_for(lock, token, std::chrono::seconds{30}, [] { return false; });
        }
    }

    std::shared_ptr<paperbreak::storage::StoragePolicyManager> manager_;
    std::shared_ptr<paperbreak::monitoring::AlarmRegistry> alarms_;
    std::weak_ptr<paperbreak::storage::NvmeRollingCache> nvme_;
    paperbreak::ThreadRegistrationFactory register_thread_;
    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::jthread worker_;
};

class PreviewPublisher final
{
  public:
    void set_server(const std::shared_ptr<paperbreak::ipc::IpcServer>& server)
    {
        std::scoped_lock lock{mutex_};
        server_ = server;
    }

    void publish(paperbreak::pipeline::PreviewDelivery delivery) const noexcept
    {
        std::shared_ptr<paperbreak::ipc::IpcServer> server;
        {
            std::scoped_lock lock{mutex_};
            server = server_.lock();
        }
        if (!server)
            return;
        nlohmann::json payload{{"cameraId", delivery.camera_id},
                               {"cameraFrameNumber", delivery.camera_frame_number},
                               {"sequenceNumber", delivery.sequence_number},
                               {"width", delivery.source_geometry.width},
                               {"height", delivery.source_geometry.height},
                               {"stride", delivery.source_geometry.stride},
                               {"cameraStatus", delivery.metadata.camera_status},
                               {"detectionResult", delivery.metadata.detection_result}};
        if (delivery.metadata.brightness)
            payload["brightness"] = delivery.metadata.brightness.value();
        if (delivery.metadata.actual_fps)
            payload["actualFps"] = delivery.metadata.actual_fps.value();
        if (delivery.metadata.roi)
        {
            const auto& roi = delivery.metadata.roi.value();
            payload["roi"] = {
                {"x", roi.x}, {"y", roi.y}, {"width", roi.width}, {"height", roi.height}};
        }
        static_cast<void>(server->try_publish(
            {.event_name = "preview.frame",
             .timestamp = paperbreak::current_utc_timestamp(),
             .payload_json = payload.dump(),
             .binary = std::move(delivery.jpeg),
             .coalescing_key =
                 "preview." + std::to_string(delivery.subscriber_id) + "." + delivery.camera_id,
             .target_connection_id = delivery.subscriber_id},
            paperbreak::ipc::PushPolicy::coalesce_latest));
    }

  private:
    mutable std::mutex mutex_;
    std::weak_ptr<paperbreak::ipc::IpcServer> server_;
};

class MonitoringLifecycleComponent final : public paperbreak::service::ILifecycleComponent
{
  public:
    explicit MonitoringLifecycleComponent(
        std::shared_ptr<paperbreak::monitoring::HealthMonitor> monitor)
        : monitor_(std::move(monitor))
    {
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "monitoring";
    }
    [[nodiscard]] paperbreak::service::ShutdownPhase shutdown_phase() const noexcept override
    {
        return paperbreak::service::ShutdownPhase::monitoring;
    }
    [[nodiscard]] paperbreak::Result<void> start(std::stop_token) override
    {
        return monitor_->start();
    }
    [[nodiscard]] paperbreak::Result<void> request_stop(paperbreak::service::StopReason) override
    {
        monitor_->request_stop();
        return paperbreak::Result<void>::success();
    }
    [[nodiscard]] paperbreak::Result<void> join(
        const std::chrono::steady_clock::time_point deadline) override
    {
        return monitor_->join(deadline);
    }

  private:
    std::shared_ptr<paperbreak::monitoring::HealthMonitor> monitor_;
};

std::filesystem::path path_from_utf8(const std::string_view value)
{
    std::u8string converted;
    converted.reserve(value.size());
    for (const unsigned char byte : value)
        converted.push_back(static_cast<char8_t>(byte));
    return std::filesystem::path{converted};
}

std::filesystem::path resolve_config_path(const std::filesystem::path& config_path,
                                          const std::string_view value)
{
    auto path = path_from_utf8(value);
    if (path.is_relative())
    {
        path = config_path.parent_path() / path;
    }
    return path.lexically_normal();
}

paperbreak::monitoring::HealthMonitorOptions monitoring_options_from_config(
    const paperbreak::config::EdgeConfig& config)
{
    paperbreak::monitoring::HealthMonitorOptions options;
    options.sample_interval = std::chrono::milliseconds{config.health.sample_interval_ms};
    options.cpu_warning_percent = config.health.cpu_warning_percent;
    options.memory_warning_percent = config.health.memory_warning_percent;
    options.disks = {
        {.metric_name = "disk.system.free_gib",
         .source = "system",
         .warning_free_gib = static_cast<double>(config.storage.warning_free_space_gib),
         .critical_free_gib = static_cast<double>(config.storage.critical_free_space_gib),
         .stop_free_gib = static_cast<double>(config.storage.stop_free_space_gib)},
        {.metric_name = "disk.event.free_gib",
         .source = "event",
         .warning_free_gib = static_cast<double>(config.storage.warning_free_space_gib),
         .critical_free_gib = static_cast<double>(config.storage.critical_free_space_gib),
         .stop_free_gib = static_cast<double>(config.storage.stop_free_space_gib)},
        {.metric_name = "disk.cache.free_gib",
         .source = "cache",
         .warning_free_gib = static_cast<double>(config.storage.warning_free_space_gib),
         .critical_free_gib = static_cast<double>(config.storage.critical_free_space_gib),
         .stop_free_gib = static_cast<double>(config.storage.stop_free_space_gib)},
        {.metric_name = "disk.log.free_gib",
         .source = "log",
         .warning_free_gib = static_cast<double>(config.storage.warning_free_space_gib),
         .critical_free_gib = static_cast<double>(config.storage.critical_free_space_gib),
         .stop_free_gib = static_cast<double>(config.storage.stop_free_space_gib)}};
    return options;
}

paperbreak::logging::Level logging_level_from_config(paperbreak::config::LogLevel level) noexcept;

class MonitoringConfigApplier final : public paperbreak::config::IConfigApplier
{
  public:
    explicit MonitoringConfigApplier(std::shared_ptr<paperbreak::monitoring::HealthMonitor> monitor)
        : monitor_(std::move(monitor))
    {
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "monitoring";
    }
    [[nodiscard]] paperbreak::Result<void> prepare(const paperbreak::config::EdgeConfig& current,
                                                   const paperbreak::config::EdgeConfig& candidate,
                                                   const std::vector<std::string>&) override
    {
        previous_ = monitoring_options_from_config(current);
        candidate_ = monitoring_options_from_config(candidate);
        return paperbreak::Result<void>::success();
    }
    [[nodiscard]] paperbreak::Result<void> apply_and_readback(
        const paperbreak::config::EdgeConfig&) override
    {
        if (!candidate_.has_value())
        {
            return paperbreak::Result<void>::failure(paperbreak::make_error(
                "SYS_CONFIG_APPLY_FAILED", paperbreak::Severity::error,
                "健康监测配置没有完成预应用", "monitoring", "monitoring.config.apply"));
        }
        return monitor_->reconfigure(candidate_.value());
    }
    [[nodiscard]] paperbreak::Result<void> commit(const paperbreak::config::EdgeConfig&) override
    {
        previous_.reset();
        candidate_.reset();
        return paperbreak::Result<void>::success();
    }
    [[nodiscard]] paperbreak::Result<void> rollback(
        const paperbreak::config::EdgeConfig& previous) noexcept override
    {
        auto options = previous_.value_or(monitoring_options_from_config(previous));
        previous_.reset();
        candidate_.reset();
        return monitor_->reconfigure(std::move(options));
    }

  private:
    std::shared_ptr<paperbreak::monitoring::HealthMonitor> monitor_;
    std::optional<paperbreak::monitoring::HealthMonitorOptions> previous_;
    std::optional<paperbreak::monitoring::HealthMonitorOptions> candidate_;
};

class LoggingConfigApplier final : public paperbreak::config::IConfigApplier
{
  public:
    explicit LoggingConfigApplier(std::shared_ptr<paperbreak::logging::LoggingRuntime> runtime)
        : runtime_(std::move(runtime))
    {
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "logging";
    }

    [[nodiscard]] paperbreak::Result<void> prepare(
        const paperbreak::config::EdgeConfig& current,
        const paperbreak::config::EdgeConfig& candidate,
        const std::vector<std::string>& changed_paths) override
    {
        relevant_ = std::ranges::find(changed_paths, "/logging/live") != changed_paths.end();
        if (relevant_)
        {
            previous_level_ = logging_level_from_config(current.logging.level);
            previous_retention_ = current.logging.retention_days;
            candidate_level_ = logging_level_from_config(candidate.logging.level);
            candidate_retention_ = candidate.logging.retention_days;
        }
        return paperbreak::Result<void>::success();
    }

    [[nodiscard]] paperbreak::Result<void> apply_and_readback(
        const paperbreak::config::EdgeConfig&) override
    {
        if (!relevant_)
            return paperbreak::Result<void>::success();
        if (!candidate_level_ || !candidate_retention_)
            return paperbreak::Result<void>::failure(paperbreak::make_error(
                "SYS_CONFIG_APPLY_FAILED", paperbreak::Severity::error, "日志配置没有完成预应用",
                "logging", "logging.config.apply"));
        auto level = runtime_->set_minimum_level(*candidate_level_);
        if (!level)
            return level;
        auto retention = runtime_->set_retention_days(*candidate_retention_);
        if (!retention)
            return retention;
        if (runtime_->minimum_level() != *candidate_level_ ||
            runtime_->retention_days() != *candidate_retention_)
            return paperbreak::Result<void>::failure(
                paperbreak::make_error("SYS_CONFIG_APPLY_FAILED", paperbreak::Severity::error,
                                       "日志配置回读不一致", "logging", "logging.config.readback"));
        return paperbreak::Result<void>::success();
    }

    [[nodiscard]] paperbreak::Result<void> commit(const paperbreak::config::EdgeConfig&) override
    {
        reset();
        return paperbreak::Result<void>::success();
    }

    [[nodiscard]] paperbreak::Result<void> rollback(
        const paperbreak::config::EdgeConfig& previous) noexcept override
    {
        if (!relevant_)
            return paperbreak::Result<void>::success();
        const auto level =
            previous_level_.value_or(logging_level_from_config(previous.logging.level));
        const auto retention = previous_retention_.value_or(previous.logging.retention_days);
        auto result = runtime_->set_minimum_level(level);
        auto retention_result = runtime_->set_retention_days(retention);
        reset();
        return !result ? result : retention_result;
    }

  private:
    void reset() noexcept
    {
        relevant_ = false;
        previous_level_.reset();
        previous_retention_.reset();
        candidate_level_.reset();
        candidate_retention_.reset();
    }

    std::shared_ptr<paperbreak::logging::LoggingRuntime> runtime_;
    bool relevant_{};
    std::optional<paperbreak::logging::Level> previous_level_;
    std::optional<std::uint32_t> previous_retention_;
    std::optional<paperbreak::logging::Level> candidate_level_;
    std::optional<std::uint32_t> candidate_retention_;
};

class EventConfigApplier final : public paperbreak::config::IConfigApplier
{
  public:
    EventConfigApplier(std::shared_ptr<paperbreak::service::EventRuntime> runtime,
                       std::shared_ptr<paperbreak::storage::StoragePolicyManager> storage_policy)
        : runtime_(std::move(runtime)), storage_policy_(std::move(storage_policy))
    {
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "event";
    }
    [[nodiscard]] paperbreak::Result<void> prepare(
        const paperbreak::config::EdgeConfig& current,
        const paperbreak::config::EdgeConfig& candidate,
        const std::vector<std::string>& changed_paths) override
    {
        event_relevant_ = std::ranges::any_of(changed_paths, [](const std::string_view path) {
            return path == "/event" || path.starts_with("/event/") || path == "/algorithm" ||
                   path.starts_with("/algorithm/") || path == "/plantIo" ||
                   path.starts_with("/plantIo/");
        });
        storage_relevant_ =
            std::ranges::find(changed_paths, "/storage/watermarks") != changed_paths.end();
        if (event_relevant_ || storage_relevant_)
        {
            previous_ = current;
            candidate_ = candidate;
        }
        return paperbreak::Result<void>::success();
    }
    [[nodiscard]] paperbreak::Result<void> apply_and_readback(
        const paperbreak::config::EdgeConfig&) override
    {
        if (!event_relevant_ && !storage_relevant_)
            return paperbreak::Result<void>::success();
        if (!candidate_)
        {
            return paperbreak::Result<void>::failure(
                paperbreak::make_error("SYS_CONFIG_APPLY_FAILED", paperbreak::Severity::error,
                                       "事件配置没有完成预应用", "event", "event.config.apply"));
        }
        if (event_relevant_)
        {
            auto runtime = runtime_->reconfigure(*candidate_);
            if (!runtime)
                return runtime;
            auto retention = storage_policy_->set_retention_age(
                std::chrono::days{candidate_->event.retention_days});
            if (!retention)
                return retention;
        }
        if (storage_relevant_)
            return apply_storage_limits(*candidate_);
        return paperbreak::Result<void>::success();
    }
    [[nodiscard]] paperbreak::Result<void> commit(const paperbreak::config::EdgeConfig&) override
    {
        reset();
        return paperbreak::Result<void>::success();
    }
    [[nodiscard]] paperbreak::Result<void> rollback(
        const paperbreak::config::EdgeConfig& previous) noexcept override
    {
        const auto rollback_configuration = previous_.value_or(previous);
        auto result = event_relevant_ ? runtime_->reconfigure(rollback_configuration)
                                      : paperbreak::Result<void>::success();
        if (event_relevant_)
        {
            auto retention = storage_policy_->set_retention_age(
                std::chrono::days{rollback_configuration.event.retention_days});
            if (result && !retention)
                result = std::move(retention);
        }
        if (storage_relevant_)
        {
            auto limits = apply_storage_limits(rollback_configuration);
            if (result && !limits)
                result = std::move(limits);
        }
        reset();
        return result;
    }

  private:
    paperbreak::Result<void> apply_storage_limits(
        const paperbreak::config::EdgeConfig& configuration) const
    {
        constexpr std::uint64_t gibibyte = 1024ULL * 1024ULL * 1024ULL;
        return storage_policy_->reconfigure_limits(
            {.warning_available_bytes = configuration.storage.warning_free_space_gib * gibibyte,
             .critical_available_bytes = configuration.storage.critical_free_space_gib * gibibyte,
             .stop_save_available_bytes = configuration.storage.stop_free_space_gib * gibibyte},
            configuration.storage.maximum_event_storage_gib * gibibyte);
    }

    void reset() noexcept
    {
        event_relevant_ = false;
        storage_relevant_ = false;
        previous_.reset();
        candidate_.reset();
    }

    std::shared_ptr<paperbreak::service::EventRuntime> runtime_;
    std::shared_ptr<paperbreak::storage::StoragePolicyManager> storage_policy_;
    bool event_relevant_{};
    bool storage_relevant_{};
    std::optional<paperbreak::config::EdgeConfig> previous_;
    std::optional<paperbreak::config::EdgeConfig> candidate_;
};

class IpcMetricSource final : public paperbreak::monitoring::IMetricSource
{
  public:
    explicit IpcMetricSource(std::weak_ptr<paperbreak::ipc::IpcServer> server)
        : server_(std::move(server))
    {
    }
    [[nodiscard]] std::string_view source_name() const noexcept override
    {
        return "ipc";
    }
    [[nodiscard]] paperbreak::Result<std::vector<paperbreak::monitoring::MetricPoint>> collect(
        std::stop_token) noexcept override
    {
        auto server = server_.lock();
        if (!server)
        {
            return paperbreak::Result<std::vector<paperbreak::monitoring::MetricPoint>>::failure(
                paperbreak::make_error("SYS_MONITORING_SAMPLE_FAILED",
                                       paperbreak::Severity::warning, "IPC 指标源已失效",
                                       "monitoring", "monitoring.ipc.collect", true));
        }
        const auto metrics = server->metrics_snapshot();
        using Point = paperbreak::monitoring::MetricPoint;
        return paperbreak::Result<std::vector<Point>>::success(
            {{.name = "ipc.connections.active",
              .value = metrics.active_connections,
              .unit = "count"},
             {.name = "ipc.requests.in_flight",
              .value = metrics.in_flight_requests,
              .unit = "count"},
             {.name = "ipc.command_queue.depth",
              .value = metrics.command_queue_depth,
              .unit = "count"},
             {.name = "ipc.command_queue.high_watermark",
              .value = metrics.command_queue_high_watermark,
              .unit = "count"},
             {.name = "ipc.control_queue.depth",
              .value = metrics.control_queue_depth,
              .unit = "count"},
             {.name = "ipc.control_queue.high_watermark",
              .value = metrics.control_queue_high_watermark,
              .unit = "count"},
             {.name = "ipc.query_queue.depth", .value = metrics.query_queue_depth, .unit = "count"},
             {.name = "ipc.query_queue.high_watermark",
              .value = metrics.query_queue_high_watermark,
              .unit = "count"},
             {.name = "ipc.publish_queue.depth",
              .value = metrics.publish_queue_depth,
              .unit = "count"},
             {.name = "ipc.publish_queue.high_watermark",
              .value = metrics.publish_queue_high_watermark,
              .unit = "count"},
             {.name = "ipc.outbound.messages", .value = metrics.outbound_messages, .unit = "count"},
             {.name = "ipc.outbound.bytes", .value = metrics.outbound_bytes, .unit = "bytes"},
             {.name = "ipc.requests.total", .value = metrics.requests_total, .unit = "count"},
             {.name = "ipc.responses.total", .value = metrics.responses_total, .unit = "count"},
             {.name = "ipc.protocol_errors.total",
              .value = metrics.protocol_errors_total,
              .unit = "count"},
             {.name = "ipc.pushes.dropped_total",
              .value = metrics.pushes_dropped_total,
              .unit = "count"},
             {.name = "ipc.request_duration.average_ms",
              .value = metrics.average_request_duration_ms,
              .unit = "milliseconds"},
             {.name = "ipc.request_duration.maximum_ms",
              .value = metrics.maximum_request_duration_ms,
              .unit = "milliseconds"},
             {.name = "ipc.control_request_duration.average_ms",
              .value = metrics.average_control_request_duration_ms,
              .unit = "milliseconds"},
             {.name = "ipc.control_request_duration.maximum_ms",
              .value = metrics.maximum_control_request_duration_ms,
              .unit = "milliseconds"},
             {.name = "ipc.query_request_duration.average_ms",
              .value = metrics.average_query_request_duration_ms,
              .unit = "milliseconds"},
             {.name = "ipc.query_request_duration.maximum_ms",
              .value = metrics.maximum_query_request_duration_ms,
              .unit = "milliseconds"}});
    }

  private:
    std::weak_ptr<paperbreak::ipc::IpcServer> server_;
};

class DatabaseMetricSource final : public paperbreak::monitoring::IMetricSource
{
  public:
    explicit DatabaseMetricSource(
        std::weak_ptr<paperbreak::storage::EventMetadataDatabase> database)
        : database_(std::move(database))
    {
    }

    [[nodiscard]] std::string_view source_name() const noexcept override
    {
        return "database";
    }
    [[nodiscard]] paperbreak::Result<std::vector<paperbreak::monitoring::MetricPoint>> collect(
        std::stop_token) noexcept override
    {
        using Point = paperbreak::monitoring::MetricPoint;
        const bool available = !database_.expired();
        return paperbreak::Result<std::vector<Point>>::success(
            {{.name = "database.available", .value = available, .unit = "boolean"},
             {.name = "database.state",
              .value = std::string{available ? "ready" : "unavailable"},
              .unit = "state"},
             {.name = "database.schema.version",
              .value = std::uint64_t{paperbreak::storage::database_schema_version},
              .unit = "version",
              .available = available}});
    }

  private:
    std::weak_ptr<paperbreak::storage::EventMetadataDatabase> database_;
};

class EventMetricSource final : public paperbreak::monitoring::IMetricSource
{
  public:
    EventMetricSource(std::weak_ptr<paperbreak::service::EventRuntime> runtime,
                      std::weak_ptr<paperbreak::storage::StoragePolicyManager> storage,
                      std::weak_ptr<paperbreak::storage::NvmeRollingCache> nvme,
                      std::weak_ptr<paperbreak::uplink::UplinkRuntime> uplink,
                      std::weak_ptr<paperbreak::storage::EventMetadataDatabase> database)
        : runtime_(std::move(runtime)), storage_(std::move(storage)), nvme_(std::move(nvme)),
          uplink_(std::move(uplink)), database_(std::move(database))
    {
    }

    [[nodiscard]] std::string_view source_name() const noexcept override
    {
        return "operations";
    }
    [[nodiscard]] paperbreak::Result<std::vector<paperbreak::monitoring::MetricPoint>> collect(
        std::stop_token) noexcept override
    {
        using Point = paperbreak::monitoring::MetricPoint;
        const auto runtime = runtime_.lock();
        const auto storage = storage_.lock();
        const auto events =
            runtime ? runtime->snapshot() : paperbreak::service::EventRuntimeSnapshot{};
        const auto policy =
            storage ? storage->snapshot() : paperbreak::storage::StoragePolicySnapshot{};
        const auto nvme = nvme_.lock();
        const auto cache =
            nvme ? nvme->snapshot() : paperbreak::storage::NvmeRollingCacheSnapshot{};
        const auto uplink = uplink_.lock();
        const auto uplink_state =
            uplink ? uplink->snapshot() : paperbreak::uplink::UplinkRuntimeSnapshot{};
        const auto database = database_.lock();
        auto upload_queue =
            database
                ? database->upload_queue_stats()
                : paperbreak::Result<paperbreak::storage::UploadQueueStats>::failure(
                      paperbreak::make_error("DATABASE_NOT_READY", paperbreak::Severity::warning,
                                             "上传数据库未初始化", "uplink", "uplink.metrics"));
        return paperbreak::Result<std::vector<Point>>::success(
            {{.name = "system.nvme_write_bytes_per_second",
              .value = cache.write_bytes_per_second,
              .unit = "bytes/second",
              .available = nvme != nullptr},
             {.name = "storage.nvme.state",
              .value = std::string{paperbreak::storage::to_string(cache.state)},
              .unit = "state",
              .available = nvme != nullptr},
             {.name = "storage.nvme.queue.depth",
              .value = static_cast<std::uint64_t>(cache.queue_depth),
              .unit = "count",
              .available = nvme != nullptr},
             {.name = "storage.nvme.queue.capacity",
              .value = static_cast<std::uint64_t>(cache.queue_capacity),
              .unit = "count",
              .available = nvme != nullptr},
             {.name = "storage.nvme.queue.high_watermark",
              .value = static_cast<std::uint64_t>(cache.queue_high_watermark),
              .unit = "count",
              .available = nvme != nullptr},
             {.name = "storage.nvme.cache_bytes",
              .value = cache.current_cache_bytes,
              .unit = "bytes",
              .available = nvme != nullptr},
             {.name = "storage.nvme.rejected_blocks_total",
              .value = cache.rejected_blocks,
              .unit = "count",
              .available = nvme != nullptr},
             {.name = "storage.nvme.indexed_blocks",
              .value = static_cast<std::uint64_t>(cache.indexed_blocks),
              .unit = "count",
              .available = nvme != nullptr},
             {.name = "storage.nvme.event_leases.active",
              .value = static_cast<std::uint64_t>(cache.active_event_leases),
              .unit = "count",
              .available = nvme != nullptr},
             {.name = "storage.nvme.protected_blocks",
              .value = static_cast<std::uint64_t>(cache.protected_blocks),
              .unit = "count",
              .available = nvme != nullptr},
             {.name = "storage.nvme.protected_bytes",
              .value = cache.protected_bytes,
              .unit = "bytes",
              .available = nvme != nullptr},
             {.name = "storage.nvme.lease_failures_total",
              .value = cache.lease_failures,
              .unit = "count",
              .available = nvme != nullptr},
             {.name = "uplink.state",
              .value = uplink ? std::string{paperbreak::uplink::uplink_runtime_state_name(
                                    uplink_state.state)}
                              : std::string{"not-initialized"},
              .unit = "state",
              .available = uplink != nullptr},
             {.name = "uplink.last_heartbeat",
              .value = uplink_state.last_heartbeat_at,
              .unit = "rfc3339",
              .available = uplink != nullptr && !uplink_state.last_heartbeat_at.empty()},
             {.name = "event.current_count",
              .value = events.events_committed,
              .unit = "count",
              .available = runtime != nullptr},
             {.name = "event.frame_queue.depth",
              .value = static_cast<std::uint64_t>(events.frame_queue_depth),
              .unit = "count",
              .available = runtime != nullptr},
             {.name = "event.frame_queue.high_watermark",
              .value = static_cast<std::uint64_t>(events.frame_queue_high_watermark),
              .unit = "count",
              .available = runtime != nullptr},
             {.name = "event.persistence.queue.depth",
              .value = static_cast<std::uint64_t>(events.persistence_queue_depth),
              .unit = "count",
              .available = runtime != nullptr},
             {.name = "event.persistence.queue.capacity",
              .value = static_cast<std::uint64_t>(events.persistence_queue_capacity),
              .unit = "count",
              .available = runtime != nullptr},
             {.name = "event.persistence.active_events",
              .value = static_cast<std::uint64_t>(events.persistence_active_events),
              .unit = "count",
              .available = runtime != nullptr},
             {.name = "event.persistence.last_write_bytes",
              .value = events.persistence_last_write_bytes,
              .unit = "bytes",
              .available = runtime != nullptr},
             {.name = "event.persistence.last_write_duration_ms",
              .value = static_cast<std::uint64_t>(events.persistence_last_write_duration.count()),
              .unit = "milliseconds",
              .available = runtime != nullptr},
             {.name = "event.persistence.last_write_mib_per_second",
              .value = events.persistence_last_write_mib_per_second,
              .unit = "MiB/s",
              .available = runtime != nullptr},
             {.name = "event.failures_total",
              .value = events.event_failures,
              .unit = "count",
              .available = runtime != nullptr},
             {.name = "storage.watermark",
              .value = std::string{paperbreak::storage::to_string(policy.watermark)},
              .unit = "state",
              .available = storage != nullptr},
             {.name = "uplink.pending_upload_tasks",
              .value = upload_queue ? static_cast<std::uint64_t>(upload_queue.value().active_jobs)
                                    : std::uint64_t{0U},
              .unit = "count",
              .available = uplink != nullptr && static_cast<bool>(upload_queue)},
             {.name = "uplink.pending_upload_bytes",
              .value = upload_queue ? upload_queue.value().active_bytes : std::uint64_t{0U},
              .unit = "bytes",
              .available = uplink != nullptr && static_cast<bool>(upload_queue)}});
    }

  private:
    std::weak_ptr<paperbreak::service::EventRuntime> runtime_;
    std::weak_ptr<paperbreak::storage::StoragePolicyManager> storage_;
    std::weak_ptr<paperbreak::storage::NvmeRollingCache> nvme_;
    std::weak_ptr<paperbreak::uplink::UplinkRuntime> uplink_;
    std::weak_ptr<paperbreak::storage::EventMetadataDatabase> database_;
};

class CameraMetricSource final : public paperbreak::monitoring::IMetricSource
{
  public:
    CameraMetricSource(std::weak_ptr<ConfigurationResources> configuration,
                       std::weak_ptr<paperbreak::camera::CameraControlRuntime> cameras)
        : configuration_(std::move(configuration)), cameras_(std::move(cameras))
    {
    }

    [[nodiscard]] std::string_view source_name() const noexcept override
    {
        return "cameras";
    }

    [[nodiscard]] paperbreak::Result<std::vector<paperbreak::monitoring::MetricPoint>> collect(
        std::stop_token stop_token) noexcept override
    {
        using Point = paperbreak::monitoring::MetricPoint;
        auto configuration = configuration_.lock();
        auto cameras = cameras_.lock();
        if (!configuration || !cameras)
            return paperbreak::Result<std::vector<Point>>::failure(paperbreak::make_error(
                "SYS_MONITORING_SAMPLE_FAILED", paperbreak::Severity::warning, "相机指标源已失效",
                "monitoring", "monitoring.camera.collect", true));
        auto snapshot = configuration->repository.snapshot();
        if (!snapshot)
            return paperbreak::Result<std::vector<Point>>::failure(snapshot.error());
        std::vector<Point> points;
        points.reserve(snapshot.value().effective->cameras.size() * 13U);
        for (const auto& configured : snapshot.value().effective->cameras)
        {
            if (stop_token.stop_requested())
                break;
            const std::string prefix = "camera." + configured.id + '.';
            auto current = cameras->get(configured.id, configured.serial_number);
            if (!current)
            {
                points.push_back({.name = prefix + "state",
                                  .value = std::string{"unavailable"},
                                  .unit = "state",
                                  .available = false});
                continue;
            }
            points.push_back({.name = prefix + "state",
                              .value = std::string{paperbreak::camera::camera_control_state_name(
                                  current.value().state)},
                              .unit = "state"});
            const auto actual = current.value().actual;
            const auto add_optional = [&](const std::string& name,
                                          const std::optional<double> value,
                                          const std::string& unit) {
                points.push_back({.name = prefix + name,
                                  .value = value.value_or(0.0),
                                  .unit = unit,
                                  .available = value.has_value()});
            };
            add_optional("exposure_us", actual ? actual->exposure_us : std::optional<double>{},
                         "microseconds");
            add_optional("gain_db", actual ? actual->gain_db : std::optional<double>{}, "dB");
            add_optional("configured_fps", actual ? actual->frame_rate : std::optional<double>{},
                         "fps");
            const auto unavailable = [&](const std::string& name, const std::string& unit) {
                points.push_back(
                    {.name = prefix + name, .value = 0.0, .unit = unit, .available = false});
            };
            const auto acquisition = current.value().acquisition;
            points.push_back({.name = prefix + "actual_fps",
                              .value = acquisition ? acquisition->actual_fps : 0.0,
                              .unit = "fps",
                              .available = acquisition.has_value()});
            points.push_back(
                {.name = prefix + "dropped_frames_total",
                 .value = acquisition ? acquisition->camera_frame_gaps : std::uint64_t{0U},
                 .unit = "count",
                 .available = acquisition.has_value()});
            points.push_back(
                {.name = prefix + "receive_timeouts_total",
                 .value = acquisition ? acquisition->capture_timeouts : std::uint64_t{0U},
                 .unit = "count",
                 .available = acquisition.has_value()});
            const auto last_frame = acquisition
                                        ? acquisition->last_frame_wall_clock_time
                                        : std::optional<paperbreak::camera::WallClockTime>{};
            points.push_back(
                {.name = prefix + "last_frame_epoch_ms",
                 .value = last_frame ? static_cast<std::uint64_t>(
                                           std::chrono::duration_cast<std::chrono::milliseconds>(
                                               last_frame->time_since_epoch())
                                               .count())
                                     : std::uint64_t{0U},
                 .unit = "unix_milliseconds",
                 .available = last_frame.has_value()});
            unavailable("brightness", "level");
            unavailable("temperature_celsius", "celsius");
            unavailable("reconnects_total", "count");
            points.push_back({.name = prefix + "bandwidth_bytes_per_second",
                              .value = acquisition ? acquisition->bandwidth_bytes_per_second : 0.0,
                              .unit = "bytes/second",
                              .available = acquisition.has_value()});
        }
        return paperbreak::Result<std::vector<Point>>::success(std::move(points));
    }

  private:
    std::weak_ptr<ConfigurationResources> configuration_;
    std::weak_ptr<paperbreak::camera::CameraControlRuntime> cameras_;
};

nlohmann::json alarm_push_json(const paperbreak::monitoring::AlarmChange& change)
{
    nlohmann::json details = nlohmann::json::object();
    for (const auto& detail : change.alarm.details)
    {
        details[detail.key] = detail.value;
    }
    return {{"registryRevision", change.registry_revision},
            {"alarmId", change.alarm.alarm_id},
            {"revision", change.alarm.revision},
            {"code", change.alarm.code},
            {"severity", paperbreak::monitoring::severity_name(change.alarm.severity)},
            {"source", change.alarm.source},
            {"firstOccurredAt", change.alarm.first_occurred_at},
            {"lastOccurredAt", change.alarm.last_occurred_at},
            {"active", change.alarm.active},
            {"occurrenceCount", change.alarm.occurrence_count},
            {"message", change.alarm.message},
            {"details", std::move(details)},
            {"acknowledged", change.alarm.acknowledged}};
}

paperbreak::logging::Level logging_level_from_config(
    const paperbreak::config::LogLevel level) noexcept
{
    using ConfigLevel = paperbreak::config::LogLevel;
    using RuntimeLevel = paperbreak::logging::Level;
    switch (level)
    {
    case ConfigLevel::trace:
        return RuntimeLevel::trace;
    case ConfigLevel::debug:
        return RuntimeLevel::debug;
    case ConfigLevel::info:
        return RuntimeLevel::info;
    case ConfigLevel::warning:
        return RuntimeLevel::warning;
    case ConfigLevel::error:
        return RuntimeLevel::error;
    case ConfigLevel::critical:
        return RuntimeLevel::critical;
    }
    return RuntimeLevel::info;
}

class HostedRuntime final : public paperbreak::service::windows::IHostedService
{
  public:
    explicit HostedRuntime(
        std::vector<std::unique_ptr<paperbreak::service::ILifecycleComponent>> components,
        std::shared_ptr<paperbreak::service::ServiceStatusStore> status)
        : runtime_(std::move(components)), status_(std::move(status))
    {
    }

    [[nodiscard]] paperbreak::Result<paperbreak::service::StartOutcome> start() override
    {
        status_->set_state(paperbreak::service::ServiceState::starting);
        auto result = runtime_.start();
        if (!result)
        {
            status_->set_state(paperbreak::service::ServiceState::failed);
        }
        else if (result.value() == paperbreak::service::StartOutcome::cancelled)
        {
            status_->set_state(paperbreak::service::ServiceState::stopped);
        }
        else
        {
            status_->set_state(paperbreak::service::ServiceState::running);
        }
        return result;
    }

    void request_stop(const paperbreak::service::StopReason reason) noexcept override
    {
        status_->set_state(paperbreak::service::ServiceState::stop_requested);
        runtime_.request_stop(reason);
    }

    [[nodiscard]] paperbreak::Result<void> shutdown() override
    {
        status_->set_state(paperbreak::service::ServiceState::draining);
        auto result = runtime_.shutdown();
        status_->set_state(result ? paperbreak::service::ServiceState::stopped
                                  : paperbreak::service::ServiceState::failed);
        return result;
    }

  private:
    paperbreak::service::ServiceRuntime runtime_;
    std::shared_ptr<paperbreak::service::ServiceStatusStore> status_;
};

void print_error(const paperbreak::Error& error)
{
    std::cerr << error.business_code << ": " << error.message;
    if (error.native_domain.has_value() || error.native_code.has_value())
    {
        std::cerr << " [native=" << error.native_domain.value_or("unknown") << ':'
                  << error.native_code.value_or("unknown") << ']';
    }
    for (const auto& detail : error.details)
    {
        std::cerr << " [" << detail.key << '=' << detail.value << ']';
    }
    std::cerr << '\n';
}

std::string portable_relative_path(const std::filesystem::path& path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

paperbreak::Result<std::string> read_upload_manifest(const std::filesystem::path& path)
{
    constexpr std::uintmax_t maximum_manifest_bytes = 8U * 1024U * 1024U;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0U || size > maximum_manifest_bytes)
        return paperbreak::Result<std::string>::failure(paperbreak::make_error(
            "UPLOAD_SOURCE_MISSING", paperbreak::Severity::error,
            "事件 manifest 不存在或超过上传上限", "uplink", "uplink.enqueue.manifest"));
    std::ifstream stream{path, std::ios::binary};
    std::string contents(static_cast<std::size_t>(size), '\0');
    if (!stream || !stream.read(contents.data(), static_cast<std::streamsize>(contents.size())))
        return paperbreak::Result<std::string>::failure(
            paperbreak::make_error("UPLOAD_SOURCE_MISSING", paperbreak::Severity::error,
                                   "无法读取事件 manifest", "uplink", "uplink.enqueue.manifest"));
    return paperbreak::Result<std::string>::success(std::move(contents));
}

paperbreak::Result<void> enqueue_committed_event_uploads(
    paperbreak::uplink::PersistentUploadScheduler& scheduler,
    const paperbreak::storage::EventMetadataRecord& record, const std::filesystem::path& event_root,
    const std::string_view machine_id, const std::string_view upload_policy)
{
    if (upload_policy == "never" ||
        (upload_policy == "confirmed" && record.event_state != "Confirmed"))
        return paperbreak::Result<void>::success();
    auto manifest = read_upload_manifest(event_root / record.relative_directory / "manifest.json");
    if (!manifest)
        return paperbreak::Result<void>::failure(manifest.error());
    auto document = nlohmann::json::parse(manifest.value(), nullptr, false);
    if (!document.is_object() || !document.contains("fileSizes") ||
        !document.at("fileSizes").is_object() || !document.contains("fileChecksums") ||
        !document.at("fileChecksums").is_object())
        return paperbreak::Result<void>::failure(paperbreak::make_error(
            "UPLOAD_JOB_INVALID", paperbreak::Severity::error,
            "事件 manifest 缺少文件大小或校验对象", "uplink", "uplink.enqueue.manifest"));
    const std::string event_json = nlohmann::json{
        {"state", record.event_state},
        {"candidateTimeUtcMs", record.candidate_time_utc_ms},
        {"confirmedTimeUtcMs", record.confirmed_time_utc_ms},
        {"cameraIds", record.camera_ids},
        {"triggerCameraId", record.trigger_camera_id},
        {"triggerFrameNumber", record.trigger_frame_number},
        {"triggerReason", record.trigger_reason},
        {"confidence",
         record.confidence}}.dump();
    const auto created_at = std::max<std::int64_t>(0, record.candidate_time_utc_ms);
    const std::string key_prefix = std::string{machine_id} + ":" + record.event_id + ":";
    auto alarm = scheduler.enqueue({.idempotency_key = key_prefix + "alarm",
                                    .event_id = record.event_id,
                                    .kind = paperbreak::storage::UploadJobKind::alarm_metadata,
                                    .logical_id = "alarm",
                                    .payload_json = event_json,
                                    .upload_bytes = std::max<std::uint64_t>(1U, event_json.size()),
                                    .created_at_utc_ms = created_at});
    if (!alarm)
        return paperbreak::Result<void>::failure(alarm.error());

    const auto manifest_path = record.relative_directory / "manifest.json";
    auto manifest_job = scheduler.enqueue({.idempotency_key = key_prefix + "manifest",
                                           .event_id = record.event_id,
                                           .kind = paperbreak::storage::UploadJobKind::manifest,
                                           .logical_id = "manifest",
                                           .relative_path = portable_relative_path(manifest_path),
                                           .payload_json = event_json,
                                           .upload_bytes = manifest.value().size(),
                                           .created_at_utc_ms = created_at});
    if (!manifest_job)
        return paperbreak::Result<void>::failure(manifest_job.error());

    std::uint32_t key_frame_index = 0U;
    std::uint32_t replay_index = 0U;
    std::uint32_t raw_index = 0U;
    for (const auto& [relative_text, size_value] : document.at("fileSizes").items())
    {
        if (!size_value.is_number_unsigned() ||
            !document.at("fileChecksums").contains(relative_text) ||
            !document.at("fileChecksums").at(relative_text).is_string())
            return paperbreak::Result<void>::failure(paperbreak::make_error(
                "UPLOAD_JOB_INVALID", paperbreak::Severity::error, "事件 manifest 文件上传字段无效",
                "uplink", "uplink.enqueue.files"));
        const std::filesystem::path relative = path_from_utf8(relative_text);
        const bool key_frame = relative_text.starts_with("keyframes/");
        const bool replay = relative.extension() == ".mp4";
        auto kind = paperbreak::storage::UploadJobKind::raw_file;
        std::string logical_id;
        if (key_frame)
        {
            kind = paperbreak::storage::UploadJobKind::key_frame;
            logical_id = "keyframe-" + std::to_string(key_frame_index++);
        }
        else if (replay)
        {
            kind = paperbreak::storage::UploadJobKind::low_rate_replay;
            logical_id = "replay-" + std::to_string(replay_index++);
        }
        else
        {
            logical_id = "raw-" + std::to_string(raw_index++);
        }
        auto queued = scheduler.enqueue(
            {.idempotency_key = key_prefix + logical_id,
             .event_id = record.event_id,
             .kind = kind,
             .logical_id = logical_id,
             .relative_path = portable_relative_path(record.relative_directory / relative),
             .payload_json = event_json,
             .checksum = document.at("fileChecksums").at(relative_text).get<std::string>(),
             .upload_bytes = size_value.get<std::uint64_t>(),
             .created_at_utc_ms = created_at});
        if (!queued)
            return paperbreak::Result<void>::failure(queued.error());
    }
    return paperbreak::Result<void>::success();
}

paperbreak::Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>
create_hosted_service(const std::filesystem::path& config_path, const bool validate_config)
{
    static_cast<void>(validate_config);
    auto configuration = std::make_shared<ConfigurationResources>(config_path);
    auto loaded = configuration->repository.load();
    if (!loaded)
    {
        return paperbreak::Result<
            std::unique_ptr<paperbreak::service::windows::IHostedService>>::failure(loaded.error());
    }

    paperbreak::logging::LoggingConfig log_config;
    log_config.file_stem = "paperbreak-service";
    log_config.directory =
        resolve_config_path(config_path, loaded.value().effective->logging.directory);
    log_config.max_file_size_bytes =
        static_cast<std::size_t>(loaded.value().effective->logging.maximum_file_size_mib) * 1024U *
        1024U;
    log_config.max_files_per_day = loaded.value().effective->logging.maximum_files_per_day;
    log_config.queue_capacity = loaded.value().effective->logging.queue_capacity;
    log_config.minimum_level = logging_level_from_config(loaded.value().effective->logging.level);
    log_config.retention_days = loaded.value().effective->logging.retention_days;
    auto logging_result = paperbreak::logging::LoggingRuntime::create(log_config);
    if (!logging_result)
    {
        return paperbreak::Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::
            failure(logging_result.error());
    }

    std::shared_ptr<paperbreak::logging::LoggingRuntime> logging{std::move(logging_result).value()};
    const paperbreak::ThreadRegistrationFactory service_thread_registrar =
        [weak_logging = std::weak_ptr<paperbreak::logging::LoggingRuntime>{logging}](
            const std::string_view name) -> std::shared_ptr<void> {
        const auto runtime = weak_logging.lock();
        if (!runtime)
            return {};
        auto registration = runtime->register_current_thread(name);
        if (!registration)
            return {};
        return std::make_shared<paperbreak::logging::LoggingRuntime::ThreadRegistration>(
            std::move(registration).value());
    };
    const auto debug_diagnostics =
        [weak_logging = std::weak_ptr<paperbreak::logging::LoggingRuntime>{logging}](
            const paperbreak::logging::Category category) {
            return paperbreak::DebugDiagnosticSink{
                .enabled =
                    [weak_logging] {
                        const auto runtime = weak_logging.lock();
                        return runtime && runtime->enabled(paperbreak::logging::Level::debug);
                    },
                .record =
                    [weak_logging, category](std::string message) {
                        if (const auto runtime = weak_logging.lock())
                            static_cast<void>(
                                runtime->log(category, paperbreak::logging::Level::debug, message));
                    }};
        };
    auto audit_attach = configuration->audit.attach(logging);
    if (!audit_attach)
    {
        static_cast<void>(logging->shutdown());
        return paperbreak::Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::
            failure(audit_attach.error());
    }

    auto status = std::make_shared<paperbreak::service::ServiceStatusStore>();
    auto metrics = std::make_shared<paperbreak::monitoring::MetricRegistry>();
    auto alarms = std::make_shared<paperbreak::monitoring::AlarmRegistry>();
    const auto event_root =
        resolve_config_path(config_path, loaded.value().effective->storage.event_root);
    auto database_result = paperbreak::storage::EventMetadataDatabase::open(
        {.database_path = event_root / ".metadata" / "events.db",
         .event_root = event_root,
         .backup_directory = event_root / ".metadata" / "backups"});
    if (!database_result)
    {
        static_cast<void>(logging->shutdown());
        return paperbreak::Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::
            failure(database_result.error());
    }
    std::shared_ptr<paperbreak::storage::EventMetadataDatabase> event_database{
        std::move(database_result).value()};
    auto inspector_result = paperbreak::storage::EventInspector::create({.event_root = event_root});
    if (!inspector_result)
    {
        static_cast<void>(logging->shutdown());
        return paperbreak::Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::
            failure(inspector_result.error());
    }
    std::shared_ptr<paperbreak::storage::EventInspector> event_inspector{
        std::move(inspector_result).value()};
    constexpr std::uint64_t gibibyte = 1024ULL * 1024ULL * 1024ULL;
    const auto cache_root =
        resolve_config_path(config_path, loaded.value().effective->storage.cache_root);
    auto storage_policy_result = paperbreak::storage::StoragePolicyManager::create(
        {.event_root = event_root,
         .temporary_roots = {cache_root},
         .watermarks = {.warning_available_bytes =
                            loaded.value().effective->storage.warning_free_space_gib * gibibyte,
                        .critical_available_bytes =
                            loaded.value().effective->storage.critical_free_space_gib * gibibyte,
                        .stop_save_available_bytes =
                            loaded.value().effective->storage.stop_free_space_gib * gibibyte},
         .retention_age = std::chrono::days{loaded.value().effective->event.retention_days},
         .maximum_event_bytes =
             loaded.value().effective->storage.maximum_event_storage_gib * gibibyte},
        *event_database);
    if (!storage_policy_result)
    {
        static_cast<void>(logging->shutdown());
        return paperbreak::Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::
            failure(storage_policy_result.error());
    }
    std::shared_ptr<paperbreak::storage::StoragePolicyManager> storage_policy{
        std::move(storage_policy_result).value()};
    const std::weak_ptr<paperbreak::monitoring::AlarmRegistry> weak_event_alarms = alarms;
    const std::weak_ptr<paperbreak::logging::LoggingRuntime> weak_event_logging = logging;
    std::shared_ptr<paperbreak::storage::NvmeRollingCache> nvme_cache;
    if (loaded.value().effective->storage.rolling_cache_enabled)
    {
        std::vector<paperbreak::storage::NvmeCameraLayout> layouts;
        for (const auto& camera : loaded.value().effective->cameras)
        {
            if (!camera.enabled)
                continue;
            const std::uint64_t bytes_per_pixel =
                camera.pixel_format == paperbreak::config::PixelFormat::mono10 ||
                        camera.pixel_format == paperbreak::config::PixelFormat::mono12
                    ? 2U
                    : 1U;
            const auto maximum_frame_bytes =
                static_cast<std::uint64_t>(camera.roi.width) * camera.roi.height * bytes_per_pixel;
            const auto index_capacity =
                static_cast<std::uint32_t>(std::ceil(camera.frame_rate)) + 2U;
            const auto required_payload = static_cast<std::uint64_t>(
                std::ceil(static_cast<long double>(maximum_frame_bytes) * camera.frame_rate));
            const auto required_metadata =
                static_cast<std::uint64_t>(std::ceil(camera.frame_rate)) *
                    paperbreak::storage::nvme_index_entry_bytes +
                2U * paperbreak::storage::nvme_page_bytes;
            layouts.push_back(
                {.camera_id = camera.id,
                 .maximum_frame_bytes = static_cast<std::uint32_t>(maximum_frame_bytes),
                 .index_capacity = index_capacity,
                 .required_input_bytes_per_second = required_payload + required_metadata});
        }
        auto cache_result = paperbreak::storage::NvmeRollingCache::create(
            {.root = cache_root,
             .maximum_cache_bytes =
                 loaded.value().effective->storage.maximum_cache_storage_gib * gibibyte,
             .write_limit_bytes_per_second =
                 loaded.value().effective->storage.rolling_cache_write_limit_mibps * 1024ULL *
                 1024ULL,
             .io_timeout =
                 std::chrono::milliseconds{
                     loaded.value().effective->storage.rolling_cache_io_timeout_ms},
             .cameras = std::move(layouts),
             .error_observer =
                 [weak_event_alarms, weak_event_logging](const paperbreak::Error& error) {
                     if (auto alarm_registry = weak_event_alarms.lock())
                     {
                         static_cast<void>(alarm_registry->raise_alarm(
                             {.code = error.business_code,
                              .severity = error.severity,
                              .source = error.source_id.value_or("nvme"),
                              .message = error.message,
                              .details = error.details}));
                     }
                     if (auto log_runtime = weak_event_logging.lock())
                     {
                         static_cast<void>(
                             log_runtime->log(paperbreak::logging::Category::storage,
                                              error.severity == paperbreak::Severity::critical
                                                  ? paperbreak::logging::Level::critical
                                              : error.severity == paperbreak::Severity::warning
                                                  ? paperbreak::logging::Level::warning
                                                  : paperbreak::logging::Level::error,
                                              error.business_code + ": " + error.message));
                     }
                 },
             .register_thread = service_thread_registrar,
             .diagnostics = debug_diagnostics(paperbreak::logging::Category::storage)});
        if (!cache_result)
        {
            static_cast<void>(logging->shutdown());
            return paperbreak::
                Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::failure(
                    cache_result.error());
        }
        nvme_cache = std::move(cache_result).value();
    }
    std::shared_ptr<paperbreak::uplink::IUplinkTransport> uplink_transport;
    std::shared_ptr<paperbreak::uplink::PersistentUploadScheduler> upload_scheduler;
    if (loaded.value().effective->uplink.enabled)
    {
        auto transport_result = paperbreak::uplink::QtUplinkTransport::create(
            {.server_url = loaded.value().effective->uplink.server_url,
             .io_timeout =
                 std::chrono::milliseconds{loaded.value().effective->uplink.io_timeout_ms},
             .chunk_bytes = loaded.value().effective->uplink.chunk_bytes,
             .upload_limit_bytes_per_second =
                 loaded.value().effective->uplink.upload_limit_mibps * 1024ULL * 1024ULL,
             .register_thread = service_thread_registrar});
        if (!transport_result)
        {
            static_cast<void>(logging->shutdown());
            return paperbreak::
                Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::failure(
                    transport_result.error());
        }
        uplink_transport = std::shared_ptr<paperbreak::uplink::IUplinkTransport>{
            std::move(transport_result).value()};
        auto executor = paperbreak::uplink::make_chunked_upload_executor(
            uplink_transport, {.event_root = event_root,
                               .machine_id = loaded.value().effective->system.machine_id,
                               .chunk_bytes = loaded.value().effective->uplink.chunk_bytes});
        if (!executor)
        {
            static_cast<void>(logging->shutdown());
            return paperbreak::
                Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::failure(
                    executor.error());
        }
        auto scheduler_result = paperbreak::uplink::PersistentUploadScheduler::create(
            event_database,
            {.register_thread = service_thread_registrar,
             .diagnostics = debug_diagnostics(paperbreak::logging::Category::uplink),
             .integrity_failure_observer =
                 [alarms](const std::string_view event_id, const std::string_view error_code) {
                     static_cast<void>(alarms->raise_alarm(
                         {.code = "EVENT_INTEGRITY_FAILED",
                          .severity = paperbreak::Severity::critical,
                          .source = std::string{event_id},
                          .message = "上传源文件完整性校验失败，事件已转人工处理",
                          .details = {{"errorCode", std::string{error_code}}}}));
                 }},
            std::move(executor).value());
        if (!scheduler_result)
        {
            static_cast<void>(logging->shutdown());
            return paperbreak::
                Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::failure(
                    scheduler_result.error());
        }
        upload_scheduler = std::shared_ptr<paperbreak::uplink::PersistentUploadScheduler>{
            std::move(scheduler_result).value()};
        std::size_t offset = 0U;
        while (true)
        {
            auto page = event_database->query_events({.offset = offset, .limit = 200U});
            if (!page)
            {
                static_cast<void>(logging->shutdown());
                return paperbreak::
                    Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::failure(
                        page.error());
            }
            for (const auto& event : page.value().events)
            {
                if (event.storage_state != "Present")
                    continue;
                auto queued =
                    enqueue_committed_event_uploads(*upload_scheduler, event, event_root,
                                                    loaded.value().effective->system.machine_id,
                                                    loaded.value().effective->event.upload_policy);
                if (!queued)
                {
                    static_cast<void>(logging->shutdown());
                    return paperbreak::Result<std::unique_ptr<
                        paperbreak::service::windows::IHostedService>>::failure(queued.error());
                }
            }
            offset += page.value().events.size();
            if (offset >= page.value().total || page.value().events.empty())
                break;
        }
    }
    const std::weak_ptr<paperbreak::uplink::PersistentUploadScheduler> weak_upload_scheduler =
        upload_scheduler;
    auto event_ipc_target = std::make_shared<std::weak_ptr<paperbreak::ipc::IpcServer>>();
    auto camera_ipc_target = std::make_shared<std::weak_ptr<paperbreak::ipc::IpcServer>>();
    const auto publish_event_lifecycle = [event_ipc_target](
                                             const paperbreak::storage::EventMetadataRecord& event,
                                             const std::string_view event_name) {
        if (auto server = event_ipc_target->lock())
        {
            nlohmann::json payload{{"eventId", event.event_id},
                                   {"eventState", event.event_state},
                                   {"decisionState", event.decision_state},
                                   {"persistenceState", event.persistence_state},
                                   {"reviewState", event.review_state},
                                   {"reviewDecision", event.review_decision
                                                          ? nlohmann::json{*event.review_decision}
                                                          : nlohmann::json{nullptr}},
                                   {"artifactsAvailable", event.artifacts_available},
                                   {"triggerCount", event.trigger_count},
                                   {"candidateTimeUtcMs", event.candidate_time_utc_ms}};
            static_cast<void>(server->try_publish({.event_name = std::string{event_name},
                                                   .timestamp = paperbreak::current_utc_timestamp(),
                                                   .payload_json = payload.dump(),
                                                   .binary = {},
                                                   .coalescing_key = event.event_id},
                                                  paperbreak::ipc::PushPolicy::coalesce_latest));
        }
    };
    const auto enqueue_event_for_upload =
        [weak_upload_scheduler, event_root,
         machine_id = loaded.value().effective->system.machine_id, configuration, weak_event_alarms,
         weak_event_logging](const paperbreak::storage::EventMetadataRecord& event) {
            const auto scheduler = weak_upload_scheduler.lock();
            if (!scheduler || event.storage_state != "Present")
                return;
            auto snapshot = configuration->repository.snapshot();
            auto queued = snapshot ? enqueue_committed_event_uploads(
                                         *scheduler, event, event_root, machine_id,
                                         snapshot.value().effective->event.upload_policy)
                                   : paperbreak::Result<void>::failure(snapshot.error());
            if (queued)
                return;
            if (auto alarm_registry = weak_event_alarms.lock())
            {
                static_cast<void>(alarm_registry->raise_alarm({.code = queued.error().business_code,
                                                               .severity = queued.error().severity,
                                                               .source = "uplink",
                                                               .message = queued.error().message,
                                                               .details = queued.error().details}));
            }
            if (auto log_runtime = weak_event_logging.lock())
            {
                static_cast<void>(log_runtime->log(
                    paperbreak::logging::Category::uplink, paperbreak::logging::Level::error,
                    queued.error().business_code + ": " + queued.error().message));
            }
        };
    const auto on_event_committed = [enqueue_event_for_upload, publish_event_lifecycle](
                                        const paperbreak::storage::EventMetadataRecord& event) {
        publish_event_lifecycle(event, "event.committed");
        enqueue_event_for_upload(event);
    };
    const auto on_event_reviewed = [enqueue_event_for_upload, publish_event_lifecycle](
                                       const paperbreak::storage::EventMetadataRecord& event) {
        publish_event_lifecycle(event, "event.lifecycleChanged");
        enqueue_event_for_upload(event);
    };
    auto event_runtime_result = paperbreak::service::EventRuntime::create(
        {.configuration = *loaded.value().effective,
         .event_root = event_root,
         .database = event_database,
         .storage_policy = storage_policy,
         .nvme_cache = nvme_cache,
         .error_observer =
             [weak_event_alarms, weak_event_logging](const paperbreak::Error& error) {
                 const bool algorithm_error = error.business_code.starts_with("ALGORITHM_");
                 const bool algorithm_alarm = error.business_code == "ALGORITHM_DEGRADED";
                 if (auto alarm_registry = weak_event_alarms.lock();
                     (!algorithm_error || algorithm_alarm) && alarm_registry)
                 {
                     static_cast<void>(alarm_registry->raise_alarm(
                         {.code = error.business_code,
                          .severity = error.severity,
                          .source =
                              error.source_id.value_or(algorithm_error ? "algorithm" : "event"),
                          .message = error.message,
                          .details = error.details}));
                 }
                 if (auto log_runtime = weak_event_logging.lock())
                 {
                     const auto level = error.severity == paperbreak::Severity::critical
                                            ? paperbreak::logging::Level::critical
                                        : error.severity == paperbreak::Severity::warning
                                            ? paperbreak::logging::Level::warning
                                            : paperbreak::logging::Level::error;
                     static_cast<void>(
                         log_runtime->log(algorithm_error ? paperbreak::logging::Category::algorithm
                                                          : paperbreak::logging::Category::event,
                                          level, error.business_code + ": " + error.message));
                 }
             },
         .detector_failure_state_observer =
             [weak_event_alarms](
                 const paperbreak::service::AlgorithmDetectorFailureStateChange& change) {
                 if (auto alarm_registry = weak_event_alarms.lock())
                 {
                     if (change.active)
                     {
                         std::vector<paperbreak::ErrorDetail> details{
                             {"failureLimit", std::to_string(change.failure_limit)},
                             {"consecutiveFailures", std::to_string(change.consecutive_failures)},
                             {"detectorFailures", std::to_string(change.detector_failures)}};
                         if (change.last_error)
                         {
                             details.push_back(
                                 {"lastBusinessCode", change.last_error->business_code});
                             const auto reason =
                                 std::ranges::find_if(change.last_error->details,
                                                      [](const paperbreak::ErrorDetail& detail) {
                                                          return detail.key == "reason";
                                                      });
                             if (reason != change.last_error->details.end())
                                 details.push_back(*reason);
                         }
                         static_cast<void>(alarm_registry->raise_alarm(
                             {.code = "ALGORITHM_PROCESS_FAILED",
                              .severity = paperbreak::Severity::error,
                              .source = change.camera_id,
                              .message = "算法检测连续失败，自动检测仍在继续",
                              .details = std::move(details)}));
                     }
                     else
                     {
                         static_cast<void>(
                             alarm_registry->clear("ALGORITHM_PROCESS_FAILED", change.camera_id));
                     }
                 }
             },
         .backlog_state_observer =
             [weak_event_alarms](const paperbreak::service::AlgorithmBacklogStateChange& change) {
                 if (auto alarm_registry = weak_event_alarms.lock())
                 {
                     if (change.active)
                     {
                         static_cast<void>(alarm_registry->raise_alarm(
                             {.code = "ALGORITHM_QUEUE_BACKLOG",
                              .severity = paperbreak::Severity::warning,
                              .source = change.camera_id,
                              .message = "算法队列积压，已跳过最旧待检测帧",
                              .details = {
                                  {"queueDepth", std::to_string(change.queue_depth)},
                                  {"queueCapacity", std::to_string(change.queue_capacity)},
                                  {"skippedFrames", std::to_string(change.skipped_frames)}}}));
                     }
                     else
                     {
                         static_cast<void>(
                             alarm_registry->clear("ALGORITHM_QUEUE_BACKLOG", change.camera_id));
                     }
                 }
             },
         .lifecycle_observer =
             [publish_event_lifecycle](const paperbreak::storage::EventMetadataRecord& event) {
                 publish_event_lifecycle(event, "event.lifecycleChanged");
             },
         .committed_observer = on_event_committed,
         .register_thread = service_thread_registrar,
         .diagnostics = debug_diagnostics(paperbreak::logging::Category::algorithm)});
    if (!event_runtime_result)
    {
        static_cast<void>(logging->shutdown());
        return paperbreak::Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::
            failure(event_runtime_result.error());
    }
    auto event_runtime = std::move(event_runtime_result).value();
    std::shared_ptr<paperbreak::pipeline::PreviewRuntime> preview;
    std::shared_ptr<paperbreak::camera::ICameraProvider> camera_provider{
        paperbreak::camera::hikrobot::create_hikrobot_camera_provider()};
    std::shared_ptr<paperbreak::camera::CameraControlRuntime> cameras;
    std::shared_ptr<PreviewPublisher> preview_publisher;
    if (loaded.value().effective->preview.enabled)
    {
        std::vector<std::string> camera_ids;
        for (const auto& camera : loaded.value().effective->cameras)
        {
            if (camera.enabled)
                camera_ids.push_back(camera.id);
        }
        if (!camera_ids.empty())
        {
            preview_publisher = std::make_shared<PreviewPublisher>();
            paperbreak::pipeline::PreviewRuntimeOptions preview_options{
                .frames_per_second = loaded.value().effective->preview.fps,
                .encoding = {.maximum_width = loaded.value().effective->preview.max_width,
                             .maximum_height = loaded.value().effective->preview.max_height,
                             .jpeg_quality = loaded.value().effective->preview.jpeg_quality},
                .register_thread = service_thread_registrar};
            preview_options.diagnostics = debug_diagnostics(paperbreak::logging::Category::ipc);
            preview = std::make_shared<paperbreak::pipeline::PreviewRuntime>(
                std::move(camera_ids), paperbreak::pipeline::make_opencv_preview_encoder(),
                [preview_publisher](paperbreak::pipeline::PreviewDelivery delivery) {
                    preview_publisher->publish(std::move(delivery));
                },
                preview_options);
        }
    }
    const std::weak_ptr<paperbreak::pipeline::PreviewRuntime> weak_preview = preview;
    const std::weak_ptr<paperbreak::service::EventRuntime> weak_event_runtime = event_runtime;
    const std::weak_ptr<paperbreak::storage::NvmeRollingCache> weak_nvme_cache = nvme_cache;
    paperbreak::camera::CameraFrameDeliveryOptions delivery_options{
        .frame_pool_capacity = loaded.value().effective->acquisition.frame_pool_capacity,
        .queue_capacity = loaded.value().effective->acquisition.queue_capacity,
        .receive_timeout =
            std::chrono::milliseconds{loaded.value().effective->acquisition.receive_timeout_ms},
        .register_thread = service_thread_registrar};
    delivery_options.diagnostics = debug_diagnostics(paperbreak::logging::Category::camera);
    cameras = std::make_shared<paperbreak::camera::CameraControlRuntime>(
        std::move(camera_provider),
        [weak_preview, weak_event_runtime, weak_nvme_cache,
         weak_event_logging](paperbreak::camera::FrameView frame) {
            const std::string camera_id = frame.camera_id();
            const auto sequence_number = frame.sequence_number();
            bool event_accepted = false;
            bool nvme_accepted = false;
            const bool preview_accepted = !weak_preview.expired();
            if (auto runtime = weak_preview.lock())
                runtime->submit(frame, {.camera_status = "acquiring"});
            if (auto runtime = weak_event_runtime.lock())
                event_accepted = static_cast<bool>(runtime->submit_frame(frame));
            if (auto runtime = weak_nvme_cache.lock())
                nvme_accepted = static_cast<bool>(runtime->submit_frame(frame));
            if (const auto log_runtime = weak_event_logging.lock();
                log_runtime && log_runtime->enabled(paperbreak::logging::Level::debug))
                static_cast<void>(log_runtime->log(
                    paperbreak::logging::Category::camera, paperbreak::logging::Level::debug,
                    "operation=frame.forward cameraId=" + camera_id +
                        " sequenceNumber=" + std::to_string(sequence_number) +
                        " eventAccepted=" + (event_accepted ? "true" : "false") +
                        " nvmeAccepted=" + (nvme_accepted ? "true" : "false") +
                        " previewAccepted=" + (preview_accepted ? "true" : "false")));
        },
        delivery_options,
        [camera_ipc_target, configuration](const std::string_view camera_id,
                                           const paperbreak::camera::LineInputEvent& event) {
            auto snapshot = configuration->repository.snapshot();
            if (!snapshot)
                return;
            const auto configured = std::ranges::find_if(
                snapshot.value().effective->cameras,
                [camera_id](const auto& item) { return item.id == camera_id; });
            if (configured == snapshot.value().effective->cameras.end())
                return;
            const bool alarm_active =
                configured->line_io.alarm_active_level == paperbreak::config::AlarmActiveLevel::high
                    ? event.raw_level
                    : !event.raw_level;
            if (auto server = camera_ipc_target->lock())
            {
                static_cast<void>(server->try_publish(
                    {.event_name = "camera.lineInputChanged",
                     .timestamp = paperbreak::current_utc_timestamp(),
                     .payload_json = nlohmann::json{{"cameraId", camera_id},
                                                    {"rawLevel", event.raw_level},
                                                    {"alarmActive", alarm_active},
                                                    {"revision", event.revision},
                                                    {"timestampUtcMs", event.timestamp_utc_ms}}
                                         .dump(),
                     .binary = {},
                     .coalescing_key = "camera.lineInputChanged:" + std::string{camera_id}},
                    paperbreak::ipc::PushPolicy::coalesce_latest));
            }
        });
    auto commands = std::make_shared<paperbreak::service::SystemCommandService>(
        configuration->repository, status, metrics, alarms, logging, config_path.parent_path(),
        preview, cameras, event_runtime, event_database, event_inspector, on_event_reviewed);
    std::shared_ptr<paperbreak::uplink::UplinkRuntime> uplink_runtime;
    if (uplink_transport && upload_scheduler)
    {
        const auto machine_id = loaded.value().effective->system.machine_id;
        const auto production_line_id = loaded.value().effective->system.production_line_id;
        paperbreak::uplink::UplinkRuntimeConfig runtime_config{
            .session_hello = {.request_id = "session-" + machine_id,
                              .machine_id = machine_id,
                              .production_line_id = production_line_id,
                              .software_version =
                                  std::string{paperbreak::version_info().application_version},
                              .supported_protocol_versions = {paperbreak::uplink::protocol_version},
                              .capabilities = {"system.requestStatus", "config.replace",
                                               "event.review", "event.retryUpload",
                                               "camera.discover", "camera.bind", "camera.connect",
                                               "camera.disconnect", "camera.start", "camera.stop",
                                               "camera.updateConfig", "camera.captureSnapshot",
                                               "camera.softwareTrigger"}},
            .register_thread = service_thread_registrar,
            .diagnostics = debug_diagnostics(paperbreak::logging::Category::uplink)};
        auto runtime_result = paperbreak::uplink::UplinkRuntime::create(
            uplink_transport, std::move(runtime_config),
            [status, event_database] {
                const auto service = status->snapshot();
                auto uploads = event_database->upload_queue_stats();
                if (!uploads)
                    return paperbreak::Result<std::string>::failure(uploads.error());
                return paperbreak::Result<std::string>::success(nlohmann::json{
                    {"serviceState", paperbreak::service::service_state_name(service.state)},
                    {"acceptingWrites", service.accepting_writes},
                    {"pendingUploadJobs", uploads.value().active_jobs},
                    {"pendingUploadBytes",
                     uploads.value().active_bytes}}.dump());
            },
            [commands](const paperbreak::uplink::RemoteCommand& command,
                       const std::stop_token stop_token) {
                return commands->handle_uplink_command(command, stop_token);
            });
        if (!runtime_result)
        {
            static_cast<void>(logging->shutdown());
            return paperbreak::
                Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::failure(
                    runtime_result.error());
        }
        uplink_runtime =
            std::shared_ptr<paperbreak::uplink::UplinkRuntime>{std::move(runtime_result).value()};
    }
    paperbreak::ipc::IpcServerOptions ipc_options;
    ipc_options.register_thread = service_thread_registrar;
    ipc_options.diagnostics = debug_diagnostics(paperbreak::logging::Category::ipc);
    auto ipc_server = std::make_shared<paperbreak::ipc::IpcServer>(
        commands, paperbreak::ipc::make_windows_peer_authorizer(), std::move(ipc_options));
    *event_ipc_target = ipc_server;
    *camera_ipc_target = ipc_server;
    if (preview_publisher)
        preview_publisher->set_server(ipc_server);

    auto monitoring_options = monitoring_options_from_config(*loaded.value().effective);
    monitoring_options.register_thread = service_thread_registrar;
    auto monitor = std::make_shared<paperbreak::monitoring::HealthMonitor>(
        metrics, alarms, std::move(monitoring_options));
    auto system_volume = paperbreak::platform::windows_system_volume();
    if (!system_volume)
    {
        static_cast<void>(logging->shutdown());
        return paperbreak::Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::
            failure(system_volume.error());
    }
    std::vector<paperbreak::platform::DiskMetricPath> disk_paths{
        {.label = "system", .path = system_volume.value()},
        {.label = "event", .path = event_root},
        {.label = "cache", .path = cache_root},
        {.label = "log", .path = log_config.directory}};
    const std::array<std::shared_ptr<paperbreak::monitoring::IMetricSource>, 6U> sources{
        paperbreak::platform::make_windows_system_metric_source(std::move(disk_paths)),
        std::make_shared<IpcMetricSource>(ipc_server),
        std::make_shared<DatabaseMetricSource>(event_database),
        std::make_shared<EventMetricSource>(event_runtime, storage_policy, nvme_cache,
                                            uplink_runtime, event_database),
        std::make_shared<CameraMetricSource>(configuration, cameras),
        paperbreak::service::make_algorithm_metric_source(event_runtime)};
    for (const auto& source : sources)
    {
        auto registered = monitor->register_source(source);
        if (!registered)
        {
            static_cast<void>(logging->shutdown());
            return paperbreak::
                Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::failure(
                    registered.error());
        }
    }

    auto logging_applier = std::make_shared<LoggingConfigApplier>(logging);
    auto registered_applier = configuration->repository.register_applier(*logging_applier);
    if (!registered_applier)
    {
        static_cast<void>(logging->shutdown());
        return paperbreak::Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::
            failure(registered_applier.error());
    }
    configuration->dynamic_appliers.push_back(logging_applier);

    auto monitoring_applier = std::make_shared<MonitoringConfigApplier>(monitor);
    registered_applier = configuration->repository.register_applier(*monitoring_applier);
    if (!registered_applier)
    {
        static_cast<void>(logging->shutdown());
        return paperbreak::Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::
            failure(registered_applier.error());
    }
    configuration->dynamic_appliers.push_back(monitoring_applier);

    auto event_applier = std::make_shared<EventConfigApplier>(event_runtime, storage_policy);
    registered_applier = configuration->repository.register_applier(*event_applier);
    if (!registered_applier)
    {
        static_cast<void>(logging->shutdown());
        return paperbreak::Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::
            failure(registered_applier.error());
    }
    configuration->dynamic_appliers.push_back(event_applier);

    const std::weak_ptr<paperbreak::ipc::IpcServer> weak_server = ipc_server;
    status->set_observer([weak_server](const paperbreak::service::ServiceStatusSnapshot& snapshot) {
        if (auto server = weak_server.lock())
        {
            nlohmann::json payload{
                {"serviceState", paperbreak::service::service_state_name(snapshot.state)},
                {"acceptingWrites", snapshot.accepting_writes}};
            static_cast<void>(server->try_publish({.event_name = "status.changed",
                                                   .timestamp = paperbreak::current_utc_timestamp(),
                                                   .payload_json = payload.dump(),
                                                   .binary = {},
                                                   .coalescing_key = "status.changed"},
                                                  paperbreak::ipc::PushPolicy::coalesce_latest));
        }
    });
    alarms->set_observer([weak_server](const paperbreak::monitoring::AlarmChange& change) {
        if (auto server = weak_server.lock())
        {
            static_cast<void>(server->try_publish(
                {.event_name = std::string{paperbreak::monitoring::alarm_change_name(change.kind)},
                 .timestamp = paperbreak::current_utc_timestamp(),
                 .payload_json = alarm_push_json(change).dump(),
                 .binary = {},
                 .coalescing_key = {}},
                paperbreak::ipc::PushPolicy::drop_newest));
        }
    });

    std::vector<std::unique_ptr<paperbreak::service::ILifecycleComponent>> components;
    components.push_back(std::make_unique<ConfigurationLifecycleComponent>(configuration));
    components.push_back(std::make_unique<LoggingLifecycleComponent>(logging));
    if (preview)
        components.push_back(std::make_unique<PreviewLifecycleComponent>(preview));
    components.push_back(std::make_unique<EventLifecycleComponent>(event_runtime));
    if (nvme_cache)
        components.push_back(std::make_unique<NvmeLifecycleComponent>(nvme_cache));
    components.push_back(std::make_unique<StorageMaintenanceLifecycleComponent>(
        storage_policy, alarms, nvme_cache, service_thread_registrar));
    components.push_back(std::make_unique<IpcLifecycleComponent>(ipc_server));
    if (uplink_runtime && upload_scheduler)
        components.push_back(
            std::make_unique<UplinkLifecycleComponent>(uplink_runtime, upload_scheduler));
    components.push_back(std::make_unique<MonitoringLifecycleComponent>(monitor));
    return paperbreak::Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::
        success(std::make_unique<HostedRuntime>(std::move(components), std::move(status)));
}

int run_console(const Arguments& arguments)
{
    auto service_result = create_hosted_service(arguments.config_path, false);
    if (!service_result)
    {
        print_error(service_result.error());
        return 1;
    }
    auto service = std::move(service_result).value();
    StopRequestChannel stop_channel;

    auto registration_result = paperbreak::service::windows::ConsoleControlRegistration::create(
        [&service, &stop_channel](const paperbreak::service::StopReason reason) {
            service->request_stop(reason);
            stop_channel.request(reason);
        });
    if (!registration_result)
    {
        print_error(registration_result.error());
        return 1;
    }
    [[maybe_unused]] auto registration = std::move(registration_result).value();

    auto start_result = service->start();
    if (!start_result)
    {
        print_error(start_result.error());
        return 1;
    }
    if (start_result.value() == paperbreak::service::StartOutcome::cancelled)
    {
        return 0;
    }

    std::cout << paperbreak::format_version_info() << '\n';
    std::cout << "PaperBreakEdgeService 正在 console 模式运行。\n";
    if (arguments.run_for_present && arguments.run_for.count() > 0)
    {
        if (!stop_channel.wait_for(arguments.run_for))
        {
            service->request_stop(paperbreak::service::StopReason::test_deadline);
        }
    }
    else
    {
        std::cout << "按 Ctrl+C 请求受控退出。\n";
        static_cast<void>(stop_channel.wait());
    }

    const auto shutdown_result = service->shutdown();
    if (!shutdown_result)
    {
        print_error(shutdown_result.error());
        return 1;
    }
    return 0;
}

paperbreak::Result<std::filesystem::path> absolute_config_path(
    const std::filesystem::path& config_path)
{
    std::error_code error_code;
    auto absolute = std::filesystem::weakly_canonical(config_path, error_code);
    if (error_code)
    {
        auto error = argument_error("无法规范化配置文件绝对路径");
        error.native_domain = "std::error_code";
        error.native_code = std::to_string(error_code.value());
        return paperbreak::Result<std::filesystem::path>::failure(std::move(error));
    }
    return paperbreak::Result<std::filesystem::path>::success(std::move(absolute));
}

int run_install(const Arguments& arguments)
{
    auto executable_result = paperbreak::service::windows::current_executable_path();
    if (!executable_result)
    {
        print_error(executable_result.error());
        return 1;
    }
    auto config_result = absolute_config_path(arguments.config_path);
    if (!config_result)
    {
        print_error(config_result.error());
        return 2;
    }

    paperbreak::service::windows::ServiceDefinition definition;
    definition.command_line = paperbreak::service::windows::build_service_command_line(
        executable_result.value(), config_result.value());
    auto api = paperbreak::service::windows::make_windows_service_manager_api();
    paperbreak::service::windows::ServiceManager manager{*api};
    auto install_result = manager.install(definition);
    if (!install_result)
    {
        print_error(install_result.error());
        return 1;
    }

    std::cout << (install_result.value() == paperbreak::service::windows::InstallOutcome::created
                      ? "Windows 服务安装完成。"
                      : "Windows 服务配置已收敛。")
              << '\n';
    return 0;
}

int run_uninstall()
{
    auto api = paperbreak::service::windows::make_windows_service_manager_api();
    paperbreak::service::windows::ServiceManager manager{*api};
    auto uninstall_result = manager.uninstall();
    if (!uninstall_result)
    {
        print_error(uninstall_result.error());
        return 1;
    }

    std::cout << (uninstall_result.value() ==
                          paperbreak::service::windows::UninstallOutcome::removed
                      ? "Windows 服务卸载完成。"
                      : "Windows 服务原本不存在。")
              << '\n';
    return 0;
}

int run_service(const Arguments& arguments)
{
    auto run_result = paperbreak::service::windows::run_service_dispatcher(
        [config_path = arguments.config_path] { return create_hosted_service(config_path, true); });
    if (!run_result)
    {
        print_error(run_result.error());
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    // system("chcp 65001");

    QCoreApplication application{argc, argv};
    auto parsed = parse_arguments(argc, argv);
    if (!parsed)
    {
        print_error(parsed.error());
        return 2;
    }

    const Arguments arguments = std::move(parsed).value();
    if (arguments.mode == Mode::version)
    {
        std::cout << paperbreak::format_version_info() << '\n';
        return 0;
    }

    if (arguments.mode == Mode::uninstall)
    {
        return run_uninstall();
    }
    if (arguments.mode == Mode::service)
    {
        return run_service(arguments);
    }

    const auto config_result = paperbreak::config::validate_basic_config(arguments.config_path);
    if (!config_result)
    {
        print_error(config_result.error());
        return 2;
    }
    if (arguments.mode == Mode::validate_config)
    {
        std::cout << "配置完整校验通过，configSchemaVersion="
                  << config_result.value().schema_version
                  << "，configRevision=" << config_result.value().config_revision
                  << "，bytes=" << config_result.value().file_size_bytes << '\n';
        return 0;
    }
    if (arguments.mode == Mode::install)
    {
        return run_install(arguments);
    }
    return run_console(arguments);
}
