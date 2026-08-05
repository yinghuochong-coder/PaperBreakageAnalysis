#include "paperbreak/storage/nvme_recovery.hpp"

#include "paperbreak/storage/nvme_cache.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace paperbreak::storage
{
namespace
{

constexpr std::size_t recovery_read_chunk_bytes = 1024U * 1024U;
constexpr std::array<std::byte, 8U> header_magic{std::byte{'P'}, std::byte{'B'}, std::byte{'N'},
                                                 std::byte{'V'}, std::byte{'M'}, std::byte{'E'},
                                                 std::byte{'1'}, std::byte{0U}};
constexpr std::array<std::byte, 8U> footer_magic{std::byte{'P'}, std::byte{'B'}, std::byte{'C'},
                                                 std::byte{'O'}, std::byte{'M'}, std::byte{'M'},
                                                 std::byte{'I'}, std::byte{'T'}};
constexpr std::array<std::byte, 8U> commit_marker{std::byte{'C'}, std::byte{'O'}, std::byte{'M'},
                                                  std::byte{'M'}, std::byte{'I'}, std::byte{'T'},
                                                  std::byte{'1'}, std::byte{0U}};

Error recovery_error(std::string code, const Severity severity, std::string message,
                     std::string operation, const std::filesystem::path& path,
                     const std::string_view reason,
                     const std::optional<DWORD> native = std::nullopt)
{
    auto error = make_error(std::move(code), severity, std::move(message), "storage",
                            std::move(operation), true);
    if (!path.empty())
    {
        const auto utf8 = path.u8string();
        error.details.push_back(
            {"path", std::string{reinterpret_cast<const char*>(utf8.data()), utf8.size()}});
    }
    error.details.push_back({"reason", std::string{reason}});
    if (native)
    {
        error.native_domain = "win32";
        error.native_code = std::to_string(*native);
    }
    return error;
}

template <typename T>
T get_little(const std::span<const std::byte> bytes, const std::size_t offset) noexcept
{
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned value{};
    for (std::size_t index = 0U; index < sizeof(T); ++index)
    {
        value |= static_cast<Unsigned>(std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (index * 8U);
    }
    if constexpr (std::is_signed_v<T>)
        return std::bit_cast<T>(value);
    else
        return value;
}

template <typename T>
void put_little(const std::span<std::byte> bytes, const std::size_t offset, const T value) noexcept
{
    using Unsigned = std::make_unsigned_t<T>;
    const auto converted = std::bit_cast<Unsigned>(value);
    for (std::size_t index = 0U; index < sizeof(T); ++index)
    {
        bytes[offset + index] =
            static_cast<std::byte>((converted >> (index * 8U)) & static_cast<Unsigned>(0xFFU));
    }
}

bool all_zero(const std::span<const std::byte> bytes) noexcept
{
    return std::ranges::all_of(bytes, [](const auto byte) { return byte == std::byte{0U}; });
}

bool matches(const std::span<const std::byte> bytes,
             const std::span<const std::byte> expected) noexcept
{
    return bytes.size() == expected.size() && std::ranges::equal(bytes, expected);
}

camera::WallClockTime wall_time(const std::int64_t nanoseconds) noexcept
{
    return camera::WallClockTime{std::chrono::duration_cast<camera::WallClockTime::duration>(
        std::chrono::nanoseconds{nanoseconds})};
}

Result<void> read_at(std::ifstream& input, const std::filesystem::path& path,
                     const std::uint64_t offset, const std::span<std::byte> destination)
{
    if (offset > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)()) ||
        destination.size() >
            static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)()))
    {
        return Result<void>::failure(
            recovery_error("NVME_RECOVERY_FAILED", Severity::error, "NVMe 恢复读取范围无效",
                           "storage.nvme.recovery.read", path, "read-range-overflow"));
    }
    input.seekg(static_cast<std::streamoff>(offset));
    input.read(reinterpret_cast<char*>(destination.data()),
               static_cast<std::streamsize>(destination.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(destination.size()))
    {
        return Result<void>::failure(
            recovery_error("NVME_RECOVERY_FAILED", Severity::error, "无法读取 NVMe 恢复块",
                           "storage.nvme.recovery.read", path, "short-read"));
    }
    return Result<void>::success();
}

