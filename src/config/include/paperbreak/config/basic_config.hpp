#pragma once

#include "paperbreak/common/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace paperbreak::config
{

inline constexpr std::size_t basic_config_max_bytes = 1024U * 1024U;
inline constexpr std::uint32_t basic_config_schema_version = 1U;

/// Metadata extracted by the M1-01 bounded preflight validator.
struct BasicConfigInfo final
{
    std::uint32_t schema_version{};
    std::size_t file_size_bytes{};
};

/// Validates bounded JSON syntax, the root object, and schemaVersion only.
[[nodiscard]] Result<BasicConfigInfo> validate_basic_config(
    const std::filesystem::path& path, std::size_t maximum_bytes = basic_config_max_bytes) noexcept;

} // namespace paperbreak::config
