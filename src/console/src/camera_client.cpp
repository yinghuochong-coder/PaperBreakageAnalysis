#include "paperbreak/console/camera_client.hpp"

#include <nlohmann/json.hpp>

#include <QTimer>

#include <algorithm>
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
    target.line_io_available = false;
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
    if (source.contains("reverseX") && source["reverseX"].is_boolean())
        target.reverse_x = source["reverseX"].get<bool>();
    if (source.contains("reverseY") && source["reverseY"].is_boolean())
        target.reverse_y = source["reverseY"].get<bool>();
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
    if (source.contains("lineIo") && source["lineIo"].is_object())
    {
        target.line_io_available = true;
        const auto& line = source["lineIo"];
        if (line.contains("alarmInputEnabled") && line["alarmInputEnabled"].is_boolean())
            target.line_io.alarm_input_enabled = line["alarmInputEnabled"].get<bool>();
        if (line.contains("alarmActiveLevel") && line["alarmActiveLevel"].is_string())
            target.line_io.alarm_active_level = line["alarmActiveLevel"].get<std::string>();
        if (line.contains("strobeOutputEnabled") && line["strobeOutputEnabled"].is_boolean())
            target.line_io.strobe_output_enabled = line["strobeOutputEnabled"].get<bool>();
        if (line.contains("strobeDurationUs") && line["strobeDurationUs"].is_number_unsigned())
            target.line_io.strobe_duration_us = line["strobeDurationUs"].get<std::uint32_t>();
        if (line.contains("strobePreDelayUs") && line["strobePreDelayUs"].is_number_unsigned())
            target.line_io.strobe_pre_delay_us = line["strobePreDelayUs"].get<std::uint32_t>();
        if (line.contains("strobePostDelayUs") && line["strobePostDelayUs"].is_number_unsigned())
            target.line_io.strobe_post_delay_us = line["strobePostDelayUs"].get<std::uint32_t>();
    }
}

bool parse_integer_range(const Json& source, CameraIntegerRangeValue& target)
{
    if (!source.is_object() || !source.contains("minimum") ||
        !source["minimum"].is_number_unsigned() || !source.contains("maximum") ||
        !source["maximum"].is_number_unsigned() || !source.contains("increment") ||
        !source["increment"].is_number_unsigned())
        return false;
    CameraIntegerRangeValue parsed{source["minimum"].get<std::uint32_t>(),
                                   source["maximum"].get<std::uint32_t>(),
                                   source["increment"].get<std::uint32_t>()};
    if (parsed.increment == 0U || parsed.maximum < parsed.minimum)
        return false;
    target = parsed;
    return true;
}

bool parse_roi_capabilities(const Json& source, CameraRoiCapabilitiesValue& target)
{
    if (!source.is_object() || !source.contains("sensorWidth") ||
        !source["sensorWidth"].is_number_unsigned() || !source.contains("sensorHeight") ||
        !source["sensorHeight"].is_number_unsigned() || !source.contains("width") ||
        !source.contains("height") || !source.contains("offsetX") || !source.contains("offsetY"))
        return false;
    CameraRoiCapabilitiesValue parsed;
    parsed.sensor_width = source["sensorWidth"].get<std::uint32_t>();
    parsed.sensor_height = source["sensorHeight"].get<std::uint32_t>();
    if (parsed.sensor_width == 0U || parsed.sensor_height == 0U ||
        !parse_integer_range(source["width"], parsed.width) ||
        !parse_integer_range(source["height"], parsed.height) ||
        !parse_integer_range(source["offsetX"], parsed.offset_x) ||
        !parse_integer_range(source["offsetY"], parsed.offset_y))
        return false;
    target = parsed;
    return true;
}

