#include "paperbreak/storage/event_store.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <exception>
#include <fstream>
#include <limits>
#include <system_error>

namespace paperbreak::storage
{
namespace
{

Error file_error(std::string message, std::string operation, const DWORD native_code = 0U)
{
    Error error = make_error("STORAGE_IO_FAILED", Severity::error, std::move(message), "storage",
                             std::move(operation), true);
    if (native_code != 0U)
    {
        error.native_domain = "win32";
        error.native_code = std::to_string(native_code);
    }
    return error;
}

class UniqueHandle final
{
  public:
    explicit UniqueHandle(const HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~UniqueHandle()
    {
        if (valid())
            static_cast<void>(CloseHandle(value_));
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept
    {
        return value_;
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return value_ != INVALID_HANDLE_VALUE;
    }

  private:
    HANDLE value_;
};

Result<std::wstring> volume_root_for(const std::filesystem::path& path)
{
    std::array<wchar_t, MAX_PATH + 1U> root{};
    if (GetVolumePathNameW(path.c_str(), root.data(), static_cast<DWORD>(root.size())) == FALSE)
    {
        return Result<std::wstring>::failure(
            file_error("无法解析事件目录所在卷", "event.file.volume", GetLastError()));
    }
    std::wstring value{root.data()};
    std::ranges::transform(value, value.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return Result<std::wstring>::success(std::move(value));
}

class WindowsEventFileSystem final : public IEventFileSystem
{
  public:
    Result<void> create_directories(const std::filesystem::path& path) override
    {
        if (path.empty())
            return Result<void>::failure(
                file_error("事件目录路径为空", "event.file.createDirectories"));
        std::error_code error;
        static_cast<void>(std::filesystem::create_directories(path, error));
        if (error)
            return Result<void>::failure(file_error("无法创建事件目录",
                                                    "event.file.createDirectories",
                                                    static_cast<DWORD>(error.value())));
        return Result<void>::success();
    }

    Result<void> create_directory_exclusive(const std::filesystem::path& path) override
    {
        if (path.empty())
            return Result<void>::failure(
                file_error("事务目录路径为空", "event.file.createExclusive"));
        std::error_code error;
        const bool created = std::filesystem::create_directory(path, error);
        if (error || !created)
        {
            const auto code = error ? static_cast<DWORD>(error.value()) : ERROR_ALREADY_EXISTS;
            return Result<void>::failure(
                file_error("无法唯一创建事件事务目录", "event.file.createExclusive", code));
        }
        return Result<void>::success();
    }

    Result<void> write_new_file_durable(const std::filesystem::path& path,
                                        const std::span<const std::byte> contents) override
    {
        if (path.empty() || contents.empty())
            return Result<void>::failure(file_error("事件文件路径或内容为空", "event.file.write"));

        UniqueHandle file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr)};
        if (!file.valid())
            return Result<void>::failure(
                file_error("无法创建事件文件", "event.file.create", GetLastError()));

        std::size_t written_total = 0U;
        while (written_total < contents.size())
        {
            const auto remaining = contents.size() - written_total;
            const DWORD chunk = static_cast<DWORD>(
                (std::min)(remaining,
                           static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written = 0U;
            if (WriteFile(file.get(), contents.data() + written_total, chunk, &written, nullptr) ==
                    FALSE ||
                written == 0U)
            {
                return Result<void>::failure(
                    file_error("事件文件写入不完整", "event.file.write", GetLastError()));
            }
            written_total += written;
        }
        if (FlushFileBuffers(file.get()) == FALSE)
            return Result<void>::failure(
                file_error("无法刷新事件文件", "event.file.flush", GetLastError()));
        return Result<void>::success();
    }

    Result<std::vector<std::byte>> read_file_bounded(const std::filesystem::path& path,
                                                     const std::size_t maximum_bytes) override
    {
        if (path.empty() || maximum_bytes == 0U)
            return Result<std::vector<std::byte>>::failure(
                file_error("事件文件读取参数无效", "event.file.read"));
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error)
            return Result<std::vector<std::byte>>::failure(
                file_error("事件文件不存在或类型错误", "event.file.inspect",
                           static_cast<DWORD>(error.value())));
        const auto size = std::filesystem::file_size(path, error);
        if (error || size == 0U || size > maximum_bytes ||
            size > static_cast<std::uintmax_t>((std::numeric_limits<std::size_t>::max)()))
            return Result<std::vector<std::byte>>::failure(
                file_error("事件文件为空、过大或无法读取大小", "event.file.inspect",
                           static_cast<DWORD>(error.value())));

        try
        {
            std::vector<std::byte> contents(static_cast<std::size_t>(size));
            std::ifstream stream{path, std::ios::binary};
            if (!stream || !stream.read(reinterpret_cast<char*>(contents.data()),
                                        static_cast<std::streamsize>(contents.size())))
                return Result<std::vector<std::byte>>::failure(
                    file_error("事件文件读取不完整", "event.file.read"));
            return Result<std::vector<std::byte>>::success(std::move(contents));
        }
        catch (const std::exception&)
        {
            return Result<std::vector<std::byte>>::failure(
                file_error("事件文件读取内存预算不足", "event.file.read"));
        }
    }

    Result<EventPathKind> path_kind(const std::filesystem::path& path) override
    {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error)
        {
            if (error == std::errc::no_such_file_or_directory)
                return Result<EventPathKind>::success(EventPathKind::missing);
            return Result<EventPathKind>::failure(file_error(
                "无法检查事件路径", "event.file.inspect", static_cast<DWORD>(error.value())));
        }
        if (!std::filesystem::exists(status))
            return Result<EventPathKind>::success(EventPathKind::missing);
        if (std::filesystem::is_symlink(status))
            return Result<EventPathKind>::success(EventPathKind::other);
        if (std::filesystem::is_regular_file(status))
            return Result<EventPathKind>::success(EventPathKind::regular_file);
        if (std::filesystem::is_directory(status))
            return Result<EventPathKind>::success(EventPathKind::directory);
        return Result<EventPathKind>::success(EventPathKind::other);
    }

    Result<std::vector<std::filesystem::path>> list_directories_bounded(
        const std::filesystem::path& path, const std::size_t maximum_entries) override
    {
        if (maximum_entries == 0U)
            return Result<std::vector<std::filesystem::path>>::failure(
                file_error("目录扫描上限为零", "event.file.list"));
        std::vector<std::filesystem::path> directories;
        std::error_code error;
        if (!std::filesystem::exists(path, error) && !error)
            return Result<std::vector<std::filesystem::path>>::success(std::move(directories));
        for (std::filesystem::directory_iterator iterator{path, error}, end;
             !error && iterator != end; iterator.increment(error))
        {
            std::error_code type_error;
            if (iterator->is_symlink(type_error))
                continue;
            if (!type_error && iterator->is_directory(type_error) && !type_error)
            {
                if (directories.size() == maximum_entries)
                    return Result<std::vector<std::filesystem::path>>::failure(
                        file_error("残留事件目录数量超过扫描上限", "event.file.list"));
                directories.push_back(iterator->path());
            }
        }
        if (error)
            return Result<std::vector<std::filesystem::path>>::failure(file_error(
                "无法扫描事件目录", "event.file.list", static_cast<DWORD>(error.value())));
        std::ranges::sort(directories);
        return Result<std::vector<std::filesystem::path>>::success(std::move(directories));
    }

    Result<void> move_directory_atomically(const std::filesystem::path& source,
                                           const std::filesystem::path& destination) override
    {
        auto source_volume = volume_root_for(source);
        if (!source_volume)
            return Result<void>::failure(std::move(source_volume).error());
        auto destination_volume = volume_root_for(destination.parent_path());
        if (!destination_volume)
            return Result<void>::failure(std::move(destination_volume).error());
        if (source_volume.value() != destination_volume.value())
            return Result<void>::failure(file_error("事件事务目录和正式目录不在同一卷",
                                                    "event.file.atomicMove",
                                                    ERROR_NOT_SAME_DEVICE));
        if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE)
            return Result<void>::failure(
                file_error("无法原子提交事件目录", "event.file.atomicMove", GetLastError()));
        return Result<void>::success();
    }
};

} // namespace

std::shared_ptr<IEventFileSystem> make_windows_event_file_system()
{
    return std::make_shared<WindowsEventFileSystem>();
}

} // namespace paperbreak::storage
