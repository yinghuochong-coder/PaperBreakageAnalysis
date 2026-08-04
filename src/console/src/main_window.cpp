#include "main_window.hpp"

#include "paperbreak/console/navigation_model.hpp"

#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimeZone>
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

constexpr int table_header_minimum_height = 32;
constexpr int table_header_vertical_margin = 12;
constexpr auto local_date_time_format = "yyyy-MM-dd HH:mm:ss.zzz ttt";
constexpr auto local_clock_format = "yyyy-MM-dd HH:mm:ss ttt";

std::optional<QDateTime> parse_external_date_time(const QString& text)
{
    QDateTime parsed = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!parsed.isValid())
        parsed = QDateTime::fromString(text, Qt::ISODate);
    if (!parsed.isValid() || parsed.timeSpec() == Qt::LocalTime)
        return std::nullopt;
    return parsed;
}

QString local_date_time_text(const std::string_view timestamp)
{
    const QString original =
        QString::fromUtf8(timestamp.data(), static_cast<qsizetype>(timestamp.size()));
    const auto parsed = parse_external_date_time(original);
    return parsed ? parsed->toLocalTime().toString(QString::fromLatin1(local_date_time_format))
                  : original;
}

QString local_epoch_milliseconds_text(const std::string_view timestamp)
{
    const QString original =
        QString::fromUtf8(timestamp.data(), static_cast<qsizetype>(timestamp.size()));
    bool converted{};
    const qint64 milliseconds = original.toLongLong(&converted);
    if (!converted)
        return original;
    return QDateTime::fromMSecsSinceEpoch(milliseconds, QTimeZone::utc())
        .toLocalTime()
        .toString(QString::fromLatin1(local_date_time_format));
}

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

void configure_table(QTableWidget* table, const QStringList& headers)
{
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(true);
    QHeaderView* horizontal_header = table->horizontalHeader();
    horizontal_header->setMinimumHeight(
        std::max(table_header_minimum_height,
                 horizontal_header->fontMetrics().height() + table_header_vertical_margin));
    horizontal_header->setStretchLastSection(true);
    table->verticalHeader()->setVisible(false);
}

bool has_readable_table_header(const QTableWidget* table)
{
    if (!table || !table->horizontalHeader())
        return false;
    const QHeaderView* header = table->horizontalHeader();
    const int required_height = std::max(
        table_header_minimum_height, header->fontMetrics().height() + table_header_vertical_margin);
    return header->minimumHeight() >= required_height && header->height() >= required_height;
}

void set_table_item(QTableWidget* table, const int row, const int column, const QString& text,
                    const QVariant& data = {})
{
    auto item = std::make_unique<QTableWidgetItem>(text);
    if (data.isValid())
        item->setData(Qt::UserRole, data);
    table->setItem(row, column, item.release());
}

