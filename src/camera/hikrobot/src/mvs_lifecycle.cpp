#include "mvs_lifecycle.hpp"

#include "paperbreak/camera/hikrobot_camera.hpp"

#include <MvErrorDefine.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace paperbreak::camera::hikrobot::detail
{
namespace
{
std::string native_code_text(const int native_code)
{
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
           << static_cast<std::uint32_t>(native_code);
    return stream.str();
}

CameraErrorKind classify_open_error(const int native_code) noexcept
{
    if (native_code == static_cast<int>(MV_E_ACCESS_DENIED) ||
        native_code == static_cast<int>(MV_E_DEV_ACCESS_DENIED) ||
        native_code == static_cast<int>(MV_E_RESOURCE_IN_USE))
    {
        return CameraErrorKind::access_denied;
    }
    return CameraErrorKind::open_failed;
}

template <std::size_t Size>
Result<std::string> bounded_sdk_text(const unsigned char (&value)[Size],
                                     const std::string_view field)
{
    const auto terminator = std::find(std::begin(value), std::end(value), 0U);
    if (terminator == std::begin(value) || terminator == std::end(value) ||
        std::any_of(std::begin(value), terminator,
                    [](const unsigned char character) { return character < 0x20U; }))
    {
        return Result<std::string>::failure(
            make_camera_error(CameraErrorKind::config_failed, "MVS GigE 设备字段无效",
                              "camera.hikrobot.mapDescriptor", std::nullopt,
                              {{"field", std::string{field}}, {"reason", "invalid-bounded-text"}}));
    }
    return Result<std::string>::success(
        std::string{reinterpret_cast<const char*>(value),
                    static_cast<std::size_t>(terminator - std::begin(value))});
}

std::string ipv4_text(const unsigned int value)
{
    std::ostringstream stream;
    stream << ((value >> 24U) & 0xFFU) << '.' << ((value >> 16U) & 0xFFU) << '.'
           << ((value >> 8U) & 0xFFU) << '.' << (value & 0xFFU);
    return stream.str();
}

Error milestone_not_implemented(const std::string_view operation)
{
    return make_camera_error(CameraErrorKind::config_failed, "当前里程碑尚未实现该相机操作",
                             std::string{operation}, std::nullopt,
                             {{"reason", "not-implemented-in-m3-04"}});
}
} // namespace

const MvsApi& production_mvs_api() noexcept
{
    static const MvsApi api{.get_sdk_version = &MV_CC_GetSDKVersion,
                            .enumerate_devices = &MV_CC_EnumDevices,
                            .is_device_accessible = &MV_CC_IsDeviceAccessible,
                            .create_handle = &MV_CC_CreateHandle,
                            .open_device = &MV_CC_OpenDevice,
                            .close_device = &MV_CC_CloseDevice,
                            .destroy_handle = &MV_CC_DestroyHandle,
                            .start_grabbing = &MV_CC_StartGrabbing,
                            .stop_grabbing = &MV_CC_StopGrabbing,
                            .get_one_frame_timeout = &MV_CC_GetOneFrameTimeout,
                            .get_float_value = &MV_CC_GetFloatValue,
                            .set_float_value = &MV_CC_SetFloatValue,
                            .get_int_value = &MV_CC_GetIntValueEx,
                            .set_int_value = &MV_CC_SetIntValueEx,
                            .get_enum_value = &MV_CC_GetEnumValue,
                            .set_enum_value = &MV_CC_SetEnumValue,
                            .set_enum_value_by_string = &MV_CC_SetEnumValueByString,
                            .get_bool_value = &MV_CC_GetBoolValue,
                            .set_bool_value = &MV_CC_SetBoolValue,
                            .set_command_value = &MV_CC_SetCommandValue,
                            .register_event_callback = &MV_CC_RegisterEventCallBackEx,
                            .event_notification_on = &MV_CC_EventNotificationOn,
                            .event_notification_off = &MV_CC_EventNotificationOff};
    return api;
}

Result<CameraDeviceDescriptor> map_gige_descriptor(const MV_CC_DEVICE_INFO& device_info,
                                                   const bool exclusive_access_available)
{
    if (device_info.nTLayerType != MV_GIGE_DEVICE)
    {
        return Result<CameraDeviceDescriptor>::failure(
            make_camera_error(CameraErrorKind::config_failed, "MVS 返回了非 GigE 设备",
                              "camera.hikrobot.mapDescriptor", std::nullopt,
                              {{"reason", "unexpected-transport-type"}}));
    }
    const auto& gige = device_info.SpecialInfo.stGigEInfo;
    auto model = bounded_sdk_text(gige.chModelName, "modelName");
    auto serial = bounded_sdk_text(gige.chSerialNumber, "serialNumber");
    if (!model)
    {
        return Result<CameraDeviceDescriptor>::failure(model.error());
    }
    if (!serial)
    {
        return Result<CameraDeviceDescriptor>::failure(serial.error());
    }
    if (gige.nCurrentIp == 0U || gige.nNetExport == 0U)
    {
        return Result<CameraDeviceDescriptor>::failure(make_camera_error(
            CameraErrorKind::config_failed, "MVS GigE 网络地址无效",
            "camera.hikrobot.mapDescriptor", std::nullopt, {{"reason", "zero-ip-address"}}));
    }
    return Result<CameraDeviceDescriptor>::success(
        {.model_name = std::move(model).value(),
         .serial_number = std::move(serial).value(),
         .ip_address = ipv4_text(gige.nCurrentIp),
         .network_interface = ipv4_text(gige.nNetExport),
         .exclusive_access_available = exclusive_access_available});
}

Error translate_mvs_error(const CameraErrorKind kind, const int native_code, std::string operation,
                          std::string message)
{
    auto error = make_camera_error(kind, std::move(message), std::move(operation));
    error.native_domain = "hikrobot-mvs";
    error.native_code = native_code_text(native_code);
    return error;
}

Result<DeviceList> DeviceList::enumerate(const MvsApi& api, const unsigned int transport_types)
{
    MV_CC_DEVICE_INFO_LIST list{};
    const int code = api.enumerate_devices(transport_types, &list);
    if (code != MV_OK)
    {
        return Result<DeviceList>::failure(translate_mvs_error(
            CameraErrorKind::not_found, code, "camera.hikrobot.enumerate", "MVS 设备枚举失败"));
    }
    if (list.nDeviceNum > MV_MAX_DEVICE_NUM ||
        std::any_of(list.pDeviceInfo, list.pDeviceInfo + list.nDeviceNum,
                    [](const auto* device) { return device == nullptr; }))
    {
        return Result<DeviceList>::failure(
            translate_mvs_error(CameraErrorKind::not_found, MV_E_INTERNAL,
                                "camera.hikrobot.enumerate", "MVS 设备枚举返回了无效列表"));
    }
    return Result<DeviceList>::success(DeviceList{list});
}

DeviceList::DeviceList(const MV_CC_DEVICE_INFO_LIST& list)
{
    devices_.reserve(list.nDeviceNum);
    for (std::size_t index = 0U; index < list.nDeviceNum; ++index)
    {
        devices_.push_back(*list.pDeviceInfo[index]);
    }
}

std::size_t DeviceList::size() const noexcept
{
    return devices_.size();
}

const MV_CC_DEVICE_INFO* DeviceList::at(const std::size_t index) const noexcept
{
    return index < devices_.size() ? &devices_[index] : nullptr;
}

namespace
{
class HikrobotCameraDevice final : public ICameraDevice
{
  public:
    HikrobotCameraDevice(const MvsApi& api, MV_CC_DEVICE_INFO device_info,
                         CameraDeviceDescriptor descriptor)
        : api_(api), device_info_(device_info), descriptor_(std::move(descriptor))
    {
    }

    ~HikrobotCameraDevice() override
    {
        static_cast<void>(disconnect());
    }

    [[nodiscard]] const CameraDeviceDescriptor& descriptor() const noexcept override
    {
        return descriptor_;
    }

    [[nodiscard]] Result<void> connect() override
    {
        if (handle_)
        {
            return Result<void>::success();
        }
        auto opened = DeviceHandle::open(api_, device_info_);
        if (!opened)
        {
            return Result<void>::failure(opened.error());
        }
        handle_.emplace(std::move(opened).value());
        handle_->set_line_input_observer(line_input_observer_);
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> disconnect() override
    {
        if (stream_)
        {
            auto stopped = stream_->stop();
            if (!stopped)
            {
                return stopped;
            }
            stream_.reset();
        }
        if (!handle_)
        {
            return Result<void>::success();
        }
        auto closed = handle_->close();
        if (closed)
        {
            handle_.reset();
        }
        return closed;
    }

    void set_line_input_observer(LineInputObserver observer) override
    {
        line_input_observer_ = std::move(observer);
        if (handle_)
            handle_->set_line_input_observer(line_input_observer_);
    }

    [[nodiscard]] Result<CameraCapabilities> capabilities() override
    {
        if (!handle_)
        {
            return Result<CameraCapabilities>::failure(
                make_camera_error(CameraErrorKind::invalid_state_transition, "相机尚未连接",
                                  "camera.hikrobot.capabilities", descriptor_.serial_number,
                                  {{"reason", "not-connected"}}));
        }
        return handle_->capabilities();
    }
    [[nodiscard]] Result<CameraParameterSnapshot> read_parameters() override
    {
        if (!handle_)
        {
            return Result<CameraParameterSnapshot>::failure(
                make_camera_error(CameraErrorKind::invalid_state_transition, "相机尚未连接",
                                  "camera.hikrobot.readParameters", descriptor_.serial_number,
                                  {{"reason", "not-connected"}}));
        }
        return handle_->read_parameters();
    }
    [[nodiscard]] Result<CameraParameterSnapshot> apply_parameters(
        const CameraParameterSnapshot& parameters) override
    {
        if (!handle_)
        {
            return Result<CameraParameterSnapshot>::failure(
                make_camera_error(CameraErrorKind::invalid_state_transition, "相机尚未连接",
                                  "camera.hikrobot.applyParameters", descriptor_.serial_number,
                                  {{"reason", "not-connected"}}));
        }
        return handle_->apply_parameters(parameters);
    }
    [[nodiscard]] Result<void> start_acquisition() override
    {
        if (!handle_)
        {
            return Result<void>::failure(
                make_camera_error(CameraErrorKind::invalid_state_transition, "相机尚未连接",
                                  "camera.hikrobot.startAcquisition", descriptor_.serial_number,
                                  {{"reason", "not-connected"}}));
        }
        auto started = handle_->start_streaming();
        if (!started)
        {
            return Result<void>::failure(started.error());
        }
        stream_.emplace(std::move(started).value());
        return Result<void>::success();
    }
    [[nodiscard]] Result<CapturedFrameMetadata> capture_into(
        FrameBuffer& destination, const std::chrono::milliseconds timeout) override
    {
        if (!handle_ || !stream_ || !stream_->active())
        {
            return Result<CapturedFrameMetadata>::failure(
                make_camera_error(CameraErrorKind::invalid_state_transition, "相机尚未开始取流",
                                  "camera.hikrobot.capture", descriptor_.serial_number,
                                  {{"reason", "not-streaming"}}));
        }
        return handle_->capture_into(destination, timeout);
    }
    [[nodiscard]] Result<void> software_trigger() override
    {
        if (!handle_)
        {
            return Result<void>::failure(
                make_camera_error(CameraErrorKind::invalid_state_transition, "相机尚未连接",
                                  "camera.hikrobot.softwareTrigger", descriptor_.serial_number,
                                  {{"reason", "not-connected"}}));
        }
        return handle_->software_trigger();
    }
    [[nodiscard]] Result<void> stop_acquisition() override
    {
        if (!stream_)
        {
            return Result<void>::success();
        }
        auto stopped = stream_->stop();
        if (stopped)
        {
            stream_.reset();
        }
        return stopped;
    }
    [[nodiscard]] Result<CameraClockSample> sample_clock(
        const std::stop_token stop_token,
        const std::chrono::steady_clock::time_point deadline) override
    {
        if (!handle_)
        {
            auto error = make_error("TIME_PROBE_UNAVAILABLE", Severity::warning,
                                    "相机尚未连接，无法采样时间能力", "camera.hikrobot",
                                    "camera.hikrobot.sampleClock", true);
            error.source_id = descriptor_.serial_number;
            return Result<CameraClockSample>::failure(std::move(error));
        }
        return handle_->sample_clock(stop_token, deadline);
    }
    [[nodiscard]] Result<void> save_user_set(std::string_view) override
    {
        return Result<void>::failure(milestone_not_implemented("camera.hikrobot.saveUserSet"));
    }
    [[nodiscard]] Result<CameraParameterSnapshot> restore_defaults() override
    {
        return Result<CameraParameterSnapshot>::failure(
            milestone_not_implemented("camera.hikrobot.restoreDefaults"));
    }

  private:
    const MvsApi& api_;
    MV_CC_DEVICE_INFO device_info_{};
    CameraDeviceDescriptor descriptor_;
    std::optional<DeviceHandle> handle_;
    std::optional<StreamSession> stream_;
    LineInputObserver line_input_observer_;
};
} // namespace

HikrobotCameraProvider::HikrobotCameraProvider(const MvsApi& api) noexcept : api_(api) {}

Result<std::vector<CameraDeviceDescriptor>> HikrobotCameraProvider::enumerate_devices()
{
    auto list_result = DeviceList::enumerate(api_, MV_GIGE_DEVICE);
    if (!list_result)
    {
        return Result<std::vector<CameraDeviceDescriptor>>::failure(list_result.error());
    }
    auto list = std::move(list_result).value();
    std::vector<CameraDeviceDescriptor> descriptors;
    descriptors.reserve(list.size());
    for (std::size_t index = 0U; index < list.size(); ++index)
    {
        const auto* info = list.at(index);
        if (info == nullptr)
        {
            return Result<std::vector<CameraDeviceDescriptor>>::failure(make_camera_error(
                CameraErrorKind::config_failed, "MVS 设备列表索引无效", "camera.hikrobot.enumerate",
                std::nullopt, {{"reason", "invalid-device-list-index"}}));
        }
        auto mutable_info = *info;
        const bool accessible = api_.is_device_accessible(&mutable_info, MV_ACCESS_Exclusive);
        auto mapped = map_gige_descriptor(*info, accessible);
        if (!mapped)
        {
            return Result<std::vector<CameraDeviceDescriptor>>::failure(mapped.error());
        }
        descriptors.push_back(std::move(mapped).value());
    }
    if (const auto validation = validate_device_inventory(descriptors); !validation)
    {
        return Result<std::vector<CameraDeviceDescriptor>>::failure(validation.error());
    }
    return Result<std::vector<CameraDeviceDescriptor>>::success(std::move(descriptors));
}

Result<std::unique_ptr<ICameraDevice>> HikrobotCameraProvider::create_device(
    const std::string_view serial_number)
{
    auto list_result = DeviceList::enumerate(api_, MV_GIGE_DEVICE);
    if (!list_result)
    {
        return Result<std::unique_ptr<ICameraDevice>>::failure(list_result.error());
    }
    auto list = std::move(list_result).value();
    std::vector<CameraDeviceDescriptor> descriptors;
    descriptors.reserve(list.size());
    for (std::size_t index = 0U; index < list.size(); ++index)
    {
        const auto* info = list.at(index);
        if (info == nullptr)
        {
            continue;
        }
        auto mutable_info = *info;
        auto mapped = map_gige_descriptor(
            *info, api_.is_device_accessible(&mutable_info, MV_ACCESS_Exclusive));
        if (!mapped)
        {
            return Result<std::unique_ptr<ICameraDevice>>::failure(mapped.error());
        }
        descriptors.push_back(mapped.value());
    }
    auto match = find_device_by_serial(descriptors, serial_number);
    if (!match)
    {
        return Result<std::unique_ptr<ICameraDevice>>::failure(match.error());
    }
    const auto descriptor_index = static_cast<std::size_t>(std::distance(
        descriptors.begin(), std::find(descriptors.begin(), descriptors.end(), match.value())));
    std::unique_ptr<ICameraDevice> device = std::make_unique<HikrobotCameraDevice>(
        api_, *list.at(descriptor_index), std::move(match).value());
    return Result<std::unique_ptr<ICameraDevice>>::success(std::move(device));
}

namespace
{
bool optional_node_absent(const int code) noexcept
{
    return code == static_cast<int>(MV_E_SUPPORT) || code == static_cast<int>(MV_E_GC_ACCESS);
}

Error parameter_error(const CameraErrorKind kind, const int code, const std::string_view operation,
                      const std::string_view node, const std::string_view reason)
{
    auto error = translate_mvs_error(kind, code, std::string{operation}, "MVS 相机参数操作失败");
    error.details = {{"node", std::string{node}}, {"reason", std::string{reason}}};
    return error;
}

Result<std::optional<MVCC_FLOATVALUE>> optional_float(const MvsApi& api, void* handle,
                                                      const char* node)
{
    MVCC_FLOATVALUE value{};
    const int code = api.get_float_value(handle, node, &value);
    if (code == MV_OK)
    {
        if (!std::isfinite(value.fCurValue) || !std::isfinite(value.fMin) ||
            !std::isfinite(value.fMax) || value.fMin > value.fMax)
        {
            return Result<std::optional<MVCC_FLOATVALUE>>::failure(
                parameter_error(CameraErrorKind::parameter_read_failed, MV_E_PARAMETER,
                                "camera.hikrobot.capabilities", node, "invalid-float-range"));
        }
        return Result<std::optional<MVCC_FLOATVALUE>>::success(value);
    }
    if (optional_node_absent(code))
    {
        return Result<std::optional<MVCC_FLOATVALUE>>::success(std::nullopt);
    }
    return Result<std::optional<MVCC_FLOATVALUE>>::failure(
        parameter_error(CameraErrorKind::parameter_read_failed, code,
                        "camera.hikrobot.capabilities", node, "read-failed"));
}

Result<std::optional<MVCC_INTVALUE_EX>> optional_integer(const MvsApi& api, void* handle,
                                                         const char* node)
{
    MVCC_INTVALUE_EX value{};
    const int code = api.get_int_value(handle, node, &value);
    if (code == MV_OK)
    {
        if (value.nMin < 0 || value.nMin > value.nMax || value.nInc <= 0 ||
            value.nMax > std::numeric_limits<std::uint32_t>::max())
        {
            return Result<std::optional<MVCC_INTVALUE_EX>>::failure(
                parameter_error(CameraErrorKind::parameter_read_failed, MV_E_PARAMETER,
                                "camera.hikrobot.capabilities", node, "invalid-integer-range"));
        }
        return Result<std::optional<MVCC_INTVALUE_EX>>::success(value);
    }
    if (optional_node_absent(code))
    {
        return Result<std::optional<MVCC_INTVALUE_EX>>::success(std::nullopt);
    }
    return Result<std::optional<MVCC_INTVALUE_EX>>::failure(
        parameter_error(CameraErrorKind::parameter_read_failed, code,
                        "camera.hikrobot.capabilities", node, "read-failed"));
}

Result<std::optional<std::uint32_t>> optional_integer_current(const MvsApi& api, void* handle,
                                                              const char* node)
{
    MVCC_INTVALUE_EX value{};
    const int code = api.get_int_value(handle, node, &value);
    if (code == MV_OK)
    {
        if (value.nCurValue < 0 || value.nCurValue > std::numeric_limits<std::uint32_t>::max())
        {
            return Result<std::optional<std::uint32_t>>::failure(
                parameter_error(CameraErrorKind::parameter_read_failed, MV_E_PARAMETER,
                                "camera.hikrobot.capabilities", node, "value-out-of-range"));
        }
        return Result<std::optional<std::uint32_t>>::success(
            static_cast<std::uint32_t>(value.nCurValue));
    }
    if (optional_node_absent(code))
        return Result<std::optional<std::uint32_t>>::success(std::nullopt);
    return Result<std::optional<std::uint32_t>>::failure(
        parameter_error(CameraErrorKind::parameter_read_failed, code,
                        "camera.hikrobot.capabilities", node, "read-failed"));
}

Result<std::optional<MVCC_ENUMVALUE>> optional_enumeration(const MvsApi& api, void* handle,
                                                           const char* node)
{
    MVCC_ENUMVALUE value{};
    const int code = api.get_enum_value(handle, node, &value);
    if (code == MV_OK)
    {
        if (value.nSupportedNum > MV_MAX_XML_SYMBOLIC_NUM)
        {
            return Result<std::optional<MVCC_ENUMVALUE>>::failure(
                parameter_error(CameraErrorKind::parameter_read_failed, MV_E_PARAMETER,
                                "camera.hikrobot.capabilities", node, "invalid-enumeration-count"));
        }
        return Result<std::optional<MVCC_ENUMVALUE>>::success(value);
    }
    if (optional_node_absent(code))
    {
        return Result<std::optional<MVCC_ENUMVALUE>>::success(std::nullopt);
    }
    return Result<std::optional<MVCC_ENUMVALUE>>::failure(
        parameter_error(CameraErrorKind::parameter_read_failed, code,
                        "camera.hikrobot.capabilities", node, "read-failed"));
}

Result<bool> supports_optional_boolean(const MvsApi& api, void* handle, const char* node)
{
    bool value{};
    const int code = api.get_bool_value(handle, node, &value);
    if (code == MV_OK)
        return Result<bool>::success(true);
    if (optional_node_absent(code))
        return Result<bool>::success(false);
    return Result<bool>::failure(parameter_error(CameraErrorKind::parameter_read_failed, code,
                                                 "camera.hikrobot.capabilities", node,
                                                 "read-failed"));
}

Result<std::optional<bool>> optional_frame_rate_enable(const MvsApi& api, void* handle,
                                                       const CameraErrorKind error_kind,
                                                       const std::string_view operation)
{
    bool value{};
    const int code = api.get_bool_value(handle, "AcquisitionFrameRateEnable", &value);
    if (code == MV_OK)
        return Result<std::optional<bool>>::success(value);
    // Some camera families expose AcquisitionFrameRate without a separate enable node.
    // Only an explicit "not supported" result is compatible; access/read failures must
    // remain visible to the caller.
    if (code == static_cast<int>(MV_E_SUPPORT))
        return Result<std::optional<bool>>::success(std::nullopt);
    return Result<std::optional<bool>>::failure(
        parameter_error(error_kind, code, operation, "AcquisitionFrameRateEnable", "read-failed"));
}

bool supports(const MVCC_ENUMVALUE& value, const unsigned int item) noexcept
{
    return std::find(value.nSupportValue, value.nSupportValue + value.nSupportedNum, item) !=
           value.nSupportValue + value.nSupportedNum;
}

SteppedRange<std::uint32_t> integer_range(const MVCC_INTVALUE_EX& value)
{
    return {static_cast<std::uint32_t>(value.nMin), static_cast<std::uint32_t>(value.nMax),
            static_cast<std::uint32_t>(value.nInc)};
}

constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000ULL;

Result<std::uint32_t> delay_ticks_to_nanoseconds(const std::uint32_t ticks,
                                                 const std::uint32_t nanoseconds_per_tick,
                                                 const std::string_view operation)
{
    if (nanoseconds_per_tick == 0U ||
        ticks > std::numeric_limits<std::uint32_t>::max() / nanoseconds_per_tick)
    {
        return Result<std::uint32_t>::failure(
            parameter_error(CameraErrorKind::parameter_read_failed, MV_E_PARAMETER, operation,
                            "GevSCPD", "delay-conversion-overflow"));
    }
    return Result<std::uint32_t>::success(ticks * nanoseconds_per_tick);
}

Result<std::uint32_t> delay_nanoseconds_to_ticks(const std::uint32_t nanoseconds,
                                                 const std::uint32_t nanoseconds_per_tick,
                                                 const CameraErrorKind error_kind,
                                                 const std::string_view operation)
{
    if (nanoseconds_per_tick == 0U || nanoseconds % nanoseconds_per_tick != 0U)
    {
        return Result<std::uint32_t>::failure(parameter_error(
            error_kind, MV_E_PARAMETER, operation, "GevSCPD", "nanoseconds-not-aligned-to-tick"));
    }
    return Result<std::uint32_t>::success(nanoseconds / nanoseconds_per_tick);
}

Result<SteppedRange<std::uint32_t>> delay_range_in_nanoseconds(
    const MVCC_INTVALUE_EX& value, const std::uint32_t nanoseconds_per_tick)
{
    const auto minimum =
        delay_ticks_to_nanoseconds(static_cast<std::uint32_t>(value.nMin), nanoseconds_per_tick,
                                   "camera.hikrobot.capabilities");
    const auto maximum =
        delay_ticks_to_nanoseconds(static_cast<std::uint32_t>(value.nMax), nanoseconds_per_tick,
                                   "camera.hikrobot.capabilities");
    const auto increment =
        delay_ticks_to_nanoseconds(static_cast<std::uint32_t>(value.nInc), nanoseconds_per_tick,
                                   "camera.hikrobot.capabilities");
    if (!minimum || !maximum || !increment)
    {
        return Result<SteppedRange<std::uint32_t>>::failure(
            !minimum ? minimum.error() : (!maximum ? maximum.error() : increment.error()));
    }
    return Result<SteppedRange<std::uint32_t>>::success(
        {minimum.value(), maximum.value(), increment.value()});
}

SteppedRange<std::uint32_t> roi_range_with_boundary(const MVCC_INTVALUE_EX& value,
                                                    const std::uint32_t boundary_maximum)
{
    auto result = integer_range(value);
    const auto offset_span =
        boundary_maximum >= result.minimum ? boundary_maximum - result.minimum : 0U;
    result.maximum = result.minimum + offset_span / result.increment * result.increment;
    return result;
}

SteppedRange<std::uint32_t> roi_offset_range(const MVCC_INTVALUE_EX& value,
                                             const std::uint32_t sensor_extent,
                                             const std::uint32_t minimum_roi_extent)
{
    const auto boundary_maximum =
        sensor_extent >= minimum_roi_extent ? sensor_extent - minimum_roi_extent : 0U;
    return roi_range_with_boundary(value, boundary_maximum);
}

std::optional<PixelFormat> map_pixel_format(const unsigned int value) noexcept
{
    switch (value)
    {
    case PixelType_Gvsp_Mono8:
        return PixelFormat::mono8;
    case PixelType_Gvsp_Mono10:
        return PixelFormat::mono10;
    case PixelType_Gvsp_Mono12:
        return PixelFormat::mono12;
    case PixelType_Gvsp_BayerRG8:
        return PixelFormat::bayer_rg8;
    default:
        return std::nullopt;
    }
}

unsigned int native_pixel_format(const PixelFormat value) noexcept
{
    switch (value)
    {
    case PixelFormat::mono8:
        return PixelType_Gvsp_Mono8;
    case PixelFormat::mono10:
        return PixelType_Gvsp_Mono10;
    case PixelFormat::mono12:
        return PixelType_Gvsp_Mono12;
    case PixelFormat::bayer_rg8:
        return PixelType_Gvsp_BayerRG8;
    }
    return 0U;
}

std::optional<ExposureAutoMode> map_exposure_auto_mode(const unsigned int value) noexcept
{
    switch (value)
    {
    case 0U:
        return ExposureAutoMode::off;
    case 1U:
        return ExposureAutoMode::once;
    case 2U:
        return ExposureAutoMode::continuous;
    default:
        return std::nullopt;
    }
}

const char* exposure_auto_mode_symbol(const ExposureAutoMode value) noexcept
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

std::optional<std::string> line_name(const unsigned int value)
{
    if (value > 3U)
    {
        return std::nullopt;
    }
    return "Line" + std::to_string(value);
}

Result<CameraCapabilities> read_capabilities_locked(
    const MvsApi& api, void* handle, std::optional<std::uint32_t>& delay_nanoseconds_per_tick,
    std::optional<std::uint64_t>& timestamp_frequency_hz)
{
    delay_nanoseconds_per_tick.reset();
    timestamp_frequency_hz.reset();
    CameraCapabilities result;
    const auto exposure = optional_float(api, handle, "ExposureTime");
    const auto exposure_auto = optional_enumeration(api, handle, "ExposureAuto");
    const auto gain = optional_float(api, handle, "Gain");
    const auto frame_rate = optional_float(api, handle, "AcquisitionFrameRate");
    if (!exposure || !exposure_auto || !gain || !frame_rate)
        return Result<CameraCapabilities>::failure(
            !exposure ? exposure.error()
                      : (!exposure_auto ? exposure_auto.error()
                                        : (!gain ? gain.error() : frame_rate.error())));
    if (exposure.value())
        result.exposure_us = {exposure.value()->fMin, exposure.value()->fMax, 0.0};
    if (exposure_auto.value())
        for (unsigned int index = 0; index < exposure_auto.value()->nSupportedNum; ++index)
            if (const auto mode =
                    map_exposure_auto_mode(exposure_auto.value()->nSupportValue[index]);
                mode &&
                std::find(result.exposure_auto_modes.begin(), result.exposure_auto_modes.end(),
                          *mode) == result.exposure_auto_modes.end())
                result.exposure_auto_modes.push_back(*mode);
    if (gain.value())
        result.gain_db = {gain.value()->fMin, gain.value()->fMax, 0.0};
    if (frame_rate.value())
        result.frame_rate = {frame_rate.value()->fMin, frame_rate.value()->fMax, 0.0};

    const auto width = optional_integer(api, handle, "Width");
    const auto height = optional_integer(api, handle, "Height");
    const auto offset_x = optional_integer(api, handle, "OffsetX");
    const auto offset_y = optional_integer(api, handle, "OffsetY");
    const auto width_max_node = optional_integer_current(api, handle, "WidthMax");
    const auto height_max_node = optional_integer_current(api, handle, "HeightMax");
    const auto sensor_width_node = optional_integer_current(api, handle, "SensorWidth");
    const auto sensor_height_node = optional_integer_current(api, handle, "SensorHeight");
    if (!width)
        return Result<CameraCapabilities>::failure(width.error());
    if (!height)
        return Result<CameraCapabilities>::failure(height.error());
    if (!offset_x)
        return Result<CameraCapabilities>::failure(offset_x.error());
    if (!offset_y)
        return Result<CameraCapabilities>::failure(offset_y.error());
    if (!width_max_node)
        return Result<CameraCapabilities>::failure(width_max_node.error());
    if (!height_max_node)
        return Result<CameraCapabilities>::failure(height_max_node.error());
    if (!sensor_width_node)
        return Result<CameraCapabilities>::failure(sensor_width_node.error());
    if (!sensor_height_node)
        return Result<CameraCapabilities>::failure(sensor_height_node.error());
    if (width.value() && height.value() && offset_x.value() && offset_y.value())
    {
        // Width/Height 的 nMax 会受当前 OffsetX/OffsetY 以及采集期间的节点锁定状态影响。
        // 优先读取只读的 WidthMax/HeightMax，其次读取 SensorWidth/SensorHeight；旧设备没有
        // 这些节点时才回退到尺寸节点。
        const auto sensor_width = width_max_node.value().value_or(
            sensor_width_node.value().value_or(static_cast<std::uint32_t>(width.value()->nMax)));
        const auto sensor_height = height_max_node.value().value_or(
            sensor_height_node.value().value_or(static_cast<std::uint32_t>(height.value()->nMax)));
        result.roi = {.sensor_width = sensor_width,
                      .sensor_height = sensor_height,
                      .width = roi_range_with_boundary(*width.value(), sensor_width),
                      .height = roi_range_with_boundary(*height.value(), sensor_height),
                      // OffsetX/OffsetY 的 nMax 取决于当前 Width/Height，不能直接当作设备的
                      // 固定能力上限。这里按传感器边界和最小 ROI 尺寸还原完整可用范围，
                      // 具体 Width/Height + Offset 组合仍由通用能力校验器检查。
                      .offset_x = roi_offset_range(*offset_x.value(), sensor_width,
                                                   static_cast<std::uint32_t>(width.value()->nMin)),
                      .offset_y =
                          roi_offset_range(*offset_y.value(), sensor_height,
                                           static_cast<std::uint32_t>(height.value()->nMin))};
    }

    const auto reverse_x = supports_optional_boolean(api, handle, "ReverseX");
    const auto reverse_y = supports_optional_boolean(api, handle, "ReverseY");
    if (!reverse_x || !reverse_y)
        return Result<CameraCapabilities>::failure(!reverse_x ? reverse_x.error()
                                                              : reverse_y.error());
    result.supports_reverse_x = reverse_x.value();
    result.supports_reverse_y = reverse_y.value();

    const auto pixels = optional_enumeration(api, handle, "PixelFormat");
    const auto trigger_mode = optional_enumeration(api, handle, "TriggerMode");
    const auto trigger_source = optional_enumeration(api, handle, "TriggerSource");
    if (!pixels || !trigger_mode || !trigger_source)
        return Result<CameraCapabilities>::failure(
            !pixels ? pixels.error()
                    : (!trigger_mode ? trigger_mode.error() : trigger_source.error()));
    if (pixels.value())
    {
        for (unsigned int index = 0U; index < pixels.value()->nSupportedNum; ++index)
            if (auto mapped = map_pixel_format(pixels.value()->nSupportValue[index]))
                result.pixel_formats.push_back(*mapped);
    }
    if (trigger_mode.value())
    {
        if (supports(*trigger_mode.value(), 0U))
            result.trigger_modes.push_back(TriggerMode::continuous);
        if (supports(*trigger_mode.value(), 1U) && trigger_source.value())
        {
            bool has_hardware{};
            for (unsigned int index = 0U; index < trigger_source.value()->nSupportedNum; ++index)
            {
                const auto source = trigger_source.value()->nSupportValue[index];
                if (source == 7U)
                    result.trigger_modes.push_back(TriggerMode::software);
                else if (auto name = line_name(source))
                {
                    result.trigger_sources.push_back(*name);
                    has_hardware = true;
                }
            }
            if (has_hardware)
                result.trigger_modes.push_back(TriggerMode::hardware);
        }
    }

    const auto trigger_delay = optional_float(api, handle, "TriggerDelay");
    if (!trigger_delay)
        return Result<CameraCapabilities>::failure(trigger_delay.error());
    if (trigger_delay.value() && trigger_delay.value()->fMin >= 0.0F &&
        trigger_delay.value()->fMax <=
            static_cast<float>(std::numeric_limits<std::uint32_t>::max()))
        result.trigger_delay_us = {static_cast<std::uint32_t>(trigger_delay.value()->fMin),
                                   static_cast<std::uint32_t>(trigger_delay.value()->fMax), 1U};

    const auto packet = optional_integer(api, handle, "GevSCPSPacketSize");
    const auto delay = optional_integer(api, handle, "GevSCPD");
    const auto payload = optional_integer(api, handle, "PayloadSize");
    if (!packet || !delay || !payload)
        return Result<CameraCapabilities>::failure(
            !packet ? packet.error() : (!delay ? delay.error() : payload.error()));
    if (packet.value())
        result.packet_size_bytes = integer_range(*packet.value());
    if (delay.value())
    {
        const auto frequency = optional_integer_current(api, handle, "GevTimestampTickFrequency");
        if (!frequency)
            return Result<CameraCapabilities>::failure(frequency.error());
        if (!frequency.value())
            return Result<CameraCapabilities>::failure(
                parameter_error(CameraErrorKind::parameter_read_failed, MV_E_SUPPORT,
                                "camera.hikrobot.capabilities", "GevTimestampTickFrequency",
                                "timestamp-frequency-unavailable"));
        const std::uint64_t frequency_value = *frequency.value();
        if (frequency_value == 0U || frequency_value > nanoseconds_per_second ||
            nanoseconds_per_second % frequency_value != 0U)
        {
            return Result<CameraCapabilities>::failure(
                parameter_error(CameraErrorKind::parameter_read_failed, MV_E_PARAMETER,
                                "camera.hikrobot.capabilities", "GevTimestampTickFrequency",
                                "timestamp-frequency-not-integral-nanoseconds"));
        }
        const auto scale = static_cast<std::uint32_t>(nanoseconds_per_second / frequency_value);
        auto converted_range = delay_range_in_nanoseconds(*delay.value(), scale);
        if (!converted_range)
            return Result<CameraCapabilities>::failure(converted_range.error());
        result.inter_packet_delay_ns = converted_range.value();
        delay_nanoseconds_per_tick = scale;
        timestamp_frequency_hz = frequency_value;
    }
    if (payload.value())
        result.maximum_payload_bytes = static_cast<std::size_t>(payload.value()->nCurValue);

    const auto line_selector = optional_enumeration(api, handle, "LineSelector");
    if (!line_selector)
        return Result<CameraCapabilities>::failure(line_selector.error());
    if (line_selector.value())
    {
        const auto original = line_selector.value()->nCurValue;
        for (unsigned int index = 0U; index < line_selector.value()->nSupportedNum; ++index)
        {
            const auto selector = line_selector.value()->nSupportValue[index];
            const auto name = line_name(selector);
            if (!name || api.set_enum_value(handle, "LineSelector", selector) != MV_OK)
                continue;
            MVCC_ENUMVALUE mode{};
            if (api.get_enum_value(handle, "LineMode", &mode) == MV_OK)
            {
                if (mode.nCurValue <= 1U)
                    result.digital_io.push_back({*name,
                                                 mode.nCurValue == 0U ? DigitalIoDirection::input
                                                                      : DigitalIoDirection::output,
                                                 mode.nCurValue == 1U});
                if (selector == 0U && supports(mode, 0U))
                {
                    result.line_io.alarm_input_supported = true;
                    result.line_io.line0_rising_edge_supported = true;
                    result.line_io.line0_falling_edge_supported = true;
                }
                if (selector == 1U && supports(mode, 8U))
                    result.line_io.strobe_output_supported = true;
            }
        }
        if (result.line_io.strobe_output_supported)
        {
            const int select_strobe = api.set_enum_value(handle, "LineSelector", 1U);
            if (select_strobe != MV_OK)
            {
                static_cast<void>(api.set_enum_value(handle, "LineSelector", original));
                return Result<CameraCapabilities>::failure(parameter_error(
                    CameraErrorKind::parameter_read_failed, select_strobe,
                    "camera.hikrobot.capabilities", "LineSelector", "strobe-selector-failed"));
            }
            const auto duration = optional_integer(api, handle, "StrobeLineDuration");
            const auto pre_delay = optional_integer(api, handle, "StrobeLinePreDelay");
            const auto post_delay = optional_integer(api, handle, "StrobeLineDelay");
            const int restore_code = api.set_enum_value(handle, "LineSelector", original);
            if (restore_code != MV_OK)
                return Result<CameraCapabilities>::failure(parameter_error(
                    CameraErrorKind::parameter_read_failed, restore_code,
                    "camera.hikrobot.capabilities", "LineSelector", "restore-selector-failed"));
            if (!duration || !pre_delay || !post_delay)
                return Result<CameraCapabilities>::failure(
                    !duration ? duration.error()
                              : (!pre_delay ? pre_delay.error() : post_delay.error()));
            if (duration.value())
                result.line_io.strobe_duration_us = integer_range(*duration.value());
            if (pre_delay.value())
                result.line_io.strobe_pre_delay_us = integer_range(*pre_delay.value());
            if (post_delay.value())
                result.line_io.strobe_post_delay_us = integer_range(*post_delay.value());
            if (!result.line_io.strobe_duration_us || !result.line_io.strobe_pre_delay_us ||
                !result.line_io.strobe_post_delay_us)
            {
                result.line_io.strobe_output_supported = false;
                result.line_io.unsupported_reason = "频闪时序节点范围无效或不可读";
            }
        }
        else if (api.set_enum_value(handle, "LineSelector", original) != MV_OK)
            return Result<CameraCapabilities>::failure(parameter_error(
                CameraErrorKind::parameter_read_failed, MV_E_GC_ACCESS,
                "camera.hikrobot.capabilities", "LineSelector", "restore-selector-failed"));
    }
    if (!result.line_io.alarm_input_supported && !result.line_io.strobe_output_supported &&
        result.line_io.unsupported_reason.empty())
        result.line_io.unsupported_reason = "设备不支持固定的 Line 0 输入或 Line 1 频闪模式";
    return Result<CameraCapabilities>::success(std::move(result));
}

template <typename T> Result<T> required_node(const int code, T value, const std::string_view node)
{
    if (code != MV_OK)
        return Result<T>::failure(parameter_error(CameraErrorKind::parameter_read_failed, code,
                                                  "camera.hikrobot.readParameters", node,
                                                  "read-failed"));
    return Result<T>::success(std::move(value));
}

Result<CameraParameterSnapshot> read_parameters_locked(
    const MvsApi& api, void* handle, const CameraCapabilities& capabilities,
    const std::optional<std::uint32_t> delay_nanoseconds_per_tick,
    const bool alarm_events_enabled = false)
{
    CameraParameterSnapshot result;
    auto read_float = [&](const char* node) -> Result<double> {
        MVCC_FLOATVALUE value{};
        return required_node(api.get_float_value(handle, node, &value),
                             static_cast<double>(value.fCurValue), node);
    };
    auto read_int = [&](const char* node) -> Result<std::uint32_t> {
        MVCC_INTVALUE_EX value{};
        const int code = api.get_int_value(handle, node, &value);
        if (code == MV_OK &&
            (value.nCurValue < 0 || value.nCurValue > std::numeric_limits<std::uint32_t>::max()))
            return Result<std::uint32_t>::failure(
                parameter_error(CameraErrorKind::parameter_read_failed, MV_E_PARAMETER,
                                "camera.hikrobot.readParameters", node, "value-out-of-range"));
        return required_node(code, static_cast<std::uint32_t>(value.nCurValue), node);
    };
    if (capabilities.exposure_us)
    {
        auto value = read_float("ExposureTime");
        if (!value)
            return Result<CameraParameterSnapshot>::failure(value.error());
        result.exposure_us = value.value();
    }
    if (!capabilities.exposure_auto_modes.empty())
    {
        MVCC_ENUMVALUE value{};
        const int code = api.get_enum_value(handle, "ExposureAuto", &value);
        const auto mode = map_exposure_auto_mode(value.nCurValue);
        if (code != MV_OK || !mode ||
            std::find(capabilities.exposure_auto_modes.begin(),
                      capabilities.exposure_auto_modes.end(),
                      *mode) == capabilities.exposure_auto_modes.end())
            return Result<CameraParameterSnapshot>::failure(parameter_error(
                CameraErrorKind::parameter_read_failed, code == MV_OK ? MV_E_PARAMETER : code,
                "camera.hikrobot.readParameters", "ExposureAuto", "read-failed"));
        result.exposure_auto_mode = *mode;
    }
    if (capabilities.gain_db)
    {
        auto value = read_float("Gain");
        if (!value)
            return Result<CameraParameterSnapshot>::failure(value.error());
        result.gain_db = value.value();
    }
    if (capabilities.frame_rate)
    {
        auto value = read_float("AcquisitionFrameRate");
        if (!value)
            return Result<CameraParameterSnapshot>::failure(value.error());
        result.frame_rate = value.value();
    }
    if (capabilities.roi)
    {
        auto width = read_int("Width");
        auto height = read_int("Height");
        auto x = read_int("OffsetX");
        auto y = read_int("OffsetY");
        if (!width || !height || !x || !y)
            return Result<CameraParameterSnapshot>::failure(
                !width ? width.error() : (!height ? height.error() : (!x ? x.error() : y.error())));
        result.roi = {width.value(), height.value(), x.value(), y.value()};
    }
    if (capabilities.supports_reverse_x)
    {
        bool value{};
        const int code = api.get_bool_value(handle, "ReverseX", &value);
        if (code != MV_OK)
            return Result<CameraParameterSnapshot>::failure(
                parameter_error(CameraErrorKind::parameter_read_failed, code,
                                "camera.hikrobot.readParameters", "ReverseX", "read-failed"));
        result.reverse_x = value;
    }
    if (capabilities.supports_reverse_y)
    {
        bool value{};
        const int code = api.get_bool_value(handle, "ReverseY", &value);
        if (code != MV_OK)
            return Result<CameraParameterSnapshot>::failure(
                parameter_error(CameraErrorKind::parameter_read_failed, code,
                                "camera.hikrobot.readParameters", "ReverseY", "read-failed"));
        result.reverse_y = value;
    }
    if (!capabilities.pixel_formats.empty())
    {
        MVCC_ENUMVALUE value{};
        const int code = api.get_enum_value(handle, "PixelFormat", &value);
        auto mapped = map_pixel_format(value.nCurValue);
        if (code != MV_OK || !mapped)
            return Result<CameraParameterSnapshot>::failure(parameter_error(
                CameraErrorKind::parameter_read_failed, code == MV_OK ? MV_E_PARAMETER : code,
                "camera.hikrobot.readParameters", "PixelFormat", "unsupported-current-value"));
        result.pixel_format = *mapped;
    }
    if (!capabilities.trigger_modes.empty())
    {
        MVCC_ENUMVALUE mode{};
        int code = api.get_enum_value(handle, "TriggerMode", &mode);
        if (code != MV_OK)
            return Result<CameraParameterSnapshot>::failure(
                parameter_error(CameraErrorKind::parameter_read_failed, code,
                                "camera.hikrobot.readParameters", "TriggerMode", "read-failed"));
        if (mode.nCurValue == 0U)
            result.trigger_mode = TriggerMode::continuous;
        else
        {
            MVCC_ENUMVALUE source{};
            code = api.get_enum_value(handle, "TriggerSource", &source);
            if (code != MV_OK)
                return Result<CameraParameterSnapshot>::failure(parameter_error(
                    CameraErrorKind::parameter_read_failed, code, "camera.hikrobot.readParameters",
                    "TriggerSource", "read-failed"));
            if (source.nCurValue == 7U)
                result.trigger_mode = TriggerMode::software;
            else if (auto name = line_name(source.nCurValue))
            {
                result.trigger_mode = TriggerMode::hardware;
                result.trigger_source = *name;
            }
            else
                return Result<CameraParameterSnapshot>::failure(
                    parameter_error(CameraErrorKind::parameter_read_failed, MV_E_PARAMETER,
                                    "camera.hikrobot.readParameters", "TriggerSource",
                                    "unsupported-current-value"));
        }
    }
    if (capabilities.trigger_delay_us)
    {
        auto value = read_float("TriggerDelay");
        if (!value)
            return Result<CameraParameterSnapshot>::failure(value.error());
        result.trigger_delay_us = static_cast<std::uint32_t>(value.value());
    }
    if (capabilities.packet_size_bytes)
    {
        auto value = read_int("GevSCPSPacketSize");
        if (!value)
            return Result<CameraParameterSnapshot>::failure(value.error());
        result.packet_size_bytes = value.value();
    }
    if (capabilities.inter_packet_delay_ns)
    {
        auto value = read_int("GevSCPD");
        if (!value)
            return Result<CameraParameterSnapshot>::failure(value.error());
        if (!delay_nanoseconds_per_tick)
            return Result<CameraParameterSnapshot>::failure(
                parameter_error(CameraErrorKind::parameter_read_failed, MV_E_PARAMETER,
                                "camera.hikrobot.readParameters", "GevTimestampTickFrequency",
                                "timestamp-frequency-unavailable"));
        auto converted = delay_ticks_to_nanoseconds(value.value(), *delay_nanoseconds_per_tick,
                                                    "camera.hikrobot.readParameters");
        if (!converted)
            return Result<CameraParameterSnapshot>::failure(converted.error());
        result.inter_packet_delay_ns = converted.value();
    }
    for (const auto& line : capabilities.digital_io)
    {
        const auto selector = static_cast<unsigned int>(line.line_id.back() - '0');
        const char* selector_node =
            line.direction == DigitalIoDirection::output ? "UserOutputSelector" : "LineSelector";
        const char* value_node =
            line.direction == DigitalIoDirection::output ? "UserOutputValue" : "LineStatus";
        int code = api.set_enum_value(handle, selector_node, selector);
        bool value{};
        if (code == MV_OK)
            code = api.get_bool_value(handle, value_node, &value);
        if (code != MV_OK)
            return Result<CameraParameterSnapshot>::failure(
                parameter_error(CameraErrorKind::parameter_read_failed, code,
                                "camera.hikrobot.readParameters", value_node, "read-failed"));
        result.digital_io.push_back({line.line_id, value});
    }
    LineIoParameters line_io{.alarm_input_enabled = alarm_events_enabled};
    if (capabilities.line_io.alarm_input_supported)
    {
        int code = api.set_enum_value(handle, "LineSelector", 0U);
        bool raw_level{};
        if (code == MV_OK)
            code = api.get_bool_value(handle, "LineStatus", &raw_level);
        if (code != MV_OK)
            return Result<CameraParameterSnapshot>::failure(parameter_error(
                CameraErrorKind::parameter_read_failed, code, "camera.hikrobot.readParameters",
                "LineStatus", "line0-read-failed"));
        result.line_input = LineInputState{.enabled = alarm_events_enabled,
                                           .raw_level = raw_level,
                                           // A device read is a snapshot, not a new edge event.
                                           // Keep this stable so rollback equality is meaningful.
                                           .timestamp_utc_ms = 0};
    }
    if (capabilities.line_io.strobe_output_supported)
    {
        int code = api.set_enum_value(handle, "LineSelector", 1U);
        bool enabled{};
        if (code == MV_OK)
            code = api.get_bool_value(handle, "StrobeEnable", &enabled);
        if (code != MV_OK)
            return Result<CameraParameterSnapshot>::failure(
                parameter_error(CameraErrorKind::parameter_read_failed, code,
                                "camera.hikrobot.readParameters", "StrobeEnable", "read-failed"));
        auto duration = read_int("StrobeLineDuration");
        auto pre_delay = read_int("StrobeLinePreDelay");
        auto post_delay = read_int("StrobeLineDelay");
        if (!duration || !pre_delay || !post_delay)
            return Result<CameraParameterSnapshot>::failure(
                !duration ? duration.error()
                          : (!pre_delay ? pre_delay.error() : post_delay.error()));
        line_io.strobe_output_enabled = enabled;
        line_io.strobe_duration_us = duration.value();
        line_io.strobe_pre_delay_us = pre_delay.value();
        line_io.strobe_post_delay_us = post_delay.value();
    }
    result.line_io = line_io;
    return Result<CameraParameterSnapshot>::success(std::move(result));
}

Result<void> write_parameters_locked(const MvsApi& api, void* handle,
                                     const CameraParameterSnapshot& parameters,
                                     const CameraCapabilities& capabilities,
                                     const std::optional<std::uint32_t> delay_nanoseconds_per_tick,
                                     const CameraErrorKind error_kind,
                                     const std::optional<bool> frame_rate_enable,
                                     LineEventCallbackBoundary* line_callback,
                                     bool& alarm_events_enabled)
{
    auto check = [&](const int code, const char* node) -> Result<void> {
        if (code == MV_OK)
            return Result<void>::success();
        return Result<void>::failure(parameter_error(
            error_kind, code, "camera.hikrobot.applyParameters", node, "write-failed"));
    };
    if (parameters.roi)
    {
        if (auto r = check(api.set_int_value(handle, "OffsetX", 0), "OffsetX"); !r)
            return r;
        if (auto r = check(api.set_int_value(handle, "OffsetY", 0), "OffsetY"); !r)
            return r;
        if (auto r = check(api.set_int_value(handle, "Width", parameters.roi->width), "Width"); !r)
            return r;
        if (auto r = check(api.set_int_value(handle, "Height", parameters.roi->height), "Height");
            !r)
            return r;
        if (auto r =
                check(api.set_int_value(handle, "OffsetX", parameters.roi->offset_x), "OffsetX");
            !r)
            return r;
        if (auto r =
                check(api.set_int_value(handle, "OffsetY", parameters.roi->offset_y), "OffsetY");
            !r)
            return r;
    }
    if (parameters.reverse_x && capabilities.supports_reverse_x)
        if (auto r =
                check(api.set_bool_value(handle, "ReverseX", *parameters.reverse_x), "ReverseX");
            !r)
            return r;
    if (parameters.reverse_y && capabilities.supports_reverse_y)
        if (auto r =
                check(api.set_bool_value(handle, "ReverseY", *parameters.reverse_y), "ReverseY");
            !r)
            return r;
    if (parameters.pixel_format)
        if (auto r = check(api.set_enum_value(handle, "PixelFormat",
                                              native_pixel_format(*parameters.pixel_format)),
                           "PixelFormat");
            !r)
            return r;
    if (parameters.exposure_us)
    {
        if (auto r =
                check(api.set_enum_value_by_string(handle, "ExposureAuto", "Off"), "ExposureAuto");
            !r)
            return r;
        if (auto r = check(api.set_float_value(handle, "ExposureTime",
                                               static_cast<float>(*parameters.exposure_us)),
                           "ExposureTime");
            !r)
            return r;
    }
    if (parameters.gain_db)
        if (auto r =
                check(api.set_float_value(handle, "Gain", static_cast<float>(*parameters.gain_db)),
                      "Gain");
            !r)
            return r;
    if (parameters.frame_rate)
    {
        if (frame_rate_enable)
            if (auto r = check(api.set_bool_value(handle, "AcquisitionFrameRateEnable", true),
                               "AcquisitionFrameRateEnable");
                !r)
                return r;
        if (auto r = check(api.set_float_value(handle, "AcquisitionFrameRate",
                                               static_cast<float>(*parameters.frame_rate)),
                           "AcquisitionFrameRate");
            !r)
            return r;
        if (frame_rate_enable && !*frame_rate_enable)
            if (auto r = check(api.set_bool_value(handle, "AcquisitionFrameRateEnable", false),
                               "AcquisitionFrameRateEnable");
                !r)
                return r;
    }
    if (parameters.trigger_delay_us)
        if (auto r = check(api.set_float_value(handle, "TriggerDelay",
                                               static_cast<float>(*parameters.trigger_delay_us)),
                           "TriggerDelay");
            !r)
            return r;
    if (parameters.packet_size_bytes)
        if (auto r =
                check(api.set_int_value(handle, "GevSCPSPacketSize", *parameters.packet_size_bytes),
                      "GevSCPSPacketSize");
            !r)
            return r;
    if (parameters.inter_packet_delay_ns)
    {
        if (!delay_nanoseconds_per_tick)
            return Result<void>::failure(
                parameter_error(error_kind, MV_E_PARAMETER, "camera.hikrobot.applyParameters",
                                "GevTimestampTickFrequency", "timestamp-frequency-unavailable"));
        auto ticks = delay_nanoseconds_to_ticks(*parameters.inter_packet_delay_ns,
                                                *delay_nanoseconds_per_tick, error_kind,
                                                "camera.hikrobot.applyParameters");
        if (!ticks)
            return Result<void>::failure(ticks.error());
        if (auto r = check(api.set_int_value(handle, "GevSCPD", ticks.value()), "GevSCPD"); !r)
            return r;
    }
    if (parameters.trigger_mode)
    {
        if (*parameters.trigger_mode == TriggerMode::continuous)
        {
            if (auto r = check(api.set_enum_value(handle, "TriggerMode", 0U), "TriggerMode"); !r)
                return r;
        }
        else
        {
            unsigned int source = 7U;
            if (*parameters.trigger_mode == TriggerMode::hardware)
                source = static_cast<unsigned int>(parameters.trigger_source->back() - '0');
            if (auto r =
                    check(api.set_enum_value(handle, "TriggerSource", source), "TriggerSource");
                !r)
                return r;
            if (auto r = check(api.set_enum_value(handle, "TriggerMode", 1U), "TriggerMode"); !r)
                return r;
        }
    }
    for (const auto& line : parameters.digital_io)
    {
        const auto capability = std::find_if(
            capabilities.digital_io.begin(), capabilities.digital_io.end(),
            [&](const DigitalIoCapability& item) { return item.line_id == line.line_id; });
        if (capability == capabilities.digital_io.end() || !capability->writable)
            continue;
        const auto selector = static_cast<unsigned int>(line.line_id.back() - '0');
        if (auto r = check(api.set_enum_value(handle, "UserOutputSelector", selector),
                           "UserOutputSelector");
            !r)
            return r;
        if (auto r =
                check(api.set_bool_value(handle, "UserOutputValue", line.value), "UserOutputValue");
            !r)
            return r;
    }
    if (parameters.line_io)
    {
        // MVS 的事件订阅 API 接收 EventSelector 的 symbolic（如 ExposureEnd），
        // 不是对应 GenICam 节点名的 "Event" + symbolic 形式。
        constexpr const char* rising_event = "Line0RisingEdge";
        constexpr const char* falling_event = "Line0FallingEdge";
        const auto event_error = [&](const int code, const char* event, const char* reason) {
            return Result<void>::failure(parameter_error(
                error_kind, code, "camera.hikrobot.applyParameters", event, reason));
        };
        const auto disable_events = [&]() -> Result<void> {
            if (!alarm_events_enabled)
                return Result<void>::success();
            const int falling = api.event_notification_off(handle, falling_event);
            const int rising = api.event_notification_off(handle, rising_event);
            if (falling != MV_OK)
                return event_error(falling, falling_event, "notification-off-failed");
            if (rising != MV_OK)
                return event_error(rising, rising_event, "notification-off-failed");
            alarm_events_enabled = false;
            return Result<void>::success();
        };

        if (!parameters.line_io->alarm_input_enabled)
        {
            if (auto disabled = disable_events(); !disabled)
                return disabled;
        }
        else if (capabilities.line_io.alarm_input_supported)
        {
            if (auto selected =
                    check(api.set_enum_value(handle, "LineSelector", 0U), "LineSelector");
                !selected)
                return selected;
            // MV-CS020-60GM 的 Line 0 是固定输入线路。其 LineMode 可读且枚举中包含
            // Input，但节点访问模式为只读；重复写入当前值仍会返回 MV_E_GC_ACCESS。
            // 能力探测已确认当前模式和 Input 支持，因此这里只选择线路并读取电平。
            bool initial{};
            const int initial_code = api.get_bool_value(handle, "LineStatus", &initial);
            if (initial_code != MV_OK)
                return event_error(initial_code, "LineStatus", "initial-read-failed");
            if (!alarm_events_enabled)
            {
                if (line_callback == nullptr)
                    return event_error(MV_E_PARAMETER, "Line0", "missing-callback-boundary");
                int code = api.register_event_callback(
                    handle, rising_event, &line_event_callback_trampoline, line_callback);
                if (code != MV_OK)
                    return event_error(code, rising_event, "register-failed");
                code = api.register_event_callback(handle, falling_event,
                                                   &line_event_callback_trampoline, line_callback);
                if (code != MV_OK)
                    return event_error(code, falling_event, "register-failed");
                code = api.event_notification_on(handle, rising_event);
                if (code != MV_OK)
                    return event_error(code, rising_event, "notification-on-failed");
                code = api.event_notification_on(handle, falling_event);
                if (code != MV_OK)
                {
                    static_cast<void>(api.event_notification_off(handle, rising_event));
                    return event_error(code, falling_event, "notification-on-failed");
                }
                alarm_events_enabled = true;
            }
        }

        if (capabilities.line_io.strobe_output_supported)
        {
            if (auto selected =
                    check(api.set_enum_value(handle, "LineSelector", 1U), "LineSelector");
                !selected)
                return selected;
            if (!parameters.line_io->strobe_output_enabled)
            {
                if (auto disabled =
                        check(api.set_bool_value(handle, "StrobeEnable", false), "StrobeEnable");
                    !disabled)
                    return disabled;
            }
            else
            {
                if (auto mode = check(api.set_enum_value(handle, "LineMode", 8U), "LineMode");
                    !mode)
                    return mode;
                if (auto source = check(
                        api.set_enum_value_by_string(handle, "LineSource", "ExposureStartActive"),
                        "LineSource");
                    !source)
                    return source;
                if (auto duration = check(api.set_int_value(handle, "StrobeLineDuration",
                                                            parameters.line_io->strobe_duration_us),
                                          "StrobeLineDuration");
                    !duration)
                    return duration;
                if (auto pre = check(api.set_int_value(handle, "StrobeLinePreDelay",
                                                       parameters.line_io->strobe_pre_delay_us),
                                     "StrobeLinePreDelay");
                    !pre)
                    return pre;
                if (auto post = check(api.set_int_value(handle, "StrobeLineDelay",
                                                        parameters.line_io->strobe_post_delay_us),
                                      "StrobeLineDelay");
                    !post)
                    return post;
                if (auto enabled =
                        check(api.set_bool_value(handle, "StrobeEnable", true), "StrobeEnable");
                    !enabled)
                    return enabled;
            }
        }
    }
    if (parameters.exposure_auto_mode)
        if (auto r = check(api.set_enum_value_by_string(
                               handle, "ExposureAuto",
                               exposure_auto_mode_symbol(*parameters.exposure_auto_mode)),
                           "ExposureAuto");
            !r)
            return r;
    return Result<void>::success();
}
} // namespace

struct DeviceHandle::State final
{
    State(const MvsApi& api_value, void* handle_value) noexcept
        : api(api_value), handle(handle_value), line_callback([this](const bool raw_level) {
              LineInputObserver observer;
              {
                  std::scoped_lock lock{observer_mutex};
                  observer = line_input_observer;
              }
              if (observer)
                  observer(
                      {.raw_level = raw_level,
                       .timestamp_utc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::system_clock::now().time_since_epoch())
                                               .count()});
          })
    {
    }

    ~State() noexcept
    {
        std::scoped_lock lock{mutex};
        cleanup_locked();
    }

    Result<void> stop_locked() noexcept
    {
        if (!streaming)
        {
            return Result<void>::success();
        }
        const int code = api.stop_grabbing(handle);
        if (code != MV_OK)
        {
            return Result<void>::failure(translate_mvs_error(CameraErrorKind::stream_start_failed,
                                                             code, "camera.hikrobot.stopGrabbing",
                                                             "MVS 停止取流失败"));
        }
        streaming = false;
        return Result<void>::success();
    }

    Error faulted_error(const std::string_view operation) const
    {
        return make_camera_error(CameraErrorKind::parameter_faulted,
                                 "相机参数状态无法恢复，必须断开并重新连接", std::string{operation},
                                 std::nullopt, {{"reason", "parameter-rollback-failed"}});
    }

    Result<CameraCapabilities> capabilities_locked()
    {
        if (parameter_faulted)
            return Result<CameraCapabilities>::failure(
                faulted_error("camera.hikrobot.capabilities"));
        std::optional<std::uint32_t> delay_nanoseconds_per_tick;
        std::optional<std::uint64_t> frequency;
        auto capabilities =
            read_capabilities_locked(api, handle, delay_nanoseconds_per_tick, frequency);
        if (capabilities && frequency)
        {
            timestamp_frequency_hz = *frequency;
            timestamp_frequency_checked = true;
        }
        return capabilities;
    }

    Result<CameraParameterSnapshot> parameters_locked()
    {
        if (parameter_faulted)
            return Result<CameraParameterSnapshot>::failure(
                faulted_error("camera.hikrobot.readParameters"));
        std::optional<std::uint32_t> delay_nanoseconds_per_tick;
        std::optional<std::uint64_t> frequency;
        auto capabilities =
            read_capabilities_locked(api, handle, delay_nanoseconds_per_tick, frequency);
        if (!capabilities)
            return Result<CameraParameterSnapshot>::failure(capabilities.error());
        if (frequency)
        {
            timestamp_frequency_hz = *frequency;
            timestamp_frequency_checked = true;
        }
        return read_parameters_locked(api, handle, capabilities.value(), delay_nanoseconds_per_tick,
                                      alarm_events_enabled);
    }

    Result<CameraParameterSnapshot> apply_locked(const CameraParameterSnapshot& parameters)
    {
        if (parameter_faulted)
            return Result<CameraParameterSnapshot>::failure(
                faulted_error("camera.hikrobot.applyParameters"));
        std::optional<std::uint32_t> delay_nanoseconds_per_tick;
        std::optional<std::uint64_t> frequency;
        auto capabilities =
            read_capabilities_locked(api, handle, delay_nanoseconds_per_tick, frequency);
        if (!capabilities)
            return Result<CameraParameterSnapshot>::failure(capabilities.error());
        if (frequency)
        {
            timestamp_frequency_hz = *frequency;
            timestamp_frequency_checked = true;
        }
        if (auto validation = validate_parameters(capabilities.value(), parameters); !validation)
            return Result<CameraParameterSnapshot>::failure(validation.error());
        auto old = read_parameters_locked(api, handle, capabilities.value(),
                                          delay_nanoseconds_per_tick, alarm_events_enabled);
        if (!old)
            return Result<CameraParameterSnapshot>::failure(old.error());
        std::optional<bool> old_frame_rate_enable;
        if (old.value().frame_rate)
        {
            auto enabled =
                optional_frame_rate_enable(api, handle, CameraErrorKind::parameter_read_failed,
                                           "camera.hikrobot.applyParameters");
            if (!enabled)
                return Result<CameraParameterSnapshot>::failure(enabled.error());
            old_frame_rate_enable = enabled.value();
        }
        const std::optional<bool> requested_frame_rate_enable =
            parameters.frame_rate && old_frame_rate_enable ? std::optional<bool>{true}
                                                           : old_frame_rate_enable;

        const bool resume = streaming;
        if (resume)
        {
            auto stopped = stop_locked();
            if (!stopped)
                return Result<CameraParameterSnapshot>::failure(stopped.error());
        }

        auto restore_or_fault = [&](Error original) -> Result<CameraParameterSnapshot> {
            auto restored = write_parameters_locked(
                api, handle, old.value(), capabilities.value(), delay_nanoseconds_per_tick,
                CameraErrorKind::parameter_faulted, old_frame_rate_enable, &line_callback,
                alarm_events_enabled);
            if (restored)
            {
                auto confirmed =
                    read_parameters_locked(api, handle, capabilities.value(),
                                           delay_nanoseconds_per_tick, alarm_events_enabled);
                if (!confirmed || confirmed.value() != old.value())
                    restored =
                        Result<void>::failure(faulted_error("camera.hikrobot.rollbackParameters"));
                if (restored && old_frame_rate_enable)
                {
                    auto enabled =
                        optional_frame_rate_enable(api, handle, CameraErrorKind::parameter_faulted,
                                                   "camera.hikrobot.rollbackParameters");
                    if (!enabled || enabled.value() != old_frame_rate_enable)
                        restored = Result<void>::failure(
                            faulted_error("camera.hikrobot.rollbackParameters"));
                }
            }
            if (resume)
            {
                const int code = api.start_grabbing(handle);
                if (code == MV_OK)
                    streaming = true;
                else
                    restored = Result<void>::failure(
                        parameter_error(CameraErrorKind::parameter_faulted, code,
                                        "camera.hikrobot.rollbackParameters", "Acquisition",
                                        "resume-after-rollback-failed"));
            }
            if (!restored)
            {
                parameter_faulted = true;
                return Result<CameraParameterSnapshot>::failure(
                    faulted_error("camera.hikrobot.applyParameters"));
            }
            return Result<CameraParameterSnapshot>::failure(std::move(original));
        };

        auto written = write_parameters_locked(
            api, handle, parameters, capabilities.value(), delay_nanoseconds_per_tick,
            CameraErrorKind::parameter_write_failed, requested_frame_rate_enable, &line_callback,
            alarm_events_enabled);
        if (!written)
            return restore_or_fault(written.error());
        auto actual = read_parameters_locked(api, handle, capabilities.value(),
                                             delay_nanoseconds_per_tick, alarm_events_enabled);
        if (!actual)
            return restore_or_fault(actual.error());
        if (parameters.frame_rate && requested_frame_rate_enable)
        {
            auto enabled =
                optional_frame_rate_enable(api, handle, CameraErrorKind::parameter_read_failed,
                                           "camera.hikrobot.applyParameters");
            if (!enabled)
                return restore_or_fault(enabled.error());
            if (enabled.value() != requested_frame_rate_enable)
                return restore_or_fault(
                    parameter_error(CameraErrorKind::parameter_read_failed, MV_E_PARAMETER,
                                    "camera.hikrobot.applyParameters", "AcquisitionFrameRateEnable",
                                    "readback-mismatch"));
        }

        if (resume)
        {
            const int code = api.start_grabbing(handle);
            if (code != MV_OK)
                return restore_or_fault(parameter_error(CameraErrorKind::stream_start_failed, code,
                                                        "camera.hikrobot.resumeAfterParameters",
                                                        "Acquisition", "resume-failed"));
            streaming = true;
        }
        return actual;
    }

    Result<void> software_trigger_locked()
    {
        if (parameter_faulted)
            return Result<void>::failure(faulted_error("camera.hikrobot.softwareTrigger"));

        MVCC_ENUMVALUE trigger_mode{};
        MVCC_ENUMVALUE trigger_source{};
        const int mode_code = api.get_enum_value(handle, "TriggerMode", &trigger_mode);
        if (mode_code != MV_OK)
            return Result<void>::failure(parameter_error(
                CameraErrorKind::parameter_read_failed, mode_code,
                "camera.hikrobot.softwareTrigger", "TriggerMode", "read-before-command-failed"));
        const int source_code = api.get_enum_value(handle, "TriggerSource", &trigger_source);
        if (source_code != MV_OK)
            return Result<void>::failure(parameter_error(
                CameraErrorKind::parameter_read_failed, source_code,
                "camera.hikrobot.softwareTrigger", "TriggerSource", "read-before-command-failed"));
        constexpr unsigned int trigger_mode_on = 1U;
        constexpr unsigned int trigger_source_software = 7U;
        if (trigger_mode.nCurValue != trigger_mode_on ||
            trigger_source.nCurValue != trigger_source_software)
        {
            return Result<void>::failure(make_camera_error(
                CameraErrorKind::invalid_state_transition, "相机实际触发配置不是软件触发模式",
                "camera.hikrobot.softwareTrigger", std::nullopt,
                {{"reason", "actual-trigger-mode-mismatch"}}));
        }
        const int command_code = api.set_command_value(handle, "TriggerSoftware");
        if (command_code != MV_OK)
            return Result<void>::failure(parameter_error(
                CameraErrorKind::parameter_write_failed, command_code,
                "camera.hikrobot.softwareTrigger", "TriggerSoftware", "command-failed"));
        return Result<void>::success();
    }

    Result<void> close_locked() noexcept
    {
        if (handle == nullptr)
        {
            return Result<void>::success();
        }
        if (alarm_events_enabled)
        {
            const int falling = api.event_notification_off(handle, "Line0FallingEdge");
            const int rising = api.event_notification_off(handle, "Line0RisingEdge");
            if (falling != MV_OK || rising != MV_OK)
                return Result<void>::failure(parameter_error(
                    CameraErrorKind::parameter_write_failed, falling != MV_OK ? falling : rising,
                    "camera.hikrobot.closeDevice", "Line0", "notification-off-failed"));
            alarm_events_enabled = false;
        }
        if (const auto stopped = stop_locked(); !stopped)
        {
            return stopped;
        }
        if (opened)
        {
            const int close_code = api.close_device(handle);
            if (close_code != MV_OK)
            {
                return Result<void>::failure(
                    translate_mvs_error(CameraErrorKind::open_failed, close_code,
                                        "camera.hikrobot.closeDevice", "MVS 关闭设备失败"));
            }
            opened = false;
        }
        const int destroy_code = api.destroy_handle(handle);
        if (destroy_code != MV_OK)
        {
            return Result<void>::failure(
                translate_mvs_error(CameraErrorKind::open_failed, destroy_code,
                                    "camera.hikrobot.destroyHandle", "MVS 销毁设备句柄失败"));
        }
        handle = nullptr;
        return Result<void>::success();
    }

    void cleanup_locked() noexcept
    {
        if (handle == nullptr)
        {
            return;
        }
        if (alarm_events_enabled)
        {
            static_cast<void>(api.event_notification_off(handle, "Line0FallingEdge"));
            static_cast<void>(api.event_notification_off(handle, "Line0RisingEdge"));
            alarm_events_enabled = false;
        }
        if (streaming)
        {
            static_cast<void>(api.stop_grabbing(handle));
            streaming = false;
        }
        if (opened)
        {
            static_cast<void>(api.close_device(handle));
            opened = false;
        }
        static_cast<void>(api.destroy_handle(handle));
        handle = nullptr;
    }

    const MvsApi& api;
    void* handle{};
    bool opened{};
    bool streaming{};
    bool parameter_faulted{};
    bool alarm_events_enabled{};
    std::optional<std::uint64_t> timestamp_frequency_hz;
    bool timestamp_frequency_checked{};
    std::mutex mutex;
    std::mutex observer_mutex;
    LineInputObserver line_input_observer;
    LineEventCallbackBoundary line_callback;
};

Result<DeviceHandle> DeviceHandle::open(const MvsApi& api, const MV_CC_DEVICE_INFO& device_info)
{
    void* handle = nullptr;
    const int create_code = api.create_handle(&handle, &device_info);
    if (create_code != MV_OK || handle == nullptr)
    {
        return Result<DeviceHandle>::failure(
            translate_mvs_error(CameraErrorKind::open_failed, create_code,
                                "camera.hikrobot.createHandle", "MVS 创建设备句柄失败"));
    }

    auto state = std::make_shared<State>(api, handle);
    const int open_code = api.open_device(handle, MV_ACCESS_Exclusive, 0U);
    if (open_code != MV_OK)
    {
        return Result<DeviceHandle>::failure(
            translate_mvs_error(classify_open_error(open_code), open_code,
                                "camera.hikrobot.openDevice", "MVS 打开设备失败"));
    }
    state->opened = true;
    return Result<DeviceHandle>::success(DeviceHandle{std::move(state)});
}

DeviceHandle::DeviceHandle(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

Result<StreamSession> DeviceHandle::start_streaming()
{
    if (!state_)
    {
        return Result<StreamSession>::failure(
            translate_mvs_error(CameraErrorKind::stream_start_failed, MV_E_HANDLE,
                                "camera.hikrobot.startGrabbing", "MVS 设备句柄无效"));
    }
    std::scoped_lock lock{state_->mutex};
    if (state_->streaming)
    {
        return Result<StreamSession>::failure(
            translate_mvs_error(CameraErrorKind::stream_start_failed, MV_E_CALLORDER,
                                "camera.hikrobot.startGrabbing", "MVS 设备已经处于取流状态"));
    }
    MVCC_ENUMVALUE exposure_auto{};
    const int exposure_code =
        state_->api.get_enum_value(state_->handle, "ExposureAuto", &exposure_auto);
    if (exposure_code != MV_OK || !map_exposure_auto_mode(exposure_auto.nCurValue))
    {
        return Result<StreamSession>::failure(parameter_error(
            CameraErrorKind::stream_start_failed,
            exposure_code == MV_OK ? MV_E_PARAMETER : exposure_code,
            "camera.hikrobot.startGrabbing", "ExposureAuto", "unconfirmed-auto-exposure-mode"));
    }
    const int code = state_->api.start_grabbing(state_->handle);
    if (code != MV_OK)
    {
        return Result<StreamSession>::failure(
            translate_mvs_error(CameraErrorKind::stream_start_failed, code,
                                "camera.hikrobot.startGrabbing", "MVS 启动取流失败"));
    }
    state_->streaming = true;
    return Result<StreamSession>::success(StreamSession{state_});
}

Result<void> DeviceHandle::software_trigger()
{
    if (!state_)
    {
        return Result<void>::failure(
            translate_mvs_error(CameraErrorKind::parameter_write_failed, MV_E_HANDLE,
                                "camera.hikrobot.softwareTrigger", "MVS 设备句柄无效"));
    }
    std::scoped_lock lock{state_->mutex};
    return state_->software_trigger_locked();
}

Result<CameraClockSample> DeviceHandle::sample_clock(
    const std::stop_token stop_token, const std::chrono::steady_clock::time_point deadline)
{
    const auto cancelled = [&] {
        return stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline;
    };
    const auto unavailable = [](std::string message, const std::string_view reason,
                                const std::optional<int> native = std::nullopt) {
        auto error = make_error("TIME_PROBE_UNAVAILABLE", Severity::warning, std::move(message),
                                "camera.hikrobot", "camera.hikrobot.sampleClock", true);
        error.details.push_back({"reason", std::string{reason}});
        if (native)
        {
            error.native_domain = "hikrobot-mvs";
            error.native_code = native_code_text(*native);
        }
        return error;
    };
    if (!state_)
        return Result<CameraClockSample>::failure(
            unavailable("MVS 设备句柄无效", "invalid-handle"));
    if (cancelled())
        return Result<CameraClockSample>::failure(
            unavailable("相机时间采样已取消或超过截止时间", "cancelled-or-deadline"));

    std::scoped_lock lock{state_->mutex};
    if (cancelled())
        return Result<CameraClockSample>::failure(
            unavailable("相机时间采样已取消或超过截止时间", "cancelled-or-deadline"));

    const int latch_code =
        state_->api.set_command_value(state_->handle, "GevTimestampControlLatch");
    if (latch_code != MV_OK)
    {
        if (latch_code == static_cast<int>(MV_E_SUPPORT))
        {
            auto error = make_error("TIME_PROBE_NOT_SUPPORTED", Severity::warning,
                                    "MVS 相机不支持时间戳锁存", "camera.hikrobot",
                                    "camera.hikrobot.sampleClock");
            error.native_domain = "hikrobot-mvs";
            error.native_code = native_code_text(latch_code);
            error.details.push_back({"reason", "timestamp-latch-not-supported"});
            return Result<CameraClockSample>::failure(std::move(error));
        }
        return Result<CameraClockSample>::failure(
            unavailable("MVS 无法锁存相机时间戳", "timestamp-latch-failed", latch_code));
    }
    if (cancelled())
        return Result<CameraClockSample>::failure(
            unavailable("相机时间采样已取消或超过截止时间", "cancelled-or-deadline"));

    MVCC_INTVALUE_EX ticks_value{};
    MVCC_INTVALUE_EX frequency_value{};
    const int ticks_code =
        state_->api.get_int_value(state_->handle, "GevTimestampValue", &ticks_value);
    const int frequency_code =
        state_->api.get_int_value(state_->handle, "GevTimestampTickFrequency", &frequency_value);
    if (ticks_code != MV_OK || frequency_code != MV_OK || ticks_value.nCurValue == 0U ||
        frequency_value.nCurValue == 0U)
        return Result<CameraClockSample>::failure(
            unavailable("MVS 相机时间戳或频率不可用", "timestamp-value-unavailable",
                        ticks_code != MV_OK ? ticks_code : frequency_code));

    bool ptp_enabled{};
    const int ptp_code = state_->api.get_bool_value(state_->handle, "GevIEEE1588", &ptp_enabled);
    const bool ptp_supported = ptp_code == MV_OK;
    bool ptp_synchronized{};
    std::optional<std::int64_t> offset_ns;
    std::optional<std::string> last_error;
    if (!ptp_supported)
    {
        ptp_enabled = false;
        last_error = "TIME_PROBE_NOT_SUPPORTED";
    }
    else if (!ptp_enabled)
    {
        last_error = "TIME_SYNC_DEGRADED";
    }
    else if (cancelled())
    {
        return Result<CameraClockSample>::failure(
            unavailable("相机时间采样已取消或超过截止时间", "cancelled-or-deadline"));
    }
    else
    {
        MVCC_ENUMVALUE status{};
        const int status_code =
            state_->api.get_enum_value(state_->handle, "GevIEEE1588Status", &status);
        // GigE Vision IEEE 1588 status value 8 is Slave. Master/PreMaster is never accepted.
        ptp_synchronized = status_code == MV_OK && status.nCurValue == 8U;
        if (status_code != MV_OK)
            last_error = "TIME_PROBE_UNAVAILABLE";
        else if (!ptp_synchronized)
            last_error = "TIME_SYNC_DEGRADED";

        MVCC_INTVALUE_EX offset{};
        if (state_->api.get_int_value(state_->handle, "GevIEEE1588OffsetFromMaster", &offset) ==
            MV_OK)
            offset_ns = static_cast<std::int64_t>(offset.nCurValue);
    }

    const auto sample_monotonic_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count();
    const auto sample_utc_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
    const auto frequency = static_cast<std::uint64_t>(frequency_value.nCurValue);
    const auto tick_resolution_ns =
        static_cast<std::int64_t>((1'000'000'000ULL + frequency - 1U) / frequency);
    std::int64_t uncertainty_ns = 50'000'000;
    if (ptp_synchronized && offset_ns)
    {
        const auto magnitude = *offset_ns == (std::numeric_limits<std::int64_t>::min)()
                                   ? (std::numeric_limits<std::int64_t>::max)()
                                   : std::abs(*offset_ns);
        uncertainty_ns = std::max(magnitude, tick_resolution_ns);
    }
    return Result<CameraClockSample>::success(
        {.camera_timestamp_ticks = static_cast<std::uint64_t>(ticks_value.nCurValue),
         .camera_timestamp_frequency_hz = frequency,
         .sample_monotonic_ns = sample_monotonic_ns,
         .sample_utc_ns = sample_utc_ns,
         .hardware_ptp_supported = ptp_supported,
         .hardware_ptp_enabled = ptp_enabled,
         .hardware_ptp_synchronized = ptp_synchronized,
         .offset_ns = offset_ns,
         .uncertainty_ns = uncertainty_ns,
         .maximum_observed_offset_ns =
             offset_ns ? std::optional<std::int64_t>{uncertainty_ns} : std::nullopt,
         .last_synchronized_utc_ns =
             ptp_synchronized ? std::optional<std::int64_t>{sample_utc_ns} : std::nullopt,
         .grandmaster_identity = std::nullopt,
         .last_error_code = std::move(last_error)});
}

namespace
{
Result<PixelFormat> map_frame_pixel_format(const MvGvspPixelType format)
{
    switch (format)
    {
    case PixelType_Gvsp_Mono8:
        return Result<PixelFormat>::success(PixelFormat::mono8);
    case PixelType_Gvsp_Mono10:
        return Result<PixelFormat>::success(PixelFormat::mono10);
    case PixelType_Gvsp_Mono12:
        return Result<PixelFormat>::success(PixelFormat::mono12);
    case PixelType_Gvsp_BayerRG8:
        return Result<PixelFormat>::success(PixelFormat::bayer_rg8);
    default:
        return Result<PixelFormat>::failure(make_camera_error(
            CameraErrorKind::frame_format_changed, "MVS 返回了不支持的像素格式",
            "camera.hikrobot.capture", std::nullopt,
            {{"reason", "unsupported-pixel-format"},
             {"vendorPixelFormat", std::to_string(static_cast<std::uint32_t>(format))}}));
    }
}

std::uint32_t minimum_stride(const std::uint32_t width, const PixelFormat format) noexcept
{
    const std::uint32_t bytes_per_pixel =
        format == PixelFormat::mono10 || format == PixelFormat::mono12 ? 2U : 1U;
    if (width > std::numeric_limits<std::uint32_t>::max() / bytes_per_pixel)
    {
        return 0U;
    }
    return width * bytes_per_pixel;
}
} // namespace

Result<CapturedFrameMetadata> DeviceHandle::capture_into(FrameBuffer& destination,
                                                         const std::chrono::milliseconds timeout)
{
    if (!state_)
    {
        return Result<CapturedFrameMetadata>::failure(make_camera_error(
            CameraErrorKind::disconnected, "MVS 设备句柄无效", "camera.hikrobot.capture"));
    }
    if (timeout <= std::chrono::milliseconds::zero() ||
        timeout.count() > std::numeric_limits<unsigned int>::max() ||
        destination.capacity() > std::numeric_limits<unsigned int>::max())
    {
        return Result<CapturedFrameMetadata>::failure(make_camera_error(
            CameraErrorKind::config_failed, "MVS 取流缓冲或超时参数无效", "camera.hikrobot.capture",
            std::nullopt, {{"reason", "invalid-capture-limits"}}));
    }

    std::scoped_lock lock{state_->mutex};
    if (!state_->streaming)
    {
        return Result<CapturedFrameMetadata>::failure(make_camera_error(
            CameraErrorKind::invalid_state_transition, "MVS 尚未开始取流",
            "camera.hikrobot.capture", std::nullopt, {{"reason", "not-streaming"}}));
    }

    if (!state_->timestamp_frequency_checked)
    {
        MVCC_INTVALUE_EX frequency{};
        if (state_->api.get_int_value(state_->handle, "GevTimestampTickFrequency", &frequency) ==
                MV_OK &&
            frequency.nCurValue > 0U)
        {
            state_->timestamp_frequency_hz = frequency.nCurValue;
        }
        state_->timestamp_frequency_checked = true;
    }

    destination.clear();
    MV_FRAME_OUT_INFO_EX info{};
    const int code = state_->api.get_one_frame_timeout(
        state_->handle, reinterpret_cast<unsigned char*>(destination.writable_bytes().data()),
        static_cast<unsigned int>(destination.capacity()), &info,
        static_cast<unsigned int>(timeout.count()));
    if (code != MV_OK)
    {
        const CameraErrorKind kind = code == static_cast<int>(MV_E_NODATA)
                                         ? CameraErrorKind::frame_timeout
                                         : CameraErrorKind::disconnected;
        auto error = translate_mvs_error(
            kind, code, "camera.hikrobot.capture",
            kind == CameraErrorKind::frame_timeout ? "MVS 等待图像帧超时" : "MVS 取流中断");
        error.details = {{"timeoutMs", std::to_string(timeout.count())}};
        return Result<CapturedFrameMetadata>::failure(std::move(error));
    }

    const std::uint32_t width = info.nExtendWidth != 0U ? info.nExtendWidth : info.nWidth;
    const std::uint32_t height = info.nExtendHeight != 0U ? info.nExtendHeight : info.nHeight;
    const std::uint64_t payload = info.nFrameLenEx != 0U ? info.nFrameLenEx : info.nFrameLen;
    auto pixel_format = map_frame_pixel_format(info.enPixelType);
    if (!pixel_format)
    {
        return Result<CapturedFrameMetadata>::failure(pixel_format.error());
    }
    if (width == 0U || height == 0U || payload == 0U || payload > destination.capacity() ||
        payload > std::numeric_limits<std::size_t>::max() || payload % height != 0U ||
        payload / height > std::numeric_limits<std::uint32_t>::max())
    {
        return Result<CapturedFrameMetadata>::failure(
            make_camera_error(CameraErrorKind::frame_format_changed, "MVS 帧几何或有效载荷无效",
                              "camera.hikrobot.capture", std::nullopt,
                              {{"reason", "invalid-frame-layout"},
                               {"width", std::to_string(width)},
                               {"height", std::to_string(height)},
                               {"payloadBytes", std::to_string(payload)},
                               {"bufferCapacityBytes", std::to_string(destination.capacity())}}));
    }
    const auto stride = static_cast<std::uint32_t>(payload / height);
    if (const auto minimum = minimum_stride(width, pixel_format.value());
        minimum == 0U || stride < minimum)
    {
        return Result<CapturedFrameMetadata>::failure(make_camera_error(
            CameraErrorKind::frame_format_changed, "MVS 帧步长与像素格式不一致",
            "camera.hikrobot.capture", std::nullopt,
            {{"reason", "stride-too-small"}, {"stride", std::to_string(stride)}}));
    }
    if (!destination.set_size(static_cast<std::size_t>(payload)))
    {
        return Result<CapturedFrameMetadata>::failure(make_camera_error(
            CameraErrorKind::frame_format_changed, "MVS 帧超出预分配缓冲",
            "camera.hikrobot.capture", std::nullopt, {{"reason", "buffer-too-small"}}));
    }

    std::optional<CameraTimestamp> timestamp;
    const std::uint64_t ticks =
        (static_cast<std::uint64_t>(info.nDevTimeStampHigh) << 32U) | info.nDevTimeStampLow;
    if (ticks != 0U && state_->timestamp_frequency_hz)
    {
        timestamp = CameraTimestamp{ticks, *state_->timestamp_frequency_hz,
                                    CameraTimestampQuality::unsynchronized};
    }
    return Result<CapturedFrameMetadata>::success({.camera_frame_number = info.nFrameNum,
                                                   .camera_timestamp = timestamp,
                                                   .geometry = {width, height, stride},
                                                   .pixel_format = pixel_format.value(),
                                                   .flags = {.incomplete = info.nLostPacket > 0U}});
}

Result<CameraCapabilities> DeviceHandle::capabilities()
{
    if (!state_)
        return Result<CameraCapabilities>::failure(make_camera_error(
            CameraErrorKind::invalid_state_transition, "MVS 设备句柄无效",
            "camera.hikrobot.capabilities", std::nullopt, {{"reason", "invalid-handle"}}));
    std::scoped_lock lock{state_->mutex};
    return state_->capabilities_locked();
}

Result<CameraParameterSnapshot> DeviceHandle::read_parameters()
{
    if (!state_)
        return Result<CameraParameterSnapshot>::failure(make_camera_error(
            CameraErrorKind::invalid_state_transition, "MVS 设备句柄无效",
            "camera.hikrobot.readParameters", std::nullopt, {{"reason", "invalid-handle"}}));
    std::scoped_lock lock{state_->mutex};
    return state_->parameters_locked();
}

Result<CameraParameterSnapshot> DeviceHandle::apply_parameters(
    const CameraParameterSnapshot& parameters)
{
    if (!state_)
        return Result<CameraParameterSnapshot>::failure(make_camera_error(
            CameraErrorKind::invalid_state_transition, "MVS 设备句柄无效",
            "camera.hikrobot.applyParameters", std::nullopt, {{"reason", "invalid-handle"}}));
    std::scoped_lock lock{state_->mutex};
    return state_->apply_locked(parameters);
}

void DeviceHandle::set_line_input_observer(LineInputObserver observer)
{
    if (!state_)
        return;
    std::scoped_lock lock{state_->observer_mutex};
    state_->line_input_observer = std::move(observer);
}

Result<void> DeviceHandle::close() noexcept
{
    if (!state_)
    {
        return Result<void>::success();
    }
    std::scoped_lock lock{state_->mutex};
    return state_->close_locked();
}

void* DeviceHandle::native_handle() const noexcept
{
    return state_ ? state_->handle : nullptr;
}

StreamSession::StreamSession(std::shared_ptr<DeviceHandle::State> state) noexcept
    : state_(std::move(state))
{
}

StreamSession::StreamSession(StreamSession&& other) noexcept : state_(std::move(other.state_)) {}

StreamSession& StreamSession::operator=(StreamSession&& other) noexcept
{
    if (this != &other)
    {
        static_cast<void>(stop());
        state_ = std::move(other.state_);
    }
    return *this;
}

StreamSession::~StreamSession() noexcept
{
    static_cast<void>(stop());
}

Result<void> StreamSession::stop() noexcept
{
    if (!state_)
    {
        return Result<void>::success();
    }
    std::scoped_lock lock{state_->mutex};
    auto result = state_->stop_locked();
    if (result)
    {
        state_.reset();
    }
    return result;
}

bool StreamSession::active() const noexcept
{
    if (!state_)
    {
        return false;
    }
    std::scoped_lock lock{state_->mutex};
    return state_->streaming;
}

LineEventCallbackBoundary::LineEventCallbackBoundary(Handler handler) : handler_(std::move(handler))
{
    if (!handler_)
        throw std::invalid_argument{"line event callback handler must not be empty"};
}

CallbackDiagnostics LineEventCallbackBoundary::diagnostics() const noexcept
{
    return {.invocations = invocations_.load(std::memory_order_relaxed),
            .failures = failures_.load(std::memory_order_relaxed),
            .last_failure = last_failure_.load(std::memory_order_relaxed)};
}

void LineEventCallbackBoundary::invoke(MV_EVENT_OUT_INFO* event_info) noexcept
{
    invocations_.fetch_add(1U, std::memory_order_relaxed);
    if (event_info == nullptr)
    {
        failures_.fetch_add(1U, std::memory_order_relaxed);
        last_failure_.store(CallbackFailure::unknown_exception, std::memory_order_relaxed);
        return;
    }
    try
    {
        const auto end =
            std::find(std::begin(event_info->EventName), std::end(event_info->EventName), '\0');
        const std::string_view name{
            event_info->EventName,
            static_cast<std::size_t>(end - std::begin(event_info->EventName))};
        if (name == "Line0RisingEdge")
            handler_(true);
        else if (name == "Line0FallingEdge")
            handler_(false);
        else
            throw std::invalid_argument{"unexpected line event"};
    }
    catch (const std::exception&)
    {
        failures_.fetch_add(1U, std::memory_order_relaxed);
        last_failure_.store(CallbackFailure::standard_exception, std::memory_order_relaxed);
    }
    catch (...)
    {
        failures_.fetch_add(1U, std::memory_order_relaxed);
        last_failure_.store(CallbackFailure::unknown_exception, std::memory_order_relaxed);
    }
}

void __stdcall line_event_callback_trampoline(MV_EVENT_OUT_INFO* event_info, void* user) noexcept
{
    if (user != nullptr)
        static_cast<LineEventCallbackBoundary*>(user)->invoke(event_info);
}

ImageCallbackBoundary::ImageCallbackBoundary(Handler handler) : handler_(std::move(handler))
{
    if (!handler_)
    {
        throw std::invalid_argument{"image callback handler must not be empty"};
    }
}

CallbackDiagnostics ImageCallbackBoundary::diagnostics() const noexcept
{
    return {.invocations = invocations_.load(std::memory_order_relaxed),
            .failures = failures_.load(std::memory_order_relaxed),
            .last_failure = last_failure_.load(std::memory_order_relaxed)};
}

void ImageCallbackBoundary::invoke(unsigned char* data, MV_FRAME_OUT_INFO_EX* frame_info) noexcept
{
    invocations_.fetch_add(1U, std::memory_order_relaxed);
    if (data == nullptr || frame_info == nullptr)
    {
        failures_.fetch_add(1U, std::memory_order_relaxed);
        last_failure_.store(CallbackFailure::unknown_exception, std::memory_order_relaxed);
        return;
    }
    try
    {
        handler_(std::span<const std::byte>{reinterpret_cast<const std::byte*>(data),
                                            frame_info->nFrameLen},
                 *frame_info);
    }
    catch (const std::exception&)
    {
        failures_.fetch_add(1U, std::memory_order_relaxed);
        last_failure_.store(CallbackFailure::standard_exception, std::memory_order_relaxed);
    }
    catch (...)
    {
        failures_.fetch_add(1U, std::memory_order_relaxed);
        last_failure_.store(CallbackFailure::unknown_exception, std::memory_order_relaxed);
    }
}

void __stdcall image_callback_trampoline(unsigned char* data, MV_FRAME_OUT_INFO_EX* frame_info,
                                         void* user) noexcept
{
    if (user == nullptr)
    {
        return;
    }
    static_cast<ImageCallbackBoundary*>(user)->invoke(data, frame_info);
}

} // namespace paperbreak::camera::hikrobot::detail

namespace paperbreak::camera::hikrobot
{
std::unique_ptr<ICameraProvider> create_hikrobot_camera_provider()
{
    return std::make_unique<detail::HikrobotCameraProvider>(detail::production_mvs_api());
}
} // namespace paperbreak::camera::hikrobot
