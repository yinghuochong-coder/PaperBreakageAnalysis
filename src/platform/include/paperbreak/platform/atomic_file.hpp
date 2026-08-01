#pragma once

#include "paperbreak/common/result.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace paperbreak::platform
{

/// Narrow, injectable file operations required by versioned configuration storage.
class IAtomicFileSystem
{
  public:
    virtual ~IAtomicFileSystem() = default;

    [[nodiscard]] virtual Result<std::string> read_bounded(const std::filesystem::path& path,
                                                           std::size_t maximum_bytes) = 0;
    [[nodiscard]] virtual Result<void> create_directories(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual Result<std::vector<std::filesystem::path>> list_regular_files(
        const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual Result<void> remove_file(const std::filesystem::path& path) = 0;

    /// Writes through a same-directory temporary file and atomically replaces the target.
    [[nodiscard]] virtual Result<void> replace_atomically(
        const std::filesystem::path& target, std::string_view contents,
        const std::optional<std::filesystem::path>& backup = std::nullopt) = 0;
};

/// Windows implementation based on CreateFileW, FlushFileBuffers and ReplaceFileW/MoveFileExW.
class WindowsAtomicFileSystem final : public IAtomicFileSystem
{
  public:
    [[nodiscard]] Result<std::string> read_bounded(const std::filesystem::path& path,
                                                   std::size_t maximum_bytes) override;
    [[nodiscard]] Result<void> create_directories(const std::filesystem::path& path) override;
    [[nodiscard]] Result<std::vector<std::filesystem::path>> list_regular_files(
        const std::filesystem::path& path) override;
    [[nodiscard]] Result<void> remove_file(const std::filesystem::path& path) override;
    [[nodiscard]] Result<void> replace_atomically(
        const std::filesystem::path& target, std::string_view contents,
        const std::optional<std::filesystem::path>& backup = std::nullopt) override;
};

} // namespace paperbreak::platform
