#include "paperbreak/storage/event_store.hpp"
#include "paperbreak/storage/nvme_cache.hpp"

#include "paperbreak/common/camera_slots.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

namespace paperbreak::storage
{
namespace
{

using Json = nlohmann::json;

constexpr std::size_t maximum_event_capacity = 64U;
constexpr std::size_t absolute_maximum_raw_frames = 262144U;
constexpr std::size_t absolute_maximum_file_bytes = 512U * 1024U * 1024U;
constexpr std::size_t absolute_maximum_manifest_bytes = 32U * 1024U * 1024U;
constexpr std::size_t absolute_maximum_recovery_entries = 8192U;
constexpr std::size_t maximum_text_bytes = 4096U;

Error event_error(std::string code, const Severity severity, std::string message,
                  std::string operation, const bool retryable = false)
{
    return make_error(std::move(code), severity, std::move(message), "storage",
                      std::move(operation), retryable);
}

Error wrap_file_error(const Error& source, std::string message, std::string operation,
                      const std::optional<std::filesystem::path>& transaction = std::nullopt)
{
    Error result = event_error("EVENT_WRITE_FAILED", Severity::critical, std::move(message),
                               std::move(operation), true);
    result.native_domain = source.native_domain;
    result.native_code = source.native_code;
    if (transaction.has_value())
        result.details.push_back({.key = "transactionDirectory", .value = transaction->string()});
    result.details.push_back({.key = "sourceCode", .value = source.business_code});
    return result;
}

std::span<const std::byte> bytes_of(const std::string_view value) noexcept
{
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

std::string format_utc(const camera::WallClockTime time)
{
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch());
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(milliseconds);
    const auto fraction = milliseconds - seconds;
    const auto raw = static_cast<std::time_t>(seconds.count());
    std::tm value{};
    if (gmtime_s(&value, &raw) != 0)
        return {};
    std::array<char, 32U> output{};
    const int written =
        std::snprintf(output.data(), output.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                      value.tm_year + 1900, value.tm_mon + 1, value.tm_mday, value.tm_hour,
                      value.tm_min, value.tm_sec, static_cast<long long>(fraction.count()));
    if (written <= 0 || static_cast<std::size_t>(written) >= output.size())
        return {};
    return {output.data(), static_cast<std::size_t>(written)};
}

std::string date_path(const camera::WallClockTime time)
{
    const auto timestamp = format_utc(time);
    if (timestamp.size() < 10U)
        return {};
    return timestamp.substr(0U, 4U) + "/" + timestamp.substr(5U, 2U) + "/" +
           timestamp.substr(8U, 2U);
}

bool safe_event_id(const std::string_view value) noexcept
{
    return !value.empty() && value.size() <= 128U &&
           std::ranges::all_of(value, [](const unsigned char character) {
               return std::isalnum(character) != 0 || character == '-';
           });
}

bool valid_text(const std::string_view value, const bool may_be_empty = false) noexcept
{
    return value.size() <= maximum_text_bytes && (may_be_empty || !value.empty());
}

std::string_view pixel_format_name(const camera::PixelFormat format) noexcept
{
    switch (format)
    {
    case camera::PixelFormat::mono8:
        return "Mono8";
    case camera::PixelFormat::mono10:
        return "Mono10";
    case camera::PixelFormat::mono12:
        return "Mono12";
    case camera::PixelFormat::bayer_rg8:
        return "BayerRG8";
    }
    return "Unknown";
}

Json metadata_json(const EventManifestMetadata& metadata,
                   const std::filesystem::path& destination_relative)
{
    const auto& decision_state =
        metadata.decision_state.empty() ? metadata.event_state : metadata.decision_state;
    Json value{{"schemaVersion", event_manifest_schema_version},
               {"eventId", metadata.event_id},
               {"decisionState", decision_state},
               {"triggerCount", metadata.trigger_count},
               {"candidateTime", format_utc(metadata.candidate_time)},
               {"confirmedTime", metadata.confirmed_time.has_value()
                                     ? Json(format_utc(*metadata.confirmed_time))
                                     : Json(nullptr)},
               {"startTime", format_utc(metadata.start_time)},
               {"endTime", format_utc(metadata.end_time)},
               {"cameraIds", metadata.camera_ids},
               {"triggerCameraId", metadata.trigger_camera_id},
               {"triggerFrameNumber", metadata.trigger_frame_number},
               {"triggerReason", metadata.trigger_reason},
               {"confidence", metadata.confidence},
               {"preEventSeconds", metadata.pre_event_duration.count() / 1000.0},
               {"postEventSeconds", metadata.post_event_duration.count() / 1000.0},
               {"algorithmName", metadata.algorithm_name},
               {"algorithmVersion", metadata.algorithm_version},
               {"configVersion", metadata.config_version},
               {"machineId", metadata.machine_id},
               {"productionLineId", metadata.production_line_id},
               {"paperType", metadata.paper_type},
               {"paperSpeed",
                metadata.paper_speed.has_value() ? Json(*metadata.paper_speed) : Json(nullptr)},
               {"uploadState", metadata.upload_state},
               {"timeQuality", metadata.time_quality},
               {"writeMode", "buffered"},
               {"powerLossDurable", false},
               {"verification", "upload-or-on-demand"},
               {"destinationRelativePath", destination_relative.generic_string()}};
    return value;
}

Result<void> validate_request(const EventPersistenceRequest& request,
                              const EventStoreOptions& options)
{
    const auto& metadata = request.metadata;
    if (!safe_event_id(metadata.event_id) || metadata.event_id != request.window.event_id ||
        !valid_text(metadata.decision_state.empty() ? metadata.event_state
                                                    : metadata.decision_state) ||
        metadata.trigger_count == 0U || metadata.start_time > metadata.end_time ||
        metadata.candidate_time < metadata.start_time ||
        metadata.candidate_time > metadata.end_time ||
        (metadata.confirmed_time.has_value() &&
         (*metadata.confirmed_time < metadata.candidate_time ||
          *metadata.confirmed_time > metadata.end_time)) ||
        metadata.camera_ids.empty() || metadata.camera_ids.size() > camera_slot_count ||
        !valid_text(metadata.trigger_camera_id) || !valid_text(metadata.trigger_reason) ||
        !std::isfinite(metadata.confidence) || metadata.confidence < 0.0 ||
        metadata.confidence > 1.0 || metadata.pre_event_duration.count() < 0 ||
        metadata.post_event_duration.count() < 0 || !valid_text(metadata.algorithm_name) ||
        !valid_text(metadata.algorithm_version) || !valid_text(metadata.config_version) ||
        !valid_text(metadata.machine_id) || !valid_text(metadata.production_line_id) ||
        !valid_text(metadata.paper_type) ||
        (metadata.paper_speed.has_value() && !std::isfinite(*metadata.paper_speed)) ||
        !valid_text(metadata.upload_state) || !valid_text(metadata.time_quality))
        return Result<void>::failure(event_error("EVENT_WRITE_FAILED", Severity::critical,
                                                 "事件持久化元数据无效", "event.persist.validate"));

    std::set<std::string> camera_ids;
    for (const auto& camera_id : metadata.camera_ids)
    {
        if (!is_canonical_camera_id(camera_id) || !camera_ids.insert(camera_id).second)
            return Result<void>::failure(event_error("EVENT_WRITE_FAILED", Severity::critical,
                                                     "事件相机列表无效", "event.persist.validate"));
    }
    if (!camera_ids.contains(metadata.trigger_camera_id) || request.window.camera_windows.empty() ||
        request.window.camera_windows.size() > camera_slot_count ||
        request.key_frames.size() > options.maximum_key_frames)
        return Result<void>::failure(event_error("EVENT_WRITE_FAILED", Severity::critical,
                                                 "事件窗口或关键帧数量无效",
                                                 "event.persist.validate"));

    std::size_t raw_count = 0U;
    std::map<std::pair<std::string, std::uint64_t>, const camera::FrameView*> raw_frames;
    std::set<std::string> window_camera_ids;
    for (const auto& window : request.window.camera_windows)
    {
        if (!camera_ids.contains(window.camera_id) ||
            !window_camera_ids.insert(window.camera_id).second)
            return Result<void>::failure(event_error("EVENT_WRITE_FAILED", Severity::critical,
                                                     "冻结窗口包含未声明相机",
                                                     "event.persist.validate"));
        if (window.frames.size() > options.maximum_raw_frames - raw_count)
            return Result<void>::failure(event_error("EVENT_WRITE_FAILED", Severity::critical,
                                                     "原始帧数量超过固定上限",
                                                     "event.persist.validate"));
        raw_count += window.frames.size();
        for (const auto& frame : window.frames)
        {
            if (frame.camera_id() != window.camera_id || frame.flags().incomplete ||
                frame.bytes().empty() || frame.bytes().size() > options.maximum_file_bytes ||
                !raw_frames
                     .emplace(std::make_pair(frame.camera_id(), frame.sequence_number()), &frame)
                     .second)
                return Result<void>::failure(event_error("EVENT_WRITE_FAILED", Severity::critical,
                                                         "原始帧布局、大小或标识无效",
                                                         "event.persist.validate"));
        }
    }
    if (raw_count == 0U)
        return Result<void>::failure(event_error("EVENT_WRITE_FAILED", Severity::critical,
                                                 "事件没有可保存原始帧", "event.persist.validate"));

    std::set<std::pair<std::string, std::uint64_t>> key_keys;
    for (const auto& key_frame : request.key_frames)
    {
        const auto& descriptor = key_frame.descriptor;
        const auto raw = raw_frames.find({descriptor.camera_id, descriptor.sequence_number});
        if (!camera_ids.contains(descriptor.camera_id) || descriptor.reasons.empty() ||
            descriptor.reasons.size() > event::key_frame_reason_count ||
            key_frame.jpeg.size() < 4U || key_frame.jpeg.size() > options.maximum_file_bytes ||
            !key_keys.emplace(descriptor.camera_id, descriptor.sequence_number).second ||
            raw == raw_frames.end() ||
            raw->second->camera_frame_number() != descriptor.camera_frame_number ||
            raw->second->received_monotonic_time() != descriptor.monotonic_time ||
            raw->second->received_wall_clock_time() != descriptor.wall_clock_time ||
            raw->second->geometry() != descriptor.geometry ||
            raw->second->pixel_format() != descriptor.pixel_format ||
            key_frame.jpeg.front() != std::byte{0xff} || key_frame.jpeg[1] != std::byte{0xd8} ||
            key_frame.jpeg[key_frame.jpeg.size() - 2U] != std::byte{0xff} ||
            key_frame.jpeg.back() != std::byte{0xd9})
            return Result<void>::failure(event_error("EVENT_WRITE_FAILED", Severity::critical,
                                                     "关键帧 JPEG 或描述无效",
                                                     "event.persist.validate"));
    }
    return Result<void>::success();
}

bool safe_relative_path(const std::filesystem::path& path) noexcept
{
    if (path.empty() || path.is_absolute() || path.has_root_path() ||
        path != path.lexically_normal())
        return false;
    for (const auto& component : path)
    {
        if (component == "." || component == ".." || component.empty())
            return false;
    }
    return true;
}

bool decimal_component(const std::filesystem::path& component, const std::size_t length) noexcept
{
    const auto value = component.string();
    return value.size() == length && std::ranges::all_of(value, [](const unsigned char character) {
               return std::isdigit(character) != 0;
           });
}

bool valid_destination_path(const std::filesystem::path& path,
                            const std::string_view event_id) noexcept
{
    if (!safe_relative_path(path))
        return false;
    std::vector<std::filesystem::path> components;
    for (const auto& component : path)
        components.push_back(component);
    return components.size() == 4U && decimal_component(components[0], 4U) &&
           decimal_component(components[1], 2U) && decimal_component(components[2], 2U) &&
           components[3].string() == event_id;
}

bool has_string(const Json& value, const std::string_view key) noexcept
{
    const auto iterator = value.find(key);
    return iterator != value.end() && iterator->is_string() &&
           valid_text(iterator->get_ref<const std::string&>());
}

Result<void> validate_manifest_shape(const Json& manifest, const EventStoreOptions& options)
{
    const auto schema = manifest.value("schemaVersion", 0U);
    static constexpr std::array<std::string_view, 13U> required_strings{
        "eventId",         "decisionState",    "candidateTime", "startTime",        "endTime",
        "triggerCameraId", "triggerReason",    "algorithmName", "algorithmVersion", "configVersion",
        "machineId",       "productionLineId", "paperType"};
    if (std::ranges::any_of(required_strings,
                            [&manifest](const auto key) { return !has_string(manifest, key); }) ||
        !has_string(manifest, "uploadState") || !has_string(manifest, "timeQuality") ||
        !has_string(manifest, "destinationRelativePath") || !manifest.contains("confirmedTime") ||
        !(manifest["confirmedTime"].is_null() || manifest["confirmedTime"].is_string()) ||
        !manifest.contains("cameraIds") || !manifest["cameraIds"].is_array() ||
        manifest["cameraIds"].empty() || manifest["cameraIds"].size() > camera_slot_count ||
        !manifest.contains("triggerCount") || !manifest["triggerCount"].is_number_unsigned() ||
        manifest["triggerCount"].get<std::uint64_t>() == 0U ||
        !manifest.contains("triggerFrameNumber") ||
        !manifest["triggerFrameNumber"].is_number_unsigned() || !manifest.contains("confidence") ||
        !manifest["confidence"].is_number() || !manifest.contains("preEventSeconds") ||
        !manifest["preEventSeconds"].is_number() || !manifest.contains("postEventSeconds") ||
        !manifest["postEventSeconds"].is_number() || !manifest.contains("paperSpeed") ||
        !(manifest["paperSpeed"].is_null() || manifest["paperSpeed"].is_number()) ||
        !manifest.contains("rawBlocks") || !manifest["rawBlocks"].is_array() ||
        manifest["rawBlocks"].empty() ||
        manifest["rawBlocks"].size() > options.maximum_raw_frames ||
        !manifest.contains("keyFrames") || !manifest["keyFrames"].is_array() ||
        manifest["keyFrames"].size() > options.maximum_key_frames)
        return Result<void>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                 "事件 manifest 必需字段缺失或类型错误",
                                                 "event.recovery.verify"));
    if (schema == event_manifest_schema_version &&
        (!manifest.contains("writeMode") || manifest["writeMode"] != "buffered" ||
         !manifest.contains("powerLossDurable") || !manifest["powerLossDurable"].is_boolean() ||
         manifest["powerLossDurable"].get<bool>() || !manifest.contains("verification") ||
         manifest["verification"] != "upload-or-on-demand"))
        return Result<void>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                 "事件 manifest v3 缓冲写策略字段无效",
                                                 "event.recovery.verify"));

    std::set<std::string> camera_ids;
    for (const auto& camera_id : manifest["cameraIds"])
    {
        if (!camera_id.is_string() ||
            !is_canonical_camera_id(camera_id.get_ref<const std::string&>()) ||
            !camera_ids.insert(camera_id.get<std::string>()).second)
            return Result<void>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                     "事件 manifest 相机列表无效",
                                                     "event.recovery.verify"));
    }
    if (!camera_ids.contains(manifest["triggerCameraId"].get<std::string>()))
        return Result<void>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                 "事件 manifest 触发相机未声明",
                                                 "event.recovery.verify"));
    const auto confidence = manifest["confidence"].get<double>();
    const auto pre_seconds = manifest["preEventSeconds"].get<double>();
    const auto post_seconds = manifest["postEventSeconds"].get<double>();
    if (!std::isfinite(confidence) || confidence < 0.0 || confidence > 1.0 ||
        !std::isfinite(pre_seconds) || pre_seconds < 0.0 || !std::isfinite(post_seconds) ||
        post_seconds < 0.0 ||
        (manifest["paperSpeed"].is_number() &&
         !std::isfinite(manifest["paperSpeed"].get<double>())))
        return Result<void>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                 "事件 manifest 数值字段无效",
                                                 "event.recovery.verify"));

    const auto event_id = manifest["eventId"].get<std::string>();
    const std::filesystem::path destination{manifest["destinationRelativePath"].get<std::string>()};
    if (!valid_destination_path(destination, event_id))
        return Result<void>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                 "事件 manifest 目标目录格式无效",
                                                 "event.recovery.verify"));
    const auto candidate_time = manifest["candidateTime"].get<std::string>();
    const auto destination_text = destination.generic_string();
    if (candidate_time.size() < 10U || destination_text.size() < 10U ||
        destination_text.substr(0U, 10U) != candidate_time.substr(0U, 4U) + "/" +
                                                candidate_time.substr(5U, 2U) + "/" +
                                                candidate_time.substr(8U, 2U))
        return Result<void>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                 "事件日期目录与候选时间不一致",
                                                 "event.recovery.verify"));

    std::set<std::string> expected_paths{"event.json"};
    const auto collect_paths = [&](const Json& files, const std::string_view prefix) {
        for (const auto& file : files)
        {
            if (!file.is_object() || !has_string(file, "path") || !has_string(file, "cameraId") ||
                !camera_ids.contains(file["cameraId"].get<std::string>()))
                return false;
            const auto path_text = file["path"].get<std::string>();
            const std::filesystem::path relative{path_text};
            if (!safe_relative_path(relative) || !path_text.starts_with(prefix) ||
                !expected_paths.insert(path_text).second)
                return false;
        }
        return true;
    };
    if (!collect_paths(manifest["rawBlocks"], "raw/") ||
        !collect_paths(manifest["keyFrames"], "keyframes/"))
        return Result<void>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                 "事件 manifest 文件路径或相机映射无效",
                                                 "event.recovery.verify"));

    if (!manifest.contains("fileChecksums") || !manifest["fileChecksums"].is_object() ||
        !manifest.contains("fileSizes") || !manifest["fileSizes"].is_object() ||
        manifest["fileChecksums"].size() != expected_paths.size() ||
        manifest["fileSizes"].size() != expected_paths.size())
        return Result<void>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                 "事件 manifest 校验清单与文件索引不一致",
                                                 "event.recovery.verify"));
    for (const auto& path : expected_paths)
    {
        if (!manifest["fileChecksums"].contains(path) || !manifest["fileSizes"].contains(path))
            return Result<void>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                     "事件 manifest 缺少文件校验项",
                                                     "event.recovery.verify"));
    }
    return Result<void>::success();
}

