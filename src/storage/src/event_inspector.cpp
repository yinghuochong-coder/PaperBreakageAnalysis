#include "paperbreak/storage/event_inspector.hpp"
#include "paperbreak/storage/nvme_cache.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace paperbreak::storage
{
namespace
{
using Json = nlohmann::json;

Error inspection_error(std::string code, Severity severity, std::string message,
                       std::string operation)
{
    return make_error(std::move(code), severity, std::move(message), "storage",
                      std::move(operation));
}

bool safe_relative_path(const std::filesystem::path& path) noexcept
{
    if (path.empty() || path.is_absolute() || path.has_root_path() ||
        path != path.lexically_normal())
        return false;
    return std::ranges::none_of(path, [](const auto& component) {
        return component.empty() || component == "." || component == "..";
    });
}

std::int64_t utc_milliseconds(const std::string_view value) noexcept
{
    if (value.size() != 24U || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value[19] != '.' || value[23] != 'Z')
        return -1;
    const auto digits = [&](const std::size_t offset, const std::size_t count) {
        int number{};
        for (std::size_t index = 0U; index < count; ++index)
        {
            if (value[offset + index] < '0' || value[offset + index] > '9')
                return -1;
            number = number * 10 + value[offset + index] - '0';
        }
        return number;
    };
    const auto year_value = digits(0U, 4U);
    const auto month_value = digits(5U, 2U);
    const auto day_value = digits(8U, 2U);
    const auto hour = digits(11U, 2U);
    const auto minute = digits(14U, 2U);
    const auto second = digits(17U, 2U);
    const auto millisecond = digits(20U, 3U);
    const std::chrono::year_month_day date{std::chrono::year{year_value},
                                           std::chrono::month{static_cast<unsigned>(month_value)},
                                           std::chrono::day{static_cast<unsigned>(day_value)}};
    if (!date.ok() || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
        second > 59 || millisecond < 0)
        return -1;
    return (std::chrono::sys_days{date} + std::chrono::hours{hour} + std::chrono::minutes{minute} +
            std::chrono::seconds{second} + std::chrono::milliseconds{millisecond})
        .time_since_epoch()
        .count();
}

Result<Json> parse_manifest(const std::string& text)
{
    auto value = Json::parse(text, nullptr, false);
    if (value.is_discarded() || !value.is_object() || !value.contains("eventId") ||
        !value["eventId"].is_string() || !value.contains("rawBlocks") ||
        !value["rawBlocks"].is_array() || !value.contains("keyFrames") ||
        !value["keyFrames"].is_array())
        return Result<Json>::failure(inspection_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                      "事件 manifest 无法解析",
                                                      "event.inspect.manifest"));
    return Result<Json>::success(std::move(value));
}

template <typename T>
T little_value(const std::span<const std::byte> bytes, const std::size_t offset) noexcept
{
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned result{};
    for (std::size_t index = 0U; index < sizeof(T); ++index)
        result |= static_cast<Unsigned>(std::to_integer<unsigned char>(bytes[offset + index]))
                  << (index * 8U);
    return static_cast<T>(result);
}

Result<std::vector<InspectedRawFrame>> raw_block_frames(const Json& value,
                                                        std::vector<std::byte> contents)
{
    try
    {
        if (!value.is_object() || !value.at("path").is_string() ||
            !value.at("cameraId").is_string() || !value.at("frameCount").is_number_unsigned() ||
            !value.at("firstSequenceNumber").is_number_unsigned() ||
            !value.at("lastSequenceNumber").is_number_unsigned() ||
            !value.at("headerCrc32c").is_number_unsigned() ||
            !value.at("indexCrc32c").is_number_unsigned() ||
            !value.at("dataCrc32c").is_number_unsigned() ||
            !value.at("footerCrc32c").is_number_unsigned())
            throw std::invalid_argument{"shape"};
        const std::filesystem::path path{value.at("path").get<std::string>()};
        const std::string camera_id = value.at("cameraId").get<std::string>();
        const auto frame_count = value.at("frameCount").get<std::size_t>();
        if (!safe_relative_path(path) || camera_id.empty() || frame_count == 0U ||
            frame_count > event_raw_block_maximum_frames || contents.size() < nvme_page_bytes * 3U)
            throw std::invalid_argument{"value"};
        const auto bytes = std::span<const std::byte>{contents};
        const auto magic = std::string{reinterpret_cast<const char*>(bytes.data()), 7U};
        const bool legacy = magic == "PBNVME1";
        const bool buffered = magic == "PBNVME2";
        const auto marker =
            std::string{reinterpret_cast<const char*>(bytes.data() + bytes.size() - 8U), 7U};
        if ((!legacy && !buffered) || (legacy && marker != "COMMIT1") ||
            (buffered && marker != "COMMIT2") ||
            little_value<std::uint16_t>(bytes, 8U) !=
                (legacy ? nvme_legacy_format_version : nvme_format_version) ||
            little_value<std::uint16_t>(bytes, 10U) != nvme_page_bytes ||
            little_value<std::uint32_t>(bytes, 84U) < frame_count ||
            little_value<std::uint32_t>(bytes, bytes.size() - nvme_page_bytes + 12U) != frame_count)
            throw std::invalid_argument{"format"};
        auto header = std::vector<std::byte>(bytes.begin(), bytes.begin() + nvme_page_bytes);
        const auto header_crc = little_value<std::uint32_t>(header, 128U);
        std::fill(header.begin() + 128U, header.begin() + 132U, std::byte{0U});
        auto footer = std::vector<std::byte>(bytes.end() - nvme_page_bytes, bytes.end());
        const auto footer_crc = little_value<std::uint32_t>(footer, 4084U);
        std::fill(footer.begin() + 4084U, footer.begin() + 4088U, std::byte{0U});
        const auto index_size = frame_count * nvme_index_entry_bytes;
        const auto index = bytes.subspan(nvme_page_bytes, index_size);
        if (crc32c(header) != header_crc || crc32c(footer) != footer_crc ||
            crc32c(index) != value.at("indexCrc32c").get<std::uint32_t>() ||
            header_crc != value.at("headerCrc32c").get<std::uint32_t>() ||
            footer_crc != value.at("footerCrc32c").get<std::uint32_t>())
            throw std::invalid_argument{"crc"};

        std::vector<InspectedRawFrame> result;
        result.reserve(frame_count);
        std::uint32_t data_crc{};
        for (std::size_t ordinal = 0U; ordinal < frame_count; ++ordinal)
        {
            const auto entry =
                index.subspan(ordinal * nvme_index_entry_bytes, nvme_index_entry_bytes);
            auto entry_copy = std::vector<std::byte>{entry.begin(), entry.end()};
            const auto entry_crc = little_value<std::uint32_t>(entry_copy, 80U);
            std::fill(entry_copy.begin() + 80U, entry_copy.begin() + 84U, std::byte{0U});
            const auto data_offset = little_value<std::uint64_t>(entry, 48U);
            const auto data_size = little_value<std::uint32_t>(entry, 56U);
            if (entry_crc != crc32c(entry_copy) || data_size == 0U ||
                data_offset > contents.size() || contents.size() - data_offset < nvme_page_bytes ||
                data_size > contents.size() - data_offset - nvme_page_bytes)
                throw std::invalid_argument{"index"};
            const auto frame_bytes =
                bytes.subspan(static_cast<std::size_t>(data_offset), data_size);
            if (legacy)
            {
                if (crc32c(frame_bytes) != little_value<std::uint32_t>(entry, 76U))
                    throw std::invalid_argument{"frame-crc"};
                data_crc = crc32c(frame_bytes, data_crc);
            }
            else if (little_value<std::uint32_t>(entry, 76U) != 0U)
                throw std::invalid_argument{"reserved-frame-crc"};
            const auto wall_ns = little_value<std::int64_t>(entry, 24U);
            result.push_back({.relative_path = path,
                              .camera_id = camera_id,
                              .camera_frame_number = little_value<std::uint64_t>(entry, 8U),
                              .sequence_number = little_value<std::uint64_t>(entry, 0U),
                              .wall_clock_time_utc_ms = wall_ns / 1000000});
        }
        if ((legacy && data_crc != value.at("dataCrc32c").get<std::uint32_t>()) ||
            (buffered && value.at("dataCrc32c").get<std::uint32_t>() != 0U) ||
            result.front().sequence_number !=
                value.at("firstSequenceNumber").get<std::uint64_t>() ||
            result.back().sequence_number != value.at("lastSequenceNumber").get<std::uint64_t>())
            throw std::invalid_argument{"range"};
        return Result<std::vector<InspectedRawFrame>>::success(std::move(result));
    }
    catch (const std::exception&)
    {
        return Result<std::vector<InspectedRawFrame>>::failure(
            inspection_error("EVENT_RECOVERY_FAILED", Severity::critical, "原始块或帧索引无效",
                             "event.inspect.rawBlock"));
    }
}

Result<InspectedKeyFrame> key_frame(const Json& value)
{
    try
    {
        if (!value.is_object() || !value.at("path").is_string() ||
            !value.at("cameraId").is_string() ||
            !value.at("cameraFrameNumber").is_number_unsigned() ||
            !value.at("sequenceNumber").is_number_unsigned() || !value.at("reasons").is_array() ||
            value.at("reasons").empty())
            throw std::invalid_argument{"shape"};
        InspectedKeyFrame result{
            .relative_path = value.at("path").get<std::string>(),
            .camera_id = value.at("cameraId").get<std::string>(),
            .camera_frame_number = value.at("cameraFrameNumber").get<std::uint64_t>(),
            .sequence_number = value.at("sequenceNumber").get<std::uint64_t>()};
        if (!safe_relative_path(result.relative_path))
            throw std::invalid_argument{"path"};
        for (const auto& reason : value.at("reasons"))
        {
            if (!reason.is_string() || reason.get_ref<const std::string&>().empty())
                throw std::invalid_argument{"reason"};
            result.reasons.push_back(reason.get<std::string>());
        }
        return Result<InspectedKeyFrame>::success(std::move(result));
    }
    catch (const std::exception&)
    {
        return Result<InspectedKeyFrame>::failure(
            inspection_error("EVENT_RECOVERY_FAILED", Severity::critical, "关键帧索引无效",
                             "event.inspect.keyframe"));
    }
}

void append_u16(std::vector<std::byte>& output, const std::uint16_t value)
{
    output.push_back(static_cast<std::byte>(value & 0xffU));
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::byte>& output, const std::uint32_t value)
{
    append_u16(output, static_cast<std::uint16_t>(value & 0xffffU));
    append_u16(output, static_cast<std::uint16_t>((value >> 16U) & 0xffffU));
}

void append_u64(std::vector<std::byte>& output, const std::uint64_t value)
{
    append_u32(output, static_cast<std::uint32_t>(value & 0xffffffffULL));
    append_u32(output, static_cast<std::uint32_t>(value >> 32U));
}

void append_bytes(std::vector<std::byte>& output, const std::string_view value)
{
    for (const unsigned char byte : value)
        output.push_back(static_cast<std::byte>(byte));
}

std::uint32_t crc32(const std::span<const std::byte> bytes) noexcept
{
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes)
    {
        crc ^= std::to_integer<std::uint32_t>(byte);
        for (unsigned bit = 0U; bit < 8U; ++bit)
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

struct ArchiveEntry final
{
    std::string name;
    std::vector<std::byte> contents;
};

Result<std::vector<std::byte>> make_zip(const std::vector<ArchiveEntry>& entries,
                                        const std::size_t maximum_bytes)
{
    struct Central final
    {
        const ArchiveEntry* entry{};
        std::uint32_t checksum{};
        std::uint32_t offset{};
    };
    std::vector<std::byte> output;
    std::vector<Central> central;
    central.reserve(entries.size());
    for (const auto& entry : entries)
    {
        if (entry.name.empty() || entry.name.size() > (std::numeric_limits<std::uint16_t>::max)() ||
            entry.contents.size() > (std::numeric_limits<std::uint32_t>::max)() ||
            output.size() > (std::numeric_limits<std::uint32_t>::max)())
            return Result<std::vector<std::byte>>::failure(
                inspection_error("EVENT_EXPORT_TOO_LARGE", Severity::error, "事件导出条目超过上限",
                                 "event.export.archive"));
        const auto checksum = crc32(entry.contents);
        const auto size = static_cast<std::uint32_t>(entry.contents.size());
        central.push_back({.entry = &entry,
                           .checksum = checksum,
                           .offset = static_cast<std::uint32_t>(output.size())});
        append_u32(output, 0x04034b50U);
        append_u16(output, 20U);
        append_u16(output, 0x0800U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u32(output, checksum);
        append_u32(output, size);
        append_u32(output, size);
        append_u16(output, static_cast<std::uint16_t>(entry.name.size()));
        append_u16(output, 0U);
        append_bytes(output, entry.name);
        output.insert(output.end(), entry.contents.begin(), entry.contents.end());
        if (output.size() > maximum_bytes)
            return Result<std::vector<std::byte>>::failure(
                inspection_error("EVENT_EXPORT_TOO_LARGE", Severity::error,
                                 "事件导出超过固定大小上限", "event.export.archive"));
    }
    const auto central_offset = static_cast<std::uint32_t>(output.size());
    for (const auto& item : central)
    {
        const auto size = static_cast<std::uint32_t>(item.entry->contents.size());
        append_u32(output, 0x02014b50U);
        append_u16(output, 20U);
        append_u16(output, 20U);
        append_u16(output, 0x0800U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u16(output, 0U);
        append_u32(output, item.checksum);
        append_u32(output, size);
        append_u32(output, size);
        append_u16(output, static_cast<std::uint16_t>(item.entry->name.size()));
        for (unsigned index = 0U; index < 4U; ++index)
            append_u16(output, 0U);
        append_u32(output, 0U);
        append_u32(output, item.offset);
        append_bytes(output, item.entry->name);
    }
    const auto central_size = static_cast<std::uint32_t>(output.size() - central_offset);
    if (central.size() > (std::numeric_limits<std::uint16_t>::max)())
        return Result<std::vector<std::byte>>::failure(
            inspection_error("EVENT_EXPORT_TOO_LARGE", Severity::error,
                             "事件导出文件数超过 ZIP 上限", "event.export.archive"));
    append_u32(output, 0x06054b50U);
    append_u16(output, 0U);
    append_u16(output, 0U);
    append_u16(output, static_cast<std::uint16_t>(central.size()));
    append_u16(output, static_cast<std::uint16_t>(central.size()));
    append_u32(output, central_size);
    append_u32(output, central_offset);
    append_u16(output, 0U);
    if (output.size() > maximum_bytes)
        return Result<std::vector<std::byte>>::failure(
            inspection_error("EVENT_EXPORT_TOO_LARGE", Severity::error, "事件导出超过固定大小上限",
                             "event.export.archive"));
    return Result<std::vector<std::byte>>::success(std::move(output));
}

std::vector<std::byte> text_bytes(const std::string& value)
{
    std::vector<std::byte> bytes;
    bytes.reserve(value.size());
    for (const unsigned char character : value)
        bytes.push_back(static_cast<std::byte>(character));
    return bytes;
}

} // namespace

struct EventInspector::Impl final
{
    using FileSink =
        std::function<Result<void>(const std::filesystem::path&, std::span<const std::byte>)>;

    EventInspectorOptions options;
    std::shared_ptr<IEventFileSystem> file_system;

    Result<std::string> verified_manifest(const std::filesystem::path& relative) const
    {
        if (!safe_relative_path(relative))
            return Result<std::string>::failure(inspection_error("EVENT_NOT_FOUND", Severity::error,
                                                                 "事件目录不在正式事件根内",
                                                                 "event.inspect.path"));
        auto writer = EventTransactionWriter::create(
            {.event_root = options.event_root,
             .maximum_raw_frames = options.maximum_files,
             .maximum_file_bytes = options.maximum_file_bytes,
             .maximum_manifest_bytes = options.maximum_manifest_bytes},
            file_system);
        if (!writer)
            return Result<std::string>::failure(std::move(writer).error());
        return writer.value()->verify_committed_manifest(options.event_root / relative);
    }

    Result<HashedFileContents> read_verified_file(const Json& manifest,
                                                  const std::filesystem::path& directory,
                                                  const std::filesystem::path& relative) const
    {
        const auto key = relative.generic_string();
        if (!safe_relative_path(relative) || !manifest.contains("fileChecksums") ||
            !manifest["fileChecksums"].contains(key) ||
            !manifest["fileChecksums"][key].is_string() || !manifest.contains("fileSizes") ||
            !manifest["fileSizes"].contains(key) ||
            !manifest["fileSizes"][key].is_number_unsigned())
            return Result<HashedFileContents>::failure(
                inspection_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                 "事件文件校验清单无效", "event.inspect.fileList"));
        auto read =
            file_system->read_file_bounded_hashed(directory / relative, options.maximum_file_bytes);
        if (!read)
            return read;
        const auto expected_size = manifest["fileSizes"][key].get<std::uint64_t>();
        const auto expected_hash = manifest["fileChecksums"][key].get<std::string>();
        if (read.value().contents.size() != expected_size ||
            expected_hash != "sha256:" + read.value().sha256)
            return Result<HashedFileContents>::failure(
                inspection_error("EVENT_INTEGRITY_FAILED", Severity::critical,
                                 "事件文件长度或 SHA-256 不匹配", "event.inspect.integrity"));
        return read;
    }

    [[nodiscard]] Result<EventInspectionReport> load_event(
        const std::filesystem::path& committed_relative_directory, const FileSink& sink) const;
};

Result<std::unique_ptr<EventInspector>> EventInspector::create(
    EventInspectorOptions options, std::shared_ptr<IEventFileSystem> file_system)
{
    if (options.event_root.empty() || options.maximum_manifest_bytes == 0U ||
        options.maximum_file_bytes == 0U || options.maximum_files == 0U ||
        options.maximum_export_bytes < 1024U || options.maximum_file_export_bytes < 1024U ||
        options.maximum_file_export_bytes > 1024ULL * 1024ULL * 1024ULL * 1024ULL || !file_system)
        return Result<std::unique_ptr<EventInspector>>::failure(inspection_error(
            "SYS_CONFIG_INVALID", Severity::error, "事件检查器配置无效", "event.inspect.create"));
    return Result<std::unique_ptr<EventInspector>>::success(std::make_unique<EventInspector>(
        ConstructionKey{}, std::move(options), std::move(file_system)));
}

EventInspector::EventInspector(ConstructionKey, EventInspectorOptions options,
                               std::shared_ptr<IEventFileSystem> file_system)
    : impl_(std::make_unique<Impl>(
          Impl{.options = std::move(options), .file_system = std::move(file_system)}))
{
}

EventInspector::~EventInspector() = default;

Result<EventInspectionReport> EventInspector::Impl::load_event(
    const std::filesystem::path& committed_relative_directory, const FileSink& sink) const
{
    auto manifest_text = verified_manifest(committed_relative_directory);
    if (!manifest_text)
        return Result<EventInspectionReport>::failure(std::move(manifest_text).error());
    auto manifest = parse_manifest(manifest_text.value());
    if (!manifest)
        return Result<EventInspectionReport>::failure(std::move(manifest).error());
    if (manifest.value()["rawBlocks"].size() + manifest.value()["keyFrames"].size() + 1U >
        options.maximum_files)
        return Result<EventInspectionReport>::failure(
            inspection_error("EVENT_RECOVERY_FAILED", Severity::critical, "事件文件数超过检查上限",
                             "event.inspect.files"));

    EventInspectionReport report{.event_id = manifest.value()["eventId"].get<std::string>(),
                                 .committed_directory =
                                     options.event_root / committed_relative_directory,
                                 .manifest_json = manifest_text.value(),
                                 .key_frames_traceable = true};
    std::map<std::pair<std::string, std::uint64_t>, std::uint64_t> raw_index;
    std::map<std::string, std::uint64_t> previous_sequences;
    for (const auto& value : manifest.value()["rawBlocks"])
    {
        const auto relative = std::filesystem::path{value.at("path").get<std::string>()};
        auto contents = read_verified_file(manifest.value(), report.committed_directory, relative);
        if (!contents)
            return Result<EventInspectionReport>::failure(std::move(contents).error());
        auto accepted = sink(relative, contents.value().contents);
        if (!accepted)
            return Result<EventInspectionReport>::failure(std::move(accepted).error());
        auto decoded = raw_block_frames(value, std::move(contents).value().contents);
        if (!decoded)
            return Result<EventInspectionReport>::failure(std::move(decoded).error());
        for (auto& raw : decoded.value())
        {
            const auto previous = previous_sequences.find(raw.camera_id);
            if (previous != previous_sequences.end())
            {
                if (raw.sequence_number <= previous->second)
                    return Result<EventInspectionReport>::failure(
                        inspection_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                         "manifest 原始帧顺序不是严格递增", "event.inspect.order"));
                report.observed_sequence_gaps += raw.sequence_number - previous->second - 1U;
            }
            previous_sequences[raw.camera_id] = raw.sequence_number;
            if (!raw_index
                     .emplace(std::make_pair(raw.camera_id, raw.sequence_number),
                              raw.camera_frame_number)
                     .second)
                return Result<EventInspectionReport>::failure(
                    inspection_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                     "manifest 原始帧标识重复", "event.inspect.order"));
            report.raw_frames.push_back(std::move(raw));
        }
    }
    for (const auto& value : manifest.value()["keyFrames"])
    {
        auto key = key_frame(value);
        if (!key)
            return Result<EventInspectionReport>::failure(std::move(key).error());
        const auto raw = raw_index.find({key.value().camera_id, key.value().sequence_number});
        if (raw == raw_index.end() || raw->second != key.value().camera_frame_number)
            report.key_frames_traceable = false;
        auto contents = read_verified_file(manifest.value(), report.committed_directory,
                                           key.value().relative_path);
        if (!contents)
            return Result<EventInspectionReport>::failure(std::move(contents).error());
        auto accepted = sink(key.value().relative_path, contents.value().contents);
        if (!accepted)
            return Result<EventInspectionReport>::failure(std::move(accepted).error());
        if (report.thumbnail_jpeg.empty())
            report.thumbnail_jpeg = contents.value().contents;
        report.key_frames.push_back(std::move(key).value());
    }
    if (!report.key_frames_traceable)
        return Result<EventInspectionReport>::failure(
            inspection_error("EVENT_RECOVERY_FAILED", Severity::critical, "关键帧无法追溯到原始帧",
                             "event.inspect.trace"));
    auto event_metadata =
        read_verified_file(manifest.value(), report.committed_directory, "event.json");
    if (!event_metadata)
        return Result<EventInspectionReport>::failure(std::move(event_metadata).error());
    auto accepted = sink("event.json", event_metadata.value().contents);
    if (!accepted)
        return Result<EventInspectionReport>::failure(std::move(accepted).error());
    return Result<EventInspectionReport>::success(std::move(report));
}

