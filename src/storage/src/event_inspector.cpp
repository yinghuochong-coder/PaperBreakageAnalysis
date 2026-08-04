#include "paperbreak/storage/event_inspector.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
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
        !value["eventId"].is_string() || !value.contains("rawFiles") ||
        !value["rawFiles"].is_array() || !value.contains("keyFrames") ||
        !value["keyFrames"].is_array())
        return Result<Json>::failure(inspection_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                      "事件 manifest 无法解析",
                                                      "event.inspect.manifest"));
    return Result<Json>::success(std::move(value));
}

Result<InspectedRawFrame> raw_frame(const Json& value)
{
    try
    {
        if (!value.is_object() || !value.at("path").is_string() ||
            !value.at("cameraId").is_string() ||
            !value.at("cameraFrameNumber").is_number_unsigned() ||
            !value.at("sequenceNumber").is_number_unsigned() ||
            !value.at("wallClockTime").is_string())
            throw std::invalid_argument{"shape"};
        InspectedRawFrame result{
            .relative_path = value.at("path").get<std::string>(),
            .camera_id = value.at("cameraId").get<std::string>(),
            .camera_frame_number = value.at("cameraFrameNumber").get<std::uint64_t>(),
            .sequence_number = value.at("sequenceNumber").get<std::uint64_t>(),
            .wall_clock_time_utc_ms =
                utc_milliseconds(value.at("wallClockTime").get_ref<const std::string&>())};
        if (!safe_relative_path(result.relative_path) || result.wall_clock_time_utc_ms < 0)
            throw std::invalid_argument{"value"};
        return Result<InspectedRawFrame>::success(std::move(result));
    }
    catch (const std::exception&)
    {
        return Result<InspectedRawFrame>::failure(inspection_error(
            "EVENT_RECOVERY_FAILED", Severity::critical, "原始帧索引无效", "event.inspect.raw"));
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

Result<EventInspectionReport> EventInspector::inspect(
    const std::filesystem::path& committed_relative_directory) const
{
    auto manifest_text = impl_->verified_manifest(committed_relative_directory);
    if (!manifest_text)
        return Result<EventInspectionReport>::failure(std::move(manifest_text).error());
    auto manifest = parse_manifest(manifest_text.value());
    if (!manifest)
        return Result<EventInspectionReport>::failure(std::move(manifest).error());
    if (manifest.value()["rawFiles"].size() + manifest.value()["keyFrames"].size() + 1U >
        impl_->options.maximum_files)
        return Result<EventInspectionReport>::failure(
            inspection_error("EVENT_RECOVERY_FAILED", Severity::critical, "事件文件数超过检查上限",
                             "event.inspect.files"));

    EventInspectionReport report{.event_id = manifest.value()["eventId"].get<std::string>(),
                                 .committed_directory =
                                     impl_->options.event_root / committed_relative_directory,
                                 .manifest_json = manifest_text.value(),
                                 .key_frames_traceable = true};
    std::map<std::pair<std::string, std::uint64_t>, std::uint64_t> raw_index;
    std::map<std::string, std::uint64_t> previous_sequences;
    for (const auto& value : manifest.value()["rawFiles"])
    {
        auto raw = raw_frame(value);
        if (!raw)
            return Result<EventInspectionReport>::failure(std::move(raw).error());
        const auto previous = previous_sequences.find(raw.value().camera_id);
        if (previous != previous_sequences.end())
        {
            if (raw.value().sequence_number <= previous->second)
                return Result<EventInspectionReport>::failure(
                    inspection_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                     "manifest 原始帧顺序不是严格递增", "event.inspect.order"));
            report.observed_sequence_gaps += raw.value().sequence_number - previous->second - 1U;
        }
        previous_sequences[raw.value().camera_id] = raw.value().sequence_number;
        if (!raw_index
                 .emplace(std::make_pair(raw.value().camera_id, raw.value().sequence_number),
                          raw.value().camera_frame_number)
                 .second)
            return Result<EventInspectionReport>::failure(
                inspection_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                 "manifest 原始帧标识重复", "event.inspect.order"));
        report.raw_frames.push_back(std::move(raw).value());
    }
    for (const auto& value : manifest.value()["keyFrames"])
    {
        auto key = key_frame(value);
        if (!key)
            return Result<EventInspectionReport>::failure(std::move(key).error());
        const auto raw = raw_index.find({key.value().camera_id, key.value().sequence_number});
        if (raw == raw_index.end() || raw->second != key.value().camera_frame_number)
            report.key_frames_traceable = false;
        report.key_frames.push_back(std::move(key).value());
    }
    if (!report.key_frames_traceable)
        return Result<EventInspectionReport>::failure(
            inspection_error("EVENT_RECOVERY_FAILED", Severity::critical, "关键帧无法追溯到原始帧",
                             "event.inspect.trace"));
    if (!report.key_frames.empty())
    {
        auto thumbnail = impl_->file_system->read_file_bounded(
            report.committed_directory / report.key_frames.front().relative_path,
            impl_->options.maximum_file_bytes);
        if (!thumbnail)
            return Result<EventInspectionReport>::failure(std::move(thumbnail).error());
        report.thumbnail_jpeg = std::move(thumbnail).value();
    }
    return Result<EventInspectionReport>::success(std::move(report));
}

