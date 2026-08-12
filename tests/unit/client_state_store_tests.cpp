#include "paperbreak/console/algorithm_client.hpp"
#include "paperbreak/console/camera_client.hpp"
#include "paperbreak/console/client_state_store.hpp"
#include "paperbreak/console/event_client.hpp"
#include "paperbreak/console/navigation_model.hpp"
#include "paperbreak/console/operations_client.hpp"
#include "paperbreak/console/preview_client.hpp"
#include "paperbreak/console/storage_client.hpp"
#include "paperbreak/console/tray_status_model.hpp"
#include "paperbreak/console/uplink_client.hpp"
#include "paperbreak/ipc/server.hpp"

#include <QCoreApplication>
#include <QEventLoop>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace
{

std::string dynamic_line_payload(std::string payload, const bool raw_level,
                                 const std::uint64_t revision)
{
    constexpr std::string_view original = R"("rawLevel":false,"alarmActive":false,"revision":3)";
    const auto position = payload.find(original);
    EXPECT_NE(position, std::string::npos);
    if (position != std::string::npos)
    {
        const std::string replacement = std::string{"\"rawLevel\":"} +
                                        (raw_level ? "true" : "false") +
                                        ",\"alarmActive\":" + (raw_level ? "true" : "false") +
                                        ",\"revision\":" + std::to_string(revision);
        payload.replace(position, original.size(), replacement);
    }
    return payload;
}

class StateAuthorizer final : public paperbreak::ipc::IPeerAuthorizer
{
  public:
    [[nodiscard]] paperbreak::Result<paperbreak::ipc::PeerIdentity> authorize(
        std::uintptr_t) noexcept override
    {
        return paperbreak::Result<paperbreak::ipc::PeerIdentity>::success(
            {.actor_sid = "S-1-5-21-state-test",
             .local = true,
             .authenticated = true,
             .administrator = false});
    }
};

class StatusHandler final : public paperbreak::ipc::IRequestHandler
{
  public:
    StatusHandler(std::string state, std::string machine,
                  const std::uint64_t malformed_metrics_responses = 0U,
                  const bool malformed_locations = false,
                  std::optional<std::string> logging_level = std::nullopt)
        : state_(std::move(state)), machine_(std::move(machine)),
          malformed_metrics_responses_(malformed_metrics_responses),
          malformed_locations_(malformed_locations), logging_level_(std::move(logging_level))
    {
    }

    [[nodiscard]] std::uint64_t metrics_requests() const noexcept
    {
        return metrics_requests_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t alarm_requests() const noexcept
    {
        return alarm_requests_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] paperbreak::Result<paperbreak::ipc::CommandResponse> handle(
        const paperbreak::ipc::RequestMessage& request, const paperbreak::ipc::PeerIdentity&,
        std::stop_token) override
    {
        if (request.command == "system.getVersion")
        {
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json = R"({"applicationVersion":"4.1.0","gitCommit":"abc123"})",
                 .binary = {}});
        }
        if (request.command == "system.getMetrics")
        {
            const auto query = nlohmann::json::parse(request.payload_json);
            const auto prefixes = query.value("prefixes", std::vector<std::string>{});
            uplink_prefix_requested.store(std::ranges::find(prefixes, "uplink.") != prefixes.end(),
                                          std::memory_order_relaxed);
            const std::uint64_t request_number =
                metrics_requests_.fetch_add(1U, std::memory_order_relaxed) + 1U;
            if (request_number <= malformed_metrics_responses_)
            {
                return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                    {.payload_json = R"({"sampledAt":42,"metrics":[]})", .binary = {}});
            }
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json =
                     R"({"sampledAt":"2026-08-03T01:00:00.000Z","metrics":[{"name":"process.cpu.percent","value":12.5,"unit":"percent","available":true},{"name":"system.memory.used_percent","value":48.0,"unit":"percent","available":true},{"name":"disk.event.free_gib","value":512.25,"unit":"GiB","available":true},{"name":"uplink.state","value":"Connected","unit":"state","available":true},{"name":"uplink.pending_upload_tasks","value":3,"unit":"count","available":true}],"truncated":false})",
                 .binary = {}});
        }
        if (request.command == "alarm.list")
        {
            alarm_requests_.fetch_add(1U, std::memory_order_relaxed);
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json =
                     R"({"registryRevision":3,"alarms":[{"alarmId":7,"revision":2,"code":"DISK_WARNING","severity":"Warning","source":"storage","firstOccurredAt":"2026-08-03T00:59:00.000Z","lastOccurredAt":"2026-08-03T01:00:00.000Z","active":true,"occurrenceCount":2,"message":"事件盘空间偏低","details":{},"acknowledged":false}],"truncated":false,"nextBeforeAlarmId":null})",
                 .binary = {}});
        }
        if (request.command == "system.getLocations")
        {
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json = malformed_locations_ ? R"({"eventRoot":42})"
                                                      : R"({"eventRoot":"C:/PaperBreak/events"})",
                 .binary = {}});
        }
        nlohmann::json status{{"serviceState", state_},
                              {"machineId", machine_},
                              {"timestamp", "2026-08-01T12:00:00.123Z"},
                              {"acceptingWrites", true}};
        if (logging_level_)
            status["loggingLevel"] = *logging_level_;
        return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
            {.payload_json = status.dump(), .binary = {}});
    }

  private:
    std::string state_;
    std::string machine_;
    std::uint64_t malformed_metrics_responses_{};
    bool malformed_locations_{};
    std::optional<std::string> logging_level_;
    std::atomic_uint64_t metrics_requests_{};
    std::atomic_uint64_t alarm_requests_{};

  public:
    std::atomic_bool uplink_prefix_requested{};
};

class CameraHandler final : public paperbreak::ipc::IRequestHandler
{
  public:
    [[nodiscard]] paperbreak::Result<paperbreak::ipc::CommandResponse> handle(
        const paperbreak::ipc::RequestMessage& request, const paperbreak::ipc::PeerIdentity&,
        std::stop_token) override
    {
        if (request.command == "camera.list")
        {
            ++list_requests;
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json = dynamic_line_payload(
                     R"({"cameras":[{"cameraId":"CAM01","location":"入口","state":"connected","serialNumber":"MOCK-01","model":"","ip":"","enabled":true,"savedConfigRevision":7,"device":{"model":"Mock","ip":"127.0.0.1"},"capabilities":{"autoExposureModes":["Off","Once","Continuous"],"roi":{"sensorWidth":1624,"sensorHeight":1240,"width":{"minimum":32,"maximum":1624,"increment":4},"height":{"minimum":4,"maximum":1240,"increment":4},"offsetX":{"minimum":0,"maximum":1592,"increment":2},"offsetY":{"minimum":0,"maximum":1232,"increment":16}},"lineIo":{"alarmInputSupported":true,"risingEdgeSupported":true,"fallingEdgeSupported":true,"strobeOutputSupported":true,"strobeDurationUs":{"minimum":1,"maximum":1000000,"increment":1},"strobePreDelayUs":{"minimum":0,"maximum":100000,"increment":1},"strobePostDelayUs":{"minimum":0,"maximum":100000,"increment":1},"unsupportedReason":""}},"lineInput":{"enabled":true,"rawLevel":false,"alarmActive":false,"revision":3,"timestampUtcMs":1000,"stale":false},"saved":{"exposureUs":100.0,"autoExposure":"Once","gainDb":2.0,"frameRate":30.0,"roi":{"width":64,"height":48,"offsetX":0,"offsetY":0},"reverseX":true,"reverseY":false,"pixelFormat":"Mono8","triggerMode":"Continuous","triggerSource":"","triggerDelayUs":0,"packetSizeBytes":1500,"interPacketDelayNs":0,"lineIo":{"alarmInputEnabled":true,"alarmActiveLevel":"High","strobeOutputEnabled":true,"strobeDurationUs":100,"strobePreDelayUs":10,"strobePostDelayUs":20}},"actual":{"exposureUs":101.0,"autoExposure":"Continuous","gainDb":2.1,"frameRate":29.9,"reverseX":true,"reverseY":false,"pixelFormat":"Mono8","triggerMode":"Continuous","lineIo":{"alarmInputEnabled":true,"strobeOutputEnabled":true,"strobeDurationUs":100,"strobePreDelayUs":10,"strobePostDelayUs":20}}}],"storedConfigRevision":7,"topologyRestartRequired":false})",
                     line_raw_level.load(std::memory_order_acquire),
                     line_revision.load(std::memory_order_acquire)),
                 .binary = {}});
        }
        ++operation_requests;
        last_command = request.command;
        last_payload_json = request.payload_json;
        return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
            {.payload_json =
                 request.command == "camera.discover"
                     ? R"({"devices":[{"model":"Mock","serialNumber":"MOCK-01","ip":"127.0.0.1","networkInterface":"mock0","exclusiveAccessAvailable":true}]})"
                 : request.command == "camera.getConfig"
                     ? R"({"cameraId":"CAM01","state":"connected","capabilities":{"autoExposureModes":["Off","Once","Continuous"],"roi":{"sensorWidth":1624,"sensorHeight":1240,"width":{"minimum":32,"maximum":1624,"increment":4},"height":{"minimum":4,"maximum":1240,"increment":4},"offsetX":{"minimum":0,"maximum":1592,"increment":2},"offsetY":{"minimum":0,"maximum":1232,"increment":16}}},"actual":{"exposureUs":777.0,"autoExposure":"Continuous","gainDb":3.0,"frameRate":25.0,"roi":{"width":64,"height":48,"offsetX":0,"offsetY":0},"reverseX":false,"reverseY":true,"pixelFormat":"Mono8","triggerMode":"Continuous","triggerSource":"","triggerDelayUs":0,"packetSizeBytes":1500,"interPacketDelayNs":0}})"
                 : request.command == "camera.connect"
                     ? R"({"cameraId":"CAM01","state":"connected","actual":{"exposureUs":101.0},"saved":false,"dispatched":false,"applied":false,"restartRequired":false,"applyError":{"code":"CAMERA_CONFIG_FAILED","message":"保存参数不符合当前设备能力"}})"
                 : request.command == "camera.updateConfig" || request.command == "camera.bind"
                     ? R"({"saved":true,"dispatched":true,"applied":true,"restartRequired":false})"
                     : R"({"state":"connected"})",
             .binary = {}});
    }

    std::atomic_uint64_t list_requests{};
    std::atomic_uint64_t operation_requests{};
    std::atomic_bool line_raw_level{};
    std::atomic_uint64_t line_revision{3U};
    std::string last_command;
    std::string last_payload_json;
};

