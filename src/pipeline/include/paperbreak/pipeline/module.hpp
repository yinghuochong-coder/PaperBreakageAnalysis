#pragma once
#include <string_view>
namespace paperbreak::pipeline
{
/// Returns the stable placeholder module name; pipeline APIs begin in M2.
[[nodiscard]] std::string_view module_name() noexcept;
} // namespace paperbreak::pipeline