struct FileRecord final
{
    std::filesystem::path relative_path;
    std::size_t bytes{};
    std::string checksum;
};

Result<FileRecord> write_verified(IEventFileSystem& file_system,
                                  const std::filesystem::path& transaction_directory,
                                  const std::filesystem::path& relative_path,
                                  const std::span<const std::byte> contents,
                                  const std::stop_token stop_token)
{
    const auto absolute_path = transaction_directory / relative_path;
    auto write = file_system.write_new_file_buffered(absolute_path, contents, stop_token);
    if (!write)
        return Result<FileRecord>::failure(wrap_file_error(
            write.error(), "事件文件写入失败", "event.persist.write", transaction_directory));
    if (write.value().bytes_written != contents.size() || write.value().sha256.size() != 64U)
    {
        Error error =
            event_error("EVENT_CHECKSUM_FAILED", Severity::critical,
                        "事件文件写入长度或 CNG SHA-256 结果无效", "event.persist.write", false);
        error.details.push_back({.key = "file", .value = relative_path.generic_string()});
        error.details.push_back(
            {.key = "transactionDirectory", .value = transaction_directory.string()});
        return Result<FileRecord>::failure(std::move(error));
    }
    return Result<FileRecord>::success(
        {.relative_path = relative_path,
         .bytes = static_cast<std::size_t>(write.value().bytes_written),
         .checksum = std::move(write).value().sha256});
}