class DelayedCameraHandler final : public paperbreak::ipc::IRequestHandler
{
  public:
    [[nodiscard]] ExecutionClass execution_class(
        const paperbreak::ipc::RequestMessage& request) const noexcept override
    {
        return request.command == "camera.list" || request.command == "camera.discover"
                   ? ExecutionClass::read_only_query
                   : ExecutionClass::serial_control;
    }

    [[nodiscard]] paperbreak::Result<paperbreak::ipc::CommandResponse> handle(
        const paperbreak::ipc::RequestMessage& request, const paperbreak::ipc::PeerIdentity&,
        std::stop_token) override
    {
        if (request.command == "camera.discover")
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json = R"({"devices":[]})", .binary = {}});
        if (request.command == "camera.list")
        {
            const std::string state =
                acquiring.load(std::memory_order_acquire) ? "acquiring" : "connected";
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json =
                     nlohmann::json{{"cameras", nlohmann::json::array({{{"cameraId", "CAM01"},
                                                                        {"state", state},
                                                                        {"enabled", true}}})},
                                    {"storedConfigRevision", 1U},
                                    {"topologyRestartRequired", false}}
                         .dump(),
                 .binary = {}});
        }
        if (request.command == "camera.start")
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{150});
            acquiring.store(true, std::memory_order_release);
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json = R"({"state":"acquiring"})", .binary = {}});
        }
        if (request.command == "camera.stop" && fail_stop.load(std::memory_order_acquire))
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::failure(
                paperbreak::make_error("CAMERA_STREAM_STOP_FAILED", paperbreak::Severity::error,
                                       "停止采集失败", "test", "camera.stop"));
        return paperbreak::Result<paperbreak::ipc::CommandResponse>::failure(
            paperbreak::make_error("IPC_REQUEST_INVALID", paperbreak::Severity::error, "unexpected",
                                   "test", "camera.handle"));
    }

    std::atomic_bool acquiring{};
    std::atomic_bool fail_stop{};
};

class OperationsHandler final : public paperbreak::ipc::IRequestHandler
{
  public:
    [[nodiscard]] paperbreak::Result<paperbreak::ipc::CommandResponse> handle(
        const paperbreak::ipc::RequestMessage& request, const paperbreak::ipc::PeerIdentity&,
        std::stop_token) override
    {
        if (request.command == "system.getMetrics")
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json =
                     R"({"snapshotVersion":4,"sampledAt":"2026-08-04T00:00:00.000Z","metrics":[{"name":"service.uptime.seconds","value":12.5,"unit":"seconds","available":true},{"name":"algorithm.state","value":"not-initialized","unit":"state","available":true},{"name":"camera.CAM01.actual_fps","value":0.0,"unit":"fps","available":false}],"truncated":false})",
                 .binary = {}});
        if (request.command == "alarm.list")
        {
            std::scoped_lock lock{mutex};
            last_alarm_payload = request.payload_json;
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json =
                     R"({"registryRevision":2,"alarms":[{"alarmId":9,"revision":2,"code":"CAMERA_OFFLINE","severity":"Error","source":"CAM01","firstOccurredAt":"2026-08-04T00:00:00.000Z","lastOccurredAt":"2026-08-04T00:00:01.000Z","active":true,"occurrenceCount":2,"message":"相机离线","details":{"reason":"timeout"},"acknowledged":false}],"truncated":false,"nextBeforeAlarmId":null})",
                 .binary = {}});
        }
        if (request.command == "log.tail")
        {
            std::scoped_lock lock{mutex};
            last_log_payload = request.payload_json;
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json =
                     R"({"firstAvailableSequence":1,"latestSequence":2,"records":[{"sequence":2,"timestamp":"2026-08-04T00:00:02.000Z","threadId":7,"category":"camera","level":"warning","message":"camera timeout"}],"truncated":false})",
                 .binary = {}});
        }
        if (request.command == "alarm.acknowledge")
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json = R"({"alarmId":9,"acknowledged":true})", .binary = {}});
        if (request.command == "system.exportDiagnostics")
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json =
                     R"({"fileName":"diagnostics.zip","contentType":"application/zip","size":4,"redacted":true})",
                 .binary = {std::byte{0x50}, std::byte{0x4b}, std::byte{0x03}, std::byte{0x04}}});
        return paperbreak::Result<paperbreak::ipc::CommandResponse>::failure(
            paperbreak::make_error("IPC_REQUEST_INVALID", paperbreak::Severity::error, "unexpected",
                                   "test", "operations.handle"));
    }

    std::mutex mutex;
    std::string last_alarm_payload;
    std::string last_log_payload;
};

class EventClientHandler final : public paperbreak::ipc::IRequestHandler
{
  public:
    EventClientHandler()
    {
        static std::atomic_uint64_t sequence{};
        export_source = std::filesystem::temp_directory_path() /
                        (L"paperbreak-verified-event-" + std::to_wstring(++sequence) + L".zip");
    }

    ~EventClientHandler() override
    {
        std::error_code error;
        std::filesystem::remove(export_source, error);
    }

    [[nodiscard]] paperbreak::Result<paperbreak::ipc::CommandResponse> handle(
        const paperbreak::ipc::RequestMessage& request, const paperbreak::ipc::PeerIdentity&,
        std::stop_token) override
    {
        if (request.command == "event.getConfig")
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json =
                     R"({"event":{"preEventSeconds":10,"postEventSeconds":10,"maxEventSeconds":60,"mergeGapSeconds":3,"keyFrameCount":7,"saveRaw":true,"generatePreviewVideo":false,"uploadPolicy":"confirmed","retentionDays":30},"storedConfigRevision":4,"effectiveConfigRevision":4,"previewVideoGenerationAvailable":false,"uploadRuntimeAvailable":false})",
                 .binary = {}});
        if (request.command == "event.list")
        {
            std::scoped_lock lock{mutex};
            last_list_payload = request.payload_json;
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json =
                     R"({"events":[{"eventId":"event-1","eventState":"Candidate","decisionState":"Candidate","persistenceState":"Committed","reviewState":"Unreviewed","reviewDecision":null,"artifactsAvailable":true,"triggerCount":2,"reviewRevision":1,"candidateTimeUtcMs":1785801600000,"triggerCameraId":"CAM01","confidence":0.875,"uploadState":"Pending","storageState":"Present","thumbnailAvailable":true}],"total":1,"offset":0,"limit":50,"summary":{"decisionCandidates":2,"decisionConfirmed":0,"persistenceCollecting":0,"persistenceEncoding":0,"persistenceQueued":0,"persistenceWriting":0,"persistenceCommitted":1,"reviewUnreviewed":1,"reviewConfirmed":0,"reviewRejected":0}})",
                 .binary = {}});
        }
        if (request.command == "event.getSummary")
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json =
                     R"({"event":{"eventId":"event-1","eventState":"Candidate","decisionState":"Candidate","persistenceState":"Committed","reviewState":"Unreviewed","reviewDecision":null,"artifactsAvailable":true,"triggerCount":2,"reviewRevision":1,"candidateTimeUtcMs":1785801600000,"triggerCameraId":"CAM01","confidence":0.875,"uploadState":"Pending","storageState":"Present","thumbnailAvailable":true},"committedDirectory":"C:/事件 数据/2026/08/04/event-1","rawFrameCount":2,"keyFrameCount":1,"observedSequenceGaps":0,"keyFramesTraceable":true,"manifestBytes":21,"thumbnailBytes":4})",
                 .binary = {std::byte{0xff}, std::byte{0xd8}, std::byte{0xff}, std::byte{0xd9}}});
        if (request.command == "event.getManifest")
        {
            const std::string manifest = R"({"eventId":"event-1"})";
            std::vector<std::byte> bytes;
            for (const unsigned char byte : manifest)
                bytes.push_back(static_cast<std::byte>(byte));
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json = nlohmann::json{{"eventId", "event-1"},
                                                {"verified", false},
                                                {"integrityState", "Unverified"},
                                                {"size", bytes.size()}}
                                     .dump(),
                 .binary = std::move(bytes)});
        }
        if (request.command == "event.export")
        {
            std::ofstream output{export_source, std::ios::binary | std::ios::trunc};
            const std::array<unsigned char, 4U> signature{0x50U, 0x4bU, 0x03U, 0x04U};
            output.write(reinterpret_cast<const char*>(signature.data()),
                         static_cast<std::streamsize>(signature.size()));
            output.close();
            const auto utf8 = export_source.generic_u8string();
            const std::string source{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json = nlohmann::json{{"eventId", "event-1"},
                                                {"verified", true},
                                                {"size", 4U},
                                                {"exportSourcePath", source}}
                                     .dump(),
                 .binary = {}});
        }
        if (request.command == "event.confirm" || request.command == "event.reject" ||
            request.command == "event.manualTrigger" || request.command == "event.retryUpload")
        {
            if (request.command == "event.retryUpload")
            {
                std::scoped_lock lock{mutex};
                last_retry_payload = request.payload_json;
            }
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json = R"({"accepted":true})", .binary = {}});
        }
        if (request.command == "event.updateConfig")
        {
            {
                std::scoped_lock lock{mutex};
                last_update_payload = request.payload_json;
            }
            if (reject_config_update.load())
                return paperbreak::Result<paperbreak::ipc::CommandResponse>::failure(
                    paperbreak::make_error("SYS_CONFIG_INVALID", paperbreak::Severity::error,
                                           "事件配置校验失败", "test", "eventClient.updateConfig"));
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json = R"({"accepted":true})", .binary = {}});
        }
        return paperbreak::Result<paperbreak::ipc::CommandResponse>::failure(
            paperbreak::make_error("IPC_REQUEST_INVALID", paperbreak::Severity::error, "unexpected",
                                   "test", "eventClient.handle"));
    }

    std::mutex mutex;
    std::string last_list_payload;
    std::string last_update_payload;
    std::string last_retry_payload;
    std::atomic_bool reject_config_update{};
    std::filesystem::path export_source;
};

