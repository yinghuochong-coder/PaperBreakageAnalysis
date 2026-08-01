#include "paperbreak/common/version.hpp"
#include "paperbreak/config/basic_config.hpp"
#include "paperbreak/logging/logging.hpp"
#include "paperbreak/service/runtime.hpp"
#include "paperbreak/service/windows/console_control.hpp"
#include "paperbreak/service/windows/scm.hpp"
#include "paperbreak/service/windows/scm_host.hpp"

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
    explicit LoggingLifecycleComponent(std::unique_ptr<paperbreak::logging::LoggingRuntime> runtime)
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
    std::unique_ptr<paperbreak::logging::LoggingRuntime> runtime_;
};

class HostedRuntime final : public paperbreak::service::windows::IHostedService
{
  public:
    explicit HostedRuntime(
        std::vector<std::unique_ptr<paperbreak::service::ILifecycleComponent>> components)
        : runtime_(std::move(components))
    {
    }

    [[nodiscard]] paperbreak::Result<paperbreak::service::StartOutcome> start() override
    {
        return runtime_.start();
    }

    void request_stop(const paperbreak::service::StopReason reason) noexcept override
    {
        runtime_.request_stop(reason);
    }

    [[nodiscard]] paperbreak::Result<void> shutdown() override
    {
        return runtime_.shutdown();
    }

  private:
    paperbreak::service::ServiceRuntime runtime_;
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
    paperbreak::logging::LoggingConfig log_config;
    log_config.directory = std::filesystem::temp_directory_path() / "PaperBreakEdge" / "logs";
    auto logging_result = paperbreak::logging::LoggingRuntime::create(log_config);
    if (!logging_result)
    {
        return paperbreak::Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::
            failure(logging_result.error());
    }

    auto logging = std::move(logging_result).value();
    if (validate_config)
    {
        auto config_result = paperbreak::config::validate_basic_config(config_path);
        if (!config_result)
        {
            static_cast<void>(logging->log(
                paperbreak::logging::Category::service, paperbreak::logging::Level::critical,
                config_result.error().business_code + ": " + config_result.error().message));
            static_cast<void>(logging->shutdown());
            return paperbreak::
                Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::failure(
                    config_result.error());
        }
    }

    std::vector<std::unique_ptr<paperbreak::service::ILifecycleComponent>> components;
    components.push_back(std::make_unique<LoggingLifecycleComponent>(std::move(logging)));
    return paperbreak::Result<std::unique_ptr<paperbreak::service::windows::IHostedService>>::
        success(std::make_unique<HostedRuntime>(std::move(components)));
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

int main(const int argc, char* argv[])
{
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
        std::cout << "配置基础校验通过，schemaVersion=" << config_result.value().schema_version
                  << "，bytes=" << config_result.value().file_size_bytes << '\n';
        return 0;
    }
    if (arguments.mode == Mode::install)
    {
        return run_install(arguments);
    }
    return run_console(arguments);
}
