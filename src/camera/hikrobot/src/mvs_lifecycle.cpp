#include "mvs_lifecycle.hpp"

#include <MvErrorDefine.h>

#include <algorithm>
#include <exception>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
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
} // namespace

const MvsApi& production_mvs_api() noexcept
{
    static const MvsApi api{.get_sdk_version = &MV_CC_GetSDKVersion,
                            .enumerate_devices = &MV_CC_EnumDevices,
                            .create_handle = &MV_CC_CreateHandle,
                            .open_device = &MV_CC_OpenDevice,
                            .close_device = &MV_CC_CloseDevice,
                            .destroy_handle = &MV_CC_DestroyHandle,
                            .start_grabbing = &MV_CC_StartGrabbing,
                            .stop_grabbing = &MV_CC_StopGrabbing};
    return api;
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

struct DeviceHandle::State final
{
    State(const MvsApi& api_value, void* handle_value) noexcept
        : api(api_value), handle(handle_value)
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

    Result<void> close_locked() noexcept
    {
        if (handle == nullptr)
        {
            return Result<void>::success();
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
    std::mutex mutex;
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
