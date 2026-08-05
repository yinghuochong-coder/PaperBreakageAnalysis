#include "system_tray_controller.hpp"

#include "paperbreak/console/tray_status_model.hpp"

#include <QAction>
#include <QColor>
#include <QPainter>
#include <QPixmap>

#include <utility>

namespace paperbreak::console
{
namespace
{

QColor status_color(const TrayStatusColor color)
{
    switch (color)
    {
    case TrayStatusColor::green:
        return QColor{34, 153, 84};
    case TrayStatusColor::yellow:
        return QColor{230, 166, 30};
    case TrayStatusColor::red:
        return QColor{205, 55, 55};
    case TrayStatusColor::gray:
        return QColor{128, 128, 128};
    }
    return QColor{128, 128, 128};
}

QIcon status_icon(const TrayStatusColor color)
{
    QPixmap pixmap{32, 32};
    pixmap.fill(Qt::transparent);
    QPainter painter{&pixmap};
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen{Qt::white, 2.0});
    painter.setBrush(status_color(color));
    painter.drawEllipse(4, 4, 24, 24);
    return QIcon{pixmap};
}

} // namespace

SystemTrayController::SystemTrayController(TrayCallbacks callbacks)
{
    auto double_click_open = callbacks.open_console;
    auto* open_action = menu_.addAction(QStringLiteral("打开控制台"));
    status_action_ = menu_.addAction(QStringLiteral("显示当前状态：正在同步"));
    preview_action_ = menu_.addAction(QStringLiteral("请在实时预览页暂停/恢复"));
    restart_action_ = menu_.addAction(QStringLiteral("重启后台服务"));
    event_directory_action_ = menu_.addAction(QStringLiteral("打开事件目录"));
    diagnostics_action_ = menu_.addAction(QStringLiteral("导出脱敏诊断包"));
    auto* about_action = menu_.addAction(QStringLiteral("关于"));
    auto* quit_action = menu_.addAction(QStringLiteral("退出界面"));

    preview_action_->setEnabled(false);
    diagnostics_action_->setEnabled(false);
    event_directory_action_->setEnabled(false);

    QObject::connect(open_action, &QAction::triggered, std::move(callbacks.open_console));
    QObject::connect(status_action_, &QAction::triggered, std::move(callbacks.show_status));
    QObject::connect(restart_action_, &QAction::triggered, std::move(callbacks.restart_service));
    QObject::connect(event_directory_action_, &QAction::triggered,
                     std::move(callbacks.open_event_directory));
    QObject::connect(diagnostics_action_, &QAction::triggered,
                     std::move(callbacks.export_diagnostics));
    QObject::connect(about_action, &QAction::triggered, std::move(callbacks.show_about));
    QObject::connect(quit_action, &QAction::triggered, std::move(callbacks.quit_interface));
    QObject::connect(
        &tray_, &QSystemTrayIcon::activated,
        [open = std::move(double_click_open)](const QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::DoubleClick && open)
                open();
        });

    tray_.setContextMenu(&menu_);
    tray_.setIcon(status_icon(TrayStatusColor::gray));
    tray_.setToolTip(QStringLiteral("PaperBreakEdge — 无法连接本地服务"));
}

void SystemTrayController::show()
{
    tray_.show();
}

void SystemTrayController::hide()
{
    tray_.hide();
}

void SystemTrayController::apply_snapshot(const ClientStateSnapshot& snapshot)
{
    const TrayStatus status = tray_status(snapshot);
    tray_.setIcon(status_icon(status.color));
    const QString label =
        QString::fromUtf8(status.label.data(), static_cast<qsizetype>(status.label.size()));
    tray_.setToolTip(QStringLiteral("PaperBreakEdge — %1").arg(label));
    status_action_->setText(QStringLiteral("显示当前状态：%1").arg(label));
    event_directory_action_->setEnabled(
        snapshot.connection.state == ipc::ClientConnectionState::connected &&
        !snapshot.locations_stale && snapshot.locations.has_value());
    diagnostics_action_->setEnabled(snapshot.connection.state ==
                                    ipc::ClientConnectionState::connected);

    if (status_initialized_ && status.color != last_color_ &&
        (status.color == TrayStatusColor::yellow || status.color == TrayStatusColor::red))
    {
        tray_.showMessage(QStringLiteral("PaperBreakEdge 状态变化"), label,
                          status.color == TrayStatusColor::red ? QSystemTrayIcon::Critical
                                                               : QSystemTrayIcon::Warning,
                          5000);
    }
    status_initialized_ = true;
    last_color_ = status.color;
}

bool SystemTrayController::is_visible() const noexcept
{
    return tray_.isVisible();
}

std::size_t SystemTrayController::action_count() const noexcept
{
    return static_cast<std::size_t>(menu_.actions().size());
}

bool SystemTrayController::preview_action_enabled() const noexcept
{
    return preview_action_->isEnabled();
}

bool SystemTrayController::diagnostics_action_enabled() const noexcept
{
    return diagnostics_action_->isEnabled();
}

} // namespace paperbreak::console
