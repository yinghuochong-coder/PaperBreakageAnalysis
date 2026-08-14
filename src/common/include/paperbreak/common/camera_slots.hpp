#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace paperbreak
{

inline constexpr std::size_t camera_slot_count = 6U;
inline constexpr std::array<std::string_view, camera_slot_count> canonical_camera_ids{
    "CAM01", "CAM02", "CAM03", "CAM04", "CAM05", "CAM06"};

[[nodiscard]] constexpr std::optional<std::size_t> camera_slot_index(
    const std::string_view camera_id) noexcept
{
    for (std::size_t index = 0U; index < canonical_camera_ids.size(); ++index)
    {
        if (canonical_camera_ids[index] == camera_id)
            return index;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr bool is_canonical_camera_id(const std::string_view camera_id) noexcept
{
    return camera_slot_index(camera_id).has_value();
}

[[nodiscard]] constexpr std::optional<std::string_view> camera_id_from_slot(
    const std::size_t slot_index) noexcept
{
    if (slot_index >= canonical_camera_ids.size())
        return std::nullopt;
    return canonical_camera_ids[slot_index];
}

} // namespace paperbreak
