#include "main_window.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <limits>
#include <string>

namespace paperbreak::uplink::simulator
{
namespace
{
QString bytes_text(const std::uint64_t bytes)
{
    constexpr double gib = 1024.0 * 1024.0 * 1024.0;
    return QStringLiteral("%1 GiB").arg(static_cast<double>(bytes) / gib, 0, 'f', 2);
}

QTableWidgetItem* item(const QString& text)
{
    auto* result = new QTableWidgetItem(text);
    result->setFlags(result->flags() & ~Qt::ItemIsEditable);
    return result;
}

} // namespace

MainWindow::MainWindow(Runtime& runtime, Options options, QWidget* parent)
    : QMainWindow(parent), runtime_(runtime), options_(std::move(options))
{
    setWindowTitle(QStringLiteral("PaperBreak Uplink v1 上位机模拟器"));
    resize(1280, 820);
    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);

    auto* warning = new QLabel(
        QStringLiteral("高风险：当前正式 Uplink v1 为明文、无鉴权，并默认监听全部网卡。"
                       "任何可访问端点的主机都可能读取数据或伪造命令；仅限隔离测试网络。"),
        central);
    warning->setWordWrap(true);
    warning->setStyleSheet(QStringLiteral(
        "QLabel { color: white; background: #b42318; padding: 10px; font-weight: bold; }"));
    root->addWidget(warning);

    auto* controls = new QHBoxLayout;
    status_label_ = new QLabel(central);
    workspace_label_ = new QLabel(central);
    start_button_ = new QPushButton(QStringLiteral("启动服务"), central);
    stop_button_ = new QPushButton(QStringLiteral("停止服务"), central);
    controls->addWidget(status_label_);
    controls->addSpacing(20);
    controls->addWidget(workspace_label_, 1);
    controls->addWidget(start_button_);
    controls->addWidget(stop_button_);
    root->addLayout(controls);

    auto* tabs = new QTabWidget(central);
    root->addWidget(tabs, 1);

    auto* overview = new QWidget(tabs);
    auto* overview_layout = new QVBoxLayout(overview);
    devices_ = new QTableWidget(0, 11, overview);
    devices_->setHorizontalHeaderLabels(
        {QStringLiteral("工控机"), QStringLiteral("生产线"), QStringLiteral("版本"),
         QStringLiteral("连接"), QStringLiteral("最后活动"), QStringLiteral("消息"),
         QStringLiteral("最后类型"), QStringLiteral("报警"), QStringLiteral("预览"),
         QStringLiteral("事件"), QStringLiteral("上传")});
    devices_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    devices_->horizontalHeader()->setStretchLastSection(true);
    overview_layout->addWidget(devices_);
    tabs->addTab(overview, QStringLiteral("设备与状态"));

    auto* transfer_page = new QWidget(tabs);
    auto* transfer_layout = new QVBoxLayout(transfer_page);
    auto* transfer_help = new QLabel(
        QStringLiteral("事件数量见设备页；下表显示持久化文件上传与断点进度。"), transfer_page);
    uploads_ = new QTableWidget(0, 7, transfer_page);
    uploads_->setHorizontalHeaderLabels({QStringLiteral("工控机"), QStringLiteral("事件"),
                                         QStringLiteral("逻辑文件"), QStringLiteral("uploadId"),
                                         QStringLiteral("状态"), QStringLiteral("已接收"),
                                         QStringLiteral("总字节")});
    uploads_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    uploads_->horizontalHeader()->setStretchLastSection(true);
    transfer_layout->addWidget(transfer_help);
    transfer_layout->addWidget(uploads_, 1);
    tabs->addTab(transfer_page, QStringLiteral("事件与上传"));

    auto* preview_page = new QWidget(tabs);
    auto* preview_layout = new QVBoxLayout(preview_page);
    preview_label_ = new QLabel(QStringLiteral("尚未收到预览帧"), preview_page);
    preview_label_->setAlignment(Qt::AlignCenter);
    preview_label_->setMinimumSize(640, 360);
    preview_label_->setStyleSheet(
        QStringLiteral("QLabel { background: #101828; color: #d0d5dd; }"));
    preview_layout->addWidget(preview_label_, 1);
    tabs->addTab(preview_page, QStringLiteral("实时预览"));