Json frame_json(const camera::FrameView& frame, const std::filesystem::path& relative_path)
{
    const auto geometry = frame.geometry();
    Json value{{"path", relative_path.generic_string()},
               {"cameraId", frame.camera_id()},
               {"cameraFrameNumber", frame.camera_frame_number()},
               {"sequenceNumber", frame.sequence_number()},
               {"wallClockTime", format_utc(frame.received_wall_clock_time())},
               {"monotonicNanoseconds", std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            frame.received_monotonic_time().time_since_epoch())
                                            .count()},
               {"width", geometry.width},
               {"height", geometry.height},
               {"stride", geometry.stride},
               {"pixelFormat", pixel_format_name(frame.pixel_format())},
               {"incomplete", frame.flags().incomplete}};
    return value;
}

template <typename T>
void put_little(std::span<std::byte> destination, const std::size_t offset, const T value) noexcept
{
    using Unsigned = std::make_unsigned_t<T>;
    const auto converted = static_cast<Unsigned>(value);
    for (std::size_t index = 0U; index < sizeof(T); ++index)
        destination[offset + index] =
            static_cast<std::byte>((converted >> (index * 8U)) & static_cast<Unsigned>(0xFFU));
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

std::uint64_t aligned_nvme_region(const std::uint64_t value) noexcept
{
    return (value + nvme_page_bytes - 1U) & ~(static_cast<std::uint64_t>(nvme_page_bytes) - 1U);
}

struct EncodedRawBlock final
{
    std::vector<std::byte> bytes;
    Json manifest;
};

Result<EncodedRawBlock> encode_raw_block(const std::string_view event_id,
                                         const std::filesystem::path& relative_path,
                                         const std::string_view camera_id,
                                         const std::span<const camera::FrameView> frames,
                                         const std::uint64_t ordinal,
                                         const std::size_t maximum_file_bytes)
{
    if (frames.empty() || frames.size() > event_raw_block_maximum_frames ||
        !is_canonical_camera_id(camera_id))
        return Result<EncodedRawBlock>::failure(
            event_error("EVENT_WRITE_FAILED", Severity::critical, "事件原始块参数无效",
                        "event.persist.rawBlock"));
    const auto geometry = frames.front().geometry();
    const auto format = frames.front().pixel_format();
    std::size_t maximum_frame_bytes{};
    for (std::size_t index = 0U; index < frames.size(); ++index)
    {
        const auto& frame = frames[index];
        maximum_frame_bytes = (std::max)(maximum_frame_bytes, frame.bytes().size());
        if (frame.camera_id() != camera_id || frame.geometry() != geometry ||
            frame.pixel_format() != format || frame.bytes().empty() ||
            (index != 0U && frame.sequence_number() <= frames[index - 1U].sequence_number()))
            return Result<EncodedRawBlock>::failure(
                event_error("EVENT_WRITE_FAILED", Severity::critical, "事件原始块帧布局无效",
                            "event.persist.rawBlock"));
    }
    if (maximum_frame_bytes > (std::numeric_limits<std::uint32_t>::max)())
        return Result<EncodedRawBlock>::failure(
            event_error("EVENT_WRITE_FAILED", Severity::critical, "事件原始块单帧过大",
                        "event.persist.rawBlock"));
    const auto index_capacity = static_cast<std::uint32_t>(frames.size());
    auto maximum =
        maximum_nvme_block_bytes(index_capacity, static_cast<std::uint32_t>(maximum_frame_bytes));
    if (!maximum || maximum.value() > maximum_file_bytes ||
        maximum.value() > (std::numeric_limits<std::size_t>::max)())
        return Result<EncodedRawBlock>::failure(
            event_error("EVENT_WRITE_FAILED", Severity::critical, "事件原始块超过固定文件上限",
                        "event.persist.rawBlock"));

    EncodedRawBlock result;
    result.bytes.resize(static_cast<std::size_t>(maximum.value()));
    auto bytes = std::span<std::byte>{result.bytes};
    constexpr std::array<std::byte, 8U> header_magic{std::byte{'P'}, std::byte{'B'}, std::byte{'N'},
                                                     std::byte{'V'}, std::byte{'M'}, std::byte{'E'},
                                                     std::byte{'2'}, std::byte{0U}};
    std::ranges::copy(header_magic, bytes.begin());
    put_little<std::uint16_t>(bytes, 8U, nvme_format_version);
    put_little<std::uint16_t>(bytes, 10U, nvme_page_bytes);
    put_little<std::uint32_t>(bytes, 12U, 0x01020304U);
    std::uint64_t identity_left = 1469598103934665603ULL;
    std::uint64_t identity_right = ordinal + 0x9E3779B97F4A7C15ULL;
    const auto mix = [&](const std::string_view value) {
        for (const unsigned char character : value)
        {
            identity_left = (identity_left ^ character) * 1099511628211ULL;
            identity_right = (identity_right << 7U) ^ (identity_right >> 3U) ^ character;
        }
    };
    mix(event_id);
    mix(camera_id);
    for (std::size_t index = 0U; index < 8U; ++index)
    {
        bytes[16U + index] = static_cast<std::byte>((identity_left >> (index * 8U)) & 0xffU);
        bytes[24U + index] = static_cast<std::byte>((identity_right >> (index * 8U)) & 0xffU);
    }
    put_little<std::uint64_t>(bytes, 32U, ordinal + 1U);
    std::ranges::copy(std::as_bytes(std::span{camera_id.data(), camera_id.size()}),
                      bytes.begin() + 40U);
    put_little<std::uint16_t>(bytes, 56U, static_cast<std::uint16_t>(camera_id.size()));
    put_little<std::uint16_t>(bytes, 58U, pixel_format_id(format));
    put_little<std::uint32_t>(bytes, 60U, geometry.width);
    put_little<std::uint32_t>(bytes, 64U, geometry.height);
    put_little<std::uint32_t>(bytes, 68U, geometry.stride);
    put_little<std::uint32_t>(bytes, 76U,
                              static_cast<std::uint32_t>(event_raw_block_duration.count()));
    put_little<std::uint32_t>(bytes, 80U, nvme_index_entry_bytes);
    put_little<std::uint32_t>(bytes, 84U, index_capacity);
    put_little<std::uint32_t>(bytes, 88U, nvme_page_bytes);
    put_little<std::uint32_t>(bytes, 92U, static_cast<std::uint32_t>(maximum_frame_bytes));
    const auto wall_nanoseconds = [](const camera::WallClockTime value) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch())
            .count();
    };
    put_little<std::int64_t>(bytes, 96U,
                             wall_nanoseconds(frames.front().received_wall_clock_time()));
    put_little<std::uint64_t>(bytes, 120U, frames.front().sequence_number());
    const auto header_crc = crc32c(bytes.first(nvme_page_bytes));
    put_little<std::uint32_t>(bytes, 128U, header_crc);

    const auto index_reserved =
        aligned_nvme_region(static_cast<std::uint64_t>(index_capacity) * nvme_index_entry_bytes);
    const auto data_start = static_cast<std::uint64_t>(nvme_page_bytes) + index_reserved;
    std::uint64_t data_offset = data_start;
    for (std::size_t frame_index = 0U; frame_index < frames.size(); ++frame_index)
    {
        const auto& frame = frames[frame_index];
        auto entry = bytes.subspan(nvme_page_bytes + frame_index * nvme_index_entry_bytes,
                                   nvme_index_entry_bytes);
        put_little<std::uint64_t>(entry, 0U, frame.sequence_number());
        put_little<std::uint64_t>(entry, 8U, frame.camera_frame_number());
        const auto monotonic_delta =
            frame.received_monotonic_time() <= frames.front().received_monotonic_time()
                ? 0U
                : static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 frame.received_monotonic_time() -
                                                 frames.front().received_monotonic_time())
                                                 .count());
        put_little<std::uint64_t>(entry, 16U, monotonic_delta);
        put_little<std::int64_t>(entry, 24U, wall_nanoseconds(frame.received_wall_clock_time()));
        put_little<std::uint64_t>(entry, 48U, data_offset);
        put_little<std::uint32_t>(entry, 56U, static_cast<std::uint32_t>(frame.bytes().size()));
        put_little<std::uint32_t>(entry, 60U, geometry.width);
        put_little<std::uint32_t>(entry, 64U, geometry.height);
        put_little<std::uint32_t>(entry, 68U, geometry.stride);
        put_little<std::uint16_t>(entry, 72U, pixel_format_id(format));
        put_little<std::uint16_t>(entry, 74U, frame.flags().incomplete ? 1U : 0U);
        put_little<std::uint32_t>(entry, 76U, 0U);
        put_little<std::uint32_t>(entry, 80U, crc32c(entry));
        std::ranges::copy(frame.bytes(), bytes.begin() + static_cast<std::ptrdiff_t>(data_offset));
        data_offset += frame.bytes().size();
    }
    const auto index_bytes = static_cast<std::uint64_t>(frames.size()) * nvme_index_entry_bytes;
    const auto index_crc =
        crc32c(bytes.subspan(nvme_page_bytes, static_cast<std::size_t>(index_bytes)));
    auto footer = bytes.last(nvme_page_bytes);
    constexpr std::array<std::byte, 8U> footer_magic{std::byte{'P'}, std::byte{'B'}, std::byte{'C'},
                                                     std::byte{'O'}, std::byte{'M'}, std::byte{'M'},
                                                     std::byte{'I'}, std::byte{'T'}};
    constexpr std::array<std::byte, 8U> marker{std::byte{'C'}, std::byte{'O'}, std::byte{'M'},
                                               std::byte{'M'}, std::byte{'I'}, std::byte{'T'},
                                               std::byte{'2'}, std::byte{0U}};
    std::ranges::copy(footer_magic, footer.begin());
    put_little<std::uint16_t>(footer, 8U, nvme_format_version);
    put_little<std::uint16_t>(footer, 10U, nvme_page_bytes);
    put_little<std::uint32_t>(footer, 12U, static_cast<std::uint32_t>(frames.size()));
    put_little<std::uint64_t>(footer, 16U, index_bytes);
    put_little<std::uint64_t>(footer, 24U, data_offset - data_start);
    put_little<std::uint64_t>(footer, 32U, maximum.value());
    put_little<std::int64_t>(footer, 40U,
                             wall_nanoseconds(frames.back().received_wall_clock_time()));
    put_little<std::uint64_t>(footer, 48U, frames.back().sequence_number());
    put_little<std::uint32_t>(footer, 56U, index_crc);
    put_little<std::uint32_t>(footer, 60U, 0U);
    put_little<std::uint32_t>(footer, 64U, header_crc);
    std::ranges::copy(marker, footer.begin() + 4088U);
    const auto footer_crc = crc32c(footer);
    put_little<std::uint32_t>(footer, 4084U, footer_crc);
    result.manifest = {
        {"path", relative_path.generic_string()},
        {"cameraId", camera_id},
        {"startTime", format_utc(frames.front().received_wall_clock_time())},
        {"endTime", format_utc(frames.back().received_wall_clock_time())},
        {"startMonotonicNanoseconds",
         std::chrono::duration_cast<std::chrono::nanoseconds>(
             frames.front().received_monotonic_time().time_since_epoch())
             .count()},
        {"endMonotonicNanoseconds", std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        frames.back().received_monotonic_time().time_since_epoch())
                                        .count()},
        {"firstCameraFrameNumber", frames.front().camera_frame_number()},
        {"lastCameraFrameNumber", frames.back().camera_frame_number()},
        {"firstSequenceNumber", frames.front().sequence_number()},
        {"lastSequenceNumber", frames.back().sequence_number()},
        {"frameCount", frames.size()},
        {"sizeBytes", result.bytes.size()},
        {"headerCrc32c", header_crc},
        {"indexCrc32c", index_crc},
        {"dataCrc32c", 0U},
        {"footerCrc32c", footer_crc}};
    return Result<EncodedRawBlock>::success(std::move(result));
}

