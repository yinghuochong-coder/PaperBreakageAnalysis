#pragma once
#include <string_view>
namespace paperbreak::camera
{
/// Returns the stable placeholder module name; camera APIs begin in M2.
[[nodiscard]] std::string_view module_name() noexcept;
} // namespace paperbreak::camera
