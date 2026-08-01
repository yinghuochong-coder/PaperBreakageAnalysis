#pragma once

#include "paperbreak/camera/camera.hpp"

#include <MvCameraControl.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace paperbreak::camera::hikrobot::detail
{

struct MvsApi final
{
    unsigned int(__stdcall* get_sdk_version)();
    int(__stdcall* enumerate_devices)(unsigned int, MV_CC_DEVICE_INFO_LIST*);
    int(__stdcall* create_handle)(void**, const MV_CC_DEVICE_INFO*);
    int(__stdcall* open_device)(void*, unsigned int, unsigned short);
    int(__stdcall* close_device)(void*);
    int(__stdcall* destroy_handle)(void*);
    int(__stdcall* start_grabbing)(void*);
    int(__stdcall* stop_grabbing)(void*);
};

[[nodiscard]] const MvsApi& production_mvs_api() noexcept;
[[nodiscard]] Error translate_mvs_error(CameraErrorKind kind, int native_code,
                                        std::string operation, std::string message);

class DeviceList final
{
  public:
    [[nodiscard]] static Result<DeviceList> enumerate(const MvsApi& api,
                                                      unsigned int transport_types);

    DeviceList(DeviceList&& other) noexcept = default;
    DeviceList& operator=(DeviceList&& other) noexcept = default;
    DeviceList(const DeviceList&) = delete;
    DeviceList& operator=(const DeviceList&) = delete;
    ~DeviceList() noexcept = default;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const MV_CC_DEVICE_INFO* at(std::size_t index) const noexcept;

  private:
    explicit DeviceList(const MV_CC_DEVICE_INFO_LIST& list);

    std::vector<MV_CC_DEVICE_INFO> devices_;
};

class StreamSession;

class DeviceHandle final
{
  public:
    [[nodiscard]] static Result<DeviceHandle> open(const MvsApi& api,
                                                   const MV_CC_DEVICE_INFO& device_info);

    DeviceHandle(DeviceHandle&&) noexcept = default;
    DeviceHandle& operator=(DeviceHandle&&) noexcept = default;
    DeviceHandle(const DeviceHandle&) = delete;
    DeviceHandle& operator=(const DeviceHandle&) = delete;
    ~DeviceHandle() = default;

    [[nodiscard]] Result<StreamSession> start_streaming();
    [[nodiscard]] Result<void> close() noexcept;
    [[nodiscard]] void* native_handle() const noexcept;

  private:
    friend class StreamSession;
    struct State;
    explicit DeviceHandle(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> state_;
};

class StreamSession final
{
  public:
    StreamSession(StreamSession&& other) noexcept;
    StreamSession& operator=(StreamSession&& other) noexcept;
    StreamSession(const StreamSession&) = delete;
    StreamSession& operator=(const StreamSession&) = delete;
    ~StreamSession() noexcept;

    [[nodiscard]] Result<void> stop() noexcept;
    [[nodiscard]] bool active() const noexcept;

  private:
    friend class DeviceHandle;
    explicit StreamSession(std::shared_ptr<DeviceHandle::State> state) noexcept;

    std::shared_ptr<DeviceHandle::State> state_;
};

enum class CallbackFailure : std::uint8_t
{
    none,
    standard_exception,
    unknown_exception,
};

struct CallbackDiagnostics final
{
    std::uint64_t invocations{};
    std::uint64_t failures{};
    CallbackFailure last_failure{CallbackFailure::none};
};

class ImageCallbackBoundary final
{
  public:
    using Handler = std::function<void(std::span<const std::byte>, const MV_FRAME_OUT_INFO_EX&)>;

    explicit ImageCallbackBoundary(Handler handler);

    [[nodiscard]] CallbackDiagnostics diagnostics() const noexcept;
    void invoke(unsigned char* data, MV_FRAME_OUT_INFO_EX* frame_info) noexcept;

  private:
    Handler handler_;
    std::atomic_uint64_t invocations_{};
    std::atomic_uint64_t failures_{};
    std::atomic<CallbackFailure> last_failure_{CallbackFailure::none};
};

void __stdcall image_callback_trampoline(unsigned char* data, MV_FRAME_OUT_INFO_EX* frame_info,
                                         void* user) noexcept;

} // namespace paperbreak::camera::hikrobot::detail
