#include "paperbreak/storage/event_store.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <exception>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

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

class CngSha256 final
{
  public:
    CngSha256() = default;
    ~CngSha256()
    {
        if (hash_ != nullptr)
            static_cast<void>(BCryptDestroyHash(hash_));
        if (algorithm_ != nullptr)
            static_cast<void>(BCryptCloseAlgorithmProvider(algorithm_, 0U));
    }
    CngSha256(const CngSha256&) = delete;
    CngSha256& operator=(const CngSha256&) = delete;

    [[nodiscard]] Result<void> initialize()
    {
        auto status =
            BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0U);
        if (!BCRYPT_SUCCESS(status))
            return failure("无法打开 Windows CNG SHA-256", "event.file.hash.open", status);
        status = BCryptCreateHash(algorithm_, &hash_, nullptr, 0U, nullptr, 0U, 0U);
        if (!BCRYPT_SUCCESS(status))
            return failure("无法创建 Windows CNG SHA-256", "event.file.hash.create", status);
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> update(const std::span<const std::byte> bytes)
    {
        std::size_t consumed{};
        while (consumed < bytes.size())
        {
            const auto count = static_cast<ULONG>(
                (std::min)(bytes.size() - consumed,
                           static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())));
            const auto status = BCryptHashData(
                hash_, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data() + consumed)),
                count, 0U);
            if (!BCRYPT_SUCCESS(status))
                return failure("Windows CNG SHA-256 更新失败", "event.file.hash.update", status);
            consumed += count;
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<std::string> finish()
    {
        std::array<UCHAR, 32U> digest{};
        const auto status =
            BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0U);
        if (!BCRYPT_SUCCESS(status))
            return Result<std::string>::failure(
                failure_error("Windows CNG SHA-256 完成失败", "event.file.hash.finish", status));
        constexpr char digits[] = "0123456789abcdef";
        std::string text(digest.size() * 2U, '0');
        for (std::size_t index = 0U; index < digest.size(); ++index)
        {
            text[index * 2U] = digits[(digest[index] >> 4U) & 0x0FU];
            text[index * 2U + 1U] = digits[digest[index] & 0x0FU];
        }
        return Result<std::string>::success(std::move(text));
    }

  private:
    static Error failure_error(std::string message, std::string operation, const NTSTATUS status)
    {
        auto error = file_error(std::move(message), std::move(operation));
        error.native_domain = "cng";
        error.native_code = std::to_string(static_cast<std::int64_t>(status));
        return error;
    }

    static Result<void> failure(std::string message, std::string operation, const NTSTATUS status)
    {
        return Result<void>::failure(
            failure_error(std::move(message), std::move(operation), status));
    }

    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_HASH_HANDLE hash_{};
};

