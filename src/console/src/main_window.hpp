#pragma once

#include "paperbreak/console/algorithm_client.hpp"
#include "paperbreak/console/camera_client.hpp"
#include "paperbreak/console/client_state_store.hpp"
#include "paperbreak/console/event_client.hpp"
#include "paperbreak/console/operations_client.hpp"
#include "paperbreak/console/preview_client.hpp"
#include "paperbreak/console/storage_client.hpp"
#include "paperbreak/console/uplink_client.hpp"
#include "theme_controller.hpp"

#include <QMainWindow>

#include <cstddef>
#include <filesystem>

class QLabel;
class QListWidget;
class QStackedWidget;
class QPushButton;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QLineEdit;
class QTableWidget;
class QCheckBox;
class QDateTimeEdit;
class QTextEdit;
class QWidget;

namespace paperbreak::console
{

struct CameraUiActions final
{
    std::function<Result<void>()> discover;
    std::function<Result<void>(std::string, std::string, std::string, std::uint64_t)> bind;
    std::function<Result<void>(std::string, std::string)> control;
    std::function<Result<void>(std::string, std::uint64_t, CameraParameterValue)> update_config;
};

struct ThemeUiActions final
{
    ThemeMode initial_mode{ThemeMode::system};
    std::function<void(ThemeMode)> set_mode;
};

struct OperationsUiActions final
{
    std::function<void()> refresh;
    std::function<Result<void>(AlarmFilter)> query_alarms;
    std::function<Result<void>(LogFilter)> query_logs;
    std::function<Result<void>(std::uint64_t)> acknowledge;
    std::function<Result<void>(std::filesystem::path)> export_diagnostics;
    std::function<Result<void>(std::filesystem::path)> export_alarm_csv;
};

struct EventUiActions final
{
    std::function<void()> refresh;
    std::function<Result<void>(EventListFilter)> query;
    std::function<Result<void>(std::string)> get;
    std::function<Result<void>(EventConfigurationValue)> update_configuration;
    std::function<Result<void>(std::string)> manual_trigger;
    std::function<Result<void>(std::string, std::uint64_t, bool)> review;
    std::function<Result<void>(std::string, std::filesystem::path)> export_event;
    std::function<Result<void>(std::string)> retry_upload;
};

struct AlgorithmUiActions final
{
    std::function<void()> refresh;
    std::function<Result<void>(std::string)> select_camera;
    std::function<Result<void>(AlgorithmConfigurationValue)> update_configuration;
    std::function<Result<void>()> test_current_frame;
};

struct StorageUiActions final
{
    std::function<void()> refresh;
    std::function<Result<void>(StorageConfigurationValue)> update_configuration;
};

struct UplinkUiActions final
{
    std::function<void()> refresh;
    std::function<Result<void>(UplinkConfigurationValue)> update_configuration;
};

class MainWindow final : public QMainWindow
{
  public:
    explicit MainWindow(std::function<void(bool)> preview_pause_changed = {},
                        CameraUiActions camera_actions = {}, ThemeUiActions theme_actions = {},
                        OperationsUiActions operations_actions = {},
                        AlgorithmUiActions algorithm_actions = {},
                        EventUiActions event_actions = {}, StorageUiActions storage_actions = {},
                        UplinkUiActions uplink_actions = {}, QWidget* parent = nullptr);

    void apply_snapshot(const ClientStateSnapshot& snapshot);
    void update_clock();
    void apply_preview_snapshot(const PreviewSnapshot& snapshot);
    void apply_camera_snapshot(const CameraClientSnapshot& snapshot);
    void apply_operations_snapshot(const OperationsSnapshot& snapshot);
    void apply_algorithm_snapshot(const AlgorithmClientSnapshot& snapshot);
    void apply_event_snapshot(const EventClientSnapshot& snapshot);
    void apply_storage_snapshot(const StorageClientSnapshot& snapshot);
    void apply_uplink_snapshot(const UplinkClientSnapshot& snapshot);
    void request_diagnostics_export();

