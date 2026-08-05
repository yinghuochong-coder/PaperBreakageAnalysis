#pragma once
#include <string_view>
namespace paperbreak::uplink
{
/// Returns the stable module name; M8-00 freezes DTOs while the edge transport starts in M8-01.
[[nodiscard]] std::string_view module_name() noexcept;
} // namespace paperbreak::uplink
