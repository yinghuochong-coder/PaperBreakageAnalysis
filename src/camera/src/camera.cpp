#include "paperbreak/camera/camera.hpp"

#include "paperbreak/common/camera_slots.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string_view>
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
    case CameraErrorKind::parameter_read_failed:
        return {"CAMERA_PARAMETER_READ_FAILED", Severity::error, true};
    case CameraErrorKind::parameter_write_failed:
        return {"CAMERA_PARAMETER_WRITE_FAILED", Severity::error, true};
    case CameraErrorKind::parameter_faulted:
        return {"CAMERA_PARAMETER_FAULTED", Severity::critical, false};
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
    case CameraErrorKind::invalid_state_transition:
        return {"CAMERA_INVALID_STATE_TRANSITION", Severity::error, false};
    }
    return {"CAMERA_CONFIG_FAILED", Severity::error, false};
}

std::string serial_suffix(const std::string_view serial_number)
{
    constexpr std::size_t suffix_length = 4U;
    return std::string{serial_number.substr(
        serial_number.size() > suffix_length ? serial_number.size() - suffix_length : 0U)};
}

bool valid_external_text(const std::string_view value, const std::size_t maximum_size) noexcept
{
    if (value.empty() || value.size() > maximum_size)
    {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return character >= 0x20U && character != 0x7FU;
    });
}

