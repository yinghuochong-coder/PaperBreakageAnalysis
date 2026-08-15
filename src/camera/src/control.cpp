#include "paperbreak/camera/control.hpp"
#include "paperbreak/camera/acquisition.hpp"
#include "paperbreak/camera/frame.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
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
    std::atomic<CameraControlState> state{CameraControlState::disconnected};
    std::mutex operation_mutex;
    std::mutex cache_mutex;
    std::unique_ptr<ICameraDevice> device;
    std::optional<CameraDeviceDescriptor> descriptor;
    std::optional<CameraCapabilities> capabilities;
    std::optional<CameraParameterSnapshot> actual;
    std::optional<Error> error;
    std::unique_ptr<FrameBufferPool> frame_pool;
    std::unique_ptr<AcquisitionQueue> acquisition_queue;
    std::shared_ptr<AcquisitionWorker> acquisition;
    std::jthread frame_forwarder;
    std::size_t line_input_slot{};
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
                                           CameraFrameDeliveryOptions delivery_options,
                                           CameraLineInputObserver line_input_observer)
    : provider_(std::move(p)), frame_observer_(std::move(frame_observer)),
      delivery_options_(delivery_options), line_input_observer_(std::move(line_input_observer))
{
    line_input_dispatcher_ =
        std::jthread([this](const std::stop_token token) { dispatch_line_inputs(token); });
}
CameraControlRuntime::~CameraControlRuntime()
{
    {
        std::scoped_lock lock{mutex_};
        for (auto& session : sessions_)
        {
            std::scoped_lock operation_lock{session->operation_mutex};
            if (!session->device)
                continue;
            if (session->state.load(std::memory_order_acquire) == CameraControlState::acquiring)
            {
                static_cast<void>(stop_frame_delivery(*session));
                static_cast<void>(session->device->stop_acquisition());
            }
            static_cast<void>(session->device->disconnect());
            session->device.reset();
            session->state.store(CameraControlState::disconnected, std::memory_order_release);
        }
    }
    if (line_input_dispatcher_.joinable())
    {
        line_input_dispatcher_.request_stop();
        line_input_condition_.notify_all();
        line_input_dispatcher_.join();
    }
}

void CameraControlRuntime::enqueue_line_input(const std::size_t slot, const bool raw_level,
                                              const std::int64_t timestamp_utc_ms) noexcept
{
    if (slot >= line_input_slots_.size())
        return;
    {
        std::scoped_lock lock{line_input_mutex_};
        const auto revision = ++line_input_revisions_[slot];
        line_input_slots_[slot] = {
            .raw_level = raw_level, .revision = revision, .timestamp_utc_ms = timestamp_utc_ms};
    }
    line_input_condition_.notify_one();
}

void CameraControlRuntime::dispatch_line_inputs(const std::stop_token stop_token) noexcept
{
    while (!stop_token.stop_requested())
    {
        std::array<std::optional<LineInputEvent>, camera_slot_count> pending;
        std::array<std::string, camera_slot_count> pending_camera_ids;
        {
            std::unique_lock lock{line_input_mutex_};
            line_input_condition_.wait(lock, stop_token, [this] {
                return std::ranges::any_of(line_input_slots_,
                                           [](const auto& item) { return item.has_value(); });
            });
            if (stop_token.stop_requested())
                break;
            pending.swap(line_input_slots_);
            pending_camera_ids = line_input_camera_ids_;
        }
        for (std::size_t slot = 0U; slot < pending.size(); ++slot)
        {
            if (!pending[slot])
                continue;
            const std::string& camera_id = pending_camera_ids[slot];
            Session* session{};
            {
                std::scoped_lock lock{mutex_};
                auto found = find(camera_id);
                if (found)
                    session = found.value();
            }
            const bool connected = session && session->state.load(std::memory_order_acquire) !=
                                                  CameraControlState::disconnected;
            if (connected)
            {
                std::scoped_lock lock{session->cache_mutex};
                if (session->actual)
                    session->actual->line_input =
                        LineInputState{.enabled = true,
                                       .raw_level = pending[slot]->raw_level,
                                       .revision = pending[slot]->revision,
                                       .timestamp_utc_ms = pending[slot]->timestamp_utc_ms};
            }
            try
            {
                if (connected && line_input_observer_ && !camera_id.empty())
                    line_input_observer_(camera_id, *pending[slot]);
            }
            catch (...)
            {
            }
        }
    }
}

