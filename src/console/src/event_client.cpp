#include "paperbreak/console/event_client.hpp"

#include <nlohmann/json.hpp>

#include <QCoreApplication>
#include <QFile>
#include <QMetaObject>
#include <QSaveFile>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>
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
            client_error("IPC_PROTOCOL_ERROR", "后台响应不是有效对象", std::string{operation}));
    return Result<Json>::success(std::move(payload));
}

EventConfigurationValue event_configuration(const Json& value)
{
    return {.pre_event_seconds = value.value("preEventSeconds", 10U),
            .post_event_seconds = value.value("postEventSeconds", 10U),
            .max_event_seconds = value.value("maxEventSeconds", 60U),
            .merge_gap_seconds = value.value("mergeGapSeconds", 3U),
            .key_frame_count = value.value("keyFrameCount", 7U),
            .save_raw = value.value("saveRaw", true),
            .generate_preview_video = value.value("generatePreviewVideo", false),
            .upload_policy = value.value("uploadPolicy", std::string{"confirmed"}),
            .retention_days = value.value("retentionDays", 30U)};
}

Json event_configuration_json(const EventConfigurationValue& value)
{
    return {{"preEventSeconds", value.pre_event_seconds},
            {"postEventSeconds", value.post_event_seconds},
            {"maxEventSeconds", value.max_event_seconds},
            {"mergeGapSeconds", value.merge_gap_seconds},
            {"keyFrameCount", value.key_frame_count},
            {"saveRaw", value.save_raw},
            {"generatePreviewVideo", value.generate_preview_video},
            {"uploadPolicy", value.upload_policy},
            {"retentionDays", value.retention_days}};
}

EventListItem event_item(const Json& value)
{
    return {.event_id = value.value("eventId", std::string{}),
            .event_state = value.value("eventState", std::string{}),
            .review_revision = value.value("reviewRevision", std::uint64_t{}),
            .candidate_time_utc_ms = value.value("candidateTimeUtcMs", std::int64_t{}),
            .trigger_camera_id = value.value("triggerCameraId", std::string{}),
            .confidence = value.value("confidence", 0.0),
            .upload_state = value.value("uploadState", std::string{}),
            .storage_state = value.value("storageState", std::string{}),
            .thumbnail_available = value.value("thumbnailAvailable", false)};
}

std::filesystem::path path_from_utf8(const std::string& value)
{
    std::u8string converted;
    converted.reserve(value.size());
    for (const unsigned char byte : value)
        converted.push_back(static_cast<char8_t>(byte));
    return std::filesystem::path{converted};
}

} // namespace

class EventClient::FileExporter final
{
  public:
    using Completion = std::function<void(Result<std::filesystem::path>)>;

    FileExporter() : worker_([this](const std::stop_token token) { run(token); }) {}
    ~FileExporter()
    {
        stop();
    }

    Result<void> submit(std::filesystem::path source, std::filesystem::path destination,
                        Completion completion)
    {
        std::scoped_lock lock{mutex_};
        if (stopped_ || job_)
            return Result<void>::failure(
                client_error("IPC_BUSY", "已有事件导出任务正在执行", "console.event.export", true));
        job_ = Job{std::move(source), std::move(destination), std::move(completion)};
        condition_.notify_one();
        return Result<void>::success();
    }

    void stop() noexcept
    {
        {
            std::scoped_lock lock{mutex_};
            if (stopped_)
                return;
            stopped_ = true;
        }
        worker_.request_stop();
        condition_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }

  private:
    struct Job final
    {
        std::filesystem::path source;
        std::filesystem::path destination;
        Completion completion;
    };

    static QString path_text(const std::filesystem::path& path)
    {
        const auto value = path.generic_u8string();
        return QString::fromUtf8(reinterpret_cast<const char*>(value.data()),
                                 static_cast<qsizetype>(value.size()));
    }