Result<void> require_before_deadline(const NvmeRecoveryLimits& limits,
                                     const std::filesystem::path& path)
{
    if (std::chrono::steady_clock::now() < limits.deadline)
        return Result<void>::success();
    return Result<void>::failure(
        recovery_error("NVME_RECOVERY_LIMIT", Severity::error, "NVMe 启动恢复超过时间上限",
                       "storage.nvme.recovery.limit", path, "deadline-exceeded"));
}

struct HeaderInfo final
{
    NvmeBlockId block_id{};
    std::uint64_t generation{};
    std::string camera_id;
    std::uint16_t pixel_format{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t stride{};
    std::uint32_t flags{};
    std::uint32_t index_capacity{};
    std::uint32_t maximum_frame_bytes{};
    camera::WallClockTime start_wall_clock_time;
    std::uint64_t start_sequence{};
    std::uint64_t maximum_block_bytes{};
    std::uint64_t data_start{};
    std::uint64_t footer_offset{};
    std::uint32_t header_crc{};
};

std::optional<HeaderInfo> parse_header(std::array<std::byte, nvme_page_bytes> page,
                                       const std::uint64_t file_bytes) noexcept
{
    const auto bytes = std::span<const std::byte>{page};
    if (!matches(bytes.first(header_magic.size()), header_magic) ||
        get_little<std::uint16_t>(bytes, 8U) != nvme_format_version ||
        get_little<std::uint16_t>(bytes, 10U) != nvme_page_bytes ||
        get_little<std::uint32_t>(bytes, 12U) != 0x01020304U || !all_zero(bytes.subspan(132U)))
        return std::nullopt;
    const auto stored_header_crc = get_little<std::uint32_t>(bytes, 128U);
    put_little<std::uint32_t>(page, 128U, 0U);
    if (stored_header_crc == 0U || crc32c(page) != stored_header_crc)
        return std::nullopt;
    const auto camera_bytes = get_little<std::uint16_t>(bytes, 56U);
    const auto pixel_format = get_little<std::uint16_t>(bytes, 58U);
    const auto flags = get_little<std::uint32_t>(bytes, 72U);
    const auto index_capacity = get_little<std::uint32_t>(bytes, 84U);
    const auto maximum_frame_bytes = get_little<std::uint32_t>(bytes, 92U);
    if (camera_bytes == 0U || camera_bytes > 16U || pixel_format < 1U || pixel_format > 4U ||
        (flags & ~0x0FU) != 0U ||
        get_little<std::uint32_t>(bytes, 76U) !=
            static_cast<std::uint32_t>(nvme_block_duration.count()) ||
        get_little<std::uint32_t>(bytes, 80U) != nvme_index_entry_bytes ||
        get_little<std::uint32_t>(bytes, 88U) != nvme_page_bytes || index_capacity == 0U ||
        maximum_frame_bytes == 0U ||
        !all_zero(bytes.subspan(40U + camera_bytes, 16U - camera_bytes)))
        return std::nullopt;
    const auto width = get_little<std::uint32_t>(bytes, 60U);
    const auto height = get_little<std::uint32_t>(bytes, 64U);
    const auto stride = get_little<std::uint32_t>(bytes, 68U);
    if (width == 0U || height == 0U || stride == 0U)
        return std::nullopt;
    const auto maximum = maximum_nvme_block_bytes(index_capacity, maximum_frame_bytes);
    if (!maximum || maximum.value() != file_bytes)
        return std::nullopt;
    HeaderInfo result;
    std::ranges::copy(bytes.subspan(16U, result.block_id.size()), result.block_id.begin());
    if (all_zero(result.block_id))
        return std::nullopt;
    result.generation = get_little<std::uint64_t>(bytes, 32U);
    if (result.generation == 0U)
        return std::nullopt;
    result.camera_id.assign(reinterpret_cast<const char*>(bytes.data() + 40U), camera_bytes);
    if (result.camera_id.find('\0') != std::string::npos)
        return std::nullopt;
    result.pixel_format = pixel_format;
    result.width = width;
    result.height = height;
    result.stride = stride;
    result.flags = flags;
    result.index_capacity = index_capacity;
    result.maximum_frame_bytes = maximum_frame_bytes;
    result.start_wall_clock_time = wall_time(get_little<std::int64_t>(bytes, 96U));
    result.start_sequence = get_little<std::uint64_t>(bytes, 120U);
    result.maximum_block_bytes = maximum.value();
    const auto index_bytes = static_cast<std::uint64_t>(index_capacity) * nvme_index_entry_bytes;
    result.data_start =
        nvme_page_bytes + ((index_bytes + nvme_page_bytes - 1U) & ~(nvme_page_bytes - 1U));
    result.footer_offset = maximum.value() - nvme_page_bytes;
    result.header_crc = stored_header_crc;
    return result;
}

struct FooterInfo final
{
    std::uint32_t frame_count{};
    std::uint64_t valid_index_bytes{};
    std::uint64_t valid_data_bytes{};
    camera::WallClockTime end_wall_clock_time;
    std::uint64_t end_sequence{};
    std::uint32_t index_crc{};
    std::uint32_t data_crc{};
    std::uint32_t footer_crc{};
};

std::optional<FooterInfo> parse_footer(std::array<std::byte, nvme_page_bytes> page,
                                       const HeaderInfo& header) noexcept
{
    const auto bytes = std::span<const std::byte>{page};
    if (!matches(bytes.first(footer_magic.size()), footer_magic) ||
        get_little<std::uint16_t>(bytes, 8U) != nvme_format_version ||
        get_little<std::uint16_t>(bytes, 10U) != nvme_page_bytes ||
        !matches(bytes.subspan(4088U, commit_marker.size()), commit_marker) ||
        !all_zero(bytes.subspan(68U, 4016U)))
        return std::nullopt;
    const auto stored_crc = get_little<std::uint32_t>(bytes, 4084U);
    put_little<std::uint32_t>(page, 4084U, 0U);
    if (stored_crc == 0U || crc32c(page) != stored_crc)
        return std::nullopt;
    FooterInfo result{.frame_count = get_little<std::uint32_t>(bytes, 12U),
                      .valid_index_bytes = get_little<std::uint64_t>(bytes, 16U),
                      .valid_data_bytes = get_little<std::uint64_t>(bytes, 24U),
                      .end_wall_clock_time = wall_time(get_little<std::int64_t>(bytes, 40U)),
                      .end_sequence = get_little<std::uint64_t>(bytes, 48U),
                      .index_crc = get_little<std::uint32_t>(bytes, 56U),
                      .data_crc = get_little<std::uint32_t>(bytes, 60U),
                      .footer_crc = stored_crc};
    if (result.frame_count == 0U || result.frame_count > header.index_capacity ||
        result.valid_index_bytes !=
            static_cast<std::uint64_t>(result.frame_count) * nvme_index_entry_bytes ||
        get_little<std::uint64_t>(bytes, 32U) != header.maximum_block_bytes ||
        get_little<std::uint32_t>(bytes, 64U) != header.header_crc ||
        result.valid_data_bytes > header.footer_offset - header.data_start)
        return std::nullopt;
    return result;
}

struct EntryScan final
{
    bool complete{};
    std::uint32_t frame_count{};
    std::uint64_t valid_data_bytes{};
    camera::WallClockTime end_wall_clock_time;
    std::uint64_t start_sequence{};
    std::uint64_t end_sequence{};
    std::uint64_t sequence_gaps{};
    std::uint32_t index_crc{};
    std::uint32_t data_crc{};
};

Result<EntryScan> scan_entries(std::ifstream& input, const std::filesystem::path& path,
                               const HeaderInfo& header,
                               const std::optional<std::uint32_t> required_count,
                               const NvmeRecoveryLimits& limits)
{
    EntryScan result{.complete = true};
    std::array<std::byte, nvme_index_entry_bytes> entry{};
    std::vector<std::byte> payload(recovery_read_chunk_bytes);
    std::optional<std::uint64_t> previous_sequence;
    std::optional<std::uint64_t> previous_delta;
    std::uint64_t next_data_offset = header.data_start;
    const auto count = required_count.value_or(header.index_capacity);
    for (std::uint32_t index = 0U; index < count; ++index)
    {
        if (auto before = require_before_deadline(limits, path); !before)
            return Result<EntryScan>::failure(std::move(before).error());
        if (auto read = read_at(input, path,
                                nvme_page_bytes +
                                    static_cast<std::uint64_t>(index) * nvme_index_entry_bytes,
                                entry);
            !read)
            return Result<EntryScan>::failure(std::move(read).error());
        if (!required_count && all_zero(entry))
            break;
        const auto stored_entry_crc = get_little<std::uint32_t>(entry, 80U);
        auto crc_entry = entry;
        put_little<std::uint32_t>(crc_entry, 80U, 0U);
        const auto sequence = get_little<std::uint64_t>(entry, 0U);
        const auto received_delta = get_little<std::uint64_t>(entry, 16U);
        const auto data_offset = get_little<std::uint64_t>(entry, 48U);
        const auto payload_bytes = get_little<std::uint32_t>(entry, 56U);
        const auto flags = get_little<std::uint16_t>(entry, 74U);
        const bool layout_valid = get_little<std::uint32_t>(entry, 60U) == header.width &&
                                  get_little<std::uint32_t>(entry, 64U) == header.height &&
                                  get_little<std::uint32_t>(entry, 68U) == header.stride &&
                                  get_little<std::uint16_t>(entry, 72U) == header.pixel_format;
        const bool ordering_valid =
            (!previous_sequence || sequence > *previous_sequence) &&
            (!previous_delta || received_delta >= *previous_delta) &&
            (index != 0U || (sequence == header.start_sequence && received_delta == 0U));
        const bool bounds_valid =
            payload_bytes > 0U && payload_bytes <= header.maximum_frame_bytes &&
            data_offset == next_data_offset && data_offset <= header.footer_offset &&
            payload_bytes <= header.footer_offset - data_offset;
        const bool metadata_valid =
            stored_entry_crc != 0U && crc32c(crc_entry) == stored_entry_crc && layout_valid &&
            ordering_valid && bounds_valid && (flags & ~0x0FU) == 0U && (flags & 0x0CU) != 0x0CU &&
            all_zero(std::span<const std::byte>{entry}.subspan(84U));
        if (!metadata_valid)
        {
            result.complete = false;
            break;
        }
        std::uint32_t payload_crc{};
        auto next_data_crc = result.data_crc;
        std::uint64_t remaining = payload_bytes;
        std::uint64_t offset = data_offset;
        while (remaining > 0U)
        {
            if (auto before = require_before_deadline(limits, path); !before)
                return Result<EntryScan>::failure(std::move(before).error());
            const auto chunk = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, recovery_read_chunk_bytes));
            auto destination = std::span<std::byte>{payload}.first(chunk);
            if (auto read = read_at(input, path, offset, destination); !read)
                return Result<EntryScan>::failure(std::move(read).error());
            payload_crc = crc32c(destination, payload_crc);
            next_data_crc = crc32c(destination, next_data_crc);
            remaining -= chunk;
            offset += chunk;
        }
        if (payload_crc != get_little<std::uint32_t>(entry, 76U))
        {
            result.complete = false;
            break;
        }
        result.data_crc = next_data_crc;
        result.index_crc = crc32c(entry, result.index_crc);
        if (previous_sequence && sequence > *previous_sequence + 1U)
            result.sequence_gaps += sequence - *previous_sequence - 1U;
        if (!previous_sequence)
            result.start_sequence = sequence;
        previous_sequence = sequence;
        previous_delta = received_delta;
        result.end_sequence = sequence;
        result.end_wall_clock_time = wall_time(get_little<std::int64_t>(entry, 24U));
        ++result.frame_count;
        result.valid_data_bytes += payload_bytes;
        next_data_offset += payload_bytes;
    }
    if (required_count && result.frame_count != *required_count)
        result.complete = false;
    if (result.frame_count == 0U || result.end_wall_clock_time < header.start_wall_clock_time)
        result.complete = false;
    return Result<EntryScan>::success(std::move(result));
}

