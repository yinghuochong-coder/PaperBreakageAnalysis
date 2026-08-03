#pragma once

#include "paperbreak/console/client_state_store.hpp"
#include "paperbreak/console/preview_client.hpp"

#include <QMainWindow>

#include <cstddef>

class QLabel;
class QListWidget;
class QStackedWidget;
class QPushButton;

namespace paperbreak::console
{

class MainWindow final : public QMainWindow
{
  public:
    explicit MainWindow(std::function<void(bool)> preview_pause_changed = {}, QWidget* parent = nullptr);

    void apply_snapshot(const ClientStateSnapshot& snapshot);
    void update_clock();
    void apply_preview_snapshot(const PreviewSnapshot& snapshot);

    [[nodiscard]] std::size_t page_count() const noexcept;
    [[nodiscard]] int current_page_index() const noexcept;
    [[nodiscard]] bool select_page(std::size_t index) noexcept;

  private:
    void closeEvent(QCloseEvent* event) override;

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
    QPushButton* preview_pause_button_{};
    std::function<void(bool)> preview_pause_changed_;
    bool preview_paused_{};
    QListWidget* navigation_{};
    QStackedWidget* pages_{};
};

} // namespace paperbreak::console