    static Result<std::filesystem::path> write(const Job& job, const std::stop_token token)
    {
        QFile source{path_text(job.source)};
        if (!source.open(QIODevice::ReadOnly))
            return Result<std::filesystem::path>::failure(
                client_error("EVENT_EXPORT_FAILED", "无法读取服务端已校验事件暂存文件",
                             "console.event.export.source"));
        QSaveFile file{path_text(job.destination)};
        if (!file.open(QIODevice::WriteOnly))
            return Result<std::filesystem::path>::failure(client_error(
                "EVENT_EXPORT_FAILED", "无法创建事件导出文件", "console.event.export.open"));
        constexpr qint64 chunk_size = 64 * 1024;
        while (!source.atEnd())
        {
            if (token.stop_requested())
            {
                file.cancelWriting();
                return Result<std::filesystem::path>::failure(client_error(
                    "IPC_REQUEST_CANCELLED", "事件导出已取消", "console.event.export.write"));
            }
            const QByteArray chunk = source.read(chunk_size);
            if (chunk.isEmpty() && !source.atEnd())
            {
                file.cancelWriting();
                return Result<std::filesystem::path>::failure(
                    client_error("EVENT_EXPORT_FAILED", "读取事件导出暂存文件失败",
                                 "console.event.export.read"));
            }
            if (file.write(chunk) != chunk.size())
            {
                file.cancelWriting();
                return Result<std::filesystem::path>::failure(client_error(
                    "EVENT_EXPORT_FAILED", "事件导出写入失败", "console.event.export.write"));
            }
        }
        if (!file.commit())
            return Result<std::filesystem::path>::failure(client_error(
                "EVENT_EXPORT_FAILED", "事件导出原子提交失败", "console.event.export.commit"));
        return Result<std::filesystem::path>::success(job.destination);
    }

    void run(const std::stop_token token)
    {
        while (!token.stop_requested())
        {
            std::optional<Job> job;
            {
                std::unique_lock lock{mutex_};
                condition_.wait(lock, token, [this] { return stopped_ || job_.has_value(); });
                if (stopped_ || token.stop_requested())
                    return;
                job = std::move(job_);
                job_.reset();
            }
            auto result = write(*job, token);
            auto completion = std::move(job->completion);
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [completion = std::move(completion), result = std::move(result)]() mutable {
                    completion(std::move(result));
                },
                Qt::QueuedConnection);
        }
    }

    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::optional<Job> job_;
    bool stopped_{};
    std::jthread worker_;
};

EventClient::EventClient(EventClientObserver observer, ipc::IpcClientOptions options)
    : observer_(std::move(observer)), exporter_(std::make_unique<FileExporter>()),
      alive_(std::make_shared<std::atomic_bool>(true))
{
    client_ = std::make_unique<ipc::IpcClient>(
        ipc::IpcClientCallbacks{
            .connection_changed = [this](const auto& value) { connection_changed(value); },
            .push_received =
                [this](const auto, const auto& push) {
                    if (push.event_name == "event.committed")
                        refresh();
                }},
        std::move(options));
}

EventClient::~EventClient()
{
    alive_->store(false);
    stop();
}

Result<void> EventClient::start()
{
    return client_->start();
}

void EventClient::stop() noexcept
{
    if (client_)
        client_->stop();
    config_request_.reset();
    list_request_.reset();
    detail_request_.reset();
    manifest_request_.reset();
    operation_request_.reset();
    snapshot_.configuration_stale = true;
    snapshot_.events_stale = true;
    snapshot_.operation_pending = false;
    if (exporter_)
        exporter_->stop();
    notify();
}

const EventClientSnapshot& EventClient::snapshot() const noexcept
{
    return snapshot_;
}

void EventClient::connection_changed(const ipc::ClientConnectionSnapshot& connection)
{
    snapshot_.connection = connection;
    if (connection.state == ipc::ClientConnectionState::connected)
        refresh();
    else
    {
        snapshot_.configuration_stale = true;
        snapshot_.events_stale = true;
        config_request_.reset();
        list_request_.reset();
        detail_request_.reset();
        manifest_request_.reset();
        operation_request_.reset();
        snapshot_.operation_pending = false;
    }
    notify();
}

void EventClient::refresh()
{
    if (snapshot_.connection.state != ipc::ClientConnectionState::connected)
        return;
    if (!config_request_)
    {
        auto sent =
            client_->send_request("event.getConfig", "{}", {}, [this](auto handle, auto result) {
                config_completed(handle, std::move(result));
            });
        if (sent)
            config_request_ = sent.value();
        else
            snapshot_.error = sent.error();
    }
    static_cast<void>(query(snapshot_.filter));
    notify();
}

Result<void> EventClient::query(EventListFilter filter)
{
    if (list_request_)
        return Result<void>::failure(
            client_error("IPC_BUSY", "事件查询正在执行", "console.event.list", true));
    Json payload{{"offset", filter.offset}, {"limit", filter.limit}};
    if (filter.start_time_utc_ms)
        payload["startTimeUtcMs"] = *filter.start_time_utc_ms;
    if (filter.end_time_utc_ms)
        payload["endTimeUtcMs"] = *filter.end_time_utc_ms;
    if (filter.event_state)
        payload["eventState"] = *filter.event_state;
    if (filter.camera_id)
        payload["cameraId"] = *filter.camera_id;
    auto sent =
        client_->send_request("event.list", payload.dump(), {}, [this](auto handle, auto result) {
            list_completed(handle, std::move(result));
        });
    if (!sent)
        return Result<void>::failure(sent.error());
    snapshot_.filter = std::move(filter);
    list_request_ = sent.value();
    return Result<void>::success();
}

