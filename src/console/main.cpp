#include "paperbreak/common/version.hpp"
#include "paperbreak/console/camera_client.hpp"
#include "paperbreak/console/client_state_store.hpp"
#include "paperbreak/console/navigation_model.hpp"
#include "paperbreak/console/preview_client.hpp"
#include "paperbreak/logging/logging.hpp"
#include "paperbreak/service/windows/scm.hpp"
#include "src/main_window.hpp"
#include "src/system_tray_controller.hpp"

#include <QApplication>
#include <QDesktopServices>
#include <QMessageBox>
#include <QMetaObject>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>

namespace
{

bool has_argument(const int argc, char* argv[], const std::string_view expected)
{
    for (int index = 1; index < argc; ++index)
    {
        if (std::string_view{argv[index]} == expected)
            return true;
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
    const bool smoke_test = has_argument(argc, argv, "--smoke-test");

    paperbreak::logging::LoggingConfig log_config;
    log_config.directory = std::filesystem::temp_directory_path() / "PaperBreakEdge" / "logs";
    auto logging_result = paperbreak::logging::LoggingRuntime::create(log_config);
    if (!logging_result)
        return 1;
    auto logging = std::move(logging_result).value();
    auto* const logging_runtime = logging.get();

    std::unique_ptr<paperbreak::console::PreviewClient> preview_client;
    std::unique_ptr<paperbreak::console::CameraClient> camera_client;
    paperbreak::console::MainWindow main_window(
        [&preview_client](const bool paused) {
            if (preview_client)
                preview_client->set_paused(paused);
        },
        {.discover =
             [&camera_client] {
                 if (camera_client)
                     return camera_client->discover();
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "相机客户端尚未初始化",
                     "console", "console.camera.discover", true));
             },
         .control =
             [&camera_client](std::string command, std::string camera_id) {
                 if (camera_client)
                     return camera_client->control(std::move(command), std::move(camera_id));
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "相机客户端尚未初始化",
                     "console", "console.camera.control", true));
             },
         .update_config =
             [&camera_client](std::string camera_id, const std::uint64_t revision,
                              paperbreak::console::CameraParameterValue parameters) {
                 if (camera_client)
                     return camera_client->update_config(std::move(camera_id), revision,
                                                         parameters);
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "相机客户端尚未初始化",
                     "console", "console.camera.updateConfig", true));
             }});
    preview_client = std::make_unique<paperbreak::console::PreviewClient>(
        [&main_window](const paperbreak::console::PreviewSnapshot& snapshot) {
            main_window.apply_preview_snapshot(snapshot);
        });
    camera_client = std::make_unique<paperbreak::console::CameraClient>(
        [&main_window](const paperbreak::console::CameraClientSnapshot& snapshot) {
            main_window.apply_camera_snapshot(snapshot);
        });
    paperbreak::console::ClientStateSnapshot latest_snapshot;
    std::atomic_bool restart_running{};
    std::jthread restart_task;

    const auto open_console = [&main_window] {
        main_window.showNormal();
        main_window.raise();
        main_window.activateWindow();
    };
    const auto show_status = [&main_window, &open_console] {
        if (const auto index = paperbreak::console::console_page_index(
                paperbreak::console::ConsolePageId::device_status))
            static_cast<void>(main_window.select_page(index.value()));
        open_console();
    };

    paperbreak::console::SystemTrayController tray({
        .open_console = open_console,
        .show_status = show_status,
        .restart_service =
            [&] {
                if (restart_running.exchange(true))
                {
                    QMessageBox::information(&main_window, QStringLiteral("重启后台服务"),
                                             QStringLiteral("后台服务重启正在进行中。"));
                    return;
                }
                if (restart_task.joinable())
                    restart_task.join();
                restart_task = std::jthread([&application, &main_window, &restart_running,
                                             logging_runtime](const std::stop_token stop_token) {
                    auto api = paperbreak::service::windows::make_windows_service_manager_api();
                    paperbreak::service::windows::ServiceManager manager{*api};
                    auto result = std::make_shared<paperbreak::Result<void>>(
                        manager.restart(paperbreak::service::windows::service_name,
                                        std::chrono::seconds{30}, stop_token));
                    QMetaObject::invokeMethod(
                        &application,
                        [&main_window, &restart_running, logging_runtime, result] {
                            restart_running.store(false);
                            if (*result)
                            {
                                static_cast<void>(logging_runtime->log(
                                    paperbreak::logging::Category::ui,
                                    paperbreak::logging::Level::info,
                                    "Windows background service restart completed"));
                                QMessageBox::information(&main_window,
                                                         QStringLiteral("重启后台服务"),
                                                         QStringLiteral("后台服务已恢复运行。"));
                            }
                            else
                            {
                                static_cast<void>(logging_runtime->log(
                                    paperbreak::logging::Category::ui,
                                    paperbreak::logging::Level::error,
                                    "Windows background service restart failed: " +
                                        result->error().business_code));
                                QMessageBox::warning(
                                    &main_window, QStringLiteral("重启后台服务失败"),
                                    QString::fromStdString(result->error().message));
                            }
                        },
                        Qt::QueuedConnection);
                });
            },
        .open_event_directory =
            [&] {
                if (latest_snapshot.locations_stale || !latest_snapshot.locations.has_value())
                {
                    QMessageBox::warning(&main_window, QStringLiteral("打开事件目录"),
                                         QStringLiteral("事件目录尚未从后台服务同步。"));
                    return;
                }
                const QString path = QString::fromUtf8(latest_snapshot.locations->event_root);
                if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
                    QMessageBox::warning(&main_window, QStringLiteral("打开事件目录"),
                                         QStringLiteral("Windows 无法打开事件目录：%1").arg(path));
            },
        .show_about =
            [&main_window] {
                QMessageBox::about(
                    &main_window, QStringLiteral("关于 PaperBreakEdge"),
                    QStringLiteral("PaperBreakEdge Console\n版本 %1\n纸机断纸分析边缘运维客户端")
                        .arg(QApplication::applicationVersion()));
            },
        .quit_interface = [&application] { application.quit(); },
    });
    tray.show();
    main_window.show();

    paperbreak::console::ClientStateStore state_store(
        [&](const paperbreak::console::ClientStateSnapshot& snapshot) {
            latest_snapshot = snapshot;
            tray.apply_snapshot(snapshot);
            main_window.apply_snapshot(snapshot);
        });
    tray.apply_snapshot(state_store.snapshot());
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
    if (!smoke_test)
    {
        const auto preview_start = preview_client->start();
        if (!preview_start)
            main_window.apply_preview_snapshot(preview_client->snapshot());
    }
    if (!smoke_test)
        static_cast<void>(camera_client->start());

    bool smoke_ok = true;
    if (smoke_test)
    {
        smoke_ok = tray.is_visible() && main_window.isVisible() && tray.action_count() == 8U &&
                   !tray.preview_action_enabled() && !tray.diagnostics_action_enabled() &&
                   main_window.page_count() == 12U && main_window.current_page_index() == 0 &&
                   main_window.camera_configuration_ready() && main_window.select_page(11U) &&
                   main_window.select_page(0U);
        for (int iteration = 0; iteration < 20 && smoke_ok; ++iteration)
        {
            main_window.close();
            smoke_ok = !main_window.isVisible();
            open_console();
            smoke_ok = smoke_ok && main_window.isVisible();
        }
    }
    if (!smoke_ok)
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
    QObject::connect(&refresh_timer, &QTimer::timeout,
                     [&camera_client] { camera_client->refresh(); });
    QTimer clock_timer;
    QObject::connect(&clock_timer, &QTimer::timeout, &main_window,
                     [&main_window] { main_window.update_clock(); });
    clock_timer.start(1000);

    static_cast<void>(logging->log(paperbreak::logging::Category::ui,
                                   paperbreak::logging::Level::info,
                                   "PaperBreakEdgeConsole tray started"));
    if (smoke_test)
        QTimer::singleShot(100, &application, &QApplication::quit);

    const int result = application.exec();
    refresh_timer.stop();
    clock_timer.stop();
    state_store.stop();
    preview_client->stop();
    preview_client.reset();
    camera_client->stop();
    camera_client.reset();
    if (restart_task.joinable())
    {
        restart_task.request_stop();
        restart_task.join();
    }
    main_window.hide();
    tray.hide();
    static_cast<void>(logging->log(paperbreak::logging::Category::ui,
                                   paperbreak::logging::Level::info,
                                   "PaperBreakEdgeConsole tray stopping"));
    const auto shutdown_result = logging->shutdown();
    return shutdown_result ? result : 1;
}
