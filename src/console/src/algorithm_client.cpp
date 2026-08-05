#include "paperbreak/console/algorithm_client.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <utility>

namespace paperbreak::console
{
namespace
{
using Json = nlohmann::json;

Error client_error(std::string code, std::string message, std::string operation,
                   const bool retryable = false)
{
    return make_error(std::move(code), Severity::warning, std::move(message), "console",
                      std::move(operation), retryable);
}

Result<Json> response_payload(const Result<ipc::ResponseMessage>& result,
                              const std::string_view operation)
{
    if (!result)
        return Result<Json>::failure(result.error());
    if (!result.value().success)
        return Result<Json>::failure(result.value().error.value_or(
            client_error("IPC_PROTOCOL_ERROR", "后台服务返回未知失败", std::string{operation})));
    auto payload = Json::parse(result.value().payload_json, nullptr, false);
    if (payload.is_discarded() || !payload.is_object())
        return Result<Json>::failure(
            client_error("IPC_PROTOCOL_ERROR", "算法响应不是有效对象", std::string{operation}));
    return Result<Json>::success(std::move(payload));
}

AlgorithmConfigurationValue configuration_value(const Json& value)
{
    const auto& roi = value.at("roi");
    return {.enabled = value.value("enabled", false),
            .type = value.value("type", std::string{"mock"}),
            .roi = {.width = roi.value("width", 1U),
                    .height = roi.value("height", 1U),
                    .offset_x = roi.value("offsetX", 0U),
                    .offset_y = roi.value("offsetY", 0U)},
            .candidate_threshold = value.value("candidateThreshold", 0.6),
            .confirmation_threshold = value.value("confirmationThreshold", 0.8),
            .consecutive_frames = value.value("consecutiveFrames", 3U),
            .cooldown_ms = value.value("cooldownMs", 1000U),
            .model_reference = value.value("modelReference", std::string{}),
            .model_version = value.value("modelVersion", std::string{}),
            .device = value.value("device", std::string{"cpu"}),
            .debug_overlay = value.value("debugOverlay", false)};
}

Json configuration_json(const AlgorithmConfigurationValue& value)
{
    return {{"enabled", value.enabled},
            {"type", value.type},
            {"roi",
             {{"width", value.roi.width},
              {"height", value.roi.height},
              {"offsetX", value.roi.offset_x},
              {"offsetY", value.roi.offset_y}}},
            {"candidateThreshold", value.candidate_threshold},
            {"confirmationThreshold", value.confirmation_threshold},
            {"consecutiveFrames", value.consecutive_frames},
            {"cooldownMs", value.cooldown_ms},
            {"modelReference", value.model_reference},
            {"modelVersion", value.model_version},
            {"device", value.device},
            {"debugOverlay", value.debug_overlay}};
}

AlgorithmRuntimeValue runtime_value(const Json& value)
{
    const Json empty_detector = Json::object();
    const auto& detector = value.contains("detector") && value["detector"].is_object()
                               ? value["detector"]
                               : empty_detector;
    const auto& metrics = value.at("metrics");
    return {
        .camera_id = value.value("cameraId", std::string{}),
        .config_revision = value.value("configRevision", std::uint64_t{}),
        .state = value.value("state", std::string{"disabled"}),
        .has_current_frame = value.value("hasCurrentFrame", false),
        .latest_sequence_number = value.value("latestSequenceNumber", std::uint64_t{}),
        .plugin_id = detector.value("pluginId", std::string{}),
        .display_name = detector.value("displayName", std::string{}),
        .implementation_version = detector.value("implementationVersion", std::string{}),
        .detector_model_version = detector.value("modelVersion", std::string{}),
        .supports_hot_update = detector.value("supportsHotUpdate", false),
        .prototype_only = detector.value("prototypeOnly", true),
        .metrics = {
            .queue_depth = metrics.value("queueDepth", std::uint64_t{}),
            .queue_capacity = metrics.value("queueCapacity", std::uint64_t{}),
            .queue_high_watermark = metrics.value("queueHighWatermark", std::uint64_t{}),
            .submitted_frames = metrics.value("submittedFrames", std::uint64_t{}),
            .processed_frames = metrics.value("processedFrames", std::uint64_t{}),
            .skipped_frames = metrics.value("skippedFrames", std::uint64_t{}),
            .detector_failures = metrics.value("detectorFailures", std::uint64_t{}),
            .consecutive_detector_failures =
                metrics.value("consecutiveDetectorFailures", std::uint64_t{}),
            .process_calls = metrics.value("processCalls", std::uint64_t{}),
            .last_processing_time_us = metrics.value("lastProcessingTimeUs", std::int64_t{}),
            .average_processing_time_us = metrics.value("averageProcessingTimeUs", std::int64_t{}),
            .maximum_processing_time_us = metrics.value("maximumProcessingTimeUs", std::int64_t{}),
            .candidates_created = metrics.value("candidatesCreated", std::uint64_t{}),
            .confirmed_events = metrics.value("confirmedEvents", std::uint64_t{}),
            .rejected_candidates = metrics.value("rejectedCandidates", std::uint64_t{})}};
}

AlgorithmTestResultValue test_result_value(const Json& payload)
{
    const auto& value = payload.at("result");
    const auto& region = value.at("evaluatedRegion");
    AlgorithmTestResultValue result{
        .isolated = payload.value("isolated", false),
        .candidate_created = payload.value("candidateCreated", true),
        .triggered = value.value("triggered", false),
        .anomalous = value.value("anomalous", false),
        .trigger_source = value.value("triggerSource", std::string{}),
        .candidate_type = value.value("candidateType", std::string{}),
        .sequence_number = value.value("sequenceNumber", std::uint64_t{}),
        .confidence = value.value("confidence", 0.0),
        .area_ratio = value.value("areaRatio", 0.0),
        .change_score = value.value("changeScore", 0.0),
        .processing_time_us = value.value("processingTimeUs", std::int64_t{}),
        .reason = value.value("reason", std::string{}),
        .detector_version = value.value("detectorVersion", std::string{}),
        .model_version = value.value("modelVersion", std::string{}),
        .evaluated_region = {.width = region.value("width", 1U),
                             .height = region.value("height", 1U),
                             .offset_x = region.value("offsetX", 0U),
                             .offset_y = region.value("offsetY", 0U)},
        .preview_source_width = payload.value("previewSourceWidth", 0U),
        .preview_source_height = payload.value("previewSourceHeight", 0U)};
    if (value.contains("debugMetrics") && value["debugMetrics"].is_array())
    {
        for (const auto& metric : value["debugMetrics"])
        {
            if (metric.is_object())
                result.debug_metrics.push_back({.name = metric.value("name", std::string{}),
                                                .value = metric.value("value", 0.0)});
        }
    }
    return result;
}

bool valid_camera_id(const std::string_view value) noexcept
{
    return value.size() == 5U && value.starts_with("CAM0") && value[4] >= '1' && value[4] <= '4';
}

} // namespace

AlgorithmClient::AlgorithmClient(AlgorithmClientObserver observer, ipc::IpcClientOptions options)
    : observer_(std::move(observer))
{
    client_ = std::make_unique<ipc::IpcClient>(
        ipc::IpcClientCallbacks{
            .connection_changed = [this](const auto& value) { connection_changed(value); }},
        std::move(options));
}

AlgorithmClient::~AlgorithmClient()
{
    stop();
}

Result<void> AlgorithmClient::start()
{
    return client_->start();
}

void AlgorithmClient::stop() noexcept
{
    if (client_)
        client_->stop();
    config_request_.reset();
    config_request_camera_id_.clear();
    operation_request_.reset();
    snapshot_.stale = true;
    snapshot_.operation_pending = false;
    notify();
}

Result<void> AlgorithmClient::select_camera(std::string camera_id)
{
    if (!valid_camera_id(camera_id))
        return Result<void>::failure(client_error("IPC_REQUEST_INVALID",
                                                  "相机编号必须为 CAM01 至 CAM04",
                                                  "console.algorithm.selectCamera"));
    if (operation_request_)
        return Result<void>::failure(client_error("IPC_BUSY", "已有算法操作正在执行",
                                                  "console.algorithm.selectCamera", true));
    if (snapshot_.camera_id == camera_id)
        return Result<void>::success();
    snapshot_.camera_id = std::move(camera_id);
    snapshot_.stale = true;
    snapshot_.test_result.reset();
    refresh();
    notify();
    return Result<void>::success();
}

void AlgorithmClient::connection_changed(const ipc::ClientConnectionSnapshot& connection)
{
    snapshot_.connection = connection;
    if (connection.state == ipc::ClientConnectionState::connected)
        refresh();
    else
    {
        snapshot_.stale = true;
        snapshot_.operation_pending = false;
        config_request_.reset();
        config_request_camera_id_.clear();
        operation_request_.reset();
    }
    notify();
}

void AlgorithmClient::refresh()
{
    if (snapshot_.connection.state != ipc::ClientConnectionState::connected || config_request_)
        return;
    auto sent = client_->send_request(
        "algorithm.getConfig", Json{{"cameraId", snapshot_.camera_id}}.dump(), {},
        [this](auto handle, auto result) { config_completed(handle, std::move(result)); });
    if (sent)
    {
        config_request_ = sent.value();
        config_request_camera_id_ = snapshot_.camera_id;
    }
    else
    {
        snapshot_.stale = true;
        snapshot_.error = sent.error();
    }
    notify();
}

Result<void> AlgorithmClient::update_configuration(AlgorithmConfigurationValue value)
{
    if (snapshot_.connection.state != ipc::ClientConnectionState::connected)
        return Result<void>::failure(client_error("IPC_NOT_CONNECTED", "后台服务尚未连接",
                                                  "console.algorithm.updateConfig", true));
    if (snapshot_.stale)
        return Result<void>::failure(client_error("ALGORITHM_CONFIG_STALE", "算法配置尚未同步",
                                                  "console.algorithm.updateConfig", true));
    if (operation_request_)
        return Result<void>::failure(client_error("IPC_BUSY", "已有算法操作正在执行",
                                                  "console.algorithm.updateConfig", true));
    snapshot_.operation = "algorithm.updateConfig";
    snapshot_.operation_pending = true;
    snapshot_.error.reset();
    auto sent = client_->send_request(
        "algorithm.updateConfig",
        Json{{"cameraId", snapshot_.camera_id},
             {"expectedConfigRevision", snapshot_.stored_config_revision},
             {"algorithm", configuration_json(value)}}
            .dump(),
        {}, [this](auto handle, auto result) { operation_completed(handle, std::move(result)); });
    if (!sent)
    {
        snapshot_.operation_pending = false;
        return Result<void>::failure(sent.error());
    }
    operation_request_ = sent.value();
    notify();
    return Result<void>::success();
}

Result<void> AlgorithmClient::test_current_frame()
{
    if (snapshot_.connection.state != ipc::ClientConnectionState::connected)
        return Result<void>::failure(client_error("IPC_NOT_CONNECTED", "后台服务尚未连接",
                                                  "console.algorithm.testCurrentFrame", true));
    if (operation_request_)
        return Result<void>::failure(client_error("IPC_BUSY", "已有算法操作正在执行",
                                                  "console.algorithm.testCurrentFrame", true));
    snapshot_.operation = "algorithm.testCurrentFrame";
    snapshot_.operation_pending = true;
    snapshot_.error.reset();
    snapshot_.test_result.reset();
    auto sent = client_->send_request(
        "algorithm.testCurrentFrame", Json{{"cameraId", snapshot_.camera_id}}.dump(), {},
        [this](auto handle, auto result) { operation_completed(handle, std::move(result)); });
    if (!sent)
    {
        snapshot_.operation_pending = false;
        return Result<void>::failure(sent.error());
    }
    operation_request_ = sent.value();
    notify();
    return Result<void>::success();
}

const AlgorithmClientSnapshot& AlgorithmClient::snapshot() const noexcept
{
    return snapshot_;
}

void AlgorithmClient::config_completed(const ipc::ClientRequestHandle handle,
                                       Result<ipc::ResponseMessage> result)
{
    if (!config_request_ || *config_request_ != handle)
        return;
    const auto requested_camera_id = std::move(config_request_camera_id_);
    config_request_.reset();
    if (requested_camera_id != snapshot_.camera_id)
    {
        refresh();
        notify();
        return;
    }
    auto payload = response_payload(result, "console.algorithm.getConfig");
    try
    {
        if (!payload)
        {
            snapshot_.stale = true;
            snapshot_.error = payload.error();
        }
        else
        {
            snapshot_.configuration = configuration_value(payload.value().at("algorithm"));
            snapshot_.effective_configuration =
                configuration_value(payload.value().at("effectiveAlgorithm"));
            snapshot_.stored_config_revision =
                payload.value().value("storedConfigRevision", std::uint64_t{});
            snapshot_.effective_config_revision =
                payload.value().value("effectiveConfigRevision", std::uint64_t{});
            snapshot_.runtime = runtime_value(payload.value().at("runtime"));
            snapshot_.stale = false;
            snapshot_.error.reset();
        }
    }
    catch (const std::exception&)
    {
        snapshot_.stale = true;
        snapshot_.error = client_error("IPC_PROTOCOL_ERROR", "算法配置响应字段不完整",
                                       "console.algorithm.getConfig");
    }
    notify();
}

void AlgorithmClient::operation_completed(const ipc::ClientRequestHandle handle,
                                          Result<ipc::ResponseMessage> result)
{
    if (!operation_request_ || *operation_request_ != handle)
        return;
    operation_request_.reset();
    snapshot_.operation_pending = false;
    auto payload = response_payload(result, "console.algorithm.operation");
    if (!payload)
    {
        snapshot_.error = payload.error();
        notify();
        return;
    }
    try
    {
        if (snapshot_.operation == "algorithm.testCurrentFrame")
        {
            if (payload.value().value("previewFormat", std::string{}) != "jpeg" ||
                !payload.value().contains("previewBytes") ||
                !payload.value()["previewBytes"].is_number_unsigned() ||
                payload.value()["previewBytes"].get<std::size_t>() != result.value().binary.size())
                throw std::runtime_error{"algorithm preview payload mismatch"};
            snapshot_.test_result = test_result_value(payload.value());
            snapshot_.test_result->preview_jpeg = result.value().binary;
            snapshot_.error.reset();
            notify();
            return;
        }
    }
    catch (const std::exception&)
    {
        snapshot_.error = client_error("IPC_PROTOCOL_ERROR", "算法测试响应字段不完整",
                                       "console.algorithm.testCurrentFrame");
        notify();
        return;
    }
    snapshot_.stale = true;
    refresh();
    notify();
}

void AlgorithmClient::notify() const noexcept
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
