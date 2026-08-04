#include "paperbreak/common/version.hpp"
#include "paperbreak/console/camera_client.hpp"
#include "paperbreak/console/client_state_store.hpp"
#include "paperbreak/console/navigation_model.hpp"
#include "paperbreak/console/operations_client.hpp"
#include "paperbreak/console/preview_client.hpp"
#include "paperbreak/logging/logging.hpp"
#include "paperbreak/service/windows/scm.hpp"
#include "src/main_window.hpp"
#include "src/system_tray_controller.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QSettings>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

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
    QApplication::setOrganizationName(QStringLiteral("PaperBreak"));
    QApplication::setOrganizationDomain(QStringLiteral("paperbreak.local"));
    QApplication::setApplicationVersion(QString::fromUtf8(
        paperbreak::version_info().application_version.data(),
        static_cast<qsizetype>(paperbreak::version_info().application_version.size())));
    QApplication::setQuitOnLastWindowClosed(false);
    const bool smoke_test = has_argument(argc, argv, "--smoke-test");
    const QString smoke_theme_settings =
        smoke_test ? QDir::current().filePath(QStringLiteral("paperbreak-theme-smoke.ini"))
                   : QString{};
    if (smoke_test)
    {
        static_cast<void>(QFile::remove(smoke_theme_settings));
        QSettings invalid_settings{smoke_theme_settings, QSettings::IniFormat};
        invalid_settings.setValue(QStringLiteral("ui/theme"), QStringLiteral("invalid"));
        invalid_settings.sync();
    }
    paperbreak::console::ThemeController theme_controller(application, true, smoke_theme_settings);

    paperbreak::logging::LoggingConfig log_config;
    log_config.directory = std::filesystem::temp_directory_path() / "PaperBreakEdge" / "logs";
    auto logging_result = paperbreak::logging::LoggingRuntime::create(log_config);
    if (!logging_result)
        return 1;
    auto logging = std::move(logging_result).value();
    auto* const logging_runtime = logging.get();

    std::unique_ptr<paperbreak::console::PreviewClient> preview_client;
    std::unique_ptr<paperbreak::console::CameraClient> camera_client;
    std::unique_ptr<paperbreak::console::OperationsClient> operations_client;
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
         .bind =
             [&camera_client](std::string camera_id, std::string serial_number,
                              std::string location, const std::uint64_t expected_revision) {
                 if (camera_client)
                     return camera_client->bind(std::move(camera_id), std::move(serial_number),
                                                std::move(location), expected_revision);
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "相机客户端尚未初始化",
                     "console", "console.camera.bind", true));
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
             }},
        {.initial_mode = theme_controller.mode(),
         .set_mode =
             [&theme_controller](const paperbreak::console::ThemeMode mode) {
                 theme_controller.set_mode(mode);
             }},
        {.refresh =
             [&operations_client] {
                 if (operations_client)
                     operations_client->refresh();
             },
         .query_alarms =
             [&operations_client](paperbreak::console::AlarmFilter filter) {
                 if (operations_client)
                     return operations_client->query_alarms(std::move(filter));
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "运维客户端尚未初始化",
                     "console", "console.operations.alarms", true));
             },
         .query_logs =
             [&operations_client](paperbreak::console::LogFilter filter) {
                 if (operations_client)
                     return operations_client->query_logs(std::move(filter));
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "运维客户端尚未初始化",
                     "console", "console.operations.logs", true));
             },
         .acknowledge =
             [&operations_client](const std::uint64_t alarm_id) {
                 if (operations_client)
                     return operations_client->acknowledge(alarm_id);
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "运维客户端尚未初始化",
                     "console", "console.operations.acknowledge", true));
             },
         .export_diagnostics =
             [&operations_client](std::filesystem::path destination) {
                 if (operations_client)
                     return operations_client->export_diagnostics(std::move(destination));
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "运维客户端尚未初始化",
                     "console", "console.operations.export", true));
             },
         .export_alarm_csv =
             [&operations_client](std::filesystem::path destination) {
                 if (operations_client)
                     return operations_client->export_alarm_csv(std::move(destination));
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "运维客户端尚未初始化",
                     "console", "console.operations.alarmExport", true));
             }});
    preview_client = std::make_unique<paperbreak::console::PreviewClient>(
        [&main_window](const paperbreak::console::PreviewSnapshot& snapshot) {
            main_window.apply_preview_snapshot(snapshot);
        });
    camera_client = std::make_unique<paperbreak::console::CameraClient>(
        [&main_window, &preview_client](const paperbreak::console::CameraClientSnapshot& snapshot) {
            main_window.apply_camera_snapshot(snapshot);
            std::vector<std::string> camera_ids;
            camera_ids.reserve(snapshot.cameras.size());
            for (const auto& camera : snapshot.cameras)
                camera_ids.push_back(camera.id);
            if (preview_client && !camera_ids.empty())
                preview_client->set_camera_ids(std::move(camera_ids));
        });
    if (!smoke_test)
        operations_client = std::make_unique<paperbreak::console::OperationsClient>(
            [&main_window](const paperbreak::console::OperationsSnapshot& snapshot) {
                main_window.apply_operations_snapshot(snapshot);
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
        .export_diagnostics = [&main_window] { main_window.request_diagnostics_export(); },
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
    if (!smoke_test)
        static_cast<void>(operations_client->start());

    bool smoke_ok = true;
    if (smoke_test)
    {
        paperbreak::console::CameraClientSnapshot camera_smoke;
        camera_smoke.stale = false;
        camera_smoke.stored_config_revision = 1U;
        camera_smoke.discovered_devices.push_back({.model = "MV-CS020-60GM",
                                                   .serial = "SMOKE-01",
                                                   .ip = "192.0.2.10",
                                                   .network_interface = "192.0.2.1",
                                                   .exclusive_access_available = false});
        main_window.apply_camera_snapshot(camera_smoke);
        const bool empty_configuration_kept_discovery =
            main_window.discovered_camera_count() == 1U &&
            main_window.camera_device_controls_disabled();
        camera_smoke.cameras.push_back({.id = "CAM01", .state = "disconnected"});
        camera_smoke.discovered_devices.front().exclusive_access_available = true;
        camera_smoke.topology_restart_required = true;
        main_window.apply_camera_snapshot(camera_smoke);
        const bool restart_state_disabled_controls = main_window.camera_device_controls_disabled();
        const bool invalid_theme_fell_back =
            theme_controller.mode() == paperbreak::console::ThemeMode::system;
        const bool selected_light =
            main_window.select_theme_mode(paperbreak::console::ThemeMode::light);
        const bool selected_dark =
            main_window.select_theme_mode(paperbreak::console::ThemeMode::dark);
        QSettings persisted_settings{smoke_theme_settings, QSettings::IniFormat};
        persisted_settings.sync();
        const bool dark_theme_persisted =
            persisted_settings.value(QStringLiteral("ui/theme")).toString() ==
            QStringLiteral("dark");
        const bool selected_system =
            main_window.select_theme_mode(paperbreak::console::ThemeMode::system);
        constexpr auto sample_first_time = "2026-08-04T00:00:01.000Z";
        constexpr auto sample_last_time = "2026-08-04T00:00:02.000Z";
        const QDateTime sample_last_date_time =
            QDateTime::fromString(QString::fromLatin1(sample_last_time), Qt::ISODateWithMs);
        const QString expected_first_local =
            QDateTime::fromString(QString::fromLatin1(sample_first_time), Qt::ISODateWithMs)
                .toLocalTime()
                .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz ttt"));
        const QString expected_last_local = sample_last_date_time.toLocalTime().toString(
            QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz ttt"));
        paperbreak::console::OperationsSnapshot operations_smoke;
        operations_smoke.metrics.push_back(
            {.name = "camera.CAM01.last_frame_epoch_ms",
             .value = std::to_string(sample_last_date_time.toMSecsSinceEpoch()),
             .unit = "unix_milliseconds",
             .available = true});
        operations_smoke.alarms.push_back({.alarm_id = 1U,
                                           .code = "CAMERA_OFFLINE",
                                           .severity = "Warning",
                                           .source = "CAM01",
                                           .first_occurred_at = sample_first_time,
                                           .last_occurred_at = sample_last_time,
                                           .active = true,
                                           .occurrence_count = 1U,
                                           .message = "camera offline"});
        operations_smoke.logs.push_back({.sequence = 1U,
                                         .timestamp = sample_last_time,
                                         .thread_id = 1U,
                                         .category = "camera",
                                         .level = "warning",
                                         .message = "camera timeout"});
        main_window.apply_operations_snapshot(operations_smoke);
        auto* const metrics_table =
            main_window.findChild<QTableWidget*>(QStringLiteral("operations-metrics"));
        auto* const alarm_table =
            main_window.findChild<QTableWidget*>(QStringLiteral("operations-alarms"));
        auto* const log_table =
            main_window.findChild<QTableWidget*>(QStringLiteral("operations-logs"));
        if (alarm_table)
            alarm_table->selectRow(0);
        auto* const alarm_details = main_window.findChild<QLabel*>(QStringLiteral("alarm-details"));
        auto* const clock = main_window.findChild<QLabel*>(QStringLiteral("current-local-time"));
        paperbreak::console::ClientStateSnapshot local_time_snapshot;
        local_time_snapshot.alarms = paperbreak::console::AlarmOverviewSummary{};
        local_time_snapshot.alarms->recent.push_back({.alarm_id = 1U,
                                                      .severity = "Warning",
                                                      .source = "CAM01",
                                                      .last_occurred_at = sample_last_time,
                                                      .message = "camera offline"});
        local_time_snapshot.alarms_stale = false;
        main_window.apply_snapshot(local_time_snapshot);
        auto* const recent_alarms = main_window.findChild<QLabel*>(QStringLiteral("recent-alarms"));
        const bool local_time_displayed =
            metrics_table && metrics_table->item(0, 1) && metrics_table->item(0, 2) &&
            metrics_table->item(0, 1)->text() == expected_last_local &&
            metrics_table->item(0, 2)->text() == QStringLiteral("本地时间") && alarm_table &&
            alarm_table->item(0, 1) && alarm_table->item(0, 1)->text() == expected_last_local &&
            log_table && log_table->item(0, 1) &&
            log_table->item(0, 1)->text() == expected_last_local && alarm_details &&
            alarm_details->text().contains(expected_first_local) &&
            alarm_details->text().contains(expected_last_local) && recent_alarms &&
            recent_alarms->text().contains(expected_last_local) && clock &&
            clock->text().endsWith(QDateTime::currentDateTime().toString(QStringLiteral("ttt")));
        paperbreak::console::ClientStateSnapshot connected_tray_smoke;
        connected_tray_smoke.connection.state = paperbreak::ipc::ClientConnectionState::connected;
        tray.apply_snapshot(connected_tray_smoke);
        const bool diagnostic_enabled_when_connected = tray.diagnostics_action_enabled();
        paperbreak::console::ClientStateSnapshot disconnected_tray_smoke;
        disconnected_tray_smoke.connection.state = paperbreak::ipc::ClientConnectionState::stopped;
        tray.apply_snapshot(disconnected_tray_smoke);
        const bool diagnostic_disabled_when_disconnected = !tray.diagnostics_action_enabled();
        smoke_ok = tray.is_visible() && main_window.isVisible() && tray.action_count() == 8U &&
                   !tray.preview_action_enabled() && !tray.diagnostics_action_enabled() &&
                   main_window.page_count() == 12U && main_window.current_page_index() == 0 &&
                   main_window.camera_configuration_ready() && empty_configuration_kept_discovery &&
                   restart_state_disabled_controls && main_window.operations_pages_ready() &&
                   local_time_displayed && diagnostic_enabled_when_connected &&
                   diagnostic_disabled_when_disconnected &&
                   theme_controller.contrast_requirements_met() && invalid_theme_fell_back &&
                   selected_light && selected_dark && dark_theme_persisted && selected_system &&
                   main_window.select_page(11U) && main_window.select_page(0U);
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
        if (smoke_test)
            static_cast<void>(QFile::remove(smoke_theme_settings));
        state_store.stop();
        main_window.hide();
        tray.hide();
        static_cast<void>(logging->shutdown());
        return 2;
    }
    if (smoke_test)
        static_cast<void>(QFile::remove(smoke_theme_settings));

    QTimer refresh_timer;
    QObject::connect(&refresh_timer, &QTimer::timeout,
                     [&state_store] { state_store.refresh_dynamic(); });
    refresh_timer.start(1000);
    QObject::connect(&refresh_timer, &QTimer::timeout,
                     [&camera_client] { camera_client->refresh(); });
    QObject::connect(&refresh_timer, &QTimer::timeout, [&operations_client] {
        if (operations_client)
            operations_client->refresh();
    });
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
    if (operations_client)
    {
        operations_client->stop();
        operations_client.reset();
    }
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