class AlgorithmClientHandler final : public paperbreak::ipc::IRequestHandler
{
  public:
    [[nodiscard]] paperbreak::Result<paperbreak::ipc::CommandResponse> handle(
        const paperbreak::ipc::RequestMessage& request, const paperbreak::ipc::PeerIdentity&,
        std::stop_token) override
    {
        const auto request_payload = nlohmann::json::parse(request.payload_json);
        const std::string camera_id = request_payload.value("cameraId", "CAM01");
        if (request.command == "algorithm.updateConfig")
        {
            std::scoped_lock lock{mutex};
            last_update_payload = request.payload_json;
        }
        if (request.command == "algorithm.getConfig" || request.command == "algorithm.updateConfig")
        {
            const nlohmann::json configuration{
                {"enabled", true},
                {"type", "classical-vision"},
                {"roi", {{"width", 4}, {"height", 4}, {"offsetX", 0}, {"offsetY", 0}}},
                {"candidateThreshold", 0.6},
                {"confirmationThreshold", 0.8},
                {"consecutiveFrames", 3},
                {"cooldownMs", 1000},
                {"modelReference", ""},
                {"modelVersion", "prototype-config"},
                {"device", "cpu"},
                {"debugOverlay", true}};
            const nlohmann::json runtime{{"cameraId", camera_id},
                                         {"configRevision", 9},
                                         {"state", "active"},
                                         {"hasCurrentFrame", true},
                                         {"latestSequenceNumber", 41},
                                         {"detector",
                                          {{"pluginId", "classical-vision"},
                                           {"displayName", "M6 Classical Vision Prototype"},
                                           {"implementationVersion", "1.0.0-prototype"},
                                           {"modelVersion", "none"},
                                           {"supportsHotUpdate", true},
                                           {"prototypeOnly", true}}},
                                         {"metrics",
                                          {{"queueDepth", 1},
                                           {"queueCapacity", 8},
                                           {"queueHighWatermark", 3},
                                           {"submittedFrames", 40},
                                           {"processedFrames", 39},
                                           {"skippedFrames", 1},
                                           {"detectorFailures", 2},
                                           {"consecutiveDetectorFailures", 0},
                                           {"consecutiveBacklogEvents", 3},
                                           {"backlogActive", true},
                                           {"consecutiveBadBacklogWindows", 2},
                                           {"consecutiveHealthyBacklogWindows", 0},
                                           {"resultQueueRejected", 4},
                                           {"processCalls", 39},
                                           {"lastProcessingTimeUs", 100},
                                           {"averageProcessingTimeUs", 90},
                                           {"maximumProcessingTimeUs", 180},
                                           {"lastQueueWaitTimeUs", 1200},
                                           {"averageQueueWaitTimeUs", 800},
                                           {"maximumQueueWaitTimeUs", 2400},
                                           {"lastEndToEndTimeUs", 10800},
                                           {"averageEndToEndTimeUs", 9900},
                                           {"maximumEndToEndTimeUs", 15000},
                                           {"inputFps", 60.0},
                                           {"processedFps", 59.0},
                                           {"skippedRatio", 0.025},
                                           {"candidatesCreated", 4},
                                           {"confirmedEvents", 2},
                                           {"rejectedCandidates", 1}}}};
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json = nlohmann::json{{"algorithm", configuration},
                                                {"effectiveAlgorithm", configuration},
                                                {"storedConfigRevision", 9},
                                                {"effectiveConfigRevision", 9},
                                                {"runtime", runtime}}
                                     .dump(),
                 .binary = {}});
        }
        if (request.command == "algorithm.testCurrentFrame")
        {
            const nlohmann::json result{
                {"triggered", true},
                {"anomalous", true},
                {"triggerSource", "RoiPaperRatio"},
                {"candidateType", "paper-missing"},
                {"sequenceNumber", 41},
                {"confidence", 0.9},
                {"areaRatio", 0.8},
                {"changeScore", 0.7},
                {"processingTimeUs", 123},
                {"reason", "paper-ratio-below-minimum"},
                {"detectorVersion", "1.0.0-prototype"},
                {"modelVersion", "none"},
                {"evaluatedRegion", {{"offsetX", 0}, {"offsetY", 0}, {"width", 4}, {"height", 4}}},
                {"debugMetrics",
                 nlohmann::json::array({{{"name", "paperRatio"}, {"value", 0.2}}})}};
            const nlohmann::json response{
                {"isolated", true},
                {"candidateCreated", false},
                {"detector", {{"pluginId", "classical-vision"}, {"prototypeOnly", true}}},
                {"previewFormat", "jpeg"},
                {"previewSourceWidth", 4},
                {"previewSourceHeight", 4},
                {"previewBytes", 4},
                {"result", result}};
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json = response.dump(),
                 .binary = {std::byte{0xff}, std::byte{0xd8}, std::byte{0xff}, std::byte{0xd9}}});
        }
        return paperbreak::Result<paperbreak::ipc::CommandResponse>::failure(
            paperbreak::make_error("IPC_REQUEST_INVALID", paperbreak::Severity::error, "unexpected",
                                   "test", "algorithmClient.handle"));
    }

    std::mutex mutex;
    std::string last_update_payload;
};

class StorageClientHandler final : public paperbreak::ipc::IRequestHandler
{
  public:
    [[nodiscard]] paperbreak::Result<paperbreak::ipc::CommandResponse> handle(
        const paperbreak::ipc::RequestMessage& request, const paperbreak::ipc::PeerIdentity&,
        std::stop_token) override
    {
        const nlohmann::json storage{{"eventRoot", "数据/事件 文件"},
                                     {"cacheRoot", "data/cache"},
                                     {"rollingCacheEnabled", false},
                                     {"maximumCacheStorageGiB", 1000U},
                                     {"rollingCacheWriteLimitMiBps", 600U},
                                     {"rollingCacheIoTimeoutMs", 10000U},
                                     {"warningFreeSpaceGiB", 200U},
                                     {"criticalFreeSpaceGiB", 100U},
                                     {"stopFreeSpaceGiB", 20U},
                                     {"maximumEventStorageGiB", 1000U}};
        if (request.command == "storage.getConfig")
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json = nlohmann::json{{"storage", storage},
                                                {"effectiveStorage", storage},
                                                {"storedConfigRevision", 4U},
                                                {"effectiveConfigRevision", 4U},
                                                {"pendingRestartPaths", nlohmann::json::array()}}
                                     .dump(),
                 .binary = {}});
        if (request.command == "storage.updateConfig")
        {
            const auto payload = nlohmann::json::parse(request.payload_json);
            {
                std::scoped_lock lock{mutex};
                last_update_payload = payload;
            }
            if (reject_update.load())
                return paperbreak::Result<paperbreak::ipc::CommandResponse>::failure(
                    paperbreak::make_error("SYS_CONFIG_INVALID", paperbreak::Severity::error,
                                           "存储配置校验失败", "test",
                                           "storageClient.updateConfig"));
            auto saved = payload.at("storage");
            auto effective = saved;
            effective["cacheRoot"] = "data/cache";
            effective["rollingCacheEnabled"] = false;
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json =
                     nlohmann::json{{"storage", saved},
                                    {"effectiveStorage", effective},
                                    {"storedConfigRevision", 5U},
                                    {"effectiveConfigRevision", 4U},
                                    {"pendingRestartPaths",
                                     nlohmann::json::array({"/storage/roots", "/storage/nvme"})}}
                         .dump(),
                 .binary = {}});
        }
        return paperbreak::Result<paperbreak::ipc::CommandResponse>::failure(
            paperbreak::make_error("IPC_REQUEST_INVALID", paperbreak::Severity::error, "unexpected",
                                   "test", "storageClient.handle"));
    }

    std::mutex mutex;
    nlohmann::json last_update_payload;
    std::atomic_bool reject_update{};
};

class UplinkClientHandler final : public paperbreak::ipc::IRequestHandler
{
  public:
    [[nodiscard]] paperbreak::Result<paperbreak::ipc::CommandResponse> handle(
        const paperbreak::ipc::RequestMessage& request, const paperbreak::ipc::PeerIdentity&,
        std::stop_token) override
    {
        const nlohmann::json uplink{
            {"enabled", false},          {"serverUrl", "http://127.0.0.1:18080"},
            {"heartbeatSeconds", 5U},    {"chunkBytes", 1048576U},
            {"ioTimeoutMs", 10000U},     {"uploadLimitMiBps", 20U},
            {"credentialReference", ""}, {"certificateReference", ""}};
        if (request.command == "uplink.getConfig")
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json = nlohmann::json{{"uplink", uplink},
                                                {"effectiveUplink", uplink},
                                                {"storedConfigRevision", 4U},
                                                {"effectiveConfigRevision", 4U},
                                                {"pendingRestartPaths", nlohmann::json::array()}}
                                     .dump(),
                 .binary = {}});
        if (request.command == "uplink.updateConfig")
        {
            const auto payload = nlohmann::json::parse(request.payload_json);
            {
                std::scoped_lock lock{mutex};
                last_update_payload = payload;
            }
            auto saved = payload.at("uplink");
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json = nlohmann::json{{"uplink", saved},
                                                {"effectiveUplink", uplink},
                                                {"storedConfigRevision", 5U},
                                                {"effectiveConfigRevision", 4U},
                                                {"pendingRestartPaths",
                                                 nlohmann::json::array({"/uplink/transport"})}}
                                     .dump(),
                 .binary = {}});
        }
        return paperbreak::Result<paperbreak::ipc::CommandResponse>::failure(
            paperbreak::make_error("IPC_REQUEST_INVALID", paperbreak::Severity::error, "unexpected",
                                   "test", "uplinkClient.handle"));
    }

    std::mutex mutex;
    nlohmann::json last_update_payload;
};

class PreviewSubscriptionHandler final : public paperbreak::ipc::IRequestHandler
{
  public:
    [[nodiscard]] paperbreak::Result<paperbreak::ipc::CommandResponse> handle(
        const paperbreak::ipc::RequestMessage& request, const paperbreak::ipc::PeerIdentity&,
        std::stop_token) override
    {
        if (request.command == "preview.unsubscribe")
        {
            return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
                {.payload_json = R"({"subscribed":false})", .binary = {}});
        }
        const auto payload = nlohmann::json::parse(request.payload_json);
        std::unique_lock lock{mutex_};
        ++requests_;
        if (requests_ == 1U)
        {
            first_request_entered_ = true;
            condition_.notify_all();
            condition_.wait(lock, [this] { return release_first_request_; });
        }
        camera_ids_ = payload.at("cameraIds").get<std::vector<std::string>>();
        frames_per_second_ = payload.at("fps").get<double>();
        condition_.notify_all();
        return paperbreak::Result<paperbreak::ipc::CommandResponse>::success(
            {.payload_json = R"({"subscribed":true})", .binary = {}});
    }

    [[nodiscard]] bool first_request_entered() const
    {
        std::scoped_lock lock{mutex_};
        return first_request_entered_;
    }

    void release_first_request()
    {
        {
            std::scoped_lock lock{mutex_};
            release_first_request_ = true;
        }
        condition_.notify_all();
    }

    [[nodiscard]] bool received_single_camera() const
    {
        std::scoped_lock lock{mutex_};
        return requests_ >= 2U && camera_ids_ == std::vector<std::string>{"CAM01"};
    }

    [[nodiscard]] bool received_fps(const double frames_per_second) const
    {
        std::scoped_lock lock{mutex_};
        return frames_per_second_ == frames_per_second;
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::uint64_t requests_{};
    bool first_request_entered_{};
    bool release_first_request_{};
    std::vector<std::string> camera_ids_;
    double frames_per_second_{};
};

