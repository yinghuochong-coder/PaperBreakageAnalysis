#include "paperbreak/camera/control.hpp"
#include "paperbreak/camera/frame.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>

namespace paperbreak::camera
{
struct CameraControlRuntime::Session final
{
    std::string id;
    std::string serial;
    CameraControlState state{CameraControlState::disconnected};
    std::unique_ptr<ICameraDevice> device;
    std::optional<Error> error;
};
namespace
{
constexpr std::size_t maximum_discovered_devices = 64U;
constexpr std::size_t maximum_operator_snapshot_bytes = 256U * 1024U * 1024U;

Error unsupported(std::string op)
{
    return make_error("SYS_NOT_SUPPORTED", Severity::warning, "相机设备提供者尚未装配", "camera",
                      std::move(op));
}
} // namespace
CameraControlRuntime::CameraControlRuntime(std::shared_ptr<ICameraProvider> p)
    : provider_(std::move(p))
{
}
CameraControlRuntime::~CameraControlRuntime()
{
    std::scoped_lock lock{mutex_};
    for (auto& session : sessions_)
    {
        if (!session->device)
            continue;
        if (session->state == CameraControlState::acquiring)
            static_cast<void>(session->device->stop_acquisition());
        static_cast<void>(session->device->disconnect());
        session->device.reset();
        session->state = CameraControlState::disconnected;
    }
}
std::string_view camera_control_state_name(CameraControlState s) noexcept
{
    switch (s)
    {
    case CameraControlState::disconnected:
        return "disconnected";
    case CameraControlState::connected:
        return "connected";
    case CameraControlState::acquiring:
        return "acquiring";
    }
    return "unknown";
}
Result<CameraControlRuntime::Session*> CameraControlRuntime::find(std::string_view id)
{
    auto i = std::find_if(sessions_.begin(), sessions_.end(),
                          [id](const auto& s) { return s->id == id; });
    if (i == sessions_.end())
        return Result<Session*>::failure(make_camera_error(
            CameraErrorKind::not_found, "逻辑相机未连接", "camera.control.find", std::string{id}));
    return Result<Session*>::success(i->get());
}
Result<CameraControlSnapshot> CameraControlRuntime::read(Session& s)
{
    CameraControlSnapshot r{
        .camera_id = s.id, .serial_number = s.serial, .state = s.state, .last_error = s.error};
    if (!s.device)
        return Result<CameraControlSnapshot>::success(std::move(r));
    r.device = s.device->descriptor();
    auto caps = s.device->capabilities();
    if (!caps)
    {
        s.error = caps.error();
        r.last_error = s.error;
        return Result<CameraControlSnapshot>::success(std::move(r));
    }
    r.capabilities = std::move(caps).value();
    auto actual = s.device->read_parameters();
    if (!actual)
    {
        s.error = actual.error();
        r.last_error = s.error;
        return Result<CameraControlSnapshot>::success(std::move(r));
    }
    s.error.reset();
    r.last_error.reset();
    r.actual = std::move(actual).value();
    return Result<CameraControlSnapshot>::success(std::move(r));
}
Result<std::vector<CameraDeviceDescriptor>> CameraControlRuntime::discover()
{
    std::scoped_lock l{mutex_};
    if (!provider_)
        return Result<std::vector<CameraDeviceDescriptor>>::failure(unsupported("camera.discover"));
    auto r = provider_->enumerate_devices();
    if (!r)
        return r;
    if (r.value().size() > maximum_discovered_devices)
        return Result<std::vector<CameraDeviceDescriptor>>::failure(
            make_camera_error(CameraErrorKind::config_failed, "发现的相机数量超过安全上限",
                              "camera.control.discover"));
    auto v = validate_device_inventory(r.value());
    if (!v)
        return Result<std::vector<CameraDeviceDescriptor>>::failure(v.error());
    return r;
}
Result<CameraControlSnapshot> CameraControlRuntime::get(std::string_view id,
                                                        std::string_view serial)
{
    std::scoped_lock l{mutex_};
    auto s = find(id);
    if (!s)
        return Result<CameraControlSnapshot>::success(
            {.camera_id = std::string{id}, .serial_number = std::string{serial}});
    return read(*s.value());
}
Result<CameraControlSnapshot> CameraControlRuntime::connect(std::string_view id,
                                                            std::string_view serial)
{
    std::scoped_lock l{mutex_};
    if (!provider_)
        return Result<CameraControlSnapshot>::failure(unsupported("camera.connect"));
    auto old = find(id);
    if (old)
    {
        if (old.value()->serial != serial)
            return Result<CameraControlSnapshot>::failure(make_camera_error(
                CameraErrorKind::config_failed, "逻辑相机已连接到不同序列号的设备",
                "camera.control.connect", std::string{id}));
        return read(*old.value());
    }
    if (sessions_.size() >= 4U)
        return Result<CameraControlSnapshot>::failure(make_camera_error(
            CameraErrorKind::config_failed, "相机数量超过四路", "camera.control.connect"));
    auto device = provider_->create_device(serial);
    if (!device)
        return Result<CameraControlSnapshot>::failure(device.error());
    auto open = device.value()->connect();
    if (!open)
        return Result<CameraControlSnapshot>::failure(open.error());
    auto s = std::make_unique<Session>();
    s->id = id;
    s->serial = serial;
    s->state = CameraControlState::connected;
    s->device = std::move(device).value();
    sessions_.push_back(std::move(s));
    return read(*sessions_.back());
}
Result<CameraControlSnapshot> CameraControlRuntime::disconnect(std::string_view id)
{
    std::scoped_lock l{mutex_};
    auto s = find(id);
    if (!s)
        return Result<CameraControlSnapshot>::failure(s.error());
    auto* x = s.value();
    if (x->state == CameraControlState::acquiring)
    {
        auto r = x->device->stop_acquisition();
        if (!r)
            return Result<CameraControlSnapshot>::failure(r.error());
    }
    auto r = x->device->disconnect();
    if (!r)
        return Result<CameraControlSnapshot>::failure(r.error());
    CameraControlSnapshot result{
        .camera_id = x->id, .serial_number = x->serial, .state = CameraControlState::disconnected};
    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                   [id](const auto& item) { return item->id == id; }),
                    sessions_.end());
    return Result<CameraControlSnapshot>::success(std::move(result));
}
Result<CameraControlSnapshot> CameraControlRuntime::start(std::string_view id)
{
    std::scoped_lock l{mutex_};
    auto s = find(id);
    if (!s)
        return Result<CameraControlSnapshot>::failure(s.error());
    auto r = s.value()->device->start_acquisition();
    if (!r)
        return Result<CameraControlSnapshot>::failure(r.error());
    s.value()->state = CameraControlState::acquiring;
    return read(*s.value());
}
Result<CameraControlSnapshot> CameraControlRuntime::stop(std::string_view id)
{
    std::scoped_lock l{mutex_};
    auto s = find(id);
    if (!s)
        return Result<CameraControlSnapshot>::failure(s.error());
    auto r = s.value()->device->stop_acquisition();
    if (!r)
        return Result<CameraControlSnapshot>::failure(r.error());
    s.value()->state = CameraControlState::connected;
    return read(*s.value());
}
Result<CameraControlSnapshot> CameraControlRuntime::update(std::string_view id,
                                                           const CameraParameterSnapshot& p)
{
    std::scoped_lock l{mutex_};
    auto s = find(id);
    if (!s)
        return Result<CameraControlSnapshot>::failure(s.error());
    auto r = apply_validated_parameters(*s.value()->device, p);
    if (!r)
        return Result<CameraControlSnapshot>::failure(r.error());
    return read(*s.value());
}
Result<CapturedFrameMetadata> CameraControlRuntime::capture_snapshot(std::string_view id)
{
    std::scoped_lock l{mutex_};
    auto s = find(id);
    if (!s)
        return Result<CapturedFrameMetadata>::failure(s.error());
    auto caps = s.value()->device->capabilities();
    if (!caps)
        return Result<CapturedFrameMetadata>::failure(caps.error());
    if (caps.value().maximum_payload_bytes == 0U ||
        caps.value().maximum_payload_bytes > maximum_operator_snapshot_bytes)
        return Result<CapturedFrameMetadata>::failure(
            make_camera_error(CameraErrorKind::config_failed, "快照缓冲区大小超出安全范围",
                              "camera.control.captureSnapshot", std::string{id}));
    FrameBuffer buffer{caps.value().maximum_payload_bytes};
    return s.value()->device->capture_into(buffer, std::chrono::milliseconds{1000});
}
Result<void> CameraControlRuntime::software_trigger(std::string_view id)
{
    std::scoped_lock l{mutex_};
    auto s = find(id);
    if (!s)
        return Result<void>::failure(s.error());
    return s.value()->device->software_trigger();
}
} // namespace paperbreak::camera
