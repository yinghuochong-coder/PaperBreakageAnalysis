#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace paperbreak::console
{

enum class ConsolePageId
{
    overview,
    preview,
    camera_configuration,
    algorithm_configuration,
    event_configuration,
    storage_configuration,
    uplink_configuration,
    device_status,
    alarms,
    events,
    logs,
    maintenance,
};

struct ConsolePageDescriptor final
{
    ConsolePageId id{ConsolePageId::overview};
    std::string_view key;
    std::string_view title;
};

[[nodiscard]] std::span<const ConsolePageDescriptor> console_pages() noexcept;
[[nodiscard]] std::size_t default_console_page_index() noexcept;
[[nodiscard]] std::optional<std::size_t> console_page_index(ConsolePageId id) noexcept;

} // namespace paperbreak::console
