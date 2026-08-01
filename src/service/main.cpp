#include "paperbreak/common/version.hpp"
#include "paperbreak/config/basic_config.hpp"
#include "paperbreak/logging/logging.hpp"
#include "paperbreak/service/runtime.hpp"
#include "paperbreak/service/windows/console_control.hpp"

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
        if (argument == "--console" || argument == "--validate-config" || argument == "--version")
        {
            const Mode requested =
                argument == "--console"
                    ? Mode::console
                    : (argument == "--validate-config" ? Mode::validate_config : Mode::version);
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
        return paperbreak::Result<Arguments>::failure(
            argument_error("必须指定 --console、--validate-config 或 --version"));
    }
    if (arguments.mode == Mode::version)
    {
        if (!arguments.config_path.empty() || arguments.run_for_present)
        {
            return paperbreak::Result<Arguments>::failure(
                argument_error("--version 不能与配置或运行时限参数组合"));
        }
        return paperbreak::Result<Arguments>::success(std::move(arguments));
    }
    if (arguments.config_path.empty())
    {
        return paperbreak::Result<Arguments>::failure(
            argument_error("--console 和 --validate-config 必须提供 --config <path>"));
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

void print_error(const paperbreak::Error& error)
{
    std::cerr << error.business_code << ": " << error.message;
    for (const auto& detail : error.details)
    {
        std::cerr << " [" << detail.key << '=' << detail.value << ']';
    }
    std::cerr << '\n';
}

int run_console(const Arguments& arguments)
{
    paperbreak::logging::LoggingConfig log_config;
    log_config.directory = std::filesystem::temp_directory_path() / "PaperBreakEdge" / "logs";
    auto logging_result = paperbreak::logging::LoggingRuntime::create(log_config);
    if (!logging_result)
    {
        print_error(logging_result.error());
        return 1;
    }

    std::vector<std::unique_ptr<paperbreak::service::ILifecycleComponent>> components;
    components.push_back(
        std::make_unique<LoggingLifecycleComponent>(std::move(logging_result).value()));
    paperbreak::service::ServiceRuntime runtime{std::move(components)};
    StopRequestChannel stop_channel;

    auto registration_result = paperbreak::service::windows::ConsoleControlRegistration::create(
        [&runtime, &stop_channel](const paperbreak::service::StopReason reason) {
            runtime.request_stop(reason);
            stop_channel.request(reason);
        });
    if (!registration_result)
    {
        print_error(registration_result.error());
        return 1;
    }
    [[maybe_unused]] auto registration = std::move(registration_result).value();

    auto start_result = runtime.start();
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
            runtime.request_stop(paperbreak::service::StopReason::test_deadline);
        }
    }
    else
    {
        std::cout << "按 Ctrl+C 请求受控退出。\n";
        static_cast<void>(stop_channel.wait());
    }

    const auto shutdown_result = runtime.shutdown();
    if (!shutdown_result)
    {
        print_error(shutdown_result.error());
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
    return run_console(arguments);
}
