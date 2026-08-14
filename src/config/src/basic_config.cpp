#include "paperbreak/config/basic_config.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <regex>
#include <set>
#include <string_view>

namespace paperbreak::config
{
namespace
{

using Json = nlohmann::json;

std::filesystem::path path_from_utf8(const std::string_view value)
{
    std::u8string converted;
    converted.reserve(value.size());
    for (const unsigned char byte : value)
        converted.push_back(static_cast<char8_t>(byte));
    return std::filesystem::path{converted};
}

Error invalid_config(std::string message, std::string operation, std::string pointer,
                     std::string reason)
{
    Error error = make_error("SYS_CONFIG_INVALID", Severity::error, std::move(message), "config",
                             std::move(operation));
    if (!pointer.empty())
    {
        error.details.push_back({"jsonPointer", std::move(pointer)});
    }
    error.details.push_back({"reason", std::move(reason)});
    return error;
}

Result<void> exact_fields(const Json& value, const std::string_view pointer,
                          const std::initializer_list<std::string_view> fields)
{
    if (!value.is_object())
    {
        return Result<void>::failure(invalid_config("配置节点必须是对象", "config.validateSchema",
                                                    std::string{pointer}, "object-required"));
    }
    std::set<std::string, std::less<>> expected;
    for (const auto field : fields)
    {
        expected.emplace(field);
        if (!value.contains(field))
        {
            return Result<void>::failure(invalid_config(
                "配置缺少必需字段", "config.validateSchema",
                std::string{pointer} + "/" + std::string{field}, "required-field-missing"));
        }
    }
    for (const auto& [name, unused] : value.items())
    {
        static_cast<void>(unused);
        if (!expected.contains(name))
        {
            return Result<void>::failure(
                invalid_config("配置包含未声明字段", "config.validateSchema",
                               std::string{pointer} + "/" + name, "unknown-field"));
        }
    }
    return Result<void>::success();
}

Result<std::string> string_field(const Json& object, const std::string_view name,
                                 const std::string_view pointer, const std::size_t maximum,
                                 const bool allow_empty = false)
{
    const auto& value = object.at(name);
    if (!value.is_string())
    {
        return Result<std::string>::failure(
            invalid_config("配置字段必须是字符串", "config.validateType",
                           std::string{pointer} + "/" + std::string{name}, "string-required"));
    }
    auto result = value.get<std::string>();
    if ((!allow_empty && result.empty()) || result.size() > maximum)
    {
        return Result<std::string>::failure(
            invalid_config("配置字符串为空或超过长度上限", "config.validateRange",
                           std::string{pointer} + "/" + std::string{name}, "string-length"));
    }
    return Result<std::string>::success(std::move(result));
}

Result<bool> bool_field(const Json& object, const std::string_view name,
                        const std::string_view pointer)
{
    const auto& value = object.at(name);
    if (!value.is_boolean())
    {
        return Result<bool>::failure(invalid_config("配置字段必须是布尔值", "config.validateType",
                                                    std::string{pointer} + "/" + std::string{name},
                                                    "boolean-required"));
    }
    return Result<bool>::success(value.get<bool>());
}

template <typename T>
Result<T> unsigned_field(const Json& object, const std::string_view name,
                         const std::string_view pointer, const std::uint64_t minimum,
                         const std::uint64_t maximum)
{
    const auto& value = object.at(name);
    if (!value.is_number_unsigned())
    {
        return Result<T>::failure(invalid_config("配置字段必须是无符号整数", "config.validateType",
                                                 std::string{pointer} + "/" + std::string{name},
                                                 "unsigned-integer-required"));
    }
    const auto number = value.get<std::uint64_t>();
    if (number < minimum || number > maximum ||
        number > static_cast<std::uint64_t>((std::numeric_limits<T>::max)()))
    {
        return Result<T>::failure(invalid_config("配置整数超出允许范围", "config.validateRange",
                                                 std::string{pointer} + "/" + std::string{name},
                                                 "number-range"));
    }
    return Result<T>::success(static_cast<T>(number));
}

Result<double> finite_field(const Json& object, const std::string_view name,
                            const std::string_view pointer, const double minimum,
                            const double maximum)
{
    const auto& value = object.at(name);
    if (!value.is_number())
    {
        return Result<double>::failure(
            invalid_config("配置字段必须是数值", "config.validateType",
                           std::string{pointer} + "/" + std::string{name}, "number-required"));
    }
    const double number = value.get<double>();
    if (!std::isfinite(number) || number < minimum || number > maximum)
    {
        return Result<double>::failure(
            invalid_config("配置数值超出允许范围", "config.validateRange",
                           std::string{pointer} + "/" + std::string{name}, "number-range"));
    }
    return Result<double>::success(number);
}

Result<RoiConfig> parse_roi(const Json& value, const std::string_view pointer)
{
    if (auto fields = exact_fields(value, pointer, {"width", "height", "offsetX", "offsetY"});
        !fields)
    {
        return Result<RoiConfig>::failure(fields.error());
    }
    auto width = unsigned_field<std::uint32_t>(value, "width", pointer, 1U, 16384U);
    auto height = unsigned_field<std::uint32_t>(value, "height", pointer, 1U, 16384U);
    auto x = unsigned_field<std::uint32_t>(value, "offsetX", pointer, 0U, 16383U);
    auto y = unsigned_field<std::uint32_t>(value, "offsetY", pointer, 0U, 16383U);
    if (!width)
        return Result<RoiConfig>::failure(width.error());
    if (!height)
        return Result<RoiConfig>::failure(height.error());
    if (!x)
        return Result<RoiConfig>::failure(x.error());
    if (!y)
        return Result<RoiConfig>::failure(y.error());
    if (static_cast<std::uint64_t>(x.value()) + width.value() > 16384U ||
        static_cast<std::uint64_t>(y.value()) + height.value() > 16384U)
    {
        return Result<RoiConfig>::failure(invalid_config("ROI 偏移与尺寸超过安全上限",
                                                         "config.validateDependency",
                                                         std::string{pointer}, "roi-extent"));
    }
    return Result<RoiConfig>::success({.width = width.value(),
                                       .height = height.value(),
                                       .offset_x = x.value(),
                                       .offset_y = y.value()});
}

Result<RoiConfig> parse_algorithm_roi(const Json& value, const std::string_view pointer)
{
    if (auto fields = exact_fields(value, pointer, {"width", "height", "offsetX", "offsetY"});
        !fields)
    {
        return Result<RoiConfig>::failure(fields.error());
    }
    auto width = unsigned_field<std::uint32_t>(value, "width", pointer, 0U, 16384U);
    auto height = unsigned_field<std::uint32_t>(value, "height", pointer, 0U, 16384U);
    auto x = unsigned_field<std::uint32_t>(value, "offsetX", pointer, 0U, 16383U);
    auto y = unsigned_field<std::uint32_t>(value, "offsetY", pointer, 0U, 16383U);
    if (!width)
        return Result<RoiConfig>::failure(width.error());
    if (!height)
        return Result<RoiConfig>::failure(height.error());
    if (!x)
        return Result<RoiConfig>::failure(x.error());
    if (!y)
        return Result<RoiConfig>::failure(y.error());

    const bool full_frame = width.value() == 0U && height.value() == 0U;
    if ((width.value() == 0U) != (height.value() == 0U) ||
        (full_frame && (x.value() != 0U || y.value() != 0U)))
    {
        return Result<RoiConfig>::failure(
            invalid_config("算法全帧 ROI 必须同时使用零宽高和零偏移", "config.validateDependency",
                           std::string{pointer}, "algorithm-full-frame-shape"));
    }
    if (!full_frame && (static_cast<std::uint64_t>(x.value()) + width.value() > 16384U ||
                        static_cast<std::uint64_t>(y.value()) + height.value() > 16384U))
    {
        return Result<RoiConfig>::failure(invalid_config("算法 ROI 偏移与尺寸超过安全上限",
                                                         "config.validateDependency",
                                                         std::string{pointer}, "roi-extent"));
    }
    return Result<RoiConfig>::success({.width = width.value(),
                                       .height = height.value(),
                                       .offset_x = x.value(),
                                       .offset_y = y.value()});
}

Result<std::string> validate_path(const Json& object, const std::string_view name,
                                  const std::string_view pointer,
                                  const std::filesystem::path& config_directory)
{
    auto text = string_field(object, name, pointer, 1024U);
    if (!text)
    {
        return text;
    }
    if (text.value().find('\0') != std::string::npos || text.value().starts_with(R"(\\.\)") ||
        text.value().starts_with(R"(\\?\)"))
    {
        return Result<std::string>::failure(
            invalid_config("配置路径包含禁止的设备路径或空字符", "config.validatePath",
                           std::string{pointer} + "/" + std::string{name}, "unsafe-path"));
    }
    const auto path = path_from_utf8(text.value()).lexically_normal();
    if (path.empty())
    {
        return Result<std::string>::failure(
            invalid_config("配置路径不能为空", "config.validatePath",
                           std::string{pointer} + "/" + std::string{name}, "empty-path"));
    }
    if (path.is_relative())
    {
        const auto first = path.begin();
        if (first != path.end() && *first == "..")
        {
            return Result<std::string>::failure(invalid_config(
                "相对配置路径不能逃逸配置目录", "config.validatePath",
                std::string{pointer} + "/" + std::string{name}, "relative-path-escape"));
        }
        static_cast<void>((config_directory / path).lexically_normal());
    }
    return text;
}

bool valid_identifier(const std::string& value)
{
    static const std::regex pattern{R"([A-Za-z0-9][A-Za-z0-9._-]{0,63})"};
    return std::regex_match(value, pattern);
}

bool valid_timestamp(const std::string& value)
{
    static const std::regex pattern{R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z)"};
    return std::regex_match(value, pattern);
}

bool contains_sensitive_key(const Json& value)
{
    static const std::regex sensitive{R"((password|token|secret|private[-_]?key))",
                                      std::regex_constants::icase};
    if (value.is_object())
    {
        for (const auto& [key, child] : value.items())
        {
            if (std::regex_search(key, sensitive) || contains_sensitive_key(child))
            {
                return true;
            }
        }
    }
    else if (value.is_array())
    {
        return std::ranges::any_of(value,
                                   [](const Json& child) { return contains_sensitive_key(child); });
    }
    return false;
}

template <typename Enum>
Result<Enum> enum_value(const std::string& value, const std::string& pointer,
                        const std::initializer_list<std::pair<std::string_view, Enum>> choices)
{
    for (const auto& [name, candidate] : choices)
    {
        if (value == name)
        {
            return Result<Enum>::success(candidate);
        }
    }
    return Result<Enum>::failure(
        invalid_config("配置枚举值不受支持", "config.validateEnum", pointer, "unsupported-enum"));
}

std::string_view pixel_format_name(const PixelFormat value)
{
    switch (value)
    {
    case PixelFormat::mono8:
        return "Mono8";
    case PixelFormat::mono10:
        return "Mono10";
    case PixelFormat::mono12:
        return "Mono12";
    case PixelFormat::bayer_rg8:
        return "BayerRG8";
    }
    return "Mono8";
}

std::string_view trigger_mode_name(const TriggerMode value)
{
    switch (value)
    {
    case TriggerMode::continuous:
        return "Continuous";
    case TriggerMode::hardware:
        return "Hardware";
    case TriggerMode::software:
        return "Software";
    }
    return "Continuous";
}

std::string_view exposure_auto_mode_name(const ExposureAutoMode value) noexcept
{
    switch (value)
    {
    case ExposureAutoMode::off:
        return "Off";
    case ExposureAutoMode::once:
        return "Once";
    case ExposureAutoMode::continuous:
        return "Continuous";
    }
    return "Off";
}

std::string_view alarm_active_level_name(const AlarmActiveLevel value) noexcept
{
    return value == AlarmActiveLevel::high ? "High" : "Low";
}

std::string_view log_level_name(const LogLevel value)
{
    switch (value)
    {
    case LogLevel::trace:
        return "trace";
    case LogLevel::debug:
        return "debug";
    case LogLevel::info:
        return "info";
    case LogLevel::warning:
        return "warning";
    case LogLevel::error:
        return "error";
    case LogLevel::critical:
        return "critical";
    }
    return "info";
}

std::string_view downsample_mode_name(const AlgorithmDownsampleMode value) noexcept
{
    switch (value)
    {
    case AlgorithmDownsampleMode::disabled:
        return "disabled";
    case AlgorithmDownsampleMode::half:
        return "half";
    case AlgorithmDownsampleMode::quarter:
        return "quarter";
    }
    return "disabled";
}

Json roi_json(const RoiConfig& roi)
{
    return {{"width", roi.width},
            {"height", roi.height},
            {"offsetX", roi.offset_x},
            {"offsetY", roi.offset_y}};
}

Result<EdgeConfig> parse_metadata(const Json& root)
{
    auto schema = unsigned_field<std::uint32_t>(root, "configSchemaVersion", "", 1U,
                                                (std::numeric_limits<std::uint32_t>::max)());
    if (!schema)
        return Result<EdgeConfig>::failure(schema.error());
    constexpr std::array migratable_schema_versions{2U, 3U, 4U, 5U, 6U};
    if (schema.value() != config_schema_version &&
        std::find(migratable_schema_versions.begin(), migratable_schema_versions.end(),
                  schema.value()) == migratable_schema_versions.end())
    {
        Error error =
            make_error("SYS_CONFIG_SCHEMA_UNSUPPORTED", Severity::error, "不支持该配置 schema 版本",
                       "config", "config.validateSchemaVersion");
        error.details.push_back({"received", std::to_string(schema.value())});
        error.details.push_back({"supported", "2,3,4,5,6,7"});
        return Result<EdgeConfig>::failure(std::move(error));
    }
    auto revision = unsigned_field<std::uint64_t>(root, "configRevision", "", 1U,
                                                  (std::numeric_limits<std::uint64_t>::max)());
    auto modified = string_field(root, "modifiedAt", "", 32U);
    if (!revision)
        return Result<EdgeConfig>::failure(revision.error());
    if (!modified)
        return Result<EdgeConfig>::failure(modified.error());
    if (!valid_timestamp(modified.value()))
    {
        return Result<EdgeConfig>::failure(invalid_config("modifiedAt 必须是 UTC RFC 3339 毫秒时间",
                                                          "config.validateTimestamp", "/modifiedAt",
                                                          "invalid-timestamp"));
    }

    EdgeConfig result;
    result.config_schema_version = config_schema_version;
    result.config_revision = revision.value();
    result.modified_at = std::move(modified).value();
    return Result<EdgeConfig>::success(std::move(result));
}

Result<EdgeConfig> parse_system(const Json& root, EdgeConfig result)
{

    const Json& system = root.at("system");
    if (auto fields =
            exact_fields(system, "/system", {"machineId", "productionLineId", "shutdownTimeoutMs"});
        !fields)
        return Result<EdgeConfig>::failure(fields.error());
    auto machine = string_field(system, "machineId", "/system", 64U);
    auto line = string_field(system, "productionLineId", "/system", 64U);
    auto shutdown =
        unsigned_field<std::uint32_t>(system, "shutdownTimeoutMs", "/system", 1000U, 300000U);
    if (!machine)
        return Result<EdgeConfig>::failure(machine.error());
    if (!line)
        return Result<EdgeConfig>::failure(line.error());
    if (!shutdown)
        return Result<EdgeConfig>::failure(shutdown.error());
    if (!valid_identifier(machine.value()) || !valid_identifier(line.value()))
    {
        return Result<EdgeConfig>::failure(
            invalid_config("设备和生产线 ID 只能包含 ASCII 字母、数字、点、下划线和连字符",
                           "config.validateIdentifier", "/system", "invalid-identifier"));
    }
    result.system = {.machine_id = std::move(machine).value(),
                     .production_line_id = std::move(line).value(),
                     .shutdown_timeout_ms = shutdown.value()};

    return Result<EdgeConfig>::success(std::move(result));
}

Result<EdgeConfig> parse_cameras(const Json& root, EdgeConfig result)
{
    const Json& cameras = root.at("cameras");
    const auto source_schema = root.at("configSchemaVersion").get<std::uint32_t>();
    const bool migrating_v2 = source_schema == 2U;
    const bool migrating_before_v4 = source_schema < 4U;
    if (!cameras.is_array() || cameras.size() > maximum_camera_count)
    {
        return Result<EdgeConfig>::failure(invalid_config(
            "cameras 必须是最多六项的数组", "config.validateSchema", "/cameras", "camera-count"));
    }
    std::set<std::string> camera_ids;
    std::set<std::string> enabled_serials;
    for (std::size_t index = 0; index < cameras.size(); ++index)
    {
        Json camera = cameras.at(index);
        const std::string pointer = "/cameras/" + std::to_string(index);
        if (camera.is_object())
        {
            if (!camera.contains("reverseX"))
                camera["reverseX"] = false;
            if (!camera.contains("reverseY"))
                camera["reverseY"] = false;
            if (migrating_before_v4 && !camera.contains("autoExposure"))
                camera["autoExposure"] = "Off";
            if (migrating_v2 && !camera.contains("lineIo"))
                camera["lineIo"] = {{"alarmInputEnabled", false},   {"alarmActiveLevel", "High"},
                                    {"strobeOutputEnabled", false}, {"strobeDurationUs", 0U},
                                    {"strobePreDelayUs", 0U},       {"strobePostDelayUs", 0U}};
        }
        if (auto fields =
                exact_fields(camera, pointer,
                             {"id", "enabled", "serialNumber", "location", "exposureUs",
                              "autoExposure", "gainDb", "frameRate", "roi", "pixelFormat",
                              "triggerMode", "triggerSource", "triggerDelayUs", "packetSizeBytes",
                              "interPacketDelayNs", "reverseX", "reverseY", "lineIo"});
            !fields)
            return Result<EdgeConfig>::failure(fields.error());
        auto id = string_field(camera, "id", pointer, 5U);
        auto enabled = bool_field(camera, "enabled", pointer);
        auto serial = string_field(camera, "serialNumber", pointer, 128U, true);
        auto location = string_field(camera, "location", pointer, 128U);
        auto exposure = finite_field(camera, "exposureUs", pointer, 1.0, 10000000.0);
        auto exposure_auto_text = string_field(camera, "autoExposure", pointer, 16U);
        auto gain = finite_field(camera, "gainDb", pointer, -24.0, 48.0);
        auto rate = finite_field(camera, "frameRate", pointer, 0.1, 1000.0);
        auto roi = parse_roi(camera.at("roi"), pointer + "/roi");
        auto reverse_x = bool_field(camera, "reverseX", pointer);
        auto reverse_y = bool_field(camera, "reverseY", pointer);
        auto pixel_text = string_field(camera, "pixelFormat", pointer, 32U);
        auto mode_text = string_field(camera, "triggerMode", pointer, 32U);
        auto trigger_source = string_field(camera, "triggerSource", pointer, 64U, true);
        auto delay =
            unsigned_field<std::uint32_t>(camera, "triggerDelayUs", pointer, 0U, 60000000U);
        auto packet =
            unsigned_field<std::uint32_t>(camera, "packetSizeBytes", pointer, 576U, 9000U);
        auto inter_packet =
            unsigned_field<std::uint32_t>(camera, "interPacketDelayNs", pointer, 0U, 1000000000U);
        const Json& line_io = camera.at("lineIo");
        if (auto fields =
                exact_fields(line_io, pointer + "/lineIo",
                             {"alarmInputEnabled", "alarmActiveLevel", "strobeOutputEnabled",
                              "strobeDurationUs", "strobePreDelayUs", "strobePostDelayUs"});
            !fields)
            return Result<EdgeConfig>::failure(fields.error());
        auto alarm_input_enabled = bool_field(line_io, "alarmInputEnabled", pointer + "/lineIo");
        auto active_level_text = string_field(line_io, "alarmActiveLevel", pointer + "/lineIo", 8U);
        auto strobe_output_enabled =
            bool_field(line_io, "strobeOutputEnabled", pointer + "/lineIo");
        auto strobe_duration = unsigned_field<std::uint32_t>(line_io, "strobeDurationUs",
                                                             pointer + "/lineIo", 0U, 60000000U);
        auto strobe_pre_delay = unsigned_field<std::uint32_t>(line_io, "strobePreDelayUs",
                                                              pointer + "/lineIo", 0U, 60000000U);
        auto strobe_post_delay = unsigned_field<std::uint32_t>(line_io, "strobePostDelayUs",
                                                               pointer + "/lineIo", 0U, 60000000U);
        if (!id)
            return Result<EdgeConfig>::failure(id.error());
        if (!enabled)
            return Result<EdgeConfig>::failure(enabled.error());
        if (!serial)
            return Result<EdgeConfig>::failure(serial.error());
        if (!location)
            return Result<EdgeConfig>::failure(location.error());
        if (!exposure)
            return Result<EdgeConfig>::failure(exposure.error());
        if (!exposure_auto_text)
            return Result<EdgeConfig>::failure(exposure_auto_text.error());
        if (!gain)
            return Result<EdgeConfig>::failure(gain.error());
        if (!rate)
            return Result<EdgeConfig>::failure(rate.error());
        if (!roi)
            return Result<EdgeConfig>::failure(roi.error());
        if (!reverse_x)
            return Result<EdgeConfig>::failure(reverse_x.error());
        if (!reverse_y)
            return Result<EdgeConfig>::failure(reverse_y.error());
        if (!pixel_text)
            return Result<EdgeConfig>::failure(pixel_text.error());
        if (!mode_text)
            return Result<EdgeConfig>::failure(mode_text.error());
        if (!trigger_source)
            return Result<EdgeConfig>::failure(trigger_source.error());
        if (!delay)
            return Result<EdgeConfig>::failure(delay.error());
        if (!packet)
            return Result<EdgeConfig>::failure(packet.error());
        if (!inter_packet)
            return Result<EdgeConfig>::failure(inter_packet.error());
        if (!alarm_input_enabled)
            return Result<EdgeConfig>::failure(alarm_input_enabled.error());
        if (!active_level_text)
            return Result<EdgeConfig>::failure(active_level_text.error());
        if (!strobe_output_enabled)
            return Result<EdgeConfig>::failure(strobe_output_enabled.error());
        if (!strobe_duration)
            return Result<EdgeConfig>::failure(strobe_duration.error());
        if (!strobe_pre_delay)
            return Result<EdgeConfig>::failure(strobe_pre_delay.error());
        if (!strobe_post_delay)
            return Result<EdgeConfig>::failure(strobe_post_delay.error());
        if (!is_canonical_camera_id(id.value()) || !camera_ids.emplace(id.value()).second)
        {
            return Result<EdgeConfig>::failure(
                invalid_config("相机 ID 必须是唯一的 CAM01 至 CAM06", "config.validateDependency",
                               pointer + "/id", "invalid-or-duplicate-camera-id"));
        }
        if (enabled.value() &&
            (serial.value().empty() || !enabled_serials.emplace(serial.value()).second))
        {
            return Result<EdgeConfig>::failure(
                invalid_config("启用相机必须配置唯一序列号", "config.validateDependency",
                               pointer + "/serialNumber", "missing-or-duplicate-serial"));
        }
        auto pixel = enum_value<PixelFormat>(pixel_text.value(), pointer + "/pixelFormat",
                                             {{"Mono8", PixelFormat::mono8},
                                              {"Mono10", PixelFormat::mono10},
                                              {"Mono12", PixelFormat::mono12},
                                              {"BayerRG8", PixelFormat::bayer_rg8}});
        auto exposure_auto =
            enum_value<ExposureAutoMode>(exposure_auto_text.value(), pointer + "/autoExposure",
                                         {{"Off", ExposureAutoMode::off},
                                          {"Once", ExposureAutoMode::once},
                                          {"Continuous", ExposureAutoMode::continuous}});
        auto mode = enum_value<TriggerMode>(mode_text.value(), pointer + "/triggerMode",
                                            {{"Continuous", TriggerMode::continuous},
                                             {"Hardware", TriggerMode::hardware},
                                             {"Software", TriggerMode::software}});
        if (!pixel)
            return Result<EdgeConfig>::failure(pixel.error());
        if (!exposure_auto)
            return Result<EdgeConfig>::failure(exposure_auto.error());
        if (!mode)
            return Result<EdgeConfig>::failure(mode.error());
        auto active_level = enum_value<AlarmActiveLevel>(
            active_level_text.value(), pointer + "/lineIo/alarmActiveLevel",
            {{"High", AlarmActiveLevel::high}, {"Low", AlarmActiveLevel::low}});
        if (!active_level)
            return Result<EdgeConfig>::failure(active_level.error());
        if (mode.value() == TriggerMode::hardware && trigger_source.value().empty())
        {
            return Result<EdgeConfig>::failure(
                invalid_config("硬件触发必须指定 triggerSource", "config.validateDependency",
                               pointer + "/triggerSource", "trigger-source-required"));
        }
        if (!enabled.value() && (alarm_input_enabled.value() || strobe_output_enabled.value()))
        {
            return Result<EdgeConfig>::failure(
                invalid_config("未启用相机不能启用线路 I/O", "config.validateDependency",
                               pointer + "/lineIo", "line-io-requires-enabled-camera"));
        }
        if (strobe_output_enabled.value() && strobe_duration.value() == 0U)
        {
            return Result<EdgeConfig>::failure(invalid_config(
                "启用频闪输出时 strobeDurationUs 必须大于 0", "config.validateDependency",
                pointer + "/lineIo/strobeDurationUs", "strobe-duration-required"));
        }
        result.cameras.push_back(
            {.id = std::move(id).value(),
             .enabled = enabled.value(),
             .serial_number = std::move(serial).value(),
             .location = std::move(location).value(),
             .exposure_us = exposure.value(),
             .exposure_auto_mode = exposure_auto.value(),
             .gain_db = gain.value(),
             .frame_rate = rate.value(),
             .roi = roi.value(),
             .reverse_x = reverse_x.value(),
             .reverse_y = reverse_y.value(),
             .pixel_format = pixel.value(),
             .trigger_mode = mode.value(),
             .trigger_source = std::move(trigger_source).value(),
             .trigger_delay_us = delay.value(),
             .packet_size_bytes = packet.value(),
             .inter_packet_delay_ns = inter_packet.value(),
             .line_io = {.alarm_input_enabled = alarm_input_enabled.value(),
                         .alarm_active_level = active_level.value(),
                         .strobe_output_enabled = strobe_output_enabled.value(),
                         .strobe_duration_us = strobe_duration.value(),
                         .strobe_pre_delay_us = strobe_pre_delay.value(),
                         .strobe_post_delay_us = strobe_post_delay.value()}});
    }

    return Result<EdgeConfig>::success(std::move(result));
}

Result<EdgeConfig> parse_acquisition(const Json& root, EdgeConfig result)
{
    Json acquisition = root.at("acquisition");
    if (acquisition.is_object())
    {
        if (!acquisition.contains("autoStart"))
            acquisition["autoStart"] = false;
        if (!acquisition.contains("startupRetryIntervalMs"))
            acquisition["startupRetryIntervalMs"] = 1000U;
        if (!acquisition.contains("startupRetryCount"))
            acquisition["startupRetryCount"] = 3U;
    }
    if (auto fields = exact_fields(acquisition, "/acquisition",
                                   {"autoStart", "startupRetryIntervalMs", "startupRetryCount",
                                    "framePoolCapacity", "queueCapacity", "receiveTimeoutMs",
                                    "threadPriority"});
        !fields)
        return Result<EdgeConfig>::failure(fields.error());
    auto auto_start = bool_field(acquisition, "autoStart", "/acquisition");
    auto retry_interval = unsigned_field<std::uint32_t>(acquisition, "startupRetryIntervalMs",
                                                        "/acquisition", 1U, 60000U);
    auto retry_count =
        unsigned_field<std::uint32_t>(acquisition, "startupRetryCount", "/acquisition", 0U, 10U);
    auto frame_pool =
        unsigned_field<std::uint32_t>(acquisition, "framePoolCapacity", "/acquisition", 1U, 4096U);
    auto queue =
        unsigned_field<std::uint32_t>(acquisition, "queueCapacity", "/acquisition", 1U, 4096U);
    auto timeout =
        unsigned_field<std::uint32_t>(acquisition, "receiveTimeoutMs", "/acquisition", 1U, 60000U);
    auto priority = string_field(acquisition, "threadPriority", "/acquisition", 16U);
    if (!auto_start)
        return Result<EdgeConfig>::failure(auto_start.error());
    if (!retry_interval)
        return Result<EdgeConfig>::failure(retry_interval.error());
    if (!retry_count)
        return Result<EdgeConfig>::failure(retry_count.error());
    if (!frame_pool)
        return Result<EdgeConfig>::failure(frame_pool.error());
    if (!queue)
        return Result<EdgeConfig>::failure(queue.error());
    if (!timeout)
        return Result<EdgeConfig>::failure(timeout.error());
    if (!priority)
        return Result<EdgeConfig>::failure(priority.error());
    if (priority.value() != "low" && priority.value() != "normal" && priority.value() != "high")
        return Result<EdgeConfig>::failure(
            invalid_config("threadPriority 必须为 low、normal 或 high", "config.validateEnum",
                           "/acquisition/threadPriority", "unsupported-enum"));
    if (frame_pool.value() < queue.value())
        return Result<EdgeConfig>::failure(
            invalid_config("framePoolCapacity 不能小于 queueCapacity", "config.validateDependency",
                           "/acquisition", "pool-smaller-than-queue"));
    result.acquisition = {.auto_start = auto_start.value(),
                          .startup_retry_interval_ms = retry_interval.value(),
                          .startup_retry_count = retry_count.value(),
                          .frame_pool_capacity = frame_pool.value(),
                          .queue_capacity = queue.value(),
                          .receive_timeout_ms = timeout.value(),
                          .thread_priority = std::move(priority).value()};

    return Result<EdgeConfig>::success(std::move(result));
}

Result<EdgeConfig> parse_preview(const Json& root, EdgeConfig result)
{
    const Json& preview = root.at("preview");
    if (auto fields = exact_fields(preview, "/preview",
                                   {"enabled", "fps", "maxWidth", "maxHeight", "jpegQuality"});
        !fields)
        return Result<EdgeConfig>::failure(fields.error());
    auto preview_enabled = bool_field(preview, "enabled", "/preview");
    auto preview_fps = finite_field(preview, "fps", "/preview", 0.1, 30.0);
    auto preview_width = unsigned_field<std::uint32_t>(preview, "maxWidth", "/preview", 64U, 8192U);
    auto preview_height =
        unsigned_field<std::uint32_t>(preview, "maxHeight", "/preview", 64U, 8192U);
    auto jpeg = unsigned_field<std::uint32_t>(preview, "jpegQuality", "/preview", 1U, 100U);
    if (!preview_enabled)
        return Result<EdgeConfig>::failure(preview_enabled.error());
    if (!preview_fps)
        return Result<EdgeConfig>::failure(preview_fps.error());
    if (!preview_width)
        return Result<EdgeConfig>::failure(preview_width.error());
    if (!preview_height)
        return Result<EdgeConfig>::failure(preview_height.error());
    if (!jpeg)
        return Result<EdgeConfig>::failure(jpeg.error());
    result.preview = {.enabled = preview_enabled.value(),
                      .fps = preview_fps.value(),
                      .max_width = preview_width.value(),
                      .max_height = preview_height.value(),
                      .jpeg_quality = jpeg.value()};

    return Result<EdgeConfig>::success(std::move(result));
}

Result<EdgeConfig> parse_algorithm(const Json& root, EdgeConfig result)
{
    const Json& algorithm = root.at("algorithm");
    const auto source_schema = root.at("configSchemaVersion").get<std::uint32_t>();
    if (source_schema >= 6U)
    {
        if (auto fields = exact_fields(
                algorithm, "/algorithm",
                {"enabled", "type", "roi", "downsampleMode", "processingFps", "candidateThreshold",
                 "confirmationThreshold", "confirmationDurationMs", "cooldownMs", "rearmDurationMs",
                 "modelReference", "modelVersion", "device", "debugOverlay"});
            !fields)
            return Result<EdgeConfig>::failure(fields.error());
    }
    else if (source_schema == 5U)
    {
        if (auto fields = exact_fields(algorithm, "/algorithm",
                                       {"enabled", "type", "roi", "downsampleMode", "processingFps",
                                        "candidateThreshold", "confirmationThreshold",
                                        "confirmationDurationMs", "cooldownMs", "modelReference",
                                        "modelVersion", "device", "debugOverlay"});
            !fields)
            return Result<EdgeConfig>::failure(fields.error());
    }
    else if (auto fields =
                 exact_fields(algorithm, "/algorithm",
                              {"enabled", "type", "roi", "candidateThreshold",
                               "confirmationThreshold", "consecutiveFrames", "cooldownMs",
                               "modelReference", "modelVersion", "device", "debugOverlay"});
             !fields)
    {
        return Result<EdgeConfig>::failure(fields.error());
    }
    auto algorithm_enabled = bool_field(algorithm, "enabled", "/algorithm");
    auto algorithm_type = string_field(algorithm, "type", "/algorithm", 64U);
    auto algorithm_roi = parse_algorithm_roi(algorithm.at("roi"), "/algorithm/roi");
    auto candidate_threshold =
        finite_field(algorithm, "candidateThreshold", "/algorithm", 0.0, 1.0);
    auto confirmation_threshold =
        finite_field(algorithm, "confirmationThreshold", "/algorithm", 0.0, 1.0);
    AlgorithmDownsampleMode downsample_mode = AlgorithmDownsampleMode::disabled;
    AlgorithmProcessingFps processing_fps = AlgorithmProcessingFps::fps60;
    std::uint32_t confirmation_duration_ms{};
    if (source_schema >= 5U)
    {
        auto mode_text = string_field(algorithm, "downsampleMode", "/algorithm", 16U);
        if (!mode_text)
            return Result<EdgeConfig>::failure(mode_text.error());
        auto mode =
            enum_value<AlgorithmDownsampleMode>(mode_text.value(), "/algorithm/downsampleMode",
                                                {{"disabled", AlgorithmDownsampleMode::disabled},
                                                 {"half", AlgorithmDownsampleMode::half},
                                                 {"quarter", AlgorithmDownsampleMode::quarter}});
        if (!mode)
            return Result<EdgeConfig>::failure(mode.error());
        downsample_mode = mode.value();

        auto fps =
            unsigned_field<std::uint32_t>(algorithm, "processingFps", "/algorithm", 15U, 60U);
        if (!fps)
            return Result<EdgeConfig>::failure(fps.error());
        switch (fps.value())
        {
        case 15U:
            processing_fps = AlgorithmProcessingFps::fps15;
            break;
        case 30U:
            processing_fps = AlgorithmProcessingFps::fps30;
            break;
        case 60U:
            processing_fps = AlgorithmProcessingFps::fps60;
            break;
        default:
            return Result<EdgeConfig>::failure(
                invalid_config("processingFps 必须为 15、30 或 60", "config.validateEnum",
                               "/algorithm/processingFps", "unsupported-enum"));
        }
        auto duration = unsigned_field<std::uint32_t>(algorithm, "confirmationDurationMs",
                                                      "/algorithm", 10U, 60000U);
        if (!duration)
            return Result<EdgeConfig>::failure(duration.error());
        confirmation_duration_ms = duration.value();
    }
    else
    {
        auto consecutive =
            unsigned_field<std::uint32_t>(algorithm, "consecutiveFrames", "/algorithm", 1U, 1000U);
        if (!consecutive)
            return Result<EdgeConfig>::failure(consecutive.error());
        const auto raw_ms = (static_cast<std::uint64_t>(consecutive.value()) * 1000U + 59U) / 60U;
        confirmation_duration_ms = static_cast<std::uint32_t>(((raw_ms + 9U) / 10U) * 10U);
    }
    auto cooldown =
        unsigned_field<std::uint32_t>(algorithm, "cooldownMs", "/algorithm", 0U, 3600000U);
    std::uint32_t rearm_duration_ms = 500U;
    if (source_schema >= 6U)
    {
        auto rearm =
            unsigned_field<std::uint32_t>(algorithm, "rearmDurationMs", "/algorithm", 0U, 3600000U);
        if (!rearm)
            return Result<EdgeConfig>::failure(rearm.error());
        rearm_duration_ms = rearm.value();
    }
    auto model_reference = string_field(algorithm, "modelReference", "/algorithm", 512U, true);
    auto model_version = string_field(algorithm, "modelVersion", "/algorithm", 128U, true);
    auto device = string_field(algorithm, "device", "/algorithm", 64U);
    auto overlay = bool_field(algorithm, "debugOverlay", "/algorithm");
    if (!algorithm_enabled)
        return Result<EdgeConfig>::failure(algorithm_enabled.error());
    if (!algorithm_type)
        return Result<EdgeConfig>::failure(algorithm_type.error());
    if (!algorithm_roi)
        return Result<EdgeConfig>::failure(algorithm_roi.error());
    if (!candidate_threshold)
        return Result<EdgeConfig>::failure(candidate_threshold.error());
    if (!confirmation_threshold)
        return Result<EdgeConfig>::failure(confirmation_threshold.error());
    if (!cooldown)
        return Result<EdgeConfig>::failure(cooldown.error());
    if (!model_reference)
        return Result<EdgeConfig>::failure(model_reference.error());
    if (!model_version)
        return Result<EdgeConfig>::failure(model_version.error());
    if (!device)
        return Result<EdgeConfig>::failure(device.error());
    if (!overlay)
        return Result<EdgeConfig>::failure(overlay.error());
    if (confirmation_threshold.value() < candidate_threshold.value())
        return Result<EdgeConfig>::failure(invalid_config("确认阈值不能低于候选阈值",
                                                          "config.validateDependency", "/algorithm",
                                                          "confirmation-below-candidate"));
    result.algorithm = {.enabled = algorithm_enabled.value(),
                        .type = std::move(algorithm_type).value(),
                        .roi = algorithm_roi.value(),
                        .downsample_mode = downsample_mode,
                        .processing_fps = processing_fps,
                        .candidate_threshold = candidate_threshold.value(),
                        .confirmation_threshold = confirmation_threshold.value(),
                        .confirmation_duration_ms = confirmation_duration_ms,
                        .cooldown_ms = cooldown.value(),
                        .rearm_duration_ms = rearm_duration_ms,
                        .model_reference = std::move(model_reference).value(),
                        .model_version = std::move(model_version).value(),
                        .device = std::move(device).value(),
                        .debug_overlay = overlay.value()};

    const bool full_frame = result.algorithm.roi.width == 0U;
    if (!full_frame)
    {
        for (const auto& camera : result.cameras)
        {
            if (!camera.enabled)
                continue;
            const auto& roi = result.algorithm.roi;
            const bool fits = roi.offset_x <= camera.roi.width &&
                              roi.offset_y <= camera.roi.height &&
                              roi.width <= camera.roi.width - roi.offset_x &&
                              roi.height <= camera.roi.height - roi.offset_y;
            if (!fits)
                return Result<EdgeConfig>::failure(
                    invalid_config("算法 ROI 超出启用相机 " + camera.id + " 的采集画面",
                                   "config.validateDependency", "/algorithm/roi",
                                   "roi-out-of-camera:" + camera.id));
        }
    }

    return Result<EdgeConfig>::success(std::move(result));
}

Result<EdgeConfig> parse_event(const Json& root, EdgeConfig result)
{
    const Json& event = root.at("event");
    if (auto fields = exact_fields(event, "/event",
                                   {"preEventSeconds", "postEventSeconds", "maxEventSeconds",
                                    "mergeGapSeconds", "keyFrameCount", "saveRaw",
                                    "generatePreviewVideo", "uploadPolicy", "retentionDays"});
        !fields)
        return Result<EdgeConfig>::failure(fields.error());
    auto pre = unsigned_field<std::uint32_t>(event, "preEventSeconds", "/event", 0U, 600U);
    auto post = unsigned_field<std::uint32_t>(event, "postEventSeconds", "/event", 0U, 600U);
    auto maximum = unsigned_field<std::uint32_t>(event, "maxEventSeconds", "/event", 1U, 3600U);
    auto merge = unsigned_field<std::uint32_t>(event, "mergeGapSeconds", "/event", 0U, 3600U);
    auto key_frames = unsigned_field<std::uint32_t>(event, "keyFrameCount", "/event", 1U, 32U);
    auto save_raw = bool_field(event, "saveRaw", "/event");
    auto preview_video = bool_field(event, "generatePreviewVideo", "/event");
    auto upload_policy = string_field(event, "uploadPolicy", "/event", 32U);
    auto retention = unsigned_field<std::uint32_t>(event, "retentionDays", "/event", 1U, 3650U);
    if (!pre)
        return Result<EdgeConfig>::failure(pre.error());
    if (!post)
        return Result<EdgeConfig>::failure(post.error());
    if (!maximum)
        return Result<EdgeConfig>::failure(maximum.error());
    if (!merge)
        return Result<EdgeConfig>::failure(merge.error());
    if (!key_frames)
        return Result<EdgeConfig>::failure(key_frames.error());
    if (!save_raw)
        return Result<EdgeConfig>::failure(save_raw.error());
    if (!preview_video)
        return Result<EdgeConfig>::failure(preview_video.error());
    if (!upload_policy)
        return Result<EdgeConfig>::failure(upload_policy.error());
    if (!retention)
        return Result<EdgeConfig>::failure(retention.error());
    if (pre.value() + post.value() > maximum.value() || merge.value() > maximum.value())
        return Result<EdgeConfig>::failure(invalid_config("事件前后窗口或合并间隔超过最大事件时长",
                                                          "config.validateDependency", "/event",
                                                          "event-window"));
    if (static_cast<std::uint64_t>(result.algorithm.confirmation_duration_ms) >
        static_cast<std::uint64_t>(maximum.value()) * 1000U)
        return Result<EdgeConfig>::failure(invalid_config(
            "算法确认持续时间不能超过候选超时", "config.validateDependency",
            "/algorithm/confirmationDurationMs", "confirmation-exceeds-candidate-timeout"));
    if (upload_policy.value() != "never" && upload_policy.value() != "confirmed" &&
        upload_policy.value() != "all")
        return Result<EdgeConfig>::failure(
            invalid_config("uploadPolicy 必须为 never、confirmed 或 all", "config.validateEnum",
                           "/event/uploadPolicy", "unsupported-enum"));
    result.event = {.pre_event_seconds = pre.value(),
                    .post_event_seconds = post.value(),
                    .max_event_seconds = maximum.value(),
                    .merge_gap_seconds = merge.value(),
                    .key_frame_count = key_frames.value(),
                    .save_raw = save_raw.value(),
                    .generate_preview_video = preview_video.value(),
                    .upload_policy = std::move(upload_policy).value(),
                    .retention_days = retention.value()};

    return Result<EdgeConfig>::success(std::move(result));
}

Result<EdgeConfig> parse_storage(const Json& root, const std::filesystem::path& config_directory,
                                 EdgeConfig result)
{
    const Json& storage = root.at("storage");
    if (auto fields = exact_fields(
            storage, "/storage",
            {"eventRoot", "cacheRoot", "rollingCacheEnabled", "maximumCacheStorageGiB",
             "rollingCacheWriteLimitMiBps", "rollingCacheIoTimeoutMs", "warningFreeSpaceGiB",
             "criticalFreeSpaceGiB", "stopFreeSpaceGiB", "maximumEventStorageGiB"});
        !fields)
        return Result<EdgeConfig>::failure(fields.error());
    auto event_root = validate_path(storage, "eventRoot", "/storage", config_directory);
    auto cache_root = validate_path(storage, "cacheRoot", "/storage", config_directory);
    auto rolling_enabled = bool_field(storage, "rollingCacheEnabled", "/storage");
    auto maximum_cache =
        unsigned_field<std::uint32_t>(storage, "maximumCacheStorageGiB", "/storage", 1U, 1000000U);
    auto write_limit = unsigned_field<std::uint32_t>(storage, "rollingCacheWriteLimitMiBps",
                                                     "/storage", 1U, 1000000U);
    auto io_timeout = unsigned_field<std::uint32_t>(storage, "rollingCacheIoTimeoutMs", "/storage",
                                                    100U, 600000U);
    auto warning =
        unsigned_field<std::uint32_t>(storage, "warningFreeSpaceGiB", "/storage", 1U, 1000000U);
    auto critical =
        unsigned_field<std::uint32_t>(storage, "criticalFreeSpaceGiB", "/storage", 1U, 1000000U);
    auto stop =
        unsigned_field<std::uint32_t>(storage, "stopFreeSpaceGiB", "/storage", 1U, 1000000U);
    auto maximum_storage =
        unsigned_field<std::uint32_t>(storage, "maximumEventStorageGiB", "/storage", 1U, 1000000U);
    if (!event_root)
        return Result<EdgeConfig>::failure(event_root.error());
    if (!cache_root)
        return Result<EdgeConfig>::failure(cache_root.error());
    if (!rolling_enabled)
        return Result<EdgeConfig>::failure(rolling_enabled.error());
    if (!maximum_cache)
        return Result<EdgeConfig>::failure(maximum_cache.error());
    if (!write_limit)
        return Result<EdgeConfig>::failure(write_limit.error());
    if (!io_timeout)
        return Result<EdgeConfig>::failure(io_timeout.error());
    if (!warning)
        return Result<EdgeConfig>::failure(warning.error());
    if (!critical)
        return Result<EdgeConfig>::failure(critical.error());
    if (!stop)
        return Result<EdgeConfig>::failure(stop.error());
    if (!maximum_storage)
        return Result<EdgeConfig>::failure(maximum_storage.error());
    if (!(warning.value() > critical.value() && critical.value() > stop.value()))
        return Result<EdgeConfig>::failure(
            invalid_config("存储水位必须满足 warning > critical > stop",
                           "config.validateDependency", "/storage", "storage-watermarks"));
    if (path_from_utf8(event_root.value()).lexically_normal() ==
        path_from_utf8(cache_root.value()).lexically_normal())
        return Result<EdgeConfig>::failure(invalid_config("事件目录和缓存目录不能相同",
                                                          "config.validateDependency", "/storage",
                                                          "storage-path-collision"));
    result.storage = {.event_root = std::move(event_root).value(),
                      .cache_root = std::move(cache_root).value(),
                      .rolling_cache_enabled = rolling_enabled.value(),
                      .maximum_cache_storage_gib = maximum_cache.value(),
                      .rolling_cache_write_limit_mibps = write_limit.value(),
                      .rolling_cache_io_timeout_ms = io_timeout.value(),
                      .warning_free_space_gib = warning.value(),
                      .critical_free_space_gib = critical.value(),
                      .stop_free_space_gib = stop.value(),
                      .maximum_event_storage_gib = maximum_storage.value()};

    return Result<EdgeConfig>::success(std::move(result));
}

Result<EdgeConfig> parse_uplink(const Json& root, EdgeConfig result)
{
    const Json& uplink = root.at("uplink");
    if (auto fields =
            exact_fields(uplink, "/uplink",
                         {"enabled", "serverUrl", "heartbeatSeconds", "chunkBytes", "ioTimeoutMs",
                          "uploadLimitMiBps", "credentialReference", "certificateReference"});
        !fields)
        return Result<EdgeConfig>::failure(fields.error());
    auto uplink_enabled = bool_field(uplink, "enabled", "/uplink");
    auto server_url = string_field(uplink, "serverUrl", "/uplink", 2048U, true);
    auto heartbeat =
        unsigned_field<std::uint32_t>(uplink, "heartbeatSeconds", "/uplink", 1U, 3600U);
    auto chunk_bytes = unsigned_field<std::uint32_t>(uplink, "chunkBytes", "/uplink", 64U * 1024U,
                                                     4U * 1024U * 1024U);
    auto io_timeout = unsigned_field<std::uint32_t>(uplink, "ioTimeoutMs", "/uplink", 100U, 60000U);
    auto upload_limit =
        unsigned_field<std::uint32_t>(uplink, "uploadLimitMiBps", "/uplink", 1U, 1024U);
    auto credential = string_field(uplink, "credentialReference", "/uplink", 256U, true);
    auto certificate = string_field(uplink, "certificateReference", "/uplink", 256U, true);
    if (!uplink_enabled)
        return Result<EdgeConfig>::failure(uplink_enabled.error());
    if (!server_url)
        return Result<EdgeConfig>::failure(server_url.error());
    if (!heartbeat)
        return Result<EdgeConfig>::failure(heartbeat.error());
    if (!chunk_bytes)
        return Result<EdgeConfig>::failure(chunk_bytes.error());
    if (!io_timeout)
        return Result<EdgeConfig>::failure(io_timeout.error());
    if (!upload_limit)
        return Result<EdgeConfig>::failure(upload_limit.error());
    if (!credential)
        return Result<EdgeConfig>::failure(credential.error());
    if (!certificate)
        return Result<EdgeConfig>::failure(certificate.error());
    if (uplink_enabled.value() && !server_url.value().starts_with("http://"))
        return Result<EdgeConfig>::failure(
            invalid_config("Uplink v1 启用时必须使用明文 http:// 基址", "config.validateDependency",
                           "/uplink", "uplink-security"));
    if (uplink_enabled.value() && (!credential.value().empty() || !certificate.value().empty()))
        return Result<EdgeConfig>::failure(
            invalid_config("Uplink v1 不使用 credentialReference 或 certificateReference",
                           "config.validateDependency", "/uplink", "uplink-unsupported-auth"));
    result.uplink = {.enabled = uplink_enabled.value(),
                     .server_url = std::move(server_url).value(),
                     .heartbeat_seconds = heartbeat.value(),
                     .chunk_bytes = chunk_bytes.value(),
                     .io_timeout_ms = io_timeout.value(),
                     .upload_limit_mibps = upload_limit.value(),
                     .credential_reference = std::move(credential).value(),
                     .certificate_reference = std::move(certificate).value()};

    return Result<EdgeConfig>::success(std::move(result));
}

Result<EdgeConfig> parse_plant_io(const Json& root, EdgeConfig result)
{
    const Json& plant = root.at("plantIo");
    if (auto fields = exact_fields(
            plant, "/plantIo",
            {"enabled", "adapterType", "endpoint", "credentialReference", "pollIntervalMs"});
        !fields)
        return Result<EdgeConfig>::failure(fields.error());
    auto plant_enabled = bool_field(plant, "enabled", "/plantIo");
    auto adapter = string_field(plant, "adapterType", "/plantIo", 32U);
    auto endpoint = string_field(plant, "endpoint", "/plantIo", 1024U, true);
    auto plant_credential = string_field(plant, "credentialReference", "/plantIo", 256U, true);
    auto poll = unsigned_field<std::uint32_t>(plant, "pollIntervalMs", "/plantIo", 10U, 60000U);
    if (!plant_enabled)
        return Result<EdgeConfig>::failure(plant_enabled.error());
    if (!adapter)
        return Result<EdgeConfig>::failure(adapter.error());
    if (!endpoint)
        return Result<EdgeConfig>::failure(endpoint.error());
    if (!plant_credential)
        return Result<EdgeConfig>::failure(plant_credential.error());
    if (!poll)
        return Result<EdgeConfig>::failure(poll.error());
    if (adapter.value() != "disabled" && adapter.value() != "mock")
        return Result<EdgeConfig>::failure(
            invalid_config("M1 只允许 disabled 或 mock Plant IO 适配器", "config.validateEnum",
                           "/plantIo/adapterType", "unapproved-plant-io-adapter"));
    if (plant_enabled.value() && adapter.value() != "mock")
        return Result<EdgeConfig>::failure(invalid_config("启用 Plant IO 时 M1 只允许 mock",
                                                          "config.validateDependency", "/plantIo",
                                                          "plant-io-not-approved"));
    result.plant_io = {.enabled = plant_enabled.value(),
                       .adapter_type = std::move(adapter).value(),
                       .endpoint = std::move(endpoint).value(),
                       .credential_reference = std::move(plant_credential).value(),
                       .poll_interval_ms = poll.value()};

    return Result<EdgeConfig>::success(std::move(result));
}

Result<EdgeConfig> parse_logging(const Json& root, const std::filesystem::path& config_directory,
                                 EdgeConfig result)
{
    const Json& logging = root.at("logging");
    if (auto fields = exact_fields(logging, "/logging",
                                   {"level", "directory", "retentionDays", "maximumFileSizeMiB",
                                    "maximumFilesPerDay", "queueCapacity"});
        !fields)
        return Result<EdgeConfig>::failure(fields.error());
    auto level_text = string_field(logging, "level", "/logging", 16U);
    auto log_directory = validate_path(logging, "directory", "/logging", config_directory);
    auto log_retention =
        unsigned_field<std::uint32_t>(logging, "retentionDays", "/logging", 1U, 3650U);
    auto max_file =
        unsigned_field<std::uint32_t>(logging, "maximumFileSizeMiB", "/logging", 1U, 1024U);
    auto max_files =
        unsigned_field<std::uint32_t>(logging, "maximumFilesPerDay", "/logging", 1U, 100U);
    auto log_queue =
        unsigned_field<std::uint32_t>(logging, "queueCapacity", "/logging", 128U, 65536U);
    if (!level_text)
        return Result<EdgeConfig>::failure(level_text.error());
    if (!log_directory)
        return Result<EdgeConfig>::failure(log_directory.error());
    if (!log_retention)
        return Result<EdgeConfig>::failure(log_retention.error());
    if (!max_file)
        return Result<EdgeConfig>::failure(max_file.error());
    if (!max_files)
        return Result<EdgeConfig>::failure(max_files.error());
    if (!log_queue)
        return Result<EdgeConfig>::failure(log_queue.error());
    auto level = enum_value<LogLevel>(level_text.value(), "/logging/level",
                                      {{"trace", LogLevel::trace},
                                       {"debug", LogLevel::debug},
                                       {"info", LogLevel::info},
                                       {"warning", LogLevel::warning},
                                       {"error", LogLevel::error},
                                       {"critical", LogLevel::critical}});
    if (!level)
        return Result<EdgeConfig>::failure(level.error());
    result.logging = {.level = level.value(),
                      .directory = std::move(log_directory).value(),
                      .retention_days = log_retention.value(),
                      .maximum_file_size_mib = max_file.value(),
                      .maximum_files_per_day = max_files.value(),
                      .queue_capacity = log_queue.value()};

    return Result<EdgeConfig>::success(std::move(result));
}

Result<EdgeConfig> parse_health(const Json& root, EdgeConfig result)
{
    const Json& health = root.at("health");
    if (auto fields = exact_fields(health, "/health",
                                   {"sampleIntervalMs", "cpuWarningPercent", "memoryWarningPercent",
                                    "droppedFrameWarningRatio", "heartbeatStaleSeconds"});
        !fields)
        return Result<EdgeConfig>::failure(fields.error());
    auto sample =
        unsigned_field<std::uint32_t>(health, "sampleIntervalMs", "/health", 100U, 60000U);
    auto cpu = finite_field(health, "cpuWarningPercent", "/health", 1.0, 100.0);
    auto memory = finite_field(health, "memoryWarningPercent", "/health", 1.0, 100.0);
    auto dropped = finite_field(health, "droppedFrameWarningRatio", "/health", 0.0, 1.0);
    auto stale =
        unsigned_field<std::uint32_t>(health, "heartbeatStaleSeconds", "/health", 1U, 3600U);
    if (!sample)
        return Result<EdgeConfig>::failure(sample.error());
    if (!cpu)
        return Result<EdgeConfig>::failure(cpu.error());
    if (!memory)
        return Result<EdgeConfig>::failure(memory.error());
    if (!dropped)
        return Result<EdgeConfig>::failure(dropped.error());
    if (!stale)
        return Result<EdgeConfig>::failure(stale.error());
    result.health = {.sample_interval_ms = sample.value(),
                     .cpu_warning_percent = cpu.value(),
                     .memory_warning_percent = memory.value(),
                     .dropped_frame_warning_ratio = dropped.value(),
                     .heartbeat_stale_seconds = stale.value()};

    return Result<EdgeConfig>::success(std::move(result));
}

} // namespace

Result<EdgeConfig> parse_config(const std::string_view contents,
                                const std::filesystem::path& config_directory) noexcept
{
    try
    {
        const Json root = Json::parse(contents.begin(), contents.end(), nullptr, false, true);
        if (root.is_discarded())
        {
            return Result<EdgeConfig>::failure(
                invalid_config("配置不是合法 UTF-8 JSON", "config.parseJson", "", "invalid-json"));
        }
        if (contains_sensitive_key(root))
        {
            return Result<EdgeConfig>::failure(
                invalid_config("普通配置不得包含密码、Token、Secret 或私钥字段",
                               "config.validateSecrets", "", "plaintext-secret-field"));
        }
        if (auto fields =
                exact_fields(root, "",
                             {"configSchemaVersion", "configRevision", "modifiedAt", "system",
                              "cameras", "acquisition", "preview", "algorithm", "event", "storage",
                              "uplink", "plantIo", "logging", "health"});
            !fields)
        {
            return Result<EdgeConfig>::failure(fields.error());
        }

        auto result = parse_metadata(root);
        if (!result)
            return result;
        result = parse_system(root, std::move(result).value());
        if (!result)
            return result;
        result = parse_cameras(root, std::move(result).value());
        if (!result)
            return result;
        result = parse_acquisition(root, std::move(result).value());
        if (!result)
            return result;
        result = parse_preview(root, std::move(result).value());
        if (!result)
            return result;
        result = parse_algorithm(root, std::move(result).value());
        if (!result)
            return result;
        result = parse_event(root, std::move(result).value());
        if (!result)
            return result;
        result = parse_storage(root, config_directory, std::move(result).value());
        if (!result)
            return result;
        result = parse_uplink(root, std::move(result).value());
        if (!result)
            return result;
        result = parse_plant_io(root, std::move(result).value());
        if (!result)
            return result;
        result = parse_logging(root, config_directory, std::move(result).value());
        if (!result)
            return result;
        return parse_health(root, std::move(result).value());
    }
    catch (const std::exception&)
    {
        return Result<EdgeConfig>::failure(
            invalid_config("配置解析发生未预期错误", "config.parse", "", "unexpected-exception"));
    }
    catch (...)
    {
        return Result<EdgeConfig>::failure(
            invalid_config("配置解析发生未知错误", "config.parse", "", "unknown-exception"));
    }
}

std::string serialize_config(const EdgeConfig& config)
{
    Json cameras = Json::array();
    for (const auto& camera : config.cameras)
    {
        cameras.push_back(
            {{"id", camera.id},
             {"enabled", camera.enabled},
             {"serialNumber", camera.serial_number},
             {"location", camera.location},
             {"exposureUs", camera.exposure_us},
             {"autoExposure", exposure_auto_mode_name(camera.exposure_auto_mode)},
             {"gainDb", camera.gain_db},
             {"frameRate", camera.frame_rate},
             {"roi", roi_json(camera.roi)},
             {"reverseX", camera.reverse_x},
             {"reverseY", camera.reverse_y},
             {"pixelFormat", pixel_format_name(camera.pixel_format)},
             {"triggerMode", trigger_mode_name(camera.trigger_mode)},
             {"triggerSource", camera.trigger_source},
             {"triggerDelayUs", camera.trigger_delay_us},
             {"packetSizeBytes", camera.packet_size_bytes},
             {"interPacketDelayNs", camera.inter_packet_delay_ns},
             {"lineIo",
              {{"alarmInputEnabled", camera.line_io.alarm_input_enabled},
               {"alarmActiveLevel", alarm_active_level_name(camera.line_io.alarm_active_level)},
               {"strobeOutputEnabled", camera.line_io.strobe_output_enabled},
               {"strobeDurationUs", camera.line_io.strobe_duration_us},
               {"strobePreDelayUs", camera.line_io.strobe_pre_delay_us},
               {"strobePostDelayUs", camera.line_io.strobe_post_delay_us}}}});
    }
    Json root = {{"configSchemaVersion", config_schema_version},
                 {"configRevision", config.config_revision},
                 {"modifiedAt", config.modified_at},
                 {"system",
                  {{"machineId", config.system.machine_id},
                   {"productionLineId", config.system.production_line_id},
                   {"shutdownTimeoutMs", config.system.shutdown_timeout_ms}}},
                 {"cameras", std::move(cameras)},
                 {"acquisition",
                  {{"autoStart", config.acquisition.auto_start},
                   {"startupRetryIntervalMs", config.acquisition.startup_retry_interval_ms},
                   {"startupRetryCount", config.acquisition.startup_retry_count},
                   {"framePoolCapacity", config.acquisition.frame_pool_capacity},
                   {"queueCapacity", config.acquisition.queue_capacity},
                   {"receiveTimeoutMs", config.acquisition.receive_timeout_ms},
                   {"threadPriority", config.acquisition.thread_priority}}},
                 {"preview",
                  {{"enabled", config.preview.enabled},
                   {"fps", config.preview.fps},
                   {"maxWidth", config.preview.max_width},
                   {"maxHeight", config.preview.max_height},
                   {"jpegQuality", config.preview.jpeg_quality}}},
                 {"algorithm",
                  {{"enabled", config.algorithm.enabled},
                   {"type", config.algorithm.type},
                   {"roi", roi_json(config.algorithm.roi)},
                   {"downsampleMode", downsample_mode_name(config.algorithm.downsample_mode)},
                   {"processingFps", static_cast<std::uint32_t>(config.algorithm.processing_fps)},
                   {"candidateThreshold", config.algorithm.candidate_threshold},
                   {"confirmationThreshold", config.algorithm.confirmation_threshold},
                   {"confirmationDurationMs", config.algorithm.confirmation_duration_ms},
                   {"cooldownMs", config.algorithm.cooldown_ms},
                   {"rearmDurationMs", config.algorithm.rearm_duration_ms},
                   {"modelReference", config.algorithm.model_reference},
                   {"modelVersion", config.algorithm.model_version},
                   {"device", config.algorithm.device},
                   {"debugOverlay", config.algorithm.debug_overlay}}},
                 {"event",
                  {{"preEventSeconds", config.event.pre_event_seconds},
                   {"postEventSeconds", config.event.post_event_seconds},
                   {"maxEventSeconds", config.event.max_event_seconds},
                   {"mergeGapSeconds", config.event.merge_gap_seconds},
                   {"keyFrameCount", config.event.key_frame_count},
                   {"saveRaw", config.event.save_raw},
                   {"generatePreviewVideo", config.event.generate_preview_video},
                   {"uploadPolicy", config.event.upload_policy},
                   {"retentionDays", config.event.retention_days}}},
                 {"storage",
                  {{"eventRoot", config.storage.event_root},
                   {"cacheRoot", config.storage.cache_root},
                   {"rollingCacheEnabled", config.storage.rolling_cache_enabled},
                   {"maximumCacheStorageGiB", config.storage.maximum_cache_storage_gib},
                   {"rollingCacheWriteLimitMiBps", config.storage.rolling_cache_write_limit_mibps},
                   {"rollingCacheIoTimeoutMs", config.storage.rolling_cache_io_timeout_ms},
                   {"warningFreeSpaceGiB", config.storage.warning_free_space_gib},
                   {"criticalFreeSpaceGiB", config.storage.critical_free_space_gib},
                   {"stopFreeSpaceGiB", config.storage.stop_free_space_gib},
                   {"maximumEventStorageGiB", config.storage.maximum_event_storage_gib}}},
                 {"uplink",
                  {{"enabled", config.uplink.enabled},
                   {"serverUrl", config.uplink.server_url},
                   {"heartbeatSeconds", config.uplink.heartbeat_seconds},
                   {"chunkBytes", config.uplink.chunk_bytes},
                   {"ioTimeoutMs", config.uplink.io_timeout_ms},
                   {"uploadLimitMiBps", config.uplink.upload_limit_mibps},
                   {"credentialReference", config.uplink.credential_reference},
                   {"certificateReference", config.uplink.certificate_reference}}},
                 {"plantIo",
                  {{"enabled", config.plant_io.enabled},
                   {"adapterType", config.plant_io.adapter_type},
                   {"endpoint", config.plant_io.endpoint},
                   {"credentialReference", config.plant_io.credential_reference},
                   {"pollIntervalMs", config.plant_io.poll_interval_ms}}},
                 {"logging",
                  {{"level", log_level_name(config.logging.level)},
                   {"directory", config.logging.directory},
                   {"retentionDays", config.logging.retention_days},
                   {"maximumFileSizeMiB", config.logging.maximum_file_size_mib},
                   {"maximumFilesPerDay", config.logging.maximum_files_per_day},
                   {"queueCapacity", config.logging.queue_capacity}}},
                 {"health",
                  {{"sampleIntervalMs", config.health.sample_interval_ms},
                   {"cpuWarningPercent", config.health.cpu_warning_percent},
                   {"memoryWarningPercent", config.health.memory_warning_percent},
                   {"droppedFrameWarningRatio", config.health.dropped_frame_warning_ratio},
                   {"heartbeatStaleSeconds", config.health.heartbeat_stale_seconds}}}};
    return root.dump(2) + "\n";
}

Result<BasicConfigInfo> validate_basic_config(const std::filesystem::path& path,
                                              const std::size_t maximum_bytes) noexcept
{
    if (path.empty() || maximum_bytes == 0U)
    {
        return Result<BasicConfigInfo>::failure(
            invalid_config("配置路径为空或读取上限无效", "config.validate", "", "invalid-input"));
    }
    try
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error)
            return Result<BasicConfigInfo>::failure(invalid_config(
                "配置文件不存在或不是普通文件", "config.inspectFile", "", "not-regular-file"));
        const auto size = std::filesystem::file_size(path, error);
        if (error || size == 0U || size > maximum_bytes ||
            size > static_cast<std::uintmax_t>((std::numeric_limits<std::size_t>::max)()))
            return Result<BasicConfigInfo>::failure(invalid_config(
                "配置文件为空或超过大小上限", "config.inspectFile", "", "invalid-file-size"));
        std::ifstream stream{path, std::ios::binary};
        std::string contents(static_cast<std::size_t>(size), '\0');
        if (!stream || !stream.read(contents.data(), static_cast<std::streamsize>(contents.size())))
            return Result<BasicConfigInfo>::failure(
                invalid_config("配置文件读取不完整", "config.readFile", "", "short-read"));
        auto parsed = parse_config(contents, path.parent_path());
        if (!parsed)
            return Result<BasicConfigInfo>::failure(parsed.error());
        return Result<BasicConfigInfo>::success(
            {.schema_version = parsed.value().config_schema_version,
             .config_revision = parsed.value().config_revision,
             .file_size_bytes = static_cast<std::size_t>(size)});
    }
    catch (...)
    {
        return Result<BasicConfigInfo>::failure(invalid_config(
            "配置校验发生未预期错误", "config.validate", "", "unexpected-exception"));
    }
}