    auto* command_page = new QWidget(tabs);
    auto* command_layout = new QVBoxLayout(command_page);
    auto* command_form = new QFormLayout;
    command_device_ = new QComboBox(command_page);
    command_type_ = new QComboBox(command_page);
    const std::array<const char*, 14> commands{
        "system.requestStatus",   "config.replace", "event.review",        "event.retryUpload",
        "camera.discover",        "camera.bind",    "camera.connect",      "camera.disconnect",
        "camera.start",           "camera.stop",    "camera.updateConfig", "camera.captureSnapshot",
        "camera.softwareTrigger", "service.restart"};
    for (const char* command : commands)
        command_type_->addItem(QString::fromLatin1(command));
    command_payload_ = new QPlainTextEdit(QStringLiteral("{}"), command_page);
    command_payload_->setMaximumBlockCount(1000);
    auto* send = new QPushButton(QStringLiteral("下发命令"), command_page);
    command_form->addRow(QStringLiteral("目标设备"), command_device_);
    command_form->addRow(QStringLiteral("命令类型"), command_type_);
    command_form->addRow(QStringLiteral("JSON payload"), command_payload_);
    command_layout->addLayout(command_form);
    command_layout->addWidget(send);
    command_layout->addStretch();
    tabs->addTab(command_page, QStringLiteral("远程命令"));

    auto* fault_page = new QWidget(tabs);
    auto* fault_layout = new QFormLayout(fault_page);
    fault_device_ = new QComboBox(fault_page);
    reject_connections_ = new QCheckBox(QStringLiteral("拒绝 HTTP/新连接"), fault_page);
    disconnect_websockets_ = new QCheckBox(QStringLiteral("拒绝 WebSocket"), fault_page);
    duplicate_acks_ = new QCheckBox(QStringLiteral("重复确认"), fault_page);
    replay_commands_ = new QCheckBox(QStringLiteral("命令重放"), fault_page);
    checksum_mismatch_ = new QCheckBox(QStringLiteral("强制校验失败"), fault_page);
    response_delay_ = new QSpinBox(fault_page);
    response_delay_->setRange(0, 60000);
    response_delay_->setSuffix(QStringLiteral(" ms"));
    fail_next_ = new QSpinBox(fault_page);
    fail_next_->setRange(0, 10000);
    disconnect_chunk_ = new QLineEdit(fault_page);
    disconnect_chunk_->setPlaceholderText(QStringLiteral("留空禁用；否则填写 0～4294967295"));
    auto* apply = new QPushButton(QStringLiteral("应用故障配置"), fault_page);
    fault_layout->addRow(QStringLiteral("目标设备"), fault_device_);
    fault_layout->addRow(reject_connections_);
    fault_layout->addRow(disconnect_websockets_);
    fault_layout->addRow(duplicate_acks_);
    fault_layout->addRow(replay_commands_);
    fault_layout->addRow(checksum_mismatch_);
    fault_layout->addRow(QStringLiteral("响应延迟"), response_delay_);
    fault_layout->addRow(QStringLiteral("失败后续请求数"), fail_next_);
    fault_layout->addRow(QStringLiteral("指定分块后中断"), disconnect_chunk_);
    fault_layout->addRow(apply);
    tabs->addTab(fault_page, QStringLiteral("故障注入"));

    auto* log_page = new QWidget(tabs);
    auto* log_layout = new QVBoxLayout(log_page);
    logs_ = new QPlainTextEdit(log_page);
    logs_->setReadOnly(true);
    logs_->setMaximumBlockCount(200);
    log_layout->addWidget(logs_);
    tabs->addTab(log_page, QStringLiteral("运行日志"));