NvmeIndexedBlock make_summary(const HeaderInfo& header, const EntryScan& entries,
                              std::filesystem::path path, const std::uint32_t footer_crc)
{
    return {.block_id = header.block_id,
            .camera_id = header.camera_id,
            .generation = header.generation,
            .path = std::move(path),
            .physical_bytes = header.maximum_block_bytes,
            .start_wall_clock_time = header.start_wall_clock_time,
            .end_wall_clock_time = entries.end_wall_clock_time,
            .start_sequence_number = entries.start_sequence,
            .end_sequence_number = entries.end_sequence,
            .frame_count = entries.frame_count,
            .sequence_gaps = entries.sequence_gaps,
            .header_crc32c = header.header_crc,
            .index_crc32c = entries.index_crc,
            .data_crc32c = entries.data_crc,
            .footer_crc32c = footer_crc,
            .commit_verified = true};
}

struct Inspection final
{
    std::optional<NvmeIndexedBlock> block;
    std::optional<HeaderInfo> header;
    std::optional<EntryScan> entries;
    bool committed_marker{};
};

Result<Inspection> inspect(const std::filesystem::path& path, const bool committed_extension,
                           const NvmeRecoveryLimits& limits)
{
    std::error_code file_error;
    const auto file_bytes = std::filesystem::file_size(path, file_error);
    if (file_error)
        return Result<Inspection>::failure(
            recovery_error("NVME_RECOVERY_FAILED", Severity::error, "无法读取 NVMe 恢复块长度",
                           "storage.nvme.recovery.stat", path, "file-size-failed",
                           static_cast<DWORD>(file_error.value())));
    if (file_bytes < 2U * nvme_page_bytes)
        return Result<Inspection>::success({});
    std::ifstream input{path, std::ios::binary};
    if (!input)
        return Result<Inspection>::failure(
            recovery_error("NVME_RECOVERY_FAILED", Severity::error, "无法打开 NVMe 恢复块",
                           "storage.nvme.recovery.open", path, "open-failed"));
    std::array<std::byte, nvme_page_bytes> header_page{};
    if (auto read = read_at(input, path, 0U, header_page); !read)
        return Result<Inspection>::failure(std::move(read).error());
    auto header = parse_header(header_page, file_bytes);
    if (!header)
        return Result<Inspection>::success({});
    std::array<std::byte, nvme_page_bytes> footer_page{};
    if (auto read = read_at(input, path, header->footer_offset, footer_page); !read)
        return Result<Inspection>::failure(std::move(read).error());
    const bool has_marker =
        matches(std::span<const std::byte>{footer_page}.subspan(4088U), commit_marker);
    if (committed_extension || has_marker)
    {
        auto footer = parse_footer(footer_page, *header);
        if (!footer)
            return Result<Inspection>::success(
                {.header = std::move(header), .committed_marker = has_marker});
        auto entries = scan_entries(input, path, *header, footer->frame_count, limits);
        if (!entries)
            return Result<Inspection>::failure(std::move(entries).error());
        if (!entries.value().complete ||
            entries.value().valid_data_bytes != footer->valid_data_bytes ||
            entries.value().end_wall_clock_time != footer->end_wall_clock_time ||
            entries.value().end_sequence != footer->end_sequence ||
            entries.value().index_crc != footer->index_crc ||
            entries.value().data_crc != footer->data_crc)
        {
            return Result<Inspection>::success(
                {.header = std::move(header), .committed_marker = has_marker});
        }
        return Result<Inspection>::success(
            {.block = make_summary(*header, entries.value(), path, footer->footer_crc),
             .header = std::move(header),
             .entries = std::move(entries).value(),
             .committed_marker = has_marker});
    }
    auto entries = scan_entries(input, path, *header, std::nullopt, limits);
    if (!entries)
        return Result<Inspection>::failure(std::move(entries).error());
    if (entries.value().frame_count == 0U)
        return Result<Inspection>::success({.header = std::move(header)});
    return Result<Inspection>::success(
        {.header = std::move(header), .entries = std::move(entries).value()});
}

