#include "paperbreak/common/version.hpp"
#include "paperbreak/logging/logging.hpp"

#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <thread>

namespace
{

struct Arguments final
{
    bool console{false};
    bool version{false};
    std::chrono::milliseconds run_for{0};
};

paperbreak::Result<Arguments> parse_arguments(const int argc, char* argv[])
{
    Arguments arguments;
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument{argv[index]};
        if (argument == "--console")
        {
            arguments.console = true;
        }
        else if (argument == "--version")
        {
            arguments.version = true;
        }
        else if (argument == "--run-for-ms" && index + 1 < argc)
        {
            ++index;
            std::uint64_t duration = 0;
            const std::string_view text{argv[index]};
            const auto parse_result =
                std::from_chars(text.data(), text.data() + text.size(), duration);
            if (parse_result.ec != std::errc{} || parse_result.ptr != text.data() + text.size() ||
                duration > 60'000U)
            {
                return paperbreak::Result<Arguments>::failure(paperbreak::make_error(
                    "SYS_CONFIG_INVALID", paperbreak::Severity::error,
                    "--run-for-ms 必须是 0 到 60000 的整数", "service", "service.parseArguments"));
            }
            arguments.run_for = std::chrono::milliseconds{duration};
        }
        else
        {
            return paperbreak::Result<Arguments>::failure(
                paperbreak::make_error("SYS_CONFIG_INVALID", paperbreak::Severity::error,
                                       "未知命令行参数", "service", "service.parseArguments"));
        }
    }
    return paperbreak::Result<Arguments>::success(arguments);
}

} // namespace

int main(const int argc, char* argv[])
{
    auto parsed = parse_arguments(argc, argv);
    if (!parsed)
    {
        std::cerr << parsed.error().business_code << ": " << parsed.error().message << '\n';
        return 2;
    }

    const Arguments arguments = parsed.value();
    if (arguments.version)
    {
        std::cout << paperbreak::format_version_info() << '\n';
        return 0;
    }
    if (!arguments.console)
    {
        std::cerr << "M0 仅支持 --console；Windows SCM 宿主将在 M1 实现。\n";
        return 2;
    }

    paperbreak::logging::LoggingConfig log_config;
    log_config.directory = std::filesystem::temp_directory_path() / "PaperBreakEdge" / "logs";
    auto logging_result = paperbreak::logging::LoggingRuntime::create(log_config);
    if (!logging_result)
    {
        std::cerr << logging_result.error().business_code << ": " << logging_result.error().message
                  << '\n';
        return 1;
    }
    auto logging = std::move(logging_result).value();
    static_cast<void>(logging->log(paperbreak::logging::Category::service,
                                   paperbreak::logging::Level::info,
                                   "PaperBreakEdgeService console host started"));

    std::cout << paperbreak::format_version_info() << '\n';
    std::cout << "PaperBreakEdgeService 正在 console 模式运行。\n";
    if (arguments.run_for.count() > 0)
    {
        std::this_thread::sleep_for(arguments.run_for);
    }
    else
    {
        std::cout << "按 Enter 请求受控退出。\n";
        std::string line;
        std::getline(std::cin, line);
    }

    static_cast<void>(logging->log(paperbreak::logging::Category::service,
                                   paperbreak::logging::Level::info,
                                   "PaperBreakEdgeService console host stopping"));
    const auto shutdown_result = logging->shutdown();
    if (!shutdown_result)
    {
        std::cerr << shutdown_result.error().business_code << ": "
                  << shutdown_result.error().message << '\n';
        return 1;
    }
    return 0;
}