Result<void> EventClient::get(std::string event_id)
{
    if (detail_request_ || manifest_request_)
        return Result<void>::failure(
            client_error("IPC_BUSY", "事件详情正在加载", "console.event.get", true));
    auto sent = client_->send_request(
        "event.get", Json{{"eventId", std::move(event_id)}}.dump(), {},
        [this](auto handle, auto result) { detail_completed(handle, std::move(result)); });
    if (!sent)
        return Result<void>::failure(sent.error());
    detail_request_ = sent.value();
    return Result<void>::success();
}

Result<void> EventClient::update_configuration(EventConfigurationValue value)
{
    if (snapshot_.connection.state != ipc::ClientConnectionState::connected)
        return Result<void>::failure(client_error("IPC_NOT_CONNECTED", "后台服务尚未连接",
                                                  "console.event.updateConfig", true));
    if (snapshot_.configuration_stale)
        return Result<void>::failure(client_error("EVENT_CONFIG_STALE",
                                                  "事件配置尚未同步，不能保存",
                                                  "console.event.updateConfig", true));
    snapshot_.configuration_error.reset();
    return send_operation("event.updateConfig",
                          Json{{"expectedConfigRevision", snapshot_.stored_config_revision},
                               {"event", event_configuration_json(value)}}
                              .dump());
}

Result<void> EventClient::manual_trigger(std::string camera_id)
{
    return send_operation("event.manualTrigger", Json{{"cameraId", std::move(camera_id)}}.dump());
}

Result<void> EventClient::review(std::string event_id, const std::uint64_t expected_revision,
                                 const bool confirmed)
{
    return send_operation(
        confirmed ? "event.confirm" : "event.reject",
        Json{{"eventId", std::move(event_id)}, {"expectedReviewRevision", expected_revision}}
            .dump());
}

Result<void> EventClient::export_event(std::string event_id, std::filesystem::path destination)
{
    return send_operation("event.export", Json{{"eventId", std::move(event_id)}}.dump(),
                          std::move(destination));
}

Result<void> EventClient::send_operation(std::string command, std::string payload_json,
                                         std::filesystem::path destination)
{
    if (operation_request_)
        return Result<void>::failure(
            client_error("IPC_BUSY", "已有事件操作正在执行", "console.event.operation", true));
    snapshot_.operation = command;
    snapshot_.operation_pending = true;
    snapshot_.error.reset();
    const auto timeout =
        command == "event.export" ? std::chrono::minutes{30} : std::chrono::milliseconds::zero();
    auto sent = client_->send_request(
        std::move(command), std::move(payload_json), {},
        [this, destination = std::move(destination)](auto handle, auto result) mutable {
            operation_completed(handle, std::move(result), std::move(destination));
        },
        timeout);
    if (!sent)
    {
        snapshot_.operation_pending = false;
        return Result<void>::failure(sent.error());
    }
    operation_request_ = sent.value();
    notify();
    return Result<void>::success();
}

void EventClient::config_completed(ipc::ClientRequestHandle handle,
                                   Result<ipc::ResponseMessage> result)
{
    if (!config_request_ || *config_request_ != handle)
        return;
    config_request_.reset();
    auto payload = response_payload(result, "console.event.getConfig");
    if (!payload)
        snapshot_.configuration_error = payload.error();
    else
    {
        snapshot_.configuration = event_configuration(payload.value().at("event"));
        snapshot_.stored_config_revision = payload.value().value("storedConfigRevision", 0U);
        snapshot_.preview_video_generation_available =
            payload.value().value("previewVideoGenerationAvailable", false);
        snapshot_.upload_runtime_available = payload.value().value("uploadRuntimeAvailable", false);
        snapshot_.configuration_stale = false;
        snapshot_.configuration_error.reset();
    }
    notify();
}

void EventClient::list_completed(ipc::ClientRequestHandle handle,
                                 Result<ipc::ResponseMessage> result)
{
    if (!list_request_ || *list_request_ != handle)
        return;
    list_request_.reset();
    auto payload = response_payload(result, "console.event.list");
    if (!payload)
        snapshot_.error = payload.error();
    else
    {
        snapshot_.events.clear();
        for (const auto& value : payload.value().at("events"))
            snapshot_.events.push_back(event_item(value));
        snapshot_.total = payload.value().value("total", 0U);
        snapshot_.events_stale = false;
    }
    notify();
}