    setCentralWidget(central);
    timer_ = new QTimer(this);
    timer_->setInterval(250);
    connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
    connect(start_button_, &QPushButton::clicked, this, [this] {
        auto result = runtime_.start(options_);
        if (!result)
            QMessageBox::critical(this, QStringLiteral("启动失败"),
                                  QString::fromStdString(result.error().business_code + ": " +
                                                         result.error().message));
    });
    connect(stop_button_, &QPushButton::clicked, this, [this] { runtime_.stop(); });
    connect(send, &QPushButton::clicked, this, [this] { send_command(); });
    connect(apply, &QPushButton::clicked, this, [this] { apply_fault_profile(); });
    timer_->start();
    refresh();
}

void MainWindow::refresh_device_choices(const Snapshot& snapshot)
{
    const QString command_current = command_device_->currentText();
    const QString fault_current = fault_device_->currentText();
    command_device_->blockSignals(true);
    fault_device_->blockSignals(true);
    command_device_->clear();
    fault_device_->clear();
    for (const auto& device : snapshot.devices)
    {
        const QString machine = QString::fromStdString(device.machine_id);
        command_device_->addItem(machine);
        fault_device_->addItem(machine);
    }
    const int command_index = command_device_->findText(command_current);
    const int fault_index = fault_device_->findText(fault_current);
    if (command_index >= 0)
        command_device_->setCurrentIndex(command_index);
    if (fault_index >= 0)
        fault_device_->setCurrentIndex(fault_index);
    command_device_->blockSignals(false);
    fault_device_->blockSignals(false);
}

