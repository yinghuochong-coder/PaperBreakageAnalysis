#pragma once

#include "paperbreak/uplink/simulator.hpp"

#include <QMainWindow>

#include <cstddef>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

namespace paperbreak::uplink::simulator
{

class MainWindow final : public QMainWindow
{
  public:
    MainWindow(Runtime& runtime, Options options, QWidget* parent = nullptr);

  private:
    void refresh();
    void refresh_device_choices(const Snapshot& snapshot);
    void send_command();
    void apply_fault_profile();

    Runtime& runtime_;
    Options options_;
    QTimer* timer_{};
    QLabel* status_label_{};
    QLabel* workspace_label_{};
    QLabel* preview_label_{};
    QPushButton* start_button_{};
    QPushButton* stop_button_{};
    QTableWidget* devices_{};
    QTableWidget* uploads_{};
    QComboBox* command_device_{};
    QComboBox* command_type_{};
    QPlainTextEdit* command_payload_{};
    QComboBox* fault_device_{};
    QCheckBox* reject_connections_{};
    QCheckBox* disconnect_websockets_{};
    QCheckBox* duplicate_acks_{};
    QCheckBox* replay_commands_{};
    QCheckBox* checksum_mismatch_{};
    QSpinBox* response_delay_{};
    QSpinBox* fail_next_{};
    QLineEdit* disconnect_chunk_{};
    QPlainTextEdit* logs_{};
    std::size_t last_log_count_{};
};

} // namespace paperbreak::uplink::simulator