Result<EventInspectionReport> EventInspector::inspect(
    const std::filesystem::path& committed_relative_directory) const
{
    return impl_->load_event(committed_relative_directory,
                             [](const std::filesystem::path&, const std::span<const std::byte>) {
                                 return Result<void>::success();
                             });
}

Result<EventInspectionSummary> EventInspector::inspect_summary(
    const std::filesystem::path& committed_relative_directory) const
{
    auto manifest_text = impl_->verified_manifest(committed_relative_directory);
    if (!manifest_text)
        return Result<EventInspectionSummary>::failure(std::move(manifest_text).error());
    auto manifest = parse_manifest(manifest_text.value());
    if (!manifest)
        return Result<EventInspectionSummary>::failure(std::move(manifest).error());
    if (manifest.value()["rawBlocks"].size() + manifest.value()["keyFrames"].size() + 1U >
        impl_->options.maximum_files)
        return Result<EventInspectionSummary>::failure(
            inspection_error("EVENT_RECOVERY_FAILED", Severity::critical, "事件文件数超过检查上限",
                             "event.inspect.summary.files"));

    struct RawRange final
    {
        std::string camera_id;
        std::uint64_t first_sequence{};
        std::uint64_t last_sequence{};
        std::uint64_t first_camera_frame{};
        std::uint64_t last_camera_frame{};
    };
    EventInspectionSummary summary{.event_id = manifest.value()["eventId"].get<std::string>(),
                                   .committed_directory =
                                       impl_->options.event_root / committed_relative_directory,
                                   .manifest_bytes = manifest_text.value().size(),
                                   .key_frame_count = manifest.value()["keyFrames"].size(),
                                   .key_frames_traceable = true};
    std::vector<RawRange> raw_ranges;
    raw_ranges.reserve(manifest.value()["rawBlocks"].size());
    std::map<std::string, std::uint64_t> previous_sequences;
    try
    {
        for (const auto& raw : manifest.value()["rawBlocks"])
        {
            if (!raw.is_object() || !raw.at("path").is_string() ||
                !raw.at("cameraId").is_string() ||
                !raw.at("firstSequenceNumber").is_number_unsigned() ||
                !raw.at("lastSequenceNumber").is_number_unsigned() ||
                !raw.at("firstCameraFrameNumber").is_number_unsigned() ||
                !raw.at("lastCameraFrameNumber").is_number_unsigned() ||
                !raw.at("frameCount").is_number_unsigned())
                throw std::invalid_argument{"shape"};
            const std::filesystem::path relative{raw.at("path").get<std::string>()};
            RawRange range{
                .camera_id = raw.at("cameraId").get<std::string>(),
                .first_sequence = raw.at("firstSequenceNumber").get<std::uint64_t>(),
                .last_sequence = raw.at("lastSequenceNumber").get<std::uint64_t>(),
                .first_camera_frame = raw.at("firstCameraFrameNumber").get<std::uint64_t>(),
                .last_camera_frame = raw.at("lastCameraFrameNumber").get<std::uint64_t>()};
            const auto frame_count = raw.at("frameCount").get<std::size_t>();
            if (!safe_relative_path(relative) || range.camera_id.empty() || frame_count == 0U ||
                frame_count > event_raw_block_maximum_frames ||
                range.first_sequence > range.last_sequence ||
                range.first_camera_frame > range.last_camera_frame ||
                range.last_sequence - range.first_sequence < frame_count - 1U)
                throw std::invalid_argument{"value"};
            if (summary.raw_frame_count > (std::numeric_limits<std::size_t>::max)() - frame_count)
                throw std::overflow_error{"frame-count"};
            summary.raw_frame_count += frame_count;
            const auto within_block_gaps =
                range.last_sequence - range.first_sequence - (frame_count - 1U);
            if (summary.observed_sequence_gaps >
                (std::numeric_limits<std::uint64_t>::max)() - within_block_gaps)
                throw std::overflow_error{"gap-count"};
            summary.observed_sequence_gaps += within_block_gaps;
            const auto previous = previous_sequences.find(range.camera_id);
            if (previous != previous_sequences.end())
            {
                if (range.first_sequence <= previous->second)
                    throw std::invalid_argument{"order"};
                const auto between_block_gaps = range.first_sequence - previous->second - 1U;
                if (summary.observed_sequence_gaps >
                    (std::numeric_limits<std::uint64_t>::max)() - between_block_gaps)
                    throw std::overflow_error{"gap-count"};
                summary.observed_sequence_gaps += between_block_gaps;
            }
            previous_sequences[range.camera_id] = range.last_sequence;
            raw_ranges.push_back(std::move(range));
        }
        for (const auto& value : manifest.value()["keyFrames"])
        {
            auto key = key_frame(value);
            if (!key)
                return Result<EventInspectionSummary>::failure(std::move(key).error());
            const auto traceable = std::ranges::any_of(raw_ranges, [&key](const RawRange& range) {
                return range.camera_id == key.value().camera_id &&
                       range.first_sequence <= key.value().sequence_number &&
                       key.value().sequence_number <= range.last_sequence &&
                       range.first_camera_frame <= key.value().camera_frame_number &&
                       key.value().camera_frame_number <= range.last_camera_frame;
            });
            summary.key_frames_traceable = summary.key_frames_traceable && traceable;
            if (summary.thumbnail_jpeg.empty())
            {
                auto thumbnail = impl_->read_verified_file(
                    manifest.value(), summary.committed_directory, key.value().relative_path);
                if (!thumbnail)
                    return Result<EventInspectionSummary>::failure(std::move(thumbnail).error());
                summary.thumbnail_jpeg = std::move(thumbnail).value().contents;
            }
        }
    }
    catch (const std::exception&)
    {
        return Result<EventInspectionSummary>::failure(
            inspection_error("EVENT_RECOVERY_FAILED", Severity::critical, "事件摘要索引无效",
                             "event.inspect.summary"));
    }
    return Result<EventInspectionSummary>::success(std::move(summary));
}

