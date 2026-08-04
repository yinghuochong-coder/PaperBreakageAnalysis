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
#include "paperbreak/service/event_runtime.hpp"
#include "paperbreak/service/runtime.hpp"
#include "paperbreak/service/system_commands.hpp"
#include "paperbreak/service/windows/console_control.hpp"
#include "paperbreak/service/windows/scm.hpp"
#include "paperbreak/service/windows/scm_host.hpp"
#include "paperbreak/storage/event_inspector.hpp"
#include "paperbreak/storage/metadata_database.hpp"
#include "paperbreak/storage/storage_policy.hpp"

#include <QCoreApplication>

#include <nlohmann/json.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <filesystem>
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
        return runtime_->shutdown();
    }

  private:
    std::shared_ptr<paperbreak::logging::LoggingRuntime> runtime_;
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

class StorageMaintenanceLifecycleComponent final : public paperbreak::service::ILifecycleComponent
{
  public:
    StorageMaintenanceLifecycleComponent(
        std::shared_ptr<paperbreak::storage::StoragePolicyManager> manager,
        std::shared_ptr<paperbreak::monitoring::AlarmRegistry> alarms)
        : manager_(std::move(manager)), alarms_(std::move(alarms))
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

            std::unique_lock lock{mutex_};
            condition_.wait_for(lock, token, std::chrono::seconds{30}, [] { return false; });
        }
    }

    std::shared_ptr<paperbreak::storage::StoragePolicyManager> manager_;
    std::shared_ptr<paperbreak::monitoring::AlarmRegistry> alarms_;
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
        relevant_ = std::ranges::any_of(changed_paths, [](const std::string_view path) {
            return path == "/event" || path.starts_with("/event/");
        });
        if (relevant_)
        {
            previous_ = current;
            candidate_ = candidate;
        }
        return paperbreak::Result<void>::success();
    }
    [[nodiscard]] paperbreak::Result<void> apply_and_readback(
        const paperbreak::config::EdgeConfig&) override
    {
        if (!relevant_)
            return paperbreak::Result<void>::success();
        if (!candidate_)
        {
            return paperbreak::Result<void>::failure(
                paperbreak::make_error("SYS_CONFIG_APPLY_FAILED", paperbreak::Severity::error,
                                       "事件配置没有完成预应用", "event", "event.config.apply"));
        }
        auto runtime = runtime_->reconfigure(*candidate_);
        if (!runtime)
            return runtime;
        return storage_policy_->set_retention_age(
            std::chrono::days{candidate_->event.retention_days});
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
        auto result = relevant_ ? runtime_->reconfigure(rollback_configuration)
                                : paperbreak::Result<void>::success();
        if (relevant_)
        {
            auto retention = storage_policy_->set_retention_age(
                std::chrono::days{rollback_configuration.event.retention_days});
            if (result && !retention)
                result = std::move(retention);
        }
        reset();
        return result;
    }

  private:
    void reset() noexcept
    {
        relevant_ = false;
        previous_.reset();
        candidate_.reset();
    }

    std::shared_ptr<paperbreak::service::EventRuntime> runtime_;
    std::shared_ptr<paperbreak::storage::StoragePolicyManager> storage_policy_;
    bool relevant_{};
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
                      std::weak_ptr<paperbreak::storage::StoragePolicyManager> storage)
        : runtime_(std::move(runtime)), storage_(std::move(storage))
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
        return paperbreak::Result<std::vector<Point>>::success(
            {{.name = "system.nvme_write_bytes_per_second",
              .value = 0.0,
              .unit = "bytes/second",
              .available = false},
             {.name = "uplink.state",
              .value = std::string{"not-initialized"},
              .unit = "state",
              .available = false},
             {.name = "uplink.last_heartbeat_epoch_ms",
              .value = std::uint64_t{0U},
              .unit = "unix_milliseconds",
              .available = false},
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
             {.name = "event.failures_total",
              .value = events.event_failures,
              .unit = "count",
              .available = runtime != nullptr},
             {.name = "storage.watermark",
              .value = std::string{paperbreak::storage::to_string(policy.watermark)},
              .unit = "state",
              .available = storage != nullptr},
             {.name = "uplink.pending_upload_tasks",
              .value = std::uint64_t{0U},
              .unit = "count",
              .available = false}});
    }

  private:
    std::weak_ptr<paperbreak::service::EventRuntime> runtime_;
    std::weak_ptr<paperbreak::storage::StoragePolicyManager> storage_;
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

class AlgorithmPlaceholderMetricSource final : public paperbreak::monitoring::IMetricSource
{
  public:
    [[nodiscard]] std::string_view source_name() const noexcept override
    {
        return "algorithm";
    }

