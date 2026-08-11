#include "paperbreak/common/version.hpp"
#include "paperbreak/console/algorithm_client.hpp"
#include "paperbreak/console/camera_client.hpp"
#include "paperbreak/console/client_state_store.hpp"
#include "paperbreak/console/event_client.hpp"
#include "paperbreak/console/navigation_model.hpp"
#include "paperbreak/console/operations_client.hpp"
#include "paperbreak/console/preview_client.hpp"
#include "paperbreak/console/uplink_client.hpp"
#include "paperbreak/logging/logging.hpp"
#include "paperbreak/service/windows/scm.hpp"
#include "src/main_window.hpp"
#include "src/system_tray_controller.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLayout>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <array>
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

void send_left_double_click(QWidget* widget)
{
    const QPointF center{widget->rect().center()};
    const QPointF global{widget->mapToGlobal(widget->rect().center())};
    QMouseEvent event{QEvent::MouseButtonDblClick,
                      center,
                      center,
                      global,
                      Qt::LeftButton,
                      Qt::LeftButton,
                      Qt::NoModifier};
    static_cast<void>(QApplication::sendEvent(widget, &event));
}

bool preview_pane_smoke(paperbreak::console::MainWindow& main_window, QApplication& application)
{
    if (!main_window.select_page(1U))
        return false;
    application.processEvents();
    std::array<QWidget*, 4U> tiles{};
    std::array<QSize, 4U> initial_sizes{};
    for (std::size_t index = 0; index < tiles.size(); ++index)
    {
        tiles[index] = main_window.findChild<QWidget*>(
            QStringLiteral("preview-tile-%1").arg(static_cast<int>(index + 1U)));
        if (!tiles[index])
            return false;
        initial_sizes[index] = tiles[index]->size();
    }
    auto* const first_image = main_window.findChild<QLabel*>(QStringLiteral("preview-image-1"));
    auto* const grid = main_window.findChild<QWidget*>(QStringLiteral("preview-grid"));
    if (!first_image || !grid)
        return false;

    paperbreak::console::PreviewSnapshot snapshot;
    snapshot.connection.state = paperbreak::ipc::ClientConnectionState::connected;
    snapshot.subscribed = true;
    paperbreak::console::PreviewImage frame;
    frame.camera_id = "CAM01";
    frame.frame_number = 1U;
    frame.image = QImage{320, 180, QImage::Format_RGB32};
    frame.image.fill(Qt::red);
    snapshot.images[0] = frame;
    main_window.apply_preview_snapshot(snapshot);
    application.processEvents();
    const bool small_frame_kept_sizes = std::ranges::equal(
        tiles, initial_sizes, {}, [](const QWidget* tile) { return tile->size(); },
        [](const QSize& size) { return size; });

    frame.frame_number = 2U;
    frame.image = QImage{1600, 1200, QImage::Format_RGB32};
    frame.image.fill(Qt::blue);
    snapshot.images[0] = frame;
    main_window.apply_preview_snapshot(snapshot);
    application.processEvents();
    const bool large_frame_kept_sizes = std::ranges::equal(
        tiles, initial_sizes, {}, [](const QWidget* tile) { return tile->size(); },
        [](const QSize& size) { return size; });

    send_left_double_click(tiles[0]);
    application.processEvents();
    const bool focused =
        tiles[0]->isVisible() && !tiles[0]->isWindow() && first_image->hasScaledContents() &&
        first_image->size() == tiles[0]->size() && tiles[0]->geometry() == grid->rect() &&
        std::ranges::none_of(tiles.begin() + 1, tiles.end(),
                             [](const QWidget* tile) { return tile->isVisible(); });
    send_left_double_click(tiles[0]);
    application.processEvents();
    const bool full_screen = tiles[0]->isWindow() && tiles[0]->isFullScreen() &&
                             first_image->hasScaledContents() &&
                             first_image->size() == tiles[0]->size();
    send_left_double_click(tiles[0]);
    application.processEvents();
    auto* const layout_choice =
        main_window.findChild<QComboBox*>(QStringLiteral("preview-layout-choice"));
    const bool restored =
        !tiles[0]->isWindow() &&
        std::ranges::all_of(tiles, [](const QWidget* tile) { return tile->isVisible(); }) &&
        tiles[0]->parentWidget() == grid && tiles[0]->size() == initial_sizes[0] &&
        !first_image->hasScaledContents() && layout_choice && layout_choice->currentIndex() == 0;
    return small_frame_kept_sizes && large_frame_kept_sizes && focused && full_screen && restored;
}

} // namespace