std::string state_name()
{
    static std::atomic_uint64_t sequence{};
    return "PaperBreakEdgeService.Ipc.StateTest." + std::to_string(++sequence);
}

paperbreak::ipc::IpcServerOptions server_options(const std::string& name)
{
    paperbreak::ipc::IpcServerOptions options;
    options.server_name = name;
    const std::wstring suffix{name.begin(), name.end()};
    options.instance_guard_name = L"Local\\" + suffix + L".Guard";
    options.shutdown_flush_timeout = std::chrono::milliseconds{10};
    return options;
}

paperbreak::ipc::IpcClientOptions client_options(const std::string& name)
{
    paperbreak::ipc::IpcClientOptions options;
    options.server_name = name;
    options.connect_timeout = std::chrono::milliseconds{50};
    options.initial_reconnect_delay = std::chrono::milliseconds{10};
    options.maximum_reconnect_delay = std::chrono::milliseconds{40};
    options.reconnect_jitter_fraction = 0.0;
    return options;
}

bool wait_until(const std::function<bool()>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return predicate();
}

void stop_server(paperbreak::ipc::IpcServer& server)
{
    server.request_stop();
    ASSERT_TRUE(server.join(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
}

} // namespace

TEST(ClientStateStore, SynchronizesMarksStaleAndRefreshesAfterReconnect)
{
    const std::string name = state_name();
    auto first_handler = std::make_shared<StatusHandler>("running", "EDGE-01");
    auto first = std::make_unique<paperbreak::ipc::IpcServer>(
        first_handler, std::make_unique<StateAuthorizer>(), server_options(name));
    ASSERT_TRUE(first->start());

    paperbreak::console::ClientStateSnapshot latest;
    paperbreak::console::ClientStateStore store([&](const auto& snapshot) { latest = snapshot; },
                                                client_options(name));
    ASSERT_TRUE(store.start());
    ASSERT_TRUE(wait_until([&] {
        const bool ready = latest.service_status.has_value() && !latest.service_status_stale &&
                           latest.version.has_value() && !latest.version_stale &&
                           latest.metrics.has_value() && !latest.metrics_stale &&
                           latest.alarms.has_value() && !latest.alarms_stale &&
                           latest.locations.has_value() && !latest.locations_stale;
        if (!ready)
        {
            store.refresh_dynamic();
        }
        return ready;
    })) << "connection="
        << static_cast<int>(latest.connection.state)
        << ", status=" << latest.service_status.has_value() << '/' << latest.service_status_stale
        << ", version=" << latest.version.has_value() << '/' << latest.version_stale
        << ", metrics=" << latest.metrics.has_value() << '/' << latest.metrics_stale
        << ", alarms=" << latest.alarms.has_value() << '/' << latest.alarms_stale
        << ", metrics requests=" << first_handler->metrics_requests()
        << ", alarm requests=" << first_handler->alarm_requests()
        << ", server responses=" << first->metrics_snapshot().responses_total
        << ", outbound=" << first->metrics_snapshot().outbound_messages;
    EXPECT_EQ(latest.service_status->service_state, "running");
    EXPECT_EQ(latest.service_status->machine_id, "EDGE-01");
    EXPECT_EQ(latest.service_status->logging_level, "info");
    EXPECT_EQ(latest.version->application_version, "4.1.0");
    EXPECT_DOUBLE_EQ(latest.metrics->process_cpu_percent.value(), 12.5);
    EXPECT_DOUBLE_EQ(latest.metrics->system_memory_used_percent.value(), 48.0);
    EXPECT_DOUBLE_EQ(latest.metrics->event_disk_free_gib.value(), 512.25);
    EXPECT_EQ(latest.metrics->uplink_state.value(), "Connected");
    EXPECT_EQ(latest.metrics->pending_upload_tasks.value(), 3U);
    EXPECT_TRUE(first_handler->uplink_prefix_requested.load(std::memory_order_relaxed));
    ASSERT_EQ(latest.alarms->recent.size(), 1U);
    EXPECT_EQ(latest.alarms->recent.front().message, "事件盘空间偏低");
    EXPECT_EQ(latest.alarms->highest_severity, "Warning");
    EXPECT_EQ(latest.locations->event_root, "C:/PaperBreak/events");
    const std::uint64_t first_generation = latest.service_status->generation;
    const std::uint64_t metrics_before_burst = first_handler->metrics_requests();
    const std::uint64_t alarms_before_burst = first_handler->alarm_requests();

    for (int iteration = 0; iteration < 20; ++iteration)
    {
        store.refresh_dynamic();
    }
    ASSERT_TRUE(wait_until([&] {
        return first_handler->metrics_requests() > metrics_before_burst &&
               first_handler->alarm_requests() > alarms_before_burst;
    }));
    EXPECT_EQ(first_handler->metrics_requests(), metrics_before_burst + 1U);
    EXPECT_EQ(first_handler->alarm_requests(), alarms_before_burst + 1U);

    ASSERT_TRUE(first->try_publish({.event_name = "alarm.raised",
                                    .timestamp = "2026-08-03T01:00:01.000Z",
                                    .payload_json = "{}",
                                    .binary = {},
                                    .coalescing_key = "alarm.raised"}));
    ASSERT_TRUE(
        wait_until([&] { return first_handler->alarm_requests() >= alarms_before_burst + 2U; }));

    stop_server(*first);
    first.reset();
    ASSERT_TRUE(wait_until([&] { return latest.service_status_stale; }));
    ASSERT_TRUE(latest.service_status.has_value());
    EXPECT_EQ(latest.service_status->service_state, "stop-requested");
    EXPECT_TRUE(latest.version_stale);
    EXPECT_TRUE(latest.metrics_stale);
    EXPECT_TRUE(latest.alarms_stale);
    EXPECT_TRUE(latest.locations_stale);

    auto second = std::make_unique<paperbreak::ipc::IpcServer>(
        std::make_shared<StatusHandler>("degraded", "EDGE-01", 0U, false, std::string{"debug"}),
        std::make_unique<StateAuthorizer>(), server_options(name));
    ASSERT_TRUE(second->start());
    ASSERT_TRUE(wait_until([&] {
        const bool ready = latest.service_status.has_value() && !latest.service_status_stale &&
                           latest.service_status->generation > first_generation;
        if (!ready)
        {
            store.refresh_dynamic();
        }
        return ready;
    }));
    EXPECT_EQ(latest.service_status->service_state, "degraded");
    EXPECT_EQ(latest.service_status->logging_level, "debug");

    store.stop();
    stop_server(*second);
}

TEST(ClientStateStore, InvalidMetricsDoNotInvalidateOtherDataAndCanRecover)
{
    const std::string name = state_name();
    auto handler = std::make_shared<StatusHandler>("running", "EDGE-02", 1U);
    paperbreak::ipc::IpcServer server(handler, std::make_unique<StateAuthorizer>(),
                                      server_options(name));
    ASSERT_TRUE(server.start());

    paperbreak::console::ClientStateSnapshot latest;
    bool saw_metrics_protocol_error = false;
    paperbreak::console::ClientStateStore store(
        [&](const auto& snapshot) {
            latest = snapshot;
            saw_metrics_protocol_error =
                saw_metrics_protocol_error ||
                (snapshot.metrics_error.has_value() &&
                 snapshot.metrics_error->business_code == "IPC_PROTOCOL_ERROR");
        },
        client_options(name));
    ASSERT_TRUE(store.start());
    ASSERT_TRUE(wait_until([&] {
        const bool ready = latest.service_status.has_value() && !latest.service_status_stale &&
                           latest.version.has_value() && !latest.version_stale &&
                           latest.alarms.has_value() && !latest.alarms_stale &&
                           latest.metrics.has_value() && !latest.metrics_stale;
        if (!ready)
        {
            store.refresh_dynamic();
        }
        return ready;
    })) << "connection="
        << static_cast<int>(latest.connection.state)
        << ", status=" << latest.service_status.has_value() << '/' << latest.service_status_stale
        << ", version=" << latest.version.has_value() << '/' << latest.version_stale
        << ", metrics=" << latest.metrics.has_value() << '/' << latest.metrics_stale
        << ", alarms=" << latest.alarms.has_value() << '/' << latest.alarms_stale
        << ", metrics requests=" << handler->metrics_requests()
        << ", alarm requests=" << handler->alarm_requests()
        << ", server responses=" << server.metrics_snapshot().responses_total
        << ", outbound=" << server.metrics_snapshot().outbound_messages;
    EXPECT_TRUE(saw_metrics_protocol_error);
    EXPECT_TRUE(latest.metrics.has_value());
    EXPECT_FALSE(latest.synchronization_error.has_value());
    EXPECT_FALSE(latest.version_error.has_value());
    EXPECT_FALSE(latest.alarms_error.has_value());

    EXPECT_FALSE(latest.metrics_error.has_value());

    store.stop();
    stop_server(server);
}

TEST(ClientStateStore, InvalidLocationsStayStaleWithoutInvalidatingStatus)
{
    const std::string name = state_name();
    auto handler = std::make_shared<StatusHandler>("running", "EDGE-03", 0U, true);
    paperbreak::ipc::IpcServer server(handler, std::make_unique<StateAuthorizer>(),
                                      server_options(name));
    ASSERT_TRUE(server.start());

    paperbreak::console::ClientStateSnapshot latest;
    paperbreak::console::ClientStateStore store([&](const auto& snapshot) { latest = snapshot; },
                                                client_options(name));
    ASSERT_TRUE(store.start());
    ASSERT_TRUE(wait_until([&] {
        return latest.service_status.has_value() && !latest.service_status_stale &&
               latest.locations_error.has_value();
    }));
    EXPECT_TRUE(latest.locations_stale);
    EXPECT_FALSE(latest.locations.has_value());
    EXPECT_EQ(latest.locations_error->business_code, "IPC_PROTOCOL_ERROR");
    EXPECT_EQ(latest.service_status->service_state, "running");

    store.stop();
    stop_server(server);
}

TEST(CameraClient, SynchronizesReadbackAndSerializesControlOperations)
{
    const std::string name = state_name();
    auto handler = std::make_shared<CameraHandler>();
    paperbreak::ipc::IpcServer server(handler, std::make_unique<StateAuthorizer>(),
                                      server_options(name));
    ASSERT_TRUE(server.start());

    paperbreak::console::CameraClientSnapshot latest;
    std::atomic_bool explicit_readback_observed{};
    paperbreak::console::CameraClient client(
        [&](const auto& snapshot) {
            latest = snapshot;
            if (snapshot.operation && snapshot.operation->operation == "camera.getConfig" &&
                !snapshot.operation->pending && snapshot.operation->succeeded &&
                !snapshot.cameras.empty() && snapshot.cameras.front().actual.exposure_us == 777.0)
                explicit_readback_observed = true;
        },
        client_options(name));
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(wait_until([&] {
        return !latest.stale && latest.cameras.size() == 1U &&
               latest.discovered_devices.size() == 1U && latest.operation.has_value() &&
               !latest.operation->pending;
    }));
    ASSERT_TRUE(wait_until([&] { return handler->list_requests.load() >= 2U; }));
    for (int iteration = 0; iteration < 10; ++iteration)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const auto& camera = latest.cameras.front();
    EXPECT_EQ(camera.id, "CAM01");
    EXPECT_EQ(camera.saved_config_revision, 7U);
    EXPECT_DOUBLE_EQ(camera.saved.exposure_us.value(), 100.0);
    EXPECT_EQ(camera.saved.exposure_auto_mode, "Once");
    EXPECT_DOUBLE_EQ(camera.actual.exposure_us.value(), 101.0);
    EXPECT_EQ(camera.actual.exposure_auto_mode, "Continuous");
    EXPECT_EQ(camera.exposure_auto_modes, (std::vector<std::string>{"Off", "Once", "Continuous"}));
    EXPECT_TRUE(camera.saved.reverse_x);
    EXPECT_FALSE(camera.saved.reverse_y);
    EXPECT_TRUE(camera.actual.reverse_x);
    EXPECT_FALSE(camera.actual.reverse_y);
    ASSERT_TRUE(camera.roi_capabilities.has_value());
    EXPECT_EQ(camera.roi_capabilities->sensor_height, 1240U);
    EXPECT_EQ(camera.roi_capabilities->offset_y.maximum, 1232U);
    EXPECT_EQ(camera.roi_capabilities->offset_y.increment, 16U);
    ASSERT_TRUE(camera.line_io_capabilities.has_value());
    EXPECT_TRUE(camera.line_io_capabilities->alarm_input_supported);
    EXPECT_TRUE(camera.line_io_capabilities->rising_edge_supported);
    EXPECT_TRUE(camera.line_io_capabilities->falling_edge_supported);
    ASSERT_TRUE(camera.line_io_capabilities->strobe_duration_us.has_value());
    EXPECT_EQ(camera.line_io_capabilities->strobe_duration_us->minimum, 1U);
    ASSERT_TRUE(camera.line_input.has_value());
    EXPECT_EQ(camera.line_input->revision, 3U);
    EXPECT_FALSE(camera.line_input->raw_level);
    EXPECT_FALSE(camera.line_input->alarm_active);
    EXPECT_FALSE(camera.line_input->stale);
    EXPECT_EQ(latest.stored_config_revision, 7U);
    EXPECT_FALSE(latest.topology_restart_required);
    EXPECT_EQ(latest.discovered_devices.front().network_interface, "mock0");
    EXPECT_TRUE(latest.discovered_devices.front().exclusive_access_available);

    ASSERT_TRUE(server.try_publish(
        {.event_name = "camera.lineInputChanged",
         .timestamp = "2026-08-10T01:00:00.000Z",
         .payload_json =
             R"({"cameraId":"CAM01","rawLevel":true,"alarmActive":true,"revision":4,"timestampUtcMs":2000})",
         .binary = {},
         .coalescing_key = "camera.lineInputChanged:CAM01"},
        paperbreak::ipc::PushPolicy::coalesce_latest));
    ASSERT_TRUE(wait_until([&] {
        return !latest.cameras.empty() && latest.cameras.front().line_input &&
               latest.cameras.front().line_input->revision == 4U;
    })) << "current revision="
        << (latest.cameras.empty() || !latest.cameras.front().line_input
                ? 0U
                : latest.cameras.front().line_input->revision)
        << ", outbound=" << server.metrics_snapshot().outbound_messages
        << ", dropped=" << server.metrics_snapshot().pushes_dropped_total;
    EXPECT_TRUE(latest.cameras.front().line_input->raw_level);
    EXPECT_TRUE(latest.cameras.front().line_input->alarm_active);

    handler->line_raw_level.store(false, std::memory_order_release);
    handler->line_revision.store(3U, std::memory_order_release);
    const auto requests_before_stale_snapshot = handler->list_requests.load();
    client.refresh();
    ASSERT_TRUE(
        wait_until([&] { return handler->list_requests.load() > requests_before_stale_snapshot; }));
    for (int iteration = 0; iteration < 10; ++iteration)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    EXPECT_EQ(latest.cameras.front().line_input->revision, 4U);
    EXPECT_TRUE(latest.cameras.front().line_input->raw_level);
    EXPECT_TRUE(latest.cameras.front().line_input->alarm_active);

    handler->line_revision.store(5U, std::memory_order_release);
    const auto requests_before_recovery = handler->list_requests.load();
    client.refresh();
    ASSERT_TRUE(wait_until([&] {
        return handler->list_requests.load() > requests_before_recovery &&
               latest.cameras.front().line_input &&
               latest.cameras.front().line_input->revision == 5U;
    }));
    EXPECT_FALSE(latest.cameras.front().line_input->raw_level);
    EXPECT_FALSE(latest.cameras.front().line_input->alarm_active);

    ASSERT_TRUE(client.control("camera.connect", "CAM01"));
    auto busy = client.control("camera.start", "CAM01");
    ASSERT_FALSE(busy);
    EXPECT_EQ(busy.error().business_code, "IPC_BUSY");
    ASSERT_TRUE(wait_until([&] {
        return latest.operation.has_value() && !latest.operation->pending &&
               latest.operation->succeeded;
    }));
    EXPECT_EQ(handler->last_command, "camera.connect");
    EXPECT_FALSE(latest.operation->applied);
    EXPECT_EQ(latest.operation->message, "保存参数不符合当前设备能力");

    ASSERT_TRUE(client.control("camera.getConfig", "CAM01"));
    ASSERT_TRUE(wait_until([&] { return explicit_readback_observed.load(); }));
    EXPECT_EQ(handler->last_command, "camera.getConfig");
    EXPECT_FALSE(latest.cameras.front().actual.reverse_x);
    EXPECT_TRUE(latest.cameras.front().actual.reverse_y);
    ASSERT_TRUE(latest.cameras.front().roi_capabilities.has_value());
    EXPECT_EQ(latest.cameras.front().roi_capabilities->offset_y.increment, 16U);

    ASSERT_TRUE(client.discover());
    ASSERT_TRUE(wait_until([&] {
        return latest.operation.has_value() && latest.operation->operation == "camera.discover" &&
               !latest.operation->pending;
    }));
    ASSERT_EQ(latest.discovered_devices.size(), 1U);
    EXPECT_EQ(latest.discovered_devices.front().serial, "MOCK-01");
    EXPECT_EQ(latest.discovered_devices.front().network_interface, "mock0");

    auto changed = camera.saved;
    changed.exposure_us = 120.0;
    changed.exposure_auto_mode = "Continuous";
    changed.reverse_x = false;
    changed.reverse_y = true;
    ASSERT_TRUE(client.update_config("CAM01", 7U, changed));
    ASSERT_TRUE(wait_until([&] {
        return latest.operation.has_value() &&
               latest.operation->operation == "camera.updateConfig" && !latest.operation->pending;
    }));
    EXPECT_TRUE(latest.operation->saved);
    EXPECT_TRUE(latest.operation->dispatched);
    EXPECT_TRUE(latest.operation->applied);
    EXPECT_FALSE(latest.operation->restart_required);
    const auto update_payload = nlohmann::json::parse(handler->last_payload_json);
    EXPECT_FALSE(update_payload["parameters"]["reverseX"].get<bool>());
    EXPECT_EQ(update_payload["parameters"]["autoExposure"], "Continuous");
    EXPECT_TRUE(update_payload["parameters"]["reverseY"].get<bool>());
    EXPECT_TRUE(update_payload["parameters"]["lineIo"]["alarmInputEnabled"].get<bool>());
    EXPECT_EQ(update_payload["parameters"]["lineIo"]["alarmActiveLevel"], "High");
    EXPECT_EQ(update_payload["parameters"]["lineIo"]["strobeDurationUs"], 100U);

    ASSERT_TRUE(client.bind("CAM02", "MOCK-02", "出口", 7U));
    ASSERT_TRUE(wait_until([&] {
        return latest.operation.has_value() && latest.operation->operation == "camera.bind" &&
               !latest.operation->pending;
    }));
    EXPECT_EQ(handler->last_command, "camera.bind");
    EXPECT_TRUE(latest.operation->saved);

    client.stop();
    EXPECT_TRUE(latest.stale);
    ASSERT_FALSE(latest.cameras.empty());
    ASSERT_TRUE(latest.cameras.front().line_input.has_value());
    EXPECT_TRUE(latest.cameras.front().line_input->stale);
    stop_server(server);
}

TEST(CameraClient, TimeoutBecomesUnknownAndIsConfirmedByLaterListSnapshot)
{
    const std::string name = state_name();
    auto handler = std::make_shared<DelayedCameraHandler>();
    paperbreak::ipc::IpcServer server(handler, std::make_unique<StateAuthorizer>(),
                                      server_options(name));
    ASSERT_TRUE(server.start());

    paperbreak::console::CameraClientSnapshot latest;
    std::atomic_bool saw_unknown{};
    paperbreak::console::CameraClient client(
        [&](const auto& snapshot) {
            latest = snapshot;
            if (snapshot.operation && snapshot.operation->outcome_unknown)
                saw_unknown = true;
        },
        client_options(name), std::chrono::milliseconds{40});
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(wait_until([&] {
        return !latest.stale && latest.cameras.size() == 1U && latest.operation &&
               latest.operation->operation == "camera.discover" && !latest.operation->pending;
    }));

    ASSERT_TRUE(client.control("camera.start", "CAM01"));
    ASSERT_TRUE(wait_until([&] { return saw_unknown.load(std::memory_order_acquire); }));
    ASSERT_TRUE(wait_until([&] {
        return latest.operation && latest.operation->operation == "camera.start" &&
               latest.operation->confirmed_by_snapshot;
    }));
    EXPECT_TRUE(latest.operation->succeeded);
    EXPECT_FALSE(latest.operation->outcome_unknown);
    EXPECT_EQ(latest.cameras.front().state, "acquiring");

    handler->fail_stop.store(true, std::memory_order_release);
    ASSERT_TRUE(client.control("camera.stop", "CAM01"));
    ASSERT_TRUE(wait_until([&] {
        return latest.operation && latest.operation->operation == "camera.stop" &&
               !latest.operation->pending;
    }));
    EXPECT_FALSE(latest.operation->succeeded);
    EXPECT_FALSE(latest.operation->outcome_unknown);
    EXPECT_EQ(latest.error->business_code, "CAMERA_STREAM_STOP_FAILED");

    client.stop();
    stop_server(server);
}

TEST(CameraClient, AggregatesFourLineInputsWithDisabledUnknownAndStalePrecedence)
{
    using paperbreak::console::aggregate_line_input_state;
    using paperbreak::console::CameraClientItem;
    using paperbreak::console::CameraLineInputAggregateState;
    using paperbreak::console::CameraLineInputValue;
    std::vector<CameraClientItem> cameras(4U);
    EXPECT_EQ(aggregate_line_input_state(cameras), CameraLineInputAggregateState::all_disabled);

    cameras[0].saved.line_io.alarm_input_enabled = true;
    EXPECT_EQ(aggregate_line_input_state(cameras),
              CameraLineInputAggregateState::partially_unknown);
    cameras[0].line_input = CameraLineInputValue{
        .enabled = true, .raw_level = false, .alarm_active = false, .revision = 1U, .stale = false};
    EXPECT_EQ(aggregate_line_input_state(cameras),
              CameraLineInputAggregateState::all_known_inactive);

    cameras[1].saved.line_io.alarm_input_enabled = true;
    cameras[1].line_input = CameraLineInputValue{
        .enabled = true, .raw_level = true, .alarm_active = true, .revision = 2U, .stale = false};
    EXPECT_EQ(aggregate_line_input_state(cameras), CameraLineInputAggregateState::active);
    cameras[1].line_input->stale = true;
    EXPECT_EQ(aggregate_line_input_state(cameras), CameraLineInputAggregateState::active_stale);

    cameras[1].line_input->alarm_active = false;
    EXPECT_EQ(aggregate_line_input_state(cameras),
              CameraLineInputAggregateState::partially_unknown);
}

TEST(OperationsClient, QueriesFiltersAcknowledgesAndExportsThroughBoundedWorker)
{
    const std::string name = state_name();
    auto handler = std::make_shared<OperationsHandler>();
    paperbreak::ipc::IpcServer server(handler, std::make_unique<StateAuthorizer>(),
                                      server_options(name));
    ASSERT_TRUE(server.start());

    paperbreak::console::OperationsSnapshot latest;
    paperbreak::console::OperationsClient client([&](const auto& snapshot) { latest = snapshot; },
                                                 client_options(name));
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(wait_until(
        [&] { return !latest.metrics_stale && !latest.alarms_stale && !latest.logs_stale; }));
    ASSERT_EQ(latest.metrics.size(), 3U);
    EXPECT_EQ(latest.metrics[1].value, "not-initialized");
    EXPECT_FALSE(latest.metrics[2].available);
    ASSERT_EQ(latest.alarms.size(), 1U);
    EXPECT_EQ(latest.alarms.front().details.front().second, "timeout");
    ASSERT_EQ(latest.logs.size(), 1U);
    EXPECT_EQ(latest.logs.front().category, "camera");

    ASSERT_TRUE(client.query_alarms({.active = false,
                                     .minimum_severity = std::string{"Warning"},
                                     .source = std::string{"CAM01"}}));
    ASSERT_TRUE(wait_until([&] {
        std::scoped_lock lock{handler->mutex};
        const auto payload = nlohmann::json::parse(handler->last_alarm_payload);
        return payload.value("active", true) == false &&
               payload.value("minimumSeverity", "") == "Warning" &&
               payload.value("source", "") == "CAM01";
    }));
    ASSERT_TRUE(client.query_logs(
        {.category = std::string{"camera"}, .minimum_level = std::string{"warning"}}));
    ASSERT_TRUE(wait_until([&] {
        std::scoped_lock lock{handler->mutex};
        const auto payload = nlohmann::json::parse(handler->last_log_payload);
        return payload.value("minimumLevel", "") == "warning" &&
               payload.at("categories").front() == "camera";
    }));

    ASSERT_TRUE(client.acknowledge(9U));
    auto busy = client.export_diagnostics("ignored-while-busy.zip");
    ASSERT_FALSE(busy);
    EXPECT_EQ(busy.error().business_code, "IPC_BUSY");
    ASSERT_TRUE(wait_until([&] { return !latest.operation_pending; }));

    static std::atomic_uint64_t file_sequence{};
    const auto base = std::filesystem::temp_directory_path() /
                      ("paperbreak-operations-" + std::to_string(++file_sequence));
    std::filesystem::create_directories(base);
    const auto diagnostic = base / "diagnostics.zip";
    ASSERT_TRUE(client.export_diagnostics(diagnostic));
    ASSERT_TRUE(wait_until(
        [&] { return !latest.operation_pending && latest.exported_path == diagnostic; }));
    std::ifstream zip{diagnostic, std::ios::binary};
    const std::vector<unsigned char> signature{std::istreambuf_iterator<char>{zip},
                                               std::istreambuf_iterator<char>{}};
    ASSERT_EQ(signature.size(), 4U);
    EXPECT_EQ(signature[0], 0x50U);
    EXPECT_EQ(signature[1], 0x4bU);
    zip.close();

    const auto csv = base / "alarms.csv";
    ASSERT_TRUE(client.export_alarm_csv(csv));
    ASSERT_TRUE(
        wait_until([&] { return !latest.operation_pending && latest.exported_path == csv; }));
    std::ifstream csv_input{csv, std::ios::binary};
    const std::string csv_text{std::istreambuf_iterator<char>{csv_input},
                               std::istreambuf_iterator<char>{}};
    EXPECT_NE(csv_text.find("CAMERA_OFFLINE"), std::string::npos);
    EXPECT_NE(csv_text.find("相机离线"), std::string::npos);
    csv_input.close();

    client.stop();
    auto disconnected_query = client.query_logs({});
    ASSERT_FALSE(disconnected_query);
    EXPECT_EQ(disconnected_query.error().business_code, "IPC_NOT_CONNECTED");
    auto stale_export = client.export_alarm_csv(base / "stale.csv");
    ASSERT_FALSE(stale_export);
    EXPECT_EQ(stale_export.error().business_code, "IPC_NOT_CONNECTED");
    stop_server(server);
    std::error_code cleanup_error;
    std::filesystem::remove_all(base, cleanup_error);
    EXPECT_FALSE(cleanup_error);
}

TEST(StorageClient, SynchronizesCompleteConfigurationAndPreservesRestartReadback)
{
    const std::string name = state_name();
    auto handler = std::make_shared<StorageClientHandler>();
    paperbreak::ipc::IpcServer server(handler, std::make_unique<StateAuthorizer>(),
                                      server_options(name));
    ASSERT_TRUE(server.start());

    paperbreak::console::StorageClientSnapshot latest;
    paperbreak::console::StorageClient client([&](const auto& snapshot) { latest = snapshot; },
                                              client_options(name));
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(wait_until([&] { return !latest.stale; }));
    EXPECT_EQ(latest.stored_config_revision, 4U);
    EXPECT_EQ(latest.configuration.event_root, "数据/事件 文件");
    EXPECT_EQ(latest.configuration.maximum_cache_storage_gib, 1000U);
    EXPECT_EQ(latest.configuration, latest.effective_configuration);

    auto changed = latest.configuration;
    changed.cache_root = "高速 缓存/NVMe";
    changed.rolling_cache_enabled = true;
    changed.maximum_cache_storage_gib = 800U;
    changed.rolling_cache_write_limit_mibps = 500U;
    changed.rolling_cache_io_timeout_ms = 20000U;
    changed.warning_free_space_gib = 220U;
    changed.critical_free_space_gib = 120U;
    changed.stop_free_space_gib = 30U;
    changed.maximum_event_storage_gib = 900U;
    ASSERT_TRUE(client.update_configuration(changed));
    auto busy = client.update_configuration(changed);
    ASSERT_FALSE(busy);
    EXPECT_EQ(busy.error().business_code, "IPC_BUSY");
    ASSERT_TRUE(wait_until([&] { return !latest.operation_pending; }));
    {
        std::scoped_lock lock{handler->mutex};
        ASSERT_TRUE(handler->last_update_payload.is_object());
        EXPECT_EQ(handler->last_update_payload["expectedConfigRevision"], 4U);
        const auto& storage = handler->last_update_payload["storage"];
        EXPECT_EQ(storage.size(), 10U);
        EXPECT_EQ(storage["cacheRoot"], "高速 缓存/NVMe");
        EXPECT_TRUE(storage["rollingCacheEnabled"].get<bool>());
        EXPECT_EQ(storage["rollingCacheWriteLimitMiBps"], 500U);
        EXPECT_EQ(storage["maximumEventStorageGiB"], 900U);
    }
    EXPECT_EQ(latest.stored_config_revision, 5U);
    EXPECT_EQ(latest.effective_config_revision, 4U);
    EXPECT_EQ(latest.configuration.cache_root, "高速 缓存/NVMe");
    EXPECT_EQ(latest.effective_configuration.cache_root, "data/cache");
    EXPECT_EQ(latest.pending_restart_paths.size(), 2U);

    client.stop();
    EXPECT_TRUE(latest.stale);
    auto disconnected = client.update_configuration(changed);
    ASSERT_FALSE(disconnected);
    EXPECT_EQ(disconnected.error().business_code, "IPC_NOT_CONNECTED");
    stop_server(server);
}

TEST(UplinkClient, SynchronizesCompleteConfigurationAndPreservesRestartReadback)
{
    const std::string name = state_name();
    auto handler = std::make_shared<UplinkClientHandler>();
    paperbreak::ipc::IpcServer server(handler, std::make_unique<StateAuthorizer>(),
                                      server_options(name));
    ASSERT_TRUE(server.start());

    paperbreak::console::UplinkClientSnapshot latest;
    paperbreak::console::UplinkClient client([&](const auto& snapshot) { latest = snapshot; },
                                             client_options(name));
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(wait_until([&] { return !latest.stale; }));
    EXPECT_EQ(latest.stored_config_revision, 4U);
    EXPECT_EQ(latest.configuration.server_url, "http://127.0.0.1:18080");
    EXPECT_EQ(latest.configuration.chunk_bytes, 1048576U);
    EXPECT_EQ(latest.configuration, latest.effective_configuration);

    auto changed = latest.configuration;
    changed.enabled = true;
    changed.server_url = "http://192.0.2.30:18080";
    changed.heartbeat_seconds = 9U;
    changed.chunk_bytes = 524288U;
    changed.io_timeout_ms = 15000U;
    changed.upload_limit_mibps = 50U;
    ASSERT_TRUE(client.update_configuration(changed));
    auto busy = client.update_configuration(changed);
    ASSERT_FALSE(busy);
    EXPECT_EQ(busy.error().business_code, "IPC_BUSY");
    ASSERT_TRUE(wait_until([&] { return !latest.operation_pending; }));
    {
        std::scoped_lock lock{handler->mutex};
        ASSERT_TRUE(handler->last_update_payload.is_object());
        EXPECT_EQ(handler->last_update_payload["expectedConfigRevision"], 4U);
        const auto& uplink = handler->last_update_payload["uplink"];
        EXPECT_EQ(uplink.size(), 8U);
        EXPECT_TRUE(uplink["enabled"].get<bool>());
        EXPECT_EQ(uplink["serverUrl"], "http://192.0.2.30:18080");
        EXPECT_EQ(uplink["chunkBytes"], 524288U);
        EXPECT_EQ(uplink["uploadLimitMiBps"], 50U);
    }
    EXPECT_EQ(latest.stored_config_revision, 5U);
    EXPECT_EQ(latest.effective_config_revision, 4U);
    EXPECT_TRUE(latest.configuration.enabled);
    EXPECT_FALSE(latest.effective_configuration.enabled);
    EXPECT_EQ(latest.pending_restart_paths, std::vector<std::string>({"/uplink/transport"}));

    client.stop();
    EXPECT_TRUE(latest.stale);
    auto disconnected = client.update_configuration(changed);
    ASSERT_FALSE(disconnected);
    EXPECT_EQ(disconnected.error().business_code, "IPC_NOT_CONNECTED");
    stop_server(server);
}

TEST(AlgorithmClient, SynchronizesConfigurationMetricsAndIsolatedTestResult)
{
    const std::string name = state_name();
    auto handler = std::make_shared<AlgorithmClientHandler>();
    paperbreak::ipc::IpcServer server(handler, std::make_unique<StateAuthorizer>(),
                                      server_options(name));
    ASSERT_TRUE(server.start());

    paperbreak::console::AlgorithmClientSnapshot latest;
    paperbreak::console::AlgorithmClient client([&](const auto& snapshot) { latest = snapshot; },
                                                client_options(name));
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(wait_until([&] { return !latest.stale; }));
    EXPECT_EQ(latest.camera_id, "CAM01");
    EXPECT_EQ(latest.stored_config_revision, 9U);
    EXPECT_EQ(latest.configuration.type, "classical-vision");
    EXPECT_EQ(latest.runtime.plugin_id, "classical-vision");
    EXPECT_EQ(latest.runtime.state, "active");
    EXPECT_TRUE(latest.runtime.prototype_only);
    EXPECT_TRUE(latest.runtime.has_current_frame);
    EXPECT_EQ(latest.runtime.metrics.queue_capacity, 8U);
    EXPECT_EQ(latest.runtime.metrics.maximum_processing_time_us, 180);
    EXPECT_EQ(latest.runtime.metrics.consecutive_backlog_events, 3U);
    EXPECT_TRUE(latest.runtime.metrics.backlog_active);
    EXPECT_EQ(latest.runtime.metrics.consecutive_bad_backlog_windows, 2U);
    EXPECT_EQ(latest.runtime.metrics.average_queue_wait_time_us, 800);
    EXPECT_EQ(latest.runtime.metrics.maximum_end_to_end_time_us, 15000);
    EXPECT_DOUBLE_EQ(latest.runtime.metrics.input_fps, 60.0);
    EXPECT_DOUBLE_EQ(latest.runtime.metrics.processed_fps, 59.0);
    EXPECT_DOUBLE_EQ(latest.runtime.metrics.skipped_ratio, 0.025);
    EXPECT_EQ(latest.runtime.metrics.result_queue_rejected, 4U);

    auto changed = latest.configuration;
    changed.enabled = false;
    changed.candidate_threshold = 0.25;
    changed.confirmation_threshold = 0.75;
    changed.consecutive_frames = 7U;
    changed.cooldown_ms = 500U;
    changed.model_reference = "models/prototype.bin";
    changed.model_version = "prototype-2";
    changed.device = "directml";
    changed.debug_overlay = false;
    ASSERT_TRUE(client.update_configuration(changed));
    ASSERT_TRUE(wait_until([&] {
        std::scoped_lock lock{handler->mutex};
        return !handler->last_update_payload.empty() && !latest.operation_pending;
    }));
    {
        std::scoped_lock lock{handler->mutex};
        const auto payload = nlohmann::json::parse(handler->last_update_payload);
        EXPECT_EQ(payload["cameraId"], "CAM01");
        EXPECT_EQ(payload["expectedConfigRevision"], 9U);
        EXPECT_FALSE(payload["algorithm"]["enabled"].get<bool>());
        EXPECT_EQ(payload["algorithm"]["candidateThreshold"], 0.25);
        EXPECT_EQ(payload["algorithm"]["consecutiveFrames"], 7U);
        EXPECT_EQ(payload["algorithm"]["modelReference"], "models/prototype.bin");
        EXPECT_EQ(payload["algorithm"]["device"], "directml");
    }

    ASSERT_TRUE(client.test_current_frame());
    ASSERT_TRUE(wait_until([&] { return latest.test_result.has_value(); }));
    EXPECT_TRUE(latest.test_result->isolated);
    EXPECT_FALSE(latest.test_result->candidate_created);
    EXPECT_TRUE(latest.test_result->triggered);
    EXPECT_EQ(latest.test_result->candidate_type, "paper-missing");
    EXPECT_EQ(latest.test_result->sequence_number, 41U);
    EXPECT_EQ(latest.test_result->preview_source_width, 4U);
    EXPECT_EQ(latest.test_result->preview_jpeg.size(), 4U);
    ASSERT_EQ(latest.test_result->debug_metrics.size(), 1U);
    EXPECT_EQ(latest.test_result->debug_metrics.front().name, "paperRatio");

    ASSERT_TRUE(client.select_camera("CAM02"));
    ASSERT_TRUE(wait_until([&] { return !latest.stale && latest.runtime.camera_id == "CAM02"; }));
    EXPECT_EQ(latest.runtime.metrics.consecutive_backlog_events, 3U);
    EXPECT_EQ(latest.runtime.metrics.result_queue_rejected, 4U);
    auto invalid = client.select_camera("CAM99");
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().business_code, "IPC_REQUEST_INVALID");

    client.stop();
    EXPECT_TRUE(latest.stale);
    auto disconnected = client.test_current_frame();
    ASSERT_FALSE(disconnected);
    EXPECT_EQ(disconnected.error().business_code, "IPC_NOT_CONNECTED");
    stop_server(server);
}

