#include "paperbreak/console/camera_client.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace paperbreak::console
{
namespace
{
using Json = nlohmann::json;

Error protocol_error(std::string message)
{
    return make_error("IPC_PROTOCOL_ERROR", Severity::error, std::move(message), "console",
                      "console.camera.parse");
}

Error busy_error()
{
    return make_error("IPC_BUSY", Severity::warning, "已有相机控制操作正在执行", "console",
                      "console.camera.command", true);
}

Result<std::vector<CameraDiscoveredDevice>> parse_discovered_devices(const Json& payload)
{
    constexpr std::size_t maximum_discovered_devices = 64U;
    if (!payload.is_object() || !payload.contains("devices") || !payload["devices"].is_array() ||
        payload["devices"].size() > maximum_discovered_devices)
        return Result<std::vector<CameraDiscoveredDevice>>::failure(
            protocol_error("camera.discover 响应结构无效"));

    std::vector<CameraDiscoveredDevice> devices;
    devices.reserve(payload["devices"].size());
    for (const auto& item : payload["devices"])
    {
        if (!item.is_object() || !item.contains("model") || !item["model"].is_string() ||
            !item.contains("serialNumber") || !item["serialNumber"].is_string() ||
            !item.contains("ip") || !item["ip"].is_string() || !item.contains("networkInterface") ||
            !item["networkInterface"].is_string() || !item.contains("exclusiveAccessAvailable") ||
            !item["exclusiveAccessAvailable"].is_boolean())
            return Result<std::vector<CameraDiscoveredDevice>>::failure(
                protocol_error("camera.discover 设备项结构无效"));
        devices.push_back(
            {.model = item["model"].get<std::string>(),
             .serial = item["serialNumber"].get<std::string>(),
             .ip = item["ip"].get<std::string>(),
             .network_interface = item["networkInterface"].get<std::string>(),
             .exclusive_access_available = item["exclusiveAccessAvailable"].get<bool>()});
    }
    return Result<std::vector<CameraDiscoveredDevice>>::success(std::move(devices));
}

void parse_parameters(const Json& source, CameraParameterValue& target)
{
    if (!source.is_object())
        return;
    if (source.contains("exposureUs") && source["exposureUs"].is_number())
        target.exposure_us = source["exposureUs"].get<double>();
    if (source.contains("gainDb") && source["gainDb"].is_number())
        target.gain_db = source["gainDb"].get<double>();
    if (source.contains("frameRate") && source["frameRate"].is_number())
        target.frame_rate = source["frameRate"].get<double>();
    if (source.contains("roi") && source["roi"].is_object())
    {
        const auto& roi = source["roi"];
        if (roi.contains("width") && roi["width"].is_number_unsigned() && roi.contains("height") &&
            roi["height"].is_number_unsigned() && roi.contains("offsetX") &&
            roi["offsetX"].is_number_unsigned() && roi.contains("offsetY") &&
            roi["offsetY"].is_number_unsigned())
            target.roi = CameraRoiValue{
                roi["width"].get<std::uint32_t>(), roi["height"].get<std::uint32_t>(),
                roi["offsetX"].get<std::uint32_t>(), roi["offsetY"].get<std::uint32_t>()};
    }
    if (source.contains("pixelFormat") && source["pixelFormat"].is_string())
        target.pixel_format = source["pixelFormat"].get<std::string>();
    if (source.contains("triggerMode") && source["triggerMode"].is_string())
        target.trigger_mode = source["triggerMode"].get<std::string>();
    if (source.contains("triggerSource") && source["triggerSource"].is_string())
        target.trigger_source = source["triggerSource"].get<std::string>();
    if (source.contains("triggerDelayUs") && source["triggerDelayUs"].is_number_unsigned())
        target.trigger_delay_us = source["triggerDelayUs"].get<std::uint32_t>();
    if (source.contains("packetSizeBytes") && source["packetSizeBytes"].is_number_unsigned())
        target.packet_size_bytes = source["packetSizeBytes"].get<std::uint32_t>();
    if (source.contains("interPacketDelayNs") && source["interPacketDelayNs"].is_number_unsigned())
        target.inter_packet_delay_ns = source["interPacketDelayNs"].get<std::uint32_t>();
}

Json parameter_json(const CameraParameterValue& value)
{
    Json result = Json::object();
    if (value.exposure_us)
        result["exposureUs"] = *value.exposure_us;
    if (value.gain_db)
        result["gainDb"] = *value.gain_db;
    if (value.frame_rate)
        result["frameRate"] = *value.frame_rate;
    if (value.roi)
        result["roi"] = {{"width", value.roi->width},
                         {"height", value.roi->height},
                         {"offsetX", value.roi->offset_x},
                         {"offsetY", value.roi->offset_y}};
    if (!value.pixel_format.empty())
        result["pixelFormat"] = value.pixel_format;
    if (!value.trigger_mode.empty())
        result["triggerMode"] = value.trigger_mode;
    result["triggerSource"] = value.trigger_source;
    if (value.trigger_delay_us)
        result["triggerDelayUs"] = *value.trigger_delay_us;
    if (value.packet_size_bytes)
        result["packetSizeBytes"] = *value.packet_size_bytes;
    if (value.inter_packet_delay_ns)
        result["interPacketDelayNs"] = *value.inter_packet_delay_ns;
    return result;
}
} // namespace

CameraClient::CameraClient(CameraClientObserver observer, ipc::IpcClientOptions options)
    : observer_(std::move(observer)),
      client_(std::make_unique<ipc::IpcClient>(
          ipc::IpcClientCallbacks{
              .connection_changed = [this](const auto& value) { connection_changed(value); }},
          std::move(options)))
{
}

CameraClient::~CameraClient()
{
    stop();
}

Result<void> CameraClient::start()
{
    return client_->start();
}

void CameraClient::stop() noexcept
{
    if (client_)
        client_->stop();
    list_request_.reset();
    operation_request_.reset();
    snapshot_.stale = true;
    notify();
}

const CameraClientSnapshot& CameraClient::snapshot() const noexcept
{
    return snapshot_;
}

void CameraClient::connection_changed(const ipc::ClientConnectionSnapshot& connection)
{
    snapshot_.connection = connection;
    snapshot_.stale = true;
    if (connection.state != ipc::ClientConnectionState::connected)
    {
        list_request_.reset();
        if (operation_request_ && snapshot_.operation && snapshot_.operation->pending)
        {
            snapshot_.operation->pending = false;
            snapshot_.operation->message = "后台服务连接中断，操作结果未知";
        }
        operation_request_.reset();
    }
    else
    {
        refresh();
        static_cast<void>(discover());
    }
    notify();
}

void CameraClient::refresh()
{
    if (list_request_ || snapshot_.connection.state != ipc::ClientConnectionState::connected)
        return;
    auto sent = client_->send_request("camera.list", "{}", {}, [this](auto handle, auto result) {
        list_completed(std::move(handle), std::move(result));
    });
    if (!sent)
    {
        snapshot_.error = sent.error();
        notify();
        return;
    }
    list_request_ = std::move(sent).value();
}

void CameraClient::list_completed(ipc::ClientRequestHandle handle,
                                  Result<ipc::ResponseMessage> result)
{
    if (!list_request_ || *list_request_ != handle ||
        handle.generation != snapshot_.connection.generation)
        return;
    list_request_.reset();
    if (!result || !result.value().success)
    {
        snapshot_.error =
            result ? result.value().error.value_or(protocol_error("查询失败")) : result.error();
        snapshot_.stale = true;
        notify();
        return;
    }
    const Json payload = Json::parse(result.value().payload_json, nullptr, false);
    if (payload.is_discarded() || !payload.contains("cameras") || !payload["cameras"].is_array() ||
        payload["cameras"].size() > 4U || !payload.contains("storedConfigRevision") ||
        !payload["storedConfigRevision"].is_number_unsigned() ||
        !payload.contains("topologyRestartRequired") ||
        !payload["topologyRestartRequired"].is_boolean())
    {
        snapshot_.error = protocol_error("camera.list 响应结构无效");
        snapshot_.stale = true;
        notify();
        return;
    }
    std::vector<CameraClientItem> items;
    for (const auto& item : payload["cameras"])
    {
        if (!item.is_object() || !item.contains("cameraId") || !item["cameraId"].is_string() ||
            !item.contains("state") || !item["state"].is_string())
        {
            snapshot_.error = protocol_error("camera.list 相机项结构无效");
            snapshot_.stale = true;
            notify();
            return;
        }
        CameraClientItem value{.id = item["cameraId"].get<std::string>(),
                               .state = item["state"].get<std::string>()};
        if (item.contains("location") && item["location"].is_string())
            value.location = item["location"].get<std::string>();
        if (item.contains("serialNumber") && item["serialNumber"].is_string())
            value.serial = item["serialNumber"].get<std::string>();
        if (item.contains("enabled") && item["enabled"].is_boolean())
            value.enabled = item["enabled"].get<bool>();
        if (item.contains("savedConfigRevision") &&
            item["savedConfigRevision"].is_number_unsigned())
            value.saved_config_revision = item["savedConfigRevision"].get<std::uint64_t>();
        if (item.contains("device") && item["device"].is_object())
        {
            const auto& device = item["device"];
            if (device.contains("model") && device["model"].is_string())
                value.model = device["model"].get<std::string>();
            if (device.contains("ip") && device["ip"].is_string())
                value.ip = device["ip"].get<std::string>();
        }
        if (item.contains("saved"))
            parse_parameters(item["saved"], value.saved);
        if (item.contains("actual"))
            parse_parameters(item["actual"], value.actual);
        items.push_back(std::move(value));
    }
    snapshot_.cameras = std::move(items);
    snapshot_.stored_config_revision = payload["storedConfigRevision"].get<std::uint64_t>();
    snapshot_.topology_restart_required = payload["topologyRestartRequired"].get<bool>();
    snapshot_.stale = false;
    snapshot_.error.reset();
    notify();
}

Result<void> CameraClient::discover()
{
    return send_operation("camera.discover", {}, "{}");
}

Result<void> CameraClient::bind(std::string camera_id, std::string serial_number,
                                std::string location, const std::uint64_t expected_revision)
{
    const std::string payload = Json{
        {"cameraId", camera_id},
        {"serialNumber", std::move(serial_number)},
        {"location", std::move(location)},
        {"expectedConfigRevision",
         expected_revision}}.dump();
    return send_operation("camera.bind", std::move(camera_id), payload);
}

Result<void> CameraClient::control(std::string command, std::string camera_id)
{
    const std::string payload = Json{{"cameraId", camera_id}}.dump();
    return send_operation(std::move(command), std::move(camera_id), payload);
}

Result<void> CameraClient::update_config(std::string camera_id,
                                         const std::uint64_t expected_revision,
                                         const CameraParameterValue& parameters)
{
    const std::string payload = Json{{"cameraId", camera_id},
                                     {"expectedConfigRevision", expected_revision},
                                     {"parameters", parameter_json(parameters)}}
                                    .dump();
    return send_operation("camera.updateConfig", std::move(camera_id), payload);
}

Result<void> CameraClient::send_operation(std::string command, std::string camera_id,
                                          std::string payload_json)
{
    if (operation_request_)
        return Result<void>::failure(busy_error());
    if (snapshot_.connection.state != ipc::ClientConnectionState::connected)
        return Result<void>::failure(make_error("IPC_NOT_CONNECTED", Severity::warning,
                                                "后台服务未连接", "console",
                                                "console.camera.command", true));
    snapshot_.operation = CameraOperationResult{
        .operation = command, .camera_id = camera_id, .pending = true, .message = "正在执行"};
    auto sent = client_->send_request(std::move(command), std::move(payload_json), {},
                                      [this](auto handle, auto result) {
                                          operation_completed(std::move(handle), std::move(result));
                                      });
    if (!sent)
    {
        snapshot_.operation->pending = false;
        snapshot_.operation->message = sent.error().message;
        snapshot_.error = sent.error();
        notify();
        return Result<void>::failure(sent.error());
    }
    operation_request_ = std::move(sent).value();
    notify();
    return Result<void>::success();
}

void CameraClient::operation_completed(ipc::ClientRequestHandle handle,
                                       Result<ipc::ResponseMessage> result)
{
    if (!operation_request_ || *operation_request_ != handle ||
        handle.generation != snapshot_.connection.generation)
        return;
    operation_request_.reset();
    if (!snapshot_.operation)
        return;
    snapshot_.operation->pending = false;
    const std::string operation = snapshot_.operation->operation;
    if (!result || !result.value().success)
    {
        const Error error =
            result ? result.value().error.value_or(protocol_error("操作失败")) : result.error();
        snapshot_.operation->message = error.message;
        snapshot_.error = error;
        notify();
        return;
    }
    const Json payload = Json::parse(result.value().payload_json, nullptr, false);
    if (operation == "camera.discover")
    {
        auto devices = parse_discovered_devices(payload);
        if (!devices)
        {
            snapshot_.operation->message = devices.error().message;
            snapshot_.error = devices.error();
            notify();
            return;
        }
        snapshot_.discovered_devices = std::move(devices).value();
    }
    snapshot_.operation->succeeded = true;
    snapshot_.operation->message = "操作成功";
    if (!payload.is_discarded() && payload.is_object())
    {
        snapshot_.operation->saved = payload.value("saved", false);
        snapshot_.operation->dispatched = payload.value("dispatched", true);
        snapshot_.operation->applied = payload.value("applied", true);
        snapshot_.operation->restart_required = payload.value("restartRequired", false);
    }
    snapshot_.error.reset();
    notify();
    refresh();
}

void CameraClient::notify() const noexcept
{
    try
    {
        if (observer_)
            observer_(snapshot_);
    }
    catch (...)
    {
    }
}

} // namespace paperbreak::console