Result<void> flush_file(const std::filesystem::path& path)
{
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return Result<void>::failure(recovery_error(
            "NVME_RECOVERY_FAILED", Severity::error, "无法打开待持久刷新的 NVMe 恢复块",
            "storage.nvme.recovery.flush", path, "flush-open-failed", GetLastError()));
    const bool flushed = FlushFileBuffers(handle) != FALSE;
    const auto native = flushed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!flushed)
        return Result<void>::failure(
            recovery_error("NVME_RECOVERY_FAILED", Severity::error, "NVMe 恢复块持久刷新失败",
                           "storage.nvme.recovery.flush", path, "flush-failed", native));
    return Result<void>::success();
}

Result<std::pair<std::filesystem::path, std::uint32_t>> repair_partial(
    const std::filesystem::path& path, const HeaderInfo& header, const EntryScan& entries,
    const NvmeRecoveryLimits& limits)
{
    if (auto before = require_before_deadline(limits, path); !before)
        return Result<std::pair<std::filesystem::path, std::uint32_t>>::failure(
            std::move(before).error());
    auto committed = path;
    committed.replace_extension(L".pbnvme");
    std::error_code exists_error;
    if (std::filesystem::exists(committed, exists_error) || exists_error)
        return Result<std::pair<std::filesystem::path, std::uint32_t>>::failure(recovery_error(
            "NVME_RECOVERY_FAILED", Severity::error, "NVMe 恢复发布目标已存在或不可检查",
            "storage.nvme.recovery.publish", committed,
            exists_error ? "target-stat-failed" : "target-exists",
            exists_error ? std::optional<DWORD>{static_cast<DWORD>(exists_error.value())}
                         : std::nullopt));
    std::array<std::byte, nvme_page_bytes> footer{};
    std::ranges::copy(footer_magic, footer.begin());
    put_little<std::uint16_t>(footer, 8U, nvme_format_version);
    put_little<std::uint16_t>(footer, 10U, nvme_page_bytes);
    put_little<std::uint32_t>(footer, 12U, entries.frame_count);
    put_little<std::uint64_t>(
        footer, 16U, static_cast<std::uint64_t>(entries.frame_count) * nvme_index_entry_bytes);
    put_little<std::uint64_t>(footer, 24U, entries.valid_data_bytes);
    put_little<std::uint64_t>(footer, 32U, header.maximum_block_bytes);
    put_little<std::int64_t>(footer, 40U,
                             std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 entries.end_wall_clock_time.time_since_epoch())
                                 .count());
    put_little<std::uint64_t>(footer, 48U, entries.end_sequence);
    put_little<std::uint32_t>(footer, 56U, entries.index_crc);
    put_little<std::uint32_t>(footer, 60U, entries.data_crc);
    put_little<std::uint32_t>(footer, 64U, header.header_crc);
    std::ranges::copy(commit_marker, footer.begin() + 4088U);
    const auto footer_crc = crc32c(footer);
    put_little<std::uint32_t>(footer, 4084U, footer_crc);
    std::fstream output{path, std::ios::binary | std::ios::in | std::ios::out};
    if (!output)
        return Result<std::pair<std::filesystem::path, std::uint32_t>>::failure(
            recovery_error("NVME_RECOVERY_FAILED", Severity::error, "无法打开待修复的 NVMe 临时块",
                           "storage.nvme.recovery.repair", path, "open-failed"));
    auto staging = footer;
    std::fill(staging.begin() + 4088U, staging.end(), std::byte{0U});
    output.seekp(static_cast<std::streamoff>(header.footer_offset));
    output.write(reinterpret_cast<const char*>(staging.data()),
                 static_cast<std::streamsize>(staging.size()));
    output.flush();
    if (!output)
        return Result<std::pair<std::filesystem::path, std::uint32_t>>::failure(
            recovery_error("NVME_RECOVERY_FAILED", Severity::error, "无法写入 NVMe 恢复尾页",
                           "storage.nvme.recovery.repair", path, "footer-write-failed"));
    output.close();
    if (auto flushed = flush_file(path); !flushed)
        return Result<std::pair<std::filesystem::path, std::uint32_t>>::failure(
            std::move(flushed).error());
    std::fstream marker_output{path, std::ios::binary | std::ios::in | std::ios::out};
    if (!marker_output)
        return Result<std::pair<std::filesystem::path, std::uint32_t>>::failure(recovery_error(
            "NVME_RECOVERY_FAILED", Severity::error, "无法重新打开 NVMe 恢复提交标记",
            "storage.nvme.recovery.commit", path, "marker-open-failed"));
    marker_output.seekp(static_cast<std::streamoff>(header.footer_offset + 4088U));
    marker_output.write(reinterpret_cast<const char*>(commit_marker.data()),
                        static_cast<std::streamsize>(commit_marker.size()));
    marker_output.flush();
    if (!marker_output)
        return Result<std::pair<std::filesystem::path, std::uint32_t>>::failure(
            recovery_error("NVME_RECOVERY_FAILED", Severity::error, "无法写入 NVMe 恢复提交标记",
                           "storage.nvme.recovery.commit", path, "marker-write-failed"));
    marker_output.close();
    if (auto flushed = flush_file(path); !flushed)
        return Result<std::pair<std::filesystem::path, std::uint32_t>>::failure(
            std::move(flushed).error());
    if (MoveFileExW(path.c_str(), committed.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE)
        return Result<std::pair<std::filesystem::path, std::uint32_t>>::failure(
            recovery_error("NVME_RECOVERY_FAILED", Severity::error, "无法原子发布已恢复的 NVMe 块",
                           "storage.nvme.recovery.publish", path, "rename-failed", GetLastError()));
    return Result<std::pair<std::filesystem::path, std::uint32_t>>::success(
        {std::move(committed), footer_crc});
}