TEST(EventClient, QueriesDetailsReviewsAndExportsVerifiedArchive)
{
    const std::string name = state_name();
    auto handler = std::make_shared<EventClientHandler>();
    paperbreak::ipc::IpcServer server(handler, std::make_unique<StateAuthorizer>(),
                                      server_options(name));
    ASSERT_TRUE(server.start());

    paperbreak::console::EventClientSnapshot latest;
    paperbreak::console::EventClient client([&](const auto& snapshot) { latest = snapshot; },
                                            client_options(name));
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(wait_until([&] { return !latest.configuration_stale && !latest.events_stale; }));
    EXPECT_EQ(latest.stored_config_revision, 4U);
    EXPECT_FALSE(latest.upload_runtime_available);
    ASSERT_EQ(latest.events.size(), 1U);
    EXPECT_EQ(latest.events.front().event_id, "event-1");
    EXPECT_EQ(latest.events.front().decision_state, "Candidate");
    EXPECT_EQ(latest.events.front().persistence_state, "Committed");
    EXPECT_EQ(latest.events.front().review_state, "Unreviewed");
    EXPECT_EQ(latest.events.front().trigger_count, 2U);
    EXPECT_EQ(latest.summary.candidate_decisions, 2U);
    EXPECT_EQ(latest.summary.committed, 1U);

    paperbreak::console::EventConfigurationValue changed_configuration;
    changed_configuration.pre_event_seconds = 600U;
    changed_configuration.post_event_seconds = 0U;
    changed_configuration.max_event_seconds = 600U;
    changed_configuration.merge_gap_seconds = 600U;
    changed_configuration.key_frame_count = 32U;
    changed_configuration.upload_policy = "never";
    ASSERT_TRUE(client.update_configuration(changed_configuration));
    ASSERT_TRUE(wait_until([&] {
        std::scoped_lock lock{handler->mutex};
        return !handler->last_update_payload.empty() && !latest.operation_pending;
    }));
    {
        std::scoped_lock lock{handler->mutex};
        const auto payload = nlohmann::json::parse(handler->last_update_payload);
        EXPECT_EQ(payload["expectedConfigRevision"], 4U);
        EXPECT_EQ(payload["event"]["preEventSeconds"], 600U);
        EXPECT_EQ(payload["event"]["mergeGapSeconds"], 600U);
        EXPECT_EQ(payload["event"]["keyFrameCount"], 32U);
        EXPECT_EQ(payload["event"]["uploadPolicy"], "never");
    }

    handler->reject_config_update.store(true);
    ASSERT_TRUE(client.update_configuration(changed_configuration));
    ASSERT_TRUE(wait_until(
        [&] { return !latest.operation_pending && latest.configuration_error.has_value(); }));
    EXPECT_EQ(latest.configuration_error->business_code, "SYS_CONFIG_INVALID");
    EXPECT_EQ(latest.configuration_error->message, "事件配置校验失败");
    handler->reject_config_update.store(false);

    ASSERT_TRUE(client.query({.start_time_utc_ms = 1000,
                              .end_time_utc_ms = 2000,
                              .decision_state = std::string{"Candidate"},
                              .through_now = false,
                              .camera_id = std::string{"CAM01"},
                              .offset = 0U,
                              .limit = 50U}));
    ASSERT_TRUE(wait_until([&] {
        std::scoped_lock lock{handler->mutex};
        const auto payload = nlohmann::json::parse(handler->last_list_payload);
        return payload.value("startTimeUtcMs", 0) == 1000 &&
               payload.value("endTimeUtcMs", 0) == 2000 &&
               payload.value("decisionState", "") == "Candidate" &&
               payload.value("cameraId", "") == "CAM01";
    }));

    ASSERT_TRUE(client.query({.start_time_utc_ms = 2500,
                              .end_time_utc_ms = 3000,
                              .decision_state = std::string{"Candidate"},
                              .through_now = true,
                              .offset = 0U,
                              .limit = 50U}));
    ASSERT_TRUE(wait_until([&] {
        std::scoped_lock lock{handler->mutex};
        const auto payload = nlohmann::json::parse(handler->last_list_payload);
        return payload.value("startTimeUtcMs", 0) == 2500 && !payload.contains("endTimeUtcMs");
    }));

    ASSERT_TRUE(client.get("event-1"));
    ASSERT_TRUE(wait_until(
        [&] { return latest.detail.has_value() && !latest.detail->manifest_json.empty(); }));
    EXPECT_TRUE(latest.detail->key_frames_traceable);
    EXPECT_EQ(latest.detail->raw_frame_count, 2U);
    EXPECT_NE(latest.detail->manifest_json.find("event-1"), std::string::npos);

    ASSERT_TRUE(client.review("event-1", 1U, true));
    auto busy = client.manual_trigger("CAM01");
    ASSERT_FALSE(busy);
    EXPECT_EQ(busy.error().business_code, "IPC_BUSY");
    ASSERT_TRUE(wait_until([&] { return !latest.operation_pending; }));

    ASSERT_TRUE(client.retry_upload("event-1"));
    ASSERT_TRUE(wait_until([&] {
        std::scoped_lock lock{handler->mutex};
        return !latest.operation_pending && !handler->last_retry_payload.empty();
    }));
    {
        std::scoped_lock lock{handler->mutex};
        EXPECT_EQ(nlohmann::json::parse(handler->last_retry_payload)["eventId"], "event-1");
    }

    static std::atomic_uint64_t file_sequence{};
    const auto base =
        std::filesystem::temp_directory_path() /
        std::filesystem::path{L"paperbreak-event-中文-" + std::to_wstring(++file_sequence)};
    std::filesystem::create_directories(base);
    const auto archive = base / L"事件导出.zip";
    ASSERT_TRUE(client.export_event("event-1", archive));
    ASSERT_TRUE(
        wait_until([&] { return !latest.operation_pending && latest.exported_path == archive; }));
    std::ifstream zip{archive, std::ios::binary};
    const std::vector<unsigned char> signature{std::istreambuf_iterator<char>{zip},
                                               std::istreambuf_iterator<char>{}};
    ASSERT_EQ(signature.size(), 4U);
    EXPECT_EQ(signature[0], 0x50U);
    EXPECT_EQ(signature[1], 0x4bU);
    zip.close();

    client.stop();
    auto disconnected_update = client.update_configuration(changed_configuration);
    ASSERT_FALSE(disconnected_update);
    EXPECT_EQ(disconnected_update.error().business_code, "IPC_NOT_CONNECTED");
    auto disconnected = client.get("event-1");
    ASSERT_FALSE(disconnected);
    EXPECT_EQ(disconnected.error().business_code, "IPC_NOT_CONNECTED");
    stop_server(server);
    std::error_code cleanup_error;
    std::filesystem::remove_all(base, cleanup_error);
    EXPECT_FALSE(cleanup_error);
}

