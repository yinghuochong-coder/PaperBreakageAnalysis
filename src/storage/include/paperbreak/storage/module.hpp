#pragma once
#include <string_view>
namespace paperbreak::storage
{
/// Returns the stable placeholder module name; storage APIs begin in M5.
[[nodiscard]] std::string_view module_name() noexcept;
} // namespace paperbreak::storage
