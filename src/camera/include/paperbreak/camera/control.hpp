#pragma once

#include "paperbreak/camera/camera.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace paperbreak::camera
{
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
    std::optional<Error> last_error;
};
class CameraControlRuntime final
{
  public:
    explicit CameraControlRuntime(std::shared_ptr<ICameraProvider> provider = {});
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
    std::shared_ptr<ICameraProvider> provider_;
    std::mutex mutex_;
    std::vector<std::unique_ptr<Session>> sessions_;
};
[[nodiscard]] std::string_view camera_control_state_name(CameraControlState state) noexcept;
} // namespace paperbreak::camera
