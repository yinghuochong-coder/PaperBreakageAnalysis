#pragma once
#include <string_view>
namespace paperbreak::uplink
{
/// Returns the stable placeholder module name; uplink APIs begin in M8.
[[nodiscard]] std::string_view module_name() noexcept;
} // namespace paperbreak::uplink