bool parse_capabilities(const Json& source, CameraClientItem& target)
{
    if (!source.is_object())
        return false;
    if (source.contains("roi"))
    {
        CameraRoiCapabilitiesValue roi;
        if (!parse_roi_capabilities(source["roi"], roi))
            return false;
        target.roi_capabilities = roi;
    }
    if (source.contains("lineIo"))
    {
        const auto& line = source["lineIo"];
        if (!line.is_object() || !line.contains("alarmInputSupported") ||
            !line["alarmInputSupported"].is_boolean() || !line.contains("risingEdgeSupported") ||
            !line["risingEdgeSupported"].is_boolean() || !line.contains("fallingEdgeSupported") ||
            !line["fallingEdgeSupported"].is_boolean() || !line.contains("strobeOutputSupported") ||
            !line["strobeOutputSupported"].is_boolean() || !line.contains("unsupportedReason") ||
            !line["unsupportedReason"].is_string())
            return false;
        CameraLineIoCapabilitiesValue parsed{
            .alarm_input_supported = line["alarmInputSupported"].get<bool>(),
            .rising_edge_supported = line["risingEdgeSupported"].get<bool>(),
            .falling_edge_supported = line["fallingEdgeSupported"].get<bool>(),
            .strobe_output_supported = line["strobeOutputSupported"].get<bool>(),
            .unsupported_reason = line["unsupportedReason"].get<std::string>()};
        const auto range = [&](const char* name, std::optional<CameraIntegerRangeValue>& output) {
            if (!line.contains(name))
                return true;
            CameraIntegerRangeValue value;
            if (!parse_integer_range(line[name], value))
                return false;
            output = value;
            return true;
        };
        if (!range("strobeDurationUs", parsed.strobe_duration_us) ||
            !range("strobePreDelayUs", parsed.strobe_pre_delay_us) ||
            !range("strobePostDelayUs", parsed.strobe_post_delay_us))
            return false;
        target.line_io_capabilities = std::move(parsed);
    }
    return true;
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
    result["reverseX"] = value.reverse_x;
    result["reverseY"] = value.reverse_y;
    if (!value.pixel_format.empty())
        result["pixelFormat"] = value.pixel_format;
    if (!value.trigger_mode.empty())
        result["triggerMode"] = value.trigger_mode;
    if (!value.trigger_source.empty() || !value.trigger_mode.empty())
        result["triggerSource"] = value.trigger_source;
    if (value.trigger_delay_us)
        result["triggerDelayUs"] = *value.trigger_delay_us;
    if (value.packet_size_bytes)
        result["packetSizeBytes"] = *value.packet_size_bytes;
    if (value.inter_packet_delay_ns)
        result["interPacketDelayNs"] = *value.inter_packet_delay_ns;
    result["lineIo"] = {{"alarmInputEnabled", value.line_io.alarm_input_enabled},
                        {"alarmActiveLevel", value.line_io.alarm_active_level},
                        {"strobeOutputEnabled", value.line_io.strobe_output_enabled},
                        {"strobeDurationUs", value.line_io.strobe_duration_us},
                        {"strobePreDelayUs", value.line_io.strobe_pre_delay_us},
                        {"strobePostDelayUs", value.line_io.strobe_post_delay_us}};
    return result;
}

bool uses_control_timeout(const std::string_view command) noexcept
{
    return command != "camera.list" && command != "camera.discover" &&
           command != "camera.getConfig";
}

bool confirms_operation(const CameraOperationResult& operation,
                        const std::vector<CameraClientItem>& cameras)
{
    const auto camera = std::ranges::find_if(
        cameras, [&](const CameraClientItem& item) { return item.id == operation.camera_id; });
    if (camera == cameras.end())
        return false;
    if (operation.operation == "camera.start")
        return camera->state == "acquiring";
    if (operation.operation == "camera.stop")
        return camera->state == "connected";
    if (operation.operation == "camera.connect")
        return camera->state == "connected" || camera->state == "acquiring";
    if (operation.operation == "camera.disconnect")
        return camera->state == "disconnected";
    return false;
}
} // namespace

CameraLineInputAggregateState aggregate_line_input_state(
    const std::vector<CameraClientItem>& cameras) noexcept
{
    bool any_enabled{};
    bool any_active{};
    bool active_stale{};
    bool any_unknown{};
    for (const auto& camera : cameras)
    {
        if (!camera.saved.line_io.alarm_input_enabled)
            continue;
        any_enabled = true;
        if (!camera.line_input)
        {
            any_unknown = true;
            continue;
        }
        if (camera.line_input->alarm_active)
        {
            any_active = true;
            active_stale = active_stale || camera.line_input->stale;
        }
        else if (camera.line_input->stale)
            any_unknown = true;
    }
    if (!any_enabled)
        return CameraLineInputAggregateState::all_disabled;
    if (any_active)
        return active_stale ? CameraLineInputAggregateState::active_stale
                            : CameraLineInputAggregateState::active;
    if (any_unknown)
        return CameraLineInputAggregateState::partially_unknown;
    return CameraLineInputAggregateState::all_known_inactive;
}

