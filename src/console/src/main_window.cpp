#include "main_window.hpp"

#include "paperbreak/console/navigation_model.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDesktopServices>
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
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QTimeZone>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <memory>
#include <string_view>
#include <tuple>
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

QGroupBox* make_camera_card(QWidget* parent, const QString& camera_id, QLabel*& state, QLabel*& fps,
                            QLabel*& brightness, QLabel*& last_frame)
{
    QGroupBox* card = make_child<QGroupBox>(parent, camera_id);
    card->setProperty("role", "cameraCard");
    auto* layout = make_layout<QGridLayout>(card);
    layout->setContentsMargins(14, 18, 14, 14);
    const std::array<std::pair<QString, QString>, 4> rows{
        std::pair{QStringLiteral("连接状态"), QStringLiteral("正在同步")},
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
        const std::array<QLabel**, 4U> outputs{&state, &fps, &brightness, &last_frame};
        *outputs[static_cast<std::size_t>(row)] = value;
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
        return QStringLiteral("预览状态正在同步。");
    case ConsolePageId::camera_configuration:
        return QStringLiteral("相机配置正在同步。");
    case ConsolePageId::algorithm_configuration:
        return QStringLiteral("算法配置正在同步。");
    case ConsolePageId::event_configuration:
        return QStringLiteral("事件配置正在同步。");
    case ConsolePageId::storage_configuration:
        return QStringLiteral("存储配置正在从后台服务同步。");
    case ConsolePageId::uplink_configuration:
        return QStringLiteral("上位机配置正在同步。");
    case ConsolePageId::device_status:
    case ConsolePageId::alarms:
    case ConsolePageId::logs:
    case ConsolePageId::maintenance:
        return QStringLiteral("运维状态正在同步。");
    case ConsolePageId::events:
        return QStringLiteral("事件记录正在同步。");
    case ConsolePageId::overview:
        break;
    }
    return QStringLiteral("页面骨架已建立。");
}

} // namespace