Result<void> CameraControlRuntime::prepare_frame_delivery(Session& session)
{
    if (!frame_observer_)
        return Result<void>::success();
    if (delivery_options_.frame_pool_capacity == 0U || delivery_options_.queue_capacity == 0U ||
        delivery_options_.queue_capacity > delivery_options_.frame_pool_capacity ||
        delivery_options_.receive_timeout <= std::chrono::milliseconds::zero())
        return Result<void>::failure(
            make_camera_error(CameraErrorKind::config_failed, "相机帧投递容量配置无效",
                              "camera.control.prepareFrameDelivery", session.id));

    std::optional<CameraCapabilities> cached_capabilities;
    {
        std::scoped_lock lock{session.cache_mutex};
        cached_capabilities = session.capabilities;
    }
    if (!cached_capabilities)
        return Result<void>::failure(
            make_camera_error(CameraErrorKind::invalid_state_transition, "相机能力缓存不可用",
                              "camera.control.prepareFrameDelivery", session.id));
    const auto payload_bytes = cached_capabilities->maximum_payload_bytes;
    if (payload_bytes == 0U || payload_bytes > maximum_operator_snapshot_bytes)
    {
        return Result<void>::failure(
            make_camera_error(CameraErrorKind::config_failed, "预览采集缓冲区大小超出安全范围",
                              "camera.control.prepareFrameDelivery", session.id));
    }

    try
    {
        session.frame_pool =
            std::make_unique<FrameBufferPool>(delivery_options_.frame_pool_capacity, payload_bytes);
        session.acquisition_queue =
            std::make_unique<AcquisitionQueue>(delivery_options_.queue_capacity);
        auto acquisition = std::make_shared<AcquisitionWorker>(
            *session.device, *session.frame_pool, *session.acquisition_queue,
            AcquisitionWorkerOptions{
                .camera_id = session.id,
                .receive_timeout = delivery_options_.receive_timeout,
                .statistics_window = std::chrono::seconds{1},
                .consecutive_timeout_limit = std::numeric_limits<std::size_t>::max(),
                .clock_model_provider =
                    delivery_options_.clock_model_provider
                        ? [provider = delivery_options_.clock_model_provider,
                           camera_id = session.id] { return provider(camera_id); }
                        : std::function<std::shared_ptr<const time::ClockModelSnapshot>()>{},
                .register_thread = delivery_options_.register_thread,
                .diagnostics = delivery_options_.diagnostics});
        {
            std::scoped_lock lock{session.cache_mutex};
            session.acquisition = std::move(acquisition);
        }
    }
    catch (const std::exception&)
    {
        {
            std::scoped_lock lock{session.cache_mutex};
            session.acquisition.reset();
        }
        session.acquisition_queue.reset();
        session.frame_pool.reset();
        return Result<void>::failure(
            make_camera_error(CameraErrorKind::stream_start_failed, "无法创建相机预览取帧通道",
                              "camera.control.prepareFrameDelivery", session.id));
    }
    return Result<void>::success();
}