Result<std::string> EventInspector::get_manifest(
    const std::filesystem::path& committed_relative_directory) const
{
    return impl_->verified_manifest(committed_relative_directory);
}

Result<EventExportArchive> EventInspector::export_zip(
    const std::filesystem::path& committed_relative_directory) const
{
    std::vector<ArchiveEntry> entries;
    auto inspected = impl_->load_event(
        committed_relative_directory, [&entries](const std::filesystem::path& relative,
                                                 const std::span<const std::byte> contents) {
            entries.push_back({.name = relative.generic_string(),
                               .contents = {contents.begin(), contents.end()}});
            return Result<void>::success();
        });
    if (!inspected)
        return Result<EventExportArchive>::failure(std::move(inspected).error());
    if (entries.size() + 1U > impl_->options.maximum_files)
        return Result<EventExportArchive>::failure(
            inspection_error("EVENT_EXPORT_TOO_LARGE", Severity::error,
                             "事件导出文件数超过固定上限", "event.export.files"));
    entries.push_back(
        {.name = "manifest.json", .contents = text_bytes(inspected.value().manifest_json)});
    auto archive = make_zip(entries, impl_->options.maximum_export_bytes);
    if (!archive)
        return Result<EventExportArchive>::failure(std::move(archive).error());
    const auto file_count = entries.size();
    const auto event_id = inspected.value().event_id;
    return Result<EventExportArchive>::success({.event_id = event_id,
                                                .file_name = event_id + ".zip",
                                                .source_file_count = file_count,
                                                .zip = std::move(archive).value()});
}