TEST(ConsoleNavigationModel, DefinesStableUniquePageOrder)
{
    const auto pages = paperbreak::console::console_pages();
    ASSERT_EQ(pages.size(), 12U);
    EXPECT_EQ(paperbreak::console::default_console_page_index(), 0U);
    EXPECT_EQ(pages.front().id, paperbreak::console::ConsolePageId::overview);
    EXPECT_EQ(pages.front().title, "总览");
    EXPECT_EQ(pages.back().id, paperbreak::console::ConsolePageId::maintenance);

    std::set<std::string_view> keys;
    for (const auto& page : pages)
    {
        EXPECT_TRUE(keys.insert(page.key).second);
        ASSERT_TRUE(paperbreak::console::console_page_index(page.id).has_value());
        EXPECT_EQ(pages[paperbreak::console::console_page_index(page.id).value()].key, page.key);
    }
}

TEST(ConsoleTrayStatusModel, MapsConnectionServiceAndAlarmPriority)
{
    using paperbreak::console::TrayStatusColor;
    paperbreak::console::ClientStateSnapshot snapshot;
    snapshot.connection.state = paperbreak::ipc::ClientConnectionState::retry_wait;
    snapshot.service_status = paperbreak::console::ServiceStatusSummary{.service_state = "running"};
    snapshot.service_status_stale = false;
    snapshot.alarms = paperbreak::console::AlarmOverviewSummary{};
    snapshot.alarms_stale = false;
    EXPECT_EQ(paperbreak::console::tray_status(snapshot).color, TrayStatusColor::gray);

    snapshot.connection.state = paperbreak::ipc::ClientConnectionState::connected;
    snapshot.service_status_stale = true;
    EXPECT_EQ(paperbreak::console::tray_status(snapshot).color, TrayStatusColor::gray);

    snapshot.service_status_stale = false;
    snapshot.service_status->service_state = "starting";
    EXPECT_EQ(paperbreak::console::tray_status(snapshot).color, TrayStatusColor::red);

    snapshot.service_status->service_state = "running";
    snapshot.alarms_stale = true;
    EXPECT_EQ(paperbreak::console::tray_status(snapshot).color, TrayStatusColor::gray);

    snapshot.alarms_stale = false;
    snapshot.alarms->highest_severity = "Info";
    EXPECT_EQ(paperbreak::console::tray_status(snapshot).color, TrayStatusColor::green);
    snapshot.alarms->highest_severity = "Warning";
    EXPECT_EQ(paperbreak::console::tray_status(snapshot).color, TrayStatusColor::yellow);
    snapshot.alarms->highest_severity = "Error";
    EXPECT_EQ(paperbreak::console::tray_status(snapshot).color, TrayStatusColor::red);
    snapshot.alarms->highest_severity = "Critical";
    EXPECT_EQ(paperbreak::console::tray_status(snapshot).color, TrayStatusColor::red);
}