Result<void> CameraControlRuntime::start_frame_delivery(Session& session)
{
    if (!frame_observer_)
        return Result<void>::success();
    std::shared_ptr<AcquisitionWorker> acquisition;
    {
        std::scoped_lock lock{session.cache_mutex};
        acquisition = session.acquisition;
    }
    if (!acquisition || !session.acquisition_queue || !session.frame_pool)
        return Result<void>::failure(
            make_camera_error(CameraErrorKind::invalid_state_transition, "相机帧投递通道尚未准备",
                              "camera.control.startFrameDelivery", session.id));

    auto started = acquisition->start();
    if (!started)
    {
        {
            std::scoped_lock lock{session.cache_mutex};
            session.acquisition.reset();
        }
        session.acquisition_queue.reset();
        session.frame_pool.reset();
        return started;
    }
    try
    {
        session.frame_forwarder = std::jthread(
            [this, &session](const std::stop_token token) { forward_frames(session, token); });
    }
    catch (const std::exception&)
    {
        acquisition->request_stop();
        static_cast<void>(
            acquisition->join(std::chrono::steady_clock::now() + preview_shutdown_timeout));
        session.acquisition_queue->close();
        session.frame_pool->close();
        {
            std::scoped_lock lock{session.cache_mutex};
            session.acquisition.reset();
        }
        session.acquisition_queue.reset();
        session.frame_pool.reset();
        return Result<void>::failure(
            make_camera_error(CameraErrorKind::stream_start_failed, "无法启动相机帧投递线程",
                              "camera.control.startFrameDelivery", session.id));
    }
    return Result<void>::success();
}

Result<void> CameraControlRuntime::stop_frame_delivery(Session& session)
{
    std::shared_ptr<AcquisitionWorker> acquisition;
    {
        std::scoped_lock lock{session.cache_mutex};
        acquisition = session.acquisition;
    }
    if (acquisition)
        acquisition->request_stop();
    if (session.acquisition_queue)
        session.acquisition_queue->close();
    if (session.frame_forwarder.joinable())
        session.frame_forwarder.request_stop();

    Result<void> joined = Result<void>::success();
    if (acquisition)
    {
        joined = acquisition->join(std::chrono::steady_clock::now() + preview_shutdown_timeout);
    }
    if (session.frame_forwarder.joinable())
        session.frame_forwarder.join();
    if (session.frame_pool)
        session.frame_pool->close();
    {
        std::scoped_lock lock{session.cache_mutex};
        session.acquisition.reset();
    }
    session.acquisition_queue.reset();
    session.frame_pool.reset();
    return joined;
}