QWidget* make_preview_tile(QWidget* parent, const int index, QLabel*& image, QLabel*& overlay)
{
    QWidget* tile = make_child<QWidget>(parent);
    tile->setProperty("role", "previewTile");
    auto* layout = make_layout<QVBoxLayout>(tile);
    layout->setContentsMargins(6, 6, 6, 6);
    image = make_child<QLabel>(
        tile, QStringLiteral("等待 CAM%1 预览帧").arg(index + 1, 2, 10, QChar{'0'}));
    image->setAlignment(Qt::AlignCenter);
    image->setMinimumSize(240, 135);
    image->setScaledContents(false);
    image->setProperty("role", "previewImage");
    overlay = make_child<QLabel>(
        tile, QStringLiteral("CAM%1 · 无数据").arg(index + 1, 2, 10, QChar{'0'}));
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

MainWindow::MainWindow(std::function<void(bool)> preview_pause_changed,
                       CameraUiActions camera_actions, ThemeUiActions theme_actions,
                       OperationsUiActions operations_actions, QWidget* parent)
    : QMainWindow(parent), preview_pause_changed_(std::move(preview_pause_changed)),
      camera_actions_(std::move(camera_actions)), theme_actions_(std::move(theme_actions)),
      operations_actions_(std::move(operations_actions))
{
    setObjectName(QStringLiteral("main-window"));
    setWindowTitle(QStringLiteral("PaperBreakEdge 断纸分析控制台"));
    resize(1280, 800);
    setMinimumSize(1040, 680);

    QWidget* central = make_child<QWidget>(this);
    central->setObjectName(QStringLiteral("app-central"));
    setCentralWidget(central);
    auto* root = make_layout<QVBoxLayout>(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    QWidget* header = make_child<QWidget>(central);
    header->setObjectName(QStringLiteral("status-header"));
    auto* header_layout = make_layout<QVBoxLayout>(header);
    header_layout->setContentsMargins(18, 12, 18, 10);
    header_layout->setSpacing(8);
    QWidget* product_row = make_child<QWidget>(header);
    auto* product_layout = make_layout<QHBoxLayout>(product_row);
    product_layout->setContentsMargins(0, 0, 0, 0);
    QLabel* product = make_child<QLabel>(product_row, QStringLiteral("纸机断纸分析边缘控制台"));
    product->setProperty("role", "productTitle");
    theme_selector_ = make_child<QComboBox>(product_row);
    theme_selector_->setObjectName(QStringLiteral("theme-selector"));
    theme_selector_->addItem(QStringLiteral("跟随系统"), static_cast<int>(ThemeMode::system));
    theme_selector_->addItem(QStringLiteral("浅色"), static_cast<int>(ThemeMode::light));
    theme_selector_->addItem(QStringLiteral("暗黑"), static_cast<int>(ThemeMode::dark));
    const int initial_theme =
        theme_selector_->findData(static_cast<int>(theme_actions_.initial_mode));
    theme_selector_->setCurrentIndex(initial_theme >= 0 ? initial_theme : 0);
    QObject::connect(theme_selector_, &QComboBox::currentIndexChanged, this,
                     [this](const int index) {
                         if (index >= 0 && theme_actions_.set_mode)
                             theme_actions_.set_mode(
                                 static_cast<ThemeMode>(theme_selector_->itemData(index).toInt()));
                     });
    product_layout->addWidget(product);
    product_layout->addStretch(1);
    product_layout->addWidget(make_child<QLabel>(product_row, QStringLiteral("主题")));
    product_layout->addWidget(theme_selector_);
    header_layout->addWidget(product_row);
    QWidget* status_container = make_child<QWidget>(header);
    auto* status_grid = make_layout<QGridLayout>(status_container);
    status_grid->setContentsMargins(0, 0, 0, 0);
    status_grid->setHorizontalSpacing(8);
    status_grid->addWidget(make_status_item(header, QStringLiteral("服务"), service_value_), 0, 0);
    status_grid->addWidget(make_status_item(header, QStringLiteral("当前时间"), clock_value_), 0,
                           1);
    clock_value_->setObjectName(QStringLiteral("current-local-time"));
    status_grid->addWidget(make_status_item(header, QStringLiteral("工控机编号"), machine_value_),
                           0, 2);
    status_grid->addWidget(make_status_item(header, QStringLiteral("上位机"), uplink_value_), 0, 3);
    status_grid->addWidget(
        make_status_item(header, QStringLiteral("正常/已配置相机"), camera_count_value_), 0, 4);
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
    body->setObjectName(QStringLiteral("app-body"));
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
    recent_alarms_value_->setObjectName(QStringLiteral("recent-alarms"));
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
        if (descriptor.id == ConsolePageId::camera_configuration)
        {
            QWidget* page = make_child<QWidget>(pages_);
            page->setObjectName(QStringLiteral("page-camera-configuration"));
            auto* layout = make_layout<QVBoxLayout>(page);
            layout->setContentsMargins(24, 20, 24, 20);
            auto* heading = make_child<QLabel>(page, QStringLiteral("相机配置与实际值"));
            heading->setProperty("role", "pageTitle");
            layout->addWidget(heading);

            auto* scroll = make_child<QScrollArea>(page);
            scroll->setWidgetResizable(true);
            QWidget* content = make_child<QWidget>(scroll);
            auto* content_layout = make_layout<QVBoxLayout>(content);
            content_layout->setContentsMargins(2, 2, 8, 2);
            content_layout->setSpacing(12);

            auto* configured_group = make_child<QGroupBox>(content, QStringLiteral("已配置槽位"));
            auto* configured_layout = make_layout<QVBoxLayout>(configured_group);
            camera_selector_ = make_child<QComboBox>(configured_group);
            camera_selector_->setObjectName(QStringLiteral("camera-selector"));
            camera_configuration_value_ =
                make_child<QLabel>(configured_group, QStringLiteral("正在从后台服务读取相机配置"));
            camera_configuration_value_->setWordWrap(true);
            camera_configuration_value_->setProperty("role", "muted");
            configured_layout->addWidget(camera_selector_);
            configured_layout->addWidget(camera_configuration_value_);
            content_layout->addWidget(configured_group);

            auto* discovered_group = make_child<QGroupBox>(content, QStringLiteral("网络发现设备"));
            auto* discovered_layout = make_layout<QVBoxLayout>(discovered_group);
            discovered_devices_ = make_child<QListWidget>(discovered_group);
            discovered_devices_->setObjectName(QStringLiteral("discovered-devices"));
            discovered_devices_->setMinimumHeight(92);
            discovered_layout->addWidget(discovered_devices_);
            auto* binding = make_child<QWidget>(discovered_group);
            auto* binding_layout = make_layout<QHBoxLayout>(binding);
            binding_layout->setContentsMargins(0, 0, 0, 0);
            camera_bind_slot_ = make_child<QComboBox>(binding);
            camera_bind_slot_->setObjectName(QStringLiteral("camera-bind-slot"));
            camera_bind_location_ = make_child<QLineEdit>(binding);
            camera_bind_location_->setObjectName(QStringLiteral("camera-bind-location"));
            camera_bind_location_->setPlaceholderText(QStringLiteral("安装位置（必填）"));
            camera_bind_location_->setMaxLength(128);
            camera_bind_button_ = make_child<QPushButton>(binding, QStringLiteral("绑定到槽位"));
            camera_bind_button_->setObjectName(QStringLiteral("camera-bind"));
            binding_layout->addWidget(make_child<QLabel>(binding, QStringLiteral("逻辑槽位")));
            binding_layout->addWidget(camera_bind_slot_);
            binding_layout->addWidget(camera_bind_location_, 1);
            binding_layout->addWidget(camera_bind_button_);
            discovered_layout->addWidget(binding);
            content_layout->addWidget(discovered_group);

            camera_editor_ = make_child<QWidget>(content);
            camera_editor_->setObjectName(QStringLiteral("camera-editor"));
            auto* form = make_layout<QFormLayout>(camera_editor_);
            form->setContentsMargins(12, 12, 12, 12);
            camera_exposure_ = make_child<QDoubleSpinBox>(camera_editor_);
            camera_exposure_->setRange(1.0, 10000000.0);
            camera_exposure_->setDecimals(1);
            camera_gain_ = make_child<QDoubleSpinBox>(camera_editor_);
            camera_gain_->setRange(-24.0, 48.0);
            camera_gain_->setDecimals(2);
            camera_fps_ = make_child<QDoubleSpinBox>(camera_editor_);
            camera_fps_->setRange(0.1, 1000.0);
            camera_fps_->setDecimals(3);
            camera_roi_width_ = make_child<QSpinBox>(camera_editor_);
            camera_roi_width_->setRange(1, 16384);
            camera_roi_height_ = make_child<QSpinBox>(camera_editor_);
            camera_roi_height_->setRange(1, 16384);
            camera_roi_x_ = make_child<QSpinBox>(camera_editor_);
            camera_roi_x_->setRange(0, 16383);
            camera_roi_y_ = make_child<QSpinBox>(camera_editor_);
            camera_roi_y_->setRange(0, 16383);
            camera_pixel_format_ = make_child<QComboBox>(camera_editor_);
            camera_pixel_format_->addItems({QStringLiteral("Mono8"), QStringLiteral("Mono10"),
                                            QStringLiteral("Mono12"), QStringLiteral("BayerRG8")});
            camera_trigger_mode_ = make_child<QComboBox>(camera_editor_);
            camera_trigger_mode_->addItems({QStringLiteral("Continuous"),
                                            QStringLiteral("Hardware"),
                                            QStringLiteral("Software")});
            camera_trigger_source_ = make_child<QLineEdit>(camera_editor_);
            camera_trigger_delay_ = make_child<QSpinBox>(camera_editor_);
            camera_trigger_delay_->setRange(0, 60000000);
            camera_packet_size_ = make_child<QSpinBox>(camera_editor_);
            camera_packet_size_->setRange(576, 9000);
            camera_packet_delay_ = make_child<QSpinBox>(camera_editor_);
            camera_packet_delay_->setRange(0, 1000000000);
            form->addRow(QStringLiteral("曝光 (us)"), camera_exposure_);
            form->addRow(QStringLiteral("增益 (dB)"), camera_gain_);
            form->addRow(QStringLiteral("帧率 (fps)"), camera_fps_);
            form->addRow(QStringLiteral("ROI 宽"), camera_roi_width_);
            form->addRow(QStringLiteral("ROI 高"), camera_roi_height_);
            form->addRow(QStringLiteral("ROI X"), camera_roi_x_);
            form->addRow(QStringLiteral("ROI Y"), camera_roi_y_);
            form->addRow(QStringLiteral("像素格式"), camera_pixel_format_);
            form->addRow(QStringLiteral("触发模式"), camera_trigger_mode_);
            form->addRow(QStringLiteral("触发源"), camera_trigger_source_);
            form->addRow(QStringLiteral("触发延迟 (us)"), camera_trigger_delay_);
            form->addRow(QStringLiteral("包大小 (bytes)"), camera_packet_size_);
            form->addRow(QStringLiteral("包间延迟 (ns)"), camera_packet_delay_);
            content_layout->addWidget(camera_editor_);

            camera_control_actions_ = make_child<QWidget>(content);
            auto* action_layout = make_layout<QHBoxLayout>(camera_control_actions_);
            action_layout->setContentsMargins(0, 0, 0, 0);
            const auto add_action = [&](const QString& text, const std::string& command,
                                        const bool confirm) {
                auto* button = make_child<QPushButton>(camera_control_actions_, text);
                button->setObjectName(
                    QStringLiteral("camera-%1").arg(QString::fromStdString(command)));
                QObject::connect(button, &QPushButton::clicked, this, [this, command, confirm] {
                    run_camera_control(command, confirm);
                });
                action_layout->addWidget(button);
            };
            add_action(QStringLiteral("连接"), "camera.connect", false);
            camera_read_parameters_button_ =
                make_child<QPushButton>(camera_control_actions_, QStringLiteral("读取当前参数"));
            camera_read_parameters_button_->setObjectName(QStringLiteral("camera-read-parameters"));
            QObject::connect(camera_read_parameters_button_, &QPushButton::clicked, this, [this] {
                if (!camera_actions_.control || !camera_selector_ ||
                    camera_selector_->currentIndex() < 0)
                    return;
                const auto result = camera_actions_.control(
                    "camera.getConfig", camera_selector_->currentText().toStdString());
                if (result)
                    camera_parameter_read_pending_ = true;
                show_camera_result(result);
            });
            action_layout->addWidget(camera_read_parameters_button_);
            add_action(QStringLiteral("断开"), "camera.disconnect", true);
            add_action(QStringLiteral("开始采集"), "camera.start", true);
            add_action(QStringLiteral("停止采集"), "camera.stop", true);
            add_action(QStringLiteral("抓取快照"), "camera.captureSnapshot", false);
            add_action(QStringLiteral("软件触发"), "camera.softwareTrigger", false);
            auto* save =
                make_child<QPushButton>(camera_control_actions_, QStringLiteral("保存并下发"));
            save->setObjectName(QStringLiteral("camera-update-config"));
            QObject::connect(save, &QPushButton::clicked, this, [this] {
                if (!camera_actions_.update_config || camera_selector_->currentIndex() < 0)
                    return;
                if (QMessageBox::question(
                        this, QStringLiteral("确认相机配置"),
                        QStringLiteral("保存并下发参数可能短暂影响采集，是否继续？")) !=
                    QMessageBox::Yes)
                    return;
                const auto index = static_cast<std::size_t>(camera_selector_->currentIndex());
                if (index >= camera_snapshot_.cameras.size())
                    return;
                CameraParameterValue value{
                    .exposure_us = camera_exposure_->value(),
                    .gain_db = camera_gain_->value(),
                    .frame_rate = camera_fps_->value(),
                    .roi = CameraRoiValue{static_cast<std::uint32_t>(camera_roi_width_->value()),
                                          static_cast<std::uint32_t>(camera_roi_height_->value()),
                                          static_cast<std::uint32_t>(camera_roi_x_->value()),
                                          static_cast<std::uint32_t>(camera_roi_y_->value())},
                    .pixel_format = camera_pixel_format_->currentText().toStdString(),
                    .trigger_mode = camera_trigger_mode_->currentText().toStdString(),
                    .trigger_source = camera_trigger_source_->text().toStdString(),
                    .trigger_delay_us = static_cast<std::uint32_t>(camera_trigger_delay_->value()),
                    .packet_size_bytes = static_cast<std::uint32_t>(camera_packet_size_->value()),
                    .inter_packet_delay_ns =
                        static_cast<std::uint32_t>(camera_packet_delay_->value())};
                show_camera_result(camera_actions_.update_config(
                    camera_snapshot_.cameras[index].id,
                    camera_snapshot_.cameras[index].saved_config_revision, value));
            });
            action_layout->addWidget(save);
            content_layout->addWidget(camera_control_actions_);

            auto* discover = make_child<QPushButton>(content, QStringLiteral("重新发现设备"));
            discover->setObjectName(QStringLiteral("camera-discover"));
            QObject::connect(discover, &QPushButton::clicked, this, [this] {
                if (camera_actions_.discover)
                    show_camera_result(camera_actions_.discover());
            });
            content_layout->addWidget(discover);
            camera_operation_value_ = make_child<QLabel>(content, QStringLiteral("尚未执行操作"));
            camera_operation_value_->setWordWrap(true);
            content_layout->addWidget(camera_operation_value_);
            content_layout->addStretch(1);
            scroll->setWidget(content);
            layout->addWidget(scroll, 1);
            QObject::connect(camera_selector_, &QComboBox::currentIndexChanged, this, [this] {
                populate_camera_editor();
                update_camera_controls();
            });
            QObject::connect(discovered_devices_, &QListWidget::currentRowChanged, this,
                             [this] { update_camera_controls(); });
            QObject::connect(camera_bind_location_, &QLineEdit::textChanged, this,
                             [this] { update_camera_controls(); });
            QObject::connect(camera_bind_slot_, &QComboBox::currentIndexChanged, this,
                             [this] { update_camera_controls(); });
            QObject::connect(camera_bind_button_, &QPushButton::clicked, this, [this] {
                const int discovered_index = discovered_devices_->currentRow();
                if (!camera_actions_.bind || discovered_index < 0 ||
                    discovered_index >=
                        static_cast<int>(camera_snapshot_.discovered_devices.size()) ||
                    camera_bind_slot_->currentIndex() < 0)
                    return;
                const auto& device = camera_snapshot_.discovered_devices[discovered_index];
                if (QMessageBox::question(
                        this, QStringLiteral("确认绑定相机"),
                        QStringLiteral(
                            "将序列号 %1 绑定到 "
                            "%2。绑定会读取相机当前参数，保存后需重启后台服务，是否继续？")
                            .arg(QString::fromStdString(device.serial),
                                 camera_bind_slot_->currentText())) != QMessageBox::Yes)
                    return;
                show_camera_result(camera_actions_.bind(
                    camera_bind_slot_->currentText().toStdString(), device.serial,
                    camera_bind_location_->text().trimmed().toStdString(),
                    camera_snapshot_.stored_config_revision));
            });
            pages_->addWidget(page);
            continue;
        }
        if (descriptor.id == ConsolePageId::device_status)
        {
            QWidget* page = make_child<QWidget>(pages_);
            page->setObjectName(QStringLiteral("page-device-status"));
            auto* layout = make_layout<QVBoxLayout>(page);
            layout->setContentsMargins(24, 20, 24, 20);
            auto* heading = make_child<QLabel>(page, QStringLiteral("系统、相机与算法指标"));
            heading->setProperty("role", "pageTitle");
            layout->addWidget(heading);
            auto* hint = make_child<QLabel>(
                page,
                QStringLiteral("指标来自后台服务有界快照；尚未实现的 M5/M6/M8 来源显示为不可用。"));
            hint->setProperty("role", "muted");
            hint->setWordWrap(true);
            layout->addWidget(hint);
            metrics_table_ = make_child<QTableWidget>(page);
            metrics_table_->setObjectName(QStringLiteral("operations-metrics"));
            configure_table(metrics_table_, {QStringLiteral("指标"), QStringLiteral("值"),
                                             QStringLiteral("单位"), QStringLiteral("可用性")});
            layout->addWidget(metrics_table_, 1);
            auto* refresh = make_child<QPushButton>(page, QStringLiteral("刷新全部运维数据"));
            refresh->setObjectName(QStringLiteral("operations-refresh"));
            QObject::connect(refresh, &QPushButton::clicked, this, [this] {
                if (operations_actions_.refresh)
                    operations_actions_.refresh();
            });
            layout->addWidget(refresh);
            pages_->addWidget(page);
            continue;
        }
        if (descriptor.id == ConsolePageId::alarms)
        {
            QWidget* page = make_child<QWidget>(pages_);
            page->setObjectName(QStringLiteral("page-alarms"));
            auto* layout = make_layout<QVBoxLayout>(page);
            layout->setContentsMargins(24, 20, 24, 20);
            auto* heading = make_child<QLabel>(page, QStringLiteral("当前与历史报警"));
            heading->setProperty("role", "pageTitle");
            layout->addWidget(heading);
            QWidget* filters = make_child<QWidget>(page);
            auto* filter_layout = make_layout<QHBoxLayout>(filters);
            filter_layout->setContentsMargins(0, 0, 0, 0);
            alarm_scope_ = make_child<QComboBox>(filters);
            alarm_scope_->setObjectName(QStringLiteral("alarm-scope"));
            alarm_scope_->addItem(QStringLiteral("当前报警"), 1);
            alarm_scope_->addItem(QStringLiteral("历史报警"), 0);
            alarm_scope_->addItem(QStringLiteral("全部"), -1);
            alarm_severity_ = make_child<QComboBox>(filters);
            alarm_severity_->setObjectName(QStringLiteral("alarm-severity"));
            alarm_severity_->addItems({QStringLiteral("全部级别"), QStringLiteral("Info"),
                                       QStringLiteral("Warning"), QStringLiteral("Error"),
                                       QStringLiteral("Critical")});
            alarm_source_ = make_child<QLineEdit>(filters);
            alarm_source_->setObjectName(QStringLiteral("alarm-source"));
            alarm_source_->setMaxLength(128);
            alarm_source_->setPlaceholderText(QStringLiteral("来源（精确匹配，可留空）"));
            auto* apply = make_child<QPushButton>(filters, QStringLiteral("应用筛选"));
            apply->setObjectName(QStringLiteral("alarm-filter-apply"));
            QObject::connect(apply, &QPushButton::clicked, this, [this] {
                if (!operations_actions_.query_alarms)
                    return;
                AlarmFilter filter;
                const int scope = alarm_scope_->currentData().toInt();
                filter.active = scope < 0 ? std::optional<bool>{} : std::optional<bool>{scope == 1};
                if (alarm_severity_->currentIndex() > 0)
                    filter.minimum_severity = alarm_severity_->currentText().toStdString();
                const QString source = alarm_source_->text().trimmed();
                if (!source.isEmpty())
                    filter.source = source.toStdString();
                show_operations_result(operations_actions_.query_alarms(std::move(filter)));
            });
            filter_layout->addWidget(alarm_scope_);
            filter_layout->addWidget(alarm_severity_);
            filter_layout->addWidget(alarm_source_, 1);
            filter_layout->addWidget(apply);
            layout->addWidget(filters);
            alarm_table_ = make_child<QTableWidget>(page);
            alarm_table_->setObjectName(QStringLiteral("operations-alarms"));
            configure_table(alarm_table_, {QStringLiteral("ID"), QStringLiteral("最近发生"),
                                           QStringLiteral("等级"), QStringLiteral("来源"),
                                           QStringLiteral("代码"), QStringLiteral("状态"),
                                           QStringLiteral("确认"), QStringLiteral("消息")});
            QObject::connect(alarm_table_, &QTableWidget::itemSelectionChanged, this,
                             [this] { update_alarm_details(); });
            layout->addWidget(alarm_table_, 1);
            alarm_details_ = make_child<QLabel>(page, QStringLiteral("选择报警查看详情"));
            alarm_details_->setObjectName(QStringLiteral("alarm-details"));
            alarm_details_->setWordWrap(true);
            alarm_details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
            layout->addWidget(alarm_details_);
            QWidget* actions = make_child<QWidget>(page);
            auto* action_layout = make_layout<QHBoxLayout>(actions);
            action_layout->setContentsMargins(0, 0, 0, 0);
            alarm_acknowledge_ = make_child<QPushButton>(actions, QStringLiteral("确认所选报警"));
            alarm_acknowledge_->setObjectName(QStringLiteral("alarm-acknowledge"));
            alarm_acknowledge_->setEnabled(false);
            QObject::connect(alarm_acknowledge_, &QPushButton::clicked, this, [this] {
                const int row = alarm_table_->currentRow();
                if (row < 0 || !operations_actions_.acknowledge || !alarm_table_->item(row, 0))
                    return;
                show_operations_result(operations_actions_.acknowledge(
                    alarm_table_->item(row, 0)->data(Qt::UserRole).toULongLong()));
            });
            alarm_export_ = make_child<QPushButton>(actions, QStringLiteral("导出当前结果 CSV"));
            alarm_export_->setObjectName(QStringLiteral("alarm-export"));
            QObject::connect(alarm_export_, &QPushButton::clicked, this, [this] {
                if (!operations_actions_.export_alarm_csv)
                    return;
                const QString path =
                    QFileDialog::getSaveFileName(this, QStringLiteral("导出报警记录"),
                                                 QStringLiteral("PaperBreakEdge-alarms.csv"),
                                                 QStringLiteral("CSV 文件 (*.csv)"));
                if (!path.isEmpty())
                    show_operations_result(operations_actions_.export_alarm_csv(
                        std::filesystem::path{path.toStdWString()}));
            });
            action_layout->addWidget(alarm_acknowledge_);
            action_layout->addWidget(alarm_export_);
            action_layout->addStretch(1);
            layout->addWidget(actions);
            pages_->addWidget(page);
            continue;
        }
        if (descriptor.id == ConsolePageId::logs)
        {
            QWidget* page = make_child<QWidget>(pages_);
            page->setObjectName(QStringLiteral("page-logs"));
            auto* layout = make_layout<QVBoxLayout>(page);
            layout->setContentsMargins(24, 20, 24, 20);
            auto* heading = make_child<QLabel>(page, QStringLiteral("最近结构化日志"));
            heading->setProperty("role", "pageTitle");
            layout->addWidget(heading);
            QWidget* filters = make_child<QWidget>(page);
            auto* filter_layout = make_layout<QHBoxLayout>(filters);
            filter_layout->setContentsMargins(0, 0, 0, 0);
            log_category_ = make_child<QComboBox>(filters);
            log_category_->setObjectName(QStringLiteral("log-category"));
            log_category_->addItems(
                {QStringLiteral("全部分类"), QStringLiteral("service"), QStringLiteral("camera"),
                 QStringLiteral("algorithm"), QStringLiteral("event"), QStringLiteral("storage"),
                 QStringLiteral("uplink"), QStringLiteral("ipc"), QStringLiteral("ui"),
                 QStringLiteral("audit"), QStringLiteral("performance")});
            log_level_ = make_child<QComboBox>(filters);
            log_level_->setObjectName(QStringLiteral("log-level"));
            log_level_->addItems({QStringLiteral("全部级别"), QStringLiteral("trace"),
                                  QStringLiteral("debug"), QStringLiteral("info"),
                                  QStringLiteral("warning"), QStringLiteral("error"),
                                  QStringLiteral("critical")});
            auto* apply = make_child<QPushButton>(filters, QStringLiteral("应用筛选"));
            apply->setObjectName(QStringLiteral("log-filter-apply"));
            QObject::connect(apply, &QPushButton::clicked, this, [this] {
                if (!operations_actions_.query_logs)
                    return;
                LogFilter filter;
                if (log_category_->currentIndex() > 0)
                    filter.category = log_category_->currentText().toStdString();
                if (log_level_->currentIndex() > 0)
                    filter.minimum_level = log_level_->currentText().toStdString();
                show_operations_result(operations_actions_.query_logs(std::move(filter)));
            });
            filter_layout->addWidget(log_category_);
            filter_layout->addWidget(log_level_);
            filter_layout->addWidget(apply);
            filter_layout->addStretch(1);
            layout->addWidget(filters);
            log_table_ = make_child<QTableWidget>(page);
            log_table_->setObjectName(QStringLiteral("operations-logs"));
            configure_table(log_table_, {QStringLiteral("序号"), QStringLiteral("时间"),
                                         QStringLiteral("线程"), QStringLiteral("分类"),
                                         QStringLiteral("等级"), QStringLiteral("消息")});
            layout->addWidget(log_table_, 1);
            pages_->addWidget(page);
            continue;
        }
        if (descriptor.id == ConsolePageId::maintenance)
        {
            QWidget* page = make_child<QWidget>(pages_);
            page->setObjectName(QStringLiteral("page-maintenance"));
            auto* layout = make_layout<QVBoxLayout>(page);
            layout->setContentsMargins(24, 20, 24, 20);
            auto* heading = make_child<QLabel>(page, QStringLiteral("系统维护与诊断"));
            heading->setProperty("role", "pageTitle");
            layout->addWidget(heading);
            auto* description = make_child<QLabel>(
                page, QStringLiteral(
                          "诊断 ZIP "
                          "包含脱敏配置、系统/相机/算法/IPC/"
                          "数据库指标、近期报警和日志、软件及依赖版本；不包含原始图像和凭据。"));
            description->setWordWrap(true);
            layout->addWidget(description);
            diagnostics_export_ = make_child<QPushButton>(page, QStringLiteral("导出脱敏诊断包"));
            diagnostics_export_->setObjectName(QStringLiteral("diagnostics-export"));
            diagnostics_export_->setEnabled(false);
            QObject::connect(diagnostics_export_, &QPushButton::clicked, this,
                             [this] { request_diagnostics_export(); });
            layout->addWidget(diagnostics_export_);
            operations_status_ = make_child<QLabel>(page, QStringLiteral("等待后台服务连接"));
            operations_status_->setObjectName(QStringLiteral("operations-status"));
            operations_status_->setWordWrap(true);
            operations_status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
            layout->addWidget(operations_status_);
            layout->addStretch(1);
            pages_->addWidget(page);
            continue;
        }
        if (descriptor.id != ConsolePageId::preview)
        {
            pages_->addWidget(
                make_placeholder_page(pages_, title, placeholder_message(descriptor.id), key));
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
        rate_choice->addItems(
            {QStringLiteral("2 fps"), QStringLiteral("3 fps"), QStringLiteral("5 fps")});
        auto* resolution_choice = make_child<QComboBox>(controls);
        resolution_choice->addItems({QStringLiteral("自适应分辨率"), QStringLiteral("1280×720"),
                                     QStringLiteral("640×360")});
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
            preview_tiles[camera] =
                make_preview_tile(grid, camera, preview_images_[camera], preview_overlays_[camera]);
            grid_layout->addWidget(preview_tiles[camera], camera / 2, camera % 2);
        }
        preview_layout->addWidget(grid, 1);
        QObject::connect(preview_pause_button_, &QPushButton::clicked, this, [this] {
            preview_paused_ = !preview_paused_;
            preview_pause_button_->setText(preview_paused_ ? QStringLiteral("恢复显示")
                                                           : QStringLiteral("暂停显示"));
            if (preview_pause_changed_)
                preview_pause_changed_(preview_paused_);
        });
        QObject::connect(layout_choice, &QComboBox::currentIndexChanged, this,
                         [preview_tiles](const int selection) {
                             for (std::size_t tile = 0; tile < preview_tiles.size(); ++tile)
                                 preview_tiles[tile]->setVisible(
                                     selection == 0 || static_cast<int>(tile + 1U) == selection);
                         });
        QObject::connect(one_to_one, &QPushButton::clicked, this, [this] {
            for (QLabel* image : preview_images_)
                image->setScaledContents(false);
        });
        QObject::connect(adaptive, &QPushButton::clicked, this, [this] {
            for (QLabel* image : preview_images_)
                image->setScaledContents(true);
        });
        QObject::connect(full_screen, &QPushButton::clicked, this,
                         [this] { isFullScreen() ? showNormal() : showFullScreen(); });
        QObject::connect(capture, &QPushButton::clicked, this, [this] {
            const auto found =
                std::find_if(preview_images_.begin(), preview_images_.end(),
                             [](const QLabel* label) { return !label->pixmap().isNull(); });
            if (found == preview_images_.end())
                return;
            const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存预览抓图"),
                                                              QStringLiteral("preview.jpg"),
                                                              QStringLiteral("JPEG 图像 (*.jpg)"));
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
        preview_overlays_[index]->setText(
            QStringLiteral("%1 · 帧 %2 · %3 fps · %4")
                .arg(QString::fromStdString(frame.camera_id))
                .arg(frame.frame_number)
                .arg(frame.actual_fps.value_or(0.0), 0, 'f', 1)
                .arg(QString::fromStdString(frame.detection_result.empty()
                                                ? frame.camera_status
                                                : frame.detection_result)));
    }
}

void MainWindow::apply_camera_snapshot(const CameraClientSnapshot& snapshot)
{
    const QString selected = camera_selector_ ? camera_selector_->currentText() : QString{};
    QString selected_serial;
    if (discovered_devices_ && discovered_devices_->currentRow() >= 0 &&
        discovered_devices_->currentRow() <
            static_cast<int>(camera_snapshot_.discovered_devices.size()))
        selected_serial = QString::fromStdString(
            camera_snapshot_.discovered_devices[discovered_devices_->currentRow()].serial);
    camera_snapshot_ = snapshot;
    if (!camera_configuration_value_)
        return;

    camera_selector_->blockSignals(true);
    camera_selector_->clear();
    for (const auto& camera : snapshot.cameras)
        camera_selector_->addItem(QString::fromStdString(camera.id));
    const int previous = camera_selector_->findText(selected);
    camera_selector_->setCurrentIndex(previous >= 0 ? previous
                                                    : (snapshot.cameras.empty() ? -1 : 0));
    camera_selector_->blockSignals(false);

    discovered_devices_->blockSignals(true);
    discovered_devices_->clear();
    int discovered_selection = -1;
    for (std::size_t index = 0; index < snapshot.discovered_devices.size(); ++index)
    {
        const auto& device = snapshot.discovered_devices[index];
        const QString availability = device.exclusive_access_available
                                         ? QStringLiteral("可独占访问")
                                         : QStringLiteral("被占用/不可独占访问");
        discovered_devices_->addItem(
            QStringLiteral("%1 · 序列号 %2 · IP %3 · 主机网卡 %4 · %5%6")
                .arg(QString::fromStdString(device.model), QString::fromStdString(device.serial),
                     QString::fromStdString(device.ip),
                     QString::fromStdString(device.network_interface), availability,
                     snapshot.stale ? QStringLiteral("（已过期）") : QString{}));
        if (QString::fromStdString(device.serial) == selected_serial)
            discovered_selection = static_cast<int>(index);
    }
    discovered_devices_->setCurrentRow(discovered_selection >= 0
                                           ? discovered_selection
                                           : (snapshot.discovered_devices.empty() ? -1 : 0));
    discovered_devices_->blockSignals(false);

    camera_bind_slot_->blockSignals(true);
    const QString previous_slot = camera_bind_slot_->currentText();
    camera_bind_slot_->clear();
    for (int index = 1; index <= 4; ++index)
    {
        const QString id = QStringLiteral("CAM%1").arg(index, 2, 10, QChar{'0'});
        const bool used = std::ranges::any_of(snapshot.cameras, [&](const auto& camera) {
            return QString::fromStdString(camera.id) == id;
        });
        if (!used)
            camera_bind_slot_->addItem(id);
    }
    const int previous_slot_index = camera_bind_slot_->findText(previous_slot);
    if (previous_slot_index >= 0)
        camera_bind_slot_->setCurrentIndex(previous_slot_index);
    camera_bind_slot_->blockSignals(false);

    QStringList lines;
    std::size_t healthy = 0U;
    for (const auto& camera : snapshot.cameras)
    {
        if (camera.state == "connected" || camera.state == "acquiring")
            ++healthy;
        lines.push_back(
            QStringLiteral(
                "%1 · %2 · 序列号 %3 · 状态 %4\n型号 %5 · IP %6\n保存值：曝光 %7 us / 增益 %8 dB / "
                "帧率 %9 fps\n实际值：曝光 %10 us / 增益 %11 dB / 帧率 %12 fps")
                .arg(QString::fromStdString(camera.id), QString::fromStdString(camera.location),
                     QString::fromStdString(camera.serial), QString::fromStdString(camera.state),
                     QString::fromStdString(camera.model), QString::fromStdString(camera.ip))
                .arg(camera.saved.exposure_us.value_or(0.0), 0, 'f', 1)
                .arg(camera.saved.gain_db.value_or(0.0), 0, 'f', 1)
                .arg(camera.saved.frame_rate.value_or(0.0), 0, 'f', 1)
                .arg(camera.actual.exposure_us.value_or(0.0), 0, 'f', 1)
                .arg(camera.actual.gain_db.value_or(0.0), 0, 'f', 1)
                .arg(camera.actual.frame_rate.value_or(0.0), 0, 'f', 1));
    }
    if (snapshot.cameras.empty())
    {
        lines.push_back(QStringLiteral(
            "当前保存配置未包含相机。网络发现结果单独显示在下方，可选择可用设备绑定。"));
    }
    if (snapshot.topology_restart_required)
        lines.push_back(QStringLiteral("相机拓扑已保存但尚未生效，请通过托盘菜单重启后台服务。"));
    if (snapshot.stale)
        lines.push_back(QStringLiteral("相机数据不可用或已过期；旧值不会作为实时状态使用。"));
    camera_configuration_value_->setText(lines.join(QStringLiteral("\n\n")));
    camera_count_value_->setText(QStringLiteral("%1/%2").arg(healthy).arg(snapshot.cameras.size()));
    if (snapshot.operation)
    {
        const auto& operation = *snapshot.operation;
        if (operation.pending)
            camera_operation_value_->setText(
                QStringLiteral("正在执行 %1").arg(QString::fromStdString(operation.operation)));
        else if (!operation.succeeded)
        {
            if (snapshot.error && snapshot.error->business_code == "SYS_NOT_SUPPORTED")
                camera_operation_value_->setText(QStringLiteral(
                    "失败：后台服务未装配 Hikrobot MVS 相机提供者，请检查部署完整性。"));
            else
                camera_operation_value_->setText(
                    QStringLiteral("失败：%1").arg(QString::fromStdString(operation.message)));
        }
        else if (operation.operation == "camera.getConfig")
            camera_operation_value_->setText(
                QStringLiteral("成功：已读取相机当前参数并载入编辑区；确认后再保存下发。"));
        else if (operation.operation == "camera.connect" && !operation.applied)
            camera_operation_value_->setText(
                QStringLiteral("相机已连接，但保存参数未应用：%1。请读取当前参数并确认保存。")
                    .arg(QString::fromStdString(operation.message)));
        else
            camera_operation_value_->setText(
                QStringLiteral("成功：已保存=%1，已下发=%2，已应用=%3，需重启=%4")
                    .arg(operation.saved ? QStringLiteral("是") : QStringLiteral("否"),
                         operation.dispatched ? QStringLiteral("是") : QStringLiteral("否"),
                         operation.applied ? QStringLiteral("是") : QStringLiteral("否"),
                         operation.restart_required ? QStringLiteral("是") : QStringLiteral("否")));
    }
    bool loaded_actual = false;
    if (camera_parameter_read_pending_ && snapshot.operation &&
        snapshot.operation->operation == "camera.getConfig" && !snapshot.operation->pending)
    {
        camera_parameter_read_pending_ = false;
        if (snapshot.operation->succeeded)
        {
            const auto actual = std::find_if(
                camera_snapshot_.cameras.begin(), camera_snapshot_.cameras.end(),
                [&](const auto& item) { return item.id == snapshot.operation->camera_id; });
            if (actual != camera_snapshot_.cameras.end() && actual->actual.exposure_us &&
                camera_selector_->currentText().toStdString() == actual->id)
            {
                camera_editor_id_ = actual->id;
                camera_editor_revision_ = actual->saved_config_revision;
                populate_camera_editor(actual->actual);
                loaded_actual = true;
            }
        }
    }
    const auto selected_index = static_cast<std::size_t>(camera_selector_->currentIndex());
    if (selected_index < camera_snapshot_.cameras.size())
    {
        const auto& selected_camera = camera_snapshot_.cameras[selected_index];
        if (!loaded_actual && (camera_editor_id_ != selected_camera.id ||
                               camera_editor_revision_ != selected_camera.saved_config_revision))
            populate_camera_editor();
    }
    update_camera_controls();
}

void MainWindow::populate_camera_editor()
{
    if (!camera_selector_ || camera_selector_->currentIndex() < 0)
        return;
    const auto index = static_cast<std::size_t>(camera_selector_->currentIndex());
    if (index >= camera_snapshot_.cameras.size())
        return;
    const auto& value = camera_snapshot_.cameras[index].saved;
    camera_editor_id_ = camera_snapshot_.cameras[index].id;
    camera_editor_revision_ = camera_snapshot_.cameras[index].saved_config_revision;
    populate_camera_editor(value);
}

void MainWindow::populate_camera_editor(const CameraParameterValue& value)
{
    camera_exposure_->setValue(value.exposure_us.value_or(1.0));
    camera_gain_->setValue(value.gain_db.value_or(0.0));
    camera_fps_->setValue(value.frame_rate.value_or(1.0));
    if (value.roi)
    {
        camera_roi_width_->setValue(static_cast<int>(value.roi->width));
        camera_roi_height_->setValue(static_cast<int>(value.roi->height));
        camera_roi_x_->setValue(static_cast<int>(value.roi->offset_x));
        camera_roi_y_->setValue(static_cast<int>(value.roi->offset_y));
    }
    camera_pixel_format_->setCurrentText(QString::fromStdString(value.pixel_format));
    camera_trigger_mode_->setCurrentText(QString::fromStdString(value.trigger_mode));
    camera_trigger_source_->setText(QString::fromStdString(value.trigger_source));
    camera_trigger_delay_->setValue(static_cast<int>(value.trigger_delay_us.value_or(0U)));
    camera_packet_size_->setValue(static_cast<int>(value.packet_size_bytes.value_or(1500U)));
    camera_packet_delay_->setValue(static_cast<int>(value.inter_packet_delay_ns.value_or(0U)));
}

void MainWindow::update_camera_controls()
{
    if (!camera_editor_ || !camera_control_actions_ || !camera_bind_button_ ||
        !camera_read_parameters_button_)
        return;
    const bool configured = camera_selector_ && camera_selector_->currentIndex() >= 0 &&
                            !camera_snapshot_.stale && !camera_snapshot_.topology_restart_required;
    camera_editor_->setEnabled(configured);
    camera_control_actions_->setEnabled(configured);
    bool connected = false;
    if (configured)
    {
        const auto index = static_cast<std::size_t>(camera_selector_->currentIndex());
        connected = index < camera_snapshot_.cameras.size() &&
                    (camera_snapshot_.cameras[index].state == "connected" ||
                     camera_snapshot_.cameras[index].state == "acquiring");
    }
    camera_read_parameters_button_->setEnabled(connected);

    const int row = discovered_devices_ ? discovered_devices_->currentRow() : -1;
    const bool valid_row =
        row >= 0 && row < static_cast<int>(camera_snapshot_.discovered_devices.size());
    bool bindable = valid_row && !camera_snapshot_.stale &&
                    !camera_snapshot_.topology_restart_required &&
                    camera_bind_slot_->currentIndex() >= 0 &&
                    !camera_bind_location_->text().trimmed().isEmpty();
    if (valid_row)
    {
        const auto& device = camera_snapshot_.discovered_devices[static_cast<std::size_t>(row)];
        bindable = bindable && device.exclusive_access_available &&
                   device.model == "MV-CS020-60GM" &&
                   !std::ranges::any_of(camera_snapshot_.cameras, [&](const auto& camera) {
                       return camera.serial == device.serial;
                   });
    }
    camera_bind_button_->setEnabled(bindable);
}

void MainWindow::run_camera_control(const std::string& command, const bool confirmation_required)
{
    if (!camera_actions_.control || !camera_selector_ || camera_selector_->currentIndex() < 0)
        return;
    if (confirmation_required &&
        QMessageBox::question(this, QStringLiteral("确认相机操作"),
                              QStringLiteral("操作 %1 可能影响当前采集，是否继续？")
                                  .arg(QString::fromStdString(command))) != QMessageBox::Yes)
        return;
    show_camera_result(
        camera_actions_.control(command, camera_selector_->currentText().toStdString()));
}

void MainWindow::show_camera_result(const Result<void>& result)
{
    if (!result && camera_operation_value_)
        camera_operation_value_->setText(
            QStringLiteral("失败：%1").arg(QString::fromStdString(result.error().message)));
}

void MainWindow::show_operations_result(const Result<void>& result)
{
    if (!result && operations_status_)
        operations_status_->setText(
            QStringLiteral("失败：%1").arg(QString::fromStdString(result.error().message)));
}

void MainWindow::request_diagnostics_export()
{
    if (!operations_actions_.export_diagnostics)
        return;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出脱敏诊断包"), QStringLiteral("PaperBreakEdge-diagnostics.zip"),
        QStringLiteral("ZIP 文件 (*.zip)"));
    if (!path.isEmpty())
        show_operations_result(
            operations_actions_.export_diagnostics(std::filesystem::path{path.toStdWString()}));
}

void MainWindow::update_alarm_details()
{
    if (!alarm_table_ || !alarm_details_ || !alarm_acknowledge_)
        return;
    const int row = alarm_table_->currentRow();
    if (row < 0 || !alarm_table_->item(row, 0))
    {
        alarm_details_->setText(QStringLiteral("选择报警查看详情"));
        alarm_acknowledge_->setEnabled(false);
        return;
    }
    const auto alarm_id = alarm_table_->item(row, 0)->data(Qt::UserRole).toULongLong();
    const auto found =
        std::find_if(operations_snapshot_.alarms.begin(), operations_snapshot_.alarms.end(),
                     [alarm_id](const auto& item) { return item.alarm_id == alarm_id; });
    if (found == operations_snapshot_.alarms.end())
        return;
    QStringList details;
    details.push_back(QStringLiteral("%1 · 首次 %2 · 最近 %3 · 发生 %4 次")
                          .arg(QString::fromStdString(found->code),
                               local_date_time_text(found->first_occurred_at),
                               local_date_time_text(found->last_occurred_at))
                          .arg(found->occurrence_count));
    details.push_back(QString::fromStdString(found->message));
    for (const auto& [key, value] : found->details)
        details.push_back(QStringLiteral("%1 = %2").arg(QString::fromStdString(key),
                                                        local_date_time_text(value)));
    alarm_details_->setText(details.join(QChar{'\n'}));
    alarm_acknowledge_->setEnabled(operations_snapshot_.connection.state ==
                                       ipc::ClientConnectionState::connected &&
                                   !operations_snapshot_.alarms_stale &&
                                   !operations_snapshot_.operation_pending && !found->acknowledged);
}

void MainWindow::apply_operations_snapshot(const OperationsSnapshot& snapshot)
{
    operations_snapshot_ = snapshot;
    if (!metrics_table_ || !alarm_table_ || !log_table_)
        return;

    metrics_table_->setRowCount(static_cast<int>(snapshot.metrics.size()));
    for (std::size_t index = 0; index < snapshot.metrics.size(); ++index)
    {
        const auto& metric = snapshot.metrics[index];
        const int row = static_cast<int>(index);
        set_table_item(metrics_table_, row, 0, QString::fromStdString(metric.name));
        const bool is_wall_clock = metric.unit == "unix_milliseconds";
        set_table_item(metrics_table_, row, 1,
                       metric.available
                           ? (is_wall_clock ? local_epoch_milliseconds_text(metric.value)
                                            : QString::fromStdString(metric.value))
                           : QStringLiteral("不可用"));
        set_table_item(metrics_table_, row, 2,
                       is_wall_clock ? QStringLiteral("本地时间")
                                     : QString::fromStdString(metric.unit));
        set_table_item(metrics_table_, row, 3,
                       stale_value(metric.available ? QStringLiteral("可用")
                                                    : QStringLiteral("未初始化/不可用"),
                                   snapshot.metrics_stale));
    }
    metrics_table_->resizeColumnsToContents();

    std::optional<std::uint64_t> selected_alarm;
    if (alarm_table_->currentRow() >= 0 && alarm_table_->item(alarm_table_->currentRow(), 0))
        selected_alarm =
            alarm_table_->item(alarm_table_->currentRow(), 0)->data(Qt::UserRole).toULongLong();
    alarm_table_->setRowCount(static_cast<int>(snapshot.alarms.size()));
    int selected_row = -1;
    for (std::size_t index = 0; index < snapshot.alarms.size(); ++index)
    {
        const auto& alarm = snapshot.alarms[index];
        const int row = static_cast<int>(index);
        set_table_item(alarm_table_, row, 0, QString::number(alarm.alarm_id),
                       QVariant::fromValue<qulonglong>(alarm.alarm_id));
        set_table_item(alarm_table_, row, 1, local_date_time_text(alarm.last_occurred_at));
        set_table_item(alarm_table_, row, 2, QString::fromStdString(alarm.severity));
        set_table_item(alarm_table_, row, 3, QString::fromStdString(alarm.source));
        set_table_item(alarm_table_, row, 4, QString::fromStdString(alarm.code));
        set_table_item(alarm_table_, row, 5,
                       alarm.active ? QStringLiteral("当前") : QStringLiteral("已清除"));
        set_table_item(alarm_table_, row, 6,
                       alarm.acknowledged ? QStringLiteral("已确认") : QStringLiteral("未确认"));
        set_table_item(alarm_table_, row, 7, QString::fromStdString(alarm.message));
        if (selected_alarm && *selected_alarm == alarm.alarm_id)
            selected_row = row;
    }
    if (selected_row >= 0)
        alarm_table_->selectRow(selected_row);
    alarm_table_->resizeColumnsToContents();
    alarm_export_->setEnabled(!snapshot.alarms.empty() && !snapshot.operation_pending);
    update_alarm_details();

    log_table_->setRowCount(static_cast<int>(snapshot.logs.size()));
    for (std::size_t index = 0; index < snapshot.logs.size(); ++index)
    {
        const auto& record = snapshot.logs[index];
        const int row = static_cast<int>(index);
        set_table_item(log_table_, row, 0, QString::number(record.sequence));
        set_table_item(log_table_, row, 1, local_date_time_text(record.timestamp));
        set_table_item(log_table_, row, 2, QString::number(record.thread_id));
        set_table_item(log_table_, row, 3, QString::fromStdString(record.category));
        set_table_item(log_table_, row, 4, QString::fromStdString(record.level));
        set_table_item(log_table_, row, 5, QString::fromStdString(record.message));
    }
    log_table_->resizeColumnsToContents();

    const bool connected = snapshot.connection.state == ipc::ClientConnectionState::connected;
    diagnostics_export_->setEnabled(connected && !snapshot.operation_pending);
    if (snapshot.operation_pending)
        operations_status_->setText(
            QStringLiteral("正在执行 %1").arg(QString::fromStdString(snapshot.operation)));
    else if (snapshot.error)
        operations_status_->setText(
            QStringLiteral("失败：%1（%2）")
                .arg(QString::fromStdString(snapshot.error->message),
                     QString::fromStdString(snapshot.error->business_code)));
    else if (snapshot.exported_path)
        operations_status_->setText(
            QStringLiteral("导出完成：%1")
                .arg(QString::fromStdWString(snapshot.exported_path->wstring())));
    else if (!connected)
        operations_status_->setText(QStringLiteral("后台服务连接中断，运维数据已标记过期"));
    else
        operations_status_->setText(QStringLiteral("运维数据已同步；诊断导出内容将强制脱敏"));
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
    if (camera_snapshot_.stale)
        camera_count_value_->setText(QStringLiteral("不可用（已过期）"));

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
                                         local_date_time_text(alarm.last_occurred_at)));
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
        QDateTime::currentDateTime().toString(QString::fromLatin1(local_clock_format)));
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

