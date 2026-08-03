#include "paperbreak/console/navigation_model.hpp"

#include <array>

namespace paperbreak::console
{
namespace
{

constexpr std::array pages{
    ConsolePageDescriptor{ConsolePageId::overview, "overview", "总览"},
    ConsolePageDescriptor{ConsolePageId::preview, "preview", "实时预览"},
    ConsolePageDescriptor{ConsolePageId::camera_configuration, "camera-configuration", "相机配置"},
    ConsolePageDescriptor{ConsolePageId::algorithm_configuration, "algorithm-configuration",
                          "算法配置"},
    ConsolePageDescriptor{ConsolePageId::event_configuration, "event-configuration", "事件配置"},
    ConsolePageDescriptor{ConsolePageId::storage_configuration, "storage-configuration",
                          "存储配置"},
    ConsolePageDescriptor{ConsolePageId::uplink_configuration, "uplink-configuration",
                          "上位机配置"},
    ConsolePageDescriptor{ConsolePageId::device_status, "device-status", "设备状态"},
    ConsolePageDescriptor{ConsolePageId::alarms, "alarms", "报警"},
    ConsolePageDescriptor{ConsolePageId::events, "events", "事件"},
    ConsolePageDescriptor{ConsolePageId::logs, "logs", "日志"},
    ConsolePageDescriptor{ConsolePageId::maintenance, "maintenance", "维护"},
};

} // namespace

std::span<const ConsolePageDescriptor> console_pages() noexcept
{
    return pages;
}

std::size_t default_console_page_index() noexcept
{
    return 0U;
}

std::optional<std::size_t> console_page_index(const ConsolePageId id) noexcept
{
    for (std::size_t index = 0; index < pages.size(); ++index)
    {
        if (pages[index].id == id)
        {
            return index;
        }
    }
    return std::nullopt;
}

} // namespace paperbreak::console
