#pragma once

#include "paperbreak/common/result.hpp"
#include "paperbreak/ipc/client.hpp"

#include <QImage>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace paperbreak::console
{

struct PreviewImage final
{
    std::string camera_id;
    std::uint64_t frame_number{};
    std::uint64_t sequence_number{};
    QImage image;
    std::string camera_status;
    std::string detection_result;
    std::optional<double> brightness;
    std::optional<double> actual_fps;
};

struct PreviewSnapshot final
{
    ipc::ClientConnectionSnapshot connection;
    std::array<std::optional<PreviewImage>, 4U> images;
    bool paused{};
    bool subscribed{};
    double target_fps{2.0};
    std::optional<Error> last_error;
    std::uint64_t accepted_frames{};
    std::uint64_t rejected_frames{};
};

using PreviewObserver = std::function<void(const PreviewSnapshot&)>;

class PreviewClient final
{
  public:
    explicit PreviewClient(PreviewObserver observer = {}, ipc::IpcClientOptions options = {});
    ~PreviewClient();
    PreviewClient(const PreviewClient&) = delete;
    PreviewClient& operator=(const PreviewClient&) = delete;

    [[nodiscard]] Result<void> start();
    void stop() noexcept;
    void set_camera_ids(std::vector<std::string> camera_ids);
    void set_target_fps(double frames_per_second);
    void set_paused(bool paused);
    [[nodiscard]] const PreviewSnapshot& snapshot() const noexcept;
    [[nodiscard]] const std::vector<std::string>& camera_ids() const noexcept;

  private:
    void connection_changed(const ipc::ClientConnectionSnapshot& connection);
    void push_received(std::uint64_t generation, const ipc::PushMessage& push);
    void subscribe();
    void unsubscribe();
    void subscription_completed(ipc::ClientRequestHandle handle,
                                Result<ipc::ResponseMessage> result);
    void notify() const noexcept;

    PreviewObserver observer_;
    PreviewSnapshot snapshot_;
    std::unique_ptr<ipc::IpcClient> client_;
    std::vector<std::string> camera_ids_{"CAM01", "CAM02", "CAM03", "CAM04"};
    std::optional<ipc::ClientRequestHandle> subscription_request_;
};

} // namespace paperbreak::console