Result<BufferedFileWriteResult> write_buffered_file(const std::filesystem::path& path,
                                                    const std::span<const std::byte> contents,
                                                    const std::stop_token stop_token)
{
    if (path.empty())
        return Result<BufferedFileWriteResult>::failure(
            file_error("事件文件路径为空", "event.file.write"));
    CngSha256 hash;
    if (auto initialized = hash.initialize(); !initialized)
        return Result<BufferedFileWriteResult>::failure(std::move(initialized).error());
    std::uint64_t written_total{};
    {
        UniqueHandle file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        if (!file.valid())
            return Result<BufferedFileWriteResult>::failure(
                file_error("无法创建事件文件", "event.file.create", GetLastError()));
        while (written_total < contents.size())
        {
            if (stop_token.stop_requested())
                return Result<BufferedFileWriteResult>::failure(
                    file_error("事件文件写入已取消", "event.file.write.cancel"));
            const auto remaining = contents.size() - static_cast<std::size_t>(written_total);
            const DWORD chunk = static_cast<DWORD>(
                (std::min)(remaining,
                           static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written{};
            if (WriteFile(file.get(), contents.data() + written_total, chunk, &written, nullptr) ==
                    FALSE ||
                written == 0U || written > chunk)
                return Result<BufferedFileWriteResult>::failure(
                    file_error("事件文件发生短写", "event.file.write", GetLastError()));
            auto hashed = hash.update(contents.subspan(static_cast<std::size_t>(written_total),
                                                       static_cast<std::size_t>(written)));
            if (!hashed)
                return Result<BufferedFileWriteResult>::failure(std::move(hashed).error());
            written_total += written;
        }
    }
    auto digest = hash.finish();
    if (!digest)
        return Result<BufferedFileWriteResult>::failure(std::move(digest).error());
    return Result<BufferedFileWriteResult>::success(
        {.bytes_written = written_total, .sha256 = std::move(digest).value()});
}

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

    Result<BufferedFileWriteResult> write_new_file_buffered(
        const std::filesystem::path& path, const std::span<const std::byte> contents,
        const std::stop_token stop_token) override
    {
        return write_buffered_file(path, contents, stop_token);
    }

    Result<BufferedFileWriteResult> write_new_raw_block_buffered(
        const std::filesystem::path& path, const std::span<const std::byte> contents,
        const std::stop_token stop_token) override
    {
        if (path.empty() || contents.size() <= 8U)
            return Result<BufferedFileWriteResult>::failure(
                file_error("原始块路径或内容无效", "event.file.block.write"));
        auto partial = path;
        partial += L".partial";
        auto written = write_buffered_file(partial, contents, stop_token);
        if (!written)
            return written;
        if (MoveFileExW(partial.c_str(), path.c_str(), 0U) == FALSE)
            return Result<BufferedFileWriteResult>::failure(
                file_error("无法原子发布原始块", "event.file.block.publish", GetLastError()));
        return written;
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

    Result<HashedFileContents> read_file_bounded_hashed(const std::filesystem::path& path,
                                                        const std::size_t maximum_bytes) override
    {
        if (path.empty() || maximum_bytes == 0U)
            return Result<HashedFileContents>::failure(
                file_error("事件文件读取参数无效", "event.file.readHashed"));
        std::error_code size_error;
        const auto size = std::filesystem::file_size(path, size_error);
        if (size_error || size == 0U || size > maximum_bytes ||
            size > static_cast<std::uintmax_t>((std::numeric_limits<std::size_t>::max)()))
            return Result<HashedFileContents>::failure(
                file_error("事件文件为空、过大或无法读取大小", "event.file.readHashed.size",
                           static_cast<DWORD>(size_error.value())));
        UniqueHandle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        if (!file.valid())
            return Result<HashedFileContents>::failure(
                file_error("无法打开事件文件", "event.file.readHashed.open", GetLastError()));
        CngSha256 hash;
        if (auto initialized = hash.initialize(); !initialized)
            return Result<HashedFileContents>::failure(std::move(initialized).error());
        std::vector<std::byte> contents;
        try
        {
            contents.resize(static_cast<std::size_t>(size));
        }
        catch (const std::exception&)
        {
            return Result<HashedFileContents>::failure(
                file_error("无法分配事件文件读取缓冲区", "event.file.readHashed.allocate"));
        }
        std::size_t read_total{};
        while (read_total < contents.size())
        {
            const DWORD requested = static_cast<DWORD>(
                (std::min)(contents.size() - read_total,
                           static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD read{};
            if (ReadFile(file.get(), contents.data() + read_total, requested, &read, nullptr) ==
                    FALSE ||
                read == 0U || read > requested)
                return Result<HashedFileContents>::failure(
                    file_error("事件文件发生短读", "event.file.readHashed.read", GetLastError()));
            if (auto updated = hash.update(
                    std::span{contents}.subspan(read_total, static_cast<std::size_t>(read)));
                !updated)
                return Result<HashedFileContents>::failure(std::move(updated).error());
            read_total += read;
        }
        auto digest = hash.finish();
        if (!digest)
            return Result<HashedFileContents>::failure(std::move(digest).error());
        return Result<HashedFileContents>::success(
            {.contents = std::move(contents), .sha256 = std::move(digest).value()});
    }

    Result<std::uint64_t> file_size(const std::filesystem::path& path) override
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error)
            return Result<std::uint64_t>::failure(file_error(
                "事件文件不存在或类型错误", "event.file.size", static_cast<DWORD>(error.value())));
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > (std::numeric_limits<std::uint64_t>::max)())
            return Result<std::uint64_t>::failure(file_error(
                "无法读取事件文件大小", "event.file.size", static_cast<DWORD>(error.value())));
        return Result<std::uint64_t>::success(static_cast<std::uint64_t>(size));
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
        if (MoveFileExW(source.c_str(), destination.c_str(), 0U) == FALSE)
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
