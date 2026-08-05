#include "main_window.hpp"

#include "paperbreak/common/version.hpp"
#include "paperbreak/uplink/simulator.hpp"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>

#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

namespace
{
using paperbreak::uplink::simulator::MainWindow;
using paperbreak::uplink::simulator::Options;
using paperbreak::uplink::simulator::Runtime;

bool has_argument(const int argc, char** argv, const std::string_view value)
{
    for (int index = 1; index < argc; ++index)
    {
        if (argv[index] == value)
            return true;
    }
    return false;
}

std::filesystem::path default_workspace()
{
    const QString local = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (!local.isEmpty())
        return std::filesystem::path{local.toStdWString()} / "PaperBreak" / "UplinkSimulator";
    return std::filesystem::temp_directory_path() / "PaperBreak" / "UplinkSimulator";
}

int parse_unsigned(const QString& value, const std::uint64_t maximum, std::uint64_t& output)
{
    bool valid = false;
    const qulonglong parsed = value.toULongLong(&valid);
    if (!valid || parsed > maximum)
        return 2;
    output = parsed;
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    const bool headless = has_argument(argc, argv, "--headless");
    std::unique_ptr<QCoreApplication> application;
    if (headless)
        application = std::make_unique<QCoreApplication>(argc, argv);
    else
        application = std::make_unique<QApplication>(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("PaperBreakUplinkSimulator"));
    QCoreApplication::setApplicationVersion(
        QString::fromStdString(std::string{paperbreak::version_info().application_version}));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("PaperBreak Uplink v1 明文、无鉴权参考上位机模拟器"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption headless_option(QStringLiteral("headless"),
                                             QStringLiteral("无界面运行。"));
    const QCommandLineOption listen_option(QStringLiteral("listen"),
                                           QStringLiteral("监听 IPv4 地址，默认 0.0.0.0。"),
                                           QStringLiteral("address"), QStringLiteral("0.0.0.0"));
    const QCommandLineOption port_option(QStringLiteral("port"), QStringLiteral("监听端口。"),
                                         QStringLiteral("port"), QStringLiteral("18080"));
    const QCommandLineOption workspace_option(
        QStringLiteral("workspace"), QStringLiteral("持久化工作区路径。"), QStringLiteral("path"),
        QString::fromStdWString(default_workspace().wstring()));
    const QCommandLineOption devices_option(QStringLiteral("max-devices"),
                                            QStringLiteral("设备上限，范围 1～16。"),
                                            QStringLiteral("count"), QStringLiteral("16"));
    const QCommandLineOption limit_option(QStringLiteral("workspace-limit-gib"),
                                          QStringLiteral("工作区上限 GiB。"), QStringLiteral("gib"),
                                          QStringLiteral("20"));
    const QCommandLineOption scenario_option(QStringLiteral("scenario"),
                                             QStringLiteral("故障场景 schema v1 JSON。"),
                                             QStringLiteral("path"));
    const QCommandLineOption run_for_option(QStringLiteral("run-for-ms"),
                                            QStringLiteral("到期后自动退出，供测试使用。"),
                                            QStringLiteral("milliseconds"));
    const QCommandLineOption smoke_option(QStringLiteral("smoke-test"),
                                          QStringLiteral("启动、构造界面并快速退出。"));
    parser.addOptions({headless_option, listen_option, port_option, workspace_option,
                       devices_option, limit_option, scenario_option, run_for_option,
                       smoke_option});
    parser.process(*application);

    std::uint64_t port = 0U;
    std::uint64_t maximum_devices = 0U;
    std::uint64_t limit_gib = 0U;
    if (parse_unsigned(parser.value(port_option), 65535U, port) != 0 ||
        parse_unsigned(parser.value(devices_option), 16U, maximum_devices) != 0 ||
        maximum_devices == 0U ||
        parse_unsigned(parser.value(limit_option), 1024U, limit_gib) != 0 || limit_gib == 0U)
    {
        std::cerr << "SYS_CONFIG_INVALID: port/max-devices/workspace-limit-gib 参数无效\n";
        return 2;
    }
    Options options{.listen_address = parser.value(listen_option).toStdString(),
                    .port = static_cast<std::uint16_t>(port),
                    .workspace =
                        std::filesystem::path{parser.value(workspace_option).toStdWString()},
                    .maximum_device_count = static_cast<std::size_t>(maximum_devices),
                    .workspace_limit_bytes = limit_gib * 1024ULL * 1024ULL * 1024ULL};
    if (parser.isSet(scenario_option))
        options.scenario_path = std::filesystem::path{parser.value(scenario_option).toStdWString()};

    std::cerr << "WARNING: plaintext unauthenticated Uplink v1 listening on "
              << options.listen_address << ':' << options.port
              << "; isolate this endpoint with VLAN/firewall controls.\n";
    Runtime runtime;
    auto started = runtime.start(options);
    if (!started)
    {
        std::cerr << started.error().business_code << ": " << started.error().message << '\n';
        return 1;
    }

    std::unique_ptr<MainWindow> window;
    if (!headless)
    {
        window = std::make_unique<MainWindow>(runtime, options);
        window->show();
    }
    std::uint64_t run_for_ms = 0U;
    if (parser.isSet(run_for_option) &&
        parse_unsigned(parser.value(run_for_option), 24ULL * 60ULL * 60ULL * 1000ULL, run_for_ms) !=
            0)
    {
        runtime.stop();
        std::cerr << "SYS_CONFIG_INVALID: run-for-ms 参数无效\n";
        return 2;
    }
    if (parser.isSet(smoke_option) && run_for_ms == 0U)
        run_for_ms = 50U;
    if (run_for_ms > 0U)
        QTimer::singleShot(static_cast<int>(run_for_ms), application.get(),
                           &QCoreApplication::quit);
    const int exit_code = application->exec();
    runtime.stop();
    return exit_code;
}