std::vector<std::string> changed_config_paths(const EdgeConfig& current,
                                              const EdgeConfig& candidate)
{
    std::vector<std::string> paths;
    if (current.system != candidate.system)
        paths.emplace_back("/system");
    if (current.cameras != candidate.cameras)
    {
        const bool topology_changed =
            current.cameras.size() != candidate.cameras.size() ||
            !std::equal(current.cameras.begin(), current.cameras.end(), candidate.cameras.begin(),
                        candidate.cameras.end(),
                        [](const CameraConfig& left, const CameraConfig& right) {
                            return left.id == right.id && left.enabled == right.enabled &&
                                   left.serial_number == right.serial_number;
                        });
        paths.emplace_back(topology_changed ? "/cameras" : "/cameras/parameters");
    }
    if (current.acquisition != candidate.acquisition)
        paths.emplace_back("/acquisition");
    if (current.preview != candidate.preview)
        paths.emplace_back("/preview");
    if (current.algorithm != candidate.algorithm)
        paths.emplace_back("/algorithm");
    if (current.event != candidate.event)
        paths.emplace_back("/event");
    if (current.storage.event_root != candidate.storage.event_root ||
        current.storage.cache_root != candidate.storage.cache_root)
        paths.emplace_back("/storage/roots");
    if (current.storage.rolling_cache_enabled != candidate.storage.rolling_cache_enabled ||
        current.storage.maximum_cache_storage_gib != candidate.storage.maximum_cache_storage_gib ||
        current.storage.rolling_cache_write_limit_mibps !=
            candidate.storage.rolling_cache_write_limit_mibps ||
        current.storage.rolling_cache_io_timeout_ms !=
            candidate.storage.rolling_cache_io_timeout_ms)
        paths.emplace_back("/storage/nvme");
    if (current.storage.warning_free_space_gib != candidate.storage.warning_free_space_gib ||
        current.storage.critical_free_space_gib != candidate.storage.critical_free_space_gib ||
        current.storage.stop_free_space_gib != candidate.storage.stop_free_space_gib ||
        current.storage.maximum_event_storage_gib != candidate.storage.maximum_event_storage_gib)
        paths.emplace_back("/storage/watermarks");
    if (current.uplink != candidate.uplink)
        paths.emplace_back("/uplink/transport");
    if (current.plant_io != candidate.plant_io)
        paths.emplace_back("/plantIo");
    if (current.logging.directory != candidate.logging.directory ||
        current.logging.queue_capacity != candidate.logging.queue_capacity ||
        current.logging.maximum_file_size_mib != candidate.logging.maximum_file_size_mib ||
        current.logging.maximum_files_per_day != candidate.logging.maximum_files_per_day)
        paths.emplace_back("/logging/runtime");
    if (current.logging != candidate.logging)
        paths.emplace_back("/logging/live");
    if (current.health != candidate.health)
        paths.emplace_back("/health");
    std::ranges::sort(paths);
    paths.erase(std::ranges::unique(paths).begin(), paths.end());
    return paths;
}

bool is_restart_required_path(const std::string_view json_pointer) noexcept
{
    return json_pointer == "/system" || json_pointer == "/cameras" ||
           json_pointer == "/acquisition" || json_pointer == "/storage/roots" ||
           json_pointer == "/storage/nvme" || json_pointer == "/uplink/transport" ||
           json_pointer == "/plantIo" || json_pointer == "/logging/runtime";
}

} // namespace paperbreak::config
