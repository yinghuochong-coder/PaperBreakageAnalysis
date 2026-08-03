#include "main_window.hpp"

#include "paperbreak/console/navigation_model.hpp"

#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QStackedWidget>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <memory>
#include <string_view>
#include <utility>

namespace paperbreak::console
{
namespace
{

template <typename T, typename... Arguments>
T* make_child(QWidget* parent, Arguments&&... arguments)
{
    auto child = std::make_unique<T>(std::forward<Arguments>(arguments)...);
    child->setParent(parent);
    T* result = child.get();
    static_cast<void>(child.release());
    return result;
}

template <typename T, typename... Arguments>
T* make_layout(QWidget* parent, Arguments&&... arguments)
{
    auto layout = std::make_unique<T>(parent, std::forward<Arguments>(arguments)...);
    T* result = layout.get();
    static_cast<void>(layout.release());
    return result;
}

QLabel* make_value_label(QWidget* parent, const QString& initial = QStringLiteral("—"))
{
    QLabel* label = make_child<QLabel>(parent, initial);
    label->setProperty("role", "statusValue");
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QWidget* make_status_item(QWidget* parent, const QString& title, QLabel*& value)
{
    QWidget* item = make_child<QWidget>(parent);
    auto* layout = make_layout<QVBoxLayout>(item);
    layout->setContentsMargins(10, 7, 10, 7);
    layout->setSpacing(2);
    QLabel* title_label = make_child<QLabel>(item, title);
    title_label->setProperty("role", "statusTitle");
    value = make_value_label(item);
    layout->addWidget(title_label);
    layout->addWidget(value);
    return item;
}

QGroupBox* make_camera_card(QWidget* parent, const QString& camera_id)
{
    QGroupBox* card = make_child<QGroupBox>(parent, camera_id);
    card->setProperty("role", "cameraCard");
    auto* layout = make_layout<QGridLayout>(card);
    layout->setContentsMargins(14, 18, 14, 14);
    const std::array<std::pair<QString, QString>, 4> rows{
        std::pair{QStringLiteral("连接状态"), QStringLiteral("待 M4-05 接入")},
        std::pair{QStringLiteral("实际帧率"), QStringLiteral("—")},
        std::pair{QStringLiteral("图像亮度"), QStringLiteral("—")},
        std::pair{QStringLiteral("最近一帧"), QStringLiteral("—")},
    };
    for (qsizetype row = 0; row < static_cast<qsizetype>(rows.size()); ++row)
    {
        QLabel* name = make_child<QLabel>(card, rows[static_cast<std::size_t>(row)].first);
        name->setProperty("role", "muted");
        QLabel* value = make_child<QLabel>(card, rows[static_cast<std::size_t>(row)].second);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(name, row, 0);
        layout->addWidget(value, row, 1);
    }
    return card;
}

QWidget* make_placeholder_page(QWidget* parent, const QString& title, const QString& message,
                               const QString& key)
{
    QWidget* page = make_child<QWidget>(parent);
    page->setObjectName(QStringLiteral("page-%1").arg(key));
    auto* layout = make_layout<QVBoxLayout>(page);
    layout->setContentsMargins(28, 24, 28, 24);
    QLabel* heading = make_child<QLabel>(page, title);
    heading->setProperty("role", "pageTitle");
    QLabel* description = make_child<QLabel>(page, message);
    description->setProperty("role", "placeholder");
    description->setAlignment(Qt::AlignCenter);
    description->setWordWrap(true);
    layout->addWidget(heading);
    layout->addWidget(description, 1);
    return page;
}

QWidget* make_preview_tile(QWidget* parent, const int index, QLabel*& image, QLabel*& overlay)
{
    QWidget* tile = make_child<QWidget>(parent);
    tile->setProperty("role", "previewTile");
    auto* layout = make_layout<QVBoxLayout>(tile);
    layout->setContentsMargins(6, 6, 6, 6);
    image = make_child<QLabel>(tile, QStringLiteral("等待 CAM%1 预览帧").arg(index + 1, 2, 10, QChar{'0'}));
    image->setAlignment(Qt::AlignCenter);
    image->setMinimumSize(240, 135);
    image->setScaledContents(false);
    image->setProperty("role", "previewImage");
    overlay = make_child<QLabel>(tile, QStringLiteral("CAM%1 · 无数据").arg(index + 1, 2, 10, QChar{'0'}));
    overlay->setProperty("role", "previewOverlay");
    layout->addWidget(image, 1);
    layout->addWidget(overlay);
    return tile;
}

QString stale_value(QString value, const bool stale)
{
    if (stale)
    {
        value += QStringLiteral("（已过期）");
    }
    return value;
}

QString service_text(const std::string& state)
{
    if (state == "running")
    {
        return QStringLiteral("运行中");
    }
    if (state == "degraded")
    {
        return QStringLiteral("降级运行");
    }
    if (state == "starting")
    {
        return QStringLiteral("启动中");
    }
    if (state == "stopping" || state == "stop-requested")
    {
        return QStringLiteral("停止中");
    }
    if (state == "faulted")
    {
        return QStringLiteral("故障");
    }
    return QString::fromStdString(state);
}

QString metric_text(const std::optional<double>& value, const QString& suffix, const bool stale,
                    const int precision)
{
    if (!value.has_value())
    {
        return stale ? QStringLiteral("不可用（已过期）") : QStringLiteral("不可用");
    }
    return stale_value(QStringLiteral("%1%2").arg(value.value(), 0, 'f', precision).arg(suffix),
                       stale);
}

QString connection_text(const ClientStateSnapshot& snapshot)
{
    switch (snapshot.connection.state)
    {
    case ipc::ClientConnectionState::stopped:
        return QStringLiteral("IPC 客户端已停止，远端数据不可用");
    case ipc::ClientConnectionState::connecting:
        return QStringLiteral("正在连接后台服务，远端数据尚不可用");
    case ipc::ClientConnectionState::retry_wait:
        return QStringLiteral("后台服务连接中断，当前显示内容均已标记过期");
    case ipc::ClientConnectionState::connected:
        if (snapshot.service_status_stale || snapshot.version_stale || snapshot.metrics_stale ||
            snapshot.alarms_stale)
        {
            return QStringLiteral("已连接后台服务，部分状态正在同步或暂不可用");
        }
        return QStringLiteral("后台服务已连接，状态数据为最新快照");
    }
    return QStringLiteral("后台服务状态未知");
}

QString placeholder_message(const ConsolePageId id)
{
    switch (id)
    {
    case ConsolePageId::preview:
        return QStringLiteral("导航骨架已建立。预览订阅与单路/四宫格显示将在 M4-03、M4-04 接入。");
    case ConsolePageId::camera_configuration:
        return QStringLiteral("导航骨架已建立。相机查询、配置与实际值回显将在 M4-05 接入。");
    case ConsolePageId::algorithm_configuration:
        return QStringLiteral("导航骨架已建立。正式算法配置将在 M6 接入。");
    case ConsolePageId::event_configuration:
        return QStringLiteral("导航骨架已建立。事件链与事件配置将在 M5 接入。");
    case ConsolePageId::storage_configuration:
        return QStringLiteral("导航骨架已建立。事件存储与 NVMe 配置将在 M5、M7 接入。");
    case ConsolePageId::uplink_configuration:
        return QStringLiteral("导航骨架已建立。上位机连接与上传配置将在 M8 接入。");
    case ConsolePageId::device_status:
    case ConsolePageId::alarms:
    case ConsolePageId::logs:
    case ConsolePageId::maintenance:
        return QStringLiteral("导航骨架已建立。完整状态、报警、日志与诊断功能将在 M4-06 接入。");
    case ConsolePageId::events:
        return QStringLiteral("导航骨架已建立。事件查询与复核将在 M5 接入。");
    case ConsolePageId::overview:
        break;
    }
    return QStringLiteral("页面骨架已建立。");
}

} // namespace

MainWindow::MainWindow(std::function<void(bool)> preview_pause_changed, QWidget* parent)
    : QMainWindow(parent), preview_pause_changed_(std::move(preview_pause_changed))
{
    setObjectName(QStringLiteral("main-window"));
    setWindowTitle(QStringLiteral("PaperBreakEdge 断纸分析控制台"));
    resize(1280, 800);
    setMinimumSize(1040, 680);

    QWidget* central = make_child<QWidget>(this);
    setCentralWidget(central);
    auto* root = make_layout<QVBoxLayout>(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    QWidget* header = make_child<QWidget>(central);
    header->setObjectName(QStringLiteral("status-header"));
    auto* header_layout = make_layout<QVBoxLayout>(header);
    header_layout->setContentsMargins(18, 12, 18, 10);
    header_layout->setSpacing(8);
    QLabel* product = make_child<QLabel>(header, QStringLiteral("纸机断纸分析边缘控制台"));
    product->setProperty("role", "productTitle");
    header_layout->addWidget(product);
    QWidget* status_container = make_child<QWidget>(header);
    auto* status_grid = make_layout<QGridLayout>(status_container);
    status_grid->setContentsMargins(0, 0, 0, 0);
    status_grid->setHorizontalSpacing(8);
    status_grid->addWidget(make_status_item(header, QStringLiteral("服务"), service_value_), 0, 0);
    status_grid->addWidget(make_status_item(header, QStringLiteral("当前时间"), clock_value_), 0,
                           1);
    status_grid->addWidget(make_status_item(header, QStringLiteral("工控机编号"), machine_value_),
                           0, 2);
    status_grid->addWidget(make_status_item(header, QStringLiteral("上位机"), uplink_value_), 0, 3);
    status_grid->addWidget(
        make_status_item(header, QStringLiteral("正常相机"), camera_count_value_), 0, 4);
    status_grid->addWidget(make_status_item(header, QStringLiteral("当前报警"), alarm_count_value_),
                           0, 5);
    status_grid->addWidget(make_status_item(header, QStringLiteral("事件盘剩余"), disk_value_), 0,
                           6);
    status_grid->addWidget(make_status_item(header, QStringLiteral("软件版本"), version_value_), 0,
                           7);
    header_layout->addWidget(status_container);
    root->addWidget(header);

    connection_banner_ = make_child<QLabel>(central, QStringLiteral("正在初始化客户端状态"));
    connection_banner_->setObjectName(QStringLiteral("connection-banner"));
    connection_banner_->setAlignment(Qt::AlignCenter);
    root->addWidget(connection_banner_);

    QWidget* body = make_child<QWidget>(central);
    auto* body_layout = make_layout<QHBoxLayout>(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(0);

    navigation_ = make_child<QListWidget>(body);
    navigation_->setObjectName(QStringLiteral("navigation"));
    navigation_->setFixedWidth(190);
    navigation_->setSpacing(2);
    navigation_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pages_ = make_child<QStackedWidget>(body);
    pages_->setObjectName(QStringLiteral("page-stack"));

    QWidget* overview = make_child<QWidget>(pages_);
    overview->setObjectName(QStringLiteral("page-overview"));
    auto* overview_layout = make_layout<QVBoxLayout>(overview);
    overview_layout->setContentsMargins(24, 20, 24, 20);
    overview_layout->setSpacing(14);
    QLabel* overview_title = make_child<QLabel>(overview, QStringLiteral("运行总览"));
    overview_title->setProperty("role", "pageTitle");
    overview_sync_value_ = make_child<QLabel>(overview, QStringLiteral("等待后台服务状态"));
    overview_sync_value_->setProperty("role", "muted");
    overview_layout->addWidget(overview_title);
    overview_layout->addWidget(overview_sync_value_);

    QWidget* cameras = make_child<QWidget>(overview);
    auto* camera_grid = make_layout<QGridLayout>(cameras);
    camera_grid->setContentsMargins(0, 0, 0, 0);
    camera_grid->setSpacing(12);
    for (int index = 0; index < 4; ++index)
    {
        camera_grid->addWidget(
            make_camera_card(cameras, QStringLiteral("CAM%1").arg(index + 1, 2, 10, QChar{'0'})),
            index / 2, index % 2);
    }
    overview_layout->addWidget(cameras);

    QWidget* summary = make_child<QWidget>(overview);
    auto* summary_layout = make_layout<QHBoxLayout>(summary);
    summary_layout->setContentsMargins(0, 0, 0, 0);
    summary_layout->setSpacing(12);

    QGroupBox* resources = make_child<QGroupBox>(summary, QStringLiteral("系统资源"));
    auto* resources_layout = make_layout<QGridLayout>(resources);
    resources_layout->addWidget(make_child<QLabel>(resources, QStringLiteral("CPU")), 0, 0);
    cpu_value_ = make_value_label(resources, QStringLiteral("不可用"));
    resources_layout->addWidget(cpu_value_, 0, 1);
    resources_layout->addWidget(make_child<QLabel>(resources, QStringLiteral("内存")), 1, 0);
    memory_value_ = make_value_label(resources, QStringLiteral("不可用"));
    resources_layout->addWidget(memory_value_, 1, 1);
    resources_layout->addWidget(make_child<QLabel>(resources, QStringLiteral("事件盘")), 2, 0);
    overview_disk_value_ = make_value_label(resources, QStringLiteral("不可用"));
    resources_layout->addWidget(overview_disk_value_, 2, 1);

    QGroupBox* detection = make_child<QGroupBox>(summary, QStringLiteral("检测状态"));
    auto* detection_layout = make_layout<QVBoxLayout>(detection);
    detection_layout->addWidget(
        make_child<QLabel>(detection, QStringLiteral("正式检测器：待 M6 接入")));
    detection_layout->addWidget(make_child<QLabel>(detection, QStringLiteral("当前候选事件：—")));

    QGroupBox* upload = make_child<QGroupBox>(summary, QStringLiteral("上位机与上传"));
    auto* upload_layout = make_layout<QVBoxLayout>(upload);
    upload_layout->addWidget(make_child<QLabel>(upload, QStringLiteral("上位机连接：待 M8 接入")));
    upload_layout->addWidget(
        make_child<QLabel>(upload, QStringLiteral("待上传事件：待 M5/M8 接入")));

    summary_layout->addWidget(resources, 1);
    summary_layout->addWidget(detection, 1);
    summary_layout->addWidget(upload, 1);
    overview_layout->addWidget(summary);

    QGroupBox* recent_alarms = make_child<QGroupBox>(overview, QStringLiteral("最近活动报警"));
    auto* recent_layout = make_layout<QVBoxLayout>(recent_alarms);
    recent_alarms_value_ = make_value_label(recent_alarms, QStringLiteral("等待报警状态"));
    recent_alarms_value_->setWordWrap(true);
    recent_layout->addWidget(recent_alarms_value_);
    overview_layout->addWidget(recent_alarms);
    overview_layout->addStretch(1);

    const auto descriptors = console_pages();
    for (std::size_t index = 0; index < descriptors.size(); ++index)
    {
        const auto& descriptor = descriptors[index];
        navigation_->addItem(QString::fromUtf8(descriptor.title.data(),
                                               static_cast<qsizetype>(descriptor.title.size())));
        if (descriptor.id == ConsolePageId::overview)
        {
            pages_->addWidget(overview);
            continue;
        }
        const QString title = QString::fromUtf8(descriptor.title.data(),
                                                static_cast<qsizetype>(descriptor.title.size()));
        const QString key =
            QString::fromUtf8(descriptor.key.data(), static_cast<qsizetype>(descriptor.key.size()));
        if (descriptor.id != ConsolePageId::preview)
        {
            pages_->addWidget(make_placeholder_page(pages_, title, placeholder_message(descriptor.id), key));
            continue;
        }
        QWidget* preview_page = make_child<QWidget>(pages_);
        preview_page->setObjectName(QStringLiteral("page-preview"));
        auto* preview_layout = make_layout<QVBoxLayout>(preview_page);
        preview_layout->setContentsMargins(24, 20, 24, 20);
        QLabel* heading = make_child<QLabel>(preview_page, QStringLiteral("实时预览"));
        heading->setProperty("role", "pageTitle");
        preview_layout->addWidget(heading);
        QWidget* controls = make_child<QWidget>(preview_page);
        auto* controls_layout = make_layout<QHBoxLayout>(controls);
        auto* layout_choice = make_child<QComboBox>(controls);
        layout_choice->addItems({QStringLiteral("四宫格"), QStringLiteral("CAM01"),
                                 QStringLiteral("CAM02"), QStringLiteral("CAM03"),
                                 QStringLiteral("CAM04")});
        auto* rate_choice = make_child<QComboBox>(controls);
        rate_choice->addItems({QStringLiteral("2 fps"), QStringLiteral("3 fps"), QStringLiteral("5 fps")});
        auto* resolution_choice = make_child<QComboBox>(controls);
        resolution_choice->addItems({QStringLiteral("自适应分辨率"), QStringLiteral("1280×720"), QStringLiteral("640×360")});
        preview_pause_button_ = make_child<QPushButton>(controls, QStringLiteral("暂停显示"));
        auto* one_to_one = make_child<QPushButton>(controls, QStringLiteral("1:1"));
        auto* adaptive = make_child<QPushButton>(controls, QStringLiteral("自适应"));
        auto* full_screen = make_child<QPushButton>(controls, QStringLiteral("全屏"));
        auto* capture = make_child<QPushButton>(controls, QStringLiteral("抓图"));
        controls_layout->addWidget(layout_choice);
        controls_layout->addWidget(rate_choice);
        controls_layout->addWidget(resolution_choice);
        controls_layout->addWidget(preview_pause_button_);
        controls_layout->addWidget(one_to_one);
        controls_layout->addWidget(adaptive);
        controls_layout->addWidget(full_screen);
        controls_layout->addWidget(capture);
        controls_layout->addStretch(1);
        preview_layout->addWidget(controls);
        preview_status_ = make_child<QLabel>(preview_page, QStringLiteral("预览正在连接后台服务"));
        preview_status_->setProperty("role", "muted");
        preview_layout->addWidget(preview_status_);
        QWidget* grid = make_child<QWidget>(preview_page);
        auto* grid_layout = make_layout<QGridLayout>(grid);
        grid_layout->setSpacing(8);
        std::array<QWidget*, 4U> preview_tiles{};
        for (int camera = 0; camera < 4; ++camera)
        {
            preview_tiles[camera] = make_preview_tile(grid, camera, preview_images_[camera], preview_overlays_[camera]);
            grid_layout->addWidget(preview_tiles[camera], camera / 2, camera % 2);
        }
        preview_layout->addWidget(grid, 1);
        QObject::connect(preview_pause_button_, &QPushButton::clicked, this, [this] {
            preview_paused_ = !preview_paused_;
            preview_pause_button_->setText(preview_paused_ ? QStringLiteral("恢复显示") : QStringLiteral("暂停显示"));
            if (preview_pause_changed_)
                preview_pause_changed_(preview_paused_);
        });
        QObject::connect(layout_choice, &QComboBox::currentIndexChanged, this, [preview_tiles](const int selection) {
            for (std::size_t tile = 0; tile < preview_tiles.size(); ++tile)
                preview_tiles[tile]->setVisible(selection == 0 || static_cast<int>(tile + 1U) == selection);
        });
        QObject::connect(one_to_one, &QPushButton::clicked, this, [this] {
            for (QLabel* image : preview_images_)
                image->setScaledContents(false);
        });
        QObject::connect(adaptive, &QPushButton::clicked, this, [this] {
            for (QLabel* image : preview_images_)
                image->setScaledContents(true);
        });
        QObject::connect(full_screen, &QPushButton::clicked, this, [this] {
            isFullScreen() ? showNormal() : showFullScreen();
        });
        QObject::connect(capture, &QPushButton::clicked, this, [this] {
            const auto found = std::find_if(preview_images_.begin(), preview_images_.end(), [](const QLabel* label) { return !label->pixmap().isNull(); });
            if (found == preview_images_.end())
                return;
            const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存预览抓图"), QStringLiteral("preview.jpg"), QStringLiteral("JPEG 图像 (*.jpg)"));
            if (!path.isEmpty())
                static_cast<void>((*found)->pixmap().save(path, "JPG"));
        });
        pages_->addWidget(preview_page);
    }

    QObject::connect(navigation_, &QListWidget::currentRowChanged, pages_,
                     &QStackedWidget::setCurrentIndex);
    navigation_->setCurrentRow(static_cast<int>(default_console_page_index()));
    body_layout->addWidget(navigation_);
    body_layout->addWidget(pages_, 1);
    root->addWidget(body, 1);

    setStyleSheet(QStringLiteral(R"(
        QMainWindow { background: #f2f5f8; color: #172033; }
        QWidget#status-header { background: #17324d; color: white; }
        QLabel[role="productTitle"] { font-size: 20px; font-weight: 600; }
        QLabel[role="statusTitle"] { color: #b9ccdc; font-size: 11px; }
        QLabel[role="statusValue"] { font-weight: 600; }
        QLabel#connection-banner { background: #fff4cf; color: #604b00; padding: 7px; }
        QListWidget#navigation { background: #223f59; color: #dce8f1; border: 0; padding: 10px 6px; font-size: 14px; }
        QListWidget#navigation::item { padding: 11px 14px; border-radius: 4px; }
        QListWidget#navigation::item:selected { background: #2c78b8; color: white; }
        QStackedWidget#page-stack { background: #f2f5f8; }
        QLabel[role="pageTitle"] { font-size: 22px; font-weight: 600; color: #17324d; }
        QLabel[role="muted"] { color: #687789; }
        QLabel[role="placeholder"] { color: #687789; font-size: 16px; background: white; border: 1px dashed #b8c5d1; border-radius: 8px; padding: 28px; }
        QGroupBox { background: white; border: 1px solid #d9e1e8; border-radius: 6px; margin-top: 8px; padding-top: 8px; font-weight: 600; }
        QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 5px; color: #34516a; }
        QGroupBox[role="cameraCard"] { min-height: 112px; }
        QWidget[role="previewTile"] { background: #132331; border-radius: 5px; }
        QLabel[role="previewImage"] { color: #b9ccdc; background: #0a1118; }
        QLabel[role="previewOverlay"] { color: #dce8f1; font-size: 11px; }
    )"));

    update_clock();
}

void MainWindow::apply_preview_snapshot(const PreviewSnapshot& snapshot)
{
    if (!preview_status_)
        return;
    if (snapshot.paused)
        preview_status_->setText(QStringLiteral("显示已暂停：后台采集和预览服务仍继续运行"));
    else if (snapshot.connection.state != ipc::ClientConnectionState::connected)
        preview_status_->setText(QStringLiteral("预览服务未连接；最后画面不代表实时状态"));
    else if (!snapshot.subscribed)
        preview_status_->setText(QStringLiteral("正在订阅预览帧"));
    else
        preview_status_->setText(QStringLiteral("已订阅预览：服务端按低帧率发送最新画面"));
    for (std::size_t index = 0; index < snapshot.images.size(); ++index)
    {
        if (!snapshot.images[index])
            continue;
        const auto& frame = snapshot.images[index].value();
        preview_images_[index]->setPixmap(QPixmap::fromImage(frame.image));
        preview_overlays_[index]->setText(QStringLiteral("%1 · 帧 %2 · %3 fps · %4")
            .arg(QString::fromStdString(frame.camera_id)).arg(frame.frame_number)
            .arg(frame.actual_fps.value_or(0.0), 0, 'f', 1)
            .arg(QString::fromStdString(frame.detection_result.empty() ? frame.camera_status : frame.detection_result)));
    }
}

void MainWindow::apply_snapshot(const ClientStateSnapshot& snapshot)
{
    connection_banner_->setText(connection_text(snapshot));

    if (snapshot.service_status.has_value())
    {
        service_value_->setText(stale_value(service_text(snapshot.service_status->service_state),
                                            snapshot.service_status_stale));
        machine_value_->setText(
            stale_value(QString::fromStdString(snapshot.service_status->machine_id),
                        snapshot.service_status_stale));
    }
    else
    {
        service_value_->setText(QStringLiteral("不可用"));
        machine_value_->setText(QStringLiteral("不可用"));
    }

    uplink_value_->setText(QStringLiteral("待 M8 接入"));
    camera_count_value_->setText(QStringLiteral("待 M4-05 接入"));

    if (snapshot.alarms.has_value())
    {
        QString count = QString::number(snapshot.alarms->active_count);
        if (snapshot.alarms->count_truncated)
        {
            count += QChar{'+'};
        }
        alarm_count_value_->setText(stale_value(count, snapshot.alarms_stale));
        if (snapshot.alarms->recent.empty())
        {
            recent_alarms_value_->setText(
                stale_value(QStringLiteral("当前无活动报警"), snapshot.alarms_stale));
        }
        else
        {
            QStringList lines;
            for (const auto& alarm : snapshot.alarms->recent)
            {
                lines.push_back(QStringLiteral("[%1] %2 · %3 · %4")
                                    .arg(QString::fromStdString(alarm.severity),
                                         QString::fromStdString(alarm.source),
                                         QString::fromStdString(alarm.message),
                                         QString::fromStdString(alarm.last_occurred_at)));
            }
            recent_alarms_value_->setText(
                stale_value(lines.join(QChar{'\n'}), snapshot.alarms_stale));
        }
    }
    else
    {
        alarm_count_value_->setText(QStringLiteral("不可用"));
        recent_alarms_value_->setText(QStringLiteral("报警状态不可用"));
    }

    const bool metrics_stale = snapshot.metrics_stale;
    const auto cpu = snapshot.metrics.has_value() ? snapshot.metrics->process_cpu_percent
                                                  : std::optional<double>{};
    const auto memory = snapshot.metrics.has_value() ? snapshot.metrics->system_memory_used_percent
                                                     : std::optional<double>{};
    const auto disk = snapshot.metrics.has_value() ? snapshot.metrics->event_disk_free_gib
                                                   : std::optional<double>{};
    cpu_value_->setText(metric_text(cpu, QStringLiteral("%"), metrics_stale, 1));
    memory_value_->setText(metric_text(memory, QStringLiteral("%"), metrics_stale, 1));
    const QString disk_text = metric_text(disk, QStringLiteral(" GiB"), metrics_stale, 1);
    disk_value_->setText(disk_text);
    overview_disk_value_->setText(disk_text);

    if (snapshot.version.has_value())
    {
        version_value_->setText(stale_value(
            QString::fromStdString(snapshot.version->application_version), snapshot.version_stale));
    }
    else
    {
        version_value_->setText(QStringLiteral("不可用"));
    }

    if (snapshot.connection.state != ipc::ClientConnectionState::connected)
    {
        overview_sync_value_->setText(QStringLiteral("后台服务连接中断；保留值均已标记过期"));
    }
    else if (snapshot.service_status_stale || snapshot.metrics_stale || snapshot.alarms_stale)
    {
        overview_sync_value_->setText(QStringLiteral("部分状态正在同步或暂不可用"));
    }
    else
    {
        overview_sync_value_->setText(QStringLiteral("状态快照已同步"));
    }
}

void MainWindow::update_clock()
{
    clock_value_->setText(
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
}

std::size_t MainWindow::page_count() const noexcept
{
    return static_cast<std::size_t>(std::max(0, pages_->count()));
}

int MainWindow::current_page_index() const noexcept
{
    return pages_->currentIndex();
}

bool MainWindow::select_page(const std::size_t index) noexcept
{
    if (index >= page_count())
    {
        return false;
    }
    navigation_->setCurrentRow(static_cast<int>(index));
    return pages_->currentIndex() == static_cast<int>(index);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    hide();
    event->ignore();
}

} // namespace paperbreak::console
