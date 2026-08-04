#pragma once
#include <string_view>
namespace paperbreak::algorithm
{
/// Returns the stable module name.
[[nodiscard]] std::string_view module_name() noexcept;
} // namespace paperbreak::algorithm
