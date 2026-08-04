#pragma once
#include <string_view>
namespace paperbreak::event
{
/// Returns the stable event module name.
[[nodiscard]] std::string_view module_name() noexcept;
} // namespace paperbreak::event
