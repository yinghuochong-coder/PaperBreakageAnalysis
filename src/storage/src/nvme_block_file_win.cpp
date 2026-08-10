#include "paperbreak/storage/nvme_cache.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <vector>

namespace paperbreak::storage
{
namespace
{

constexpr std::size_t write_chunk_bytes = 1024U * 1024U;

Error file_error(std::string code, const Severity severity, std::string message,
                 std::string operation, const std::filesystem::path& path,
                 const std::string_view reason, const std::optional<DWORD> native = std::nullopt)
{
    auto error = make_error(std::move(code), severity, std::move(message), "storage",
                            std::move(operation), true);
    const auto utf8_path = path.u8string();
    error.details.push_back(
        {"path", std::string{reinterpret_cast<const char*>(utf8_path.data()), utf8_path.size()}});
    error.details.push_back({"reason", std::string{reason}});
    if (native)
    {
        error.native_domain = "win32";
        error.native_code = std::to_string(*native);
    }
    return error;
}

template <typename T>
void put_little(std::span<std::byte> destination, const std::size_t offset, const T value) noexcept
{
    using Unsigned = std::make_unsigned_t<T>;
    const auto converted = static_cast<Unsigned>(value);
    for (std::size_t index = 0U; index < sizeof(T); ++index)
    {
        destination[offset + index] =
            static_cast<std::byte>((converted >> (index * 8U)) & static_cast<Unsigned>(0xFFU));
    }
}

std::int64_t wall_nanoseconds(const camera::WallClockTime value) noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}

std::uint64_t monotonic_delta_nanoseconds(const camera::MonotonicTime value,
                                          const camera::MonotonicTime start) noexcept
{
    if (value <= start)
        return 0U;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(value - start).count());
}

std::uint16_t pixel_format_id(const camera::PixelFormat value) noexcept
{
    switch (value)
    {
    case camera::PixelFormat::mono8:
        return 1U;
    case camera::PixelFormat::mono10:
        return 2U;
    case camera::PixelFormat::mono12:
        return 3U;
    case camera::PixelFormat::bayer_rg8:
        return 4U;
    }
    return 0U;
}

std::string block_id_text(const std::array<std::byte, 16U>& id)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : id)
        output << std::setw(2) << std::to_integer<unsigned>(byte);
    return output.str();
}

std::string safe_camera_name(const std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const unsigned char byte : value)
        result.push_back((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                 (byte >= '0' && byte <= '9') || byte == '-' || byte == '_'
                             ? static_cast<char>(byte)
                             : '_');
    return result;
}

class RateGate final
{
  public:
    RateGate(const std::uint64_t bytes_per_second,
             const std::chrono::steady_clock::time_point deadline, const std::stop_token token)
        : bytes_per_second_(bytes_per_second), deadline_(deadline), token_(token),
          started_(std::chrono::steady_clock::now())
    {
    }

    [[nodiscard]] bool before(const std::size_t bytes)
    {
        if (token_.stop_requested() || std::chrono::steady_clock::now() >= deadline_)
            return false;
        const auto next_total = written_ + bytes;
        const auto required = std::chrono::duration<long double>{
            static_cast<long double>(next_total) / static_cast<long double>(bytes_per_second_)};
        const auto target =
            started_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(required);
        if (target > deadline_)
            return false;
        if (target > std::chrono::steady_clock::now())
        {
            std::unique_lock lock{mutex_};
            condition_.wait_until(lock, token_, target, [] { return false; });
        }
        if (token_.stop_requested() || std::chrono::steady_clock::now() >= deadline_)
            return false;
        written_ = next_total;
        return true;
    }

  private:
    std::uint64_t bytes_per_second_{};
    std::chrono::steady_clock::time_point deadline_;
    std::stop_token token_;
    std::chrono::steady_clock::time_point started_;
    std::uint64_t written_{};
    std::mutex mutex_;
    std::condition_variable_any condition_;
};

class WindowsNvmeBlockStore final : public INvmeBlockStore
{
  public:
    Result<void> prepare(const std::filesystem::path& root) override
    {
        std::error_code error;
        std::filesystem::create_directories(root, error);
        if (error)
            return Result<void>::failure(
                file_error("NVME_CACHE_UNAVAILABLE", Severity::error, "无法创建 NVMe 缓存目录",
                           "storage.nvme.prepare", root, "create-directory-failed",
                           static_cast<DWORD>(error.value())));
        return Result<void>::success();
    }

