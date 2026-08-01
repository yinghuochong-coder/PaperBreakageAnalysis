#include "paperbreak/common/version.hpp"
#include "paperbreak/logging/logging.hpp"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMenu>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>

#include <filesystem>
#include <iostream>
#include <string_view>

namespace
{

bool has_argument(const int argc, char* argv[], const std::string_view expected)
{
    for (int index = 1; index < argc; ++index)
    {
        if (std::string_view{argv[index]} == expected)
        {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char* argv[])
{
    if (has_argument(argc, argv, "--version"))
    {
        std::cout << paperbreak::format_version_info() << '\n';
        return 0;
    }

    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("PaperBreakEdgeConsole"));
    QApplication::setApplicationVersion(QString::fromUtf8(
        paperbreak::version_info().application_version.data(),
        static_cast<qsizetype>(paperbreak::version_info().application_version.size())));
    QApplication::setQuitOnLastWindowClosed(false);

    paperbreak::logging::LoggingConfig log_config;
    log_config.directory = std::filesystem::temp_directory_path() / "PaperBreakEdge" / "logs";
    auto logging_result = paperbreak::logging::LoggingRuntime::create(log_config);
    if (!logging_result)
    {
        return 1;
    }
    auto logging = std::move(logging_result).value();

    QMenu tray_menu;
    QAction about_action(QStringLiteral("关于 PaperBreakEdge"), &tray_menu);
    about_action.setEnabled(false);
    QAction quit_action(QStringLiteral("退出界面"), &tray_menu);
    tray_menu.addAction(&about_action);
    tray_menu.addSeparator();
    tray_menu.addAction(&quit_action);

    QSystemTrayIcon tray;
    tray.setIcon(application.style()->standardIcon(QStyle::SP_ComputerIcon));
    tray.setToolTip(QStringLiteral("PaperBreakEdge Console — M0"));
    tray.setContextMenu(&tray_menu);
    QObject::connect(&quit_action, &QAction::triggered, &application, &QApplication::quit);
    tray.show();

    if (has_argument(argc, argv, "--smoke-test") && !tray.isVisible())
    {
        static_cast<void>(logging->shutdown());
        return 2;
    }

    static_cast<void>(logging->log(paperbreak::logging::Category::ui,
                                   paperbreak::logging::Level::info,
                                   "PaperBreakEdgeConsole tray started"));

    if (has_argument(argc, argv, "--smoke-test"))
    {
        QTimer::singleShot(100, &application, &QApplication::quit);
    }

    const int result = application.exec();
    tray.hide();
    static_cast<void>(logging->log(paperbreak::logging::Category::ui,
                                   paperbreak::logging::Level::info,
                                   "PaperBreakEdgeConsole tray stopping"));
    const auto shutdown_result = logging->shutdown();
    return shutdown_result ? result : 1;
}