Error invalid_parameter(std::string parameter, std::string reason)
{
    const auto message = "相机参数不符合设备能力（参数：" + parameter + "，原因：" + reason + "）";
    return make_camera_error(CameraErrorKind::config_failed, message, "camera.validateParameters",
                             std::nullopt,
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
           range.increment >= 0.0;
}

bool contains_floating(const SteppedRange<double>& range, const double value) noexcept
{
    if (!valid_floating_range(range) || !std::isfinite(value) || value < range.minimum ||
        value > range.maximum)
    {
        return false;
    }
    if (range.increment == 0.0)
    {
        return true;
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
    if (!contains_integral(limits.width, value.width))
        return Result<void>::failure(invalid_parameter("roi.width", "out-of-range-or-step"));
    if (!contains_integral(limits.height, value.height))
        return Result<void>::failure(invalid_parameter("roi.height", "out-of-range-or-step"));
    if (!contains_integral(limits.offset_x, value.offset_x))
        return Result<void>::failure(invalid_parameter("roi.offsetX", "out-of-range-or-step"));
    if (!contains_integral(limits.offset_y, value.offset_y))
        return Result<void>::failure(invalid_parameter("roi.offsetY", "out-of-range-or-step"));
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
    if (parameters.exposure_auto_mode &&
        !contains_value(capabilities.exposure_auto_modes, *parameters.exposure_auto_mode))
    {
        return Result<void>::failure(invalid_parameter("autoExposure", "unsupported"));
    }
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

Result<void> validate_optional_feature(const std::optional<bool>& value, const bool supported,
                                       const std::string_view name)
{
    if (value.value_or(false) && !supported)
    {
        return Result<void>::failure(invalid_parameter(std::string{name}, "unsupported"));
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

Result<void> validate_line_io(const CameraCapabilities& capabilities,
                              const CameraParameterSnapshot& parameters)
{
    if (!parameters.line_io)
        return Result<void>::success();
    const auto& requested = *parameters.line_io;
    const auto& available = capabilities.line_io;
    if (requested.alarm_input_enabled &&
        (!available.alarm_input_supported || !available.line0_rising_edge_supported ||
         !available.line0_falling_edge_supported))
        return Result<void>::failure(invalid_parameter("lineIo.alarmInputEnabled", "unsupported"));
    if (requested.strobe_output_enabled && !available.strobe_output_supported)
        return Result<void>::failure(
            invalid_parameter("lineIo.strobeOutputEnabled", "unsupported"));
    if (requested.strobe_output_enabled && requested.strobe_duration_us == 0U)
        return Result<void>::failure(
            invalid_parameter("lineIo.strobeDurationUs", "required-when-enabled"));
    const auto validate_range = [](const std::uint32_t value,
                                   const std::optional<SteppedRange<std::uint32_t>>& range,
                                   const char* name) {
        if (!range || !contains_integral(*range, value))
            return Result<void>::failure(invalid_parameter(name, "outside-capability"));
        return Result<void>::success();
    };
    if (available.strobe_output_supported && requested.strobe_output_enabled)
    {
        if (auto result = validate_range(requested.strobe_duration_us, available.strobe_duration_us,
                                         "lineIo.strobeDurationUs");
            !result)
            return result;
        if (auto result = validate_range(requested.strobe_pre_delay_us,
                                         available.strobe_pre_delay_us, "lineIo.strobePreDelayUs");
            !result)
            return result;
        if (auto result =
                validate_range(requested.strobe_post_delay_us, available.strobe_post_delay_us,
                               "lineIo.strobePostDelayUs");
            !result)
            return result;
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
        if (!valid_external_text(device.serial_number, 128U) ||
            !valid_external_text(device.model_name, 128U) ||
            !valid_external_text(device.ip_address, 45U) ||
            !valid_external_text(device.network_interface, 128U))
        {
            return Result<void>::failure(make_camera_error(
                CameraErrorKind::config_failed, "发现的相机描述信息无效", "camera.enumerate",
                std::nullopt, {{"reason", "invalid-device-descriptor"}}));
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

Result<CameraDiscoveryReport> reconcile_camera_slots(
    const std::span<const CameraSlotBinding> bindings,
    const std::span<const CameraDeviceDescriptor> devices)
{
    if (bindings.size() > camera_slot_count)
    {
        return Result<CameraDiscoveryReport>::failure(make_camera_error(
            CameraErrorKind::config_failed, "逻辑相机槽位不能超过六个", "camera.reconcileSlots",
            std::nullopt, {{"reason", "too-many-camera-slots"}}));
    }
    if (const auto inventory = validate_device_inventory(devices); !inventory)
    {
        return Result<CameraDiscoveryReport>::failure(inventory.error());
    }

    std::set<std::string> camera_ids;
    std::set<std::string> configured_serials;
    for (const auto& binding : bindings)
    {
        if (!is_canonical_camera_id(binding.camera_id) ||
            !camera_ids.insert(binding.camera_id).second)
        {
            return Result<CameraDiscoveryReport>::failure(make_camera_error(
                CameraErrorKind::config_failed, "逻辑相机 ID 必须是唯一的 CAM01 至 CAM06",
                "camera.reconcileSlots", std::nullopt,
                {{"reason", "invalid-or-duplicate-camera-id"}}));
        }
        if (!valid_external_text(binding.serial_number, 128U) ||
            !configured_serials.insert(binding.serial_number).second)
        {
            return Result<CameraDiscoveryReport>::failure(make_camera_error(
                CameraErrorKind::config_failed, "逻辑相机必须绑定唯一的有效序列号",
                "camera.reconcileSlots", binding.camera_id,
                {{"reason", "invalid-or-duplicate-configured-serial"}}));
        }
    }

    CameraDiscoveryReport report;
    report.slots.reserve(bindings.size());
    report.unexpected_devices.reserve(devices.size());
    for (const auto& binding : bindings)
    {
        const auto match = std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
            return device.serial_number == binding.serial_number;
        });
        if (match == devices.end())
        {
            report.slots.push_back({binding.camera_id, binding.serial_number,
                                    CameraSlotStatus::missing, std::nullopt});
            continue;
        }
        report.slots.push_back({binding.camera_id, binding.serial_number,
                                match->exclusive_access_available ? CameraSlotStatus::ready
                                                                  : CameraSlotStatus::occupied,
                                *match});
    }
    for (const auto& device : devices)
    {
        if (!configured_serials.contains(device.serial_number))
        {
            report.unexpected_devices.push_back(device);
        }
    }
    return Result<CameraDiscoveryReport>::success(std::move(report));
}

Result<CameraDeviceDescriptor> find_device_by_serial(
    const std::span<const CameraDeviceDescriptor> devices, const std::string_view serial_number)
{
    if (!valid_external_text(serial_number, 128U))
    {
        return Result<CameraDeviceDescriptor>::failure(make_camera_error(
            CameraErrorKind::config_failed, "相机序列号无效", "camera.findBySerial", std::nullopt,
            {{"reason", "invalid-serial-number"}}));
    }
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
    if (auto result = validate_optional_feature(parameters.reverse_x,
                                                capabilities.supports_reverse_x, "reverseX");
        !result)
    {
        return result;
    }
    if (auto result = validate_optional_feature(parameters.reverse_y,
                                                capabilities.supports_reverse_y, "reverseY");
        !result)
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
    if (auto result = validate_digital_io(capabilities, parameters); !result)
        return result;
    return validate_line_io(capabilities, parameters);
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