bool MainWindow::camera_configuration_ready() const noexcept
{
    return camera_selector_ && camera_exposure_ && camera_gain_ && camera_fps_ &&
           camera_roi_width_ && camera_roi_height_ && camera_pixel_format_ &&
           camera_trigger_mode_ && camera_trigger_source_ && camera_packet_size_ &&
           discovered_devices_ && camera_bind_slot_ && camera_bind_location_ &&
           camera_bind_button_ && findChild<QPushButton*>(QStringLiteral("camera-discover")) &&
           findChild<QPushButton*>(QStringLiteral("camera-read-parameters")) &&
           findChild<QPushButton*>(QStringLiteral("camera-camera.disconnect")) &&
           findChild<QPushButton*>(QStringLiteral("camera-update-config"));
}

std::size_t MainWindow::discovered_camera_count() const noexcept
{
    return discovered_devices_ ? static_cast<std::size_t>(discovered_devices_->count()) : 0U;
}

bool MainWindow::camera_device_controls_disabled() const noexcept
{
    return camera_editor_ && camera_control_actions_ && camera_bind_button_ &&
           !camera_editor_->isEnabled() && !camera_control_actions_->isEnabled() &&
           !camera_bind_button_->isEnabled();
}

bool MainWindow::select_theme_mode(const ThemeMode mode) noexcept
{
    if (!theme_selector_)
        return false;
    const int index = theme_selector_->findData(static_cast<int>(mode));
    if (index < 0)
        return false;
    theme_selector_->setCurrentIndex(index);
    return static_cast<ThemeMode>(theme_selector_->currentData().toInt()) == mode;
}

bool MainWindow::operations_pages_ready() const noexcept
{
    return metrics_table_ && alarm_scope_ && alarm_severity_ && alarm_source_ && alarm_table_ &&
           alarm_details_ && alarm_acknowledge_ && alarm_export_ && log_category_ && log_level_ &&
           log_table_ && operations_status_ && diagnostics_export_ &&
           has_readable_table_header(metrics_table_) && has_readable_table_header(alarm_table_) &&
           has_readable_table_header(log_table_);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    hide();
    event->ignore();
}

} // namespace paperbreak::console
