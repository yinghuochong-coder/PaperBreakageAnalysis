#include "paperbreak/camera/control.hpp"
#include "paperbreak/camera/acquisition.hpp"
#include "paperbreak/camera/frame.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <thread>
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
    std::unique_ptr<FrameBufferPool> frame_pool;
    std::unique_ptr<AcquisitionQueue> acquisition_queue;
    std::unique_ptr<AcquisitionWorker> acquisition;
    std::jthread frame_forwarder;
};
namespace
{
constexpr std::size_t maximum_discovered_devices = 64U;
constexpr std::size_t maximum_operator_snapshot_bytes = 256U * 1024U * 1024U;
constexpr auto preview_forward_timeout = std::chrono::milliseconds{50};
constexpr auto preview_shutdown_timeout = std::chrono::seconds{2};

Error unsupported(std::string op)
{
    return make_error("SYS_NOT_SUPPORTED", Severity::warning, "相机设备提供者尚未装配", "camera",
                      std::move(op));
}
} // namespace
CameraControlRuntime::CameraControlRuntime(std::shared_ptr<ICameraProvider> p,
                                           CameraFrameObserver frame_observer,
                                           CameraFrameDeliveryOptions delivery_options)
    : provider_(std::move(p)), frame_observer_(std::move(frame_observer)),
      delivery_options_(delivery_options)
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
        {
            static_cast<void>(stop_frame_delivery(*session));
            static_cast<void>(session->device->stop_acquisition());
        }
        static_cast<void>(session->device->disconnect());
        session->device.reset();
        session->state = CameraControlState::disconnected;
    }
}

Result<void> CameraControlRuntime::start_frame_delivery(Session& session)
{
    if (!frame_observer_)
        return Result<void>::success();
    if (delivery_options_.frame_pool_capacity == 0U || delivery_options_.queue_capacity == 0U ||
        delivery_options_.queue_capacity > delivery_options_.frame_pool_capacity ||
        delivery_options_.receive_timeout <= std::chrono::milliseconds::zero())
        return Result<void>::failure(
            make_camera_error(CameraErrorKind::config_failed, "相机帧投递容量配置无效",
                              "camera.control.startFrameDelivery", session.id));

    auto capabilities = session.device->capabilities();
    if (!capabilities)
        return Result<void>::failure(capabilities.error());
    const auto payload_bytes = capabilities.value().maximum_payload_bytes;
    if (payload_bytes == 0U || payload_bytes > maximum_operator_snapshot_bytes)
    {
        return Result<void>::failure(
            make_camera_error(CameraErrorKind::config_failed, "预览采集缓冲区大小超出安全范围",
                              "camera.control.startFrameDelivery", session.id));
    }

    try
    {
        session.frame_pool =
            std::make_unique<FrameBufferPool>(delivery_options_.frame_pool_capacity, payload_bytes);
        session.acquisition_queue =
            std::make_unique<AcquisitionQueue>(delivery_options_.queue_capacity);
        session.acquisition = std::make_unique<AcquisitionWorker>(
            *session.device, *session.frame_pool, *session.acquisition_queue,
            AcquisitionWorkerOptions{.camera_id = session.id,
                                     .receive_timeout = delivery_options_.receive_timeout,
                                     .statistics_window = std::chrono::seconds{1},
                                     .consecutive_timeout_limit =
                                         std::numeric_limits<std::size_t>::max()});
        auto started = session.acquisition->start();
        if (!started)
        {
            session.acquisition.reset();
            session.acquisition_queue.reset();
            session.frame_pool.reset();
            return started;
        }
        session.frame_forwarder = std::jthread(
            [this, &session](const std::stop_token token) { forward_frames(session, token); });
    }
    catch (const std::exception&)
    {
        if (session.acquisition)
        {
            session.acquisition->request_stop();
            static_cast<void>(session.acquisition->join(std::chrono::steady_clock::now() +
                                                        preview_shutdown_timeout));
        }
        if (session.acquisition_queue)
            session.acquisition_queue->close();
        if (session.frame_pool)
            session.frame_pool->close();
        session.acquisition.reset();
        session.acquisition_queue.reset();
        session.frame_pool.reset();
        return Result<void>::failure(
            make_camera_error(CameraErrorKind::stream_start_failed, "无法创建相机预览取帧通道",
                              "camera.control.startFrameDelivery", session.id));
    }
    return Result<void>::success();
}