    Result<NvmeCommittedBlock> write_block(const NvmeWriteRequest& request,
                                           const std::stop_token stop_token) override
    {
        if (!begin_active_thread())
        {
            return Result<NvmeCommittedBlock>::failure(
                file_error("NVME_WRITE_FAILED", Severity::error, "无法建立 NVMe I/O 取消句柄",
                           "storage.nvme.write", request.root, "duplicate-thread-handle-failed",
                           GetLastError()));
        }
        ActiveThreadGuard active_thread_guard{*this};
        if (request.block == nullptr || request.write_limit_bytes_per_second == 0U)
        {
            return Result<NvmeCommittedBlock>::failure(
                file_error("NVME_WRITE_FAILED", Severity::error, "NVMe 块写入请求无效",
                           "storage.nvme.write", request.root, "invalid-request"));
        }
        const auto& block = *request.block;
        const auto maximum =
            maximum_nvme_block_bytes(block.index_capacity, block.maximum_frame_bytes);
        if (!maximum || block.frames.empty() || block.frames.size() > block.index_capacity ||
            block.camera_id.empty() || block.camera_id.size() > 16U)
        {
            return Result<NvmeCommittedBlock>::failure(
                file_error("NVME_BLOCK_INVALID", Severity::error, "NVMe 块内容无效",
                           "storage.nvme.write", request.root, "invalid-block"));
        }
        const auto name = safe_camera_name(block.camera_id) + "-" +
                          std::to_string(block.generation) + "-" + block_id_text(block.block_id);
        const auto temporary = request.root / (name + ".partial");
        const auto committed = request.root / (name + ".pbnvme");
        if (std::chrono::steady_clock::now() >= request.deadline)
            return Result<NvmeCommittedBlock>::failure(timeout_error(temporary));

        const HANDLE exclusive =
            CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (exclusive == INVALID_HANDLE_VALUE)
            return Result<NvmeCommittedBlock>::failure(
                file_error("NVME_WRITE_FAILED", Severity::error, "无法唯一创建 NVMe 临时块",
                           "storage.nvme.create", temporary, "create-new-failed", GetLastError()));
        static_cast<void>(CloseHandle(exclusive));
        std::fstream stream{temporary, std::ios::binary | std::ios::in | std::ios::out};
        if (!stream)
            return Result<NvmeCommittedBlock>::failure(
                file_error("NVME_WRITE_FAILED", Severity::error, "无法创建 NVMe 临时块",
                           "storage.nvme.create", temporary, "open-failed"));
        stream.seekp(static_cast<std::streamoff>(maximum.value() - 1U));
        stream.put('\0');
        if (!stream)
            return Result<NvmeCommittedBlock>::failure(
                file_error("NVME_WRITE_FAILED", Severity::error, "无法预分配 NVMe 块",
                           "storage.nvme.preallocate", temporary, "preallocate-failed"));

        RateGate gate{request.write_limit_bytes_per_second, request.deadline, stop_token};
        std::array<std::byte, nvme_page_bytes> header{};
        constexpr std::array<std::byte, 8U> header_magic{
            std::byte{'P'}, std::byte{'B'}, std::byte{'N'}, std::byte{'V'},
            std::byte{'M'}, std::byte{'E'}, std::byte{'2'}, std::byte{0U}};
        std::ranges::copy(header_magic, header.begin());
        put_little<std::uint16_t>(header, 8U, nvme_format_version);
        put_little<std::uint16_t>(header, 10U, nvme_page_bytes);
        put_little<std::uint32_t>(header, 12U, 0x01020304U);
        std::ranges::copy(block.block_id, header.begin() + 16U);
        put_little<std::uint64_t>(header, 32U, block.generation);
        std::ranges::copy(std::as_bytes(std::span{block.camera_id.data(), block.camera_id.size()}),
                          header.begin() + 40U);
        put_little<std::uint16_t>(header, 56U, static_cast<std::uint16_t>(block.camera_id.size()));
        const auto first_geometry = block.frames.front().geometry();
        const auto first_format = block.frames.front().pixel_format();
        put_little<std::uint16_t>(header, 58U, pixel_format_id(first_format));
        put_little<std::uint32_t>(header, 60U, first_geometry.width);
        put_little<std::uint32_t>(header, 64U, first_geometry.height);
        put_little<std::uint32_t>(header, 68U, first_geometry.stride);
        const bool camera_timestamp_present = std::ranges::any_of(
            block.frames, [](const auto& frame) { return frame.camera_timestamp().has_value(); });
        const bool camera_timestamp_synchronized =
            camera_timestamp_present && std::ranges::all_of(block.frames, [](const auto& frame) {
                return frame.camera_timestamp() && frame.camera_timestamp()->quality ==
                                                       camera::CameraTimestampQuality::synchronized;
            });
        const std::uint32_t header_flags = (camera_timestamp_present ? 1U << 1U : 0U) |
                                           (camera_timestamp_synchronized ? 1U << 2U : 0U);
        put_little<std::uint32_t>(header, 72U, header_flags);
        put_little<std::uint32_t>(header, 76U,
                                  static_cast<std::uint32_t>(nvme_block_duration.count()));
        put_little<std::uint32_t>(header, 80U, nvme_index_entry_bytes);
        put_little<std::uint32_t>(header, 84U, block.index_capacity);
        put_little<std::uint32_t>(header, 88U, nvme_page_bytes);
        put_little<std::uint32_t>(header, 92U, block.maximum_frame_bytes);
        put_little<std::int64_t>(header, 96U, wall_nanoseconds(block.start_wall_clock_time));
        if (const auto& timestamp = block.frames.front().camera_timestamp())
        {
            put_little<std::uint64_t>(header, 104U, timestamp->ticks);
            put_little<std::uint64_t>(header, 112U, timestamp->frequency_hz);
        }
        put_little<std::uint64_t>(header, 120U, block.frames.front().sequence_number());
        const auto header_crc = crc32c(header);
        put_little<std::uint32_t>(header, 128U, header_crc);

        const auto index_reserved =
            align_region(static_cast<std::uint64_t>(block.index_capacity) * nvme_index_entry_bytes);
        const auto data_start = static_cast<std::uint64_t>(nvme_page_bytes) + index_reserved;
        std::vector<std::byte> index_bytes(block.frames.size() * nvme_index_entry_bytes);
        std::uint64_t valid_data_bytes{};
        for (std::size_t frame_index = 0U; frame_index < block.frames.size(); ++frame_index)
        {
            const auto& frame = block.frames[frame_index];
            const auto geometry = frame.geometry();
            if (frame.camera_id() != block.camera_id || frame.pixel_format() != first_format ||
                geometry != first_geometry || frame.bytes().size() > block.maximum_frame_bytes ||
                (frame_index > 0U &&
                 frame.sequence_number() <= block.frames[frame_index - 1U].sequence_number()))
            {
                return Result<NvmeCommittedBlock>::failure(
                    file_error("NVME_BLOCK_INVALID", Severity::error, "NVMe 块帧布局不一致",
                               "storage.nvme.encode", temporary, "frame-layout-mismatch"));
            }
            auto entry = std::span<std::byte>{index_bytes}.subspan(
                frame_index * nvme_index_entry_bytes, nvme_index_entry_bytes);
            put_little<std::uint64_t>(entry, 0U, frame.sequence_number());
            put_little<std::uint64_t>(entry, 8U, frame.camera_frame_number());
            put_little<std::uint64_t>(entry, 16U,
                                      monotonic_delta_nanoseconds(frame.received_monotonic_time(),
                                                                  block.start_monotonic_time));
            put_little<std::int64_t>(entry, 24U,
                                     wall_nanoseconds(frame.received_wall_clock_time()));
            std::uint16_t flags = frame.flags().incomplete ? 1U : 0U;
            if (const auto& timestamp = frame.camera_timestamp())
            {
                put_little<std::uint64_t>(entry, 32U, timestamp->ticks);
                put_little<std::uint64_t>(entry, 40U, timestamp->frequency_hz);
                flags |= 1U << 1U;
                flags |= timestamp->quality == camera::CameraTimestampQuality::synchronized
                             ? 1U << 2U
                         : timestamp->quality == camera::CameraTimestampQuality::unsynchronized
                             ? 1U << 3U
                             : 0U;
            }
            put_little<std::uint64_t>(entry, 48U, data_start + valid_data_bytes);
            put_little<std::uint32_t>(entry, 56U, static_cast<std::uint32_t>(frame.bytes().size()));
            put_little<std::uint32_t>(entry, 60U, geometry.width);
            put_little<std::uint32_t>(entry, 64U, geometry.height);
            put_little<std::uint32_t>(entry, 68U, geometry.stride);
            put_little<std::uint16_t>(entry, 72U, pixel_format_id(frame.pixel_format()));
            put_little<std::uint16_t>(entry, 74U, flags);
            put_little<std::uint32_t>(entry, 76U, 0U);
            put_little<std::uint32_t>(entry, 80U, crc32c(entry));
            valid_data_bytes += frame.bytes().size();
        }

        if (auto result = write_at(stream, temporary, 0U, header, gate, request.deadline); !result)
            return Result<NvmeCommittedBlock>::failure(result.error());
        if (auto result =
                write_at(stream, temporary, nvme_page_bytes, index_bytes, gate, request.deadline);
            !result)
            return Result<NvmeCommittedBlock>::failure(result.error());
        std::uint64_t data_offset = data_start;
        for (const auto& frame : block.frames)
        {
            if (auto result =
                    write_at(stream, temporary, data_offset, frame.bytes(), gate, request.deadline);
                !result)
                return Result<NvmeCommittedBlock>::failure(result.error());
            data_offset += frame.bytes().size();
        }

        std::array<std::byte, nvme_page_bytes> footer{};
        constexpr std::array<std::byte, 8U> footer_magic{
            std::byte{'P'}, std::byte{'B'}, std::byte{'C'}, std::byte{'O'},
            std::byte{'M'}, std::byte{'M'}, std::byte{'I'}, std::byte{'T'}};
        constexpr std::array<std::byte, 8U> marker{std::byte{'C'}, std::byte{'O'}, std::byte{'M'},
                                                   std::byte{'M'}, std::byte{'I'}, std::byte{'T'},
                                                   std::byte{'2'}, std::byte{0U}};
        std::ranges::copy(footer_magic, footer.begin());
        put_little<std::uint16_t>(footer, 8U, nvme_format_version);
        put_little<std::uint16_t>(footer, 10U, nvme_page_bytes);
        put_little<std::uint32_t>(footer, 12U, static_cast<std::uint32_t>(block.frames.size()));
        put_little<std::uint64_t>(footer, 16U, index_bytes.size());
        put_little<std::uint64_t>(footer, 24U, valid_data_bytes);
        put_little<std::uint64_t>(footer, 32U, maximum.value());
        put_little<std::int64_t>(footer, 40U,
                                 wall_nanoseconds(block.frames.back().received_wall_clock_time()));
        put_little<std::uint64_t>(footer, 48U, block.frames.back().sequence_number());
        const auto index_crc = crc32c(index_bytes);
        put_little<std::uint32_t>(footer, 56U, index_crc);
        put_little<std::uint32_t>(footer, 60U, 0U);
        put_little<std::uint32_t>(footer, 64U, header_crc);
        std::ranges::copy(marker, footer.begin() + 4088U);
        const auto footer_crc = crc32c(footer);
        put_little<std::uint32_t>(footer, 4084U, footer_crc);
        const auto footer_offset = maximum.value() - nvme_page_bytes;
        if (auto result =
                write_at(stream, temporary, footer_offset, footer, gate, request.deadline);
            !result)
            return Result<NvmeCommittedBlock>::failure(result.error());
        stream.flush();
        const bool footer_stream_ok = static_cast<bool>(stream);
        stream.close();
        if (!footer_stream_ok || std::chrono::steady_clock::now() >= request.deadline)
            return Result<NvmeCommittedBlock>::failure(
                std::chrono::steady_clock::now() >= request.deadline
                    ? timeout_error(temporary)
                    : file_error("NVME_WRITE_FAILED", Severity::error, "无法刷新 NVMe 提交尾页",
                                 "storage.nvme.flush", temporary, "flush-footer-failed"));
        if (MoveFileExW(temporary.c_str(), committed.c_str(), 0U) == FALSE)
        {
            return Result<NvmeCommittedBlock>::failure(
                file_error("NVME_WRITE_FAILED", Severity::error, "无法原子发布 NVMe 块",
                           "storage.nvme.publish", temporary, "rename-failed", GetLastError()));
        }
        return Result<NvmeCommittedBlock>::success({.path = committed,
                                                    .physical_bytes = maximum.value(),
                                                    .header_crc32c = header_crc,
                                                    .index_crc32c = index_crc,
                                                    .data_crc32c = 0U,
                                                    .footer_crc32c = footer_crc,
                                                    .commit_verified = true});
    }