MainWindow::MainWindow(std::function<void(bool)> preview_pause_changed,
                       CameraUiActions camera_actions, ThemeUiActions theme_actions,
                       OperationsUiActions operations_actions, AlgorithmUiActions algorithm_actions,
                       EventUiActions event_actions, StorageUiActions storage_actions,
                       UplinkUiActions uplink_actions, QWidget* parent)
    : QMainWindow(parent), preview_pause_changed_(std::move(preview_pause_changed)),
      camera_actions_(std::move(camera_actions)), theme_actions_(std::move(theme_actions)),
      operations_actions_(std::move(operations_actions)),
      algorithm_actions_(std::move(algorithm_actions)), event_actions_(std::move(event_actions)),
      storage_actions_(std::move(storage_actions)), uplink_actions_(std::move(uplink_actions))
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
            make_camera_card(cameras, QStringLiteral("CAM%1").arg(index + 1, 2, 10, QChar{'0'}),
                             overview_camera_states_[static_cast<std::size_t>(index)],
                             overview_camera_fps_[static_cast<std::size_t>(index)],
                             overview_camera_brightness_[static_cast<std::size_t>(index)],
                             overview_camera_last_frames_[static_cast<std::size_t>(index)]),
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
    overview_detector_value_ = make_child<QLabel>(detection, QStringLiteral("检测器：正在同步"));
    overview_candidate_value_ =
        make_child<QLabel>(detection, QStringLiteral("累计候选事件：正在同步"));
    detection_layout->addWidget(overview_detector_value_);
    detection_layout->addWidget(overview_candidate_value_);

    QGroupBox* upload = make_child<QGroupBox>(summary, QStringLiteral("上位机与上传"));
    auto* upload_layout = make_layout<QVBoxLayout>(upload);
    overview_uplink_value_ = make_child<QLabel>(upload, QStringLiteral("上位机连接：正在同步"));
    overview_upload_value_ = make_child<QLabel>(upload, QStringLiteral("待上传任务：正在同步"));
    upload_layout->addWidget(overview_uplink_value_);
    upload_layout->addWidget(overview_upload_value_);

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
        if (descriptor.id == ConsolePageId::algorithm_configuration)
        {
            QWidget* page = make_child<QWidget>(pages_);
            page->setObjectName(QStringLiteral("page-algorithm-configuration"));
            auto* layout = make_layout<QVBoxLayout>(page);
            layout->setContentsMargins(24, 20, 24, 20);
            auto* heading =
                make_child<QLabel>(page, QStringLiteral("算法配置、实际状态与单帧测试"));
            heading->setProperty("role", "pageTitle");
            layout->addWidget(heading);
            auto* prototype_notice = make_child<QLabel>(
                page,
                QStringLiteral(
                    "M6-00 验收基线尚未冻结：当前检测器均为原型，不代表正式断纸算法已通过。"));
            prototype_notice->setObjectName(QStringLiteral("algorithm-prototype-notice"));
            prototype_notice->setWordWrap(true);
            prototype_notice->setProperty("role", "warning");
            layout->addWidget(prototype_notice);

            auto* scroll = make_child<QScrollArea>(page);
            scroll->setWidgetResizable(true);
            QWidget* content = make_child<QWidget>(scroll);
            auto* content_layout = make_layout<QVBoxLayout>(content);
            content_layout->setContentsMargins(2, 2, 8, 2);
            content_layout->setSpacing(12);

            auto* selection = make_child<QWidget>(content);
            auto* selection_layout = make_layout<QHBoxLayout>(selection);
            selection_layout->setContentsMargins(0, 0, 0, 0);
            algorithm_camera_selector_ = make_child<QComboBox>(selection);
            algorithm_camera_selector_->setObjectName(QStringLiteral("algorithm-camera-selector"));
            algorithm_camera_selector_->addItems({QStringLiteral("CAM01"), QStringLiteral("CAM02"),
                                                  QStringLiteral("CAM03"),
                                                  QStringLiteral("CAM04")});
            auto* refresh = make_child<QPushButton>(selection, QStringLiteral("刷新实际状态"));
            refresh->setObjectName(QStringLiteral("algorithm-refresh"));
            selection_layout->addWidget(make_child<QLabel>(selection, QStringLiteral("逻辑相机")));
            selection_layout->addWidget(algorithm_camera_selector_);
            selection_layout->addStretch(1);
            selection_layout->addWidget(refresh);
            content_layout->addWidget(selection);

            algorithm_editor_ = make_child<QWidget>(content);
            algorithm_editor_->setObjectName(QStringLiteral("algorithm-editor"));
            auto* form = make_layout<QFormLayout>(algorithm_editor_);
            algorithm_enabled_ =
                make_child<QCheckBox>(algorithm_editor_, QStringLiteral("启用自动检测"));
            algorithm_type_ = make_child<QComboBox>(algorithm_editor_);
            algorithm_type_->setObjectName(QStringLiteral("algorithm-type"));
            algorithm_type_->addItem(QStringLiteral("模拟检测器（原型）"), QStringLiteral("mock"));
            algorithm_type_->addItem(QStringLiteral("传统视觉（原型）"),
                                     QStringLiteral("classical-vision"));
            algorithm_roi_width_ = make_child<QSpinBox>(algorithm_editor_);
            algorithm_roi_height_ = make_child<QSpinBox>(algorithm_editor_);
            algorithm_roi_x_ = make_child<QSpinBox>(algorithm_editor_);
            algorithm_roi_y_ = make_child<QSpinBox>(algorithm_editor_);
            algorithm_roi_width_->setRange(1, 16384);
            algorithm_roi_height_->setRange(1, 16384);
            algorithm_roi_x_->setRange(0, 16383);
            algorithm_roi_y_->setRange(0, 16383);
            algorithm_candidate_threshold_ = make_child<QDoubleSpinBox>(algorithm_editor_);
            algorithm_confirmation_threshold_ = make_child<QDoubleSpinBox>(algorithm_editor_);
            for (auto* threshold :
                 {algorithm_candidate_threshold_, algorithm_confirmation_threshold_})
            {
                threshold->setRange(0.0, 1.0);
                threshold->setDecimals(4);
                threshold->setSingleStep(0.01);
            }
            algorithm_consecutive_frames_ = make_child<QSpinBox>(algorithm_editor_);
            algorithm_consecutive_frames_->setRange(1, 1000);
            algorithm_cooldown_ms_ = make_child<QSpinBox>(algorithm_editor_);
            algorithm_cooldown_ms_->setRange(0, 3600000);
            algorithm_model_reference_ = make_child<QLineEdit>(algorithm_editor_);
            algorithm_model_reference_->setMaxLength(512);
            algorithm_model_version_ = make_child<QLineEdit>(algorithm_editor_);
            algorithm_model_version_->setMaxLength(128);
            algorithm_device_ = make_child<QComboBox>(algorithm_editor_);
            algorithm_device_->setEditable(true);
            algorithm_device_->addItems(
                {QStringLiteral("cpu"), QStringLiteral("cuda"), QStringLiteral("directml")});
            algorithm_device_->lineEdit()->setMaxLength(64);
            algorithm_debug_overlay_ =
                make_child<QCheckBox>(algorithm_editor_, QStringLiteral("启用调试可视化数据"));
            form->addRow(QStringLiteral("启用"), algorithm_enabled_);
            form->addRow(QStringLiteral("检测器类型"), algorithm_type_);
            form->addRow(QStringLiteral("ROI 宽"), algorithm_roi_width_);
            form->addRow(QStringLiteral("ROI 高"), algorithm_roi_height_);
            form->addRow(QStringLiteral("ROI X"), algorithm_roi_x_);
            form->addRow(QStringLiteral("ROI Y"), algorithm_roi_y_);
            form->addRow(QStringLiteral("候选阈值"), algorithm_candidate_threshold_);
            form->addRow(QStringLiteral("确认阈值"), algorithm_confirmation_threshold_);
            form->addRow(QStringLiteral("连续帧"), algorithm_consecutive_frames_);
            form->addRow(QStringLiteral("冷却时间 (ms)"), algorithm_cooldown_ms_);
            form->addRow(QStringLiteral("模型引用"), algorithm_model_reference_);
            form->addRow(QStringLiteral("配置模型版本"), algorithm_model_version_);
            form->addRow(QStringLiteral("设备"), algorithm_device_);
            form->addRow(QStringLiteral("调试"), algorithm_debug_overlay_);
            algorithm_editor_->setEnabled(false);
            content_layout->addWidget(algorithm_editor_);

            auto* actions = make_child<QWidget>(content);
            auto* actions_layout = make_layout<QHBoxLayout>(actions);
            actions_layout->setContentsMargins(0, 0, 0, 0);
            algorithm_save_ =
                make_child<QPushButton>(actions, QStringLiteral("保存并应用算法配置"));
            algorithm_save_->setObjectName(QStringLiteral("algorithm-save"));
            algorithm_test_ = make_child<QPushButton>(actions, QStringLiteral("测试当前图像"));
            algorithm_test_->setObjectName(QStringLiteral("algorithm-test-current-frame"));
            algorithm_save_->setEnabled(false);
            algorithm_test_->setEnabled(false);
            actions_layout->addWidget(algorithm_save_);
            actions_layout->addWidget(algorithm_test_);
            actions_layout->addStretch(1);
            content_layout->addWidget(actions);

            algorithm_runtime_status_ =
                make_child<QLabel>(content, QStringLiteral("正在读取实际生效状态"));
            algorithm_runtime_status_->setObjectName(QStringLiteral("algorithm-runtime-status"));
            algorithm_runtime_status_->setWordWrap(true);
            algorithm_runtime_status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
            content_layout->addWidget(algorithm_runtime_status_);

            auto* metric_group =
                make_child<QGroupBox>(content, QStringLiteral("算法运行时汇总性能与结果指标"));
            auto* metric_layout = make_layout<QVBoxLayout>(metric_group);
            algorithm_metrics_ = make_child<QTableWidget>(metric_group);
            algorithm_metrics_->setObjectName(QStringLiteral("algorithm-metrics"));
            configure_table(algorithm_metrics_,
                            {QStringLiteral("指标"), QStringLiteral("值"), QStringLiteral("单位")});
            metric_layout->addWidget(algorithm_metrics_);
            content_layout->addWidget(metric_group);

            auto* test_group = make_child<QGroupBox>(content, QStringLiteral("隔离单帧测试结果"));
            auto* test_layout = make_layout<QVBoxLayout>(test_group);
            algorithm_test_result_ = make_child<QLabel>(test_group, QStringLiteral("尚未测试"));
            algorithm_test_result_->setObjectName(QStringLiteral("algorithm-test-result"));
            algorithm_test_result_->setWordWrap(true);
            algorithm_test_preview_ =
                make_child<QLabel>(test_group, QStringLiteral("测试后显示当前图像"));
            algorithm_test_preview_->setObjectName(QStringLiteral("algorithm-test-preview"));
            algorithm_test_preview_->setAlignment(Qt::AlignCenter);
            algorithm_test_preview_->setMinimumHeight(240);
            algorithm_debug_metrics_ = make_child<QTableWidget>(test_group);
            algorithm_debug_metrics_->setObjectName(QStringLiteral("algorithm-debug-metrics"));
            configure_table(algorithm_debug_metrics_,
                            {QStringLiteral("调试指标"), QStringLiteral("值")});
            test_layout->addWidget(algorithm_test_result_);
            test_layout->addWidget(algorithm_test_preview_);
            test_layout->addWidget(algorithm_debug_metrics_);
            content_layout->addWidget(test_group);

            algorithm_operation_status_ =
                make_child<QLabel>(content, QStringLiteral("等待后台服务连接"));
            algorithm_operation_status_->setWordWrap(true);
            content_layout->addWidget(algorithm_operation_status_);
            content_layout->addStretch(1);
            scroll->setWidget(content);
            layout->addWidget(scroll, 1);

            QObject::connect(algorithm_camera_selector_, &QComboBox::currentTextChanged, this,
                             [this](const QString& camera_id) {
                                 if (algorithm_actions_.select_camera)
                                     show_algorithm_result(
                                         algorithm_actions_.select_camera(camera_id.toStdString()));
                             });
            QObject::connect(refresh, &QPushButton::clicked, this, [this] {
                if (algorithm_actions_.refresh)
                    algorithm_actions_.refresh();
            });
            QObject::connect(algorithm_save_, &QPushButton::clicked, this, [this] {
                if (!algorithm_actions_.update_configuration)
                    return;
                show_algorithm_result(algorithm_actions_.update_configuration(
                    {.enabled = algorithm_enabled_->isChecked(),
                     .type = algorithm_type_->currentData().toString().toStdString(),
                     .roi = {.width = static_cast<std::uint32_t>(algorithm_roi_width_->value()),
                             .height = static_cast<std::uint32_t>(algorithm_roi_height_->value()),
                             .offset_x = static_cast<std::uint32_t>(algorithm_roi_x_->value()),
                             .offset_y = static_cast<std::uint32_t>(algorithm_roi_y_->value())},
                     .candidate_threshold = algorithm_candidate_threshold_->value(),
                     .confirmation_threshold = algorithm_confirmation_threshold_->value(),
                     .consecutive_frames =
                         static_cast<std::uint32_t>(algorithm_consecutive_frames_->value()),
                     .cooldown_ms = static_cast<std::uint32_t>(algorithm_cooldown_ms_->value()),
                     .model_reference = algorithm_model_reference_->text().toStdString(),
                     .model_version = algorithm_model_version_->text().toStdString(),
                     .device = algorithm_device_->currentText().toStdString(),
                     .debug_overlay = algorithm_debug_overlay_->isChecked()}));
            });
            QObject::connect(algorithm_test_, &QPushButton::clicked, this, [this] {
                if (algorithm_actions_.test_current_frame)
                    show_algorithm_result(algorithm_actions_.test_current_frame());
            });
            pages_->addWidget(page);
            continue;
        }
        if (descriptor.id == ConsolePageId::event_configuration)
        {
            QWidget* page = make_child<QWidget>(pages_);
            page->setObjectName(QStringLiteral("page-event-configuration"));
            auto* layout = make_layout<QVBoxLayout>(page);
            layout->setContentsMargins(24, 20, 24, 20);
            auto* heading = make_child<QLabel>(page, QStringLiteral("事件窗口与保存策略"));
            heading->setProperty("role", "pageTitle");
            layout->addWidget(heading);
            event_config_editor_ = make_child<QWidget>(page);
            auto* form = make_layout<QFormLayout>(event_config_editor_);
            event_pre_seconds_ = make_child<QSpinBox>(event_config_editor_);
            event_post_seconds_ = make_child<QSpinBox>(event_config_editor_);
            event_max_seconds_ = make_child<QSpinBox>(event_config_editor_);
            event_merge_seconds_ = make_child<QSpinBox>(event_config_editor_);
            event_key_frames_ = make_child<QSpinBox>(event_config_editor_);
            event_retention_days_ = make_child<QSpinBox>(event_config_editor_);
            event_pre_seconds_->setRange(0, 600);
            event_post_seconds_->setRange(0, 600);
            event_max_seconds_->setRange(1, 3600);
            event_merge_seconds_->setRange(0, 3600);
            event_key_frames_->setRange(1, 32);
            event_retention_days_->setRange(1, 3650);
            event_save_raw_ =
                make_child<QCheckBox>(event_config_editor_, QStringLiteral("保存原始帧"));
            event_preview_video_ =
                make_child<QCheckBox>(event_config_editor_, QStringLiteral("生成预览视频"));
            event_upload_policy_ = make_child<QComboBox>(event_config_editor_);
            event_upload_policy_->setObjectName(QStringLiteral("event-upload-policy"));
            event_upload_policy_->addItem(QStringLiteral("仅已确认"), QStringLiteral("confirmed"));
            event_upload_policy_->addItem(QStringLiteral("全部事件"), QStringLiteral("all"));
            event_upload_policy_->addItem(QStringLiteral("不上传"), QStringLiteral("never"));
            form->addRow(QStringLiteral("前置秒数"), event_pre_seconds_);
            form->addRow(QStringLiteral("后置秒数"), event_post_seconds_);
            form->addRow(QStringLiteral("最大事件时长（秒）"), event_max_seconds_);
            form->addRow(QStringLiteral("合并间隔（秒）"), event_merge_seconds_);
            form->addRow(QStringLiteral("关键帧数"), event_key_frames_);
            form->addRow(QStringLiteral("原始数据"), event_save_raw_);
            form->addRow(QStringLiteral("预览视频"), event_preview_video_);
            form->addRow(QStringLiteral("上传策略"), event_upload_policy_);
            form->addRow(QStringLiteral("保留天数"), event_retention_days_);
            layout->addWidget(event_config_editor_);
            event_config_save_ = make_child<QPushButton>(page, QStringLiteral("保存事件配置"));
            event_config_save_->setObjectName(QStringLiteral("event-config-save"));
            event_config_editor_->setEnabled(false);
            event_config_save_->setEnabled(false);
            QObject::connect(event_config_save_, &QPushButton::clicked, this, [this] {
                if (!event_actions_.update_configuration)
                    return;
                show_event_config_result(event_actions_.update_configuration(
                    {.pre_event_seconds = static_cast<std::uint32_t>(event_pre_seconds_->value()),
                     .post_event_seconds = static_cast<std::uint32_t>(event_post_seconds_->value()),
                     .max_event_seconds = static_cast<std::uint32_t>(event_max_seconds_->value()),
                     .merge_gap_seconds = static_cast<std::uint32_t>(event_merge_seconds_->value()),
                     .key_frame_count = static_cast<std::uint32_t>(event_key_frames_->value()),
                     .save_raw = event_save_raw_->isChecked(),
                     .generate_preview_video = event_preview_video_->isChecked(),
                     .upload_policy = event_upload_policy_->currentData().toString().toStdString(),
                     .retention_days =
                         static_cast<std::uint32_t>(event_retention_days_->value())}));
            });
            layout->addWidget(event_config_save_);
            event_config_status_ = make_child<QLabel>(page, QStringLiteral("正在读取事件配置"));
            event_config_status_->setObjectName(QStringLiteral("event-config-status"));
            event_config_status_->setWordWrap(true);
            layout->addWidget(event_config_status_);
            layout->addStretch(1);
            pages_->addWidget(page);
            continue;
        }
        if (descriptor.id == ConsolePageId::uplink_configuration)
        {
            QWidget* page = make_child<QWidget>(pages_);
            page->setObjectName(QStringLiteral("page-uplink-configuration"));
            auto* layout = make_layout<QVBoxLayout>(page);
            layout->setContentsMargins(24, 20, 24, 20);
            layout->setSpacing(12);
            auto* heading = make_child<QLabel>(page, QStringLiteral("上位机连接与上传传输"));
            heading->setProperty("role", "pageTitle");
            layout->addWidget(heading);
            auto* notice = make_child<QLabel>(
                page, QStringLiteral(
                          "Uplink v1 使用隔离 VLAN 内的明文 HTTP/WebSocket，不提供 TLS 或应用鉴权。"
                          "本页参数保存后需要重启后台服务，正在运行的连接不会被即时替换。"));
            notice->setObjectName(QStringLiteral("uplink-restart-notice"));
            notice->setWordWrap(true);
            notice->setProperty("role", "warning");
            layout->addWidget(notice);

            uplink_editor_ = make_child<QWidget>(page);
            uplink_editor_->setObjectName(QStringLiteral("uplink-editor"));
            auto* form = make_layout<QFormLayout>(uplink_editor_);
            uplink_enabled_ = make_child<QCheckBox>(uplink_editor_, QStringLiteral("启用 Uplink"));
            uplink_enabled_->setObjectName(QStringLiteral("uplink-enabled"));
            uplink_server_url_ = make_child<QLineEdit>(uplink_editor_);
            uplink_server_url_->setObjectName(QStringLiteral("uplink-server-url"));
            uplink_server_url_->setMaxLength(2048);
            uplink_server_url_->setPlaceholderText(QStringLiteral("http://192.0.2.10:18080"));
            uplink_heartbeat_seconds_ = make_child<QSpinBox>(uplink_editor_);
            uplink_heartbeat_seconds_->setRange(1, 3600);
            uplink_chunk_kib_ = make_child<QSpinBox>(uplink_editor_);
            uplink_chunk_kib_->setRange(64, 4096);
            uplink_io_timeout_ms_ = make_child<QSpinBox>(uplink_editor_);
            uplink_io_timeout_ms_->setRange(100, 60000);
            uplink_upload_limit_mibps_ = make_child<QSpinBox>(uplink_editor_);
            uplink_upload_limit_mibps_->setRange(1, 1024);
            form->addRow(QStringLiteral("运行状态（需重启）"), uplink_enabled_);
            form->addRow(QStringLiteral("上位机 HTTP 基址（需重启）"), uplink_server_url_);
            form->addRow(QStringLiteral("心跳间隔 (s，需重启)"), uplink_heartbeat_seconds_);
            form->addRow(QStringLiteral("上传分块 (KiB，需重启)"), uplink_chunk_kib_);
            form->addRow(QStringLiteral("网络 I/O 截止 (ms，需重启)"), uplink_io_timeout_ms_);
            form->addRow(QStringLiteral("上传限速 (MiB/s，需重启)"), uplink_upload_limit_mibps_);
            uplink_editor_->setEnabled(false);
            layout->addWidget(uplink_editor_);

            uplink_save_ = make_child<QPushButton>(page, QStringLiteral("保存上位机配置"));
            uplink_save_->setObjectName(QStringLiteral("uplink-config-save"));
            uplink_save_->setEnabled(false);
            QObject::connect(uplink_save_, &QPushButton::clicked, this, [this] {
                if (!uplink_actions_.update_configuration)
                    return;
                const QString url = uplink_server_url_->text().trimmed();
                if (uplink_enabled_->isChecked() && !url.startsWith(QStringLiteral("http://")))
                {
                    QMessageBox::warning(this, QStringLiteral("上位机地址无效"),
                                         QStringLiteral("启用 Uplink 时基址必须以 http:// 开头。"));
                    return;
                }
                show_uplink_result(uplink_actions_.update_configuration(
                    {.enabled = uplink_enabled_->isChecked(),
                     .server_url = url.toStdString(),
                     .heartbeat_seconds =
                         static_cast<std::uint32_t>(uplink_heartbeat_seconds_->value()),
                     .chunk_bytes = static_cast<std::uint32_t>(uplink_chunk_kib_->value()) * 1024U,
                     .io_timeout_ms = static_cast<std::uint32_t>(uplink_io_timeout_ms_->value()),
                     .upload_limit_mibps =
                         static_cast<std::uint32_t>(uplink_upload_limit_mibps_->value()),
                     .credential_reference = uplink_snapshot_.configuration.credential_reference,
                     .certificate_reference =
                         uplink_snapshot_.configuration.certificate_reference}));
            });
            layout->addWidget(uplink_save_);
            uplink_status_ = make_child<QLabel>(page, QStringLiteral("正在读取上位机配置"));
            uplink_status_->setObjectName(QStringLiteral("uplink-config-status"));
            uplink_status_->setWordWrap(true);
            uplink_status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
            layout->addWidget(uplink_status_);
            layout->addStretch(1);
            pages_->addWidget(page);
            continue;
        }
        if (descriptor.id == ConsolePageId::storage_configuration)
        {
            QWidget* page = make_child<QWidget>(pages_);
            page->setObjectName(QStringLiteral("page-storage-configuration"));
            auto* layout = make_layout<QVBoxLayout>(page);
            layout->setContentsMargins(24, 20, 24, 20);
            layout->setSpacing(12);
            auto* heading = make_child<QLabel>(page, QStringLiteral("事件存储与 NVMe 滚动缓存"));
            heading->setProperty("role", "pageTitle");
            layout->addWidget(heading);
            auto* notice = make_child<QLabel>(
                page, QStringLiteral(
                          "事件根目录、缓存根目录及 NVMe 滚动缓存参数保存后需要重启后台服务；"
                          "磁盘水位与事件容量上限会立即应用。启用 NVMe 前须完成目标盘带宽验收。"));
            notice->setObjectName(QStringLiteral("storage-restart-notice"));
            notice->setWordWrap(true);
            notice->setProperty("role", "warning");
            layout->addWidget(notice);

            auto* scroll = make_child<QScrollArea>(page);
            scroll->setWidgetResizable(true);
            QWidget* content = make_child<QWidget>(scroll);
            auto* content_layout = make_layout<QVBoxLayout>(content);
            content_layout->setContentsMargins(2, 2, 8, 2);
            content_layout->setSpacing(12);

            storage_editor_ = make_child<QWidget>(content);
            storage_editor_->setObjectName(QStringLiteral("storage-editor"));
            auto* form = make_layout<QFormLayout>(storage_editor_);
            storage_event_root_ = make_child<QLineEdit>(storage_editor_);
            storage_cache_root_ = make_child<QLineEdit>(storage_editor_);
            storage_event_root_->setMaxLength(1024);
            storage_cache_root_->setMaxLength(1024);
            storage_event_root_->setObjectName(QStringLiteral("storage-event-root"));
            storage_cache_root_->setObjectName(QStringLiteral("storage-cache-root"));
            storage_rolling_cache_enabled_ =
                make_child<QCheckBox>(storage_editor_, QStringLiteral("启用普通滚动缓存"));
            storage_rolling_cache_enabled_->setObjectName(
                QStringLiteral("storage-rolling-cache-enabled"));
            storage_maximum_cache_gib_ = make_child<QSpinBox>(storage_editor_);
            storage_write_limit_mibps_ = make_child<QSpinBox>(storage_editor_);
            storage_io_timeout_ms_ = make_child<QSpinBox>(storage_editor_);
            storage_warning_gib_ = make_child<QSpinBox>(storage_editor_);
            storage_critical_gib_ = make_child<QSpinBox>(storage_editor_);
            storage_stop_gib_ = make_child<QSpinBox>(storage_editor_);
            storage_maximum_event_gib_ = make_child<QSpinBox>(storage_editor_);
            storage_maximum_cache_gib_->setRange(1, 1000000);
            storage_write_limit_mibps_->setRange(1, 1000000);
            storage_io_timeout_ms_->setRange(100, 600000);
            storage_warning_gib_->setRange(1, 1000000);
            storage_critical_gib_->setRange(1, 1000000);
            storage_stop_gib_->setRange(1, 1000000);
            storage_maximum_event_gib_->setRange(1, 1000000);
            form->addRow(QStringLiteral("事件根目录（需重启）"), storage_event_root_);
            form->addRow(QStringLiteral("NVMe 缓存根目录（需重启）"), storage_cache_root_);
            form->addRow(QStringLiteral("滚动缓存（需重启）"), storage_rolling_cache_enabled_);
            form->addRow(QStringLiteral("滚动缓存容量上限 (GiB，需重启)"),
                         storage_maximum_cache_gib_);
            form->addRow(QStringLiteral("滚动写限速 (MiB/s，需重启)"), storage_write_limit_mibps_);
            form->addRow(QStringLiteral("单块 I/O 截止时间 (ms，需重启)"), storage_io_timeout_ms_);
            form->addRow(QStringLiteral("预警剩余空间 (GiB)"), storage_warning_gib_);
            form->addRow(QStringLiteral("严重剩余空间 (GiB)"), storage_critical_gib_);
            form->addRow(QStringLiteral("停止保存剩余空间 (GiB)"), storage_stop_gib_);
            form->addRow(QStringLiteral("事件容量上限 (GiB)"), storage_maximum_event_gib_);
            storage_editor_->setEnabled(false);
            content_layout->addWidget(storage_editor_);

            storage_save_ = make_child<QPushButton>(content, QStringLiteral("保存存储配置"));
            storage_save_->setObjectName(QStringLiteral("storage-config-save"));
            storage_save_->setEnabled(false);
            QObject::connect(storage_save_, &QPushButton::clicked, this, [this] {
                if (!storage_actions_.update_configuration)
                    return;
                if (storage_warning_gib_->value() <= storage_critical_gib_->value() ||
                    storage_critical_gib_->value() <= storage_stop_gib_->value())
                {
                    QMessageBox::warning(
                        this, QStringLiteral("存储水位无效"),
                        QStringLiteral("必须满足：预警水位 > 严重水位 > 停止保存水位。"));
                    return;
                }
                show_storage_result(storage_actions_.update_configuration(
                    {.event_root = storage_event_root_->text().trimmed().toStdString(),
                     .cache_root = storage_cache_root_->text().trimmed().toStdString(),
                     .rolling_cache_enabled = storage_rolling_cache_enabled_->isChecked(),
                     .maximum_cache_storage_gib =
                         static_cast<std::uint32_t>(storage_maximum_cache_gib_->value()),
                     .rolling_cache_write_limit_mibps =
                         static_cast<std::uint32_t>(storage_write_limit_mibps_->value()),
                     .rolling_cache_io_timeout_ms =
                         static_cast<std::uint32_t>(storage_io_timeout_ms_->value()),
                     .warning_free_space_gib =
                         static_cast<std::uint32_t>(storage_warning_gib_->value()),
                     .critical_free_space_gib =
                         static_cast<std::uint32_t>(storage_critical_gib_->value()),
                     .stop_free_space_gib = static_cast<std::uint32_t>(storage_stop_gib_->value()),
                     .maximum_event_storage_gib =
                         static_cast<std::uint32_t>(storage_maximum_event_gib_->value())}));
            });
            content_layout->addWidget(storage_save_);
            storage_status_ = make_child<QLabel>(content, QStringLiteral("正在读取存储配置"));
            storage_status_->setObjectName(QStringLiteral("storage-config-status"));
            storage_status_->setWordWrap(true);
            storage_status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
            content_layout->addWidget(storage_status_);

            auto* runtime_group =
                make_child<QGroupBox>(content, QStringLiteral("实际存储与 NVMe 运行指标"));
            auto* runtime_layout = make_layout<QVBoxLayout>(runtime_group);
            storage_metrics_ = make_child<QTableWidget>(runtime_group);
            storage_metrics_->setObjectName(QStringLiteral("storage-runtime-metrics"));
            configure_table(storage_metrics_, {QStringLiteral("指标"), QStringLiteral("值"),
                                               QStringLiteral("单位"), QStringLiteral("状态")});
            runtime_layout->addWidget(storage_metrics_);
            content_layout->addWidget(runtime_group);
            content_layout->addStretch(1);
            scroll->setWidget(content);
            layout->addWidget(scroll, 1);
            pages_->addWidget(page);
            continue;
        }
        if (descriptor.id == ConsolePageId::events)
        {
            QWidget* page = make_child<QWidget>(pages_);
            page->setObjectName(QStringLiteral("page-events"));
            auto* layout = make_layout<QVBoxLayout>(page);
            layout->setContentsMargins(24, 20, 24, 20);
            auto* heading = make_child<QLabel>(page, QStringLiteral("事件查询、校验与复核"));
            heading->setProperty("role", "pageTitle");
            layout->addWidget(heading);
            auto* filters = make_child<QWidget>(page);
            auto* filter_layout = make_layout<QHBoxLayout>(filters);
            filter_layout->setContentsMargins(0, 0, 0, 0);
            event_filter_start_ = make_child<QDateTimeEdit>(filters);
            event_filter_start_->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            event_filter_start_->setCalendarPopup(true);
            event_filter_start_->setDateTime(QDateTime::currentDateTime().addDays(-1));
            event_filter_end_ = make_child<QDateTimeEdit>(filters);
            event_filter_end_->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            event_filter_end_->setCalendarPopup(true);
            event_filter_end_->setDateTime(QDateTime::currentDateTime());
            event_filter_state_ = make_child<QComboBox>(filters);
            event_filter_state_->addItem(QStringLiteral("全部状态"), QString{});
            event_filter_state_->addItem(QStringLiteral("候选"), QStringLiteral("Candidate"));
            event_filter_state_->addItem(QStringLiteral("已确认"), QStringLiteral("Confirmed"));
            event_filter_state_->addItem(QStringLiteral("误报"), QStringLiteral("Rejected"));
            event_filter_camera_ = make_child<QLineEdit>(filters);
            event_filter_camera_->setPlaceholderText(QStringLiteral("相机 ID（可留空）"));
            event_filter_camera_->setMaxLength(32);
            auto* apply = make_child<QPushButton>(filters, QStringLiteral("查询"));
            apply->setObjectName(QStringLiteral("event-filter-apply"));
            QObject::connect(apply, &QPushButton::clicked, this, [this] {
                if (!event_actions_.query)
                    return;
                EventListFilter filter{
                    .start_time_utc_ms = event_filter_start_->dateTime().toMSecsSinceEpoch(),
                    .end_time_utc_ms = event_filter_end_->dateTime().toMSecsSinceEpoch(),
                    .offset = 0U,
                    .limit = 50U};
                if (!event_filter_state_->currentData().toString().isEmpty())
                    filter.event_state =
                        event_filter_state_->currentData().toString().toStdString();
                if (!event_filter_camera_->text().trimmed().isEmpty())
                    filter.camera_id = event_filter_camera_->text().trimmed().toStdString();
                show_event_result(event_actions_.query(std::move(filter)));
            });
            auto* manual = make_child<QPushButton>(filters, QStringLiteral("人工触发"));
            manual->setObjectName(QStringLiteral("event-manual-trigger"));
            QObject::connect(manual, &QPushButton::clicked, this, [this] {
                if (event_actions_.manual_trigger &&
                    !event_filter_camera_->text().trimmed().isEmpty())
                    show_event_result(event_actions_.manual_trigger(
                        event_filter_camera_->text().trimmed().toStdString()));
            });
            filter_layout->addWidget(event_filter_start_);
            filter_layout->addWidget(event_filter_end_);
            filter_layout->addWidget(event_filter_state_);
            filter_layout->addWidget(event_filter_camera_);
            filter_layout->addWidget(apply);
            filter_layout->addWidget(manual);
            layout->addWidget(filters);
            event_table_ = make_child<QTableWidget>(page);
            event_table_->setObjectName(QStringLiteral("event-table"));
            configure_table(event_table_, {QStringLiteral("时间"), QStringLiteral("状态"),
                                           QStringLiteral("触发相机"), QStringLiteral("置信度"),
                                           QStringLiteral("上传状态"), QStringLiteral("缩略图"),
                                           QStringLiteral("事件 ID")});
            QObject::connect(event_table_, &QTableWidget::itemSelectionChanged, this, [this] {
                const int row = event_table_->currentRow();
                if (row >= 0 && event_actions_.get && event_table_->item(row, 6))
                    show_event_result(
                        event_actions_.get(event_table_->item(row, 6)->text().toStdString()));
            });
            layout->addWidget(event_table_, 1);
            auto* paging = make_child<QWidget>(page);
            auto* paging_layout = make_layout<QHBoxLayout>(paging);
            paging_layout->setContentsMargins(0, 0, 0, 0);
            event_previous_ = make_child<QPushButton>(paging, QStringLiteral("上一页"));
            event_next_ = make_child<QPushButton>(paging, QStringLiteral("下一页"));
            const auto page_change = [this](const bool forward) {
                if (!event_actions_.query)
                    return;
                auto filter = event_snapshot_.filter;
                filter.offset =
                    forward ? filter.offset + filter.limit
                            : (filter.offset > filter.limit ? filter.offset - filter.limit : 0U);
                show_event_result(event_actions_.query(std::move(filter)));
            };
            QObject::connect(event_previous_, &QPushButton::clicked, this,
                             [page_change] { page_change(false); });
            QObject::connect(event_next_, &QPushButton::clicked, this,
                             [page_change] { page_change(true); });
            paging_layout->addWidget(event_previous_);
            paging_layout->addWidget(event_next_);
            paging_layout->addStretch(1);
            layout->addWidget(paging);
            auto* details = make_child<QWidget>(page);
            auto* details_layout = make_layout<QHBoxLayout>(details);
            event_thumbnail_ = make_child<QLabel>(details, QStringLiteral("选择事件加载缩略图"));
            event_thumbnail_->setMinimumSize(240, 140);
            event_thumbnail_->setAlignment(Qt::AlignCenter);
            event_thumbnail_->setScaledContents(true);
            event_manifest_ = make_child<QTextEdit>(details);
            event_manifest_->setReadOnly(true);
            event_manifest_->setPlaceholderText(QStringLiteral("选择事件查看已校验 manifest"));
            details_layout->addWidget(event_thumbnail_);
            details_layout->addWidget(event_manifest_, 1);
            layout->addWidget(details);
            auto* actions = make_child<QWidget>(page);
            auto* action_layout = make_layout<QHBoxLayout>(actions);
            action_layout->setContentsMargins(0, 0, 0, 0);
            event_confirm_ = make_child<QPushButton>(actions, QStringLiteral("确认为断纸"));
            event_reject_ = make_child<QPushButton>(actions, QStringLiteral("标记误报"));
            event_export_ = make_child<QPushButton>(actions, QStringLiteral("导出已校验事件"));
            event_open_directory_ =
                make_child<QPushButton>(actions, QStringLiteral("打开事件目录"));
            event_retry_upload_ = make_child<QPushButton>(actions, QStringLiteral("重试上传"));
            event_retry_upload_->setEnabled(false);
            event_retry_upload_->setToolTip(
                QStringLiteral("将所选事件的既有上传任务恢复为待处理，不创建重复任务"));
            const auto review = [this](const bool confirmed) {
                if (event_actions_.review && event_snapshot_.detail)
                    show_event_result(event_actions_.review(
                        event_snapshot_.detail->event.event_id,
                        event_snapshot_.detail->event.review_revision, confirmed));
            };
            QObject::connect(event_confirm_, &QPushButton::clicked, this,
                             [review] { review(true); });
            QObject::connect(event_reject_, &QPushButton::clicked, this,
                             [review] { review(false); });
            QObject::connect(event_export_, &QPushButton::clicked, this, [this] {
                if (!event_actions_.export_event || !event_snapshot_.detail)
                    return;
                const QString path = QFileDialog::getSaveFileName(
                    this, QStringLiteral("导出事件"),
                    QString::fromStdString(event_snapshot_.detail->event.event_id + ".zip"),
                    QStringLiteral("ZIP 文件 (*.zip)"));
                if (!path.isEmpty())
                    show_event_result(
                        event_actions_.export_event(event_snapshot_.detail->event.event_id,
                                                    std::filesystem::path{path.toStdWString()}));
            });
            QObject::connect(event_open_directory_, &QPushButton::clicked, this, [this] {
                if (event_snapshot_.detail)
                    static_cast<void>(
                        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(
                            event_snapshot_.detail->committed_directory.wstring()))));
            });
            QObject::connect(event_retry_upload_, &QPushButton::clicked, this, [this] {
                if (event_actions_.retry_upload && event_snapshot_.detail)
                    show_event_result(
                        event_actions_.retry_upload(event_snapshot_.detail->event.event_id));
            });
            action_layout->addWidget(event_confirm_);
            action_layout->addWidget(event_reject_);
            action_layout->addWidget(event_export_);
            action_layout->addWidget(event_open_directory_);
            action_layout->addWidget(event_retry_upload_);
            action_layout->addStretch(1);
            layout->addWidget(actions);
            event_status_ = make_child<QLabel>(page, QStringLiteral("正在读取事件列表"));
            event_status_->setWordWrap(true);
            layout->addWidget(event_status_);
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
                page, QStringLiteral(
                          "指标来自后台服务有界快照；未初始化或设备不支持的来源显示为不可用。"));
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
    for (std::size_t index = 0; index < overview_camera_states_.size(); ++index)
    {
        const std::string id = "CAM0" + std::to_string(index + 1U);
        const auto found = std::ranges::find_if(
            snapshot.cameras, [&id](const auto& camera) { return camera.id == id; });
        const QString state = found == snapshot.cameras.end()
                                  ? QStringLiteral("未配置")
                                  : QString::fromStdString(found->state);
        overview_camera_states_[index]->setText(stale_value(state, snapshot.stale));
    }
    if (!camera_configuration_value_ || !camera_selector_ || !discovered_devices_)
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