Result<void> publish_committed_partial(const std::filesystem::path& path)
{
    auto committed = path;
    committed.replace_extension(L".pbnvme");
    std::error_code error;
    if (std::filesystem::exists(committed, error) || error)
        return Result<void>::failure(recovery_error(
            "NVME_RECOVERY_FAILED", Severity::error, "NVMe 恢复发布目标已存在或不可检查",
            "storage.nvme.recovery.publish", committed,
            error ? "target-stat-failed" : "target-exists",
            error ? std::optional<DWORD>{static_cast<DWORD>(error.value())} : std::nullopt));
    if (MoveFileExW(path.c_str(), committed.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE)
        return Result<void>::failure(recovery_error(
            "NVME_RECOVERY_FAILED", Severity::error, "无法发布已完整提交的 NVMe 临时块",
            "storage.nvme.recovery.publish", path, "rename-failed", GetLastError()));
    return Result<void>::success();
}

Result<void> quarantine(const std::filesystem::path& root, const std::filesystem::path& path,
                        const std::size_t maximum_attempts)
{
    const auto directory = root / ".quarantine";
    std::error_code file_error;
    std::filesystem::create_directories(directory, file_error);
    if (file_error)
        return Result<void>::failure(
            recovery_error("NVME_RECOVERY_FAILED", Severity::error, "无法创建 NVMe 隔离目录",
                           "storage.nvme.recovery.quarantine", directory, "create-directory-failed",
                           static_cast<DWORD>(file_error.value())));
    for (std::size_t attempt = 0U; attempt <= maximum_attempts; ++attempt)
    {
        auto name = path.filename().wstring() + L".quarantine";
        if (attempt > 0U)
            name += L"." + std::to_wstring(attempt);
        const auto target = directory / name;
        if (MoveFileExW(path.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE)
            return Result<void>::success();
        const auto native = GetLastError();
        if (native != ERROR_ALREADY_EXISTS && native != ERROR_FILE_EXISTS)
            return Result<void>::failure(
                recovery_error("NVME_RECOVERY_FAILED", Severity::error, "无法隔离不可信 NVMe 块",
                               "storage.nvme.recovery.quarantine", path, "rename-failed", native));
    }
    return Result<void>::failure(
        recovery_error("NVME_RECOVERY_LIMIT", Severity::error, "NVMe 隔离文件名冲突达到上限",
                       "storage.nvme.recovery.quarantine", path, "name-attempt-limit"));
}

std::size_t summary_bytes(const NvmeIndexedBlock& block) noexcept
{
    return sizeof(block) + block.camera_id.capacity() +
           block.path.native().capacity() * sizeof(std::filesystem::path::value_type);
}

class WindowsNvmeBlockRecovery final : public INvmeBlockRecovery
{
  public:
    Result<NvmeRecoveryReport> recover(const std::filesystem::path& root,
                                       const NvmeRecoveryLimits& limits) override
    {
        if (root.empty() || limits.maximum_files == 0U || limits.maximum_summary_bytes == 0U ||
            limits.deadline <= std::chrono::steady_clock::now())
        {
            return Result<NvmeRecoveryReport>::failure(
                recovery_error("NVME_RECOVERY_LIMIT", Severity::error, "NVMe 恢复上限配置无效",
                               "storage.nvme.recovery", root, "invalid-limits"));
        }
        NvmeRecoveryReport report;
        std::size_t used_summary_bytes{};
        std::error_code iterator_error;
        for (std::filesystem::directory_iterator iterator{root, iterator_error}, end;
             !iterator_error && iterator != end; iterator.increment(iterator_error))
        {
            if (auto before = require_before_deadline(limits, iterator->path()); !before)
                return Result<NvmeRecoveryReport>::failure(std::move(before).error());
            std::error_code type_error;
            if (!iterator->is_regular_file(type_error))
            {
                if (type_error)
                    iterator_error = type_error;
                continue;
            }
            const auto extension = iterator->path().extension();
            const bool committed = extension == L".pbnvme";
            if (!committed && extension != L".partial")
                continue;
            if (++report.scanned_files > limits.maximum_files)
            {
                return Result<NvmeRecoveryReport>::failure(recovery_error(
                    "NVME_RECOVERY_LIMIT", Severity::error, "NVMe 恢复文件数达到上限",
                    "storage.nvme.recovery.limit", iterator->path(), "file-count-exceeded"));
            }
            const auto source = iterator->path();
            auto inspected = inspect(source, committed, limits);
            if (!inspected)
                return Result<NvmeRecoveryReport>::failure(std::move(inspected).error());
            std::optional<NvmeIndexedBlock> recovered;
            bool repaired{};
            if (committed && inspected.value().block)
            {
                recovered = std::move(inspected).value().block;
            }
            else if (!committed && inspected.value().block && inspected.value().committed_marker)
            {
                auto block = std::move(*inspected.value().block);
                if (auto published = publish_committed_partial(source); !published)
                    return Result<NvmeRecoveryReport>::failure(std::move(published).error());
                block.path.replace_extension(L".pbnvme");
                recovered = std::move(block);
                repaired = true;
            }
            else if (!committed && !inspected.value().committed_marker &&
                     inspected.value().header && inspected.value().entries &&
                     inspected.value().entries->frame_count > 0U)
            {
                auto published = repair_partial(source, *inspected.value().header,
                                                *inspected.value().entries, limits);
                if (!published)
                    return Result<NvmeRecoveryReport>::failure(std::move(published).error());
                recovered = make_summary(*inspected.value().header, *inspected.value().entries,
                                         published.value().first, published.value().second);
                repaired = true;
            }
            else
            {
                if (auto isolated = quarantine(root, source, limits.maximum_files); !isolated)
                    return Result<NvmeRecoveryReport>::failure(std::move(isolated).error());
                ++report.quarantined_blocks;
                continue;
            }
            const auto required = summary_bytes(*recovered);
            if (required > limits.maximum_summary_bytes -
                               std::min(used_summary_bytes, limits.maximum_summary_bytes))
            {
                return Result<NvmeRecoveryReport>::failure(recovery_error(
                    "NVME_RECOVERY_LIMIT", Severity::error, "NVMe 恢复摘要内存达到上限",
                    "storage.nvme.recovery.limit", recovered->path, "summary-memory-exceeded"));
            }
            used_summary_bytes += required;
            if (recovered->physical_bytes >
                (std::numeric_limits<std::uint64_t>::max)() - report.recovered_bytes)
            {
                return Result<NvmeRecoveryReport>::failure(recovery_error(
                    "NVME_RECOVERY_LIMIT", Severity::error, "NVMe 恢复容量累计溢出",
                    "storage.nvme.recovery.limit", recovered->path, "byte-count-overflow"));
            }
            report.recovered_bytes += recovered->physical_bytes;
            ++report.accepted_blocks;
            if (repaired)
                ++report.repaired_blocks;
            report.blocks.push_back(std::move(*recovered));
        }
        if (iterator_error)
        {
            return Result<NvmeRecoveryReport>::failure(
                recovery_error("NVME_RECOVERY_FAILED", Severity::error, "无法枚举 NVMe 恢复目录",
                               "storage.nvme.recovery.scan", root, "directory-iteration-failed",
                               static_cast<DWORD>(iterator_error.value())));
        }
        return Result<NvmeRecoveryReport>::success(std::move(report));
    }
};

} // namespace

std::shared_ptr<INvmeBlockRecovery> make_windows_nvme_block_recovery()
{
    return std::make_shared<WindowsNvmeBlockRecovery>();
}

} // namespace paperbreak::storage
