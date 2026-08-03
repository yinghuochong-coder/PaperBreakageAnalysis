#include "paperbreak/console/tray_status_model.hpp"

namespace paperbreak::console
{

TrayStatus tray_status(const ClientStateSnapshot& snapshot) noexcept
{
    if (snapshot.connection.state != ipc::ClientConnectionState::connected ||
        snapshot.service_status_stale || !snapshot.service_status.has_value())
    {
        return {.color = TrayStatusColor::gray, .label = "无法连接本地服务"};
    }
    if (snapshot.service_status->service_state != "running")
    {
        return {.color = TrayStatusColor::red, .label = "后台服务未正常运行"};
    }
    if (snapshot.alarms_stale || !snapshot.alarms.has_value())
    {
        return {.color = TrayStatusColor::gray, .label = "报警状态正在同步"};
    }
    if (snapshot.alarms->highest_severity == "Critical" ||
        snapshot.alarms->highest_severity == "Error")
    {
        return {.color = TrayStatusColor::red, .label = "存在严重故障"};
    }
    if (snapshot.alarms->highest_severity == "Warning")
    {
        return {.color = TrayStatusColor::yellow, .label = "存在警告"};
    }
    return {.color = TrayStatusColor::green, .label = "服务运行正常"};
}

} // namespace paperbreak::console