TEST(PreviewClient, PausesWithoutStartingAnyServiceControlOperation)
{
    paperbreak::console::PreviewSnapshot latest;
    paperbreak::console::PreviewClient client(
        [&](const paperbreak::console::PreviewSnapshot& snapshot) { latest = snapshot; });

    client.set_camera_ids({"CAM01", "CAM02", "CAM03", "CAM04"});
    client.set_paused(true);
    EXPECT_TRUE(latest.paused);
    EXPECT_FALSE(latest.subscribed);
    EXPECT_EQ(latest.accepted_frames, 0U);

    client.set_camera_ids({"CAM01", "CAM01"});
    ASSERT_TRUE(latest.last_error.has_value());
    EXPECT_EQ(latest.last_error->business_code, "IPC_PROTOCOL_ERROR");
    EXPECT_TRUE(latest.paused);

    client.set_paused(false);
    EXPECT_FALSE(latest.paused);
    EXPECT_FALSE(latest.subscribed);
    client.stop();
}

TEST(PreviewClient, UsesConfiguredCameraSlotsInsteadOfAlwaysRequestingFourCameras)
{
    paperbreak::console::PreviewClient client;

    client.set_camera_ids({"CAM01"});

    ASSERT_EQ(client.camera_ids().size(), 1U);
    EXPECT_EQ(client.camera_ids().front(), "CAM01");
    EXPECT_FALSE(client.snapshot().last_error.has_value());
}