void MainWindow::show_algorithm_result(const Result<void>& result)
{
    if (!result && algorithm_operation_status_)
        algorithm_operation_status_->setText(
            QStringLiteral("失败：%1（%2）")
                .arg(QString::fromStdString(result.error().message),
                     QString::fromStdString(result.error().business_code)));
}

void MainWindow::show_event_result(const Result<void>& result)
{
    if (!result && event_status_)
        event_status_->setText(
            QStringLiteral("失败：%1").arg(QString::fromStdString(result.error().message)));
}

void MainWindow::show_event_config_result(const Result<void>& result)
{
    if (!result && event_config_status_)
        event_config_status_->setText(
            QStringLiteral("保存失败：%1（%2）")
                .arg(QString::fromStdString(result.error().message),
                     QString::fromStdString(result.error().business_code)));
}

void MainWindow::show_storage_result(const Result<void>& result)
{
    if (!result && storage_status_)
        storage_status_->setText(QStringLiteral("保存失败：%1（%2）")
                                     .arg(QString::fromStdString(result.error().message),
                                          QString::fromStdString(result.error().business_code)));
}

void MainWindow::show_uplink_result(const Result<void>& result)
{
    if (!result && uplink_status_)
        uplink_status_->setText(QStringLiteral("保存失败：%1（%2）")
                                    .arg(QString::fromStdString(result.error().message),
                                         QString::fromStdString(result.error().business_code)));
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
    for (std::size_t camera = 0; camera < overview_camera_fps_.size(); ++camera)
    {
        const std::string prefix = "camera.CAM0" + std::to_string(camera + 1U) + '.';
        const auto find_metric = [&](const std::string_view suffix) {
            return std::ranges::find_if(snapshot.metrics, [&](const auto& metric) {
                return metric.name == prefix + std::string{suffix};
            });
        };
        const auto fps = find_metric("actual_fps");
        const auto brightness = find_metric("brightness");
        const auto last_frame = find_metric("last_frame_epoch_ms");
        overview_camera_fps_[camera]->setText(
            fps != snapshot.metrics.end() && fps->available
                ? stale_value(QStringLiteral("%1 fps").arg(QString::fromStdString(fps->value)),
                              snapshot.metrics_stale)
                : QStringLiteral("不可用"));
        overview_camera_brightness_[camera]->setText(
            brightness != snapshot.metrics.end() && brightness->available
                ? stale_value(QString::fromStdString(brightness->value), snapshot.metrics_stale)
                : QStringLiteral("不可用"));
        overview_camera_last_frames_[camera]->setText(
            last_frame != snapshot.metrics.end() && last_frame->available
                ? stale_value(local_epoch_milliseconds_text(last_frame->value),
                              snapshot.metrics_stale)
                : QStringLiteral("不可用"));
    }
    metrics_table_->resizeColumnsToContents();

    if (storage_metrics_)
    {
        const auto storage_metric = [](const std::string_view name) {
            return name.starts_with("storage.") || name == "system.nvme_write_bytes_per_second" ||
                   name.starts_with("disk.event.") || name.starts_with("disk.cache.");
        };
        const auto count = std::ranges::count_if(
            snapshot.metrics, [&](const auto& metric) { return storage_metric(metric.name); });
        storage_metrics_->setRowCount(static_cast<int>(count));
        int storage_row = 0;
        for (const auto& metric : snapshot.metrics)
        {
            if (!storage_metric(metric.name))
                continue;
            set_table_item(storage_metrics_, storage_row, 0, QString::fromStdString(metric.name));
            set_table_item(storage_metrics_, storage_row, 1,
                           metric.available ? QString::fromStdString(metric.value)
                                            : QStringLiteral("不可用"));
            set_table_item(storage_metrics_, storage_row, 2, QString::fromStdString(metric.unit));
            set_table_item(storage_metrics_, storage_row, 3,
                           stale_value(metric.available ? QStringLiteral("可用")
                                                        : QStringLiteral("未初始化/不可用"),
                                       snapshot.metrics_stale));
            ++storage_row;
        }
        storage_metrics_->resizeColumnsToContents();
    }

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
        set_table_item(log_table_, row, 2,
                       QStringLiteral("%1（%2）")
                           .arg(QString::fromStdString(record.thread_name),
                                QString::number(record.thread_id)));
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

void MainWindow::apply_algorithm_snapshot(const AlgorithmClientSnapshot& snapshot)
{
    const bool configuration_changed =
        algorithm_snapshot_.camera_id != snapshot.camera_id ||
        algorithm_snapshot_.stored_config_revision != snapshot.stored_config_revision ||
        algorithm_snapshot_.stale;
    algorithm_snapshot_ = snapshot;
    if (overview_detector_value_ && overview_candidate_value_)
    {
        if (snapshot.stale)
        {
            overview_detector_value_->setText(QStringLiteral("检测器：不可用（已过期）"));
            overview_candidate_value_->setText(QStringLiteral("累计候选事件：不可用（已过期）"));
        }
        else
        {
            const QString detector = snapshot.runtime.display_name.empty()
                                         ? QString::fromStdString(snapshot.runtime.state)
                                         : QString::fromStdString(snapshot.runtime.display_name);
            overview_detector_value_->setText(
                QStringLiteral("检测器：%1 · %2")
                    .arg(detector, QString::fromStdString(snapshot.runtime.state)));
            overview_candidate_value_->setText(QStringLiteral("累计候选事件：%1 · 已确认 %2")
                                                   .arg(snapshot.runtime.metrics.candidates_created)
                                                   .arg(snapshot.runtime.metrics.confirmed_events));
        }
    }
    if (!algorithm_editor_ || !algorithm_runtime_status_ || !algorithm_metrics_)
        return;

    const QString selected = QString::fromStdString(snapshot.camera_id);
    if (algorithm_camera_selector_->currentText() != selected)
    {
        algorithm_camera_selector_->blockSignals(true);
        algorithm_camera_selector_->setCurrentText(selected);
        algorithm_camera_selector_->blockSignals(false);
    }
    if (configuration_changed && !snapshot.stale)
    {
        const auto& value = snapshot.configuration;
        algorithm_enabled_->setChecked(value.enabled);
        int type = algorithm_type_->findData(QString::fromStdString(value.type));
        if (type < 0)
        {
            algorithm_type_->addItem(QString::fromStdString(value.type),
                                     QString::fromStdString(value.type));
            type = algorithm_type_->count() - 1;
        }
        algorithm_type_->setCurrentIndex(type);
        algorithm_roi_width_->setValue(static_cast<int>(value.roi.width));
        algorithm_roi_height_->setValue(static_cast<int>(value.roi.height));
        algorithm_roi_x_->setValue(static_cast<int>(value.roi.offset_x));
        algorithm_roi_y_->setValue(static_cast<int>(value.roi.offset_y));
        algorithm_candidate_threshold_->setValue(value.candidate_threshold);
        algorithm_confirmation_threshold_->setValue(value.confirmation_threshold);
        algorithm_consecutive_frames_->setValue(static_cast<int>(value.consecutive_frames));
        algorithm_cooldown_ms_->setValue(static_cast<int>(value.cooldown_ms));
        algorithm_model_reference_->setText(QString::fromStdString(value.model_reference));
        algorithm_model_version_->setText(QString::fromStdString(value.model_version));
        algorithm_device_->setEditText(QString::fromStdString(value.device));
        algorithm_debug_overlay_->setChecked(value.debug_overlay);
    }

    const bool connected = snapshot.connection.state == ipc::ClientConnectionState::connected;
    const bool editable = connected && !snapshot.stale && !snapshot.operation_pending;
    algorithm_camera_selector_->setEnabled(connected && !snapshot.operation_pending);
    algorithm_editor_->setEnabled(editable);
    algorithm_save_->setEnabled(editable);
    algorithm_test_->setEnabled(editable && snapshot.runtime.has_current_frame);

    if (snapshot.stale)
        algorithm_runtime_status_->setText(
            QStringLiteral("算法配置或实际状态不可用，旧值已标记过期"));
    else
    {
        const auto& runtime = snapshot.runtime;
        const QString prototype =
            runtime.prototype_only ? QStringLiteral("是") : QStringLiteral("否");
        QString application_state = QStringLiteral("已保存");
        if (snapshot.stored_config_revision == snapshot.effective_config_revision)
            application_state += QStringLiteral(" · 已下发");
        if (runtime.config_revision == snapshot.effective_config_revision)
            application_state += QStringLiteral(" · 已应用 · 无需重启");
        else
            application_state += QStringLiteral(" · 运行时修订尚未一致");
        algorithm_runtime_status_->setText(
            QStringLiteral("%1\n实际状态：%2 · 相机 %3 · 生效修订 %4（保存修订 %5）\n"
                           "插件：%6 / %7 · 实现版本 %8 · 检测器模型版本 %9 · 配置设备 %10\n"
                           "仅原型：%11 · 热更新：%12 · 当前帧：%13（序号 %14）")
                .arg(application_state, QString::fromStdString(runtime.state),
                     QString::fromStdString(runtime.camera_id))
                .arg(snapshot.effective_config_revision)
                .arg(snapshot.stored_config_revision)
                .arg(QString::fromStdString(runtime.plugin_id),
                     QString::fromStdString(runtime.display_name),
                     QString::fromStdString(runtime.implementation_version),
                     QString::fromStdString(runtime.detector_model_version),
                     QString::fromStdString(snapshot.effective_configuration.device), prototype,
                     runtime.supports_hot_update ? QStringLiteral("支持")
                                                 : QStringLiteral("不支持"),
                     runtime.has_current_frame ? QStringLiteral("可测试") : QStringLiteral("无"))
                .arg(runtime.latest_sequence_number));
    }

    const auto& metrics = snapshot.runtime.metrics;
    const std::array metric_rows{
        std::tuple{QStringLiteral("算法队列深度"), QString::number(metrics.queue_depth),
                   QStringLiteral("帧")},
        std::tuple{QStringLiteral("算法队列容量"), QString::number(metrics.queue_capacity),
                   QStringLiteral("帧")},
        std::tuple{QStringLiteral("队列高水位"), QString::number(metrics.queue_high_watermark),
                   QStringLiteral("帧")},
        std::tuple{QStringLiteral("已提交帧"), QString::number(metrics.submitted_frames),
                   QStringLiteral("帧")},
        std::tuple{QStringLiteral("已处理帧"), QString::number(metrics.processed_frames),
                   QStringLiteral("帧")},
        std::tuple{QStringLiteral("跳帧"), QString::number(metrics.skipped_frames),
                   QStringLiteral("帧")},
        std::tuple{QStringLiteral("检测失败"), QString::number(metrics.detector_failures),
                   QStringLiteral("次")},
        std::tuple{QStringLiteral("连续失败"),
                   QString::number(metrics.consecutive_detector_failures), QStringLiteral("次")},
        std::tuple{QStringLiteral("连续积压"), QString::number(metrics.consecutive_backlog_events),
                   QStringLiteral("次")},
        std::tuple{QStringLiteral("结果拒绝"), QString::number(metrics.result_queue_rejected),
                   QStringLiteral("次")},
        std::tuple{QStringLiteral("处理调用"), QString::number(metrics.process_calls),
                   QStringLiteral("次")},
        std::tuple{QStringLiteral("最近处理耗时"), QString::number(metrics.last_processing_time_us),
                   QStringLiteral("us")},
        std::tuple{QStringLiteral("平均处理耗时"),
                   QString::number(metrics.average_processing_time_us), QStringLiteral("us")},
        std::tuple{QStringLiteral("最大处理耗时"),
                   QString::number(metrics.maximum_processing_time_us), QStringLiteral("us")},
        std::tuple{QStringLiteral("候选"), QString::number(metrics.candidates_created),
                   QStringLiteral("个")},
        std::tuple{QStringLiteral("确认"), QString::number(metrics.confirmed_events),
                   QStringLiteral("个")},
        std::tuple{QStringLiteral("拒绝"), QString::number(metrics.rejected_candidates),
                   QStringLiteral("个")}};
    algorithm_metrics_->setRowCount(static_cast<int>(metric_rows.size()));
    for (std::size_t index = 0; index < metric_rows.size(); ++index)
    {
        const int row = static_cast<int>(index);
        set_table_item(algorithm_metrics_, row, 0, std::get<0>(metric_rows[index]));
        set_table_item(algorithm_metrics_, row, 1, std::get<1>(metric_rows[index]));
        set_table_item(algorithm_metrics_, row, 2, std::get<2>(metric_rows[index]));
    }
    algorithm_metrics_->resizeColumnsToContents();

    algorithm_debug_metrics_->setRowCount(
        snapshot.test_result ? static_cast<int>(snapshot.test_result->debug_metrics.size()) : 0);
    if (snapshot.test_result)
    {
        const auto& result = *snapshot.test_result;
        algorithm_test_result_->setText(
            QStringLiteral(
                "隔离测试=%1；未创建正式候选=%2；帧 %3；异常=%4；触发=%5；类型=%6；"
                "置信度=%7；面积比=%8；变化量=%9；耗时=%10 us；原因=%11；实现=%12；模型=%13")
                .arg(result.isolated ? QStringLiteral("是") : QStringLiteral("否"),
                     result.candidate_created ? QStringLiteral("否") : QStringLiteral("是"))
                .arg(result.sequence_number)
                .arg(result.anomalous ? QStringLiteral("是") : QStringLiteral("否"),
                     result.triggered ? QStringLiteral("是") : QStringLiteral("否"),
                     QString::fromStdString(result.candidate_type))
                .arg(result.confidence, 0, 'f', 4)
                .arg(result.area_ratio, 0, 'f', 4)
                .arg(result.change_score, 0, 'f', 4)
                .arg(result.processing_time_us)
                .arg(QString::fromStdString(result.reason),
                     QString::fromStdString(result.detector_version),
                     QString::fromStdString(result.model_version)));
        QPixmap preview;
        if (!result.preview_jpeg.empty() &&
            preview.loadFromData(reinterpret_cast<const uchar*>(result.preview_jpeg.data()),
                                 static_cast<uint>(result.preview_jpeg.size()), "JPEG"))
        {
            if (snapshot.configuration.debug_overlay && result.preview_source_width > 0U &&
                result.preview_source_height > 0U)
            {
                const double scale_x = static_cast<double>(preview.width()) /
                                       static_cast<double>(result.preview_source_width);
                const double scale_y = static_cast<double>(preview.height()) /
                                       static_cast<double>(result.preview_source_height);
                const QRectF region{result.evaluated_region.offset_x * scale_x,
                                    result.evaluated_region.offset_y * scale_y,
                                    result.evaluated_region.width * scale_x,
                                    result.evaluated_region.height * scale_y};
                QPainter painter{&preview};
                const QColor color = result.anomalous ? QColor{220, 50, 47} : QColor{38, 166, 91};
                painter.setPen(QPen{color, 3.0});
                painter.drawRect(region);
                painter.fillRect(QRectF{region.left(), region.top(), 260.0, 28.0},
                                 QColor{0, 0, 0, 170});
                painter.setPen(Qt::white);
                painter.drawText(QRectF{region.left() + 6.0, region.top(), 250.0, 28.0},
                                 Qt::AlignVCenter,
                                 QStringLiteral("%1 置信度 %2")
                                     .arg(QString::fromStdString(result.candidate_type))
                                     .arg(result.confidence, 0, 'f', 3));
            }
            algorithm_test_preview_->setPixmap(
                preview.scaled(900, 520, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
        else
        {
            algorithm_test_preview_->setPixmap({});
            algorithm_test_preview_->setText(QStringLiteral("测试图像不可用"));
        }
        for (std::size_t index = 0; index < result.debug_metrics.size(); ++index)
        {
            set_table_item(algorithm_debug_metrics_, static_cast<int>(index), 0,
                           QString::fromStdString(result.debug_metrics[index].name));
            set_table_item(algorithm_debug_metrics_, static_cast<int>(index), 1,
                           QString::number(result.debug_metrics[index].value, 'g', 10));
        }
    }
    else
    {
        algorithm_test_result_->setText(QStringLiteral("尚未对当前图像执行隔离测试"));
        algorithm_test_preview_->setPixmap({});
        algorithm_test_preview_->setText(QStringLiteral("测试后显示当前图像及 ROI / 检测结果叠加"));
    }
    algorithm_debug_metrics_->resizeColumnsToContents();

    if (snapshot.operation_pending)
        algorithm_operation_status_->setText(
            QStringLiteral("正在执行 %1").arg(QString::fromStdString(snapshot.operation)));
    else if (snapshot.error)
        algorithm_operation_status_->setText(
            QStringLiteral("失败：%1（%2）")
                .arg(QString::fromStdString(snapshot.error->message),
                     QString::fromStdString(snapshot.error->business_code)));
    else if (!connected)
        algorithm_operation_status_->setText(QStringLiteral("后台服务连接中断"));
    else
        algorithm_operation_status_->setText(
            QStringLiteral("配置与实际状态已同步；单帧测试不会创建候选或写入磁盘"));
}

void MainWindow::apply_storage_snapshot(const StorageClientSnapshot& snapshot)
{
    const bool configuration_changed =
        storage_snapshot_.stored_config_revision != snapshot.stored_config_revision ||
        storage_snapshot_.stale;
    storage_snapshot_ = snapshot;
    if (!storage_editor_ || !storage_status_ || !storage_save_)
        return;

    if (configuration_changed && !snapshot.stale)
    {
        const auto& value = snapshot.configuration;
        storage_event_root_->setText(QString::fromStdString(value.event_root));
        storage_cache_root_->setText(QString::fromStdString(value.cache_root));
        storage_rolling_cache_enabled_->setChecked(value.rolling_cache_enabled);
        storage_maximum_cache_gib_->setValue(static_cast<int>(value.maximum_cache_storage_gib));
        storage_write_limit_mibps_->setValue(
            static_cast<int>(value.rolling_cache_write_limit_mibps));
        storage_io_timeout_ms_->setValue(static_cast<int>(value.rolling_cache_io_timeout_ms));
        storage_warning_gib_->setValue(static_cast<int>(value.warning_free_space_gib));
        storage_critical_gib_->setValue(static_cast<int>(value.critical_free_space_gib));
        storage_stop_gib_->setValue(static_cast<int>(value.stop_free_space_gib));
        storage_maximum_event_gib_->setValue(static_cast<int>(value.maximum_event_storage_gib));
    }

    const bool editable = snapshot.connection.state == ipc::ClientConnectionState::connected &&
                          !snapshot.stale && !snapshot.operation_pending;
    storage_editor_->setEnabled(editable);
    storage_save_->setEnabled(editable);
    if (snapshot.operation_pending)
        storage_status_->setText(QStringLiteral("正在保存并应用存储配置"));
    else if (snapshot.error)
        storage_status_->setText(QStringLiteral("存储配置失败：%1（%2）")
                                     .arg(QString::fromStdString(snapshot.error->message),
                                          QString::fromStdString(snapshot.error->business_code)));
    else if (snapshot.stale)
        storage_status_->setText(QStringLiteral("存储配置尚未从后台服务同步，旧值不可用于保存"));
    else
    {
        QString pending;
        for (const auto& path : snapshot.pending_restart_paths)
        {
            if (path == "/storage/roots" || path == "/storage/nvme")
            {
                if (!pending.isEmpty())
                    pending += QStringLiteral("、");
                pending += QString::fromStdString(path);
            }
        }
        const QString application = pending.isEmpty()
                                        ? QStringLiteral("全部字段已应用，无需重启")
                                        : QStringLiteral("以下保存值需重启后生效：%1").arg(pending);
        storage_status_->setText(
            QStringLiteral("保存配置修订 %1；当前有效修订 %2。%3\n"
                           "实际事件根：%4；实际缓存根：%5；实际滚动缓存：%6")
                .arg(snapshot.stored_config_revision)
                .arg(snapshot.effective_config_revision)
                .arg(application,
                     QString::fromStdString(snapshot.effective_configuration.event_root),
                     QString::fromStdString(snapshot.effective_configuration.cache_root),
                     snapshot.effective_configuration.rolling_cache_enabled
                         ? QStringLiteral("已启用")
                         : QStringLiteral("未启用")));
    }
}

void MainWindow::apply_uplink_snapshot(const UplinkClientSnapshot& snapshot)
{
    const bool configuration_changed =
        uplink_snapshot_.stored_config_revision != snapshot.stored_config_revision ||
        uplink_snapshot_.stale;
    uplink_snapshot_ = snapshot;
    if (!uplink_editor_ || !uplink_status_ || !uplink_save_)
        return;
    if (configuration_changed && !snapshot.stale)
    {
        const auto& value = snapshot.configuration;
        uplink_enabled_->setChecked(value.enabled);
        uplink_server_url_->setText(QString::fromStdString(value.server_url));
        uplink_heartbeat_seconds_->setValue(static_cast<int>(value.heartbeat_seconds));
        uplink_chunk_kib_->setValue(static_cast<int>(value.chunk_bytes / 1024U));
        uplink_io_timeout_ms_->setValue(static_cast<int>(value.io_timeout_ms));
        uplink_upload_limit_mibps_->setValue(static_cast<int>(value.upload_limit_mibps));
    }
    const bool editable = snapshot.connection.state == ipc::ClientConnectionState::connected &&
                          !snapshot.stale && !snapshot.operation_pending;
    uplink_editor_->setEnabled(editable);
    uplink_save_->setEnabled(editable);
    if (snapshot.operation_pending)
        uplink_status_->setText(QStringLiteral("正在保存上位机配置"));
    else if (snapshot.error)
        uplink_status_->setText(QStringLiteral("上位机配置失败：%1（%2）")
                                    .arg(QString::fromStdString(snapshot.error->message),
                                         QString::fromStdString(snapshot.error->business_code)));
    else if (snapshot.stale)
        uplink_status_->setText(QStringLiteral("上位机配置尚未从后台服务同步，旧值不可用于保存"));
    else
    {
        const bool restart_required =
            std::ranges::find(snapshot.pending_restart_paths, "/uplink/transport") !=
            snapshot.pending_restart_paths.end();
        uplink_status_->setText(
            QStringLiteral("保存配置修订 %1；当前有效修订 %2。%3\n"
                           "实际运行配置：%4；上位机 %5；分块 %6 KiB；限速 %7 MiB/s")
                .arg(snapshot.stored_config_revision)
                .arg(snapshot.effective_config_revision)
                .arg(restart_required ? QStringLiteral("保存值需重启后台服务后生效")
                                      : QStringLiteral("保存值与当前有效配置一致"),
                     snapshot.effective_configuration.enabled ? QStringLiteral("已启用")
                                                              : QStringLiteral("未启用"),
                     QString::fromStdString(snapshot.effective_configuration.server_url))
                .arg(snapshot.effective_configuration.chunk_bytes / 1024U)
                .arg(snapshot.effective_configuration.upload_limit_mibps));
    }
}

void MainWindow::apply_event_snapshot(const EventClientSnapshot& snapshot)
{
    const bool configuration_changed =
        event_snapshot_.stored_config_revision != snapshot.stored_config_revision ||
        event_snapshot_.configuration_stale;
    event_snapshot_ = snapshot;
    if (!event_table_ || !event_config_status_)
        return;

    if (configuration_changed && !snapshot.configuration_stale)
    {
        event_pre_seconds_->setValue(static_cast<int>(snapshot.configuration.pre_event_seconds));
        event_post_seconds_->setValue(static_cast<int>(snapshot.configuration.post_event_seconds));
        event_max_seconds_->setValue(static_cast<int>(snapshot.configuration.max_event_seconds));
        event_merge_seconds_->setValue(static_cast<int>(snapshot.configuration.merge_gap_seconds));
        event_key_frames_->setValue(static_cast<int>(snapshot.configuration.key_frame_count));
        event_retention_days_->setValue(static_cast<int>(snapshot.configuration.retention_days));
        event_save_raw_->setChecked(snapshot.configuration.save_raw);
        event_preview_video_->setChecked(snapshot.configuration.generate_preview_video);
        const int upload = event_upload_policy_->findData(
            QString::fromStdString(snapshot.configuration.upload_policy));
        event_upload_policy_->setCurrentIndex(upload >= 0 ? upload : 0);
    }
    const bool configuration_editable =
        snapshot.connection.state == ipc::ClientConnectionState::connected &&
        !snapshot.configuration_stale && !snapshot.operation_pending;
    event_config_editor_->setEnabled(configuration_editable);
    event_config_save_->setEnabled(configuration_editable);
    event_preview_video_->setEnabled(!snapshot.configuration_stale && !snapshot.operation_pending);
    event_preview_video_->setToolTip(
        snapshot.preview_video_generation_available
            ? QString{}
            : QStringLiteral("预览视频生成器尚未实现；配置可保存，但当前不会伪造视频"));
    if (snapshot.operation_pending && snapshot.operation == "event.updateConfig")
        event_config_status_->setText(QStringLiteral("正在保存事件配置"));
    else if (snapshot.configuration_error)
        event_config_status_->setText(
            QStringLiteral("事件配置失败：%1（%2）")
                .arg(QString::fromStdString(snapshot.configuration_error->message),
                     QString::fromStdString(snapshot.configuration_error->business_code)));
    else if (snapshot.configuration_stale)
        event_config_status_->setText(QStringLiteral("事件配置尚未从后台服务同步，暂不能保存"));
    else
        event_config_status_->setText(
            QStringLiteral("配置版本 %1；预览视频当前%2；上传策略由持久化上传队列执行")
                .arg(snapshot.stored_config_revision)
                .arg(snapshot.preview_video_generation_available ? QStringLiteral("可用")
                                                                 : QStringLiteral("不可用")));

    const QString selected_id =
        event_table_->currentRow() >= 0 && event_table_->item(event_table_->currentRow(), 6)
            ? event_table_->item(event_table_->currentRow(), 6)->text()
            : QString{};
    event_table_->setRowCount(static_cast<int>(snapshot.events.size()));
    int selected_row = -1;
    for (std::size_t index = 0; index < snapshot.events.size(); ++index)
    {
        const auto& event = snapshot.events[index];
        const int row = static_cast<int>(index);
        set_table_item(event_table_, row, 0,
                       QDateTime::fromMSecsSinceEpoch(event.candidate_time_utc_ms, QTimeZone::utc())
                           .toLocalTime()
                           .toString(QString::fromLatin1(local_date_time_format)));
        set_table_item(event_table_, row, 1, QString::fromStdString(event.event_state));
        set_table_item(event_table_, row, 2, QString::fromStdString(event.trigger_camera_id));
        set_table_item(event_table_, row, 3, QString::number(event.confidence, 'f', 3));
        set_table_item(event_table_, row, 4, QString::fromStdString(event.upload_state));
        set_table_item(event_table_, row, 5,
                       event.thumbnail_available ? QStringLiteral("可用")
                                                 : QStringLiteral("不可用"));
        set_table_item(event_table_, row, 6, QString::fromStdString(event.event_id));
        if (event_table_->item(row, 6)->text() == selected_id)
            selected_row = row;
    }
    if (selected_row >= 0)
        event_table_->selectRow(selected_row);
    event_table_->resizeColumnsToContents();
    event_previous_->setEnabled(!snapshot.operation_pending && snapshot.filter.offset > 0U);
    event_next_->setEnabled(!snapshot.operation_pending &&
                            snapshot.filter.offset + snapshot.events.size() < snapshot.total);

    const bool has_detail = snapshot.detail.has_value();
    if (has_detail)
    {
        const auto& detail = *snapshot.detail;
        event_manifest_->setPlainText(QString::fromStdString(detail.manifest_json));
        QImage thumbnail;
        if (!detail.thumbnail_jpeg.empty())
            static_cast<void>(
                thumbnail.loadFromData(reinterpret_cast<const uchar*>(detail.thumbnail_jpeg.data()),
                                       static_cast<int>(detail.thumbnail_jpeg.size()), "JPG"));
        event_thumbnail_->setPixmap(thumbnail.isNull() ? QPixmap{} : QPixmap::fromImage(thumbnail));
        if (thumbnail.isNull())
            event_thumbnail_->setText(QStringLiteral("缩略图不可用"));
    }
    const bool candidate = has_detail && snapshot.detail->event.event_state == "Candidate" &&
                           snapshot.connection.state == ipc::ClientConnectionState::connected &&
                           !snapshot.operation_pending;
    event_confirm_->setEnabled(candidate);
    event_reject_->setEnabled(candidate);
    event_export_->setEnabled(has_detail && !snapshot.operation_pending);
    event_open_directory_->setEnabled(has_detail && !snapshot.operation_pending);
    event_retry_upload_->setEnabled(has_detail && !snapshot.operation_pending &&
                                    snapshot.connection.state ==
                                        ipc::ClientConnectionState::connected);
    if (snapshot.operation_pending)
        event_status_->setText(
            QStringLiteral("正在执行 %1").arg(QString::fromStdString(snapshot.operation)));
    else if (snapshot.error)
        event_status_->setText(QStringLiteral("失败：%1（%2）")
                                   .arg(QString::fromStdString(snapshot.error->message),
                                        QString::fromStdString(snapshot.error->business_code)));
    else if (snapshot.exported_path)
        event_status_->setText(
            QStringLiteral("导出完成：%1")
                .arg(QString::fromStdWString(snapshot.exported_path->wstring())));
    else
        event_status_->setText(
            QStringLiteral("共 %1 个事件；当前从第 %2 条开始；详情读取前会校验 manifest 与文件摘要")
                .arg(snapshot.total)
                .arg(snapshot.filter.offset + 1U));
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

    if (snapshot.metrics && snapshot.metrics->uplink_state)
    {
        QString uplink = QString::fromStdString(*snapshot.metrics->uplink_state);
        if (snapshot.metrics->pending_upload_tasks)
            uplink += QStringLiteral(" · 待传 %1").arg(*snapshot.metrics->pending_upload_tasks);
        uplink_value_->setText(stale_value(uplink, snapshot.metrics_stale));
        overview_uplink_value_->setText(
            stale_value(QStringLiteral("上位机连接：%1")
                            .arg(QString::fromStdString(*snapshot.metrics->uplink_state)),
                        snapshot.metrics_stale));
        overview_upload_value_->setText(
            snapshot.metrics->pending_upload_tasks
                ? stale_value(
                      QStringLiteral("待上传任务：%1").arg(*snapshot.metrics->pending_upload_tasks),
                      snapshot.metrics_stale)
                : QStringLiteral("待上传任务：不可用"));
    }
    else
    {
        uplink_value_->setText(QStringLiteral("未启用/不可用"));
        overview_uplink_value_->setText(QStringLiteral("上位机连接：未启用/不可用"));
        overview_upload_value_->setText(QStringLiteral("待上传任务：不可用"));
    }
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

bool MainWindow::algorithm_page_ready() const noexcept
{
    return algorithm_camera_selector_ && algorithm_editor_ && algorithm_enabled_ &&
           algorithm_type_ && algorithm_roi_width_ && algorithm_roi_height_ && algorithm_roi_x_ &&
           algorithm_roi_y_ && algorithm_candidate_threshold_ &&
           algorithm_confirmation_threshold_ && algorithm_consecutive_frames_ &&
           algorithm_cooldown_ms_ && algorithm_model_reference_ && algorithm_model_version_ &&
           algorithm_device_ && algorithm_debug_overlay_ && algorithm_save_ && algorithm_test_ &&
           algorithm_runtime_status_ && algorithm_metrics_ && algorithm_test_result_ &&
           algorithm_test_preview_ && algorithm_debug_metrics_ &&
           findChild<QLabel*>(QStringLiteral("algorithm-prototype-notice"));
}

bool MainWindow::event_pages_ready() const noexcept
{
    return event_config_editor_ && event_config_save_ && event_pre_seconds_ &&
           event_post_seconds_ && event_max_seconds_ && event_merge_seconds_ && event_key_frames_ &&
           event_retention_days_ && event_save_raw_ && event_preview_video_ &&
           event_upload_policy_ && event_filter_start_ && event_filter_end_ &&
           event_filter_state_ && event_filter_camera_ && event_table_ && event_thumbnail_ &&
           event_manifest_ && event_confirm_ && event_reject_ && event_export_ &&
           event_open_directory_ && event_retry_upload_;
}

bool MainWindow::storage_page_ready() const noexcept
{
    return storage_editor_ && storage_event_root_ && storage_cache_root_ &&
           storage_rolling_cache_enabled_ && storage_maximum_cache_gib_ &&
           storage_write_limit_mibps_ && storage_io_timeout_ms_ && storage_warning_gib_ &&
           storage_critical_gib_ && storage_stop_gib_ && storage_maximum_event_gib_ &&
           storage_save_ && storage_status_ && storage_metrics_ &&
           has_readable_table_header(storage_metrics_) &&
           findChild<QLabel*>(QStringLiteral("storage-restart-notice"));
}

bool MainWindow::uplink_page_ready() const noexcept
{
    return uplink_editor_ && uplink_enabled_ && uplink_server_url_ && uplink_heartbeat_seconds_ &&
           uplink_chunk_kib_ && uplink_io_timeout_ms_ && uplink_upload_limit_mibps_ &&
           uplink_save_ && uplink_status_ &&
           findChild<QLabel*>(QStringLiteral("uplink-restart-notice"));
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    hide();
    event->ignore();
}

} // namespace paperbreak::console