Result<EventExportArchive> EventInspector::export_zip(
    const std::filesystem::path& committed_relative_directory) const
{
    auto inspected = inspect(committed_relative_directory);
    if (!inspected)
        return Result<EventExportArchive>::failure(std::move(inspected).error());
    std::vector<std::filesystem::path> ordered_paths{std::filesystem::path{"event.json"}};
    for (const auto& frame : inspected.value().raw_frames)
        ordered_paths.push_back(frame.relative_path);
    for (const auto& frame : inspected.value().key_frames)
        ordered_paths.push_back(frame.relative_path);
    if (ordered_paths.size() + 1U > impl_->options.maximum_files)
        return Result<EventExportArchive>::failure(
            inspection_error("EVENT_EXPORT_TOO_LARGE", Severity::error,
                             "事件导出文件数超过固定上限", "event.export.files"));
    std::vector<ArchiveEntry> entries;
    entries.reserve(ordered_paths.size() + 1U);
    for (const auto& relative : ordered_paths)
    {
        auto bytes = impl_->file_system->read_file_bounded(
            inspected.value().committed_directory / relative, impl_->options.maximum_file_bytes);
        if (!bytes)
            return Result<EventExportArchive>::failure(std::move(bytes).error());
        entries.push_back(
            {.name = relative.generic_string(), .contents = std::move(bytes).value()});
    }
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
    auto inspected = inspect(committed_relative_directory);
    if (!inspected)
        return Result<EventExportFile>::failure(std::move(inspected).error());

    std::vector<std::filesystem::path> ordered_paths{std::filesystem::path{"event.json"}};
    for (const auto& frame : inspected.value().raw_frames)
        ordered_paths.push_back(frame.relative_path);
    for (const auto& frame : inspected.value().key_frames)
        ordered_paths.push_back(frame.relative_path);
    ordered_paths.push_back(std::filesystem::path{"manifest.json"});
    if (ordered_paths.size() > impl_->options.maximum_files ||
        ordered_paths.size() > (std::numeric_limits<std::uint16_t>::max)())
        return Result<EventExportFile>::failure(
            inspection_error("EVENT_EXPORT_TOO_LARGE", Severity::error,
                             "事件导出文件数超过 ZIP 上限", "event.export.file.entries"));

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
    central.reserve(ordered_paths.size());
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
    for (const auto& relative : ordered_paths)
    {
        std::vector<std::byte> contents;
        if (relative == "manifest.json")
            contents = text_bytes(inspected.value().manifest_json);
        else
        {
            auto read = impl_->file_system->read_file_bounded(
                inspected.value().committed_directory / relative,
                impl_->options.maximum_file_bytes);
            if (!read)
            {
                output.close();
                std::filesystem::remove(partial, file_error);
                return Result<EventExportFile>::failure(std::move(read).error());
            }
            contents = std::move(read).value();
        }
        const auto name = relative.generic_string();
        if (name.empty() || name.size() > (std::numeric_limits<std::uint16_t>::max)() ||
            contents.size() > (std::numeric_limits<std::uint32_t>::max)())
            return fail("事件导出条目超过 ZIP 单文件上限", "event.export.file.entry");
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
            return fail("事件导出写入失败或超过固定上限", "event.export.file.write");
    }

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
                                             .source_file_count = ordered_paths.size(),
                                             .size_bytes = written,
                                             .path = destination});
}

} // namespace paperbreak::storage
