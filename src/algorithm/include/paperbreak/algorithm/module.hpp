#pragma once
#include <string_view>
namespace paperbreak::algorithm
{
/// Returns the stable placeholder module name; detector APIs begin in M6.
[[nodiscard]] std::string_view module_name() noexcept;
} // namespace paperbreak::algorithm
