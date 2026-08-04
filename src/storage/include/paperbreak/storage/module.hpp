#pragma once
#include <string_view>
namespace paperbreak::storage
{
/// Returns the stable storage module name.
[[nodiscard]] std::string_view module_name() noexcept;
} // namespace paperbreak::storage