Json key_frame_json(const PersistedKeyFrame& key_frame, const std::filesystem::path& relative_path)
{
    const auto& descriptor = key_frame.descriptor;
    Json reasons = Json::array();
    for (const auto reason : descriptor.reasons)
        reasons.push_back(event::to_string(reason));
    return {{"path", relative_path.generic_string()},
            {"cameraId", descriptor.camera_id},
            {"cameraFrameNumber", descriptor.camera_frame_number},
            {"sequenceNumber", descriptor.sequence_number},
            {"wallClockTime", format_utc(descriptor.wall_clock_time)},
            {"monotonicNanoseconds", std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         descriptor.monotonic_time.time_since_epoch())
                                         .count()},
            {"width", descriptor.geometry.width},
            {"height", descriptor.geometry.height},
            {"stride", descriptor.geometry.stride},
            {"pixelFormat", pixel_format_name(descriptor.pixel_format)},
            {"reasons", std::move(reasons)}};
}

Result<void> verify_manifest_files(IEventFileSystem& file_system,
                                   const std::filesystem::path& directory, const Json& manifest,
                                   const EventStoreOptions& options)
{
    auto shape = validate_manifest_shape(manifest, options);
    if (!shape)
        return shape;
    if (!manifest.contains("fileChecksums") || !manifest["fileChecksums"].is_object() ||
        !manifest.contains("fileSizes") || !manifest["fileSizes"].is_object())
        return Result<void>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                 "事件 manifest 缺少文件校验信息",
                                                 "event.recovery.verify"));
    const auto& checksums = manifest["fileChecksums"];
    const auto& sizes = manifest["fileSizes"];
    if (checksums.empty() || checksums.size() != sizes.size() ||
        checksums.size() > options.maximum_raw_frames + options.maximum_key_frames + 1U)
        return Result<void>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                 "事件 manifest 文件清单数量无效",
                                                 "event.recovery.verify"));

    for (const auto& [key, checksum_value] : checksums.items())
    {
        const std::filesystem::path relative{key};
        if (!safe_relative_path(relative) || key == "manifest.json" ||
            !checksum_value.is_string() || !sizes.contains(key) || !sizes[key].is_number_unsigned())
            return Result<void>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                     "事件 manifest 文件项无效",
                                                     "event.recovery.verify"));
        const auto expected_size = sizes[key].get<std::uint64_t>();
        if (expected_size == 0U || expected_size > options.maximum_file_bytes)
            return Result<void>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                     "事件 manifest 文件大小无效",
                                                     "event.recovery.verify"));
        auto actual_size = file_system.file_size(directory / relative);
        if (!actual_size)
            return Result<void>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                     "manifest 引用文件不存在或类型错误",
                                                     "event.recovery.verify", true));
        const std::string expected = checksum_value.get<std::string>();
        if (actual_size.value() != expected_size || !expected.starts_with("sha256:") ||
            expected.size() != 71U)
            return Result<void>::failure(event_error("EVENT_CHECKSUM_FAILED", Severity::critical,
                                                     "事件文件长度或摘要声明无效",
                                                     "event.recovery.verify"));
    }
    return Result<void>::success();
}