void EventClient::detail_completed(ipc::ClientRequestHandle handle,
                                   Result<ipc::ResponseMessage> result)
{
    if (!detail_request_ || *detail_request_ != handle)
        return;
    detail_request_.reset();
    auto payload = response_payload(result, "console.event.get");
    if (!payload)
        snapshot_.error = payload.error();
    else
    {
        snapshot_.detail =
            EventDetail{.event = event_item(payload.value().at("event")),
                        .committed_directory = path_from_utf8(
                            payload.value().value("committedDirectory", std::string{})),
                        .manifest_json = {},
                        .thumbnail_jpeg = result.value().binary,
                        .raw_frame_count = payload.value().value("rawFrameCount", 0U),
                        .key_frame_count = payload.value().value("keyFrameCount", 0U),
                        .observed_sequence_gaps = payload.value().value("observedSequenceGaps", 0U),
                        .key_frames_traceable = payload.value().value("keyFramesTraceable", false)};
        auto sent = client_->send_request(
            "event.getManifest", Json{{"eventId", snapshot_.detail->event.event_id}}.dump(), {},
            [this](auto manifest_handle, auto manifest_result) {
                manifest_completed(manifest_handle, std::move(manifest_result));
            });
        if (!sent)
            snapshot_.error = sent.error();
        else
        {
            manifest_request_ = sent.value();
            return;
        }
    }
    notify();
}

void EventClient::manifest_completed(ipc::ClientRequestHandle handle,
                                     Result<ipc::ResponseMessage> result)
{
    if (!manifest_request_ || *manifest_request_ != handle)
        return;
    manifest_request_.reset();
    auto payload = response_payload(result, "console.event.getManifest");
    if (!payload)
        snapshot_.error = payload.error();
    else if (!snapshot_.detail || !payload.value().value("verified", false) ||
             payload.value().value("eventId", std::string{}) != snapshot_.detail->event.event_id ||
             payload.value().value("size", std::size_t{}) != result.value().binary.size())
        snapshot_.error = client_error("IPC_PROTOCOL_ERROR", "事件 manifest 响应不一致",
                                       "console.event.getManifest");
    else
    {
        std::string manifest;
        manifest.reserve(result.value().binary.size());
        for (const auto byte : result.value().binary)
            manifest.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
        const auto parsed = Json::parse(manifest, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object())
            snapshot_.error = client_error("IPC_PROTOCOL_ERROR", "事件 manifest 不是有效 JSON",
                                           "console.event.getManifest");
        else
            snapshot_.detail->manifest_json = parsed.dump(2);
    }
    notify();
}

void EventClient::operation_completed(ipc::ClientRequestHandle handle,
                                      Result<ipc::ResponseMessage> result,
                                      std::filesystem::path destination)
{
    if (!operation_request_ || *operation_request_ != handle)
        return;
    operation_request_.reset();
    auto payload = response_payload(result, "console.event.operation");
    if (!payload)
    {
        snapshot_.operation_pending = false;
        snapshot_.error = payload.error();
        if (snapshot_.operation == "event.updateConfig")
            snapshot_.configuration_error = payload.error();
        notify();
        return;
    }
    if (!destination.empty())
    {
        const auto source_text = payload.value().value("exportSourcePath", std::string{});
        if (source_text.empty())
        {
            snapshot_.operation_pending = false;
            snapshot_.error = client_error("IPC_PROTOCOL_ERROR", "事件导出响应缺少暂存路径",
                                           "console.event.export.response");
            notify();
            return;
        }
        auto alive = alive_;
        auto submitted = exporter_->submit(path_from_utf8(source_text), std::move(destination),
                                           [this, alive](Result<std::filesystem::path> exported) {
                                               if (!alive->load())
                                                   return;
                                               snapshot_.operation_pending = false;
                                               if (exported)
                                                   snapshot_.exported_path =
                                                       std::move(exported).value();
                                               else
                                                   snapshot_.error = exported.error();
                                               notify();
                                           });
        if (!submitted)
        {
            snapshot_.operation_pending = false;
            snapshot_.error = submitted.error();
            notify();
        }
        return;
    }
    snapshot_.operation_pending = false;
    if (snapshot_.operation == "event.updateConfig")
        snapshot_.configuration_error.reset();
    refresh();
    notify();
}

void EventClient::notify() const noexcept
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
