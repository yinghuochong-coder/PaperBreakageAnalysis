#pragma once

#include "paperbreak/console/client_state_store.hpp"
#include "paperbreak/console/tray_status_model.hpp"

#include <QMenu>
#include <QSystemTrayIcon>

#include <cstddef>
#include <functional>

namespace paperbreak::console
{

struct TrayCallbacks final
{
    std::function<void()> open_console;
    std::function<void()> show_status;
    std::function<void()> restart_service;
    std::function<void()> open_event_directory;
    std::function<void()> show_about;
    std::function<void()> quit_interface;
};

class SystemTrayController final
{
  public:
    explicit SystemTrayController(TrayCallbacks callbacks);

    void show();
    void hide();
    void apply_snapshot(const ClientStateSnapshot& snapshot);

    [[nodiscard]] bool is_visible() const noexcept;
    [[nodiscard]] std::size_t action_count() const noexcept;
    [[nodiscard]] bool preview_action_enabled() const noexcept;
    [[nodiscard]] bool diagnostics_action_enabled() const noexcept;

  private:
    QMenu menu_;
    QSystemTrayIcon tray_;
    QAction* status_action_{};
    QAction* preview_action_{};
    QAction* restart_action_{};
    QAction* event_directory_action_{};
    QAction* diagnostics_action_{};
    bool status_initialized_{};
    TrayStatusColor last_color_{TrayStatusColor::gray};
};

} // namespace paperbreak::console