#if defined(_MSC_VER)
// This entry point intentionally owns the GUI objects for the application lifetime. The
// statically reported frame remains bounded (well below the Windows main-thread stack).
#pragma warning(suppress : 6262)
#endif
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
    log_config.file_stem = "paperbreak-console";
    auto logging_result = paperbreak::logging::LoggingRuntime::create(log_config);
    if (!logging_result)
        return 1;
    auto logging = std::move(logging_result).value();
    auto* const logging_runtime = logging.get();
    auto gui_registration_result = logging->register_current_thread("console-gui");
    if (!gui_registration_result)
    {
        static_cast<void>(logging->shutdown());
        return 1;
    }
    auto gui_registration = std::move(gui_registration_result).value();
    const paperbreak::ThreadRegistrationFactory register_thread =
        [logging_runtime](const std::string_view name) -> std::shared_ptr<void> {
        auto registration = logging_runtime->register_current_thread(name);
        if (!registration)
            return {};
        return std::make_shared<paperbreak::logging::LoggingRuntime::ThreadRegistration>(
            std::move(registration).value());
    };
    paperbreak::ipc::IpcClientOptions console_ipc_options;
    console_ipc_options.diagnostics = {
        .enabled =
            [logging_runtime] {
                return logging_runtime->enabled(paperbreak::logging::Level::debug);
            },
        .record =
            [logging_runtime](std::string message) {
                static_cast<void>(logging_runtime->log(paperbreak::logging::Category::ipc,
                                                       paperbreak::logging::Level::debug, message));
            }};

    std::unique_ptr<paperbreak::console::PreviewClient> preview_client;
    std::unique_ptr<paperbreak::console::CameraClient> camera_client;
    std::unique_ptr<paperbreak::console::OperationsClient> operations_client;
    std::unique_ptr<paperbreak::console::AlgorithmClient> algorithm_client;
    std::unique_ptr<paperbreak::console::EventClient> event_client;
    std::unique_ptr<paperbreak::console::StorageClient> storage_client;
    std::unique_ptr<paperbreak::console::UplinkClient> uplink_client;
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
             }},
        {.refresh =
             [&algorithm_client] {
                 if (algorithm_client)
                     algorithm_client->refresh();
             },
         .select_camera =
             [&algorithm_client](std::string camera_id) {
                 if (algorithm_client)
                     return algorithm_client->select_camera(std::move(camera_id));
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "算法客户端尚未初始化",
                     "console", "console.algorithm.selectCamera", true));
             },
         .update_configuration =
             [&algorithm_client](paperbreak::console::AlgorithmConfigurationValue value) {
                 if (algorithm_client)
                     return algorithm_client->update_configuration(std::move(value));
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "算法客户端尚未初始化",
                     "console", "console.algorithm.updateConfig", true));
             },
         .test_current_frame =
             [&algorithm_client] {
                 if (algorithm_client)
                     return algorithm_client->test_current_frame();
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "算法客户端尚未初始化",
                     "console", "console.algorithm.testCurrentFrame", true));
             }},
        {.refresh =
             [&event_client] {
                 if (event_client)
                     event_client->refresh();
             },
         .query =
             [&event_client](paperbreak::console::EventListFilter filter) {
                 if (event_client)
                     return event_client->query(std::move(filter));
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "事件客户端尚未初始化",
                     "console", "console.event.list", true));
             },
         .get =
             [&event_client](std::string event_id) {
                 if (event_client)
                     return event_client->get(std::move(event_id));
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "事件客户端尚未初始化",
                     "console", "console.event.get", true));
             },
         .update_configuration =
             [&event_client](paperbreak::console::EventConfigurationValue value) {
                 if (event_client)
                     return event_client->update_configuration(std::move(value));
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "事件客户端尚未初始化",
                     "console", "console.event.updateConfig", true));
             },
         .manual_trigger =
             [&event_client](std::string camera_id) {
                 if (event_client)
                     return event_client->manual_trigger(std::move(camera_id));
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "事件客户端尚未初始化",
                     "console", "console.event.manualTrigger", true));
             },
         .review =
             [&event_client](std::string event_id, const std::uint64_t revision,
                             const bool confirmed) {
                 if (event_client)
                     return event_client->review(std::move(event_id), revision, confirmed);
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "事件客户端尚未初始化",
                     "console", "console.event.review", true));
             },
         .export_event =
             [&event_client](std::string event_id, std::filesystem::path destination) {
                 if (event_client)
                     return event_client->export_event(std::move(event_id), std::move(destination));
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "事件客户端尚未初始化",
                     "console", "console.event.export", true));
             },
         .retry_upload =
             [&event_client](std::string event_id) {
                 if (event_client)
                     return event_client->retry_upload(std::move(event_id));
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "事件客户端尚未初始化",
                     "console", "console.event.retryUpload", true));
             }},
        {.refresh =
             [&storage_client] {
                 if (storage_client)
                     storage_client->refresh();
             },
         .update_configuration =
             [&storage_client](paperbreak::console::StorageConfigurationValue value) {
                 if (storage_client)
                     return storage_client->update_configuration(std::move(value));
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "存储客户端尚未初始化",
                     "console", "console.storage.updateConfig", true));
             }},
        {.refresh =
             [&uplink_client] {
                 if (uplink_client)
                     uplink_client->refresh();
             },
         .update_configuration =
             [&uplink_client](paperbreak::console::UplinkConfigurationValue value) {
                 if (uplink_client)
                     return uplink_client->update_configuration(std::move(value));
                 return paperbreak::Result<void>::failure(paperbreak::make_error(
                     "IPC_NOT_CONNECTED", paperbreak::Severity::warning, "上位机客户端尚未初始化",
                     "console", "console.uplink.updateConfig", true));
             }});
    preview_client = std::make_unique<paperbreak::console::PreviewClient>(
        [&main_window](const paperbreak::console::PreviewSnapshot& snapshot) {
            main_window.apply_preview_snapshot(snapshot);
        },
        console_ipc_options);
    camera_client = std::make_unique<paperbreak::console::CameraClient>(
        [&main_window, &preview_client](const paperbreak::console::CameraClientSnapshot& snapshot) {
            main_window.apply_camera_snapshot(snapshot);
            std::vector<std::string> camera_ids;
            camera_ids.reserve(snapshot.cameras.size());
            for (const auto& camera : snapshot.cameras)
                camera_ids.push_back(camera.id);
            if (preview_client && !camera_ids.empty())
                preview_client->set_camera_ids(std::move(camera_ids));
        },
        console_ipc_options);
    if (!smoke_test)
    {
        operations_client = std::make_unique<paperbreak::console::OperationsClient>(
            [&main_window](const paperbreak::console::OperationsSnapshot& snapshot) {
                main_window.apply_operations_snapshot(snapshot);
            },
            console_ipc_options, register_thread);
        algorithm_client = std::make_unique<paperbreak::console::AlgorithmClient>(
            [&main_window](const paperbreak::console::AlgorithmClientSnapshot& snapshot) {
                main_window.apply_algorithm_snapshot(snapshot);
            },
            console_ipc_options);
        event_client = std::make_unique<paperbreak::console::EventClient>(
            [&main_window](const paperbreak::console::EventClientSnapshot& snapshot) {
                main_window.apply_event_snapshot(snapshot);
            },
            console_ipc_options, register_thread);
        storage_client = std::make_unique<paperbreak::console::StorageClient>(
            [&main_window](const paperbreak::console::StorageClientSnapshot& snapshot) {
                main_window.apply_storage_snapshot(snapshot);
            },
            console_ipc_options);
        uplink_client = std::make_unique<paperbreak::console::UplinkClient>(
            [&main_window](const paperbreak::console::UplinkClientSnapshot& snapshot) {
                main_window.apply_uplink_snapshot(snapshot);
            },
            console_ipc_options);
    }
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
                    auto thread_registration_result =
                        logging_runtime->register_current_thread("console-service-restart");
                    std::optional<paperbreak::logging::LoggingRuntime::ThreadRegistration>
                        thread_registration;
                    if (thread_registration_result)
                        thread_registration = std::move(thread_registration_result).value();
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
            if (snapshot.service_status)
            {
                if (const auto level =
                        paperbreak::logging::parse_level(snapshot.service_status->logging_level))
                    static_cast<void>(logging_runtime->set_minimum_level(*level));
            }
            tray.apply_snapshot(snapshot);
            main_window.apply_snapshot(snapshot);
        },
        console_ipc_options);
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
    if (!smoke_test)
        static_cast<void>(algorithm_client->start());
    if (!smoke_test)
        static_cast<void>(event_client->start());
    if (!smoke_test)
        static_cast<void>(storage_client->start());
    if (!smoke_test)
        static_cast<void>(uplink_client->start());

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
        camera_smoke.cameras.push_back(
            {.id = "CAM01", .location = "入口", .state = "disconnected", .serial = "CONFIG-01"});
        camera_smoke.cameras.push_back(
            {.id = "CAM02", .location = "出口", .state = "connected", .serial = "CONFIG-02"});
        camera_smoke.cameras.front().saved.reverse_x = true;
        camera_smoke.cameras.front().saved.reverse_y = false;
        camera_smoke.cameras.back().saved.reverse_x = false;
        camera_smoke.cameras.back().saved.reverse_y = true;
        camera_smoke.cameras.back().roi_capabilities =
            paperbreak::console::CameraRoiCapabilitiesValue{
                .sensor_width = 1624U,
                .sensor_height = 1240U,
                .width = {32U, 1624U, 4U},
                .height = {4U, 1240U, 4U},
                .offset_x = {0U, 1592U, 2U},
                .offset_y = {0U, 1232U, 16U}};
        camera_smoke.discovered_devices.front().exclusive_access_available = true;
        main_window.apply_camera_snapshot(camera_smoke);
        auto* const camera_selector =
            main_window.findChild<QComboBox*>(QStringLiteral("camera-selector"));
        auto* const camera_status =
            main_window.findChild<QLabel*>(QStringLiteral("camera-configuration-status"));
        auto* const camera_discover =
            main_window.findChild<QPushButton*>(QStringLiteral("camera-discover"));
        auto* const camera_discovered_panel =
            main_window.findChild<QWidget*>(QStringLiteral("camera-discovered-panel"));
        auto* const discovered_devices =
            main_window.findChild<QWidget*>(QStringLiteral("discovered-devices"));
        auto* const reverse_x =
            main_window.findChild<QCheckBox*>(QStringLiteral("camera-reverse-x"));
        auto* const reverse_y =
            main_window.findChild<QCheckBox*>(QStringLiteral("camera-reverse-y"));
        const bool first_camera_mirroring_loaded =
            reverse_x && reverse_y && reverse_x->isChecked() && !reverse_y->isChecked();
        const bool camera_discover_above_list =
            camera_discover && camera_discovered_panel && discovered_devices &&
            camera_discover->parentWidget() == camera_discovered_panel &&
            camera_discovered_panel->layout() &&
            camera_discovered_panel->layout()->indexOf(camera_discover) <
                camera_discovered_panel->layout()->indexOf(discovered_devices);
        const bool first_camera_status_only =
            camera_selector && camera_status && camera_selector->currentText() ==
                                                   QStringLiteral("CAM01") &&
            camera_status->text().contains(QStringLiteral("CAM01")) &&
            !camera_status->text().contains(QStringLiteral("CAM02"));
        if (camera_selector)
            camera_selector->setCurrentText(QStringLiteral("CAM02"));
        application.processEvents();
        const bool selected_camera_status_only =
            camera_status && camera_status->text().contains(QStringLiteral("CAM02")) &&
            !camera_status->text().contains(QStringLiteral("CAM01"));
        const bool selected_camera_mirroring_loaded =
            reverse_x && reverse_y && !reverse_x->isChecked() && reverse_y->isChecked();
        auto* const offset_y =
            main_window.findChild<QSpinBox*>(QStringLiteral("camera-roi-offset-y"));
        const bool roi_capabilities_loaded =
            offset_y && offset_y->minimum() == 0 && offset_y->maximum() == 1232 &&
            offset_y->singleStep() == 16;
        if (offset_y)
        {
            offset_y->setValue(100);
            static_cast<void>(QMetaObject::invokeMethod(offset_y, "editingFinished",
                                                        Qt::DirectConnection));
        }
        const bool roi_offset_aligned = offset_y && offset_y->value() == 96;
        const bool camera_banner_removed =
            std::ranges::none_of(main_window.findChildren<QLabel*>(), [](const QLabel* label) {
                return label && label->text() == QStringLiteral("相机配置与实际值");
            });
        const bool fixed_acquisition_controls_removed =
            !main_window.findChild<QWidget*>(QStringLiteral("camera-trigger-panel")) &&
            !main_window.findChild<QPushButton*>(
                QStringLiteral("camera-camera.softwareTrigger"));
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
        operations_smoke.metrics.push_back(
            {.name = "storage.nvme.state", .value = "running", .unit = "state", .available = true});
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
        paperbreak::console::EventClientSnapshot event_smoke;
        event_smoke.connection.state = paperbreak::ipc::ClientConnectionState::connected;
        event_smoke.configuration_stale = false;
        event_smoke.events_stale = false;
        event_smoke.stored_config_revision = 1U;
        main_window.apply_event_snapshot(event_smoke);
        paperbreak::console::StorageClientSnapshot storage_smoke;
        storage_smoke.connection.state = paperbreak::ipc::ClientConnectionState::connected;
        storage_smoke.stale = false;
        storage_smoke.stored_config_revision = 1U;
        storage_smoke.effective_config_revision = 1U;
        storage_smoke.configuration.event_root = "data/events";
        storage_smoke.configuration.cache_root = "data/cache";
        storage_smoke.effective_configuration = storage_smoke.configuration;
        main_window.apply_storage_snapshot(storage_smoke);
        paperbreak::console::UplinkClientSnapshot uplink_smoke;
        uplink_smoke.connection.state = paperbreak::ipc::ClientConnectionState::connected;
        uplink_smoke.stale = false;
        uplink_smoke.stored_config_revision = 1U;
        uplink_smoke.effective_config_revision = 1U;
        uplink_smoke.configuration.server_url = "http://127.0.0.1:18080";
        uplink_smoke.effective_configuration = uplink_smoke.configuration;
        main_window.apply_uplink_snapshot(uplink_smoke);
        paperbreak::console::AlgorithmClientSnapshot algorithm_smoke;
        algorithm_smoke.connection.state = paperbreak::ipc::ClientConnectionState::connected;
        algorithm_smoke.stale = false;
        algorithm_smoke.stored_config_revision = 1U;
        algorithm_smoke.effective_config_revision = 1U;
        algorithm_smoke.runtime.camera_id = "CAM01";
        algorithm_smoke.runtime.state = "active";
        algorithm_smoke.runtime.plugin_id = "classical-vision";
        algorithm_smoke.runtime.display_name = "M6 Classical Vision Prototype";
        algorithm_smoke.runtime.implementation_version = "1.0.0-prototype";
        algorithm_smoke.runtime.detector_model_version = "none";
        algorithm_smoke.runtime.prototype_only = true;
        algorithm_smoke.runtime.has_current_frame = true;
        main_window.apply_algorithm_snapshot(algorithm_smoke);
        auto* const camera_parameter_grid =
            main_window.findChild<QWidget*>(QStringLiteral("camera-editor"));
        auto* const camera_control_grid =
            main_window.findChild<QWidget*>(QStringLiteral("camera-control-grid"));
        auto* const camera_scroll =
            main_window.findChild<QScrollArea*>(QStringLiteral("camera-configuration-scroll"));
        const bool camera_page_selected = main_window.select_page(2U);
        main_window.resize(1680, 900);
        application.processEvents();
        const int wide_parameter_columns =
            camera_parameter_grid ? camera_parameter_grid->property("layoutColumns").toInt() : 0;
        const int wide_control_columns =
            camera_control_grid ? camera_control_grid->property("layoutColumns").toInt() : 0;
        main_window.resize(1040, 680);
        application.processEvents();
        const int narrow_parameter_columns =
            camera_parameter_grid ? camera_parameter_grid->property("layoutColumns").toInt() : 0;
        const int narrow_control_columns =
            camera_control_grid ? camera_control_grid->property("layoutColumns").toInt() : 0;
        const bool camera_layout_responsive =
            camera_page_selected && wide_parameter_columns >= 2 &&
            narrow_parameter_columns < wide_parameter_columns && wide_control_columns >= 4 &&
            narrow_control_columns < wide_control_columns && camera_scroll &&
            !camera_scroll->horizontalScrollBar()->isVisible();
        if (!camera_layout_responsive)
        {
            std::cerr << "camera layout smoke failed: selected=" << camera_page_selected
                      << " parameterColumns=" << wide_parameter_columns << "/"
                      << narrow_parameter_columns << " controlColumns=" << wide_control_columns
                      << "/" << narrow_control_columns << " horizontalScrollVisible="
                      << (camera_scroll && camera_scroll->horizontalScrollBar()->isVisible())
                      << '\n';
        }
        main_window.resize(1280, 800);
        application.processEvents();
        const bool preview_panes_stable_and_cycle = preview_pane_smoke(main_window, application);
        if (!preview_panes_stable_and_cycle)
            std::cerr << "preview pane sizing or double-click cycle smoke failed\n";
        static_cast<void>(main_window.select_page(0U));
        auto* const event_upload_policy =
            main_window.findChild<QComboBox*>(QStringLiteral("event-upload-policy"));
        auto* const event_config_save =
            main_window.findChild<QPushButton*>(QStringLiteral("event-config-save"));
        const bool event_configuration_editable =
            event_upload_policy && event_upload_policy->findData(QStringLiteral("never")) >= 0 &&
            event_config_save && event_config_save->isEnabled();
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
                   main_window.camera_configuration_ready() && camera_layout_responsive &&
                   preview_panes_stable_and_cycle && camera_banner_removed &&
                   fixed_acquisition_controls_removed &&
                   camera_discover_above_list && first_camera_status_only &&
                   selected_camera_status_only && first_camera_mirroring_loaded &&
                   selected_camera_mirroring_loaded && roi_capabilities_loaded &&
                   roi_offset_aligned && empty_configuration_kept_discovery &&
                   restart_state_disabled_controls && main_window.operations_pages_ready() &&
                   main_window.algorithm_page_ready() && main_window.event_pages_ready() &&
                   main_window.storage_page_ready() && main_window.uplink_page_ready() &&
                   event_configuration_editable && local_time_displayed &&
                   diagnostic_enabled_when_connected && diagnostic_disabled_when_disconnected &&
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
    refresh_timer.start(5000);
    QObject::connect(&refresh_timer, &QTimer::timeout,
                     [&camera_client] { camera_client->refresh(); });
    QObject::connect(&refresh_timer, &QTimer::timeout, [&operations_client] {
        if (operations_client)
            operations_client->refresh();
    });
    QObject::connect(&refresh_timer, &QTimer::timeout, [&algorithm_client] {
        if (algorithm_client)
            algorithm_client->refresh();
    });
    QObject::connect(&refresh_timer, &QTimer::timeout, [&event_client] {
        if (event_client)
            event_client->refresh();
    });
    QObject::connect(&refresh_timer, &QTimer::timeout, [&storage_client] {
        if (storage_client)
            storage_client->refresh();
    });
    QObject::connect(&refresh_timer, &QTimer::timeout, [&uplink_client] {
        if (uplink_client)
            uplink_client->refresh();
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
    if (algorithm_client)
    {
        algorithm_client->stop();
        algorithm_client.reset();
    }
    if (event_client)
    {
        event_client->stop();
        event_client.reset();
    }
    if (storage_client)
    {
        storage_client->stop();
        storage_client.reset();
    }
    if (uplink_client)
    {
        uplink_client->stop();
        uplink_client.reset();
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