    Result<void> remove_committed(const std::filesystem::path& path) override
    {
        std::error_code error;
        const bool removed = std::filesystem::remove(path, error);
        if (error || !removed)
            return Result<void>::failure(file_error(
                "NVME_WRITE_FAILED", Severity::error, "无法回收最旧 NVMe 普通块",
                "storage.nvme.reclaim", path, error ? "remove-failed" : "file-missing",
                error ? std::optional<DWORD>{static_cast<DWORD>(error.value())} : std::nullopt));
        return Result<void>::success();
    }

    void cancel_pending() noexcept override
    {
        std::scoped_lock lock{active_thread_mutex_};
        if (active_thread_ != nullptr)
            static_cast<void>(CancelSynchronousIo(active_thread_));
    }

  private:
    class ActiveThreadGuard final
    {
      public:
        explicit ActiveThreadGuard(WindowsNvmeBlockStore& owner) : owner_(owner) {}
        ~ActiveThreadGuard()
        {
            owner_.end_active_thread();
        }

      private:
        WindowsNvmeBlockStore& owner_;
    };

    [[nodiscard]] bool begin_active_thread() noexcept
    {
        HANDLE thread{};
        if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
                            0U, FALSE, DUPLICATE_SAME_ACCESS) == FALSE)
            return false;
        std::scoped_lock lock{active_thread_mutex_};
        active_thread_ = thread;
        return true;
    }

    void end_active_thread() noexcept
    {
        std::scoped_lock lock{active_thread_mutex_};
        if (active_thread_ != nullptr)
        {
            CloseHandle(active_thread_);
            active_thread_ = nullptr;
        }
    }

    static std::uint64_t align_region(const std::uint64_t value) noexcept
    {
        return (value + nvme_page_bytes - 1U) & ~(static_cast<std::uint64_t>(nvme_page_bytes) - 1U);
    }

    static Error timeout_error(const std::filesystem::path& path)
    {
        return file_error("NVME_WRITE_TIMEOUT", Severity::error, "NVMe 块写入超过截止时间",
                          "storage.nvme.write", path, "deadline-exceeded");
    }

    static Result<void> write_at(std::fstream& stream, const std::filesystem::path& path,
                                 std::uint64_t offset, std::span<const std::byte> bytes,
                                 RateGate& gate,
                                 const std::chrono::steady_clock::time_point deadline)
    {
        std::size_t consumed{};
        while (consumed < bytes.size())
        {
            const auto count = std::min(write_chunk_bytes, bytes.size() - consumed);
            if (!gate.before(count) || std::chrono::steady_clock::now() >= deadline)
                return Result<void>::failure(timeout_error(path));
            stream.seekp(static_cast<std::streamoff>(offset + consumed));
            stream.write(reinterpret_cast<const char*>(bytes.data() + consumed),
                         static_cast<std::streamsize>(count));
            if (!stream)
                return Result<void>::failure(file_error("NVME_WRITE_FAILED", Severity::error,
                                                        "NVMe 块发生短写", "storage.nvme.write",
                                                        path, "short-write"));
            if (std::chrono::steady_clock::now() >= deadline)
                return Result<void>::failure(timeout_error(path));
            consumed += count;
        }
        return Result<void>::success();
    }

    std::mutex active_thread_mutex_;
    HANDLE active_thread_{};
};

} // namespace

std::shared_ptr<INvmeBlockStore> make_windows_nvme_block_store()
{
    return std::make_shared<WindowsNvmeBlockStore>();
}

} // namespace paperbreak::storage