    [[nodiscard]] paperbreak::Result<std::vector<paperbreak::monitoring::MetricPoint>> collect(
        std::stop_token) noexcept override
    {
        using Point = paperbreak::monitoring::MetricPoint;
        return paperbreak::Result<std::vector<Point>>::success(
            {{.name = "algorithm.state", .value = std::string{"not-initialized"}, .unit = "state"},
             {.name = "algorithm.frame_duration.current_ms",
              .value = 0.0,
              .unit = "milliseconds",
              .available = false},
             {.name = "algorithm.frame_duration.average_ms",
              .value = 0.0,
              .unit = "milliseconds",
              .available = false},
             {.name = "algorithm.frame_duration.maximum_ms",
              .value = 0.0,
              .unit = "milliseconds",
              .available = false},
             {.name = "algorithm.queue.depth",
              .value = std::uint64_t{0U},
              .unit = "count",
              .available = false},
             {.name = "algorithm.skipped_frames_total",
              .value = std::uint64_t{0U},
              .unit = "count",
              .available = false},
             {.name = "algorithm.candidates_total",
              .value = std::uint64_t{0U},
              .unit = "count",
              .available = false},
             {.name = "algorithm.confirmed_total",
              .value = std::uint64_t{0U},
              .unit = "count",
              .available = false},
             {.name = "algorithm.false_positives_total",
              .value = std::uint64_t{0U},
              .unit = "count",
              .available = false}});
    }
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
    log_config.directory =
        resolve_config_path(config_path, loaded.value().effective->logging.directory);
    log_config.max_file_size_bytes =
        static_cast<std::size_t>(loaded.value().effective->logging.maximum_file_size_mib) * 1024U *
        1024U;
    log_config.max_files_per_day = loaded.value().effective->logging.maximum_files_per_day;
    log_config.queue_capacity = loaded.value().effective->logging.queue_capacity;
    log_config.minimum_level = logging_level_from_config(loaded.value().effective->logging.level);
    auto logging_result = paperbreak::logging::LoggingRuntime::create(log_config);
    if (!logging_result)
    {
        return paperbreak::Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::
            failure(logging_result.error());
    }

    std::shared_ptr<paperbreak::logging::LoggingRuntime> logging{std::move(logging_result).value()};
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
    auto event_runtime_result = paperbreak::service::EventRuntime::create(
        {.configuration = *loaded.value().effective,
         .event_root = event_root,
         .database = event_database,
         .storage_policy = storage_policy,
         .error_observer = [weak_event_alarms, weak_event_logging](const paperbreak::Error& error) {
             if (auto alarm_registry = weak_event_alarms.lock())
             {
                 static_cast<void>(alarm_registry->raise_alarm({.code = error.business_code,
                                                                .severity = error.severity,
                                                                .source = "event",
                                                                .message = error.message,
                                                                .details = error.details}));
             }
             if (auto log_runtime = weak_event_logging.lock())
             {
                 static_cast<void>(log_runtime->log(paperbreak::logging::Category::storage,
                                                    error.severity == paperbreak::Severity::critical
                                                        ? paperbreak::logging::Level::critical
                                                        : paperbreak::logging::Level::error,
                                                    error.business_code + ": " + error.message));
             }
         }});
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
                             .jpeg_quality = loaded.value().effective->preview.jpeg_quality}};
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
    paperbreak::camera::CameraFrameDeliveryOptions delivery_options{
        .frame_pool_capacity = loaded.value().effective->acquisition.frame_pool_capacity,
        .queue_capacity = loaded.value().effective->acquisition.queue_capacity,
        .receive_timeout =
            std::chrono::milliseconds{loaded.value().effective->acquisition.receive_timeout_ms}};
    cameras = std::make_shared<paperbreak::camera::CameraControlRuntime>(
        std::move(camera_provider),
        [weak_preview, weak_event_runtime](paperbreak::camera::FrameView frame) {
            if (auto runtime = weak_event_runtime.lock())
                static_cast<void>(runtime->submit_frame(frame));
            if (auto runtime = weak_preview.lock())
                runtime->submit(std::move(frame), {.camera_status = "acquiring"});
        },
        delivery_options);
    auto commands = std::make_shared<paperbreak::service::SystemCommandService>(
        configuration->repository, status, metrics, alarms, logging, config_path.parent_path(),
        preview, cameras, event_runtime, event_database, event_inspector);
    auto ipc_server = std::make_shared<paperbreak::ipc::IpcServer>(commands);
    if (preview_publisher)
        preview_publisher->set_server(ipc_server);

    auto monitor = std::make_shared<paperbreak::monitoring::HealthMonitor>(
        metrics, alarms, monitoring_options_from_config(*loaded.value().effective));
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
        std::make_shared<EventMetricSource>(event_runtime, storage_policy),
        std::make_shared<CameraMetricSource>(configuration, cameras),
        std::make_shared<AlgorithmPlaceholderMetricSource>()};
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

    auto monitoring_applier = std::make_shared<MonitoringConfigApplier>(monitor);
    auto registered_applier = configuration->repository.register_applier(*monitoring_applier);
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
    components.push_back(
        std::make_unique<StorageMaintenanceLifecycleComponent>(storage_policy, alarms));
    components.push_back(std::make_unique<IpcLifecycleComponent>(ipc_server));
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
