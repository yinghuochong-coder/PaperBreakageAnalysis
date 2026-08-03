#pragma once

#include "paperbreak/console/camera_client.hpp"
#include "paperbreak/console/client_state_store.hpp"
#include "paperbreak/console/preview_client.hpp"
#include "theme_controller.hpp"

#include <QMainWindow>

#include <cstddef>

class QLabel;
class QListWidget;
class QStackedWidget;
class QPushButton;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QLineEdit;
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

class MainWindow final : public QMainWindow
{
  public:
    explicit MainWindow(std::function<void(bool)> preview_pause_changed = {},
                        CameraUiActions camera_actions = {}, ThemeUiActions theme_actions = {},
                        QWidget* parent = nullptr);

    void apply_snapshot(const ClientStateSnapshot& snapshot);
    void update_clock();
    void apply_preview_snapshot(const PreviewSnapshot& snapshot);
    void apply_camera_snapshot(const CameraClientSnapshot& snapshot);

    [[nodiscard]] std::size_t page_count() const noexcept;
    [[nodiscard]] int current_page_index() const noexcept;
    [[nodiscard]] bool select_page(std::size_t index) noexcept;
    [[nodiscard]] bool camera_configuration_ready() const noexcept;
    [[nodiscard]] std::size_t discovered_camera_count() const noexcept;
    [[nodiscard]] bool camera_device_controls_disabled() const noexcept;
    [[nodiscard]] bool select_theme_mode(ThemeMode mode) noexcept;

  private:
    void closeEvent(QCloseEvent* event) override;
    void populate_camera_editor();
    void populate_camera_editor(const CameraParameterValue& value);
    void update_camera_controls();
    void run_camera_control(const std::string& command, bool confirmation_required);
    void show_camera_result(const Result<void>& result);

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
    CameraClientSnapshot camera_snapshot_;
    std::string camera_editor_id_;
    std::uint64_t camera_editor_revision_{};
    bool camera_parameter_read_pending_{};
    QPushButton* preview_pause_button_{};
    std::function<void(bool)> preview_pause_changed_;
    bool preview_paused_{};
    QListWidget* navigation_{};
    QStackedWidget* pages_{};
    QComboBox* theme_selector_{};
};

} // namespace paperbreak::console