Result<Json> parse_manifest(const std::span<const std::byte> bytes)
{
    try
    {
        const auto begin = reinterpret_cast<const char*>(bytes.data());
        Json value = Json::parse(begin, begin + bytes.size());
        if (!value.is_object() || !value.contains("schemaVersion") ||
            !value["schemaVersion"].is_number_unsigned())
            return Result<Json>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                     "事件 manifest 结构无效",
                                                     "event.recovery.parse"));
        const auto schema = value["schemaVersion"].get<std::uint32_t>();
        if (schema != event_manifest_schema_version &&
            schema != event_manifest_legacy_schema_version)
            return Result<Json>::failure(event_error("EVENT_SCHEMA_UNSUPPORTED", Severity::error,
                                                     "事件 manifest schema 不受支持",
                                                     "event.recovery.parse"));
        if (!value.contains("eventId") || !value["eventId"].is_string() ||
            !safe_event_id(value["eventId"].get_ref<const std::string&>()) ||
            !value.contains("destinationRelativePath") ||
            !value["destinationRelativePath"].is_string())
            return Result<Json>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                     "事件 manifest 标识或目标路径无效",
                                                     "event.recovery.parse"));
        const std::filesystem::path destination{
            value["destinationRelativePath"].get<std::string>()};
        if (!valid_destination_path(destination, value["eventId"].get<std::string>()))
            return Result<Json>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                     "事件 manifest 目标路径越界",
                                                     "event.recovery.parse"));
        return Result<Json>::success(std::move(value));
    }
    catch (const std::exception&)
    {
        return Result<Json>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                 "事件 manifest JSON 无法解析",
                                                 "event.recovery.parse"));
    }
}

std::string json_text(const Json& value)
{
    return value.dump(2, ' ', false, Json::error_handler_t::strict) + "\n";
}

} // namespace

struct EventTransactionWriter::Impl final
{
    Impl(EventStoreOptions value, std::shared_ptr<IEventFileSystem> file_system_value)
        : options(std::move(value)), file_system(std::move(file_system_value))
    {
    }

    [[nodiscard]] std::filesystem::path transactions_root() const
    {
        return options.event_root / ".transactions";
    }

    [[nodiscard]] std::filesystem::path quarantine_root() const
    {
        return options.event_root / ".quarantine";
    }

    Result<void> initialize_roots() const
    {
        for (const auto& path : {options.event_root, transactions_root(), quarantine_root()})
        {
            auto created = file_system->create_directories(path);
            if (!created)
                return Result<void>::failure(wrap_file_error(
                    created.error(), "无法初始化事件事务目录", "event.persist.initialize"));
        }
        return Result<void>::success();
    }

    Result<EventRecoveryItem> quarantine(const std::filesystem::path& source, std::string event_id,
                                         std::string reason) const
    {
        auto marker_kind = file_system->path_kind(source / "recovery.json");
        if (marker_kind && marker_kind.value() == EventPathKind::missing)
        {
            const auto marker = json_text(Json{{"schemaVersion", 1},
                                               {"status", "Damaged"},
                                               {"eventId", event_id},
                                               {"reason", reason},
                                               {"recoveredAt", current_utc_timestamp()}});
            static_cast<void>(
                file_system->write_new_file_buffered(source / "recovery.json", bytes_of(marker)));
        }

        for (std::size_t attempt = 0U; attempt < 1024U; ++attempt)
        {
            const auto suffix = attempt == 0U ? std::string{} : "." + std::to_string(attempt);
            const auto destination =
                quarantine_root() / (source.filename().string() + ".damaged" + suffix);
            auto kind = file_system->path_kind(destination);
            if (!kind)
                return Result<EventRecoveryItem>::failure(
                    event_error("EVENT_RECOVERY_FAILED", Severity::critical, "无法检查事件隔离目录",
                                "event.recovery.quarantine", true));
            if (kind.value() != EventPathKind::missing)
                continue;
            auto moved = file_system->move_directory_atomically(source, destination);
            if (!moved)
                return Result<EventRecoveryItem>::failure(
                    event_error("EVENT_RECOVERY_FAILED", Severity::critical, "无法隔离未完成事件",
                                "event.recovery.quarantine", true));
            return Result<EventRecoveryItem>::success(
                {.original_directory = source,
                 .resulting_directory = destination,
                 .disposition = EventRecoveryDisposition::quarantined,
                 .event_id = std::move(event_id),
                 .reason = std::move(reason)});
        }
        return Result<EventRecoveryItem>::failure(
            event_error("EVENT_RECOVERY_FAILED", Severity::critical, "事件隔离目录命名空间已耗尽",
                        "event.recovery.quarantine"));
    }

    EventStoreOptions options;
    std::shared_ptr<IEventFileSystem> file_system;
    std::atomic_uint64_t transaction_sequence{};
};

Result<std::unique_ptr<EventTransactionWriter>> EventTransactionWriter::create(
    EventStoreOptions options, std::shared_ptr<IEventFileSystem> file_system)
{
    std::error_code path_error;
    auto absolute_root =
        std::filesystem::absolute(options.event_root, path_error).lexically_normal();
    if (path_error || options.event_root.empty() || !file_system ||
        options.maximum_raw_frames == 0U ||
        options.maximum_raw_frames > absolute_maximum_raw_frames ||
        options.maximum_key_frames == 0U ||
        options.maximum_key_frames > event::key_frame_reason_count ||
        options.maximum_file_bytes == 0U ||
        options.maximum_file_bytes > absolute_maximum_file_bytes ||
        options.maximum_manifest_bytes == 0U ||
        options.maximum_manifest_bytes > absolute_maximum_manifest_bytes ||
        options.maximum_recovery_entries == 0U ||
        options.maximum_recovery_entries > absolute_maximum_recovery_entries)
        return Result<std::unique_ptr<EventTransactionWriter>>::failure(event_error(
            "SYS_CONFIG_INVALID", Severity::error, "事件存储配置无效", "event.persist.create"));
    options.event_root = std::move(absolute_root);
    return Result<std::unique_ptr<EventTransactionWriter>>::success(
        std::make_unique<EventTransactionWriter>(ConstructionKey{}, std::move(options),
                                                 std::move(file_system)));
}

EventTransactionWriter::EventTransactionWriter(ConstructionKey, EventStoreOptions options,
                                               std::shared_ptr<IEventFileSystem> file_system)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(file_system)))
{
}

EventTransactionWriter::~EventTransactionWriter() = default;

Result<EventPersistenceOutcome> EventTransactionWriter::persist(
    const EventPersistenceRequest& request)
{
    return persist(request, {});
}

