#include "paperbreak/platform/atomic_file.hpp"

#include <Windows.h>

#include <atomic>
#include <fstream>
#include <limits>
#include <system_error>

namespace paperbreak::platform
{
namespace
{

Error file_error(std::string message, std::string operation, const DWORD native_code = 0)
{
    Error error = make_error("SYS_CONFIG_PERSIST_FAILED", Severity::critical, std::move(message),
                             "platform", std::move(operation), true);
    if (native_code != 0)
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
        if (value_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(value_);
        }
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

    void close() noexcept
    {
        if (value_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(value_);
            value_ = INVALID_HANDLE_VALUE;
        }
    }

  private:
    HANDLE value_;
};

std::filesystem::path temporary_path_for(const std::filesystem::path& target)
{
    static std::atomic_uint64_t sequence{0};
    const auto suffix = L".paperbreak.tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
                        std::to_wstring(sequence.fetch_add(1U, std::memory_order_relaxed));
    return target.parent_path() / (target.filename().wstring() + suffix);
}

} // namespace

Result<std::string> WindowsAtomicFileSystem::read_bounded(const std::filesystem::path& path,
                                                          const std::size_t maximum_bytes)
{
    if (maximum_bytes == 0U)
    {
        return Result<std::string>::failure(
            file_error("文件读取上限不能为零", "platform.file.read"));
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
    {
        return Result<std::string>::failure(
            file_error("目标不存在或不是普通文件", "platform.file.inspect"));
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0U || size > maximum_bytes ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
    {
        return Result<std::string>::failure(
            file_error("文件为空、过大或无法读取大小", "platform.file.inspect"));
    }
    std::ifstream stream{path, std::ios::binary};
    std::string contents(static_cast<std::size_t>(size), '\0');
    if (!stream || !stream.read(contents.data(), static_cast<std::streamsize>(contents.size())))
    {
        return Result<std::string>::failure(
            file_error("文件读取不完整", "platform.file.read"));
    }
    return Result<std::string>::success(std::move(contents));
}

Result<void> WindowsAtomicFileSystem::create_directories(const std::filesystem::path& path)
{
    std::error_code error;
    static_cast<void>(std::filesystem::create_directories(path, error));
    if (error)
    {
        return Result<void>::failure(file_error("无法创建目录", "platform.file.createDirectory",
                                                static_cast<DWORD>(error.value())));
    }
    return Result<void>::success();
}

Result<std::vector<std::filesystem::path>> WindowsAtomicFileSystem::list_regular_files(
    const std::filesystem::path& path)
{
    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (!std::filesystem::exists(path, error))
    {
        return Result<std::vector<std::filesystem::path>>::success(std::move(files));
    }
    for (std::filesystem::directory_iterator iterator{path, error}, end; !error && iterator != end;
         iterator.increment(error))
    {
        std::error_code type_error;
        if (iterator->is_regular_file(type_error) && !type_error)
        {
            files.push_back(iterator->path());
        }
    }
    if (error)
    {
        return Result<std::vector<std::filesystem::path>>::failure(file_error(
            "无法枚举目录", "platform.file.list", static_cast<DWORD>(error.value())));
    }
    return Result<std::vector<std::filesystem::path>>::success(std::move(files));
}

Result<void> WindowsAtomicFileSystem::remove_file(const std::filesystem::path& path)
{
    std::error_code error;
    static_cast<void>(std::filesystem::remove(path, error));
    if (error)
    {
        return Result<void>::failure(file_error("无法删除文件", "platform.file.remove",
                                                static_cast<DWORD>(error.value())));
    }
    return Result<void>::success();
}

Result<void> WindowsAtomicFileSystem::replace_atomically(
    const std::filesystem::path& target, const std::string_view contents,
    const std::optional<std::filesystem::path>& backup)
{
    if (target.empty() || contents.empty())
    {
        return Result<void>::failure(
            file_error("原子写入目标或内容为空", "platform.file.atomicReplace"));
    }
    auto directory_result = create_directories(target.parent_path());
    if (!directory_result)
    {
        return directory_result;
    }

    const auto temporary = temporary_path_for(target);
    UniqueHandle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr)};
    if (!file.valid())
    {
        return Result<void>::failure(file_error("无法创建同目录临时文件",
                                                "platform.file.createTemporary", GetLastError()));
    }

    std::size_t written_total = 0U;
    while (written_total < contents.size())
    {
        const auto remaining = contents.size() - written_total;
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (WriteFile(file.get(), contents.data() + written_total, chunk, &written, nullptr) ==
                FALSE ||
            written == 0U)
        {
            const DWORD code = GetLastError();
            file.close();
            static_cast<void>(DeleteFileW(temporary.c_str()));
            return Result<void>::failure(
                file_error("临时文件写入失败", "platform.file.writeTemporary", code));
        }
        written_total += written;
    }
    if (FlushFileBuffers(file.get()) == FALSE)
    {
        const DWORD code = GetLastError();
        file.close();
        static_cast<void>(DeleteFileW(temporary.c_str()));
        return Result<void>::failure(
            file_error("临时文件刷新失败", "platform.file.flushTemporary", code));
    }
    file.close();

    std::error_code exists_error;
    const bool exists = std::filesystem::exists(target, exists_error);
    if (exists_error)
    {
        static_cast<void>(DeleteFileW(temporary.c_str()));
        return Result<void>::failure(
            file_error("无法检查原子替换目标", "platform.file.atomicReplace",
                       static_cast<DWORD>(exists_error.value())));
    }
    BOOL replaced = FALSE;
    if (exists)
    {
        replaced = ReplaceFileW(target.c_str(), temporary.c_str(),
                                backup.has_value() ? backup->c_str() : nullptr,
                                REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
    }
    else
    {
        replaced = MoveFileExW(temporary.c_str(), target.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    if (replaced == FALSE)
    {
        const DWORD code = GetLastError();
        static_cast<void>(DeleteFileW(temporary.c_str()));
        return Result<void>::failure(
            file_error("无法原子替换目标文件", "platform.file.atomicReplace", code));
    }
    return Result<void>::success();
}

} // namespace paperbreak::platform