void MainWindow::refresh()
{
    const Snapshot snapshot = runtime_.snapshot();
    status_label_->setText(snapshot.running
                               ? QStringLiteral("运行中：http://%1:%2")
                                     .arg(QString::fromStdString(snapshot.listen_address))
                                     .arg(snapshot.port)
                               : QStringLiteral("已停止"));
    status_label_->setStyleSheet(snapshot.running
                                     ? QStringLiteral("color:#027a48;font-weight:bold;")
                                     : QStringLiteral("color:#b42318;font-weight:bold;"));
    workspace_label_->setText(QStringLiteral("工作区：%1 / %2；存储队列：%3（高水位 %4，拒绝 %5）")
                                  .arg(bytes_text(snapshot.workspace_used_bytes),
                                       bytes_text(snapshot.workspace_limit_bytes))
                                  .arg(snapshot.storage_queue_depth)
                                  .arg(snapshot.storage_queue_high_watermark)
                                  .arg(snapshot.rejected_storage_tasks));
    start_button_->setEnabled(!snapshot.running);
    stop_button_->setEnabled(snapshot.running);

    devices_->setRowCount(static_cast<int>(snapshot.devices.size()));
    for (std::size_t row = 0U; row < snapshot.devices.size(); ++row)
    {
        const auto& device = snapshot.devices[row];
        const int index = static_cast<int>(row);
        devices_->setItem(index, 0, item(QString::fromStdString(device.machine_id)));
        devices_->setItem(index, 1, item(QString::fromStdString(device.production_line_id)));
        devices_->setItem(index, 2, item(QString::fromStdString(device.software_version)));
        devices_->setItem(
            index, 3, item(device.connected ? QStringLiteral("已连接") : QStringLiteral("已断开")));
        devices_->setItem(index, 4, item(QString::fromStdString(device.last_seen)));
        devices_->setItem(index, 5, item(QString::number(device.received_messages)));
        devices_->setItem(index, 6, item(QString::fromStdString(device.last_message_type)));
        devices_->setItem(index, 7, item(QString::number(device.alarm_count)));
        devices_->setItem(index, 8,
                          item(QStringLiteral("%1 / 覆盖 %2")
                                   .arg(device.received_previews)
                                   .arg(device.overwritten_previews)));
        devices_->setItem(index, 9, item(QString::number(device.event_count)));
        devices_->setItem(index, 10, item(QString::number(device.upload_count)));
    }
    refresh_device_choices(snapshot);

    uploads_->setRowCount(static_cast<int>(snapshot.uploads.size()));
    for (std::size_t row = 0U; row < snapshot.uploads.size(); ++row)
    {
        const auto& upload = snapshot.uploads[row];
        const int index = static_cast<int>(row);
        uploads_->setItem(index, 0, item(QString::fromStdString(upload.machine_id)));
        uploads_->setItem(index, 1, item(QString::fromStdString(upload.event_id)));
        uploads_->setItem(index, 2, item(QString::fromStdString(upload.logical_file_id)));
        uploads_->setItem(index, 3, item(QString::fromStdString(upload.upload_id)));
        uploads_->setItem(index, 4, item(QString::fromStdString(upload.state)));
        uploads_->setItem(index, 5, item(QString::number(upload.received_bytes)));
        uploads_->setItem(index, 6, item(QString::number(upload.total_bytes)));
    }

    const int selected_row = devices_->currentRow() >= 0 ? devices_->currentRow() : 0;
    if (selected_row < static_cast<int>(snapshot.devices.size()))
    {
        const auto& device = snapshot.devices[static_cast<std::size_t>(selected_row)];
        if (!device.latest_preview_jpeg.empty())
        {
            const QImage image =
                QImage::fromData(reinterpret_cast<const uchar*>(device.latest_preview_jpeg.data()),
                                 static_cast<int>(device.latest_preview_jpeg.size()), "JPEG");
            if (!image.isNull())
                preview_label_->setPixmap(QPixmap::fromImage(image).scaled(
                    preview_label_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    }
    if (snapshot.recent_logs.size() != last_log_count_)
    {
        logs_->clear();
        for (const auto& record : snapshot.recent_logs)
            logs_->appendPlainText(QString::fromStdString(record));
        last_log_count_ = snapshot.recent_logs.size();
    }
}

void MainWindow::send_command()
{
    if (command_device_->currentText().isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("无法下发"),
                             QStringLiteral("没有可用目标设备。"));
        return;
    }
    const QString type = command_type_->currentText();
    const bool dangerous = type != QStringLiteral("system.requestStatus");
    if (dangerous &&
        QMessageBox::warning(
            this, QStringLiteral("危险远程命令"),
            QStringLiteral("协议无鉴权，且该命令可能改变边缘设备状态。确认下发 %1？").arg(type),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    CommandRequest command{
        .command_id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
        .machine_id = command_device_->currentText().toStdString(),
        .command_type = type.toStdString(),
        .deadline =
            QDateTime::currentDateTimeUtc().addSecs(300).toString(Qt::ISODateWithMs).toStdString(),
        .payload_json = command_payload_->toPlainText().toStdString(),
        .operator_confirmed = dangerous};
    auto result = runtime_.enqueue_command(std::move(command));
    if (!result)
        QMessageBox::critical(
            this, QStringLiteral("命令未入队"),
            QString::fromStdString(result.error().business_code + ": " + result.error().message));
}

void MainWindow::apply_fault_profile()
{
    if (fault_device_->currentText().isEmpty())
    {
        QMessageBox::warning(this, QStringLiteral("无法应用"),
                             QStringLiteral("没有可用目标设备。"));
        return;
    }
    FaultProfile profile{.reject_connections = reject_connections_->isChecked(),
                         .disconnect_websockets = disconnect_websockets_->isChecked(),
                         .duplicate_acknowledgements = duplicate_acks_->isChecked(),
                         .replay_commands = replay_commands_->isChecked(),
                         .force_checksum_mismatch = checksum_mismatch_->isChecked(),
                         .response_delay_ms = static_cast<std::uint32_t>(response_delay_->value()),
                         .fail_next_requests = static_cast<std::uint32_t>(fail_next_->value())};
    if (!disconnect_chunk_->text().trimmed().isEmpty())
    {
        bool valid = false;
        const qulonglong value = disconnect_chunk_->text().toULongLong(&valid);
        if (!valid || value > std::numeric_limits<std::uint32_t>::max())
        {
            QMessageBox::warning(this, QStringLiteral("参数无效"),
                                 QStringLiteral("指定分块必须是 0～4294967295。"));
            return;
        }
        profile.disconnect_after_chunk = static_cast<std::uint32_t>(value);
    }
    auto result = runtime_.set_fault_profile(fault_device_->currentText().toStdString(), profile);
    if (!result)
        QMessageBox::critical(
            this, QStringLiteral("故障配置失败"),
            QString::fromStdString(result.error().business_code + ": " + result.error().message));
}

} // namespace paperbreak::uplink::simulator