Result<EventExportFile> EventInspector::export_zip_file(
    const std::filesystem::path& committed_relative_directory,
    const std::filesystem::path& destination) const
{
    if (destination.empty() || !destination.is_absolute() || destination.extension() != ".zip")
        return Result<EventExportFile>::failure(
            inspection_error("IPC_REQUEST_INVALID", Severity::error, "事件导出暂存目标无效",
                             "event.export.file.path"));
    std::error_code file_error;
    if (std::filesystem::exists(destination, file_error) || file_error)
        return Result<EventExportFile>::failure(
            inspection_error("EVENT_EXPORT_FAILED", Severity::error,
                             "事件导出暂存目标已存在或不可检查", "event.export.file.exists"));
    std::filesystem::create_directories(destination.parent_path(), file_error);
    if (file_error)
        return Result<EventExportFile>::failure(
            inspection_error("EVENT_EXPORT_FAILED", Severity::error, "无法创建事件导出暂存目录",
                             "event.export.file.directory"));
    auto partial = destination;
    partial += ".partial";
    if (std::filesystem::exists(partial, file_error) || file_error)
        return Result<EventExportFile>::failure(
            inspection_error("EVENT_EXPORT_FAILED", Severity::error,
                             "事件导出临时目标已存在或不可检查", "event.export.file.exists"));

    struct CentralEntry final
    {
        std::string name;
        std::uint32_t checksum{};
        std::uint32_t size{};
        std::uint64_t offset{};
    };
    std::vector<CentralEntry> central;
    central.reserve(std::min<std::size_t>(impl_->options.maximum_files, 1024U));
    std::ofstream output{partial, std::ios::binary | std::ios::out};
    std::uint64_t written{};
    const auto fail = [&](std::string message, std::string operation) {
        output.close();
        std::error_code cleanup_error;
        std::filesystem::remove(partial, cleanup_error);
        return Result<EventExportFile>::failure(inspection_error(
            "EVENT_EXPORT_FAILED", Severity::error, std::move(message), std::move(operation)));
    };
    if (!output)
        return fail("无法创建事件导出临时文件", "event.export.file.open");

    const auto write_bytes = [&](const std::span<const std::byte> bytes) -> bool {
        if (bytes.size() > impl_->options.maximum_file_export_bytes - written)
            return false;
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!output)
            return false;
        written += bytes.size();
        return true;
    };
    const auto write_entry = [&](const std::filesystem::path& relative,
                                 const std::span<const std::byte> contents) -> Result<void> {
        const auto name = relative.generic_string();
        if (name.empty() || name.size() > (std::numeric_limits<std::uint16_t>::max)() ||
            contents.size() > (std::numeric_limits<std::uint32_t>::max)())
            return Result<void>::failure(inspection_error("EVENT_EXPORT_TOO_LARGE", Severity::error,
                                                          "事件导出条目超过 ZIP 单文件上限",
                                                          "event.export.file.entry"));
        const auto checksum = crc32(contents);
        const auto size = static_cast<std::uint32_t>(contents.size());
        central.push_back({.name = name, .checksum = checksum, .size = size, .offset = written});
        std::vector<std::byte> header;
        header.reserve(30U + name.size());
        append_u32(header, 0x04034b50U);
        append_u16(header, 20U);
        append_u16(header, 0x0800U);
        append_u16(header, 0U);
        append_u16(header, 0U);
        append_u16(header, 0U);
        append_u32(header, checksum);
        append_u32(header, size);
        append_u32(header, size);
        append_u16(header, static_cast<std::uint16_t>(name.size()));
        append_u16(header, 0U);
        append_bytes(header, name);
        if (!write_bytes(header) || !write_bytes(contents))
            return Result<void>::failure(inspection_error("EVENT_EXPORT_FAILED", Severity::error,
                                                          "事件导出写入失败或超过固定上限",
                                                          "event.export.file.write"));
        return Result<void>::success();
    };
    auto inspected = impl_->load_event(committed_relative_directory, write_entry);
    if (!inspected)
    {
        output.close();
        std::filesystem::remove(partial, file_error);
        return Result<EventExportFile>::failure(std::move(inspected).error());
    }
    const auto manifest_bytes = text_bytes(inspected.value().manifest_json);
    auto manifest_written = write_entry("manifest.json", manifest_bytes);
    if (!manifest_written)
    {
        output.close();
        std::filesystem::remove(partial, file_error);
        return Result<EventExportFile>::failure(std::move(manifest_written).error());
    }
    if (central.size() > impl_->options.maximum_files ||
        central.size() > (std::numeric_limits<std::uint16_t>::max)())
        return fail("事件导出文件数超过 ZIP 上限", "event.export.file.entries");

    const auto central_offset = written;
    for (const auto& item : central)
    {
        const bool zip64_offset = item.offset > (std::numeric_limits<std::uint32_t>::max)();
        std::vector<std::byte> header;
        header.reserve(58U + item.name.size());
        append_u32(header, 0x02014b50U);
        append_u16(header, zip64_offset ? 45U : 20U);
        append_u16(header, zip64_offset ? 45U : 20U);
        append_u16(header, 0x0800U);
        append_u16(header, 0U);
        append_u16(header, 0U);
        append_u16(header, 0U);
        append_u32(header, item.checksum);
        append_u32(header, item.size);
        append_u32(header, item.size);
        append_u16(header, static_cast<std::uint16_t>(item.name.size()));
        append_u16(header, zip64_offset ? 12U : 0U);
        append_u16(header, 0U);
        append_u16(header, 0U);
        append_u16(header, 0U);
        append_u32(header, 0U);
        append_u32(header, zip64_offset ? 0xffffffffU : static_cast<std::uint32_t>(item.offset));
        append_bytes(header, item.name);
        if (zip64_offset)
        {
            append_u16(header, 0x0001U);
            append_u16(header, 8U);
            append_u64(header, item.offset);
        }
        if (!write_bytes(header))
            return fail("事件导出中心目录写入失败", "event.export.file.central");
    }
    const auto central_size = written - central_offset;
    const bool zip64 = central_offset > (std::numeric_limits<std::uint32_t>::max)() ||
                       central_size > (std::numeric_limits<std::uint32_t>::max)();
    if (zip64)
    {
        const auto zip64_offset = written;
        std::vector<std::byte> zip64_records;
        append_u32(zip64_records, 0x06064b50U);
        append_u64(zip64_records, 44U);
        append_u16(zip64_records, 45U);
        append_u16(zip64_records, 45U);
        append_u32(zip64_records, 0U);
        append_u32(zip64_records, 0U);
        append_u64(zip64_records, central.size());
        append_u64(zip64_records, central.size());
        append_u64(zip64_records, central_size);
        append_u64(zip64_records, central_offset);
        append_u32(zip64_records, 0x07064b50U);
        append_u32(zip64_records, 0U);
        append_u64(zip64_records, zip64_offset);
        append_u32(zip64_records, 1U);
        if (!write_bytes(zip64_records))
            return fail("事件 ZIP64 尾记录写入失败", "event.export.file.zip64");
    }
    std::vector<std::byte> trailer;
    append_u32(trailer, 0x06054b50U);
    append_u16(trailer, 0U);
    append_u16(trailer, 0U);
    append_u16(trailer, static_cast<std::uint16_t>(central.size()));
    append_u16(trailer, static_cast<std::uint16_t>(central.size()));
    append_u32(trailer, zip64 ? 0xffffffffU : static_cast<std::uint32_t>(central_size));
    append_u32(trailer, zip64 ? 0xffffffffU : static_cast<std::uint32_t>(central_offset));
    append_u16(trailer, 0U);
    if (!write_bytes(trailer))
        return fail("事件导出尾记录写入失败", "event.export.file.trailer");
    output.flush();
    output.close();
    if (!output)
        return fail("事件导出刷新失败", "event.export.file.flush");
    std::filesystem::rename(partial, destination, file_error);
    if (file_error)
        return fail("事件导出暂存提交失败", "event.export.file.commit");
    return Result<EventExportFile>::success({.event_id = inspected.value().event_id,
                                             .file_name = inspected.value().event_id + ".zip",
                                             .source_file_count = central.size(),
                                             .size_bytes = written,
                                             .path = destination});
}

} // namespace paperbreak::storage