Result<EventPersistenceOutcome> EventTransactionWriter::persist(
    const EventPersistenceRequest& request, const std::stop_token stop_token)
{
    const auto cancelled = [&]() -> std::optional<Result<EventPersistenceOutcome>> {
        if (!stop_token.stop_requested())
            return std::nullopt;
        return Result<EventPersistenceOutcome>::failure(
            event_error("EVENT_WRITE_CANCELLED", Severity::warning,
                        "事件持久化已取消，事务目录已保留", "event.persist.cancel", true));
    };
    if (auto stopped = cancelled())
        return std::move(*stopped);
    auto valid = validate_request(request, impl_->options);
    if (!valid)
        return Result<EventPersistenceOutcome>::failure(std::move(valid).error());
    auto initialized = impl_->initialize_roots();
    if (!initialized)
        return Result<EventPersistenceOutcome>::failure(std::move(initialized).error());

    const auto relative_destination =
        std::filesystem::path{date_path(request.metadata.candidate_time)} /
        request.metadata.event_id;
    if (!valid_destination_path(relative_destination, request.metadata.event_id))
        return Result<EventPersistenceOutcome>::failure(
            event_error("EVENT_WRITE_FAILED", Severity::critical, "无法生成事件正式目录",
                        "event.persist.path"));
    const auto destination = impl_->options.event_root / relative_destination;
    auto destination_kind = impl_->file_system->path_kind(destination);
    if (!destination_kind)
        return Result<EventPersistenceOutcome>::failure(wrap_file_error(
            destination_kind.error(), "无法预检事件正式目录", "event.persist.preflight"));
    if (destination_kind.value() != EventPathKind::missing)
        return Result<EventPersistenceOutcome>::failure(
            event_error("EVENT_WRITE_FAILED", Severity::critical, "正式事件目录已存在",
                        "event.persist.preflight"));

    std::filesystem::path transaction;
    for (std::size_t attempt = 0U; attempt < 16U; ++attempt)
    {
        const auto sequence = impl_->transaction_sequence.fetch_add(1U, std::memory_order_relaxed);
        transaction = impl_->transactions_root() /
                      (request.metadata.event_id + "." + std::to_string(sequence) + ".pending");
        auto created = impl_->file_system->create_directory_exclusive(transaction);
        if (created)
            break;
        if (attempt == 15U)
            return Result<EventPersistenceOutcome>::failure(wrap_file_error(
                created.error(), "无法创建唯一事件事务目录", "event.persist.begin"));
        transaction.clear();
    }
    if (auto stopped = cancelled())
        return std::move(*stopped);

    Json manifest = metadata_json(request.metadata, relative_destination);
    manifest["windowComplete"] = request.window.complete;
    manifest["truncatedByMaximumDuration"] = request.window.truncated_by_maximum_duration;
    manifest["stoppedEarly"] = request.window.stopped_early;
    manifest["rawBlocks"] = Json::array();
    manifest["keyFrames"] = Json::array();
    std::vector<FileRecord> records;
    records.reserve(1U + request.key_frames.size());

    const auto event_metadata_text =
        json_text(metadata_json(request.metadata, relative_destination));
    auto event_metadata_record = write_verified(*impl_->file_system, transaction, "event.json",
                                                bytes_of(event_metadata_text), stop_token);
    if (!event_metadata_record)
        return Result<EventPersistenceOutcome>::failure(std::move(event_metadata_record).error());
    records.push_back(std::move(event_metadata_record).value());

    std::size_t raw_count = 0U;
    std::size_t raw_block_count = 0U;
    for (std::size_t camera_index = 0U; camera_index < request.window.camera_windows.size();
         ++camera_index)
    {
        const auto camera_directory =
            std::filesystem::path{"raw"} / ("camera-" + std::to_string(camera_index));
        auto created = impl_->file_system->create_directories(transaction / camera_directory);
        if (!created)
            return Result<EventPersistenceOutcome>::failure(wrap_file_error(
                created.error(), "无法创建原始帧目录", "event.persist.rawDirectory", transaction));
        const auto& frames = request.window.camera_windows[camera_index].frames;
        std::size_t block_begin = 0U;
        while (block_begin < frames.size())
        {
            if (auto stopped = cancelled())
                return std::move(*stopped);
            const auto bucket =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    frames[block_begin].received_monotonic_time().time_since_epoch())
                    .count() /
                event_raw_block_duration.count();
            const auto geometry = frames[block_begin].geometry();
            const auto format = frames[block_begin].pixel_format();
            std::size_t block_end = block_begin + 1U;
            while (block_end < frames.size() &&
                   block_end - block_begin < event_raw_block_maximum_frames &&
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       frames[block_end].received_monotonic_time().time_since_epoch())
                               .count() /
                           event_raw_block_duration.count() ==
                       bucket &&
                   frames[block_end].geometry() == geometry &&
                   frames[block_end].pixel_format() == format)
                ++block_end;
            const auto relative =
                camera_directory / ("block-" + std::to_string(raw_block_count) + ".pbnvme");
            auto encoded = encode_raw_block(request.metadata.event_id, relative,
                                            request.window.camera_windows[camera_index].camera_id,
                                            std::span<const camera::FrameView>{frames}.subspan(
                                                block_begin, block_end - block_begin),
                                            raw_block_count, impl_->options.maximum_file_bytes);
            if (!encoded)
                return Result<EventPersistenceOutcome>::failure(std::move(encoded).error());
            auto write = impl_->file_system->write_new_raw_block_buffered(
                transaction / relative, encoded.value().bytes, stop_token);
            if (!write)
                return Result<EventPersistenceOutcome>::failure(wrap_file_error(
                    write.error(), "原始块缓冲写入失败", "event.persist.rawBlock", transaction));
            if (write.value().bytes_written != encoded.value().bytes.size() ||
                write.value().sha256.size() != 64U)
                return Result<EventPersistenceOutcome>::failure(
                    event_error("EVENT_CHECKSUM_FAILED", Severity::critical,
                                "原始块写入长度或 CNG SHA-256 结果无效", "event.persist.rawBlock"));
            records.push_back({.relative_path = relative,
                               .bytes = static_cast<std::size_t>(write.value().bytes_written),
                               .checksum = write.value().sha256});
            auto raw_manifest = std::move(encoded).value().manifest;
            raw_manifest["sha256"] = "sha256:" + write.value().sha256;
            manifest["rawBlocks"].push_back(std::move(raw_manifest));
            raw_count += block_end - block_begin;
            ++raw_block_count;
            block_begin = block_end;
        }
    }

    if (!request.key_frames.empty())
    {
        auto created = impl_->file_system->create_directories(transaction / "keyframes");
        if (!created)
            return Result<EventPersistenceOutcome>::failure(
                wrap_file_error(created.error(), "无法创建关键帧目录",
                                "event.persist.keyframeDirectory", transaction));
    }
    for (std::size_t index = 0U; index < request.key_frames.size(); ++index)
    {
        if (auto stopped = cancelled())
            return std::move(*stopped);
        const auto relative =
            std::filesystem::path{"keyframes"} / ("keyframe-" + std::to_string(index) + ".jpg");
        auto record = write_verified(*impl_->file_system, transaction, relative,
                                     request.key_frames[index].jpeg, stop_token);
        if (!record)
            return Result<EventPersistenceOutcome>::failure(std::move(record).error());
        manifest["keyFrames"].push_back(key_frame_json(request.key_frames[index], relative));
        records.push_back(std::move(record).value());
    }

    Json checksums = Json::object();
    Json sizes = Json::object();
    for (const auto& record : records)
    {
        const auto path = record.relative_path.generic_string();
        checksums[path] = "sha256:" + record.checksum;
        sizes[path] = record.bytes;
    }
    manifest["fileChecksums"] = std::move(checksums);
    manifest["fileSizes"] = std::move(sizes);
    if (auto stopped = cancelled())
        return std::move(*stopped);
    const auto manifest_text = json_text(manifest);
    if (manifest_text.size() > impl_->options.maximum_manifest_bytes)
        return Result<EventPersistenceOutcome>::failure(
            event_error("EVENT_WRITE_FAILED", Severity::critical, "事件 manifest 超过固定大小上限",
                        "event.persist.manifest"));
    auto manifest_write = impl_->file_system->write_new_file_buffered(
        transaction / "manifest.json", bytes_of(manifest_text), stop_token);
    if (!manifest_write)
        return Result<EventPersistenceOutcome>::failure(
            wrap_file_error(manifest_write.error(), "事件 manifest 写入失败",
                            "event.persist.manifest", transaction));
    if (manifest_write.value().bytes_written != manifest_text.size() ||
        manifest_write.value().sha256.size() != 64U)
        return Result<EventPersistenceOutcome>::failure(
            event_error("EVENT_CHECKSUM_FAILED", Severity::critical, "事件 manifest 写入结果无效",
                        "event.persist.manifest"));

    auto parent = impl_->file_system->create_directories(destination.parent_path());
    if (!parent)
        return Result<EventPersistenceOutcome>::failure(wrap_file_error(
            parent.error(), "无法创建事件日期目录", "event.persist.commit", transaction));
    destination_kind = impl_->file_system->path_kind(destination);
    if (!destination_kind)
        return Result<EventPersistenceOutcome>::failure(wrap_file_error(
            destination_kind.error(), "无法复检事件正式目录", "event.persist.commit", transaction));
    if (destination_kind.value() != EventPathKind::missing)
        return Result<EventPersistenceOutcome>::failure(
            event_error("EVENT_WRITE_FAILED", Severity::critical, "正式事件目录已存在",
                        "event.persist.commit"));
    auto moved = impl_->file_system->move_directory_atomically(transaction, destination);
    if (!moved)
        return Result<EventPersistenceOutcome>::failure(wrap_file_error(
            moved.error(), "事件目录原子提交失败", "event.persist.commit", transaction));
    std::uint64_t bytes_written = manifest_text.size();
    for (const auto& record : records)
        bytes_written += record.bytes;
    return Result<EventPersistenceOutcome>::success({.event_id = request.metadata.event_id,
                                                     .committed_directory = destination,
                                                     .transaction_directory = transaction,
                                                     .manifest_json = manifest_text,
                                                     .raw_block_count = raw_block_count,
                                                     .raw_frame_count = raw_count,
                                                     .raw_file_count = raw_block_count,
                                                     .key_frame_count = request.key_frames.size(),
                                                     .bytes_written = bytes_written});
}