    [[nodiscard]] std::size_t page_count() const noexcept;
    [[nodiscard]] int current_page_index() const noexcept;
    [[nodiscard]] bool select_page(std::size_t index) noexcept;
    [[nodiscard]] bool camera_configuration_ready() const noexcept;
    [[nodiscard]] std::size_t discovered_camera_count() const noexcept;
    [[nodiscard]] bool camera_device_controls_disabled() const noexcept;
    [[nodiscard]] bool select_theme_mode(ThemeMode mode) noexcept;
    [[nodiscard]] bool operations_pages_ready() const noexcept;
    [[nodiscard]] bool algorithm_page_ready() const noexcept;
    [[nodiscard]] bool event_pages_ready() const noexcept;
    [[nodiscard]] bool storage_page_ready() const noexcept;
    [[nodiscard]] bool uplink_page_ready() const noexcept;

  private:
    void closeEvent(QCloseEvent* event) override;
    void populate_camera_editor();
    void populate_camera_editor(const CameraParameterValue& value);
    void update_camera_controls();
    void run_camera_control(const std::string& command, bool confirmation_required);
    void show_camera_result(const Result<void>& result);
    void show_operations_result(const Result<void>& result);
    void show_algorithm_result(const Result<void>& result);
    void show_event_result(const Result<void>& result);
    void show_event_config_result(const Result<void>& result);
    void show_storage_result(const Result<void>& result);
    void show_uplink_result(const Result<void>& result);
    void update_alarm_details();

