#pragma once

#include "paperbreak/camera/acquisition.hpp"
#include "paperbreak/camera/camera.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace paperbreak::camera
{
using CameraFrameObserver = std::function<void(FrameView)>;
using CameraLineInputObserver = std::function<void(std::string_view, const LineInputEvent&)>;

struct CameraFrameDeliveryOptions final
{
    std::size_t frame_pool_capacity{8U};
    std::size_t queue_capacity{4U};
    std::chrono::milliseconds receive_timeout{250};
    ThreadRegistrationFactory register_thread;
    DebugDiagnosticSink diagnostics;
};

enum class CameraControlState
{
    disconnected,
    connected,
    acquiring
};
struct CameraControlSnapshot final
{
    std::string camera_id;
    std::string serial_number;
    CameraControlState state{CameraControlState::disconnected};
    std::optional<CameraDeviceDescriptor> device;
    std::optional<CameraCapabilities> capabilities;
    std::optional<CameraParameterSnapshot> actual;
    std::optional<AcquisitionWorkerSnapshot> acquisition;
    std::optional<Error> last_error;
};
class CameraControlRuntime final
{
  public:
    explicit CameraControlRuntime(std::shared_ptr<ICameraProvider> provider = {},
                                  CameraFrameObserver frame_observer = {},
                                  CameraFrameDeliveryOptions delivery_options = {},
                                  CameraLineInputObserver line_input_observer = {});
    ~CameraControlRuntime();
    CameraControlRuntime(const CameraControlRuntime&) = delete;
    [[nodiscard]] Result<std::vector<CameraDeviceDescriptor>> discover();
    [[nodiscard]] Result<CameraControlSnapshot> get(std::string_view id, std::string_view serial);
    [[nodiscard]] Result<CameraControlSnapshot> connect(std::string_view id,
                                                        std::string_view serial);
    [[nodiscard]] Result<CameraControlSnapshot> disconnect(std::string_view id);
    [[nodiscard]] Result<CameraControlSnapshot> start(std::string_view id);
    [[nodiscard]] Result<CameraControlSnapshot> stop(std::string_view id);
    [[nodiscard]] Result<CameraControlSnapshot> update(std::string_view id,
                                                       const CameraParameterSnapshot& parameters);
    [[nodiscard]] Result<CapturedFrameMetadata> capture_snapshot(std::string_view id);
    [[nodiscard]] Result<void> software_trigger(std::string_view id);

  private:
    struct Session;
    [[nodiscard]] Result<Session*> find(std::string_view id);
    [[nodiscard]] Result<CameraControlSnapshot> read(Session& session);
    [[nodiscard]] Result<void> refresh_cache(Session& session);
    [[nodiscard]] Result<void> prepare_frame_delivery(Session& session);
    [[nodiscard]] Result<void> start_frame_delivery(Session& session);
    [[nodiscard]] Result<void> stop_frame_delivery(Session& session);
    void forward_frames(Session& session, std::stop_token stop_token) noexcept;
    void enqueue_line_input(std::size_t slot, bool raw_level,
                            std::int64_t timestamp_utc_ms) noexcept;
    void dispatch_line_inputs(std::stop_token stop_token) noexcept;
    std::shared_ptr<ICameraProvider> provider_;
    CameraFrameObserver frame_observer_;
    CameraFrameDeliveryOptions delivery_options_;
    CameraLineInputObserver line_input_observer_;
    std::mutex line_input_mutex_;
    std::condition_variable_any line_input_condition_;
    std::array<std::optional<LineInputEvent>, 4U> line_input_slots_{};
    std::array<std::uint64_t, 4U> line_input_revisions_{};
    std::array<std::string, 4U> line_input_camera_ids_{};
    std::jthread line_input_dispatcher_;
    std::mutex mutex_;
    std::vector<std::unique_ptr<Session>> sessions_;
};
[[nodiscard]] std::string_view camera_control_state_name(CameraControlState state) noexcept;
} // namespace paperbreak::camera
