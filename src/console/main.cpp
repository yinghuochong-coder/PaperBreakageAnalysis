#include "paperbreak/common/version.hpp"
#include "paperbreak/console/client_state_store.hpp"
#include "paperbreak/logging/logging.hpp"
#include "src/main_window.hpp"

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
    QAction status_action(QStringLiteral("后台服务：正在初始化"), &tray_menu);
    status_action.setEnabled(false);
    QAction about_action(QStringLiteral("关于 PaperBreakEdge"), &tray_menu);
    about_action.setEnabled(false);
    QAction quit_action(QStringLiteral("退出界面"), &tray_menu);
    tray_menu.addAction(&status_action);
    tray_menu.addSeparator();
    tray_menu.addAction(&about_action);
    tray_menu.addSeparator();
    tray_menu.addAction(&quit_action);

    QSystemTrayIcon tray;
    tray.setIcon(application.style()->standardIcon(QStyle::SP_ComputerIcon));
    tray.setToolTip(QStringLiteral("PaperBreakEdge Console — M4-01"));
    tray.setContextMenu(&tray_menu);
    QObject::connect(&quit_action, &QAction::triggered, &application, &QApplication::quit);
    tray.show();

    paperbreak::console::MainWindow main_window;
    main_window.show();

    paperbreak::console::ClientStateStore state_store(
        [&status_action, &tray,
         &main_window](const paperbreak::console::ClientStateSnapshot& snapshot) {
            QString text;
            switch (snapshot.connection.state)
            {
            case paperbreak::ipc::ClientConnectionState::stopped:
                text = QStringLiteral("后台服务：客户端已停止");
                break;
            case paperbreak::ipc::ClientConnectionState::connecting:
                text = QStringLiteral("后台服务：连接中");
                break;
            case paperbreak::ipc::ClientConnectionState::retry_wait:
                text = QStringLiteral("后台服务连接中断（状态已过期）");
                break;
            case paperbreak::ipc::ClientConnectionState::connected:
                if (snapshot.service_status_stale || !snapshot.service_status.has_value())
                {
                    text = QStringLiteral("后台服务：已连接，状态同步中");
                }
                else
                {
                    text = QStringLiteral("后台服务状态：%1")
                               .arg(QString::fromStdString(snapshot.service_status->service_state));
                }
                break;
            }
            status_action.setText(text);
            tray.setToolTip(QStringLiteral("PaperBreakEdge Console — %1").arg(text));
            main_window.apply_snapshot(snapshot);
        });
    main_window.apply_snapshot(state_store.snapshot());
    auto client_start = state_store.start();
    if (!client_start)
    {
        static_cast<void>(logging->log(paperbreak::logging::Category::ui,
                                       paperbreak::logging::Level::error,
                                       "PaperBreakEdgeConsole IPC client failed to start"));
        main_window.hide();
        tray.hide();
        static_cast<void>(logging->shutdown());
        return 1;
    }

    if (has_argument(argc, argv, "--smoke-test") &&
        (!tray.isVisible() || !main_window.isVisible() || main_window.page_count() != 12U ||
         main_window.current_page_index() != 0 || !main_window.select_page(11U) ||
         !main_window.select_page(0U)))
    {
        state_store.stop();
        main_window.hide();
        tray.hide();
        static_cast<void>(logging->shutdown());
        return 2;
    }

    QTimer refresh_timer;
    QObject::connect(&refresh_timer, &QTimer::timeout,
                     [&state_store] { state_store.refresh_dynamic(); });
    refresh_timer.start(1000);

    QTimer clock_timer;
    QObject::connect(&clock_timer, &QTimer::timeout, &main_window,
                     [&main_window] { main_window.update_clock(); });
    clock_timer.start(1000);

    static_cast<void>(logging->log(paperbreak::logging::Category::ui,
                                   paperbreak::logging::Level::info,
                                   "PaperBreakEdgeConsole tray started"));

    if (has_argument(argc, argv, "--smoke-test"))
    {
        QTimer::singleShot(100, &application, &QApplication::quit);
    }

    const int result = application.exec();
    refresh_timer.stop();
    clock_timer.stop();
    state_store.stop();
    main_window.hide();
    tray.hide();
    static_cast<void>(logging->log(paperbreak::logging::Category::ui,
                                   paperbreak::logging::Level::info,
                                   "PaperBreakEdgeConsole tray stopping"));
    const auto shutdown_result = logging->shutdown();
    return shutdown_result ? result : 1;
}