void CameraControlRuntime::forward_frames(Session& session,
                                          const std::stop_token stop_token) noexcept
{
    std::string camera_suffix = session.id;
    std::ranges::transform(camera_suffix, camera_suffix.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    const auto thread_registration =
        delivery_options_.register_thread
            ? delivery_options_.register_thread("camera-forward-" + camera_suffix)
            : nullptr;
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
    CameraControlSnapshot r{.camera_id = s.id, .state = s.state.load(std::memory_order_acquire)};
    std::shared_ptr<AcquisitionWorker> acquisition;
    {
        std::scoped_lock lock{s.cache_mutex};
        r.serial_number = s.serial;
        r.last_error = s.error;
        r.device = s.descriptor;
        r.capabilities = s.capabilities;
        r.actual = s.actual;
        acquisition = s.acquisition;
    }
    if (acquisition)
        r.acquisition = acquisition->snapshot();
    return Result<CameraControlSnapshot>::success(std::move(r));
}

Result<void> CameraControlRuntime::refresh_cache(Session& s)
{
    if (!s.device)
        return Result<void>::failure(make_camera_error(CameraErrorKind::invalid_state_transition,
                                                       "相机尚未连接",
                                                       "camera.control.refreshCache", s.id));
    const auto descriptor = s.device->descriptor();
    auto caps = s.device->capabilities();
    if (!caps)
    {
        std::scoped_lock lock{s.cache_mutex};
        s.error = caps.error();
        return Result<void>::failure(caps.error());
    }
    auto capabilities = std::move(caps).value();
    auto actual = s.device->read_parameters();
    if (!actual)
    {
        std::scoped_lock lock{s.cache_mutex};
        s.error = actual.error();
        return Result<void>::failure(actual.error());
    }
    {
        std::scoped_lock lock{s.cache_mutex};
        s.descriptor = descriptor;
        s.capabilities = std::move(capabilities);
        s.actual = std::move(actual).value();
        s.error.reset();
    }
    return Result<void>::success();
}
Result<std::vector<CameraDeviceDescriptor>> CameraControlRuntime::discover()
{
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
    Session* session{};
    {
        std::scoped_lock lock{mutex_};
        auto found = find(id);
        if (found)
            session = found.value();
    }
    if (session == nullptr)
        return Result<CameraControlSnapshot>::success(
            {.camera_id = std::string{id}, .serial_number = std::string{serial}});
    return read(*session);
}
Result<CameraControlSnapshot> CameraControlRuntime::connect(std::string_view id,
                                                            std::string_view serial)
{
    if (!provider_)
        return Result<CameraControlSnapshot>::failure(unsupported("camera.connect"));
    if (!is_canonical_camera_id(id))
        return Result<CameraControlSnapshot>::failure(
            make_camera_error(CameraErrorKind::config_failed, "逻辑相机 ID 必须为 CAM01 至 CAM06",
                              "camera.control.connect", std::string{id}));
    Session* session{};
    {
        std::scoped_lock lock{mutex_};
        auto found = find(id);
        if (found)
            session = found.value();
        else
        {
            if (sessions_.size() >= camera_slot_count)
                return Result<CameraControlSnapshot>::failure(make_camera_error(
                    CameraErrorKind::config_failed, "相机数量超过六路", "camera.control.connect"));
            auto created = std::make_unique<Session>();
            created->id = id;
            created->serial = serial;
            created->line_input_slot = sessions_.size();
            {
                std::scoped_lock input_lock{line_input_mutex_};
                line_input_camera_ids_[created->line_input_slot] = created->id;
            }
            session = created.get();
            sessions_.push_back(std::move(created));
        }
    }
    std::scoped_lock operation_lock{session->operation_mutex};
    if (session->device)
    {
        bool serial_matches{};
        {
            std::scoped_lock cache_lock{session->cache_mutex};
            serial_matches = session->serial == serial;
        }
        if (!serial_matches)
            return Result<CameraControlSnapshot>::failure(make_camera_error(
                CameraErrorKind::config_failed, "逻辑相机已连接到不同序列号的设备",
                "camera.control.connect", std::string{id}));
        return read(*session);
    }
    {
        std::scoped_lock cache_lock{session->cache_mutex};
        session->serial = serial;
    }
    auto device = provider_->create_device(serial);
    if (!device)
        return Result<CameraControlSnapshot>::failure(device.error());
    const auto input_slot = session->line_input_slot;
    device.value()->set_line_input_observer([this, input_slot](const LineInputEvent& event) {
        enqueue_line_input(input_slot, event.raw_level, event.timestamp_utc_ms);
    });
    auto open = device.value()->connect();
    if (!open)
        return Result<CameraControlSnapshot>::failure(open.error());
    session->device = std::move(device).value();
    auto refreshed = refresh_cache(*session);
    if (!refreshed)
    {
        static_cast<void>(session->device->disconnect());
        session->device.reset();
        return Result<CameraControlSnapshot>::failure(refreshed.error());
    }
    session->state.store(CameraControlState::connected, std::memory_order_release);
    return read(*session);
}
Result<CameraControlSnapshot> CameraControlRuntime::disconnect(std::string_view id)
{
    Session* session{};
    {
        std::scoped_lock lock{mutex_};
        auto found = find(id);
        if (found)
            session = found.value();
    }
    if (session == nullptr)
        return Result<CameraControlSnapshot>::success(
            {.camera_id = std::string{id}, .state = CameraControlState::disconnected});
    std::scoped_lock operation_lock{session->operation_mutex};
    if (!session->device)
        return read(*session);
    if (session->state.load(std::memory_order_acquire) == CameraControlState::acquiring)
    {
        auto delivery = stop_frame_delivery(*session);
        if (!delivery)
            return Result<CameraControlSnapshot>::failure(delivery.error());
        auto r = session->device->stop_acquisition();
        if (!r)
            return Result<CameraControlSnapshot>::failure(r.error());
    }
    auto r = session->device->disconnect();
    if (!r)
        return Result<CameraControlSnapshot>::failure(r.error());
    session->device.reset();
    session->state.store(CameraControlState::disconnected, std::memory_order_release);
    {
        std::scoped_lock input_lock{line_input_mutex_};
        line_input_slots_[session->line_input_slot].reset();
    }
    return read(*session);
}
Result<CameraControlSnapshot> CameraControlRuntime::start(std::string_view id)
{
    Session* session{};
    {
        std::scoped_lock lock{mutex_};
        auto found = find(id);
        if (!found)
            return Result<CameraControlSnapshot>::failure(found.error());
        session = found.value();
    }
    std::scoped_lock operation_lock{session->operation_mutex};
    if (!session->device)
        return Result<CameraControlSnapshot>::failure(
            make_camera_error(CameraErrorKind::invalid_state_transition, "相机尚未连接",
                              "camera.control.start", std::string{id}));
    if (session->state.load(std::memory_order_acquire) == CameraControlState::acquiring)
        return read(*session);
    auto prepared = prepare_frame_delivery(*session);
    if (!prepared)
        return Result<CameraControlSnapshot>::failure(prepared.error());
    auto r = session->device->start_acquisition();
    if (!r)
    {
        static_cast<void>(stop_frame_delivery(*session));
        return Result<CameraControlSnapshot>::failure(r.error());
    }
    session->state.store(CameraControlState::acquiring, std::memory_order_release);
    auto delivery = start_frame_delivery(*session);
    if (!delivery)
    {
        static_cast<void>(session->device->stop_acquisition());
        session->state.store(CameraControlState::connected, std::memory_order_release);
        return Result<CameraControlSnapshot>::failure(delivery.error());
    }
    return read(*session);
}
Result<CameraControlSnapshot> CameraControlRuntime::stop(std::string_view id)
{
    Session* session{};
    {
        std::scoped_lock lock{mutex_};
        auto found = find(id);
        if (!found)
            return Result<CameraControlSnapshot>::failure(found.error());
        session = found.value();
    }
    std::scoped_lock operation_lock{session->operation_mutex};
    if (!session->device)
        return Result<CameraControlSnapshot>::failure(
            make_camera_error(CameraErrorKind::invalid_state_transition, "相机尚未连接",
                              "camera.control.stop", std::string{id}));
    if (session->state.load(std::memory_order_acquire) == CameraControlState::connected)
        return read(*session);
    auto delivery = stop_frame_delivery(*session);
    if (!delivery)
        return Result<CameraControlSnapshot>::failure(delivery.error());
    auto r = session->device->stop_acquisition();
    if (!r)
        return Result<CameraControlSnapshot>::failure(r.error());
    session->state.store(CameraControlState::connected, std::memory_order_release);
    return read(*session);
}

Result<CameraClockSample> CameraControlRuntime::sample_clock(
    const std::string_view id, const std::stop_token stop_token,
    const std::chrono::steady_clock::time_point deadline)
{
    Session* session{};
    {
        std::scoped_lock lock{mutex_};
        auto found = find(id);
        if (!found)
            return Result<CameraClockSample>::failure(found.error());
        session = found.value();
    }
    if (stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline)
    {
        auto error = make_error("TIME_PROBE_UNAVAILABLE", Severity::warning,
                                "相机时间采样已取消或超过截止时间", "camera",
                                "camera.control.sampleClock", true);
        error.source_id = std::string{id};
        return Result<CameraClockSample>::failure(std::move(error));
    }
    std::scoped_lock operation_lock{session->operation_mutex};
    if (!session->device)
    {
        auto error = make_error("TIME_PROBE_UNAVAILABLE", Severity::warning,
                                "相机尚未连接，无法采样时间能力", "camera",
                                "camera.control.sampleClock", true);
        error.source_id = std::string{id};
        return Result<CameraClockSample>::failure(std::move(error));
    }
    return session->device->sample_clock(stop_token, deadline);
}
Result<CameraControlSnapshot> CameraControlRuntime::update(std::string_view id,
                                                           const CameraParameterSnapshot& p)
{
    Session* session{};
    {
        std::scoped_lock lock{mutex_};
        auto found = find(id);
        if (!found)
            return Result<CameraControlSnapshot>::failure(found.error());
        session = found.value();
    }
    std::scoped_lock operation_lock{session->operation_mutex};
    if (!session->device)
        return Result<CameraControlSnapshot>::failure(
            make_camera_error(CameraErrorKind::invalid_state_transition, "相机尚未连接",
                              "camera.control.update", std::string{id}));
    const bool resume_acquisition =
        session->state.load(std::memory_order_acquire) == CameraControlState::acquiring;
    if (resume_acquisition)
    {
        auto delivery = stop_frame_delivery(*session);
        if (!delivery)
            return Result<CameraControlSnapshot>::failure(delivery.error());
        auto stopped = session->device->stop_acquisition();
        if (!stopped)
        {
            if (prepare_frame_delivery(*session))
                static_cast<void>(start_frame_delivery(*session));
            return Result<CameraControlSnapshot>::failure(stopped.error());
        }
        session->state.store(CameraControlState::connected, std::memory_order_release);
    }
    auto r = apply_validated_parameters(*session->device, p);
    {
        std::scoped_lock cache_lock{session->cache_mutex};
        if (r)
        {
            session->actual = r.value();
            session->error.reset();
        }
        else
            session->error = r.error();
    }
    Result<void> resumed = Result<void>::success();
    if (resume_acquisition)
    {
        resumed = prepare_frame_delivery(*session);
        if (resumed)
        {
            resumed = session->device->start_acquisition();
            if (resumed)
            {
                session->state.store(CameraControlState::acquiring, std::memory_order_release);
                resumed = start_frame_delivery(*session);
                if (!resumed)
                {
                    static_cast<void>(session->device->stop_acquisition());
                    session->state.store(CameraControlState::connected, std::memory_order_release);
                }
            }
            else
                static_cast<void>(stop_frame_delivery(*session));
        }
    }
    if (!resumed)
        return Result<CameraControlSnapshot>::failure(resumed.error());
    if (!r)
        return Result<CameraControlSnapshot>::failure(r.error());
    return read(*session);
}
Result<CapturedFrameMetadata> CameraControlRuntime::capture_snapshot(std::string_view id)
{
    Session* session{};
    {
        std::scoped_lock lock{mutex_};
        auto found = find(id);
        if (!found)
            return Result<CapturedFrameMetadata>::failure(found.error());
        session = found.value();
    }
    std::scoped_lock operation_lock{session->operation_mutex};
    std::optional<CameraCapabilities> capabilities;
    {
        std::scoped_lock cache_lock{session->cache_mutex};
        capabilities = session->capabilities;
    }
    if (!capabilities)
        return Result<CapturedFrameMetadata>::failure(
            make_camera_error(CameraErrorKind::invalid_state_transition, "相机能力缓存不可用",
                              "camera.control.captureSnapshot", std::string{id}));
    if (capabilities->maximum_payload_bytes == 0U ||
        capabilities->maximum_payload_bytes > maximum_operator_snapshot_bytes)
        return Result<CapturedFrameMetadata>::failure(
            make_camera_error(CameraErrorKind::config_failed, "快照缓冲区大小超出安全范围",
                              "camera.control.captureSnapshot", std::string{id}));
    FrameBuffer buffer{capabilities->maximum_payload_bytes};
    return session->device->capture_into(buffer, std::chrono::milliseconds{1000});
}
Result<void> CameraControlRuntime::software_trigger(std::string_view id)
{
    Session* session{};
    {
        std::scoped_lock lock{mutex_};
        auto found = find(id);
        if (!found)
            return Result<void>::failure(found.error());
        session = found.value();
    }
    std::scoped_lock operation_lock{session->operation_mutex};
    if (!session->device)
        return Result<void>::failure(
            make_camera_error(CameraErrorKind::invalid_state_transition, "相机尚未连接",
                              "camera.control.softwareTrigger", std::string{id}));
    return session->device->software_trigger();
}
} // namespace paperbreak::camera