Result<EventRecoveryReport> EventTransactionWriter::recover_pending()
{
    auto initialized = impl_->initialize_roots();
    if (!initialized)
        return Result<EventRecoveryReport>::failure(
            event_error("EVENT_RECOVERY_FAILED", Severity::critical, "无法初始化事件恢复目录",
                        "event.recovery.initialize", true));
    auto directories = impl_->file_system->list_directories_bounded(
        impl_->transactions_root(), impl_->options.maximum_recovery_entries);
    if (!directories)
        return Result<EventRecoveryReport>::failure(
            event_error("EVENT_RECOVERY_FAILED", Severity::critical, "无法扫描未完成事件",
                        "event.recovery.scan", true));

    EventRecoveryReport report;
    report.items.reserve(directories.value().size());
    for (const auto& directory : directories.value())
    {
        ++report.scanned;
        std::string event_id;
        std::string quarantine_reason;
        auto manifest_kind = impl_->file_system->path_kind(directory / "manifest.json");
        if (!manifest_kind || manifest_kind.value() != EventPathKind::regular_file)
        {
            quarantine_reason = "manifest-missing";
        }
        else
        {
            auto manifest_bytes = impl_->file_system->read_file_bounded(
                directory / "manifest.json", impl_->options.maximum_manifest_bytes);
            if (!manifest_bytes)
                quarantine_reason = "manifest-unreadable";
            else
            {
                auto parsed = parse_manifest(manifest_bytes.value());
                if (!parsed)
                    quarantine_reason = parsed.error().business_code;
                else
                {
                    const auto& manifest = parsed.value();
                    event_id = manifest["eventId"].get<std::string>();
                    auto verified = verify_manifest_files(*impl_->file_system, directory, manifest,
                                                          impl_->options);
                    if (!verified)
                        quarantine_reason = verified.error().business_code;
                    else
                    {
                        const auto relative = std::filesystem::path{
                            manifest["destinationRelativePath"].get<std::string>()};
                        const auto destination = impl_->options.event_root / relative;
                        auto parent =
                            impl_->file_system->create_directories(destination.parent_path());
                        if (!parent)
                            return Result<EventRecoveryReport>::failure(event_error(
                                "EVENT_RECOVERY_FAILED", Severity::critical,
                                "恢复时无法创建事件日期目录", "event.recovery.commit", true));
                        auto destination_kind = impl_->file_system->path_kind(destination);
                        if (!destination_kind)
                            return Result<EventRecoveryReport>::failure(event_error(
                                "EVENT_RECOVERY_FAILED", Severity::critical,
                                "恢复时无法检查正式事件目录", "event.recovery.commit", true));
                        if (destination_kind.value() == EventPathKind::missing)
                        {
                            auto moved = impl_->file_system->move_directory_atomically(directory,
                                                                                       destination);
                            if (!moved)
                                return Result<EventRecoveryReport>::failure(event_error(
                                    "EVENT_RECOVERY_FAILED", Severity::critical,
                                    "无法提交已完成残留事件", "event.recovery.commit", true));
                            report.items.push_back(
                                {.original_directory = directory,
                                 .resulting_directory = destination,
                                 .disposition = EventRecoveryDisposition::committed,
                                 .event_id = event_id,
                                 .reason = "validated-and-committed"});
                            ++report.committed;
                            continue;
                        }
                        quarantine_reason = "destination-already-exists";
                    }
                }
            }
        }

        auto quarantined = impl_->quarantine(directory, std::move(event_id), quarantine_reason);
        if (!quarantined)
            return Result<EventRecoveryReport>::failure(std::move(quarantined).error());
        report.items.push_back(std::move(quarantined).value());
        ++report.quarantined;
    }
    return Result<EventRecoveryReport>::success(std::move(report));
}

Result<std::string> EventTransactionWriter::verify_committed_manifest(
    const std::filesystem::path& committed_directory) const
{
    std::error_code path_error;
    const auto absolute =
        std::filesystem::absolute(committed_directory, path_error).lexically_normal();
    if (path_error)
        return Result<std::string>::failure(event_error(
            "EVENT_NOT_FOUND", Severity::error, "正式事件目录路径无效", "event.read.verify"));
    const auto relative = absolute.lexically_relative(impl_->options.event_root);
    std::vector<std::filesystem::path> components;
    for (const auto& component : relative)
        components.push_back(component);
    if (!safe_relative_path(relative) || components.size() != 4U ||
        components.front().string().starts_with('.'))
        return Result<std::string>::failure(event_error(
            "EVENT_NOT_FOUND", Severity::error, "路径不是已提交事件目录", "event.read.verify"));
    auto bytes = impl_->file_system->read_file_bounded(absolute / "manifest.json",
                                                       impl_->options.maximum_manifest_bytes);
    if (!bytes)
        return Result<std::string>::failure(event_error(
            "EVENT_NOT_FOUND", Severity::error, "正式事件 manifest 不可读取", "event.read.verify"));
    auto parsed = parse_manifest(bytes.value());
    if (!parsed)
        return Result<std::string>::failure(std::move(parsed).error());
    if (parsed.value()["eventId"].get<std::string>() != absolute.filename().string())
        return Result<std::string>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                        "正式事件目录标识与 manifest 不一致",
                                                        "event.read.verify"));
    if (std::filesystem::path{parsed.value()["destinationRelativePath"].get<std::string>()} !=
        relative)
        return Result<std::string>::failure(event_error("EVENT_RECOVERY_FAILED", Severity::critical,
                                                        "正式事件路径与 manifest 目标路径不一致",
                                                        "event.read.verify"));
    auto verified =
        verify_manifest_files(*impl_->file_system, absolute, parsed.value(), impl_->options);
    if (!verified)
        return Result<std::string>::failure(std::move(verified).error());
    return Result<std::string>::success(
        std::string{reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()});
}

struct EventPersistenceRuntime::Impl final
{
    struct Job final
    {
        EventPersistenceRequest request;
    };

    Impl(std::unique_ptr<IEventTransactionWriter> writer_value,
         EventPersistenceCallback callback_value, const EventPersistenceRuntimeOptions value)
        : writer(std::move(writer_value)), callback(std::move(callback_value)), options(value),
          jobs(options.event_capacity)
    {
    }