    QLabel* connection_banner_{};
    QLabel* service_value_{};
    QLabel* clock_value_{};
    QLabel* machine_value_{};
    QLabel* uplink_value_{};
    QLabel* camera_count_value_{};
    QLabel* alarm_count_value_{};
    QLabel* disk_value_{};
    QLabel* version_value_{};
    QLabel* cpu_value_{};
    QLabel* memory_value_{};
    QLabel* overview_disk_value_{};
    QLabel* recent_alarms_value_{};
    QLabel* overview_sync_value_{};
    std::array<QLabel*, 4U> overview_camera_states_{};
    std::array<QLabel*, 4U> overview_camera_fps_{};
    std::array<QLabel*, 4U> overview_camera_brightness_{};
    std::array<QLabel*, 4U> overview_camera_last_frames_{};
    QLabel* overview_detector_value_{};
    QLabel* overview_candidate_value_{};
    QLabel* overview_uplink_value_{};
    QLabel* overview_upload_value_{};
    std::array<QLabel*, 4U> preview_images_{};
    std::array<QLabel*, 4U> preview_overlays_{};
    QLabel* preview_status_{};
    QLabel* camera_configuration_value_{};
    QLabel* camera_operation_value_{};
    QComboBox* camera_selector_{};
    QListWidget* discovered_devices_{};
    QComboBox* camera_bind_slot_{};
    QLineEdit* camera_bind_location_{};
    QPushButton* camera_bind_button_{};
    QPushButton* camera_read_parameters_button_{};
    QWidget* camera_editor_{};
    QWidget* camera_control_actions_{};
    QDoubleSpinBox* camera_exposure_{};
    QDoubleSpinBox* camera_gain_{};
    QDoubleSpinBox* camera_fps_{};
    QSpinBox* camera_roi_width_{};
    QSpinBox* camera_roi_height_{};
    QSpinBox* camera_roi_x_{};
    QSpinBox* camera_roi_y_{};
    QComboBox* camera_pixel_format_{};
    QComboBox* camera_trigger_mode_{};
    QLineEdit* camera_trigger_source_{};
    QSpinBox* camera_trigger_delay_{};
    QSpinBox* camera_packet_size_{};
    QSpinBox* camera_packet_delay_{};
    CameraUiActions camera_actions_;
    ThemeUiActions theme_actions_;
    OperationsUiActions operations_actions_;
    AlgorithmUiActions algorithm_actions_;
    EventUiActions event_actions_;
    StorageUiActions storage_actions_;
    UplinkUiActions uplink_actions_;
    CameraClientSnapshot camera_snapshot_;
    std::string camera_editor_id_;
    std::uint64_t camera_editor_revision_{};
    bool camera_parameter_read_pending_{};
    QPushButton* preview_pause_button_{};
    std::function<void(bool)> preview_pause_changed_;
    bool preview_paused_{};
    QTableWidget* metrics_table_{};
    QComboBox* alarm_scope_{};
    QComboBox* alarm_severity_{};
    QLineEdit* alarm_source_{};
    QTableWidget* alarm_table_{};
    QLabel* alarm_details_{};
    QPushButton* alarm_acknowledge_{};
    QPushButton* alarm_export_{};
    QComboBox* log_category_{};
    QComboBox* log_level_{};
    QTableWidget* log_table_{};
    QLabel* operations_status_{};
    QPushButton* diagnostics_export_{};
    OperationsSnapshot operations_snapshot_;
    AlgorithmClientSnapshot algorithm_snapshot_;
    QComboBox* algorithm_camera_selector_{};
    QCheckBox* algorithm_enabled_{};
    QComboBox* algorithm_type_{};
    QSpinBox* algorithm_roi_width_{};
    QSpinBox* algorithm_roi_height_{};
    QSpinBox* algorithm_roi_x_{};
    QSpinBox* algorithm_roi_y_{};
    QDoubleSpinBox* algorithm_candidate_threshold_{};
    QDoubleSpinBox* algorithm_confirmation_threshold_{};
    QSpinBox* algorithm_consecutive_frames_{};
    QSpinBox* algorithm_cooldown_ms_{};
    QLineEdit* algorithm_model_reference_{};
    QLineEdit* algorithm_model_version_{};
    QComboBox* algorithm_device_{};
    QCheckBox* algorithm_debug_overlay_{};
    QWidget* algorithm_editor_{};
    QPushButton* algorithm_save_{};
    QPushButton* algorithm_test_{};
    QLabel* algorithm_runtime_status_{};
    QLabel* algorithm_operation_status_{};
    QLabel* algorithm_test_result_{};
    QLabel* algorithm_test_preview_{};
    QTableWidget* algorithm_metrics_{};
    QTableWidget* algorithm_debug_metrics_{};
    EventClientSnapshot event_snapshot_;
    QSpinBox* event_pre_seconds_{};
    QSpinBox* event_post_seconds_{};
    QSpinBox* event_max_seconds_{};
    QSpinBox* event_merge_seconds_{};
    QSpinBox* event_key_frames_{};
    QSpinBox* event_retention_days_{};
    QCheckBox* event_save_raw_{};
    QCheckBox* event_preview_video_{};
    QComboBox* event_upload_policy_{};
    QWidget* event_config_editor_{};
    QPushButton* event_config_save_{};
    QLabel* event_config_status_{};
    QDateTimeEdit* event_filter_start_{};
    QDateTimeEdit* event_filter_end_{};
    QComboBox* event_filter_state_{};
    QLineEdit* event_filter_camera_{};
    QTableWidget* event_table_{};
    QLabel* event_thumbnail_{};
    QTextEdit* event_manifest_{};
    QLabel* event_status_{};
    QPushButton* event_previous_{};
    QPushButton* event_next_{};
    QPushButton* event_confirm_{};
    QPushButton* event_reject_{};
    QPushButton* event_export_{};
    QPushButton* event_open_directory_{};
    QPushButton* event_retry_upload_{};
    StorageClientSnapshot storage_snapshot_;
    QLineEdit* storage_event_root_{};
    QLineEdit* storage_cache_root_{};
    QCheckBox* storage_rolling_cache_enabled_{};
    QSpinBox* storage_maximum_cache_gib_{};
    QSpinBox* storage_write_limit_mibps_{};
    QSpinBox* storage_io_timeout_ms_{};
    QSpinBox* storage_warning_gib_{};
    QSpinBox* storage_critical_gib_{};
    QSpinBox* storage_stop_gib_{};
    QSpinBox* storage_maximum_event_gib_{};
    QWidget* storage_editor_{};
    QPushButton* storage_save_{};
    QLabel* storage_status_{};
    QTableWidget* storage_metrics_{};
    UplinkClientSnapshot uplink_snapshot_;
    QCheckBox* uplink_enabled_{};
    QLineEdit* uplink_server_url_{};
    QSpinBox* uplink_heartbeat_seconds_{};
    QSpinBox* uplink_chunk_kib_{};
    QSpinBox* uplink_io_timeout_ms_{};
    QSpinBox* uplink_upload_limit_mibps_{};
    QWidget* uplink_editor_{};
    QPushButton* uplink_save_{};
    QLabel* uplink_status_{};
    QListWidget* navigation_{};
    QStackedWidget* pages_{};
    QComboBox* theme_selector_{};
};

} // namespace paperbreak::console
