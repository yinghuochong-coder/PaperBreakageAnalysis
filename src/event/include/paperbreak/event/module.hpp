#pragma once
#include <string_view>
namespace paperbreak::event
{
/// Returns the stable placeholder module name; event APIs begin in M5.
[[nodiscard]] std::string_view module_name() noexcept;
} // namespace paperbreak::event