TEST(PreviewClient, ValidatesAndResubscribesWithSelectedPreviewFps)
{
    const auto name = state_name();
    auto handler = std::make_shared<PreviewSubscriptionHandler>();
    paperbreak::ipc::IpcServer server{handler, std::make_unique<StateAuthorizer>(),
                                      server_options(name)};
    ASSERT_TRUE(server.start());
    paperbreak::console::PreviewClient client{{}, client_options(name)};
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(wait_until([&] { return handler->first_request_entered(); }));
    handler->release_first_request();
    ASSERT_TRUE(wait_until([&] { return client.snapshot().subscribed; }));
    EXPECT_TRUE(handler->received_fps(2.0));

    client.set_target_fps(30.0);
    ASSERT_TRUE(
        wait_until([&] { return handler->received_fps(30.0) && client.snapshot().subscribed; }));
    EXPECT_EQ(client.snapshot().target_fps, 30.0);

    client.set_target_fps(7.0);
    ASSERT_TRUE(client.snapshot().last_error.has_value());
    EXPECT_EQ(client.snapshot().target_fps, 30.0);
    client.stop();
    server.request_stop();
    ASSERT_TRUE(server.join(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
}

TEST(PreviewClient, ReplacesAnInFlightDefaultSubscriptionWithConfiguredCameraSlots)
{
    const auto name = state_name();
    auto handler = std::make_shared<PreviewSubscriptionHandler>();
    paperbreak::ipc::IpcServer server{handler, std::make_unique<StateAuthorizer>(),
                                      server_options(name)};
    ASSERT_TRUE(server.start());
    paperbreak::console::PreviewClient client{{}, client_options(name)};
    ASSERT_TRUE(client.start());
    ASSERT_TRUE(wait_until([&] { return handler->first_request_entered(); }));

    client.set_camera_ids({"CAM01"});
    handler->release_first_request();

    ASSERT_TRUE(wait_until(
        [&] { return handler->received_single_camera() && client.snapshot().subscribed; }));
    EXPECT_EQ(client.camera_ids(), std::vector<std::string>{"CAM01"});
    client.stop();
    server.request_stop();
    ASSERT_TRUE(server.join(std::chrono::steady_clock::now() + std::chrono::seconds{2}));
}