Result<void> CameraControlRuntime::stop_frame_delivery(Session& session)
{
    if (session.acquisition)
        session.acquisition->request_stop();
    if (session.acquisition_queue)
        session.acquisition_queue->close();
    if (session.frame_forwarder.joinable())
        session.frame_forwarder.request_stop();

    Result<void> joined = Result<void>::success();
    if (session.acquisition)
    {
        joined =
            session.acquisition->join(std::chrono::steady_clock::now() + preview_shutdown_timeout);
    }
    if (session.frame_forwarder.joinable())
        session.frame_forwarder.join();
    if (session.frame_pool)
        session.frame_pool->close();
    session.acquisition.reset();
    session.acquisition_queue.reset();
    session.frame_pool.reset();
    return joined;
}

void CameraControlRuntime::forward_frames(Session& session,
                                          const std::stop_token stop_token) noexcept
{
    while (!stop_token.stop_requested())
    {
        auto dequeued = session.acquisition_queue->wait_pop(stop_token, preview_forward_timeout);
        if (dequeued.status == FrameDequeueStatus::stopped ||
            dequeued.status == FrameDequeueStatus::closed)
        {
            break;
        }
        if (dequeued.status == FrameDequeueStatus::timeout)
        {
            if (session.acquisition->snapshot().completed)
                break;
            continue;
        }
        if (!dequeued.packet)
            continue;
        auto frame = make_frame_view(*dequeued.packet);
        if (!frame)
            continue;
        try
        {
            frame_observer_(std::move(frame).value());
        }
        catch (...)
        {
        }
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
    if (s.acquisition)
        r.acquisition = s.acquisition->snapshot();
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
        auto delivery = stop_frame_delivery(*x);
        if (!delivery)
            return Result<CameraControlSnapshot>::failure(delivery.error());
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
    auto snapshot = read(*s.value());
    auto delivery = start_frame_delivery(*s.value());
    if (!delivery)
    {
        static_cast<void>(s.value()->device->stop_acquisition());
        s.value()->state = CameraControlState::connected;
        return Result<CameraControlSnapshot>::failure(delivery.error());
    }
    return snapshot;
}
Result<CameraControlSnapshot> CameraControlRuntime::stop(std::string_view id)
{
    std::scoped_lock l{mutex_};
    auto s = find(id);
    if (!s)
        return Result<CameraControlSnapshot>::failure(s.error());
    auto delivery = stop_frame_delivery(*s.value());
    if (!delivery)
        return Result<CameraControlSnapshot>::failure(delivery.error());
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
    const bool resume_acquisition = s.value()->state == CameraControlState::acquiring;
    if (resume_acquisition)
    {
        auto delivery = stop_frame_delivery(*s.value());
        if (!delivery)
            return Result<CameraControlSnapshot>::failure(delivery.error());
        auto stopped = s.value()->device->stop_acquisition();
        if (!stopped)
        {
            static_cast<void>(start_frame_delivery(*s.value()));
            return Result<CameraControlSnapshot>::failure(stopped.error());
        }
        s.value()->state = CameraControlState::connected;
    }
    auto r = apply_validated_parameters(*s.value()->device, p);
    Result<void> resumed = Result<void>::success();
    if (resume_acquisition)
    {
        resumed = s.value()->device->start_acquisition();
        if (resumed)
        {
            s.value()->state = CameraControlState::acquiring;
            resumed = start_frame_delivery(*s.value());
            if (!resumed)
            {
                static_cast<void>(s.value()->device->stop_acquisition());
                s.value()->state = CameraControlState::connected;
            }
        }
    }
    if (!resumed)
        return Result<CameraControlSnapshot>::failure(resumed.error());
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