    void run() noexcept
    {
        const auto thread_registration =
            options.register_thread ? options.register_thread("event-persistence") : nullptr;
        for (;;)
        {
            std::unique_ptr<Job> job;
            {
                std::unique_lock lock{mutex};
                condition.wait(lock, [this] { return depth != 0U || stopping; });
                if (depth == 0U && stopping)
                    break;
                job = std::move(jobs[read_index]);
                read_index = (read_index + 1U) % jobs.size();
                --depth;
            }

            EventPersistenceCompletion completion{.event_id = job->request.metadata.event_id};
            std::size_t frame_count = 0U;
            for (const auto& camera_window : job->request.window.camera_windows)
                frame_count += camera_window.frames.size();
            const std::size_t key_frame_count = job->request.key_frames.size();
            const auto write_started = std::chrono::steady_clock::now();
            {
                std::scoped_lock lock{mutex};
                active_events = 1U;
            }
            try
            {
                if (options.writing_observer)
                    options.writing_observer(job->request.metadata.event_id);
            }
            catch (...)
            {
                std::scoped_lock lock{mutex};
                ++callback_failures;
            }
            try
            {
                auto result = writer->persist(job->request);
                std::scoped_lock lock{mutex};
                ++completed;
                if (result)
                {
                    completion.outcome = std::move(result).value();
                    last_write_bytes = completion.outcome->bytes_written;
                }
                else
                {
                    ++write_failures;
                    completion.error = std::move(result).error();
                }
            }
            catch (const std::exception&)
            {
                std::scoped_lock lock{mutex};
                ++completed;
                ++write_failures;
                completion.error = event_error("EVENT_WRITE_FAILED", Severity::critical,
                                               "事件写入器抛出异常", "event.persist.worker", true);
            }
            const auto write_finished = std::chrono::steady_clock::now();
            {
                std::scoped_lock lock{mutex};
                active_events = 0U;
                last_write_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    write_finished - write_started);
                const auto seconds =
                    std::chrono::duration<double>(write_finished - write_started).count();
                last_write_mib_per_second = seconds > 0.0 ? static_cast<double>(last_write_bytes) /
                                                                (1024.0 * 1024.0 * seconds)
                                                          : 0.0;
            }

            const bool failed = completion.error.has_value();
            const std::string business_code = failed ? completion.error->business_code : "OK";
            if (options.diagnostics.enabled && options.diagnostics.enabled() &&
                options.diagnostics.record)
            {
                options.diagnostics.record(
                    "operation=event.persist eventId=" + completion.event_id +
                    " frameCount=" + std::to_string(frame_count) +
                    " keyFrameCount=" + std::to_string(key_frame_count) + " result=" +
                    (failed ? "failure" : "success") + " businessCode=" + business_code);
            }

            try
            {
                callback(std::move(completion));
            }
            catch (const std::exception&)
            {
                std::scoped_lock lock{mutex};
                ++callback_failures;
            }
        }
        {
            std::scoped_lock lock{mutex};
            completed_run = true;
        }
        completed_condition.notify_all();
    }

    std::unique_ptr<IEventTransactionWriter> writer;
    EventPersistenceCallback callback;
    EventPersistenceRuntimeOptions options;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::condition_variable completed_condition;
    std::vector<std::unique_ptr<Job>> jobs;
    std::size_t read_index{};
    std::size_t write_index{};
    std::size_t depth{};
    std::jthread worker;
    bool started{};
    bool stopping{};
    bool completed_run{true};
    std::size_t high_watermark{};
    std::uint64_t submitted{};
    std::uint64_t completed{};
    std::uint64_t rejected{};
    std::uint64_t write_failures{};
    std::uint64_t callback_failures{};
    std::size_t active_events{};
    std::uint64_t last_write_bytes{};
    std::chrono::milliseconds last_write_duration{};
    double last_write_mib_per_second{};
};

Result<std::unique_ptr<EventPersistenceRuntime>> EventPersistenceRuntime::create(
    std::unique_ptr<IEventTransactionWriter> writer, EventPersistenceCallback callback,
    const EventPersistenceRuntimeOptions options)
{
    if (!writer || !callback || options.event_capacity == 0U ||
        options.event_capacity > maximum_event_capacity)
        return Result<std::unique_ptr<EventPersistenceRuntime>>::failure(
            event_error("SYS_CONFIG_INVALID", Severity::error, "事件持久化运行时配置无效",
                        "event.persist.runtime.create"));
    return Result<std::unique_ptr<EventPersistenceRuntime>>::success(
        std::make_unique<EventPersistenceRuntime>(ConstructionKey{}, std::move(writer),
                                                  std::move(callback), options));
}

EventPersistenceRuntime::EventPersistenceRuntime(ConstructionKey,
                                                 std::unique_ptr<IEventTransactionWriter> writer,
                                                 EventPersistenceCallback callback,
                                                 const EventPersistenceRuntimeOptions options)
    : impl_(std::make_unique<Impl>(std::move(writer), std::move(callback), options))
{
}

EventPersistenceRuntime::~EventPersistenceRuntime()
{
    request_stop();
    static_cast<void>(join(camera::MonotonicTime::max()));
}

Result<void> EventPersistenceRuntime::start()
{
    std::scoped_lock lock{impl_->mutex};
    if (impl_->started)
        return Result<void>::failure(event_error("EVENT_INVALID_TRANSITION", Severity::error,
                                                 "事件持久化运行时不能重复启动",
                                                 "event.persist.runtime.start"));
    impl_->started = true;
    impl_->stopping = false;
    impl_->completed_run = false;
    try
    {
        impl_->worker = std::jthread([this] { impl_->run(); });
    }
    catch (const std::exception&)
    {
        impl_->started = false;
        impl_->completed_run = true;
        return Result<void>::failure(event_error("EVENT_WRITE_FAILED", Severity::critical,
                                                 "无法创建事件持久化工作线程",
                                                 "event.persist.runtime.start", true));
    }
    return Result<void>::success();
}

Result<void> EventPersistenceRuntime::submit(EventPersistenceRequest request)
{
    std::unique_ptr<Impl::Job> job;
    try
    {
        job = std::make_unique<Impl::Job>(Impl::Job{.request = std::move(request)});
    }
    catch (const std::exception&)
    {
        std::scoped_lock lock{impl_->mutex};
        ++impl_->rejected;
        return Result<void>::failure(event_error("EVENT_WRITE_FAILED", Severity::critical,
                                                 "事件持久化任务内存预算不足",
                                                 "event.persist.runtime.submit", true));
    }

    std::scoped_lock lock{impl_->mutex};
    if (!impl_->started || impl_->stopping || impl_->depth == impl_->jobs.size())
    {
        ++impl_->rejected;
        Error error =
            event_error("EVENT_WRITE_FAILED", Severity::critical, "事件持久化队列已满或停止接收",
                        "event.persist.runtime.submit", true);
        error.details.push_back({.key = "reason", .value = "queue-full-or-stopping"});
        error.details.push_back({.key = "capacity", .value = std::to_string(impl_->jobs.size())});
        return Result<void>::failure(std::move(error));
    }
    impl_->jobs[impl_->write_index] = std::move(job);
    impl_->write_index = (impl_->write_index + 1U) % impl_->jobs.size();
    ++impl_->depth;
    ++impl_->submitted;
    impl_->high_watermark = std::max(impl_->high_watermark, impl_->depth);
    impl_->condition.notify_one();
    return Result<void>::success();
}

void EventPersistenceRuntime::request_stop() noexcept
{
    {
        std::scoped_lock lock{impl_->mutex};
        if (!impl_->started)
            return;
        impl_->stopping = true;
    }
    impl_->condition.notify_all();
}

Result<void> EventPersistenceRuntime::join(const camera::MonotonicTime deadline)
{
    std::unique_lock lock{impl_->mutex};
    if (!impl_->started || impl_->completed_run)
    {
        lock.unlock();
        if (impl_->worker.joinable())
            impl_->worker.join();
        return Result<void>::success();
    }
    if (!impl_->completed_condition.wait_until(lock, deadline,
                                               [this] { return impl_->completed_run; }))
        return Result<void>::failure(event_error("SYS_SHUTDOWN_TIMEOUT", Severity::error,
                                                 "事件持久化工作线程未在截止时间内停止",
                                                 "event.persist.runtime.join", true));
    lock.unlock();
    if (impl_->worker.joinable())
        impl_->worker.join();
    return Result<void>::success();
}

EventPersistenceRuntimeSnapshot EventPersistenceRuntime::snapshot() const noexcept
{
    std::scoped_lock lock{impl_->mutex};
    return {.started = impl_->started && !impl_->completed_run,
            .accepting = impl_->started && !impl_->stopping,
            .depth = impl_->depth,
            .capacity = impl_->jobs.size(),
            .high_watermark = impl_->high_watermark,
            .submitted = impl_->submitted,
            .completed = impl_->completed,
            .rejected = impl_->rejected,
            .write_failures = impl_->write_failures,
            .callback_failures = impl_->callback_failures,
            .active_events = impl_->active_events,
            .last_write_bytes = impl_->last_write_bytes,
            .last_write_duration = impl_->last_write_duration,
            .last_write_mib_per_second = impl_->last_write_mib_per_second};
}

} // namespace paperbreak::storage
