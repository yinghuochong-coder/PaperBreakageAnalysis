#pragma once

#include "paperbreak/console/client_state_store.hpp"

#include <string_view>

namespace paperbreak::console
{

enum class TrayStatusColor
{
    green,
    yellow,
    red,
    gray,
};

struct TrayStatus final
{
    TrayStatusColor color{TrayStatusColor::gray};
    std::string_view label{"无法连接本地服务"};
};

[[nodiscard]] TrayStatus tray_status(const ClientStateSnapshot& snapshot) noexcept;

} // namespace paperbreak::console