CameraClient::CameraClient(CameraClientObserver observer, ipc::IpcClientOptions options,
                           const std::chrono::milliseconds control_operation_timeout)
    : observer_(std::move(observer)),
      client_(std::make_unique<ipc::IpcClient>(
          ipc::IpcClientCallbacks{
              .connection_changed = [this](const auto& value) { connection_changed(value); },
              .push_received = [this](const std::uint64_t generation,
                                      const auto& push) { push_received(generation, push); }},
          std::move(options))),
      reconciliation_timer_(std::make_unique<QTimer>()),
      control_operation_timeout_(control_operation_timeout > std::chrono::milliseconds::zero()
                                     ? control_operation_timeout
                                     : std::chrono::seconds{30})
{
    reconciliation_timer_->setSingleShot(true);
    reconciliation_timer_->setInterval(250);
    QObject::connect(reconciliation_timer_.get(), &QTimer::timeout, [this] { refresh(); });
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
    if (reconciliation_timer_)
        reconciliation_timer_->stop();
    if (client_)
        client_->stop();
    list_request_.reset();
    operation_request_.reset();
    snapshot_.stale = true;
    for (auto& camera : snapshot_.cameras)
        if (camera.line_input)
            camera.line_input->stale = true;
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
    for (auto& camera : snapshot_.cameras)
        if (camera.line_input)
            camera.line_input->stale = true;
    if (connection.state != ipc::ClientConnectionState::connected)
    {
        reconciliation_timer_->stop();
        list_request_.reset();
        if (operation_request_ && snapshot_.operation && snapshot_.operation->pending)
        {
            snapshot_.operation->pending = false;
            snapshot_.operation->outcome_unknown = true;
            snapshot_.operation->message = "后台服务连接中断，操作结果未知，正在同步";
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

void CameraClient::push_received(const std::uint64_t generation, const ipc::PushMessage& push)
{
    if (generation != snapshot_.connection.generation ||
        snapshot_.connection.state != ipc::ClientConnectionState::connected ||
        push.event_name != "camera.lineInputChanged")
        return;
    const Json value = Json::parse(push.payload_json, nullptr, false);
    if (value.is_discarded() || !value.is_object() || value.size() != 5U ||
        !value.contains("cameraId") || !value["cameraId"].is_string() ||
        !value.contains("rawLevel") || !value["rawLevel"].is_boolean() ||
        !value.contains("alarmActive") || !value["alarmActive"].is_boolean() ||
        !value.contains("revision") || !value["revision"].is_number_unsigned() ||
        !value.contains("timestampUtcMs") || !value["timestampUtcMs"].is_number_integer())
        return;
    const auto camera_id = value["cameraId"].get<std::string>();
    const auto camera = std::ranges::find_if(
        snapshot_.cameras, [&](const auto& item) { return item.id == camera_id; });
    if (camera == snapshot_.cameras.end())
        return;
    const auto revision = value["revision"].get<std::uint64_t>();
    if (camera->line_input && revision <= camera->line_input->revision)
        return;
    camera->line_input = {.enabled = true,
                          .raw_level = value["rawLevel"].get<bool>(),
                          .alarm_active = value["alarmActive"].get<bool>(),
                          .revision = revision,
                          .timestamp_utc_ms = value["timestampUtcMs"].get<std::int64_t>(),
                          .stale = false,
                          .connection_generation = generation};
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
        if (item.contains("capabilities") && !parse_capabilities(item["capabilities"], value))
        {
            snapshot_.error = protocol_error("camera.list 相机能力结构无效");
            snapshot_.stale = true;
            notify();
            return;
        }
        if (item.contains("lineInput"))
        {
            const auto& line = item["lineInput"];
            if (!line.is_object() || !line.contains("enabled") || !line["enabled"].is_boolean() ||
                !line.contains("rawLevel") || !line["rawLevel"].is_boolean() ||
                !line.contains("alarmActive") || !line["alarmActive"].is_boolean() ||
                !line.contains("revision") || !line["revision"].is_number_unsigned() ||
                !line.contains("timestampUtcMs") || !line["timestampUtcMs"].is_number_integer() ||
                !line.contains("stale") || !line["stale"].is_boolean())
            {
                snapshot_.error = protocol_error("camera.list Line 0 状态结构无效");
                snapshot_.stale = true;
                notify();
                return;
            }
            CameraLineInputValue parsed{.enabled = line["enabled"].get<bool>(),
                                        .raw_level = line["rawLevel"].get<bool>(),
                                        .alarm_active = line["alarmActive"].get<bool>(),
                                        .revision = line["revision"].get<std::uint64_t>(),
                                        .timestamp_utc_ms =
                                            line["timestampUtcMs"].get<std::int64_t>(),
                                        .stale = line["stale"].get<bool>(),
                                        .connection_generation = handle.generation};
            const auto previous = std::ranges::find_if(
                snapshot_.cameras, [&](const auto& existing) { return existing.id == value.id; });
            if (previous != snapshot_.cameras.end() && previous->line_input &&
                previous->line_input->connection_generation == handle.generation &&
                previous->line_input->revision > parsed.revision)
                value.line_input = previous->line_input;
            else
                value.line_input = parsed;
        }
        else
        {
            const auto previous = std::ranges::find_if(
                snapshot_.cameras, [&](const auto& existing) { return existing.id == value.id; });
            if (previous != snapshot_.cameras.end() && previous->line_input)
            {
                value.line_input = previous->line_input;
                value.line_input->stale = true;
            }
        }
        items.push_back(std::move(value));
    }
    snapshot_.cameras = std::move(items);
    snapshot_.stored_config_revision = payload["storedConfigRevision"].get<std::uint64_t>();
    snapshot_.topology_restart_required = payload["topologyRestartRequired"].get<bool>();
    snapshot_.stale = false;
    snapshot_.error.reset();
    if (snapshot_.operation && snapshot_.operation->outcome_unknown)
    {
        if (confirms_operation(*snapshot_.operation, snapshot_.cameras))
        {
            snapshot_.operation->outcome_unknown = false;
            snapshot_.operation->confirmed_by_snapshot = true;
            snapshot_.operation->succeeded = true;
            snapshot_.operation->message = "操作已由状态快照确认成功";
            reconciliation_timer_->stop();
        }
        else
        {
            snapshot_.operation->message = "操作结果未知，正在同步";
            reconciliation_timer_->start();
        }
    }
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
    reconciliation_timer_->stop();
    const auto timeout = uses_control_timeout(command) ? control_operation_timeout_
                                                       : std::chrono::milliseconds::zero();
    auto sent = client_->send_request(
        std::move(command), std::move(payload_json), {},
        [this](auto handle, auto result) {
            operation_completed(std::move(handle), std::move(result));
        },
        timeout);
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
        if (error.business_code == "IPC_REQUEST_TIMEOUT")
        {
            snapshot_.operation->outcome_unknown = true;
            snapshot_.operation->message = "操作结果未知，正在同步";
            snapshot_.error.reset();
            notify();
            refresh();
            return;
        }
        snapshot_.operation->message = error.message;
        snapshot_.error = error;
        reconciliation_timer_->stop();
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
    else if ((operation == "camera.getConfig" || operation == "camera.connect") &&
             payload.is_object())
    {
        const auto camera = std::find_if(
            snapshot_.cameras.begin(), snapshot_.cameras.end(),
            [this](const auto& item) { return item.id == snapshot_.operation->camera_id; });
        if (camera != snapshot_.cameras.end())
        {
            if (payload.contains("state") && payload["state"].is_string())
                camera->state = payload["state"].get<std::string>();
            if (payload.contains("actual") && payload["actual"].is_object())
            {
                camera->actual = {};
                parse_parameters(payload["actual"], camera->actual);
            }
            if (payload.contains("capabilities") &&
                !parse_capabilities(payload["capabilities"], *camera))
            {
                snapshot_.operation->message = "相机能力响应结构无效";
                snapshot_.error = protocol_error(snapshot_.operation->message);
                notify();
                return;
            }
        }
    }
    snapshot_.operation->succeeded = true;
    snapshot_.operation->message = "操作成功";
    if (!payload.is_discarded() && payload.is_object())
    {
        snapshot_.operation->saved = payload.value("saved", false);
        snapshot_.operation->dispatched = payload.value("dispatched", true);
        snapshot_.operation->applied = payload.value("applied", true);
        snapshot_.operation->restart_required = payload.value("restartRequired", false);
        if (payload.contains("applyError") && payload["applyError"].is_object() &&
            payload["applyError"].contains("message") &&
            payload["applyError"]["message"].is_string())
            snapshot_.operation->message = payload["applyError"]["message"].get<std::string>();
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
