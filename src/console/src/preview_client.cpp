#include "paperbreak/console/preview_client.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace paperbreak::console
{
namespace
{
constexpr std::size_t maximum_preview_binary_bytes = 16U * 1024U * 1024U;
constexpr std::array<double, 6U> supported_preview_frames_per_second{2.0,  3.0,  5.0,
                                                                     10.0, 20.0, 30.0};

Error protocol_error(std::string message)
{
    return {.business_code = "IPC_PROTOCOL_ERROR",
            .severity = Severity::warning,
            .message = std::move(message),
            .operation = "console.preview.frame"};
}

std::optional<std::size_t> camera_index(const std::vector<std::string>& camera_ids,
                                        const std::string& camera_id)
{
    const auto found = std::find(camera_ids.begin(), camera_ids.end(), camera_id);
    if (found == camera_ids.end())
        return std::nullopt;
    return static_cast<std::size_t>(std::distance(camera_ids.begin(), found));
}
} // namespace

PreviewClient::PreviewClient(PreviewObserver observer, ipc::IpcClientOptions options)
    : observer_(std::move(observer))
{
    client_ = std::make_unique<ipc::IpcClient>(
        ipc::IpcClientCallbacks{
            .connection_changed =
                [this](const auto& connection) { connection_changed(connection); },
            .push_received = [this](const std::uint64_t generation,
                                    const auto& push) { push_received(generation, push); }},
        std::move(options));
}

PreviewClient::~PreviewClient()
{
    stop();
}

Result<void> PreviewClient::start()
{
    auto result = client_->start();
    if (!result)
        snapshot_.last_error = result.error();
    notify();
    return result;
}

void PreviewClient::stop() noexcept
{
    if (client_)
        client_->stop();
    subscription_request_.reset();
    snapshot_.subscribed = false;
    notify();
}

void PreviewClient::set_camera_ids(std::vector<std::string> camera_ids)
{
    if (camera_ids.empty() || camera_ids.size() > 4U ||
        std::any_of(camera_ids.begin(), camera_ids.end(),
                    [](const std::string& id) { return id.empty() || id.size() > 32U; }))
    {
        snapshot_.last_error = protocol_error("预览相机编号必须为 1 至 4 个非空值");
        notify();
        return;
    }
    std::sort(camera_ids.begin(), camera_ids.end());
    if (std::adjacent_find(camera_ids.begin(), camera_ids.end()) != camera_ids.end())
    {
        snapshot_.last_error = protocol_error("预览相机编号不能重复");
        notify();
        return;
    }
    if (camera_ids == camera_ids_)
        return;
    if (subscription_request_)
        static_cast<void>(client_->cancel_request(subscription_request_.value()));
    subscription_request_.reset();
    camera_ids_ = std::move(camera_ids);
    snapshot_.images = {};
    snapshot_.subscribed = false;
    if (!snapshot_.paused && snapshot_.connection.state == ipc::ClientConnectionState::connected)
        subscribe();
    notify();
}

void PreviewClient::set_target_fps(const double frames_per_second)
{
    if (std::ranges::find(supported_preview_frames_per_second, frames_per_second) ==
        supported_preview_frames_per_second.end())
    {
        snapshot_.last_error = protocol_error("预览帧率必须为 2、3、5、10、20 或 30 fps");
        notify();
        return;
    }
    if (snapshot_.target_fps == frames_per_second)
        return;
    if (subscription_request_)
        static_cast<void>(client_->cancel_request(subscription_request_.value()));
    subscription_request_.reset();
    snapshot_.target_fps = frames_per_second;
    snapshot_.subscribed = false;
    if (!snapshot_.paused && snapshot_.connection.state == ipc::ClientConnectionState::connected)
        subscribe();
    notify();
}

void PreviewClient::set_paused(const bool paused)
{
    if (snapshot_.paused == paused)
        return;
    snapshot_.paused = paused;
    if (paused)
        unsubscribe();
    else if (snapshot_.connection.state == ipc::ClientConnectionState::connected)
        subscribe();
    notify();
}

const PreviewSnapshot& PreviewClient::snapshot() const noexcept
{
    return snapshot_;
}

const std::vector<std::string>& PreviewClient::camera_ids() const noexcept
{
    return camera_ids_;
}

void PreviewClient::connection_changed(const ipc::ClientConnectionSnapshot& connection)
{
    snapshot_.connection = connection;
    subscription_request_.reset();
    snapshot_.subscribed = false;
    if (connection.state == ipc::ClientConnectionState::connected && !snapshot_.paused)
        subscribe();
    notify();
}

void PreviewClient::push_received(const std::uint64_t generation, const ipc::PushMessage& push)
{
    if (push.event_name != "preview.frame" || snapshot_.paused ||
        generation != snapshot_.connection.generation)
        return;
    if (push.binary.empty() || push.binary.size() > maximum_preview_binary_bytes)
    {
        ++snapshot_.rejected_frames;
        snapshot_.last_error = protocol_error("预览 JPEG 负载无效");
        notify();
        return;
    }
    try
    {
        const auto payload = nlohmann::json::parse(push.payload_json);
        if (!payload.is_object() || !payload.contains("cameraId") ||
            !payload.contains("cameraFrameNumber") || !payload.contains("sequenceNumber") ||
            !payload["cameraId"].is_string() ||
            !payload["cameraFrameNumber"].is_number_unsigned() ||
            !payload["sequenceNumber"].is_number_unsigned())
            throw std::invalid_argument{"required fields"};
        const std::string camera_id = payload["cameraId"].get<std::string>();
        const auto index = camera_index(camera_ids_, camera_id);
        if (!index)
            throw std::invalid_argument{"unknown camera"};
        QImage image;
        if (!image.loadFromData(reinterpret_cast<const uchar*>(push.binary.data()),
                                static_cast<int>(push.binary.size()), "JPEG"))
            throw std::invalid_argument{"invalid JPEG"};
        PreviewImage frame{.camera_id = camera_id,
                           .frame_number = payload["cameraFrameNumber"].get<std::uint64_t>(),
                           .sequence_number = payload["sequenceNumber"].get<std::uint64_t>(),
                           .image = std::move(image)};
        if (payload.contains("cameraStatus") && payload["cameraStatus"].is_string())
            frame.camera_status = payload["cameraStatus"].get<std::string>();
        if (payload.contains("detectionResult") && payload["detectionResult"].is_string())
            frame.detection_result = payload["detectionResult"].get<std::string>();
        if (payload.contains("brightness") && payload["brightness"].is_number())
            frame.brightness = payload["brightness"].get<double>();
        if (payload.contains("actualFps") && payload["actualFps"].is_number())
            frame.actual_fps = payload["actualFps"].get<double>();
        snapshot_.images[index.value()] = std::move(frame);
        ++snapshot_.accepted_frames;
        snapshot_.last_error.reset();
    }
    catch (const std::exception&)
    {
        ++snapshot_.rejected_frames;
        snapshot_.last_error = protocol_error("预览帧元数据或 JPEG 无效");
    }
    notify();
}

void PreviewClient::subscribe()
{
    if (subscription_request_.has_value() || snapshot_.paused ||
        snapshot_.connection.state != ipc::ClientConnectionState::connected)
        return;
    const auto generation = snapshot_.connection.generation;
    auto sent = client_->send_request(
        "preview.subscribe",
        nlohmann::json{{"cameraIds", camera_ids_}, {"fps", snapshot_.target_fps}}.dump(), {},
        [this](ipc::ClientRequestHandle handle, Result<ipc::ResponseMessage> result) {
            subscription_completed(std::move(handle), std::move(result));
        });
    if (!sent)
    {
        snapshot_.last_error = sent.error();
        notify();
        return;
    }
    if (sent.value().generation == generation)
        subscription_request_ = sent.value();
}

void PreviewClient::unsubscribe()
{
    snapshot_.subscribed = false;
    if (subscription_request_)
        static_cast<void>(client_->cancel_request(subscription_request_.value()));
    subscription_request_.reset();
    if (snapshot_.connection.state == ipc::ClientConnectionState::connected)
    {
        static_cast<void>(
            client_->send_request("preview.unsubscribe", "{}", {},
                                  [](ipc::ClientRequestHandle, Result<ipc::ResponseMessage>) {}));
    }
}

void PreviewClient::subscription_completed(ipc::ClientRequestHandle handle,
                                           Result<ipc::ResponseMessage> result)
{
    if (!subscription_request_ || handle != subscription_request_.value())
        return;
    subscription_request_.reset();
    if (!result)
    {
        snapshot_.last_error = result.error();
    }
    else if (snapshot_.paused || handle.generation != snapshot_.connection.generation)
    {
        snapshot_.subscribed = false;
    }
    else
    {
        snapshot_.subscribed = true;
        snapshot_.last_error.reset();
    }
    notify();
}

void PreviewClient::notify() const noexcept
{
    if (!observer_)
        return;
    try
    {
        observer_(snapshot_);
    }
    catch (...)
    {
    }
}

} // namespace paperbreak::console
