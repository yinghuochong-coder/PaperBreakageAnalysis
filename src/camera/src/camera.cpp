#include "paperbreak/camera/camera.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace paperbreak::camera
{
namespace
{
struct ErrorDefaults final
{
    std::string_view code;
    Severity severity;
    bool retryable;
};

ErrorDefaults error_defaults(const CameraErrorKind kind) noexcept
{
    switch (kind)
    {
    case CameraErrorKind::not_found:
        return {"CAMERA_NOT_FOUND", Severity::error, true};
    case CameraErrorKind::open_failed:
        return {"CAMERA_OPEN_FAILED", Severity::error, true};
    case CameraErrorKind::access_denied:
        return {"CAMERA_ACCESS_DENIED", Severity::error, true};
    case CameraErrorKind::config_failed:
        return {"CAMERA_CONFIG_FAILED", Severity::error, false};
    case CameraErrorKind::stream_start_failed:
        return {"CAMERA_STREAM_START_FAILED", Severity::error, true};
    case CameraErrorKind::disconnected:
        return {"CAMERA_DISCONNECTED", Severity::warning, true};
    case CameraErrorKind::frame_timeout:
        return {"CAMERA_FRAME_TIMEOUT", Severity::warning, true};
    case CameraErrorKind::frame_incomplete:
        return {"CAMERA_FRAME_INCOMPLETE", Severity::warning, false};
    case CameraErrorKind::frame_format_changed:
        return {"CAMERA_FRAME_FORMAT_CHANGED", Severity::error, false};
    }
    return {"CAMERA_CONFIG_FAILED", Severity::error, false};
}

std::string serial_suffix(const std::string_view serial_number)
{
    constexpr std::size_t suffix_length = 4U;
    return std::string{serial_number.substr(
        serial_number.size() > suffix_length ? serial_number.size() - suffix_length : 0U)};
}

Error invalid_parameter(std::string parameter, std::string reason)
{
    return make_camera_error(CameraErrorKind::config_failed, "相机参数不符合设备能力",
                             "camera.validateParameters", std::nullopt,
                             {{"parameter", std::move(parameter)}, {"reason", std::move(reason)}});
}

template <typename T> bool valid_integral_range(const SteppedRange<T>& range) noexcept
{
    return range.minimum <= range.maximum && range.increment > 0;
}

template <typename T> bool contains_integral(const SteppedRange<T>& range, const T value) noexcept
{
    return valid_integral_range(range) && value >= range.minimum && value <= range.maximum &&
           (value - range.minimum) % range.increment == 0;
}

bool valid_floating_range(const SteppedRange<double>& range) noexcept
{
    return std::isfinite(range.minimum) && std::isfinite(range.maximum) &&
           std::isfinite(range.increment) && range.minimum <= range.maximum &&
           range.increment > 0.0;
}

bool contains_floating(const SteppedRange<double>& range, const double value) noexcept
{
    if (!valid_floating_range(range) || !std::isfinite(value) || value < range.minimum ||
        value > range.maximum)
    {
        return false;
    }
    const double steps = (value - range.minimum) / range.increment;
    const double tolerance = 1e-8 * std::max(1.0, std::abs(steps));
    return std::abs(steps - std::round(steps)) <= tolerance;
}

template <typename T, typename Predicate>
Result<void> validate_optional(const std::optional<T>& value,
                               const std::optional<SteppedRange<T>>& capability,
                               const std::string_view name, Predicate contains)
{
    if (!value)
    {
        return Result<void>::success();
    }
    if (!capability)
    {
        return Result<void>::failure(invalid_parameter(std::string{name}, "unsupported"));
    }
    if (!contains(*capability, *value))
    {
        return Result<void>::failure(invalid_parameter(std::string{name}, "out-of-range-or-step"));
    }
    return Result<void>::success();
}

template <typename T> bool contains_value(const std::vector<T>& values, const T value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

Result<void> validate_roi(const CameraCapabilities& capabilities,
                          const CameraParameterSnapshot& parameters)
{
    if (!parameters.roi)
    {
        return Result<void>::success();
    }
    if (!capabilities.roi)
    {
        return Result<void>::failure(invalid_parameter("roi", "unsupported"));
    }
    const auto& value = *parameters.roi;
    const auto& limits = *capabilities.roi;
    if (!contains_integral(limits.width, value.width) ||
        !contains_integral(limits.height, value.height) ||
        !contains_integral(limits.offset_x, value.offset_x) ||
        !contains_integral(limits.offset_y, value.offset_y))
    {
        return Result<void>::failure(invalid_parameter("roi", "out-of-range-or-step"));
    }
    if (value.width > limits.sensor_width || value.offset_x > limits.sensor_width - value.width ||
        value.height > limits.sensor_height || value.offset_y > limits.sensor_height - value.height)
    {
        return Result<void>::failure(invalid_parameter("roi", "outside-sensor"));
    }
    return Result<void>::success();
}

Result<void> validate_enums(const CameraCapabilities& capabilities,
                            const CameraParameterSnapshot& parameters)
{
    if (parameters.pixel_format &&
        !contains_value(capabilities.pixel_formats, *parameters.pixel_format))
    {
        return Result<void>::failure(invalid_parameter("pixelFormat", "unsupported"));
    }
    if (parameters.trigger_mode &&
        !contains_value(capabilities.trigger_modes, *parameters.trigger_mode))
    {
        return Result<void>::failure(invalid_parameter("triggerMode", "unsupported"));
    }
    if (parameters.trigger_source)
    {
        if (!contains_value(capabilities.trigger_sources, *parameters.trigger_source))
        {
            return Result<void>::failure(invalid_parameter("triggerSource", "unsupported"));
        }
        if (!parameters.trigger_mode || *parameters.trigger_mode != TriggerMode::hardware)
        {
            return Result<void>::failure(
                invalid_parameter("triggerSource", "requires-hardware-trigger"));
        }
    }
    if (parameters.trigger_mode && *parameters.trigger_mode == TriggerMode::hardware &&
        (!parameters.trigger_source || parameters.trigger_source->empty()))
    {
        return Result<void>::failure(
            invalid_parameter("triggerSource", "required-for-hardware-trigger"));
    }
    return Result<void>::success();
}

Result<void> validate_digital_io(const CameraCapabilities& capabilities,
                                 const CameraParameterSnapshot& parameters)
{
    std::set<std::string> seen;
    for (const auto& state : parameters.digital_io)
    {
        const auto capability = std::find_if(
            capabilities.digital_io.begin(), capabilities.digital_io.end(),
            [&state](const DigitalIoCapability& item) { return item.line_id == state.line_id; });
        if (capability == capabilities.digital_io.end() || !capability->writable)
        {
            return Result<void>::failure(invalid_parameter("digitalIo", "unsupported-line"));
        }
        if (!seen.insert(state.line_id).second)
        {
            return Result<void>::failure(invalid_parameter("digitalIo", "duplicate-line"));
        }
    }
    return Result<void>::success();
}
} // namespace

std::string_view camera_business_code(const CameraErrorKind kind) noexcept
{
    return error_defaults(kind).code;
}

Error make_camera_error(const CameraErrorKind kind, std::string message, std::string operation,
                        std::optional<std::string> source_id, std::vector<ErrorDetail> details)
{
    const auto defaults = error_defaults(kind);
    auto error = make_error(std::string{defaults.code}, defaults.severity, std::move(message),
                            "camera", std::move(operation), defaults.retryable);
    error.source_id = std::move(source_id);
    error.details = std::move(details);
    return error;
}

Result<void> validate_device_inventory(const std::span<const CameraDeviceDescriptor> devices)
{
    std::set<std::string> serial_numbers;
    for (const auto& device : devices)
    {
        if (device.serial_number.empty())
        {
            return Result<void>::failure(make_camera_error(
                CameraErrorKind::config_failed, "发现的相机缺少序列号", "camera.enumerate",
                std::nullopt, {{"reason", "missing-serial-number"}}));
        }
        if (!serial_numbers.insert(device.serial_number).second)
        {
            return Result<void>::failure(
                make_camera_error(CameraErrorKind::config_failed, "发现重复的相机序列号",
                                  "camera.enumerate", std::nullopt,
                                  {{"reason", "duplicate-serial-number"},
                                   {"serialNumberSuffix", serial_suffix(device.serial_number)}}));
        }
    }
    return Result<void>::success();
}

Result<CameraDeviceDescriptor> find_device_by_serial(
    const std::span<const CameraDeviceDescriptor> devices, const std::string_view serial_number)
{
    if (const auto inventory = validate_device_inventory(devices); !inventory)
    {
        return Result<CameraDeviceDescriptor>::failure(inventory.error());
    }
    const auto match = std::find_if(devices.begin(), devices.end(),
                                    [serial_number](const CameraDeviceDescriptor& device) {
                                        return device.serial_number == serial_number;
                                    });
    if (match == devices.end())
    {
        return Result<CameraDeviceDescriptor>::failure(make_camera_error(
            CameraErrorKind::not_found, "未发现绑定的实体相机", "camera.findBySerial", std::nullopt,
            {{"serialNumberSuffix", serial_suffix(serial_number)}}));
    }
    return Result<CameraDeviceDescriptor>::success(*match);
}

Result<void> validate_parameters(const CameraCapabilities& capabilities,
                                 const CameraParameterSnapshot& parameters)
{
    if (auto result = validate_optional(parameters.exposure_us, capabilities.exposure_us,
                                        "exposureUs", contains_floating);
        !result)
    {
        return result;
    }
    if (auto result = validate_optional(parameters.gain_db, capabilities.gain_db, "gainDb",
                                        contains_floating);
        !result)
    {
        return result;
    }
    if (auto result = validate_optional(parameters.frame_rate, capabilities.frame_rate, "frameRate",
                                        contains_floating);
        !result)
    {
        return result;
    }
    if (auto result = validate_roi(capabilities, parameters); !result)
    {
        return result;
    }
    if (auto result = validate_enums(capabilities, parameters); !result)
    {
        return result;
    }
    if (auto result = validate_optional(parameters.trigger_delay_us, capabilities.trigger_delay_us,
                                        "triggerDelayUs", contains_integral<std::uint32_t>);
        !result)
    {
        return result;
    }
    if (auto result =
            validate_optional(parameters.packet_size_bytes, capabilities.packet_size_bytes,
                              "packetSizeBytes", contains_integral<std::uint32_t>);
        !result)
    {
        return result;
    }
    if (auto result =
            validate_optional(parameters.inter_packet_delay_ns, capabilities.inter_packet_delay_ns,
                              "interPacketDelayNs", contains_integral<std::uint32_t>);
        !result)
    {
        return result;
    }
    return validate_digital_io(capabilities, parameters);
}

Result<CameraParameterSnapshot> apply_validated_parameters(
    ICameraDevice& device, const CameraParameterSnapshot& parameters)
{
    auto capabilities = device.capabilities();
    if (!capabilities)
    {
        return Result<CameraParameterSnapshot>::failure(capabilities.error());
    }
    if (auto validation = validate_parameters(capabilities.value(), parameters); !validation)
    {
        return Result<CameraParameterSnapshot>::failure(validation.error());
    }
    return device.apply_parameters(parameters);
}

} // namespace paperbreak::camera
